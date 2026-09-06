#pragma once
#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
typedef enum { HTTP_METHOD_GET, HTTP_METHOD_POST, HTTP_METHOD_PUT, HTTP_METHOD_DELETE } esp_http_client_method_t;
bool credential_storage_allowed(void);
esp_err_t credential_storage_init(void);
esp_err_t credential_read(const char *, char *, size_t);
esp_err_t credential_write(const char *, const char *);
esp_err_t credential_erase_all(void);
bool recorder_network_ready(void);
esp_err_t recorder_network_init(void);
bool recorder_time_valid(void);
void secret_zero(void *, size_t);
char *form_encode(const char *);
esp_err_t https_json(const char *, esp_http_client_method_t, const char *,
                     const char *, const char *, int *, cJSON **);
