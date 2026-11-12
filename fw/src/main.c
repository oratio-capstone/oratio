#ifndef APP_TEST_BUILD
#include "storage.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

#define BLINK_GPIO GPIO_NUM_8

static const char *TAG = "BLINK_APP";

void app_main(void)
{
    /* Application startup owns persistent storage; initialize it before networking. */
    const esp_err_t storage_result = storage_init();
    if (storage_result != ESP_OK)
    {
        ESP_LOGE(TAG, "Storage initialization failed: %s", esp_err_to_name(storage_result));
        return;
    }

    ESP_LOGI(TAG, "Configuring LED GPIO...");

    // 1. Reset pin to default state and set direction as output
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    bool state = false;

    // 2. Loop forever
    while (true)
    {
        state = !state;
        gpio_set_level(BLINK_GPIO, state);

        ESP_LOGI(TAG, "LED State: %s", state ? "ON" : "OFF");

        // 3. Delay for 1000 milliseconds using FreeRTOS ticks
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif