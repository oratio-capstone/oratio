#pragma once

#include "esp_err.h"

esp_err_t mesh_network_init(void);
void mesh_network_log_status(void);
esp_err_t network_task_start(void);