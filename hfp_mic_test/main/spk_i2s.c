#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "spk_i2s.h"

static const char *TAG = "SPK";

#define SPK_I2S_PORT      I2S_NUM_0
#define SPK_BCLK_GPIO     GPIO_NUM_27
#define SPK_LRC_GPIO      GPIO_NUM_14
#define SPK_DIN_GPIO      GPIO_NUM_25

static i2s_chan_handle_t s_tx;
static uint32_t s_rate;
static SemaphoreHandle_t s_lock;   /* rate changes wait out an in-flight write */

static bool spk_create(uint32_t rate)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   /* underruns play zeros, not the last buffer */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
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
    if (i2s_new_channel(&chan_cfg, &s_tx, NULL) != ESP_OK ||
        i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK ||
        i2s_channel_enable(s_tx) != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed");
        s_tx = NULL;
        return false;
    }
    ESP_LOGI(TAG, "MAX98357A I2S ready (BCLK=%d LRC=%d DIN=%d)",
             SPK_BCLK_GPIO, SPK_LRC_GPIO, SPK_DIN_GPIO);
    return true;
}

bool spk_i2s_set_rate(uint32_t rate)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = true;
    if (!s_tx) {
        ok = spk_create(rate);
    } else if (rate != s_rate) {
        i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
        ok = i2s_channel_disable(s_tx) == ESP_OK &&
             i2s_channel_reconfig_std_clock(s_tx, &clk) == ESP_OK &&
             i2s_channel_enable(s_tx) == ESP_OK;
    }
    if (ok && rate != s_rate) {
        ESP_LOGI(TAG, "rate %lu Hz", (unsigned long)rate);
        s_rate = rate;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void spk_i2s_write(const int16_t *pcm, size_t samples, TickType_t timeout)
{
    if (!s_tx) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t written;
    i2s_channel_write(s_tx, pcm, samples * sizeof(int16_t), &written, timeout);
    xSemaphoreGive(s_lock);
}
