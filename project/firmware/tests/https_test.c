#include "recorder_network.h"
#include "recorder_core.h"
#include "http_limits.h"
#include "esp_http_client.h"
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
    /* The real SDK must fit each whole request line and Authorization header. */
    assert((size_t)client->config.buffer_size_tx > strlen(client->config.url) + 32);
    assert((size_t)client->config.buffer_size_tx > client->authorization_length + 32);
    esp_http_client_event_t event = {
        .event_id = HTTP_EVENT_ON_DATA, .user_data = client->config.user_data,
        .data = (void *)response_body, .data_len = (int)strlen(response_body)
    };
    return client->config.event_handler(&event);
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
    valid_time = false;
    assert(https_json("https://example.invalid", HTTP_METHOD_GET, NULL, NULL,
        NULL, &status, &json) == ESP_ERR_INVALID_STATE && initialized == before);
    puts("PASS: actual HTTPS helper, long bearer/request-line bounds and readiness guards");
    return 0;
}
