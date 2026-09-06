#include "identity.h"
#include "recorder_provisioning.h"
#include "recorder_network.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int64_t clock_us;
static int64_t cancel_at;
static TaskFunction_t pending_task;
static void *pending_arg;
static bool wifi = true, storage_allowed = true, storage_fail, setup_stopped;
static char stored[2][8193];
typedef struct { const char *suffix; int status; const char *json; } response_t;
static response_t responses[12];
static size_t response_count, response_index;
static int64_t request_times[12];
static char last_body[4096];
static unsigned commits;
static bool observe_pending;
static unsigned pending_checks;

int64_t esp_timer_get_time(void) { return clock_us; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
BaseType_t xTaskCreate(TaskFunction_t task, const char *name, unsigned stack,
    void *arg, unsigned priority, void *handle)
{
    (void)name; (void)stack; (void)priority; (void)handle;
    assert(!pending_task);
    pending_task = task; pending_arg = arg;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks)
{
    clock_us += (int64_t)ticks * 1000;
    if (observe_pending && clock_us >= 1000000) {
        const char request[] = "{\"v\":1,\"id\":\"async-code\",\"type\":\"auth.status\",\"resource\":\"graph\"}";
        uint8_t *response = NULL; ssize_t length = 0;
        assert(recorder_control(1, (const uint8_t *)request, strlen(request),
            &response, &length, NULL) == ESP_OK);
        assert(length > 0 && length <= 496);
        cJSON *json = cJSON_ParseWithLength((char *)response, length);
        assert(json);
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(json, "state")->valuestring, "pending"));
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(json, "user_code")->valuestring, "ABCD-EFGH"));
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(json, "verification_uri")->valuestring,
                       "https://microsoft.com/devicelogin"));
        assert(cJSON_GetObjectItemCaseSensitive(json, "expires_in")->valueint == 899);
        assert(!strstr((char *)response, "TEST_DEVICE_ONLY"));
        cJSON_Delete(json); free(response);
        observe_pending = false; ++pending_checks;
    }
    if (cancel_at && clock_us >= cancel_at) identity_cancel(AUTH_GRAPH);
}
void vTaskDelete(void *task) { (void)task; }
bool credential_storage_allowed(void) { return storage_allowed; }
bool recorder_network_ready(void) { return wifi; }
bool recorder_time_valid(void) { return true; }
void recorder_setup_stop(void) { setup_stopped = true; }
void secret_zero(void *p, size_t n) { memset(p, 0, n); }
esp_err_t credential_read(const char *key, char *value, size_t capacity)
{
    const char *source = stored[!strcmp(key, "proxy_refresh")];
    if (!source[0]) return ESP_ERR_NOT_FOUND;
    assert(strlen(source) < capacity); strcpy(value, source); return ESP_OK;
}
esp_err_t credential_write(const char *key, const char *value)
{
    if (storage_fail) return ESP_FAIL;
    assert(!strcmp(key, "graph_refresh") || !strcmp(key, "proxy_refresh"));
    assert(strlen(value) < sizeof(stored[0]));
    strcpy(stored[!strcmp(key, "proxy_refresh")], value);
    ++commits;
    return ESP_OK;
}
esp_err_t credential_erase_all(void)
{
    if (storage_fail) return ESP_FAIL;
    memset(stored, 0, sizeof(stored));
    return ESP_OK;
}
char *form_encode(const char *value) { return strdup(value); }
esp_err_t https_json(const char *url, esp_http_client_method_t method, const char *bearer,
    const char *content_type, const char *body, int *status, cJSON **json)
{
    assert(response_index < response_count);
    assert(method == HTTP_METHOD_POST && bearer == NULL);
    assert(!strcmp(content_type, "application/x-www-form-urlencoded"));
    const response_t *r = &responses[response_index];
    assert(strlen(url) >= strlen(r->suffix));
    assert(!strcmp(url + strlen(url) - strlen(r->suffix), r->suffix));
    assert(strlen(body) < sizeof(last_body));
    strcpy(last_body, body);
    request_times[response_index++] = clock_us;
    *status = r->status;
    *json = cJSON_Parse(r->json);
    assert(*json);
    return ESP_OK;
}
static void script(const response_t *values, size_t count)
{
    if (count) memcpy(responses, values, count * sizeof(*values));
    response_count = count; response_index = 0;
    memset(request_times, 0, sizeof(request_times));
    cancel_at = 0; clock_us = 0;
}
static void run_task(void)
{
    assert(pending_task);
    TaskFunction_t task = pending_task;
    void *arg = pending_arg;
    pending_task = NULL;
    task(arg);
}
static cJSON *request(const char *body)
{
    uint8_t *out = NULL; ssize_t size = 0;
    assert(recorder_control(1, (const uint8_t *)body, strlen(body), &out, &size, NULL) == ESP_OK);
    assert(out && size > 0 && size <= RECORDER_CONTROL_MAX_JSON);
    assert(!strstr((char *)out, "access_token") && !strstr((char *)out, "refresh_token"));
    assert(!strstr((char *)out, "device_code") && !strstr((char *)out, "TEST_ACCESS"));
    cJSON *json = cJSON_ParseWithLength((char *)out, size);
    free(out);
    assert(json);
    return json;
}
static void rejects(const char *body, const char *code)
{
    cJSON *json = request(body);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "ok")));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(json, "code")->valuestring, code));
    cJSON_Delete(json);
}
#define DEVICE_REPLY "{\"device_code\":\"TEST_DEVICE_ONLY\",\"user_code\":\"ABCD-EFGH\",\"verification_uri\":\"https://microsoft.com/devicelogin\",\"expires_in\":900,\"interval\":5}"
#define GRAPH_REPLY "{\"access_token\":\"TEST_ACCESS_GRAPH\",\"refresh_token\":\"TEST_REFRESH_GRAPH\",\"token_type\":\"Bearer\",\"expires_in\":3600}"
#define PROXY_REPLY "{\"access_token\":\"TEST_ACCESS_PROXY\",\"refresh_token\":\"TEST_REFRESH_PROXY\",\"token_type\":\"Bearer\",\"expires_in\":3600}"
static void polling_and_resources(void)
{
    response_t flow[] = {
        {"devicecode",200,DEVICE_REPLY},
        {"token",400,"{\"error\":\"slow_down\"}"},
        {"token",400,"{\"error\":\"authorization_pending\"}"},
        {"token",200,GRAPH_REPLY}
    };
    script(flow, 4);
    observe_pending = true;
    assert(identity_start(AUTH_GRAPH) == ESP_OK);
    assert(identity_start(AUTH_PROXY) == ESP_ERR_INVALID_STATE);
    assert(identity_status(AUTH_GRAPH).state == AUTH_PENDING);
    run_task();
    assert(response_index == 4);
    assert(pending_checks == 1);
    assert(request_times[1] == 5000000 && request_times[2] == 15000000 && request_times[3] == 25000000);
    assert(identity_status(AUTH_GRAPH).state == AUTH_AUTHORIZED);
    assert(identity_status(AUTH_GRAPH).user_code[0] == 0);
    char token[128];
    assert(identity_access(AUTH_GRAPH, false, token, sizeof(token)) == ESP_OK);
    assert(!strcmp(token, "TEST_ACCESS_GRAPH"));
    assert(strstr(last_body, "https://graph.microsoft.com/Files.ReadWrite offline_access"));
    response_t proxy[] = {{"devicecode",200,DEVICE_REPLY},{"token",200,PROXY_REPLY}};
    script(proxy, 2);
    assert(identity_start(AUTH_PROXY) == ESP_OK); run_task();
    assert(identity_access(AUTH_PROXY, false, token, sizeof(token)) == ESP_OK);
    assert(!strcmp(token, "TEST_ACCESS_PROXY"));
    assert(strstr(last_body, "api://00000000-0000-0000-0000-000000000002/access_as_user"));
    assert(identity_access(AUTH_GRAPH, false, token, sizeof(token)) == ESP_OK);
    assert(!strcmp(token, "TEST_ACCESS_GRAPH"));
    assert(!strcmp(stored[0], "TEST_REFRESH_GRAPH") && !strcmp(stored[1], "TEST_REFRESH_PROXY"));
}
static void endpoint_validation(void)
{
    rejects("{", "invalid_request");
    rejects("[]", "invalid_request");
    rejects("{\"v\":2,\"id\":\"a\",\"type\":\"status\"}", "invalid_request");
    rejects("{\"v\":1,\"v\":1,\"id\":\"a\",\"type\":\"status\"}", "invalid_request");
    rejects("{\"v\":1,\"id\":\"\",\"type\":\"status\"}", "invalid_request");
    rejects("{\"v\":1,\"id\":\"a\",\"type\":123}", "invalid_request");
    rejects("{\"v\":1,\"id\":\"a\",\"type\":\"status\\u0000ignored\"}", "invalid_request");
    rejects("{\"v\":1,\"id\":\"a\",\"type\":\"unknown\"}", "unknown_type");
    rejects("{\"v\":1,\"id\":\"a\",\"type\":\"auth.start\",\"resource\":\"both\"}", "invalid_resource");
    rejects("{\"v\":1,\"id\":\"a\",\"type\":\"auth.status\",\"resource\":1}", "invalid_resource");
    rejects("{\"v\":1,\"id\":\"a\",\"type\":\"auth.unlink\",\"confirm\":\"true\"}", "confirmation_required");
    const uint8_t embedded[] = "{\"v\":1,\"id\":\"a\",\"type\":\"status\"}\0ignored";
    uint8_t *out = NULL; ssize_t size;
    assert(recorder_control(1, embedded, sizeof(embedded) - 1, &out, &size, NULL) == ESP_OK);
    assert(strstr((char *)out, "invalid_request")); free(out);
    assert(recorder_control(1, embedded, 4097, &out, &size, NULL) == ESP_ERR_INVALID_SIZE);
    assert(out == NULL && size == 0);
    assert(recorder_control(1, embedded, 497, &out, &size, NULL) == ESP_ERR_INVALID_SIZE);
    uint8_t maximum[RECORDER_CONTROL_MAX_JSON];
    memset(maximum, ' ', sizeof(maximum));
    const char status_request[] = "{\"v\":1,\"id\":\"limit\",\"type\":\"status\"}";
    memcpy(maximum, status_request, strlen(status_request));
    assert(recorder_control(1, maximum, sizeof(maximum), &out, &size, NULL) == ESP_OK);
    assert(out && size > 0 && size <= RECORDER_CONTROL_MAX_JSON);
    free(out);
    cJSON *json = request("{\"v\":1,\"id\":\"request-1\",\"type\":\"status\"}");
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ok")));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(json, "id")->valuestring, "request-1"));
    cJSON_Delete(json);
    json = request("{\"v\":1,\"id\":\"a\",\"type\":\"auth.start\",\"resource\":\"graph\"}");
    assert(!pending_task);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(json, "state")->valuestring, "authorized"));
    cJSON_Delete(json);
    json = request("{\"v\":1,\"id\":\"a\",\"type\":\"setup.finish\"}");
    assert(setup_stopped && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ok")));
    cJSON_Delete(json);
}
static void refresh_and_failures(void)
{
    response_t rotate[] = {{"token",200,"{\"access_token\":\"TEST_ACCESS_ROTATED\",\"refresh_token\":\"TEST_REFRESH_ROTATED\",\"token_type\":\"Bearer\",\"expires_in\":3600}"}};
    char token[128];
    script(rotate, 1);
    assert(identity_access(AUTH_GRAPH, true, token, sizeof(token)) == ESP_OK);
    assert(!strcmp(token, "TEST_ACCESS_ROTATED") && !strcmp(stored[0], "TEST_REFRESH_ROTATED"));
    script(rotate, 1); storage_fail = true;
    assert(identity_access(AUTH_GRAPH, true, token, sizeof(token)) == ESP_FAIL);
    storage_fail = false;
    response_t revoked[] = {{"token",400,"{\"error\":\"invalid_grant\"}"}};
    script(revoked, 1);
    assert(identity_access(AUTH_GRAPH, true, token, sizeof(token)) != ESP_OK);
    assert(!stored[0][0] && identity_status(AUTH_GRAPH).state == AUTH_EXPIRED);
    assert(identity_access(AUTH_PROXY, false, token, sizeof(token)) == ESP_OK);
    assert(identity_unlink() == ESP_OK);
    response_t denied[] = {{"devicecode",200,DEVICE_REPLY},{"token",400,"{\"error\":\"authorization_declined\"}"}};
    script(denied, 2); assert(identity_start(AUTH_GRAPH) == ESP_OK); run_task();
    assert(identity_status(AUTH_GRAPH).state == AUTH_DENIED);
    response_t cancel[] = {{"devicecode",200,DEVICE_REPLY}};
    script(cancel, 1); cancel_at = 1000000;
    assert(identity_start(AUTH_GRAPH) == ESP_OK); run_task();
    assert(identity_status(AUTH_GRAPH).state == AUTH_IDLE && response_index == 1);
    assert(identity_unlink() == ESP_OK);
    assert(identity_access(AUTH_PROXY, false, token, sizeof(token)) == ESP_ERR_NOT_FOUND);
    script(NULL, 0);
    assert(identity_start(AUTH_GRAPH) == ESP_OK);
    identity_cancel(AUTH_GRAPH); run_task();
    assert(identity_status(AUTH_GRAPH).state == AUTH_IDLE && !response_index);
}
static void invalid_grant_responses(void)
{
    response_t empty_access[] = {{"devicecode",200,DEVICE_REPLY},
        {"token",200,"{\"access_token\":\"\",\"refresh_token\":\"TEST_REFRESH\",\"token_type\":\"Bearer\",\"expires_in\":3600}"}};
    script(empty_access, 2); assert(identity_start(AUTH_GRAPH) == ESP_OK); run_task();
    assert(identity_status(AUTH_GRAPH).state == AUTH_ERROR);
    assert(!stored[0][0]);
    assert(identity_unlink() == ESP_OK);
    response_t missing_refresh[] = {{"devicecode",200,DEVICE_REPLY},
        {"token",200,"{\"access_token\":\"TEST_ACCESS\",\"token_type\":\"Bearer\",\"expires_in\":3600}"}};
    script(missing_refresh, 2); assert(identity_start(AUTH_GRAPH) == ESP_OK); run_task();
    assert(identity_status(AUTH_GRAPH).state == AUTH_ERROR);
    assert(identity_unlink() == ESP_OK);
    response_t expired[] = {{"devicecode",200,"{\"device_code\":\"TEST_DEVICE\",\"user_code\":\"CODE\",\"verification_uri\":\"https://microsoft.com/devicelogin\",\"expires_in\":1,\"interval\":5}"}};
    script(expired, 1); assert(identity_start(AUTH_GRAPH) == ESP_OK); run_task();
    assert(identity_status(AUTH_GRAPH).state == AUTH_EXPIRED && response_index == 1);
}
int main(void)
{
    storage_allowed = false;
    assert(identity_init() == ESP_ERR_NOT_ALLOWED);
    storage_allowed = true;
    assert(identity_init() == ESP_OK);
    polling_and_resources();
    endpoint_validation();
    refresh_and_failures();
    invalid_grant_responses();
    assert(commits >= 4);
    puts("PASS: actual OAuth/refresh and BLE control sources against synthetic host mocks");
    return 0;
}
