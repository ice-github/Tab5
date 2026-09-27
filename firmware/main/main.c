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
#include "esp_lcd_touch.h"
#include "esp_codec_dev.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"
#include "esp_pm.h"
#include "ina_diag.h"
/* Raw FatFs + DMA-capable alloc for the prefetch path */
#include "esp_dma_utils.h"
#include "ff.h"

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

/* ---------------- touch pause / OSD ----------------
 * No LVGL in this pipeline: the OSD is drawn with direct RGB565
 * rectangle fills + a minimal embedded 5x7 font into the DPI
 * framebuffer that is currently on screen. A PSRAM backup holds the
 * pristine paused frame so the OSD can be redrawn / removed cleanly.
 *
 * Pause runs on a flag, not vTaskSuspend: audio_task finishes its
 * in-flight 10ms chunk, then stops advancing audio_pos/audio_samples
 * (the video clock). Resume continues from the exact frozen position,
 * so A/V sync — including loop-boundary cases — is automatic.
 *
 * Touch UI is landscape 1280x720, locked to the video content orientation.
 * Video frames are pre-rotated with transpose=1 (90deg CW): landscape (lx,ly)
 * -> portrait fb (719-ly, lx). The OSD uses the same transform so text reads
 * upright when the scene is upright: UI (ux,uy) -> fb (719-uy, ux), and raw
 * touch (rx,ry) -> UI (ry, 719-rx). touch_to_ui() is the single compensation
 * point; every press logs raw + UI coords + hit result for on-device check.
 *
 * Actions fire on the press edge (no press/release same-button requirement):
 * one tap = one action, immune to release-coordinate jitter. Tapping outside
 * the buttons resumes playback (no RESUME button).
 */
#define OSD_VOL_STEP  10
#define OSD_BRI_STEP  10
#define OSD_BRI_MIN   10   /* keep OSD visible */
#define OSD_BRI_MAX   100
#define OSD_BRI_INIT  50
#define OSD_FB_SZ     (FB_W * FB_H * 2)

#define OSD_C_WHITE 0xFFFF
#define OSD_C_BG    0x10A2  /* dark panel */
#define OSD_C_BTN   0x2945  /* slate button fill */
#define OSD_C_VAL   0xFFE0  /* yellow values */

static volatile bool s_paused = false;
static int s_vol = 0;              /* 0..100, esp_codec_dev range */
static int s_bri = OSD_BRI_INIT;   /* 10..100 */
static esp_lcd_touch_handle_t s_tp = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_pause_backup = NULL; /* PSRAM copy of paused frame */
static void *s_displayed_fb = NULL;     /* fb last sent to the panel */

/* ---- power saving state (A/B/D) ----
 * s_codec_open=false means esp_codec_dev is closed and the speaker amp
 * rail is off (BSP_FEATURE_SPEAKER). audio_task never calls write while
 * closed; ui_touch_step owns suspend/resume from the main-loop context
 * (I2C inside bsp_feature_enable is not ISR-safe).
 * s_lcd_off=true means DISPOFF + backlight 0. DSI PHY/LDO stays on
 * (no public API to drop it); resume re-sends the OSD framebuffer. */
static volatile bool s_codec_open = true;
static bool s_lcd_off = false;

static void audio_suspend(void)
{
    if (!s_codec_open || spk == NULL)
        return;
    /* s_paused is already true: audio_task finishes its in-flight ~10ms
     * write and then freezes. Wait it out so close() never races write(). */
    vTaskDelay(pdMS_TO_TICKS(25));
    esp_codec_dev_close(spk);
    s_codec_open = false;
    bsp_feature_enable(BSP_FEATURE_SPEAKER, false);
    ESP_LOGI(TAG, "audio suspended (codec closed, amp off)");
}

static void audio_resume(void)
{
    if (s_codec_open || spk == NULL)
        return;
    bsp_feature_enable(BSP_FEATURE_SPEAKER, true);
    vTaskDelay(pdMS_TO_TICKS(30)); /* amp rail settle + I2C flush */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = A_RATE, .channel = A_CH, .bits_per_sample = 16};
    if (esp_codec_dev_open(spk, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "audio resume: open failed");
        return;
    }
    s_codec_open = true;
    /* Re-apply the user volume; _update_codec_setting restores it too,
     * but explicit is robust across sw_vol recreation. */
    esp_codec_dev_set_out_vol(spk, s_vol);
    ESP_LOGI(TAG, "audio resumed (vol %d)", s_vol);
}

static void display_off(void)
{
    if (s_lcd_off)
        return;
    if (s_panel != NULL)
        esp_lcd_panel_disp_on_off(s_panel, false); /* DISPOFF, not deep sleep */
    bsp_display_brightness_set(0);
    /* Touch sleep is intentionally never used: on rev1 (GT911, INT=NC)
     * enter_sleep succeeds but exit_sleep is a silent no-op, leaving touch
     * stuck and the OFF-state wake tap dead. ST712x has no sleep support
     * at all. Polling continues so any tap still wakes. */
    s_lcd_off = true;
}

static void display_on(void)
{
    if (s_panel != NULL)
        esp_lcd_panel_disp_on_off(s_panel, true); /* DISPON */
    /* Never bsp_display_backlight_on(): it forces 100%, ignoring s_bri. */
    bsp_display_brightness_set(s_bri);
    /* No touch wake call: sleep is never entered (see display_off). */
    s_lcd_off = false;
}

/* Minimal 5x7 font: rows are 5-bit values, bit4 = leftmost pixel.
 * Covers only what the OSD needs: space + - % 0-9 A B D E I L M O P R S U V T */
