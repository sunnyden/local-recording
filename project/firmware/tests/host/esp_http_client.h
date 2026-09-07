#pragma once
#include "recorder_network.h"
#include <stdint.h>
typedef struct fake_http *esp_http_client_handle_t;
enum { HTTP_EVENT_ON_HEADER = 1, HTTP_EVENT_ON_DATA = 2 };
#define ESP_ERR_HTTP_EAGAIN 0x7007
typedef struct {
    int event_id;
    esp_http_client_handle_t client;
    const char *header_key, *header_value;
    void *user_data;
    void *data;
    int data_len;
} esp_http_client_event_t;
typedef struct {
    const char *url;
    esp_http_client_method_t method;
    int (*crt_bundle_attach)(void *);
    int timeout_ms, buffer_size, buffer_size_tx;
    bool disable_auto_redirect;
    bool is_async;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t, const char *, int);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t, int);
esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
int esp_http_client_write(esp_http_client_handle_t, const char *, int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
