#pragma once
#include "esp_err.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool credential_storage_allowed(void);
esp_err_t credential_storage_init(void);
esp_err_t credential_read(const char *key, char *value, size_t capacity);
esp_err_t credential_write(const char *key, const char *value);
esp_err_t credential_erase_all(void);
void secret_zero(void *memory, size_t length);
esp_err_t recorder_network_init(void);
bool recorder_network_ready(void);
bool recorder_time_valid(void);
esp_err_t https_json(const char *url, esp_http_client_method_t method,
    const char *bearer, const char *content_type, const char *body,
    int *status, cJSON **response);
char *form_encode(const char *value);
esp_err_t https_json_retry_info(const char *url, esp_http_client_method_t method,
    const char *bearer, const char *content_type, const char *body,
    int *status, cJSON **response, unsigned *retry_after);
