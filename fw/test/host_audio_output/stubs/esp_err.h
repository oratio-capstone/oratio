#pragma once

#include <stdint.h>

typedef int32_t esp_err_t;
enum
{
    ESP_OK = 0,
    ESP_FAIL = -1,
    ESP_ERR_NO_MEM = 0x101,
    ESP_ERR_INVALID_ARG,
    ESP_ERR_INVALID_STATE,
    ESP_ERR_TIMEOUT
};

static inline const char *esp_err_to_name(esp_err_t result)
{
    (void)result;
    return "host stub";
}
