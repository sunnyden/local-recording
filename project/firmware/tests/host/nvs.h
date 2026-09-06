#pragma once
#include "esp_err.h"
#include <stddef.h>
typedef unsigned nvs_handle_t;
enum { NVS_READONLY, NVS_READWRITE };
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
esp_err_t nvs_erase_all(nvs_handle_t);
