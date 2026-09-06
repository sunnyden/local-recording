#include "cloud_sync.h"
#include "recorder_core.h"
#include "http_limits.h"
#include "recorder_network.h"
#include "esp_http_client.h"
#include "freertos/task.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum scenario { NORMAL, UNCERTAIN, THROTTLED, MALFORMED, EXPIRED, QUOTA, CANCEL };
static enum scenario scenario;
static bool cancelled;
static unsigned puts_count, query_count, delete_count, confirmed, ticks;
static unsigned server_offset;
static const unsigned file_size = 700000;
static const char upload_url[] = "https://upload.invalid/TEST_CAPABILITY";
struct fake_http {
    esp_http_client_config_t cfg;
    char range[100], response[160];
    unsigned offset, length, written, read;
    int status;
};
bool cloud_sync_cancelled(void) { return cancelled; }
void cloud_sync_progress(uint32_t bytes, uint32_t total)
{
    assert(total == file_size && bytes < total);
    confirmed = bytes;
}
void vTaskDelay(TickType_t delay) { ticks += delay; }
uint32_t esp_random(void) { return 0; }
int esp_crt_bundle_attach(void *conf) { (void)conf; return 0; }
void secret_zero(void *p, size_t n) { memset(p, 0, n); }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    assert(!strcmp(cfg->url, upload_url));
    assert(cfg->method == HTTP_METHOD_PUT && cfg->crt_bundle_attach);
    assert(cfg->disable_auto_redirect && cfg->buffer_size <= 4096);
    assert(cfg->buffer_size_tx == recorder_http_tx_size(upload_url, NULL));
    struct fake_http *client = calloc(1, sizeof(*client));
    assert(client);
    client->cfg = *cfg;
    return client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value)
{
    assert(strcmp(key, "Authorization"));
    if (!strcmp(key, "Content-Range")) {
        assert(strlen(value) < sizeof(c->range)); strcpy(c->range, value);
    } else assert(!strcmp(key, "Content-Type") && !strcmp(value, "application/octet-stream"));
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int length)
{
    unsigned long start, end, total;
    assert(sscanf(c->range, "bytes %lu-%lu/%lu", &start, &end, &total) == 3);
    assert(total == file_size && end >= start && length == (int)(end - start + 1));
    assert((unsigned)length == upload_range_bytes(file_size, start));
    assert(start == server_offset);
    c->offset = start; c->length = length;
    ++puts_count;
    return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t c, const char *data, int length)
{
    assert(length <= 4096);
    int n = length > 511 ? 511 : length;
    for (int i = 0; i < n; ++i)
        assert((unsigned char)data[i] == (c->offset + c->written + (unsigned)i) % 251);
    c->written += n;
    assert(c->written <= c->length);
    if (scenario == CANCEL) cancelled = true;
    return n;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    assert(c->written == c->length);
    if (puts_count == 1 && scenario == UNCERTAIN) {
        server_offset = c->length;
        return -1;
    }
    c->status = c->offset + c->length == file_size ? 201 : 202;
    if (puts_count == 1 && scenario == THROTTLED) c->status = 429;
    if (scenario == EXPIRED) c->status = 410;
    if (scenario == QUOTA) c->status = 507;
    if (c->status == 201) strcpy(c->response, "{\"id\":\"TEST_ITEM\"}");
    else if (c->status == 202) {
        server_offset = c->offset + c->length;
        snprintf(c->response, sizeof(c->response), "{\"nextExpectedRanges\":[\"%u-%s\"]}",
            server_offset, scenario == MALFORMED ? "garbage" : "");
    } else strcpy(c->response, "{}");
    if (c->status == 429) {
        esp_http_client_event_t event = {.event_id = HTTP_EVENT_ON_HEADER,
            .header_key = "Retry-After", .header_value = "7", .user_data = c->cfg.user_data};
        assert(c->cfg.event_handler(&event) == ESP_OK);
    }
    return strlen(c->response);
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }
int esp_http_client_read(esp_http_client_handle_t c, char *data, int capacity)
{
    unsigned remaining = strlen(c->response) - c->read;
    unsigned n = remaining < (unsigned)capacity ? remaining : (unsigned)capacity;
    if (n > 7) n = 7;
    memcpy(data, c->response + c->read, n);
    c->read += n;
    return n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c)
{
    return c->read == strlen(c->response);
}
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { free(c); return ESP_OK; }
esp_err_t https_json(const char *url, esp_http_client_method_t method, const char *bearer,
    const char *content_type, const char *body, int *status, cJSON **json)
{
    assert(!strcmp(url, upload_url) && !bearer && !content_type && !body);
    *json = NULL;
    if (method == HTTP_METHOD_DELETE) { ++delete_count; *status = 204; return ESP_OK; }
    assert(method == HTTP_METHOD_GET);
    ++query_count;
    char response[128];
    snprintf(response, sizeof(response), "{\"nextExpectedRanges\":[\"%u-%s\"]}",
        server_offset, scenario == MALFORMED ? "garbage" : "");
    *json = cJSON_Parse(response);
    *status = 200;
    return ESP_OK;
}
static void run(FILE *file, enum scenario which, bool success)
{
    scenario = which; cancelled = false;
    puts_count = query_count = delete_count = confirmed = ticks = server_offset = 0;
    cJSON *item = NULL;
    esp_err_t result = upload_file_ranges(file, file_size, upload_url, &item);
    assert((result == ESP_OK) == success);
    if (success) {
        assert(item && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(item, "id")));
        assert(confirmed < file_size); /* Caller validates final item before reporting 100%. */
        assert(puts_count == (which == THROTTLED ? 4 : 3));
    }
    if (which == UNCERTAIN) assert(query_count == 1);
    if (which == THROTTLED) assert(query_count == 1 && ticks >= 7000);
    if (which == MALFORMED) assert(puts_count == 1 && query_count == 1);
    if (which == EXPIRED || which == QUOTA) assert(puts_count == 1 && !query_count);
    if (which == CANCEL) assert(delete_count == 1 && !confirmed);
    cJSON_Delete(item);
}
int main(void)
{
    FILE *file = fopen("upload-fixture.wav", "wb+");
    assert(file);
    unsigned char buffer[4096];
    for (unsigned offset = 0; offset < file_size;) {
        unsigned n = file_size - offset;
        if (n > sizeof(buffer)) n = sizeof(buffer);
        for (unsigned i = 0; i < n; ++i) buffer[i] = (offset + i) % 251;
        assert(fwrite(buffer, 1, n, file) == n); offset += n;
    }
    assert(!fflush(file));
    run(file, NORMAL, true);
    run(file, UNCERTAIN, true);
    run(file, THROTTLED, true);
    run(file, MALFORMED, false);
    run(file, EXPIRED, false);
    run(file, QUOTA, false);
    run(file, CANCEL, false);
    fclose(file);
    remove("upload-fixture.wav");
    puts("PASS: actual streamed uploader, short writes, range reconciliation, throttling and cancellation");
    return 0;
}