typedef struct { char ch; uint8_t row[7]; } osd_glyph_t;
static const osd_glyph_t OSD_FONT[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'+', {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'%', {0x19,0x1A,0x02,0x04,0x08,0x0B,0x13}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3', {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'I', {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x19,0x19,0x15,0x13,0x13,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
};
static const uint8_t *osd_glyph(char c)
{
    for (size_t i = 0; i < sizeof(OSD_FONT) / sizeof(OSD_FONT[0]); i++)
        if (OSD_FONT[i].ch == c)
            return OSD_FONT[i].row;
    return OSD_FONT[0].row; /* unknown -> space */
}

#define UI_W 1280
#define UI_H 720

/* UI (landscape) -> portrait fb, same transform as the video frames. */
static void ui_fill(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > UI_W) x1 = UI_W;
    if (y1 > UI_H) y1 = UI_H;
    for (int uy = y0; uy < y1; uy++)
        for (int ux = x0; ux < x1; ux++)
            fb[(size_t)ux * FB_W + (FB_W - 1 - uy)] = c;
}
static void ui_char(uint16_t *fb, int x, int y, int sc, char c, uint16_t col)
{
    const uint8_t *g = osd_glyph(c);
    for (int r = 0; r < 7; r++)
        for (int b = 0; b < 5; b++)
            if (g[r] & (1u << (4 - b)))
                ui_fill(fb, x + b * sc, y + r * sc, x + (b + 1) * sc,
                        y + (r + 1) * sc, col);
}
static void ui_text(uint16_t *fb, int x, int y, int sc, const char *s, uint16_t col)
{
    while (*s) {
        ui_char(fb, x, y, sc, *s, col);
        x += 6 * sc;
        s++;
    }
}
static int osd_text_w(const char *s, int sc)
{
    return (int)strlen(s) * 6 * sc - sc;
}

/* OSD layout in UI (landscape 1280x720) coords. Shared by draw + hit test.
 * Panel is ~2/3 scale, centered: 854x480 at ((1280-854)/2, (720-480)/2).
 * Every button stays finger-sized (>=140 UI px wide). */
enum { BTN_V_MINUS = 0, BTN_V_PLUS, BTN_B_MINUS, BTN_B_PLUS, BTN_OFF, BTN_REPLACE, BTN_N };
typedef struct { int x0, y0, x1, y1; const char *label; int sc; } osd_btn_t;
static const osd_btn_t OSD_BTNS[BTN_N] = {
    [BTN_V_MINUS] = { 250, 240, 430, 340, "V-", 6 },
    [BTN_V_PLUS]  = { 450, 240, 630, 340, "V+", 6 },
    [BTN_B_MINUS] = { 650, 240, 830, 340, "B-", 6 },
    [BTN_B_PLUS]  = { 850, 240, 1030, 340, "B+", 6 },
    [BTN_OFF]     = { 400, 365, 880, 435, "SCREEN OFF", 5 },
    [BTN_REPLACE] = { 400, 445, 880, 515, "REPLACE", 5 },
};
#define OSD_BOX_X0 213
#define OSD_BOX_Y0 120
#define OSD_BOX_X1 1067
#define OSD_BOX_Y1 600
/* label rows / scales inside the shrunk panel */
#define OSD_TITLE_Y   145
#define OSD_TITLE_SC  5
#define OSD_LAB_Y     200
#define OSD_LAB_SC    4
#define OSD_LAB_VOL_X 440   /* center over the V button pair */
#define OSD_LAB_BRI_X 840   /* center over the B button pair */
#define OSD_HINT_Y    550
#define OSD_HINT_SC   3
#define OSD_BAT_Y     520
#define OSD_BAT_SC    3

static int osd_hit(int x, int y)
{
    for (int i = 0; i < BTN_N; i++)
        if (x >= OSD_BTNS[i].x0 && x < OSD_BTNS[i].x1 &&
            y >= OSD_BTNS[i].y0 && y < OSD_BTNS[i].y1)
            return i;
    return -1;
}

static void osd_present(void)
{
    /* CPU-modified framebuffer -> panel, same pattern as video path. */
    esp_cache_msync(s_displayed_fb, OSD_FB_SZ,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, FB_W, FB_H,
                                              s_displayed_fb));
}

static void osd_draw_all(void)
{
    uint16_t *fb = (uint16_t *)s_displayed_fb;
    char line[16];
    /* restore pristine frame, then dim it for OSD contrast */
    memcpy(fb, s_pause_backup, OSD_FB_SZ);
    size_t px = (size_t)FB_W * FB_H;
    for (size_t i = 0; i < px; i++)
        fb[i] = (uint16_t)((fb[i] & 0xF7DEu) >> 1);
    /* panel */
    ui_fill(fb, OSD_BOX_X0, OSD_BOX_Y0, OSD_BOX_X1, OSD_BOX_Y1, OSD_C_BG);
    ui_fill(fb, OSD_BOX_X0, OSD_BOX_Y0, OSD_BOX_X1, OSD_BOX_Y0 + 4, OSD_C_WHITE);
    ui_fill(fb, OSD_BOX_X0, OSD_BOX_Y1 - 4, OSD_BOX_X1, OSD_BOX_Y1, OSD_C_WHITE);
    ui_fill(fb, OSD_BOX_X0, OSD_BOX_Y0, OSD_BOX_X0 + 4, OSD_BOX_Y1, OSD_C_WHITE);
    ui_fill(fb, OSD_BOX_X1 - 4, OSD_BOX_Y0, OSD_BOX_X1, OSD_BOX_Y1, OSD_C_WHITE);
    const char *title = "PAUSED";
    ui_text(fb, (UI_W - osd_text_w(title, OSD_TITLE_SC)) / 2, OSD_TITLE_Y,
            OSD_TITLE_SC, title, OSD_C_WHITE);
    snprintf(line, sizeof(line), "VOL %d%%", s_vol);
    ui_text(fb, OSD_LAB_VOL_X - osd_text_w(line, OSD_LAB_SC) / 2, OSD_LAB_Y,
            OSD_LAB_SC, line, OSD_C_VAL);
    snprintf(line, sizeof(line), "BRI %d%%", s_bri);
    ui_text(fb, OSD_LAB_BRI_X - osd_text_w(line, OSD_LAB_SC) / 2, OSD_LAB_Y,
            OSD_LAB_SC, line, OSD_C_VAL);
    for (int i = 0; i < BTN_N; i++) {
        ui_fill(fb, OSD_BTNS[i].x0, OSD_BTNS[i].y0,
                OSD_BTNS[i].x1, OSD_BTNS[i].y1, OSD_C_BTN);
        ui_fill(fb, OSD_BTNS[i].x0, OSD_BTNS[i].y0,
                OSD_BTNS[i].x1, OSD_BTNS[i].y0 + 3, OSD_C_WHITE);
        ui_fill(fb, OSD_BTNS[i].x0, OSD_BTNS[i].y1 - 3,
                OSD_BTNS[i].x1, OSD_BTNS[i].y1, OSD_C_WHITE);
        ui_fill(fb, OSD_BTNS[i].x0, OSD_BTNS[i].y0,
                OSD_BTNS[i].x0 + 3, OSD_BTNS[i].y1, OSD_C_WHITE);
        ui_fill(fb, OSD_BTNS[i].x1 - 3, OSD_BTNS[i].y0,
                OSD_BTNS[i].x1, OSD_BTNS[i].y1, OSD_C_WHITE);
        int tw = osd_text_w(OSD_BTNS[i].label, OSD_BTNS[i].sc);
        int th = 7 * OSD_BTNS[i].sc;
        ui_text(fb, (OSD_BTNS[i].x0 + OSD_BTNS[i].x1 - tw) / 2,
                (OSD_BTNS[i].y0 + OSD_BTNS[i].y1 - th) / 2, OSD_BTNS[i].sc,
                OSD_BTNS[i].label, OSD_C_WHITE);
    }
    const char *hint = "TAP OUTSIDE TO RESUME";
    ui_text(fb, (UI_W - osd_text_w(hint, OSD_HINT_SC)) / 2, OSD_HINT_Y,
            OSD_HINT_SC, hint, OSD_C_WHITE);
    /* Battery status: voltage from INA226 bus rail, + while trickle
     * charging (CHG_STAT low), - otherwise. Unknown -> dashes. */
    {
        char bat[20];
        int32_t mv = ina_bus_mv();
        int chg = power_charge_level();
        if (mv < 0)
            snprintf(bat, sizeof(bat), "BAT --.--V -");
        else
            snprintf(bat, sizeof(bat), "BAT %d.%02dV %c",
                     (int)(mv / 1000), (int)((mv % 1000) / 10),
                     (chg == 0) ? '+' : '-');
        ui_text(fb, (UI_W - osd_text_w(bat, OSD_BAT_SC)) / 2, OSD_BAT_Y,
                OSD_BAT_SC, bat, OSD_C_VAL);
    }
}

