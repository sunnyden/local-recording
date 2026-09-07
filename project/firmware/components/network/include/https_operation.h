#pragma once
#include "recorder_network.h"

typedef struct {
    unsigned timeout_ms;
    size_t response_limit;
    bool (*cancelled)(void);
} https_operation_t;
esp_err_t https_json_operation(const char *url, esp_http_client_method_t method,
    const char *bearer, const char *content_type, const char *body,
    int *status, cJSON **response, unsigned *retry_after, const https_operation_t *operation);
bool recorder_proxy_endpoint(const char *configured_wss, const char *path,
                             char *out, size_t capacity);
