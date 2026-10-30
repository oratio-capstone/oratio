#pragma once

#include "FreeRTOS.h"

typedef struct host_task *TaskHandle_t;
BaseType_t xTaskCreate(void (*entry)(void *), const char *name, uint32_t stack, void *arg,
                      UBaseType_t priority, TaskHandle_t *task);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void vTaskDelete(TaskHandle_t task);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
