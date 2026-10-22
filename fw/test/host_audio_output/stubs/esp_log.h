#pragma once

/* Keep the logging tag referenced without emitting firmware logs on the host. */
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
