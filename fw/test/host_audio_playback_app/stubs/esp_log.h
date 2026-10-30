#pragma once

/* Preserve argument evaluation so logging-only values are checked by the host compiler. */
static inline void ESP_LOGI(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

static inline void ESP_LOGW(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

static inline void ESP_LOGE(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
