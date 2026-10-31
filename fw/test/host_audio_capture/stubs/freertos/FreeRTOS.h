#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t configSTACK_DEPTH_TYPE;

#define pdPASS  1
#define pdFAIL  0
#define pdTRUE  1
#define pdFALSE 0

/* 1 ms tick on the host so timeouts are easy to check. */
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

typedef struct
{
    int locked;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(mux)      ((mux)->locked++)
#define portEXIT_CRITICAL(mux)       ((mux)->locked--)
