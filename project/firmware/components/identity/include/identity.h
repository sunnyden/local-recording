#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
typedef enum { AUTH_GRAPH, AUTH_PROXY } auth_resource_t;
typedef enum { AUTH_IDLE, AUTH_PENDING, AUTH_AUTHORIZED, AUTH_DENIED, AUTH_EXPIRED, AUTH_ERROR } auth_state_t;
typedef struct {
    auth_state_t state;
    char user_code[32], verification_uri[128];
    unsigned expires_in;
} auth_status_t;
esp_err_t identity_init(void);
esp_err_t identity_start(auth_resource_t resource);
auth_status_t identity_status(auth_resource_t resource);
void identity_cancel(auth_resource_t resource);
esp_err_t identity_unlink(void);
esp_err_t identity_access(auth_resource_t resource, bool force_refresh, char *token, size_t capacity);
const char *identity_state_name(auth_state_t state);
