#pragma once

#include "esp_err.h"

/* Initialize NVS before networking or other modules use persistent settings.
 * Erase and retry only when NVS reports no free pages or a version mismatch.
 */
esp_err_t storage_init(void);
