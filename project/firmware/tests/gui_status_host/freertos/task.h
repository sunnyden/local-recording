#pragma once
#include "freertos/FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(TaskFunction_t, const char *, unsigned, void *, unsigned, void *);
void vTaskDelay(TickType_t);
void vTaskDelete(void *);
void xTaskNotifyGive(TaskHandle_t);
unsigned ulTaskNotifyTake(BaseType_t, TickType_t);
