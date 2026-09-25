/* Pipeline check: runs the DHT11, OLED, INMP441 mic and GPIO25 speaker all
 * at once, using the same pins and drivers as the real projects, and shows
 * each part's live state on the OLED and on serial:
 *
 *   DHT11 -> OLED line 1    temperature / humidity, good and failed reads
 *   mic   -> OLED line 2-3  live level bar, flagged if all-zero or stuck
 *   speaker                 1 s of 1 kHz, 1 s off; OLED says which
 *   speaker -> mic          1 kHz level the mic hears, tone on vs off
 *
 * The mic uses I2S1 and the MAX98357A amp uses I2S0, exactly as hfp_mic_test
 * does, so a pass here means the call audio hardware works too. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "dht11.h"

static const char *TAG = "PIPE";

/* ---- Pin map (see hw_verify/README.md) ---- */
#define DHT11_GPIO      GPIO_NUM_4
#define OLED_SDA_GPIO   21
#define OLED_SCL_GPIO   22
#define OLED_ADDR       0x3C
#define MIC_WS_GPIO     GPIO_NUM_33
#define MIC_SCK_GPIO    GPIO_NUM_26
#define MIC_SD_GPIO     GPIO_NUM_32
#define MIC_I2S_PORT    I2S_NUM_1   /* the DAC's DMA always takes I2S0 */

#define SPK_I2S_PORT    I2S_NUM_0
#define SPK_BCLK_GPIO   GPIO_NUM_27
#define SPK_LRC_GPIO    GPIO_NUM_14
#define SPK_DIN_GPIO    GPIO_NUM_25

#define SAMPLE_RATE     16000
#define SPK_CHUNK       256
#define SPK_TONE_HZ     1000.0f
#define SPK_AMPLITUDE   8000        /* of 32767; the MAX98357A adds 9 dB */

#define OLED_W          128
#define OLED_H          64

/* ---- Shared state, written by the part tasks, read by the report task ---- */
static volatile int s_temp = -1, s_hum = -1, s_dht_ok, s_dht_fail;
static volatile int s_mic_peak, s_mic_zero_blocks, s_mic_stuck_blocks, s_mic_blocks;
static volatile int32_t s_mic_first_raw;
static volatile float s_mic_tone;   /* max 1 kHz level per block since last report */
static volatile bool s_spk_on, s_spk_ready, s_mic_ready;

/* ---------------- OLED: tiny 5x7 font over a page-format framebuffer ------ */

static esp_lcd_panel_handle_t s_panel;
static uint8_t s_fb[OLED_W * OLED_H / 8];
static int s_oled_errors;

