#pragma once
#include "FreeRTOS.h"
#include <stddef.h>
typedef struct host_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned, unsigned);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
void vQueueDelete(QueueHandle_t);
