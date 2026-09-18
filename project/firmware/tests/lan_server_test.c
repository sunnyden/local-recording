#include "lan_server.h"
#include "recorder.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static httpd_uri_t handlers[2];
static unsigned registered;
void vTaskDelay(TickType_t ticks) { (void)ticks; }
esp_err_t httpd_start(httpd_handle_t *server, const httpd_config_t *config)
{
    assert(config->server_port == 80 && config->max_open_sockets == 2 &&
           config->stack_size == 4096 && config->max_uri_handlers == 2);
    *server = (void *)1; return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t server) { (void)server; return ESP_OK; }
esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *handler)
{ assert(server && registered < 2); handlers[registered++] = *handler; return ESP_OK; }
bool httpd_uri_match_wildcard(const char *a, const char *b, size_t n)
{ (void)a; (void)b; (void)n; return true; }
static void append(char *out, size_t capacity, const char *format, ...)
{
    va_list args; va_start(args, format);
    size_t used = strlen(out);
    vsnprintf(out + used, capacity - used, format, args);
    va_end(args);
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *name, const char *value)
{ append(r->headers, sizeof(r->headers), "%s:%s\n", name, value); return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{ snprintf(r->status, sizeof(r->status), "%s", status); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{ snprintf(r->type, sizeof(r->type), "%s", type); return ESP_OK; }
static esp_err_t send(httpd_req_t *r, const char *data, ssize_t bytes)
{
    if (!data) return ESP_OK;
    size_t n = bytes == HTTPD_RESP_USE_STRLEN ? strlen(data) : (size_t)bytes;
    assert(n <= sizeof(r->output) - r->output_bytes);
    memcpy(r->output + r->output_bytes, data, n);
    r->output_bytes += n; r->output[r->output_bytes] = 0;
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *r, const char *data, ssize_t bytes)
{ return send(r, data, bytes); }
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *data, ssize_t bytes)
{ return send(r, data, bytes); }
size_t httpd_req_get_url_query_len(httpd_req_t *r)
{ return r->query ? strlen(r->query) : 0; }
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *out, size_t size)
{ if (!r->query || strlen(r->query) >= size) return ESP_FAIL; strcpy(out, r->query); return ESP_OK; }
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *name)
{ return !strcmp(name, "Range") && r->range ? strlen(r->range) : 0; }
int httpd_send(httpd_req_t *r, const char *data, size_t bytes)
{ send(r, data, (ssize_t)bytes); return (int)bytes; }
int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return 7; }
esp_err_t httpd_sess_trigger_close(httpd_handle_t server, int socket)
{ assert(server && socket == 7); return ESP_OK; }
esp_err_t mdns_init(void) { return ESP_OK; }
void mdns_free(void) {}
esp_err_t mdns_hostname_set(const char *name)
{ assert(!strcmp(name, "embedded-recorder")); return ESP_OK; }
esp_err_t mdns_instance_name_set(const char *name)
{ assert(!strcmp(name, "Embedded Recorder")); return ESP_OK; }
esp_err_t mdns_service_add(const char *instance, const char *service,
    const char *protocol, unsigned port, const void *txt, size_t count)
{
    assert(!strcmp(instance, "Embedded Recorder") && !strcmp(service, "_http") &&
           !strcmp(protocol, "_tcp") && port == 80 && !txt && !count);
    return ESP_OK;
}
bool storage_valid_name(const char *name)
{ return name && !strcmp(name, "rec-test.opus"); }
esp_err_t storage_catalog(size_t index, char *name, size_t capacity, size_t *count)
{
    *count = 2; name[0] = 0;
    if (index < 2) snprintf(name, capacity, "rec-test.opus");
    return ESP_OK;
}
esp_err_t storage_recording_info(const char *name, recording_info_t *info)
{
    assert(storage_valid_name(name));
    *info = (recording_info_t){.bytes = 8, .samples = 320};
    return ESP_OK;
}
esp_err_t storage_catalog_info(size_t index, char *name, size_t capacity,
                               recording_info_t *info, size_t *count)
{
    esp_err_t err = storage_catalog(index, name, capacity, count);
    if (err == ESP_OK && name[0]) err = storage_recording_info(name, info);
    return err;
}
esp_err_t storage_catalog_page(size_t offset, size_t limit,
    recording_catalog_item_t *items, size_t capacity, size_t *count, size_t *total)
{
    assert(limit <= capacity);
    *count = 0; *total = 2;
    for (size_t i = offset; i < *total && *count < limit; ++i) {
        snprintf(items[*count].name, sizeof(items[*count].name), "rec-test.opus");
        items[*count].info = (recording_info_t){.bytes = 8, .samples = 320};
        ++*count;
    }
    return ESP_OK;
}
esp_err_t storage_open_recording(const char *name, FILE **file, recording_info_t *info)
{
    if (!storage_valid_name(name)) return ESP_ERR_INVALID_ARG;
    *file = tmpfile(); assert(*file);
    assert(fwrite("OggSdata", 1, 8, *file) == 8 && !fseek(*file, 0, SEEK_SET));
    *info = (recording_info_t){.bytes = 8, .samples = 320};
    return ESP_OK;
}
bool recording_timestamp(const recording_time_t *time, char out[36])
{ (void)time; out[0] = 0; return false; }

#include "../components/lan_server/lan_server.c"

static httpd_req_t request(httpd_method_t method, const char *uri)
{ return (httpd_req_t){.method = method, .uri = uri}; }
int main(void)
{
    assert(lan_server_init() == ESP_OK && registered == 2);
    httpd_req_t r = request(HTTP_GET, "/v1/recordings");
    assert(handlers[0].handler(&r) == ESP_OK);
    assert(!strcmp(r.status, "503 Service Unavailable") && strstr(r.output, "\"busy\""));
    assert(strstr(r.headers, "Access-Control-Allow-Origin:*"));

    lan_server_set_available(true);
    r = request(HTTP_GET, "/v1/recordings"); r.query = "offset=0&limit=2";
    assert(handlers[0].handler(&r) == ESP_OK);
    assert(strstr(r.output, "\"count\":2") && strstr(r.output, "rec-test.opus"));
    r = request(HTTP_GET, "/v1/recordings"); r.query = "limit=2&limit=3";
    assert(handlers[0].handler(&r) == ESP_OK);
    assert(!strcmp(r.status, "400 Bad Request"));
    r = request(HTTP_OPTIONS, "/v1/recordings");
    assert(handlers[0].handler(&r) == ESP_OK && !strcmp(r.status, "204 No Content"));

    r = request(HTTP_GET, "/v1/recordings/../bad.opus");
    assert(handlers[1].handler(&r) == ESP_OK && !strcmp(r.status, "400 Bad Request"));
    r = request(HTTP_GET, "/v1/recordings/rec-test.opus"); r.range = "bytes=0-1";
    assert(handlers[1].handler(&r) == ESP_OK &&
           !strcmp(r.status, "416 Range Not Satisfiable"));
    r = request(HTTP_HEAD, "/v1/recordings/rec-test.opus");
    assert(handlers[1].handler(&r) == ESP_OK &&
           !strcmp(r.status, "405 Method Not Allowed"));
    r = request(HTTP_GET, "/v1/recordings/rec-test.opus");
    assert(handlers[1].handler(&r) == ESP_OK);
    assert(strstr(r.output, "Content-Length: 8") && strstr(r.output, "OggSdata"));
    puts("PASS: LAN listing, pagination, CORS, idle gate, path/range/method rejection and streaming");
}
