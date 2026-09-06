#pragma once
#include "esp_err.h"
#include <stdint.h>
typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
extern const char test_wifi_events[], test_ip_events[];
#define WIFI_EVENT test_wifi_events
#define IP_EVENT test_ip_events
#define ESP_EVENT_ANY_ID -1
enum { WIFI_EVENT_STA_START = 1, WIFI_EVENT_STA_DISCONNECTED, WIFI_EVENT_STA_STOP, IP_EVENT_STA_GOT_IP };
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_register(esp_event_base_t, int32_t, esp_event_handler_t, void *);