static void osd_apply(int id)
{
    if (id == BTN_V_MINUS || id == BTN_V_PLUS) {
        s_vol += (id == BTN_V_PLUS) ? OSD_VOL_STEP : -OSD_VOL_STEP;
        if (s_vol < 0) s_vol = 0;
        if (s_vol > 100) s_vol = 100;
        if (!s_codec_open) {
            /* Paused with codec closed: defer HW apply to audio_resume(). */
            ESP_LOGI(TAG, "osd volume %d (deferred, codec closed)", s_vol);
        } else if (esp_codec_dev_set_out_vol(spk, s_vol) != 0) {
            ESP_LOGW(TAG, "set_out_vol %d failed", s_vol);
        } else {
            ESP_LOGI(TAG, "osd volume %d", s_vol);
        }
    } else if (id == BTN_B_MINUS || id == BTN_B_PLUS) {
        s_bri += (id == BTN_B_PLUS) ? OSD_BRI_STEP : -OSD_BRI_STEP;
        if (s_bri < OSD_BRI_MIN) s_bri = OSD_BRI_MIN;
        if (s_bri > OSD_BRI_MAX) s_bri = OSD_BRI_MAX;
        ESP_ERROR_CHECK(bsp_display_brightness_set(s_bri));
        ESP_LOGI(TAG, "osd brightness %d", s_bri);
    }
    osd_draw_all();
    osd_present();
}

/* Single compensation point for touch orientation. Assumes the controller
 * reports portrait fb pixels (BSP defaults: x_max=720, y_max=1280, no
 * swap/mirror); converts to UI landscape coords with the video transform.
 * Adjust here if the on-device orientation check shows otherwise. */
static void touch_to_ui(uint16_t rx, uint16_t ry, int *ox, int *oy)
{
    int ux = (int)ry, uy = (FB_W - 1) - (int)rx;
    if (ux < 0) ux = 0;
    if (ux >= UI_W) ux = UI_W - 1;
    if (uy < 0) uy = 0;
    if (uy >= UI_H) uy = UI_H - 1;
    *ox = ux;
    *oy = uy;
}

/* Non-blocking touch poll: true + RAW controller position while down. */
static bool touch_poll_raw(unsigned *rx, unsigned *ry)
{
    if (s_tp == NULL)
        return false;
    if (esp_lcd_touch_read_data(s_tp) != ESP_OK)
        return false;
    esp_lcd_touch_point_data_t d[2];
    uint8_t n = 0;
    if (esp_lcd_touch_get_data(s_tp, d, &n, 2) != ESP_OK)
        return false;
    if (n == 0)
        return false;
    *rx = d[0].x;
    *ry = d[0].y;
    return true;
}

typedef enum { UI_PLAY = 0, UI_OSD, UI_OFF } ui_state_t;
static ui_state_t s_ui = UI_PLAY;

/* Press-edge state machine, called once per main-loop iteration.
 * PLAY: any press pauses. OSD: button press acts, press outside resumes.
 * OFF (backlight 0): any press wakes back to OSD, still paused. */