typedef struct { char c; uint8_t col[5]; } glyph_t;
static const glyph_t s_font[] = {
    {'0',{0x3E,0x51,0x49,0x45,0x3E}}, {'1',{0x00,0x42,0x7F,0x40,0x00}},
    {'2',{0x42,0x61,0x51,0x49,0x46}}, {'3',{0x21,0x41,0x45,0x4B,0x31}},
    {'4',{0x18,0x14,0x12,0x7F,0x10}}, {'5',{0x27,0x45,0x45,0x45,0x39}},
    {'6',{0x3C,0x4A,0x49,0x49,0x30}}, {'7',{0x01,0x71,0x09,0x05,0x03}},
    {'8',{0x36,0x49,0x49,0x49,0x36}}, {'9',{0x06,0x49,0x49,0x29,0x1E}},
    {'A',{0x7E,0x11,0x11,0x11,0x7E}}, {'B',{0x7F,0x49,0x49,0x49,0x36}},
    {'C',{0x3E,0x41,0x41,0x41,0x22}}, {'D',{0x7F,0x41,0x41,0x22,0x1C}},
    {'E',{0x7F,0x49,0x49,0x49,0x41}}, {'F',{0x7F,0x09,0x09,0x09,0x01}},
    {'G',{0x3E,0x41,0x49,0x49,0x7A}}, {'H',{0x7F,0x08,0x08,0x08,0x7F}},
    {'I',{0x00,0x41,0x7F,0x41,0x00}}, {'K',{0x7F,0x08,0x14,0x22,0x41}},
    {'L',{0x7F,0x40,0x40,0x40,0x40}}, {'M',{0x7F,0x02,0x0C,0x02,0x7F}},
    {'N',{0x7F,0x04,0x08,0x10,0x7F}}, {'O',{0x3E,0x41,0x41,0x41,0x3E}},
    {'P',{0x7F,0x09,0x09,0x09,0x06}}, {'R',{0x7F,0x09,0x19,0x29,0x46}},
    {'S',{0x46,0x49,0x49,0x49,0x31}}, {'T',{0x01,0x01,0x7F,0x01,0x01}},
    {'U',{0x3F,0x40,0x40,0x40,0x3F}}, {'W',{0x3F,0x40,0x38,0x40,0x3F}},
    {'Z',{0x61,0x51,0x49,0x45,0x43}}, {'%',{0x23,0x13,0x08,0x64,0x62}},
    {':',{0x00,0x36,0x36,0x00,0x00}}, {'-',{0x08,0x08,0x08,0x08,0x08}},
    {'/',{0x20,0x10,0x08,0x04,0x02}}, {'!',{0x00,0x00,0x5F,0x00,0x00}},
};

static void fb_pixel(int x, int y)
{
    if (x >= 0 && x < OLED_W && y >= 0 && y < OLED_H) {
        s_fb[(y / 8) * OLED_W + x] |= 1 << (y % 8);
    }
}

/* Draws text on one of the 8 text rows (8 px each); unknown chars are blank. */
static void fb_text(int row, int x, const char *s)
{
    for (; *s && x <= OLED_W - 6; s++, x += 6) {
        for (size_t g = 0; g < sizeof(s_font) / sizeof(s_font[0]); g++) {
            if (s_font[g].c == *s) {
                memcpy(&s_fb[row * OLED_W + x], s_font[g].col, 5);
                break;
            }
        }
    }
}

static void fb_rect(int x0, int y0, int x1, int y1, bool fill)
{
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (fill || y == y0 || y == y1 || x == x0 || x == x1) {
                fb_pixel(x, y);
            }
        }
    }
}

static bool oled_init(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = 0,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        return false;
    }
    /* The panel sometimes misses the first probe right after power-on. */
    int tries = 0;
    while (i2c_master_probe(bus, OLED_ADDR, 100) != ESP_OK) {
        if (++tries == 5) {
            ESP_LOGE(TAG, "[OLED] no reply at 0x%02X - check SDA/SCL/VCC/GND", OLED_ADDR);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
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

    return esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) == ESP_OK &&
           esp_lcd_new_panel_ssd1306(io, &panel_cfg, &s_panel) == ESP_OK &&
           esp_lcd_panel_reset(s_panel) == ESP_OK &&
           esp_lcd_panel_init(s_panel) == ESP_OK &&
           esp_lcd_panel_disp_on_off(s_panel, true) == ESP_OK;
}

static void oled_flush(void)
{
    if (s_panel && esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_W, OLED_H, s_fb) != ESP_OK) {
        s_oled_errors++;
    }
}

/* ---------------- DHT11 ---------------- */

static void dht_task(void *arg)
{
    dht11_init(DHT11_GPIO);
    while (1) {
        dht11_data_t d;
        if (dht11_read(DHT11_GPIO, &d) == ESP_OK) {
            s_temp = d.temperature;
            s_hum = d.humidity;
            s_dht_ok++;
        } else {
            s_dht_fail++;
        }
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}

/* ---------------- Mic (same setup as hfp_mic_test) ---------------- */

static i2s_chan_handle_t mic_open(i2s_std_slot_mask_t slot)
{
    i2s_chan_handle_t rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_GPIO,
            .ws = MIC_WS_GPIO,
            .din = MIC_SD_GPIO,
            .dout = I2S_GPIO_UNUSED,
        },
    };
    std_cfg.slot_cfg.slot_mask = slot;
    if (i2s_new_channel(&chan_cfg, NULL, &rx) != ESP_OK ||
        i2s_channel_init_std_mode(rx, &std_cfg) != ESP_OK ||
        i2s_channel_enable(rx) != ESP_OK) {
        return NULL;
    }
    return rx;
}

