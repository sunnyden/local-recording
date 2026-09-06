#pragma once
#include "esp_err.h"
#include <stdint.h>
typedef struct { int unused; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct { struct { uint8_t ssid[32], password[64]; } sta; } wifi_config_t;
typedef enum { WIFI_MODE_STA = 1 } wifi_mode_t;
typedef enum { WIFI_IF_STA = 0 } wifi_interface_t;
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_set_mode(wifi_mode_t);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_get_config(wifi_interface_t, wifi_config_t *);
