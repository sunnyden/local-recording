#pragma once
#include "esp_err.h"
typedef struct { const char *server; } esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(host_name) { .server = host_name }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *);
