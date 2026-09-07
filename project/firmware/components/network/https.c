#include "recorder_network.h"
#include "https_operation.h"
#include "recorder_core.h"
#include "http_limits.h"
#include "json_limits.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

#define RESPONSE_LIMIT 24576
typedef struct {
    char *data;
    size_t used, limit;
    bool overflow;
    unsigned retry_after;
    const https_operation_t *operation;
    int64_t deadline;
    esp_err_t aborted;
} response_t;
static esp_err_t abort_receive(esp_http_client_event_t *event, esp_err_t error)
{
    response_t *r = event->user_data;
    if (!r->aborted) {
        r->aborted = error;
        /* SDK data callbacks' return codes alone do not abort perform(). Close
           the transport in this same task; cleanup remains with the caller. */
        esp_http_client_close(event->client);
    }
    return error;
}
static esp_err_t receive(esp_http_client_event_t *event)
{
    response_t *r = event->user_data;
    if (r->aborted) return r->aborted;
    if (r->operation && (event->event_id == HTTP_EVENT_ON_HEADER || event->event_id == HTTP_EVENT_ON_DATA)) {
        if (r->operation->cancelled && r->operation->cancelled())
            return abort_receive(event, ESP_ERR_INVALID_STATE);
        if (esp_timer_get_time() >= r->deadline) return abort_receive(event, ESP_ERR_TIMEOUT);
    }
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
        !strcasecmp(event->header_key, "Retry-After")) {
        char *end;
        unsigned long seconds = strtoul(event->header_value, &end, 10);
        r->retry_after = *end || seconds > 3600 ? 3601 : (unsigned)seconds;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (event->data_len < 0 || (size_t)event->data_len > r->limit - r->used) {
        r->overflow = true;
        return r->operation ? abort_receive(event, ESP_ERR_INVALID_SIZE) : ESP_ERR_INVALID_SIZE;
    }
    memcpy(r->data + r->used, event->data, event->data_len);
    r->used += event->data_len;
    r->data[r->used] = 0;
    return ESP_OK;
}
esp_err_t https_json_operation(const char *url, esp_http_client_method_t method,
    const char *bearer, const char *content_type, const char *body,
    int *status, cJSON **response, unsigned *retry_after, const https_operation_t *operation)
{
    *response = NULL; *status = 0;
    if (retry_after) *retry_after = 0;
    if (operation && (!operation->timeout_ms || operation->timeout_ms > 210000 ||
        !operation->response_limit || operation->response_limit > RESPONSE_LIMIT ||
        (body && strlen(body) > 4096))) return ESP_ERR_INVALID_SIZE;
    if (!url || strncmp(url, "https://", 8) || !recorder_network_ready() ||
        !recorder_time_valid()) return ESP_ERR_INVALID_STATE;
    int tx_size = recorder_http_tx_size(url, bearer);
    if (!tx_size) return ESP_ERR_INVALID_SIZE;
    size_t limit = operation ? operation->response_limit : RESPONSE_LIMIT;
    response_t r = {.data = calloc(1, limit + 1), .limit = limit, .operation = operation,
        .deadline = esp_timer_get_time() + (operation ? operation->timeout_ms : 10000) * 1000LL};
    if (!r.data) return ESP_ERR_NO_MEM;
    esp_http_client_config_t cfg = {.url = url, .method = method,
        .timeout_ms = operation ? (operation->timeout_ms < 1000 ? operation->timeout_ms : 1000) : 10000,
        .is_async = operation != NULL,
        .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
        .buffer_size = 2048, .buffer_size_tx = tx_size, .event_handler = receive, .user_data = &r};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    char *authorization = NULL;
    esp_err_t err = client ? ESP_OK : ESP_ERR_NO_MEM;
    if (client && bearer) {
        size_t n = strlen(bearer) + 8;
        authorization = malloc(n);
        if (!authorization) err = ESP_ERR_NO_MEM;
        else {
            snprintf(authorization, n, "Bearer %s", bearer);
            err = esp_http_client_set_header(client, "Authorization", authorization);
        }
    }
    if (client && content_type && err == ESP_OK)
        err = esp_http_client_set_header(client, "Content-Type", content_type);
    if (client && body && err == ESP_OK)
        err = esp_http_client_set_post_field(client, body, strlen(body));
    int64_t deadline = r.deadline;
    if (err == ESP_OK) {
        do {
            if (operation && operation->cancelled && operation->cancelled()) {
                err = ESP_ERR_INVALID_STATE; break;
            }
            if (operation) {
                int64_t remaining = (deadline - esp_timer_get_time()) / 1000;
                if (remaining <= 0) { err = ESP_ERR_TIMEOUT; break; }
                err = esp_http_client_set_timeout_ms(client, remaining < 1000 ? (int)remaining : 1000);
                if (err != ESP_OK) break;
            }
            err = esp_http_client_perform(client);
            if (operation && esp_timer_get_time() >= deadline) { err = ESP_ERR_TIMEOUT; break; }
            if (r.overflow) { err = ESP_ERR_INVALID_SIZE; break; }
            if (!operation || err != ESP_ERR_HTTP_EAGAIN) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        } while (true);
    }
    if (client) *status = esp_http_client_get_status_code(client);
    if (r.aborted) err = r.aborted;
    if (r.overflow) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK && operation && !recorder_json_depth_ok((const uint8_t *)r.data, r.used, 8))
        err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && r.used) {
        const char *end = NULL;
        *response = cJSON_ParseWithLengthOpts(r.data, r.used + 1, &end, true);
        if (json_has_nul((const uint8_t *)r.data, r.used) || !*response || end != r.data + r.used) {
            cJSON_Delete(*response); *response = NULL;
            err = ESP_ERR_INVALID_RESPONSE;
        }
        if (err != ESP_OK) {
            ESP_LOGE("http", "Request failed: %s HTTP=%d response_bytes=%u tx_capacity=%d",
                     esp_err_to_name(err), *status, (unsigned)r.used, tx_size);
        }
    }
    if (client) esp_http_client_cleanup(client);
    if (retry_after) *retry_after = r.retry_after;
    if (authorization) { secret_zero(authorization, strlen(authorization)); free(authorization); }
    secret_zero(r.data, r.used); free(r.data);
    return err;
}
esp_err_t https_json_retry_info(const char *url, esp_http_client_method_t method,
    const char *bearer, const char *content_type, const char *body,
    int *status, cJSON **response, unsigned *retry_after)
{
    return https_json_operation(url, method, bearer, content_type, body, status,
                                response, retry_after, NULL);
}
esp_err_t https_json(const char *url, esp_http_client_method_t method,
    const char *bearer, const char *content_type, const char *body,
    int *status, cJSON **response)
{
    return https_json_retry_info(url, method, bearer, content_type, body, status, response, NULL);
}
char *form_encode(const char *value)
{
    if (!value || strlen(value) > 8192) return NULL;
    size_t len = strlen(value);
    char *result = malloc(len * 3 + 1);
    if (!result) return NULL;
    char *p = result;
    const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = value[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') *p++ = c;
        else { *p++ = '%'; *p++ = hex[c >> 4]; *p++ = hex[c & 15]; }
    }
    *p = 0;
    return result;
}
