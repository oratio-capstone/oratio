#include "storage.h"
#include "network.h"
#include "esp_log.h"

static const char *TAG = "TEST_109";
void app_main(void)
{
    /* Application startup owns persistent storage; initialize it before networking. */
    const esp_err_t storage_result = storage_init();
    if (storage_result != ESP_OK)
    {
        ESP_LOGE(TAG, "Storage initialization failed: %s", esp_err_to_name(storage_result));
        return;
    }

    /* Initialize networking and request mesh startup using the shared module. */
    const esp_err_t init_result = mesh_network_init();

    /* Stop the test if initialization fails and print the readable error name. */
    if (init_result != ESP_OK)
    {
        ESP_LOGE(TAG, "Mesh startup failed: %s", esp_err_to_name(init_result));
        return;
    }

    const esp_err_t task_result = network_task_start(NULL, NULL);
    if (task_result != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start network task: %s", esp_err_to_name(task_result));
        return;
    }

    ESP_LOGI(TAG, "Network task started successfully");
}