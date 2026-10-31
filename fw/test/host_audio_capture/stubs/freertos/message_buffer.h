#pragma once

#include "freertos/FreeRTOS.h"

typedef struct MessageBufferDef_t *MessageBufferHandle_t;

size_t xMessageBufferSend(MessageBufferHandle_t buffer, const void *data, size_t length, TickType_t ticks_to_wait);
