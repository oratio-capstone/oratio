#include "storage.h"

#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "STORAGE";

esp_err_t storage_init(void)
{
    esp_err_t result = nvs_flash_init();
    if (result != ESP_ERR_NVS_NO_FREE_PAGES && result != ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        return result;
    }

    ESP_LOGW(TAG, "Reinitializing NVS; stored settings will be erased");
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Failed to erase NVS");
    return nvs_flash_init();
}
