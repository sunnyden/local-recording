#include "recorder_network.h"
#include "recorder_core.h"
#include "http_limits.h"
#include "esp_http_client.h"
#include "https_operation.h"
#include "freertos/task.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct fake_http {
    esp_http_client_config_t config;
    size_t authorization_length;
};
static unsigned initialized;
static bool ready = true;
static bool valid_time = true;
static const char *response_body = "{\"id\":\"TEST\"}";
static int64_t now;
static bool again, cancelled;
static int expected_timeout = 10000;
static unsigned closed;
static bool dribble, cancel_during_receive;
int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(TickType_t ticks) { now += ticks * 1000LL; }
static bool is_cancelled(void) { return cancelled; }

bool recorder_network_ready(void) { return ready; }
bool recorder_time_valid(void) { return valid_time; }
void secret_zero(void *p, size_t n) { memset(p, 0, n); }
int esp_crt_bundle_attach(void *config) { (void)config; return 0; }

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    struct fake_http *client = calloc(1, sizeof(*client));
    assert(client);
    client->config = *config;
    assert(config->crt_bundle_attach && config->disable_auto_redirect);
    assert(config->timeout_ms == expected_timeout);
    ++initialized;
    return client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{
    if (!strcmp(key, "Authorization")) client->authorization_length = strlen(value);
    return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *body, int length)
{
    (void)client;
    assert(body && length == (int)strlen(body));
    return ESP_OK;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    if (again) return ESP_ERR_HTTP_EAGAIN;
    if (dribble) {
        unsigned was_closed = closed;
        for (unsigned i = 0; i < 20 && closed == was_closed; ++i) {
            now += 250000;
            if (cancel_during_receive && i == 1) cancelled = true;
            esp_http_client_event_t event = {.event_id = HTTP_EVENT_ON_DATA, .client = client,
                .user_data = client->config.user_data, .data = (void *)" ", .data_len = 1};
            (void)client->config.event_handler(&event); /* SDK ignores callback error codes. */
        }
        return ESP_FAIL;
    }
    /* The real SDK must fit each whole request line and Authorization header. */
    assert((size_t)client->config.buffer_size_tx > strlen(client->config.url) + 32);
    assert((size_t)client->config.buffer_size_tx > client->authorization_length + 32);
    esp_http_client_event_t event = {
        .event_id = HTTP_EVENT_ON_DATA, .user_data = client->config.user_data,
        .client = client,
        .data = (void *)response_body, .data_len = (int)strlen(response_body)
    };
    return client->config.event_handler(&event);
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout)
{
    assert(timeout > 0 && timeout <= 1000);
    client->config.timeout_ms = timeout;
    return ESP_OK;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t client)
{
    assert(client); ++closed; return ESP_OK;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client) { (void)client; return 200; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) { free(client); return ESP_OK; }

