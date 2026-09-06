#include "cloud_sync.h"
#include "recorder_network.h"
#include "recorder_core.h"
#include "http_limits.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool wait_retry(unsigned seconds)
{
    if (seconds > 120) return false;
    for (unsigned i = 0; i < seconds * 10; ++i) {
        if (cloud_sync_cancelled()) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return !cloud_sync_cancelled();
}
static esp_err_t header(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
        !strcasecmp(event->header_key, "Retry-After")) {
        char *end;
        unsigned long value = strtoul(event->header_value, &end, 10);
        *(unsigned *)event->user_data = *end || value > 3600 ? 3601 : (unsigned)value;
    }
    return ESP_OK;
}
static esp_err_t put_range(FILE *file, const char *url, uint32_t offset, uint32_t length,
    uint32_t total, int *status, cJSON **json, unsigned *retry_after)
{
    *json = NULL; *status = 0;
    int tx_size = recorder_http_tx_size(url, NULL);
    if (!tx_size) return ESP_ERR_INVALID_SIZE;
    esp_http_client_config_t cfg = {.url = url, .method = HTTP_METHOD_PUT,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000,
        .disable_auto_redirect = true, .buffer_size = 2048, .buffer_size_tx = tx_size,
        .event_handler = header, .user_data = retry_after};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    char range[96];
    snprintf(range, sizeof(range), "bytes %lu-%lu/%lu", (unsigned long)offset,
        (unsigned long)(offset + length - 1), (unsigned long)total);
    esp_err_t err = esp_http_client_set_header(client, "Content-Range", range);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    if (err == ESP_OK && fseek(file, offset, SEEK_SET)) err = ESP_FAIL;
    if (err == ESP_OK) err = esp_http_client_open(client, length);
    uint8_t buffer[4096];
    uint32_t remaining = length;
    while (err == ESP_OK && remaining) {
        if (cloud_sync_cancelled()) { err = ESP_ERR_INVALID_STATE; break; }
        size_t bytes = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        if (fread(buffer, 1, bytes, file) != bytes) { err = ESP_FAIL; break; }
        size_t sent = 0;
        while (sent < bytes) {
            int n = esp_http_client_write(client, (char *)buffer + sent, bytes - sent);
            if (n <= 0) { err = ESP_FAIL; break; }
            sent += n;
        }
        remaining -= bytes;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    if (err == ESP_OK) {
        *status = esp_http_client_get_status_code(client);
        char *response = calloc(1, 8193);
        if (!response) err = ESP_ERR_NO_MEM;
        else {
            size_t used = 0;
            while (used < 8192) {
                int n = esp_http_client_read(client, response + used, 8192 - used);
                if (n < 0) { err = ESP_FAIL; break; }
                if (!n) break;
                used += n;
            }
            if (!esp_http_client_is_complete_data_received(client)) err = ESP_ERR_INVALID_SIZE;
            if (err == ESP_OK && used) {
                const char *end = NULL;
                *json = cJSON_ParseWithLengthOpts(response, used + 1, &end, true);
                if (json_has_nul((const uint8_t *)response, used) || !*json || end != response + used) {
                    cJSON_Delete(*json); *json = NULL;
                    err = ESP_ERR_INVALID_RESPONSE;
                }
            }
            secret_zero(response, used); free(response);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}
static bool next_offset(cJSON *json, uint32_t total, uint32_t *offset)
{
    cJSON *ranges = cJSON_GetObjectItemCaseSensitive(json, "nextExpectedRanges");
    if (!cJSON_IsArray(ranges) || cJSON_GetArraySize(ranges) != 1) return false;
    cJSON *first = cJSON_GetArrayItem(ranges, 0);
    if (!cJSON_IsString(first) || !first->valuestring[0]) return false;
    const char *s = first->valuestring;
    if (*s < '0' || *s > '9') return false;
    errno = 0;
    char *end = NULL;
    unsigned long long n = strtoull(s, &end, 10);
    if (errno || !end || *end != '-' || n >= total || n % UPLOAD_RANGE_SIZE) return false;
    if (end[1]) {
        const char *last = end + 1;
        if (*last < '0' || *last > '9') return false;
        errno = 0;
        unsigned long long final = strtoull(last, &end, 10);
        if (errno || *end || final != total - 1) return false;
    }
    *offset = (uint32_t)n;
    return true;
}
esp_err_t upload_file_ranges(FILE *file, uint32_t total, const char *url, cJSON **completed)
{
    *completed = NULL;
    if (!file || !total || !url || strncmp(url, "https://", 8) || strlen(url) > 4096)
        return ESP_ERR_INVALID_ARG;
    uint32_t offset = 0;
    unsigned failures = 0;
    esp_err_t err = ESP_OK;
    while (offset < total && !cloud_sync_cancelled()) {
        uint32_t length = upload_range_bytes(total, offset);
        cJSON *json = NULL;
        int status = 0;
        unsigned retry_after = 0;
        err = put_range(file, url, offset, length, total, &status, &json, &retry_after);
        if (err == ESP_OK && (status == 200 || status == 201)) {
            if (offset + length != total) { cJSON_Delete(json); return ESP_ERR_INVALID_RESPONSE; }
            *completed = json;
            return ESP_OK;
        }
        uint32_t next = 0;
        if (err == ESP_OK && status == 202 && next_offset(json, total, &next) &&
            next == offset + length) {
            offset = next; failures = 0;
            cloud_sync_progress(offset, total);
            cJSON_Delete(json);
            continue;
        }
        cJSON_Delete(json); json = NULL;
        if (status == 401 || status == 403 || status == 404 || status == 410 || status == 507)
            return ESP_ERR_INVALID_STATE;
        if (++failures > 4 || !wait_retry(retry_after ? retry_after : (1u << failures) + esp_random() % 3))
            break;
        /* PUT outcome may be ambiguous; never blindly resend a committed range. */
        err = https_json(url, HTTP_METHOD_GET, NULL, NULL, NULL, &status, &json);
        if (err == ESP_OK && status == 200 && next_offset(json, total, &next)) {
            offset = next;
            cloud_sync_progress(offset, total);
        } else {
            cJSON_Delete(json);
            return ESP_ERR_INVALID_RESPONSE; /* Caller checks final drive item. */
        }
        cJSON_Delete(json);
    }
    if (cloud_sync_cancelled()) {
        cJSON *ignored = NULL; int status;
        https_json(url, HTTP_METHOD_DELETE, NULL, NULL, NULL, &status, &ignored);
        cJSON_Delete(ignored);
    }
    return err == ESP_OK ? ESP_FAIL : err;
}
