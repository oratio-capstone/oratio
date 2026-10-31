#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

#define BLINK_GPIO GPIO_NUM_8

static const char *TAG = "BLINK_APP";

void app_main(void)
{
    ESP_LOGI(TAG, "Configuring LED GPIO...");

    // 1. Reset pin to default state and set direction as output
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    uint8_t state = 0;

    // 2. Loop forever
    while (1)
    {
        state = !state;
        gpio_set_level(BLINK_GPIO, state);

        ESP_LOGI(TAG, "LED State: %s", state ? "ON" : "OFF");

        // 3. Delay for 1000 milliseconds using FreeRTOS ticks
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
