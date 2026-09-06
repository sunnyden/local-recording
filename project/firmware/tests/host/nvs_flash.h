#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
typedef struct { uint8_t eky[32], tky[32]; } nvs_sec_cfg_t;
typedef esp_err_t (*nvs_key_callback_t)(const void *, nvs_sec_cfg_t *);
typedef struct {
    int scheme_id;
    void *scheme_data;
    nvs_key_callback_t nvs_flash_key_gen, nvs_flash_read_cfg;
} nvs_sec_scheme_t;
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_init_partition(const char *);
esp_err_t nvs_flash_secure_init(nvs_sec_cfg_t *);
esp_err_t nvs_flash_register_security_scheme(nvs_sec_scheme_t *);
void nvs_flash_deregister_security_scheme(void);
esp_err_t nvs_flash_read_security_cfg_v2(nvs_sec_scheme_t *, nvs_sec_cfg_t *);
