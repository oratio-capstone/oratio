#pragma once

#include <stdint.h>

#include "esp_err.h"

/* ESP32-C3: GPIO 0-21. */
typedef enum
{
    GPIO_NUM_NC = -1,
    GPIO_NUM_0 = 0,
    GPIO_NUM_MAX = 22
} gpio_num_t;

typedef enum
{
    GPIO_MODE_OUTPUT = 2
} gpio_mode_t;

#define GPIO_IS_VALID_GPIO(n)        ((int)(n) >= 0 && (int)(n) < (int)GPIO_NUM_MAX)
#define GPIO_IS_VALID_OUTPUT_GPIO(n) GPIO_IS_VALID_GPIO(n)

esp_err_t gpio_reset_pin(gpio_num_t gpio_num);
esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);
