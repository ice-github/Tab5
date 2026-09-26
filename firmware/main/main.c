/* Tab5 10s video player: HW JPEG + I2S audio, A/V sync on audio clock.
 *
 * Boot mode:
 *   SD missing bundle or /sdcard/video/complete absent -> TRANSFER mode:
 *     receive meta.json/frames.mjpeg/frames.idx/audio.pcm over USB-Serial-JTAG,
 *     CRC-check, write flag, reboot.
 *   else -> PLAYER mode: loop playback, frame chosen from audio sample clock.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/jpeg_decode.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_codec_dev.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "tab5player";

#define VID_DIR      "/sdcard/video"
#define FLAG_FILE    "/sdcard/video/complete"
#define FB_W         720
#define FB_H         1280
#define FPS          30
#define A_RATE       48000
#define A_CH         2
#define CRC_POLY     0xEDB88320u

static uint32_t crc32_tab[256];
static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (CRC_POLY ^ (c >> 1)) : (c >> 1);
        crc32_tab[i] = c;
    }
}
/* ---------- console line helpers (TX via VFS stdout, RX via driver) ---------- */
/* RX uses the driver API directly with explicit timeouts. Rationale: VFS
 * fread() semantics flip with driver state — without the driver it
 * returns 0 immediately, with the driver it blocks forever — so neither
 * gives the poll-with-deadline behavior this protocol needs. */
static int usj_read_exact(uint8_t *buf, size_t n, int timeout_ms)
{
    size_t got = 0;
    int64_t dl = esp_timer_get_time() + (int64_t)timeout_ms * 1000; /* us */
    while (got < n) {
        if (esp_timer_get_time() > dl)
            return -1;
        int r = usb_serial_jtag_read_bytes(buf + got, n - got,
                                           pdMS_TO_TICKS(20));
        if (r > 0)
            got += (size_t)r;
    }
    return 0;
}
static int usj_read_line(char *buf, size_t cap)
{
    size_t n = 0;
    int64_t dl = esp_timer_get_time() + (int64_t)120 * 1000 * 1000; /* 120 s */
    while (n + 1 < cap) {
        if (esp_timer_get_time() > dl)
            return -1;
        char c;
        int r = usb_serial_jtag_read_bytes((uint8_t *)&c, 1,
                                           pdMS_TO_TICKS(5));
        if (r <= 0)
            continue;
        if (c == '\n')
            break;
        if (c != '\r')
            buf[n++] = c;
    }
    buf[n] = 0;
    return (int)n;
}
static void usj_write_str(const char *s)
{
    fputs(s, stdout);
    fflush(stdout);
}

/* ---------------- TRANSFER mode ---------------- */
static const char *kFiles[] = {"meta.json", "frames.mjpeg", "frames.idx", "audio.pcm"};

