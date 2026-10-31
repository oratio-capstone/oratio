#pragma once

/* Host double: logs are formatted (so format strings are type-checked) but not printed. */
void host_log(const char *tag, const char *format, ...) __attribute__((format(printf, 2, 3)));

#define ESP_LOGE(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) host_log(tag, __VA_ARGS__)
