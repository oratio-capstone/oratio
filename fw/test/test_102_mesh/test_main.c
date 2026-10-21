#include "network.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "TEST_102";
static const uint32_t STATUS_INTERVAL_MS = 5000;

/* Standalone mesh bring-up test, built for either the root or node role. */
void app_main(void)
{
    /* Initialize networking and request mesh startup using the shared module. */
    const esp_err_t result = mesh_network_init();

    /* Stop the test if initialization fails and print the readable error name. */
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Mesh startup failed: %s", esp_err_to_name(result));
        return;
    }

    /* Connections form in the background; successful initialization does not prove connectivity. */
    ESP_LOGI(TAG, "Mesh startup requested; waiting for topology to form");

    /* Continuously report status for manual inspection rather than automatic pass/fail assertions. */
    while (true)
    {
        ESP_LOGI(TAG, "Task 102 test is running");

        /* Report role, mesh level, SoftAP name, and parent details for non-root nodes. */
        mesh_network_log_status();

        /* Wait five seconds without busy-waiting, allowing other FreeRTOS tasks to run. */
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
    }
}