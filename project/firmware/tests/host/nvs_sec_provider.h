#pragma once
#include "nvs_flash.h"
typedef int hmac_key_id_t;
#define NVS_SEC_SCHEME_HMAC 1
typedef struct { hmac_key_id_t hmac_key_id; } nvs_sec_config_hmac_t;
esp_err_t nvs_sec_provider_register_hmac(const nvs_sec_config_hmac_t *, nvs_sec_scheme_t **);
esp_err_t nvs_sec_provider_deregister(nvs_sec_scheme_t *);
esp_err_t esp_hmac_calculate(hmac_key_id_t, const void *, size_t, uint8_t *);