int main(void)
{
    char token[8193];
    memset(token, 'A', sizeof(token));
    token[1467] = 0;
    int status;
    cJSON *json = NULL;
    assert(https_json("https://graph.microsoft.com/v1.0/me/drive", HTTP_METHOD_GET,
        token, "application/json", NULL, &status, &json) == ESP_OK);
    assert(status == 200 && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json, "id")));
    cJSON_Delete(json);
    token[1467] = 'A';
    token[8192] = 0;
    assert(https_json("https://graph.microsoft.com/v1.0/me/drive", HTTP_METHOD_GET,
        token, NULL, NULL, &status, &json) == ESP_OK);
    cJSON_Delete(json);
    char url[4097];
    memset(url, 'x', sizeof(url));
    memcpy(url, "https://", 8);
    url[4096] = 0;
    assert(https_json(url, HTTP_METHOD_GET, NULL, NULL, NULL, &status, &json) == ESP_OK);
    cJSON_Delete(json);
    response_body = "{\"id\":\"TEST\"}garbage";
    assert(https_json("https://example.invalid", HTTP_METHOD_GET, NULL, NULL,
        NULL, &status, &json) == ESP_ERR_INVALID_RESPONSE && json == NULL);
    unsigned before = initialized;
    ready = false;
    assert(https_json("https://example.invalid", HTTP_METHOD_GET, NULL, NULL,
        NULL, &status, &json) == ESP_ERR_INVALID_STATE && initialized == before);
    ready = true;
    valid_time = true;
    expected_timeout = 1000;
    https_operation_t operation = {.timeout_ms = 210000, .response_limit = 16,
        .cancelled = is_cancelled};
    response_body = "{\"v\":1}";
    assert(https_json_operation("https://proxy.invalid", HTTP_METHOD_POST, token,
        "application/json", "{}", &status, &json, NULL, &operation) == ESP_OK);
    cJSON_Delete(json);
    response_body = "{\"this_exceeds_the_limit\":true}";
    assert(https_json_operation("https://proxy.invalid", HTTP_METHOD_POST, token,
        "application/json", "{}", &status, &json, NULL, &operation) == ESP_ERR_INVALID_SIZE);
    assert(closed == 1);
    again = true;
    int64_t start = now;
    assert(https_json_operation("https://proxy.invalid", HTTP_METHOD_POST, token,
        "application/json", "{}", &status, &json, NULL, &operation) == ESP_ERR_TIMEOUT);
    assert(now - start == 210000000LL);
    cancelled = true;
    assert(https_json_operation("https://proxy.invalid", HTTP_METHOD_POST, token,
        "application/json", "{}", &status, &json, NULL, &operation) == ESP_ERR_INVALID_STATE);
    cancelled = again = false;
    dribble = true; operation.timeout_ms = 1000;
    start = now;
    assert(https_json_operation("https://proxy.invalid", HTTP_METHOD_POST, token,
        "application/json", "{}", &status, &json, NULL, &operation) == ESP_ERR_TIMEOUT);
    assert(closed == 2 && now - start == 1000000);
    cancel_during_receive = true;
    assert(https_json_operation("https://proxy.invalid", HTTP_METHOD_POST, token,
        "application/json", "{}", &status, &json, NULL, &operation) == ESP_ERR_INVALID_STATE);
    assert(closed == 3);
    char huge_body[4098];
    memset(huge_body, 'x', sizeof(huge_body) - 1); huge_body[sizeof(huge_body) - 1] = 0;
    before = initialized;
    assert(https_json_operation("https://proxy.invalid", HTTP_METHOD_POST, token,
        "application/json", huge_body, &status, &json, NULL, &operation) == ESP_ERR_INVALID_SIZE);
    assert(initialized == before);
    char endpoint[320];
    assert(recorder_proxy_endpoint("wss://proxy.invalid/v1/voice", "/readyz", endpoint, sizeof(endpoint)));
    assert(!strcmp(endpoint, "https://proxy.invalid/readyz"));
    assert(!recorder_proxy_endpoint("wss://user@evil.invalid/v1/voice", "/readyz", endpoint, sizeof(endpoint)));
    assert(!recorder_proxy_endpoint("ws://proxy.invalid/v1/voice", "/readyz", endpoint, sizeof(endpoint)));
    assert(!recorder_proxy_endpoint("wss://proxy.invalid/v1/voice?url=evil", "/readyz", endpoint, sizeof(endpoint)));
    assert(!recorder_proxy_endpoint("wss://proxy.invalid/v1/voice", "//evil.invalid", endpoint, sizeof(endpoint)));
    ready = true;
    valid_time = false;
    before = initialized;
    assert(https_json("https://example.invalid", HTTP_METHOD_GET, NULL, NULL,
        NULL, &status, &json) == ESP_ERR_INVALID_STATE && initialized == before);
    puts("PASS: actual HTTPS helper, long bearer/request-line bounds and readiness guards");
    return 0;
}