static void mic_close(i2s_chan_handle_t rx)
{
    i2s_channel_disable(rx);
    i2s_del_channel(rx);
}

/* Reads ~0.5 s from one slot, optionally with a pull-up on SD, and logs
 * how many samples were all-zero, all-ones, or anything else. With the
 * pull-up, all-ones means nothing is driving SD at all. */
static void mic_probe(const char *name, i2s_std_slot_mask_t slot, bool pullup)
{
    i2s_chan_handle_t rx = mic_open(slot);
    if (!rx) {
        ESP_LOGE(TAG, "[MIC probe] %s: I2S init failed", name);
        return;
    }
    if (pullup) {
        gpio_pullup_en(MIC_SD_GPIO);
    } else {
        gpio_pullup_dis(MIC_SD_GPIO);
    }

    int32_t raw[256];
    int zeros = 0, ones = 0, other = 0, peak = 0;
    int32_t example = 0;
    vTaskDelay(pdMS_TO_TICKS(100));  /* INMP441 start-up time */
    for (int b = 0; b < 32; b++) {
        size_t got = 0;
        if (i2s_channel_read(rx, raw, sizeof(raw), &got, 100) != ESP_OK) {
            continue;
        }
        for (int i = 0; i < (int)(got / sizeof(int32_t)); i++) {
            if (raw[i] == 0) zeros++;
            else if (raw[i] == -1) ones++;
            else {
                other++;
                example = raw[i];
                int a = abs((int16_t)(raw[i] >> 16));
                if (a > peak) peak = a;
            }
        }
    }
    ESP_LOGI(TAG, "[MIC probe] %-22s zeros %5d  all-ones %5d  other %5d  peak %5d  e.g. 0x%08lx",
             name, zeros, ones, other, peak, (unsigned long)example);
    gpio_pullup_dis(MIC_SD_GPIO);
    mic_close(rx);
}

/* Goertzel magnitude of SPK_TONE_HZ in one block, normalised to the
 * amplitude of a sine at that frequency (0..32768). */
static float tone_level(const int32_t *raw, int n)
{
    float coeff = 2.0f * cosf(2.0f * (float)M_PI * SPK_TONE_HZ / SAMPLE_RATE);
    float s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        float s0 = (float)(int16_t)(raw[i] >> 16) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0f * sqrtf(power > 0 ? power : 0) / n;
}

static void mic_task(void *arg)
{
    mic_probe("left slot", I2S_STD_SLOT_LEFT, false);
    mic_probe("right slot", I2S_STD_SLOT_RIGHT, false);
    mic_probe("left slot + pull-up", I2S_STD_SLOT_LEFT, true);
    mic_probe("right slot + pull-up", I2S_STD_SLOT_RIGHT, true);

    i2s_chan_handle_t rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_GPIO,
            .ws = MIC_WS_GPIO,
            .din = MIC_SD_GPIO,
            .dout = I2S_GPIO_UNUSED,
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    if (i2s_new_channel(&chan_cfg, NULL, &rx) != ESP_OK ||
        i2s_channel_init_std_mode(rx, &std_cfg) != ESP_OK ||
        i2s_channel_enable(rx) != ESP_OK) {
        ESP_LOGE(TAG, "[MIC] I2S init failed");
        vTaskDelete(NULL);
    }
    s_mic_ready = true;

    int32_t raw[256];
    while (1) {
        size_t got = 0;
        if (i2s_channel_read(rx, raw, sizeof(raw), &got, 100) != ESP_OK || got == 0) {
            continue;
        }
        int n = got / sizeof(int32_t);
        int peak = 0;
        bool all_zero = true, all_same = true;
        for (int i = 0; i < n; i++) {
            int a = abs((int16_t)(raw[i] >> 16));
            if (a > peak) peak = a;
            if (raw[i] != 0) all_zero = false;
            if (raw[i] != raw[0]) all_same = false;
        }
        if (peak > s_mic_peak) s_mic_peak = peak;
        if (all_zero) s_mic_zero_blocks++;
        else if (all_same) s_mic_stuck_blocks++;
        float tone = tone_level(raw, n);
        if (tone > s_mic_tone) s_mic_tone = tone;
        s_mic_first_raw = raw[0];
        s_mic_blocks++;
    }
}