static void ui_touch_step(bool can_pause)
{
    static bool pressed = false;
    unsigned rx = 0, ry = 0;
    bool touching = touch_poll_raw(&rx, &ry);
    if (touching && !pressed) {
        pressed = true;
        int ux = 0, uy = 0;
        touch_to_ui((uint16_t)rx, (uint16_t)ry, &ux, &uy);
        int hit = (s_ui == UI_OSD) ? osd_hit(ux, uy) : -1;
        ESP_LOGI(TAG, "tap raw=%u,%u ui=%d,%d state=%d hit=%d",
                 rx, ry, ux, uy, (int)s_ui, hit);
        if (s_ui == UI_PLAY) {
            if (can_pause && s_displayed_fb != NULL) {
                s_paused = true; /* audio_task freezes at chunk boundary */
                audio_suspend(); /* A: codec close + amp off (video+audio cut) */
                memcpy(s_pause_backup, s_displayed_fb, OSD_FB_SZ);
                osd_draw_all();
                osd_present();
                s_ui = UI_OSD;
                ina_diag_dump(); /* flush power history for post-reconnect read */
                ESP_LOGI(TAG, "paused at sample %lu",
                         (unsigned long)audio_samples);
            }
        } else if (s_ui == UI_OSD) {
            if (hit == BTN_OFF) {
                /* B: DISPOFF + backlight 0 (+best-effort touch sleep).
                 * Audio is already suspended from the PLAY->OSD transition;
                 * re-assert in case the state was reached another way. */
                audio_suspend();
                display_off();
                s_ui = UI_OFF;
                ESP_LOGI(TAG, "screen off (lcd+audio suspended)");
            } else if (hit == BTN_REPLACE) {
                /* Drop the bundle flag and reboot into transfer mode so the
                 * host can send a new bundle. Old files are overwritten. */
                ESP_LOGI(TAG, "replace bundle: removing flag, rebooting");
                unlink(FLAG_FILE);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            } else if (hit >= 0) {
                osd_apply(hit);
            } else {
                /* outside buttons: hide OSD, resume from frozen position */
                memcpy(s_displayed_fb, s_pause_backup, OSD_FB_SZ);
                osd_present();
                audio_resume(); /* amp on + codec open + vol re-apply */
                s_paused = false;
                s_ui = UI_PLAY;
                ESP_LOGI(TAG, "resumed at sample %lu",
                         (unsigned long)audio_samples);
            }
        } else { /* UI_OFF: wake to OSD, stay paused */
            display_on(); /* DISPON + s_bri restore + OSD FB resend */
            osd_draw_all();
            osd_present();
            s_ui = UI_OSD;
            ESP_LOGI(TAG, "screen on (bri %d)", s_bri);
        }
    } else if (!touching && pressed) {
        pressed = false;
    }
}

static volatile uint32_t a_write_us; /* last esp_codec_dev_write duration, us */

