#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void *httpd_handle_t;
typedef enum {
    HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE, HTTP_HEAD, HTTP_OPTIONS,
    HTTP_PATCH, HTTP_ANY = 255
} httpd_method_t;
typedef struct httpd_req {
    httpd_method_t method;
    const char *uri;
    const char *query;
    const char *range;
    char status[40];
    char type[64];
    char headers[1024];
    char output[32768];
    size_t output_bytes;
} httpd_req_t;
typedef esp_err_t (*httpd_uri_func_t)(httpd_req_t *);
typedef struct {
    const char *uri;
    httpd_method_t method;
    httpd_uri_func_t handler;
    void *user_ctx;
} httpd_uri_t;
typedef struct {
    unsigned server_port, stack_size, max_open_sockets, max_uri_handlers;
    bool lru_purge_enable;
    unsigned recv_wait_timeout, send_wait_timeout;
    bool (*uri_match_fn)(const char *, const char *, size_t);
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define HTTPD_RESP_USE_STRLEN ((ssize_t)-1)
esp_err_t httpd_start(httpd_handle_t *, const httpd_config_t *);
esp_err_t httpd_stop(httpd_handle_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
bool httpd_uri_match_wildcard(const char *, const char *, size_t);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_send(httpd_req_t *, const char *, ssize_t);
esp_err_t httpd_resp_send_chunk(httpd_req_t *, const char *, ssize_t);
size_t httpd_req_get_url_query_len(httpd_req_t *);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *, char *, size_t);
size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *);
int httpd_send(httpd_req_t *, const char *, size_t);
int httpd_req_to_sockfd(httpd_req_t *);
esp_err_t httpd_sess_trigger_close(httpd_handle_t, int);
