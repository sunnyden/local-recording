#pragma once
#include "recorder_provisioning.h"
typedef enum { NETWORK_PROV_END } network_prov_cb_event_t;
typedef struct {
    const char *salt;
    int salt_len;
    const char *verifier;
    int verifier_len;
} network_prov_security2_params_t;
typedef struct {
    const void *scheme;
    int scheme_event_handler;
    struct { void (*event_cb)(void *, network_prov_cb_event_t, void *); } app_event_handler;
    struct { unsigned wifi_conn_attempts; } network_prov_wifi_conn_cfg;
} network_prov_mgr_config_t;
#define NETWORK_PROV_EVENT_HANDLER_NONE 0
#define NETWORK_PROV_SECURITY_2 2
esp_err_t network_prov_mgr_init(network_prov_mgr_config_t);
void network_prov_mgr_deinit(void);
void network_prov_mgr_stop_provisioning(void);
esp_err_t network_prov_mgr_disable_auto_stop(unsigned);
esp_err_t network_prov_mgr_endpoint_create(const char *);
esp_err_t network_prov_mgr_start_provisioning(int, const void *, const char *, const char *);
esp_err_t network_prov_mgr_endpoint_register(const char *,
    esp_err_t (*)(uint32_t, const uint8_t *, ssize_t, uint8_t **, ssize_t *, void *), void *);
