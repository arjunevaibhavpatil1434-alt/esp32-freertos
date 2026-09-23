#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

#include "dht11.h"

/* ---- Pin map under test — matches what was reported as wired on the breadboard ---- */
#define DHT11_GPIO      GPIO_NUM_4

#define OLED_SDA_GPIO   GPIO_NUM_21
#define OLED_SCL_GPIO   GPIO_NUM_22

#define MIC_WS_GPIO     GPIO_NUM_25  /* LRCL / word-select */
#define MIC_SCK_GPIO    GPIO_NUM_26  /* BCLK / bit-clock   */
#define MIC_SD_GPIO     GPIO_NUM_32  /* SD   / data out    */

#define MIC_SAMPLE_RATE 16000

static const char *TAG = "HW_VERIFY";

/* ---------------- DHT11 ---------------- */

static void dht11_task(void *pvParameters)
{
    dht11_data_t data;
    int ok = 0, fail = 0;

    for (int attempt = 0; attempt < 5; attempt++) {
        esp_err_t ret = dht11_read(DHT11_GPIO, &data);
        if (ret == ESP_OK) {
            ok++;
            ESP_LOGI(TAG, "[DHT11] OK  Temp=%dC Humidity=%d%%", data.temperature, data.humidity);
        } else {
            fail++;
            ESP_LOGE(TAG, "[DHT11] read failed: %s", esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(2500));
    }

    ESP_LOGI(TAG, "[DHT11] RESULT: %d ok / %d failed out of 5 on GPIO%d — %s",
             ok, fail, DHT11_GPIO,
             ok > 0 ? "wiring looks OK" : "check DATA pin, pull-up, and VCC/GND");

    vTaskDelete(NULL);
}

/* ---------------- OLED (I2C bus scan) ---------------- */

static void i2c_scan(void)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[OLED] Failed to init I2C bus on SDA=%d SCL=%d: %s",
                 OLED_SDA_GPIO, OLED_SCL_GPIO, esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "[OLED] Scanning I2C bus (SDA=%d, SCL=%d)...", OLED_SDA_GPIO, OLED_SCL_GPIO);

    int found = 0;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            found++;
            const char *hint = "";
            if (addr == 0x3C || addr == 0x3D) {
                hint = "  <-- typical SSD1306/SH1106 OLED address";
            }
            ESP_LOGI(TAG, "[OLED] Found device at 0x%02X%s", addr, hint);
        }
    }

    if (found == 0) {
        ESP_LOGE(TAG, "[OLED] No I2C devices found — check SDA/SCL wiring, VCC/GND, "
                       "and that SDA/SCL aren't swapped");
    } else {
        ESP_LOGI(TAG, "[OLED] RESULT: %d device(s) found on the bus", found);
    }

    i2c_del_master_bus(bus);
}

/* ---------------- Mic (I2S level meter) ---------------- */

static void mic_task(void *pvParameters)
{
    i2s_chan_handle_t rx_handle = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[MIC] i2s_new_channel failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
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

    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[MIC] init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "[MIC] I2S started WS=%d SCK=%d SD=%d — tap/blow on the mic and watch the peak level",
             MIC_WS_GPIO, MIC_SCK_GPIO, MIC_SD_GPIO);

    int32_t buf[256];
    size_t bytes_read = 0;
    int seen_nonzero = 0;

    for (int round = 0; round < 20; round++) {
        ret = i2s_channel_read(rx_handle, buf, sizeof(buf), &bytes_read, 1000);
        int32_t peak = 0;
        if (ret == ESP_OK && bytes_read > 0) {
            int n = bytes_read / sizeof(int32_t);
            for (int i = 0; i < n; i++) {
                int32_t sample = buf[i] >> 8; /* 24-bit sample, MSB-justified in 32-bit word */
                int32_t mag = sample < 0 ? -sample : sample;
                if (mag > peak) {
                    peak = mag;
                }
            }
            if (peak > 500) {
                seen_nonzero = 1;
            }
            ESP_LOGI(TAG, "[MIC] peak amplitude = %" PRId32, peak);
        } else {
            ESP_LOGE(TAG, "[MIC] read failed: %s", esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    ESP_LOGI(TAG, "[MIC] RESULT: %s",
             seen_nonzero ? "signal detected — wiring looks OK"
                           : "flat/near-zero the whole time — check WS/SCK/SD pins "
                             "(try swapping WS and SCK), L/R pin, and VDD/GND");

    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);
    vTaskDelete(NULL);
}

/* ---------------- Entry point ---------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Hardware verification: DHT11 / OLED (I2C) / I2S mic ===");

    esp_err_t ret = dht11_init(DHT11_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[DHT11] init failed: %s", esp_err_to_name(ret));
    } else {
        xTaskCreate(dht11_task, "dht11_task", 4096, NULL, 5, NULL);
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    i2c_scan();

    vTaskDelay(pdMS_TO_TICKS(500));
    xTaskCreate(mic_task, "mic_task", 4096, NULL, 5, NULL);
}
