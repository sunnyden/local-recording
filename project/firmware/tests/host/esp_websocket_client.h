#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stdint.h>
typedef const char *esp_event_base_t;
typedef struct host_websocket *esp_websocket_client_handle_t;
typedef void (*host_ws_callback_t)(void *, esp_event_base_t, int32_t, void *);
enum { WEBSOCKET_EVENT_ANY = -1, WEBSOCKET_EVENT_CONNECTED, WEBSOCKET_EVENT_DATA,
    WEBSOCKET_EVENT_ERROR, WEBSOCKET_EVENT_DISCONNECTED };
typedef struct {
    const char *data_ptr;
    int data_len;
    bool fin;
    uint8_t op_code;
    int payload_len, payload_offset;
} esp_websocket_event_data_t;
typedef struct {
    const char *uri, *subprotocol, *headers;
    int (*crt_bundle_attach)(void *);
    bool disable_auto_reconnect;
    int buffer_size, task_stack, task_prio, network_timeout_ms, ping_interval_sec, pingpong_timeout_sec;
} esp_websocket_client_config_t;
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *);
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t, int, host_ws_callback_t, void *);
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t);
int esp_websocket_client_send_text(esp_websocket_client_handle_t, const char *, int, TickType_t);
int esp_websocket_client_send_bin(esp_websocket_client_handle_t, const char *, int, TickType_t);
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t);
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t);
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t);
