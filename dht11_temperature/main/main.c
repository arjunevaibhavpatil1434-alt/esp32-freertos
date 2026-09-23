#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "dht11.h"

#define DHT11_GPIO GPIO_NUM_4

static const char *TAG = "MAIN";

static void dht11_task(void *pvParameters)
{
    dht11_data_t data;

    while (1) {
        esp_err_t ret = dht11_read(DHT11_GPIO, &data);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "Temperature: %d C | Humidity: %d %%",
                     data.temperature,
                     data.humidity);
        } else {
            ESP_LOGE(TAG,
                     "DHT11 read failed: %s",
                     esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "DHT11 Temperature Sensor Starting...");

    esp_err_t ret = dht11_init(DHT11_GPIO);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "DHT11 initialization failed: %s",
                 esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "DHT11 initialized on GPIO %d", DHT11_GPIO);

    /*
     * Give the DHT11 time to power up and stabilize.
     */
    ESP_LOGI(TAG, "Waiting 2 seconds for DHT11 startup...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    xTaskCreate(
        dht11_task,
        "dht11_task",
        4096,
        NULL,
        5,
        NULL
    );
}
