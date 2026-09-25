/* Call and music status on the 128x64 SSD1306: Bluetooth link, call state,
 * caller or track, and DHT11 temperature / humidity. Same panel setup as
 * pipeline_check, with a full printable-ASCII 5x7 font plus a degree sign.
 * Text too wide for the screen scrolls. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "oled_status.h"

static const char *TAG = "OLED";

#define OLED_SDA_GPIO   21
#define OLED_SCL_GPIO   22
#define OLED_ADDR       0x3C
#define OLED_W          128
#define OLED_H          64
#define REDRAW_MS       150
#define SCROLL_PX       3       /* per redraw */
#define SCROLL_GAP      24      /* px between the end of the text and its repeat */
#define TEXT_X          2

volatile bool g_stat_slc, g_stat_in_call, g_stat_ringing, g_stat_outgoing, g_stat_audio, g_stat_wideband;
volatile uint32_t g_stat_music_tick;
volatile int g_stat_temp_c = -1, g_stat_humidity = -1;

static char s_caller_name[40], s_caller_num[24], s_title[64], s_artist[48];
static portMUX_TYPE s_text_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_lcd_panel_handle_t s_panel;
static uint8_t s_fb[OLED_W * OLED_H / 8];

/* Classic 5x7 font, columns LSB-top, for ' ' (0x20) through '~' (0x7E),
 * then a degree sign in the unused 0x7F slot. */
#define DEGREE "\x7F"
static const uint8_t s_font[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, {0x08,0x2A,0x1C,0x2A,0x08}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14}, {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78}, {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78}, {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C}, {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00}, {0x08,0x04,0x08,0x10,0x08},
    {0x00,0x06,0x09,0x09,0x06},
};

static void fb_pixel(int x, int y)
{
    if (x >= 0 && x < OLED_W && y >= 0 && y < OLED_H) {
        s_fb[(y / 8) * OLED_W + x] |= 1 << (y % 8);
    }
}

/* Draws text on one of the 8 text rows (8 px each) from pixel column x,
 * which may be off-screen; columns outside the screen are clipped. */
static void fb_text(int row, int x, const char *s)
{
    for (; *s && x < OLED_W; s++, x += 6) {
        unsigned char c = (unsigned char)*s;
        const uint8_t *g = s_font[(c >= 0x20 && c <= 0x7F ? c : '?') - 0x20];
        for (int col = 0; col < 5; col++) {
            if (x + col >= 0 && x + col < OLED_W) {
                s_fb[row * OLED_W + x + col] = g[col];
            }
        }
    }
}

/* Text that fits is drawn as is; wider text scrolls in a loop. */
static void fb_text_scroll(int row, const char *s, int tick)
{
    int w = (int)strlen(s) * 6;
    if (w <= OLED_W - TEXT_X) {
        fb_text(row, TEXT_X, s);
        return;
    }
    int period = w + SCROLL_GAP;
    int off = (tick * SCROLL_PX) % period;
    fb_text(row, TEXT_X - off, s);
    fb_text(row, TEXT_X - off + period, s);
}

static void fb_hline(int y)
{
    for (int x = 0; x < OLED_W; x++) {
        fb_pixel(x, y);
    }
}

/* Copies text for display: each multi-byte UTF-8 character becomes one '?',
 * since the font is ASCII only. */
static void set_text(char *dst, size_t size, const char *src)
{
    char tmp[64];
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p && o < sizeof(tmp) - 1; p++) {
        if (*p >= 0x20 && *p <= 0x7E) {
            tmp[o++] = (char)*p;
        } else if (*p >= 0xC0) {
            tmp[o++] = '?';     /* lead byte; continuation bytes (0x80-0xBF) are skipped */
        }
    }
    tmp[o] = '\0';
    portENTER_CRITICAL(&s_text_lock);
    strlcpy(dst, tmp, size);
    portEXIT_CRITICAL(&s_text_lock);
}

void oled_status_set_caller(const char *name, const char *number)
{
    set_text(s_caller_name, sizeof(s_caller_name), name);
    set_text(s_caller_num, sizeof(s_caller_num), number);
}

void oled_status_set_track_title(const char *title)
{
    set_text(s_title, sizeof(s_title), title);
}

void oled_status_set_track_artist(const char *artist)
{
    set_text(s_artist, sizeof(s_artist), artist);
}

