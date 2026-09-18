#include "lan_server.h"
#include "recorder.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define STREAM_BYTES 8192u
#define PAGE_MAX 50u
typedef struct {
    httpd_handle_t server;
    uint8_t *buffer;
    _Atomic bool leased;
} lan_server_t;
static lan_server_t state;
static _Atomic bool available;
static bool mdns_started;

static esp_err_t headers(httpd_req_t *request)
{
    esp_err_t err = httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
    if (err == ESP_OK) err = httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (err == ESP_OK) err = httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    return err;
}
static esp_err_t error_json(httpd_req_t *request, const char *status,
                            const char *code)
{
    char body[96];
    snprintf(body, sizeof(body), "{\"v\":1,\"error\":\"%s\"}", code);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    headers(request);
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}
static bool lease(void)
{
    bool expected = false;
    return atomic_compare_exchange_strong(&state.leased, &expected, true);
}
static void release(void) { atomic_store(&state.leased, false); }
static esp_err_t check_available(httpd_req_t *request)
{
    if (atomic_load(&available)) return ESP_OK;
    httpd_resp_set_hdr(request, "Retry-After", "2");
    error_json(request, "503 Service Unavailable", "busy");
    return ESP_ERR_INVALID_STATE;
}
static esp_err_t options(httpd_req_t *request)
{
    httpd_resp_set_status(request, "204 No Content");
    headers(request);
    httpd_resp_set_hdr(request, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Headers", "Content-Type");
    return httpd_resp_send(request, NULL, 0);
}
static bool decimal(const char *text, unsigned maximum, unsigned *value)
{
    if (!text || !*text || !value) return false;
    unsigned result = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || result > (maximum - (unsigned)(*p - '0')) / 10)
            return false;
        result = result * 10 + (unsigned)(*p - '0');
    }
    *value = result;
    return true;
}
static bool pagination(httpd_req_t *request, unsigned *offset, unsigned *limit)
{
    *offset = 0; *limit = 20;
    size_t length = httpd_req_get_url_query_len(request);
    if (!length) return true;
    if (length >= 128) return false;
    char query[128];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK)
        return false;
    bool saw_offset = false, saw_limit = false;
    char *part = query;
    while (part && *part) {
        char *next = strchr(part, '&');
        if (next) *next++ = 0;
        char *equals = strchr(part, '=');
        if (!equals || strchr(equals + 1, '=')) return false;
        *equals++ = 0;
        if (!strcmp(part, "offset") && !saw_offset) {
            if (!decimal(equals, 999999, offset)) return false;
            saw_offset = true;
        } else if (!strcmp(part, "limit") && !saw_limit) {
            if (!decimal(equals, PAGE_MAX, limit) || !*limit) return false;
            saw_limit = true;
        } else return false;
        part = next;
    }
    return true;
}
static esp_err_t chunk(httpd_req_t *request, const char *text)
{
    return httpd_resp_send_chunk(request, text, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t list_recordings(httpd_req_t *request)
{
    if (request->method == HTTP_OPTIONS) return options(request);
    if (request->method != HTTP_GET)
        return error_json(request, "405 Method Not Allowed", "method_not_allowed");
    if (check_available(request) != ESP_OK) return ESP_OK;
    unsigned offset, limit;
    if (!pagination(request, &offset, &limit))
        return error_json(request, "400 Bad Request", "invalid_query");
    if (!lease())
        return error_json(request, "503 Service Unavailable", "busy");
    recording_catalog_item_t *items = (recording_catalog_item_t *)state.buffer;
    size_t total = 0, count = 0;
    esp_err_t err = atomic_load(&available) ?
        storage_catalog_page(offset, limit, items, PAGE_MAX, &count, &total) :
        ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        release();
        if (err == ESP_ERR_INVALID_STATE)
            return error_json(request, "503 Service Unavailable", "busy");
        return error_json(request, "500 Internal Server Error", "storage_error");
    }
    httpd_resp_set_type(request, "application/json");
    headers(request);
    char text[384];
    snprintf(text, sizeof(text),
             "{\"v\":1,\"offset\":%u,\"next_offset\":%u,\"limit\":%u,"
             "\"count\":%u,\"total\":%u,\"items\":[",
             offset, (unsigned)(offset + count), limit,
             (unsigned)count, (unsigned)total);
    err = chunk(request, text);
    for (size_t i = 0; err == ESP_OK && i < count; ++i) {
        if (!atomic_load(&available)) { err = ESP_FAIL; break; }
        char timestamp[36] = "";
        if (items[i].info.has_time)
            recording_timestamp(&items[i].info.time, timestamp);
        snprintf(text, sizeof(text),
            "%s{\"name\":\"%s\",\"bytes\":%lu,\"samples\":%lu,"
            "\"duration_ms\":%lu%s%s%s,\"download\":\"/v1/recordings/%s\"}",
            i ? "," : "", items[i].name, (unsigned long)items[i].info.bytes,
            (unsigned long)items[i].info.samples,
            (unsigned long)((uint64_t)items[i].info.samples * 1000u / PCM_RATE),
            timestamp[0] ? ",\"recorded_at\":\"" : "",
            timestamp, timestamp[0] ? "\"" : "", items[i].name);
        err = chunk(request, text);
    }
    if (err == ESP_OK) err = chunk(request, "]}");
    if (err == ESP_OK) err = httpd_resp_send_chunk(request, NULL, 0);
    release();
    return err;
}
static int send_all(httpd_req_t *request, const void *data, size_t bytes)
{
    const char *p = data;
    size_t sent = 0;
    while (sent < bytes) {
        int result = httpd_send(request, p + sent, bytes - sent);
        if (result <= 0) return result;
        sent += (size_t)result;
    }
    return (int)sent;
}
static esp_err_t download(httpd_req_t *request)
{
    if (request->method == HTTP_OPTIONS) return options(request);
    if (request->method != HTTP_GET)
        return error_json(request, "405 Method Not Allowed", "method_not_allowed");
    if (check_available(request) != ESP_OK) return ESP_OK;
    if (httpd_req_get_hdr_value_len(request, "Range"))
        return error_json(request, "416 Range Not Satisfiable", "range_not_supported");
    static const char prefix[] = "/v1/recordings/";
    const char *name = request->uri + sizeof(prefix) - 1;
    if (!*name || strlen(name) > 64 || strchr(name, '?') || strchr(name, '%') ||
        strchr(name, '/') || strchr(name, '\\') || !storage_valid_name(name))
        return error_json(request, "400 Bad Request", "invalid_name");
    if (!lease())
        return error_json(request, "503 Service Unavailable", "busy");
    if (!atomic_load(&available)) {
        release();
        return error_json(request, "503 Service Unavailable", "busy");
    }
    FILE *file = NULL;
    recording_info_t info;
    esp_err_t err = storage_open_recording(name, &file, &info);
    if (err != ESP_OK) {
        release();
        if (err == ESP_ERR_NOT_FOUND)
            return error_json(request, "404 Not Found", "not_found");
        if (err == ESP_ERR_NOT_SUPPORTED)
            return error_json(request, "415 Unsupported Media Type", "invalid_recording");
        return error_json(request, "500 Internal Server Error", "storage_error");
    }
    char disposition[104], header[384];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
    int header_bytes = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: audio/ogg; codecs=opus\r\n"
        "Content-Length: %lu\r\nContent-Disposition: %s\r\n"
        "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
        "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
        (unsigned long)info.bytes, disposition);
    bool ok = header_bytes > 0 && (size_t)header_bytes < sizeof(header) &&
              send_all(request, header, (size_t)header_bytes) == header_bytes;
    uint32_t remaining = info.bytes;
    while (ok && remaining) {
        if (!atomic_load(&available)) { ok = false; break; }
        size_t bytes = remaining < STREAM_BYTES ? remaining : STREAM_BYTES;
        size_t read = fread(state.buffer, 1, bytes, file);
        if (read != bytes || send_all(request, state.buffer, read) != (int)read) {
            ok = false; break;
        }
        remaining -= (uint32_t)read;
    }
    if (fclose(file)) ok = false;
    release();
    httpd_sess_trigger_close(state.server, httpd_req_to_sockfd(request));
    return ok ? ESP_OK : ESP_FAIL;
}
esp_err_t lan_server_init(void)
{
    if (state.server) return ESP_OK;
    state.buffer = heap_caps_malloc(STREAM_BYTES,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!state.buffer) return ESP_ERR_NO_MEM;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 4096;
    config.max_open_sockets = 2;
    config.max_uri_handlers = 2;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&state.server, &config);
    httpd_uri_t list = {.uri = "/v1/recordings", .method = HTTP_ANY,
                        .handler = list_recordings};
    httpd_uri_t file = {.uri = "/v1/recordings/*", .method = HTTP_ANY,
                        .handler = download};
    if (err == ESP_OK) err = httpd_register_uri_handler(state.server, &list);
    if (err == ESP_OK) err = httpd_register_uri_handler(state.server, &file);
    if (err == ESP_OK) {
        err = mdns_init();
        if (err == ESP_OK) mdns_started = true;
    }
    if (err == ESP_OK) err = mdns_hostname_set("embedded-recorder");
    if (err == ESP_OK) err = mdns_instance_name_set("Embedded Recorder");
    if (err == ESP_OK) err = mdns_service_add("Embedded Recorder", "_http",
                                               "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        if (mdns_started) {
            mdns_free();
            mdns_started = false;
        }
        if (state.server) httpd_stop(state.server);
        state.server = NULL;
        heap_caps_free(state.buffer);
        state.buffer = NULL;
    }
    return err;
}
void lan_server_set_available(bool value) { atomic_store(&available, value); }
bool lan_server_available(void) { return atomic_load(&available); }
esp_err_t lan_server_pause(void)
{
    atomic_store(&available, false);
    for (unsigned i = 0; atomic_load(&state.leased) && i < 250; ++i)
        vTaskDelay(pdMS_TO_TICKS(20));
    return atomic_load(&state.leased) ? ESP_ERR_TIMEOUT : ESP_OK;
}