/* ---------------- Speaker (same I2S amp setup as hfp_mic_test) ---------------- */

static void spk_task(void *arg)
{
    i2s_chan_handle_t tx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_PORT, I2S_ROLE_MASTER);
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_GPIO,
            .ws = SPK_LRC_GPIO,
            .dout = SPK_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    if (i2s_new_channel(&chan_cfg, &tx, NULL) != ESP_OK ||
        i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK ||
        i2s_channel_enable(tx) != ESP_OK) {
        ESP_LOGE(TAG, "[SPK] I2S init failed");
        vTaskDelete(NULL);
    }
    s_spk_ready = true;

    /* Read each amp pin's pad level back while I2S drives it: a pin that
     * never changes is shorted or held by the wiring. */
    const struct { const char *name; int gpio; } pins[] = {
        {"BCLK", SPK_BCLK_GPIO}, {"LRC", SPK_LRC_GPIO}, {"DIN", SPK_DIN_GPIO},
    };
    {
        int16_t tone[SPK_CHUNK];
        for (int i = 0; i < SPK_CHUNK; i++) {
            tone[i] = (int16_t)(SPK_AMPLITUDE * sinf(2.0f * (float)M_PI * SPK_TONE_HZ * i / SAMPLE_RATE));
        }
        for (size_t p = 0; p < sizeof(pins) / sizeof(pins[0]); p++) {
            gpio_input_enable(pins[p].gpio);
            size_t written;
            /* ~250 ms of tone: flushes the zeros the DMA queue starts with */
            for (int k = 0; k < 16; k++) {
                i2s_channel_write(tx, tone, sizeof(tone), &written, portMAX_DELAY);
            }
            int highs = 0, edges = 0, last = gpio_get_level(pins[p].gpio);
            for (int i = 0; i < 20000; i++) {
                int v = gpio_get_level(pins[p].gpio);
                highs += v;
                edges += v != last;
                last = v;
            }
            ESP_LOGI(TAG, "[SPK pin] %-4s GPIO%-2d high %3d%%  edges %5d -> %s",
                     pins[p].name, pins[p].gpio, highs / 200, edges,
                     edges > 20 ? "toggling OK" : highs > 10000 ? "STUCK HIGH" : "STUCK LOW");
        }
    }

    int16_t buf[SPK_CHUNK];
    float phase = 0;
    int chunks_per_sec = SAMPLE_RATE / SPK_CHUNK;
    for (int c = 0;; c++) {
        s_spk_on = (c / chunks_per_sec) % 2 == 0;
        for (int i = 0; i < SPK_CHUNK; i++) {
            phase += 2.0f * (float)M_PI * SPK_TONE_HZ / SAMPLE_RATE;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
            buf[i] = s_spk_on ? (int16_t)(SPK_AMPLITUDE * sinf(phase)) : 0;
        }
        size_t written;
        i2s_channel_write(tx, buf, sizeof(buf), &written, portMAX_DELAY);
    }
}