static int run_transfer(void)
{
    char line[128];
    /* Bulk SD writes + polled reads can exceed the task watchdog window;
     * this phase is not real-time, so drop the watchdog here. */
    esp_task_wdt_deinit();
    ESP_LOGI(TAG, "transfer mode: waiting for host");
    /* Drain stale RX bytes left over from previous boots or old-protocol
     * senders; otherwise they corrupt the first FILE header / payload. */
    {
        int64_t dend = esp_timer_get_time() + (int64_t)300 * 1000;
        uint8_t db;
        while (esp_timer_get_time() < dend) {
            if (usb_serial_jtag_read_bytes(&db, 1, pdMS_TO_TICKS(10)) > 0)
                dend = esp_timer_get_time() + (int64_t)100 * 1000;
        }
    }
    usj_write_str("TAB5_XFER_READY\n");
    mkdir(VID_DIR, 0777);
    for (int i = 0; i < 4; i++) {
        /* skip blank lines (stale newlines, terminal noise) */
        do {
            if (usj_read_line(line, sizeof(line)) < 0)
                return -1;
        } while (line[0] == 0);
        char name[32];
        unsigned int size, want;
        if (sscanf(line, "FILE %31s %u %x", name, &size, &want) != 3)
            return -1;
        if (strcmp(name, kFiles[i])) {
            usj_write_str("ERR\n");
            return -1;
        }
        usj_write_str("GO\n");
        char path[64];
        snprintf(path, sizeof(path), VID_DIR "/%s", name);
        {
            struct stat dst;
            int ms = mkdir(VID_DIR, 0777);
            int se = stat(VID_DIR, &dst);
            ESP_LOGI(TAG, "mkdir=%d stat=%d mode=%o", ms, se,
                     se == 0 ? (unsigned)dst.st_mode : 0);
        }
        FILE *f = fopen(path, "wb");
        if (!f) {
            ESP_LOGE(TAG, "fopen %s failed errno=%d", path, errno);
            usj_write_str("ERR\n");
            return -1;
        }
        uint32_t left = size;
        static uint8_t buf[4096];
        /* Chunked transfer with per-chunk handshake: the small USB RX
         * ringbuffer can overflow during slow SD writes, so the host
         * sends NEXT, waits for SEND, then streams one chunk. */
        while (left) {
            do {
                if (usj_read_line(line, sizeof(line)) < 0) {
                    fclose(f);
                    return -1;
                }
            } while (line[0] == 0);
            if (strcmp(line, "NEXT")) {
                fclose(f);
                return -1;
            }
            size_t ch = left > sizeof(buf) ? sizeof(buf) : left;
            usj_write_str("SEND\n");
            if (usj_read_exact(buf, ch, 30000) < 0) {
                fclose(f);
                return -1;
            }
            if (fwrite(buf, 1, ch, f) != ch) {
                fclose(f);
                return -1;
            }
            left -= ch;
            usj_write_str("ACK\n");
            vTaskDelay(1);
        }
        fclose(f);
        /* verify by re-reading */
        f = fopen(path, "rb");
        uint32_t v = 0;
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
            v ^= 0xFFFFFFFFu;
            for (size_t k = 0; k < r; k++)
                v = crc32_tab[(v ^ buf[k]) & 0xFF] ^ (v >> 8);
            v ^= 0xFFFFFFFFu;
        }
        fclose(f);
        if (v != want) {
            ESP_LOGE(TAG, "%s crc mismatch got %08X want %08X", name, (unsigned)v, want);
            usj_write_str("ERR\n");
            return -1;
        }
        char ok[48];
        snprintf(ok, sizeof(ok), "OK %08X\n", (unsigned)v);
        usj_write_str(ok);
        ESP_LOGI(TAG, "%s %u bytes crc %08X", name, size, (unsigned)v);
    }
    if (usj_read_line(line, sizeof(line)) < 0 || strcmp(line, "DONE"))
        return -1;
    FILE *f = fopen(FLAG_FILE, "w");
    if (f) {
        fputs("1\n", f);
        fclose(f);
    }
    usj_write_str("COMPLETE\n");
    return 0;
}

/* ---------------- PLAYER mode ---------------- */
typedef struct {
    uint32_t off;
    uint32_t len;
} frame_ent_t;

static esp_codec_dev_handle_t spk;
static volatile uint32_t audio_samples; /* stereo frames written */
static uint8_t *audio_buf;
static size_t audio_len, audio_pos;

static void audio_task(void *arg)
{
    /* Small chunks so audio_samples (the video clock) updates every ~10ms.
     * An 8KB chunk blocks ~43ms per write and quantizes video to 23fps. */
    const size_t CHUNK = 1920; /* 480 stereo frames = 10.0ms @48kHz */
    while (1) {
        if (audio_pos >= audio_len) {
            audio_pos = 0; /* loop */
            audio_samples = 0;
        }
        size_t n = audio_len - audio_pos;
        if (n > CHUNK)
            n = CHUNK;
        esp_codec_dev_write(spk, audio_buf + audio_pos, n);
        audio_pos += n;
        audio_samples += (uint32_t)(n / (A_CH * 2));
    }
}

