#include "dht11.h"

#include "esp_log.h"
#include "esp_rom_sys.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DHT11_TIMEOUT_US 1000

static const char *TAG = "DHT11";
static gpio_num_t dht11_gpio;

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

    gpio_set_level(dht11_gpio, 1);
    esp_rom_delay_us(30);

    gpio_set_direction(dht11_gpio, GPIO_MODE_INPUT);

    if (wait_for_level(0) < 0) {
        ESP_LOGE(TAG, "No response LOW");
        return ESP_FAIL;
    }

    if (wait_for_level(1) < 0) {
        ESP_LOGE(TAG, "No response HIGH");
        return ESP_FAIL;
    }

    if (wait_for_level(0) < 0) {
        ESP_LOGE(TAG, "No data start");
        return ESP_FAIL;
    }

    for (int i = 0; i < 40; i++) {

        if (wait_for_level(1) < 0) {
            ESP_LOGE(TAG, "Bit %d HIGH timeout", i);
            return ESP_FAIL;
        }

        esp_rom_delay_us(40);

        if (gpio_get_level(dht11_gpio)) {
            bits[i / 8] |= (1 << (7 - (i % 8)));
        }

        if (wait_for_level(0) < 0) {
            ESP_LOGE(TAG, "Bit %d LOW timeout", i);
            return ESP_FAIL;
        }
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

    ESP_LOGI(TAG,
             "Humidity=%d Temperature=%d",
             data->humidity,
             data->temperature);

    return ESP_OK;
}