/* ---------------- Report: OLED + serial, twice a second ---------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Pipeline check: DHT11 -> OLED, mic level -> OLED, speaker tone -> mic ===");
    bool oled_ok = oled_init();
    ESP_LOGI(TAG, "[OLED] %s", oled_ok ? "init OK at 0x3C - you should see a border and text"
                                       : "init FAILED");

    xTaskCreate(dht_task, "dht", 3072, NULL, 5, NULL);
    xTaskCreate(mic_task, "mic", 4096, NULL, 6, NULL);
    xTaskCreate(spk_task, "spk", 3072, NULL, 7, NULL);

    char line[24];
    for (int tick = 0;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(500));

        int peak = s_mic_peak, blocks = s_mic_blocks;
        int zero = s_mic_zero_blocks, stuck = s_mic_stuck_blocks;
        int32_t first_raw = s_mic_first_raw;
        float tone = s_mic_tone;
        bool spk_on = s_spk_on;   /* state for most of this half-second slot */
        s_mic_peak = s_mic_blocks = s_mic_zero_blocks = s_mic_stuck_blocks = 0;
        s_mic_tone = 0;

        const char *mic_state = !s_mic_ready ? "NO I2S"
                              : blocks == 0 ? "NO DATA"
                              : zero == blocks ? "ALL ZERO"
                              : stuck + zero == blocks ? "STUCK"
                              : peak < 30 ? "QUIET" : "SIGNAL";

        memset(s_fb, 0, sizeof(s_fb));
        fb_rect(0, 0, OLED_W - 1, OLED_H - 1, false);
        if (s_temp >= 0) {
            snprintf(line, sizeof(line), "T %dC  H %d%%", s_temp, s_hum);
        } else {
            snprintf(line, sizeof(line), "DHT ERR %d", s_dht_fail);
        }
        fb_text(1, 4, line);
        snprintf(line, sizeof(line), "MIC %s", mic_state);
        fb_text(3, 4, line);
        int bar = peak * (OLED_W - 10) / 4000;
        if (bar > OLED_W - 10) bar = OLED_W - 10;
        fb_rect(4, 34, OLED_W - 5, 41, false);
        if (bar > 0) fb_rect(4, 34, 4 + bar, 41, true);
        fb_text(6, 4, !s_spk_ready ? "SPK ERR" : s_spk_on ? "SPK TONE 1K" : "SPK OFF");
        oled_flush();

        /* The tone flips every 1 s = 2 report slots; skip the first slot
         * after each flip, which straddles the change. */
        static float on_sum, off_sum;
        static int on_n, off_n, last_on = -1;
        if (s_spk_ready && s_mic_ready && tick > 6) {
            if (spk_on == last_on) {
                if (spk_on) { on_sum += tone; on_n++; } else { off_sum += tone; off_n++; }
            }
        }
        last_on = spk_on;
        if (tick % 20 == 19 && on_n && off_n) {
            float on = on_sum / on_n, off = off_sum / off_n;
            ESP_LOGI(TAG, "[LOOPBACK] mic 1 kHz level: tone ON %.0f, tone OFF %.0f (x%.1f) -> %s",
                     on, off, off > 0 ? on / off : 0,
                     on > 4 * off && on > 50 ? "PASS, mic hears the speaker"
                                             : "FAIL, mic does not hear the speaker");
            on_sum = off_sum = 0;
            on_n = off_n = 0;
        }

        if (tick % 2 == 0) {
            ESP_LOGI(TAG, "DHT %s T=%dC H=%d%% (ok %d fail %d) | MIC %s peak %d 1k %.0f raw0 0x%08lx | "
                     "SPK %s | OLED %s errors %d",
                     s_temp >= 0 ? "OK" : "--", s_temp, s_hum, s_dht_ok, s_dht_fail,
                     mic_state, peak, tone, (unsigned long)first_raw,
                     !s_spk_ready ? "ERR" : s_spk_on ? "tone" : "off",
                     oled_ok ? "OK" : "FAIL", s_oled_errors);
        }
    }
}
