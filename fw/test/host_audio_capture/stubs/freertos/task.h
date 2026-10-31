#pragma once

#include "freertos/FreeRTOS.h"

typedef struct tskTaskControlBlock *TaskHandle_t;
typedef void (*TaskFunction_t)(void *arg);

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, configSTACK_DEPTH_TYPE stack_depth, void *arg,
                       UBaseType_t priority, TaskHandle_t *created_task);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
