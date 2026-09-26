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
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/jpeg_decode.h"
#include "driver/usb_serial_jtag.h"
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
static uint32_t crc_upd(uint32_t c, const uint8_t *d, size_t n)
{
    c ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc32_tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------- USB-serial line helpers (raw, logs off in xfer mode) ---------- */
static int usj_read_exact(uint8_t *buf, size_t n, int timeout_ms)
{
    size_t got = 0;
    int64_t dl = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (got < n) {
        int r = usb_serial_jtag_read_bytes(buf + got, n - got, 20);
        if (r > 0)
            got += r;
        if (esp_timer_get_time() > dl)
            return -1;
    }
    return 0;
}
static int usj_read_line(char *buf, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        if (usj_read_exact((uint8_t *)&c, 1, 5000) < 0)
            return -1;
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
    usb_serial_jtag_write_bytes(s, strlen(s), 1000);
    usb_serial_jtag_wait_transmit_done(1000);
}

/* ---------------- TRANSFER mode ---------------- */
static const char *kFiles[] = {"meta.json", "frames.mjpeg", "frames.idx", "audio.pcm"};

static int run_transfer(void)
{
    char line[128];
    ESP_LOGI(TAG, "waiting for host (TAB5XFER) ...");
    while (1) {
        if (usj_read_line(line, sizeof(line)) < 0)
            continue;
        if (!strcmp(line, "TAB5XFER"))
            break;
    }
    usj_write_str("READY\n");
    mkdir(VID_DIR, 0777);
    for (int i = 0; i < 4; i++) {
        if (usj_read_line(line, sizeof(line)) < 0)
            return -1;
        char name[32];
        uint32_t size, want;
        if (sscanf(line, "FILE %31s %u %u", name, &size, &want) != 3)
            return -1;
        if (strcmp(name, kFiles[i])) {
            usj_write_str("ERR\n");
            return -1;
        }
        usj_write_str("GO\n");
        char path[64];
        snprintf(path, sizeof(path), VID_DIR "/%s", name);
        FILE *f = fopen(path, "wb");
        if (!f) {
            usj_write_str("ERR\n");
            return -1;
        }
        uint32_t crc = 0xFFFFFFFFu ^ 0xFFFFFFFFu; /* start 0 */
        crc = 0;
        uint32_t left = size;
        uint8_t buf[4096];
        uint32_t c = 0;
        while (left) {
            size_t ch = left > sizeof(buf) ? sizeof(buf) : left;
            if (usj_read_exact(buf, ch, 10000) < 0) {
                fclose(f);
                return -1;
            }
            /* incremental crc32 */
            c ^= 0xFFFFFFFFu;
            for (size_t k = 0; k < ch; k++)
                c = crc32_tab[(c ^ buf[k]) & 0xFF] ^ (c >> 8);
            c ^= 0xFFFFFFFFu;
            if (fwrite(buf, 1, ch, f) != ch) {
                fclose(f);
                return -1;
            }
            left -= ch;
        }
        (void)crc;
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
            ESP_LOGE(TAG, "%s crc mismatch got %08X want %08X", name, v, want);
            usj_write_str("ERR\n");
            return -1;
        }
        char ok[48];
        snprintf(ok, sizeof(ok), "OK %08X\n", v);
        usj_write_str(ok);
        ESP_LOGI(TAG, "%s %u bytes crc %08X", name, size, v);
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
    const size_t CHUNK = 8192;
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
    ESP_ERROR_CHECK(bsp_sdcard_mount());
    ESP_ERROR_CHECK(bsp_display_brightness_init());

    struct stat st;
    bool have_bundle = (stat(FLAG_FILE, &st) == 0);
    if (!have_bundle) {
        /* quiet console so the byte stream stays clean */
        esp_log_level_set("*", ESP_LOG_NONE);
        usb_serial_jtag_driver_install(NULL);
        if (run_transfer() == 0) {
            esp_restart();
        }
        /* on error just retry after reboot */
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
    int mjpg = open(VID_DIR "/frames.mjpeg", O_RDONLY);
    assert(mjpg >= 0);

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
    bsp_display_config_t dc = {0};
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
    uint8_t *in_buf = jpeg_alloc_decoder_mem(256 * 1024, NULL, &in_sz);
    assert(in_buf);

    /* audio path */
    spk = bsp_audio_codec_speaker_init();
    assert(spk);
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = A_RATE, .channel = A_CH, .bits_per_sample = 16};
    ESP_ERROR_CHECK(esp_codec_dev_open(spk, &fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(spk, 70));
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 12, NULL, 1);

    /* video loop driven by audio clock */
    uint32_t shown = 0xFFFFFFFFu;
    void *fbs[2] = {fb0, fb1};
    int64_t t0 = esp_timer_get_time();
    uint32_t frames_shown = 0, frames_skip = 0;
    int cur = 0;
    while (1) {
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
        uint32_t got = pread(mjpg, in_buf, ents[target].len, ents[target].off);
        assert(got == ents[target].len);
        uint32_t out_sz = 0;
        esp_err_t r = jpeg_decoder_process(dec, &jpg_cfg, in_buf, got,
                                           fbs[cur], FB_W * FB_H * 2, &out_sz);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "jpg dec fail f%u", target);
            continue;
        }
        esp_cache_msync(fbs[cur], FB_W * FB_H * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, FB_W, FB_H, fbs[cur]));
        shown = target;
        frames_shown++;
        if ((frames_shown & 63) == 0) {
            float el = (esp_timer_get_time() - t0) / 1e6f;
            ESP_LOGI(TAG, "shown=%u skip=%u t=%.1fs fps=%.1f", frames_shown,
                     frames_skip, el, frames_shown / el);
        }
    }
}
