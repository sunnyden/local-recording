#pragma once
#include "esp_err.h"
#include <stdint.h>
extern const void *network_prov_scheme_ble;
esp_err_t network_prov_scheme_ble_set_service_uuid(uint8_t *);
