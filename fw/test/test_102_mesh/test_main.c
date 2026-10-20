#include "network.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "TEST_102";
static const uint32_t STATUS_INTERVAL_MS = 5000;

void app_main(void)
{
    const esp_err_t result = mesh_network_init();

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Mesh startup failed: %s", esp_err_to_name(result));
        return;
    }

    ESP_LOGI(TAG, "Mesh startup requested; waiting for topology to form");

    while (true)
    {
        ESP_LOGI(TAG, "Task 102 test is running");
        mesh_network_log_status();
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
    }
}