#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t lan_server_init(void);
void lan_server_set_available(bool available);
bool lan_server_available(void);
esp_err_t lan_server_pause(void);
