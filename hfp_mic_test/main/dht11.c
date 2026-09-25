#include "dht11.h"

#include "esp_log.h"
#include "esp_rom_sys.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DHT11_TIMEOUT_US 1000

static const char *TAG = "DHT11";
static gpio_num_t dht11_gpio;
static portMUX_TYPE dht11_spinlock = portMUX_INITIALIZER_UNLOCKED;

static int wait_for_level(int level)
{
    int timeout = 0;

    while (gpio_get_level(dht11_gpio) != level) {
        esp_rom_delay_us(1);

        if (++timeout >= DHT11_TIMEOUT_US) {
            return -1;
        }
    }

    return timeout;
}

esp_err_t dht11_init(gpio_num_t gpio)
{
    dht11_gpio = gpio;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    return gpio_config(&io_conf);
}

esp_err_t dht11_read(gpio_num_t gpio, dht11_data_t *data)
{
    uint8_t bits[5] = {0};

    dht11_gpio = gpio;

    gpio_set_direction(dht11_gpio, GPIO_MODE_OUTPUT);

    gpio_set_level(dht11_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* The response + 40-bit frame is ~4ms of microsecond-level timing. With
     * the BT controller and LVGL running, an interrupt landing mid-frame makes
     * us miss a ~50us LOW pulse, so run this part with interrupts masked and
     * only log once we're out of the critical section. */
    const char *fail_stage = NULL;
    int fail_bit = -1;

    portENTER_CRITICAL(&dht11_spinlock);

    gpio_set_level(dht11_gpio, 1);
    esp_rom_delay_us(30);

    gpio_set_direction(dht11_gpio, GPIO_MODE_INPUT);

    if (wait_for_level(0) < 0) {
        fail_stage = "No response LOW";
    } else if (wait_for_level(1) < 0) {
        fail_stage = "No response HIGH";
    } else if (wait_for_level(0) < 0) {
        fail_stage = "No data start";
    } else {
        for (int i = 0; i < 40; i++) {
            if (wait_for_level(1) < 0) {
                fail_stage = "HIGH timeout";
                fail_bit = i;
                break;
            }

            esp_rom_delay_us(40);

            if (gpio_get_level(dht11_gpio)) {
                bits[i / 8] |= (1 << (7 - (i % 8)));
            }

            if (wait_for_level(0) < 0) {
                fail_stage = "LOW timeout";
                fail_bit = i;
                break;
            }
        }
    }

    portEXIT_CRITICAL(&dht11_spinlock);

    if (fail_stage) {
        if (fail_bit >= 0) {
            ESP_LOGE(TAG, "Bit %d %s", fail_bit, fail_stage);
        } else {
            ESP_LOGE(TAG, "%s", fail_stage);
        }
        return ESP_FAIL;
    }

    uint8_t checksum =
        bits[0] +
        bits[1] +
        bits[2] +
        bits[3];

    if (checksum != bits[4]) {
        ESP_LOGE(TAG,
                 "Checksum Error calc=%d recv=%d",
                 checksum,
                 bits[4]);
        return ESP_FAIL;
    }

    data->humidity = bits[0];
    data->temperature = bits[2];

    ESP_LOGD(TAG,
             "Humidity=%d Temperature=%d",
             data->humidity,
             data->temperature);

    return ESP_OK;
}