static i2c_master_bus_handle_t s_bus;

static bool oled_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = 0,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    if (!s_bus && i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        return false;
    }
    if (i2c_master_probe(s_bus, OLED_ADDR, 100) != ESP_OK) {
        return false;
    }

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = OLED_ADDR,
        .scl_speed_hz = 400 * 1000,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    esp_lcd_panel_dev_config_t panel_cfg = { .bits_per_pixel = 1, .reset_gpio_num = -1 };
    esp_lcd_panel_ssd1306_config_t ssd_cfg = { .height = OLED_H };
    panel_cfg.vendor_config = &ssd_cfg;

    return esp_lcd_new_panel_io_i2c(s_bus, &io_cfg, &io) == ESP_OK &&
           esp_lcd_new_panel_ssd1306(io, &panel_cfg, &s_panel) == ESP_OK &&
           esp_lcd_panel_reset(s_panel) == ESP_OK &&
           esp_lcd_panel_init(s_panel) == ESP_OK &&
           esp_lcd_panel_disp_on_off(s_panel, true) == ESP_OK;
}

/* Layout (8 text rows):
 *   0  BT CONNECTED / BT WAITING
 *   1  INCOMING CALL / CALLING / IN CALL / MUSIC / NO CALL
 *   2  caller name, or track title (kept while paused; scrolls if long)
 *   3  caller number, or artist          (scrolls if long)
 *   5  (divider line)
 *   6  Temp  28 °C
 *   7  Humidity  67 % */
static void oled_task(void *arg)
{
    /* A panel that is slow to power up, or a loose wire, just means retrying. */
    for (int tries = 1; !oled_init(); tries++) {
        if (tries == 1 || tries % 30 == 0) {
            ESP_LOGW(TAG, "no reply at 0x%02X - check SDA/SCL/VCC/GND; retrying", OLED_ADDR);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "status display on at 0x%02X", OLED_ADDR);

    char a[64], b[48], line[24];
    for (int tick = 0;; tick++) {
        bool call = g_stat_ringing || g_stat_outgoing || g_stat_in_call || g_stat_audio;
        /* music is "playing" while packets keep arriving; no start/stop
         * event to miss, and pauses show up within a second */
        uint32_t since = xTaskGetTickCount() - g_stat_music_tick;
        bool music = g_stat_music_tick && since < pdMS_TO_TICKS(1000);

        portENTER_CRITICAL(&s_text_lock);
        if (call && s_caller_name[0]) {
            strlcpy(a, s_caller_name, sizeof(a));
            strlcpy(b, s_caller_num, sizeof(b));
        } else if (call) {
            strlcpy(a, s_caller_num, sizeof(a));
            b[0] = '\0';
        } else {
            /* last track stays up while paused, until AVRCP disconnects */
            strlcpy(a, s_title, sizeof(a));
            strlcpy(b, s_artist, sizeof(b));
        }
        portEXIT_CRITICAL(&s_text_lock);

        memset(s_fb, 0, sizeof(s_fb));
        fb_text(0, TEXT_X, g_stat_slc ? "BT CONNECTED" : "BT WAITING");
        fb_text(1, TEXT_X, g_stat_ringing ? "INCOMING CALL"
                         : g_stat_outgoing ? "CALLING"
                         : g_stat_audio   ? (g_stat_wideband ? "IN CALL (HD)" : "IN CALL")
                         : g_stat_in_call ? "IN CALL"
                         : music          ? "MUSIC"
                         : "NO CALL");
        fb_text_scroll(2, a, tick);
        fb_text_scroll(3, b, tick);
        fb_hline(5 * 8 + 3);
        int t = g_stat_temp_c, h = g_stat_humidity;
        if (t < 0) {
            fb_text(6, TEXT_X, "Temp      --");
            fb_text(7, TEXT_X, "Humidity  --");
        } else {
            snprintf(line, sizeof(line), "Temp      %d " DEGREE "C", t);
            fb_text(6, TEXT_X, line);
            snprintf(line, sizeof(line), "Humidity  %d %%", h);
            fb_text(7, TEXT_X, line);
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_W, OLED_H, s_fb);
        vTaskDelay(pdMS_TO_TICKS(REDRAW_MS));
    }
}

void oled_status_start(void)
{
    xTaskCreate(oled_task, "oled", 3072, NULL, 2, NULL);
}
