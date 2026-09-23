#ifndef DHT11_H
#define DHT11_H

#include "esp_err.h"
#include "driver/gpio.h"

typedef struct {
    int temperature;
    int humidity;
} dht11_data_t;

esp_err_t dht11_init(gpio_num_t gpio);

esp_err_t dht11_read(gpio_num_t gpio, dht11_data_t *data);

#endif /* DHT11_H */
