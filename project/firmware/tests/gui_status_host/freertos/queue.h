#pragma once
#include "freertos/FreeRTOS.h"
#include <stddef.h>
typedef struct host_queue *QueueHandle_t;
typedef struct { unsigned unused; } StaticQueue_t;
QueueHandle_t xQueueCreate(unsigned, unsigned);
QueueHandle_t xQueueCreateStatic(unsigned, unsigned, unsigned char *, StaticQueue_t *);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
BaseType_t xQueueReset(QueueHandle_t);
void vQueueDelete(QueueHandle_t);
