#pragma once
#include "FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreate(TaskFunction_t, const char *, unsigned, void *, unsigned, void *);
void vTaskDelay(TickType_t);
void vTaskDelete(void *);