void app_main(void)
{
    crc_init();
    ESP_LOGI(TAG, "tab5player build %s %s", __DATE__, __TIME__);
    esp_err_t sdr = bsp_sdcard_mount();
    if (sdr != ESP_OK) {
        ESP_LOGE(TAG, "sd mount failed: %s (retrying)", esp_err_to_name(sdr));
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    ESP_ERROR_CHECK(bsp_display_brightness_init());

    struct stat st;
    bool have_bundle = (stat(FLAG_FILE, &st) == 0);
    if (!have_bundle) {
        /* Transfer mode over the console (stdin/stdout, unbuffered).
         * Boot logs may precede the READY marker; host ignores all lines
         * until it sees it.
         * Critical: disable VFS line-ending translation (it corrupts
         * binary payloads deterministically) and install the driver with
         * a large RX buffer (default path is a tiny HW FIFO that drops
         * bytes during slow SD writes). */
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stdin, NULL, _IONBF, 0);
        usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
        usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
        {
            usb_serial_jtag_driver_config_t usj_cfg = {
                .rx_buffer_size = 16384,
                .tx_buffer_size = 4096,
            };
            ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj_cfg));
            /* Route VFS stdin/stdout through the driver ringbuffer.
             * Without this, the installed driver's ISR drains the HW
             * FIFO while fread() polls the empty FIFO: reads always
             * return 0, GO is never sent, and the 120s deadline reboots. */
            usb_serial_jtag_vfs_use_driver();
        }
        if (run_transfer() == 0) {
            esp_restart();
        }
        /* on error just retry after reboot */
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    /* ---- player ---- */
    ESP_LOGI(TAG, "player start");
    FILE *ix = fopen(VID_DIR "/frames.idx", "rb");
    assert(ix);
    uint32_t nframes;
    assert(fread(&nframes, 4, 1, ix) == 1);
    frame_ent_t *ents = malloc(nframes * sizeof(*ents));
    assert(ents);
    assert(fread(ents, sizeof(*ents), nframes, ix) == nframes);
    fclose(ix);
    /* Whole MJPEG in PSRAM: per-frame SD pread costs ~24ms (the bottleneck);
     * 7.2MB fits easily in 32MB PSRAM alongside the 1.9MB audio. */
    uint8_t *mjpg_buf;
    size_t mjpg_len;
    {
        FILE *mf = fopen(VID_DIR "/frames.mjpeg", "rb");
        assert(mf);
        fseek(mf, 0, SEEK_END);
        mjpg_len = ftell(mf);
        fseek(mf, 0, SEEK_SET);
        mjpg_buf = heap_caps_malloc(mjpg_len, MALLOC_CAP_SPIRAM);
        assert(mjpg_buf);
        assert(fread(mjpg_buf, 1, mjpg_len, mf) == mjpg_len);
        fclose(mf);
        ESP_LOGI(TAG, "mjpeg %u bytes in PSRAM", (unsigned)mjpg_len);
    }

    FILE *af = fopen(VID_DIR "/audio.pcm", "rb");
    assert(af);
    fseek(af, 0, SEEK_END);
    audio_len = ftell(af);
    fseek(af, 0, SEEK_SET);
    audio_buf = heap_caps_malloc(audio_len, MALLOC_CAP_SPIRAM);
    assert(audio_buf);
    assert(fread(audio_buf, 1, audio_len, af) == audio_len);
    fclose(af);

    /* display, no LVGL */
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    bsp_display_config_t dc = {
        .dsi_bus = {
            .phy_clk_src = 0, /* let the driver choose the default */
            .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        }
    };
    ESP_ERROR_CHECK(bsp_display_new(&dc, &panel, &io));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    void *fb0, *fb1;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &fb0, &fb1));
    ESP_LOGI(TAG, "fb0=%p fb1=%p", fb0, fb1);

    /* jpeg decoder */
    jpeg_decoder_handle_t dec;
    jpeg_decode_engine_cfg_t dec_cfg = {.intr_priority = 0, .timeout_ms = 100};
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&dec_cfg, &dec));
    jpeg_decode_cfg_t jpg_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    /* PSRAM staging for compressed input (max frame observed ~64KB, margin x4) */
    size_t in_sz = 0;
    jpeg_decode_memory_alloc_cfg_t in_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    uint8_t *in_buf = jpeg_alloc_decoder_mem(256 * 1024, &in_cfg, &in_sz);
    assert(in_buf);

    /* audio path */
    spk = bsp_audio_codec_speaker_init();
    assert(spk);
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = A_RATE, .channel = A_CH, .bits_per_sample = 16};
    ESP_ERROR_CHECK(esp_codec_dev_open(spk, &fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(spk, 0)); /* muted for now */
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 12, NULL, 1);

    /* video loop driven by audio clock */
    uint32_t shown = 0xFFFFFFFFu;
    void *fbs[2] = {fb0, fb1};
    int64_t t0 = esp_timer_get_time();
    uint32_t frames_shown = 0, frames_skip = 0;
    uint64_t rd_us = 0, dec_us = 0, dr_us = 0;
    int cur = 0;
    /* (Re)subscribe the main task: startup does not guarantee subscription,
     * and transfer mode deinitializes the watchdog. */
    {
        esp_err_t wr = esp_task_wdt_add(NULL);
        if (wr != ESP_OK && wr != ESP_ERR_INVALID_STATE)
            ESP_ERROR_CHECK(wr);
    }
    while (1) {
        esp_task_wdt_reset();
        uint32_t target = (audio_samples * FPS) / A_RATE;
        if (target >= nframes) {
            /* loop handled by audio wrap; wait for it */
            vTaskDelay(1);
            continue;
        }
        if (target == shown) {
            vTaskDelay(1);
            continue;
        }
        if (target < shown)
            shown = 0xFFFFFFFFu; /* wrapped */
        if (target > shown + 1 && shown != 0xFFFFFFFFu)
            frames_skip += (target - shown - 1);
        /* read + decode into inactive fb */
        cur ^= 1;
        int64_t r0 = esp_timer_get_time();
        assert(ents[target].off + ents[target].len <= mjpg_len);
        memcpy(in_buf, mjpg_buf + ents[target].off, ents[target].len);
        uint32_t got = ents[target].len;
        int64_t r1 = esp_timer_get_time();
        uint32_t out_sz = 0;
        esp_err_t r = jpeg_decoder_process(dec, &jpg_cfg, in_buf, got,
                                           fbs[cur], FB_W * FB_H * 2, &out_sz);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "jpg dec fail f%u", (unsigned)target);
            continue;
        }
        int64_t r2 = esp_timer_get_time();
        esp_cache_msync(fbs[cur], FB_W * FB_H * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, FB_W, FB_H, fbs[cur]));
        int64_t r3 = esp_timer_get_time();
        rd_us += (uint64_t)(r1 - r0);
        dec_us += (uint64_t)(r2 - r1);
        dr_us += (uint64_t)(r3 - r2);
        shown = target;
        frames_shown++;
        if ((frames_shown & 63) == 0) {
            float el = (esp_timer_get_time() - t0) / 1e6f;
            ESP_LOGI(TAG, "shown=%u skip=%u t=%.1fs fps=%.1f rd=%ums dec=%ums dr=%ums",
                     (unsigned)frames_shown, (unsigned)frames_skip, el,
                     frames_shown / el, (unsigned)(rd_us / 1000 / frames_shown),
                     (unsigned)(dec_us / 1000 / frames_shown),
                     (unsigned)(dr_us / 1000 / frames_shown));
        }
    }
}