static void audio_task(void *arg)
{
    /* Small chunks so audio_samples (the video clock) updates every ~10ms.
     * An 8KB chunk blocks ~43ms per write and quantizes video to 23fps. */
    const size_t CHUNK = 1920; /* 480 stereo frames = 10.0ms @48kHz */
    while (1) {
        if (s_paused || !s_codec_open) {
            /* frozen: audio_pos/audio_samples untouched, video clock holds.
             * !s_codec_open covers the suspend window (close+amp off). */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (audio_pos >= audio_len) {
            audio_pos = 0; /* loop */
            audio_samples = 0;
        }
        size_t n = audio_len - audio_pos;
        if (n > CHUNK)
            n = CHUNK;
        if (s_vol == 0) {
            /* D: vol=0 is -50dB HW attenuation with DMA still running.
             * Skip the I2S transaction and advance the video clock on the
             * FreeRTOS tick instead. A/V sync holds because the clock is
             * the only consumer; wall-clock rate error is acceptable for
             * a muted stream. ES8388/PA rails stay up (see A for full
             * suspend when paused). */
            vTaskDelay(pdMS_TO_TICKS(10));
            a_write_us = 0;
            audio_pos += n;
            audio_samples += (uint32_t)(n / (A_CH * 2));
            continue;
        }
        int64_t a0 = esp_timer_get_time();
        esp_codec_dev_write(spk, audio_buf + audio_pos, n);
        a_write_us = (uint32_t)(esp_timer_get_time() - a0);
        audio_pos += n;
        audio_samples += (uint32_t)(n / (A_CH * 2));
    }
}

/* ---------------- SD prefetch + profiling ----------------
 * A 30s bundle holds 900 frames (~90MB MJPEG): far beyond 32MB PSRAM,
 * so the whole-file preload is replaced by an SD reader task feeding a
 * PSRAM slot ring. Each slot holds one contiguous FILE EXTENT covering
 * several whole frames (a single fseek+fread per extent): per-frame
 * fseek+fread costs ~26ms on this SD path even sequentially, so one
 * frame per slot starves the 33ms budget. The bundle format is unchanged
 * (frames.mjpeg + frames.idx with u32 n + n x (u32 off, u32 len) +
 * audio.pcm s16le 48k stereo); runs never cross EOF (loop wrap bumps
 * pf_epoch, which empties the ring).
 *
 * Sync contract (unchanged from preload design):
 *  - video clock = audio_samples; pause freezes it via s_paused;
 *  - pause: the reader freezes too (filled slots are kept), so resume
 *    continues from the exact frozen position;
 *  - loop wrap: main bumps pf_epoch; the reader drops its window and
 *    restarts prefetch at pf_want;
 *  - underrun (extent not READY within PF_WAIT_MS): counted + throttled
 *    WARN, frame skipped; the audio clock keeps running so A/V sync is
 *    preserved by construction (the miss surfaces in skip too).
 * A 10s bundle plays through the same path (fewer frames, same code).
 */
#define PF_SLOT_BYTES   (256 * 1024)
#define PF_SLOTS_MIN    3
#define PF_SLOTS_MAX    16
#define PF_WAIT_MS      100
#define PF_AHEAD        48    /* reader keeps this many frames buffered */
#define PROF_WIN        64

typedef enum { PF_EMPTY = 0, PF_FILLING, PF_READY } pf_state_t;
typedef struct {
    uint8_t *data;               /* PSRAM, PF_SLOT_BYTES */
    uint32_t file_off;           /* first wanted byte offset in frames.mjpeg */
    uint32_t data_off;           /* file offset of data[0] (sector-aligned) */
    uint32_t total;              /* wanted extent bytes (whole frames only) */
    uint32_t first;              /* first frame index in extent */
    uint32_t count;              /* frames in extent */
    uint32_t sd_us;              /* single fseek+fread time, reader-measured */
    volatile pf_state_t state;
} pf_slot_t;

static pf_slot_t *pf_slots;
static int pf_nslots;
static uint32_t pf_nframes;
static frame_ent_t *pf_ents;
static size_t pf_mjpg_len;       /* from stat, for bounds checks */
static volatile uint32_t pf_want;  /* next frame the main loop needs */
static volatile uint32_t pf_epoch; /* bumped on loop wrap to reset reader */
static SemaphoreHandle_t pf_mutex;
static volatile uint32_t pf_underrun;
static volatile bool pf_primed; /* reader has PF_AHEAD frames buffered */
/* FIL is ~4KB+ (per-file FATFS cache); far too big for the reader task
 * stack (was: stack protection fault), so it lives in BSS. Only
 * prefetch_task touches it. */
static FIL pf_mf;

/* pf_mutex must be held. Find the READY extent containing frame. */
static pf_slot_t *pf_find(uint32_t frame)
{
    uint32_t off = pf_ents[frame].off;
    for (int i = 0; i < pf_nslots; i++)
        if (pf_slots[i].state == PF_READY &&
            off >= pf_slots[i].file_off &&
            off < pf_slots[i].file_off + pf_slots[i].total)
            return &pf_slots[i];
    return NULL;
}

static void prefetch_task(void *arg)
{
    /* Raw FatFs (not stdio/VFS fread): measured ~10MB/s vs ~1.2MB/s
     * through fread on the same card/bus. Drive 0 = SD (bench-verified). */
    FIL *mf = &pf_mf;
    if (f_open(mf, "0:/video/frames.mjpeg", FA_READ) != FR_OK) {
        ESP_LOGE(TAG, "prefetch: f_open frames.mjpeg failed");
        vTaskDelete(NULL);
        return;
    }
    /* mf file offset after the last successful f_read; -1 = unknown.
     * Extents are fetched in file order, so consecutive extents usually
     * need no f_lseek at all. */
    long file_pos = -1;
    uint32_t seen_epoch = 0;
    uint32_t last_bad = 0xFFFFFFFFu; /* oversize-run log guard */
    uint32_t fail_spin = 0; /* consecutive fill failures (spin guard) */
    ESP_LOGI(TAG, "prefetch start nslots=%d nframes=%lu stack=%u",
             pf_nslots, (unsigned long)pf_nframes,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    while (1) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        uint32_t want = pf_want;
        xSemaphoreTake(pf_mutex, portMAX_DELAY);
        /* Loop wrap does NOT empty the ring: the reader is want-driven,
         * so it resyncs from pf_want on its own and stale extents are
         * evicted by age. Emptying would stall every loop boundary. */
        if (seen_epoch != pf_epoch) {
            seen_epoch = pf_epoch;
            ESP_LOGI(TAG, "prefetch new epoch want f%u", (unsigned)want);
        }
        /* coverage: consecutive READY frames starting at want */
        uint32_t covered = 0;
        while (covered < PF_AHEAD) {
            uint32_t f = want + covered;
            if (f >= pf_nframes)
                break; /* stop at EOF; wrap resets the epoch */
            if (pf_find(f) == NULL)
                break;
            covered++;
            /* Coverage must terminate in <=48 hits; abort with
             * state instead of wedging silently. */
            if (covered > PF_AHEAD + 8) {
                ESP_LOGE(TAG,
                         "coverage runaway want=%lu covered=%lu nslots=%d "
                         "nframes=%lu",
                         (unsigned long)want, (unsigned long)covered,
                         pf_nslots, (unsigned long)pf_nframes);
                abort();
            }
        }
        /* next run: whole frames from want+covered, capped by slot size.
         * The read starts at the sector-aligned floor of f0's offset:
         * a mid-sector start would misalign the DMA destination and push
         * the whole bulk read into the 512B bounce path (~8x slower).
         * Frame bytes begin at data[] + (off - aoff). */
        uint32_t f0 = want + covered, total = 0, n = 0;
        uint32_t aoff = 0, head = 0;
        if (covered < PF_AHEAD && f0 < pf_nframes) {
            aoff = pf_ents[f0].off & ~511u;
            head = pf_ents[f0].off - aoff;
            while (f0 + n < pf_nframes &&
                   total + pf_ents[f0 + n].len <=
                       PF_SLOT_BYTES - 512) {
                total += pf_ents[f0 + n].len;
                n++;
            }
        }
        /* claim an EMPTY slot, else evict the stalest READY extent that
         * does not overlap the useful window [want, f0+n). */
        pf_slot_t *slot = NULL;
        if (n > 0) {
            for (int i = 0; i < pf_nslots; i++)
                if (pf_slots[i].state == PF_EMPTY) {
                    slot = &pf_slots[i];
                    break;
                }
            if (slot == NULL) {
                uint32_t best_age = 0;
                for (int i = 0; i < pf_nslots; i++) {
                    if (pf_slots[i].state != PF_READY)
                        continue;
                    /* The extent holding frame 0 is pinned: every loop
                     * starts there, so re-fetching it each wrap would
                     * stall the boundary (fseek + full extent read). */
                    if (pf_slots[i].first == 0)
                        continue;
                    uint32_t s_first = pf_slots[i].first;
                    uint32_t s_last = s_first + pf_slots[i].count - 1;
                    if (s_first < f0 + n && s_last >= want)
                        continue; /* overlaps useful window, keep */
                    uint32_t age =
                        (want + pf_nframes - s_first) % pf_nframes;
                    if (slot == NULL || age > best_age) {
                        slot = &pf_slots[i];
                        best_age = age;
                    }
                }
            }
        }
        if (slot == NULL || n == 0) {
            /* prime the pump: start the audio clock once the ring holds a
             * useful cushion. Fixed PF_AHEAD is unreachable when frames
             * are large (fewer frames fit the ring), so a full ring of
             * useful extents counts too. */
            if (!pf_primed) {
                int nempty = 0;
                for (int i = 0; i < pf_nslots; i++)
                    if (pf_slots[i].state == PF_EMPTY)
                        nempty++;
                if (covered >= PF_AHEAD || (nempty == 0 && covered >= 16))
                    pf_primed = true;
            }
            xSemaphoreGive(pf_mutex);
            if (n == 0 && covered < PF_AHEAD && f0 < pf_nframes &&
                f0 != last_bad) {
                ESP_LOGE(TAG, "prefetch f%u len %u exceeds slot %u",
                         (unsigned)f0, (unsigned)pf_ents[f0].len,
                         (unsigned)PF_SLOT_BYTES);
                last_bad = f0;
            }
            /* window full (reader ahead) or oversize frame: back off and
             * let the consumer run; it skips past uncacheable frames.
             * NOTE: raw ticks, not pdMS_TO_TICKS — at 100Hz anything
             * below 10ms rounds to ZERO ticks (yield-only, IDLE starves).
             * 1 tick (10ms) is harmless with a full ring cushion. */
            vTaskDelay(n == 0 ? pdMS_TO_TICKS(50) : 1);
            continue;
        }
        slot->state = PF_FILLING;
        uint32_t off = pf_ents[f0].off;
        xSemaphoreGive(pf_mutex);


        bool ok = true;
        FRESULT fr = FR_OK;
        UINT br = 0;
        uint32_t want_total = total + head; /* sector-aligned read size */
        int64_t t0 = esp_timer_get_time();
        if (off + total > pf_mjpg_len) {
            ESP_LOGE(TAG, "prefetch f%u extent %u+%u past EOF %u",
                     (unsigned)f0, (unsigned)off, (unsigned)total,
                     (unsigned)pf_mjpg_len);
            ok = false;
        } else if (file_pos < 0 || (uint32_t)file_pos != aoff) {
            fr = f_lseek(mf, aoff);
            if (fr != FR_OK) {
                ESP_LOGW(TAG, "prefetch f%u f_lseek failed", (unsigned)f0);
                ok = false;
                file_pos = -1;
            }
        }
        if (ok) {
            if ((fr = f_read(mf, slot->data, want_total, &br)) != FR_OK ||
                br != want_total) {
                ESP_LOGW(TAG,
                         "prefetch f%u extent SD read failed fr=%d br=%u",
                         (unsigned)f0, (int)fr, (unsigned)br);
                ok = false;
                file_pos = -1;
            } else {
                file_pos = (long)(aoff + want_total);
            }
        }
        uint32_t sd_us = (uint32_t)(esp_timer_get_time() - t0);
        xSemaphoreTake(pf_mutex, portMAX_DELAY);
        if (ok) {
            slot->file_off = off;
            slot->data_off = aoff;
            slot->total = total;
            slot->first = f0;
            slot->count = n;
            slot->sd_us = sd_us;
            slot->state = PF_READY;
        } else {
            slot->state = PF_EMPTY;
        }
        xSemaphoreGive(pf_mutex);
        /* Spin guard: persistent fill failures would otherwise loop with
         * no delay and starve IDLE (task watchdog). Back off and say why. */
        if (!ok) {
            if (++fail_spin == 5 || fail_spin % 50 == 0)
                ESP_LOGW(TAG,
                         "prefetch: %lu consecutive fill failures "
                         "(f0=%u off=%u total=%u mjpg=%u)",
                         (unsigned long)fail_spin, (unsigned)f0,
                         (unsigned)off, (unsigned)total,
                         (unsigned)pf_mjpg_len);
            vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            fail_spin = 0;
        }
    }
}

/* profiling: last-PROF_WIN samples per stage, avg/p95/max per window */
typedef struct {
    uint32_t v[PROF_WIN];
    int idx, n;
} prof_hist_t;
static void prof_push(prof_hist_t *h, uint32_t us)
{
    h->v[h->idx] = us;
    h->idx = (h->idx + 1) % PROF_WIN;
    if (h->n < PROF_WIN)
        h->n++;
}
static void prof_stats(prof_hist_t *h, uint32_t *avg, uint32_t *p95,
                       uint32_t *mx)
{
    if (h->n == 0) {
        *avg = *p95 = *mx = 0;
        return;
    }
    uint32_t tmp[PROF_WIN];
    uint64_t sum = 0;
    uint32_t m = 0;
    for (int i = 0; i < h->n; i++) {
        sum += h->v[i];
        if (h->v[i] > m)
            m = h->v[i];
        tmp[i] = h->v[i];
    }
    for (int i = 1; i < h->n; i++) { /* insertion sort for p95 */
        uint32_t k = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > k) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = k;
    }
    *avg = (uint32_t)(sum / (uint64_t)h->n);
    *p95 = tmp[(h->n * 95 + 99) / 100 - 1];
    *mx = m;
}

void app_main(void)
{
    /* C: DFS first step (light sleep stays OFF). max=360 (rev<3上限),
     * min from Kconfig (既定90). 再生中はDSI/DPI/I2Sの恒久ロックで
     * CPUは360固定のままなので、効果は主にpause/SCREEN OFFで出る。
     * TRANSFER中はUSB受信溢れ防止のため後段でNO_LIGHT_SLEEP相当の
     * 扱いにする (現状light_sleep=falseなので実害なし、将来用)。 */
#if CONFIG_PM_ENABLE
    {
        int min_mhz = 90;
#ifdef CONFIG_TAB5_PM_MIN_40
        min_mhz = 40;
#elif defined(CONFIG_TAB5_PM_MIN_180)
        min_mhz = 180;
#endif
        esp_pm_config_t pm_cfg = {
            .max_freq_mhz = 360,
            .min_freq_mhz = min_mhz,
            .light_sleep_enable = false,
        };
        esp_err_t pmr = esp_pm_configure(&pm_cfg);
        ESP_LOGI(TAG, "pm dfs max=360 min=%d rs=%s", min_mhz,
                 esp_err_to_name(pmr));
    }
#endif
    /* Enable battery charging (IP2326 via IO expander U7). Power-on
     * default is OFF and the factory FW turns it on after init; without
     * this the battery never charges. Covers transfer mode too. */
    power_charge_enable();
    crc_init();
    ESP_LOGI(TAG, "tab5player build %s %s", __DATE__, __TIME__);
    esp_err_t sdr = bsp_sdcard_mount();
    if (sdr != ESP_OK) {
        ESP_LOGE(TAG, "sd mount failed: %s (retrying)", esp_err_to_name(sdr));
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
#ifdef CONFIG_TAB5_SD_FREQ_LOW
    /* E: power-test remount at 20MHz (default Kconfig is 40MHz HIGHSPEED).
     * Compare sd_us avg/p95/max + pf_underrun + skip before adopting. */
    {
        bsp_sdcard_unmount();
        sdmmc_host_t host;
        bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_1, &host);
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        sdmmc_slot_config_t slot;
        bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_1, &slot);
        bsp_sdcard_cfg_t cfg = {
            .host = &host,
            .slot = &slot,
        };
        sdr = bsp_sdcard_sdmmc_mount(&cfg);
        ESP_LOGI(TAG, "sd remount 20MHz rs=%s", esp_err_to_name(sdr));
        if (sdr != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
    }
#else
    ESP_LOGI(TAG, "sd HIGHSPEED 40MHz");
#endif
    /* No bsp_display_brightness_init() here: bsp_display_new() below
     * self-initializes it (BSP docs). The early call only re-ran the same
     * LEDC setup on GPIO22. */

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
    /* No whole-MJPEG preload: a 30s bundle (~900 frames, ~90MB) does not
     * fit in 32MB PSRAM. Frames stay on SD and are pulled through the
     * prefetch ring; only the size is needed here for bounds checks. */
    {
        struct stat mst;
        assert(stat(VID_DIR "/frames.mjpeg", &mst) == 0);
        pf_mjpg_len = (size_t)mst.st_size;
        pf_nframes = nframes;
        pf_ents = ents;
        ESP_LOGI(TAG, "bundle: %u frames mjpeg %u bytes",
                 (unsigned)nframes, (unsigned)pf_mjpg_len);
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
    ESP_ERROR_CHECK(bsp_display_brightness_set(OSD_BRI_INIT)); /* 50%, not 100% */
    s_panel = panel;
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

    /* PSRAM budget, measured not assumed: audio (1.9MB for 10s, ~5.8MB
     * for 30s) is already allocated above, display FBs by the driver.
     * Remaining free must cover the 1.8MB pause backup + decoder input
     * + prefetch ring, with 1MB slack for driver/GDMA use. */
    {
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t need_fixed = OSD_FB_SZ + in_sz;
        size_t headroom = 1024 * 1024;
        int nslots = 0;
        if (free_psram > need_fixed + headroom)
            nslots = (int)((free_psram - need_fixed - headroom) /
                           PF_SLOT_BYTES);
        if (nslots > PF_SLOTS_MAX)
            nslots = PF_SLOTS_MAX;
        ESP_LOGI(TAG,
                 "psram budget: free=%u audio=%u backup=%u inbuf=%u "
                 "slots=%d x %u headroom=%u",
                 (unsigned)free_psram, (unsigned)audio_len,
                 (unsigned)OSD_FB_SZ, (unsigned)in_sz, nslots,
                 (unsigned)PF_SLOT_BYTES, (unsigned)headroom);
        if (nslots < PF_SLOTS_MIN) {
            ESP_LOGE(TAG,
                     "psram budget: %d prefetch slots < min %d, aborting",
                     nslots, PF_SLOTS_MIN);
            abort();
        }
        pf_slots = malloc((size_t)nslots * sizeof(*pf_slots));
        assert(pf_slots);
        pf_nslots = nslots;
        for (int i = 0; i < nslots; i++) {
            /* DMA-capable PSRAM: satisfies sdmmc alignment, takes the
             * direct-DMA path instead of the 512B bounce buffer. */
            esp_dma_mem_info_t di;
            sdmmc_host_get_dma_info(0, &di);
            di.extra_heap_caps |= MALLOC_CAP_SPIRAM;
            size_t actual = 0;
            if (esp_dma_capable_malloc(PF_SLOT_BYTES, &di,
                                       (void **)&pf_slots[i].data,
                                       &actual) != ESP_OK ||
                pf_slots[i].data == NULL) {
                ESP_LOGE(TAG, "psram budget: slot %d alloc failed, aborting",
                         i);
                abort();
            }
            pf_slots[i].state = PF_EMPTY;
        }
        s_pause_backup = heap_caps_malloc(OSD_FB_SZ, MALLOC_CAP_SPIRAM);
        if (s_pause_backup == NULL) {
            ESP_LOGE(TAG, "psram budget: pause backup alloc failed, aborting");
            abort();
        }
        pf_mutex = xSemaphoreCreateMutex();
        assert(pf_mutex);
    }

    /* audio path */
    spk = bsp_audio_codec_speaker_init();
    assert(spk);
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = A_RATE, .channel = A_CH, .bits_per_sample = 16};
    ESP_ERROR_CHECK(esp_codec_dev_open(spk, &fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(spk, 0)); /* muted for now */
    /* audio task starts after pre-roll (below), so the A/V clock never
     * outruns the prefetch ring at startup. */

    /* touch for pause/OSD; playback continues without it if init fails */
    if (bsp_touch_new(NULL, &s_tp) != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed, running without OSD");
        s_tp = NULL;
    }
    ina_diag_init(); /* INA226 1Hz raw sampler; silent when 0x41 absent */


    /* (Re)subscribe the main task early: pre-roll below feeds the WDT,
     * and transfer mode deinitializes the watchdog. Startup does not
     * guarantee subscription. */
    {
        esp_err_t wr = esp_task_wdt_add(NULL);
        if (wr != ESP_OK && wr != ESP_ERR_INVALID_STATE)
            ESP_ERROR_CHECK(wr);
    }
    /* prefetch reader owns its own FILE handle; main loop consumes slots */
    pf_want = 0;
    pf_epoch = 1;
    pf_primed = false;
    /* 16KB: FatFs f_read + SDMMC call chain is deep; 4KB overflowed
     * (mystery WDTs) as instrumentation locals grew. */
    xTaskCreatePinnedToCore(prefetch_task, "prefetch", 16384, NULL, 5, NULL,
                            0);
    /* Pre-roll: hold the audio (video) clock until PF_AHEAD frames are
     * buffered, so playback never races an empty ring at startup. */
    {
        int64_t pdl = esp_timer_get_time() + (int64_t)15 * 1000 * 1000;
        while (!pf_primed && esp_timer_get_time() < pdl) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        ESP_LOGI(TAG, "prefetch primed=%d", (int)pf_primed);
    }
    /* Start the A/V clock only now: video target derives from it. */
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 12, NULL, 1);

    /* video loop driven by audio clock; stages profiled over PROF_WIN */
    uint32_t shown = 0xFFFFFFFFu;
    void *fbs[2] = {fb0, fb1};
    int64_t t0 = esp_timer_get_time();
    uint32_t frames_shown = 0, frames_skip = 0;
    prof_hist_t h_dec = {0}, h_sd = {0}, h_draw = {0}, h_wait = {0},
                h_aud = {0};
    uint32_t low_water = UINT32_MAX;
    int cur = 0;
    while (1) {
        esp_task_wdt_reset();
        /* non-blocking touch step first; while paused it also owns the delay */
        ui_touch_step(s_pause_backup != NULL && frames_shown > 0);
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
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
        if (target < shown) {
            shown = 0xFFFFFFFFu; /* wrapped */
            pf_epoch++; /* reader drops its window, restarts at pf_want */
        }
        if (target > shown + 1 && shown != 0xFFFFFFFFu)
            frames_skip += (target - shown - 1);
        /* consume the prefetched extent for target (audio clock keeps
         * running while we wait, so a miss stays in A/V sync by skipping).
         * Lookup, copy, and release of fully-consumed extents all happen
         * under the mutex so the reader never sees a torn state. */
        pf_want = target;
        int64_t w0 = esp_timer_get_time();
        uint32_t got = 0, hit_sd = 0;
        int ready_now = 0;
        while (1) {
            xSemaphoreTake(pf_mutex, portMAX_DELAY);
            pf_slot_t *hit = pf_find(target);
            if (hit != NULL) {
                uint32_t base = hit->data_off;
                got = pf_ents[target].len;
                assert(pf_ents[target].off >= base &&
                       pf_ents[target].off + got <=
                           base + hit->total +
                               (hit->file_off - hit->data_off));
                memcpy(in_buf, hit->data + (pf_ents[target].off - base),
                       got);
                hit_sd = hit->sd_us / (hit->count ? hit->count : 1);
                /* release extents fully behind target (hit included when
                 * target was its last frame) */
                for (int i = 0; i < pf_nslots; i++) {
                    if (pf_slots[i].state != PF_READY)
                        continue;
                    if (pf_slots[i].first == 0)
                        continue; /* pinned loop-start extent */
                    uint32_t last =
                        pf_slots[i].first + pf_slots[i].count - 1;
                    uint32_t behind =
                        (target + pf_nframes - last) % pf_nframes;
                    if (behind > 0 && behind < pf_nframes / 2)
                        pf_slots[i].state = PF_EMPTY;
                }
                if (hit->first != 0 && hit->first + hit->count - 1 == target)
                    hit->state = PF_EMPTY; /* pinned f0 extent never freed */
            }
            ready_now = 0;
            for (int i = 0; i < pf_nslots; i++)
                if (pf_slots[i].state == PF_READY)
                    ready_now++;
            xSemaphoreGive(pf_mutex);
            if (got > 0 || s_paused)
                break;
            if (esp_timer_get_time() - w0 > (int64_t)PF_WAIT_MS * 1000) {
                /* definitive miss debug: dump ring state once per miss */
                xSemaphoreTake(pf_mutex, portMAX_DELAY);
                ESP_LOGW(TAG, "miss f%u want=%lu epoch=%lu slots:",
                         (unsigned)target, (unsigned long)pf_want,
                         (unsigned long)pf_epoch);
                for (int i = 0; i < pf_nslots; i++)
                    ESP_LOGW(TAG, "  slot%d st=%d first=%lu cnt=%lu off=%lu",
                             i, (int)pf_slots[i].state,
                             (unsigned long)pf_slots[i].first,
                             (unsigned long)pf_slots[i].count,
                             (unsigned long)pf_slots[i].file_off);
                xSemaphoreGive(pf_mutex);
                break;
            }
            vTaskDelay(1);
        }
        uint32_t wait_us = (uint32_t)(esp_timer_get_time() - w0);
        if (got == 0) {
            if (!s_paused) {
                /* underrun: don't stall, don't silently pass — count it
                 * and re-evaluate target (the skip shows up above too). */
                pf_underrun++;
                if ((pf_underrun & 15) == 1)
                    ESP_LOGW(TAG, "prefetch underrun f%u total=%lu",
                             (unsigned)target, (unsigned long)pf_underrun);
            }
            continue;
        }
        if (ready_now < low_water)
            low_water = ready_now;
        /* frame bytes are already in in_buf (copied under the mutex) */
        cur ^= 1;
        int64_t r0 = esp_timer_get_time();
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
        s_displayed_fb = fbs[cur];
        int64_t r3 = esp_timer_get_time();
        prof_push(&h_wait, wait_us + (uint32_t)(r1 - r0));
        prof_push(&h_sd, hit_sd);
        prof_push(&h_dec, (uint32_t)(r2 - r1));
        prof_push(&h_draw, (uint32_t)(r3 - r2));
        prof_push(&h_aud, a_write_us);
        shown = target;
        frames_shown++;
        if ((frames_shown & (PROF_WIN - 1)) == 0) {
            float el = (esp_timer_get_time() - t0) / 1e6f;
            uint32_t d_a, d_p, d_m, s_a, s_p, s_m, w_a, w_p, w_m, dw_a,
                dw_p, dw_m, a_a, a_p, a_m;
            prof_stats(&h_dec, &d_a, &d_p, &d_m);
            prof_stats(&h_sd, &s_a, &s_p, &s_m);
            prof_stats(&h_draw, &dw_a, &dw_p, &dw_m);
            prof_stats(&h_wait, &w_a, &w_p, &w_m);
            prof_stats(&h_aud, &a_a, &a_p, &a_m);
            ESP_LOGI(TAG,
                     "prof shown=%u fps=%.1f "
                     "dec %u/%u/%u sd %u/%u/%u draw %u/%u/%u "
                     "wait %u/%u/%u aud %u/%u/%u "
                     "low=%u skip=%u underrun=%lu",
                     (unsigned)frames_shown, frames_shown / el,
                     (unsigned)d_a, (unsigned)d_p, (unsigned)d_m,
                     (unsigned)s_a, (unsigned)s_p, (unsigned)s_m,
                     (unsigned)dw_a, (unsigned)dw_p, (unsigned)dw_m,
                     (unsigned)w_a, (unsigned)w_p, (unsigned)w_m,
                     (unsigned)a_a, (unsigned)a_p, (unsigned)a_m,
                     (unsigned)low_water, (unsigned)frames_skip,
                     (unsigned long)pf_underrun);
            /* Piggyback latest INA226 raw sample on its own line so the
             * existing prof line stays parser-compatible. */
            {
                ina_sample_t ina_latest;
                if (ina_diag_latest(&ina_latest))
                    ESP_LOGI(TAG, "ina t=%lld bus=%u shunt=%u cal=%u err=%s",
                             (long long)ina_latest.t_us, ina_latest.bus_raw,
                             ina_latest.shunt_raw, ina_latest.cal_raw,
                             esp_err_to_name(ina_latest.err));
            }
            /* Battery status about every 10s (every 5th prof line). */
            if ((frames_shown & 319) == 0) {
                int32_t mv = ina_bus_mv();
                int chg = power_charge_level();
                ESP_LOGI(TAG, "batt %ldmV chg_stat=%d", (long)mv, chg);
            }
            low_water = UINT32_MAX;
        }
    }
}
