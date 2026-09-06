#include "identity.h"
#include "recorder_network.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define TOKEN_SIZE 8193
#define AUTHORITY "https://login.microsoftonline.com/consumers/oauth2/v2.0/"
typedef struct {
    auth_status_t public;
    char access[TOKEN_SIZE];
    char refresh[TOKEN_SIZE];
    int64_t expiry;
    int64_t grant_deadline;
    bool running;
    _Atomic bool cancel;
} resource_t;
static resource_t *resources;
static SemaphoreHandle_t lock;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *refresh_key[] = {"graph_refresh", "proxy_refresh"};
static const char *scope(auth_resource_t r)
{
    return r == AUTH_GRAPH ? "https://graph.microsoft.com/Files.ReadWrite offline_access" :
                            CONFIG_RECORDER_PROXY_SCOPE " offline_access";
}
const char *identity_state_name(auth_state_t state)
{
    static const char *names[] = {"idle","pending","authorized","denied","expired","error"};
    return (unsigned)state < 6 ? names[state] : "error";
}
static void set_state(resource_t *r, auth_state_t state)
{
    portENTER_CRITICAL(&status_lock);
    r->public.state = state;
    if (state != AUTH_PENDING) {
        secret_zero(r->public.user_code, sizeof(r->public.user_code));
        r->public.verification_uri[0] = 0;
        r->public.expires_in = 0;
    }
    portEXIT_CRITICAL(&status_lock);
}
auth_status_t identity_status(auth_resource_t resource)
{
    auth_status_t result = {.state = AUTH_IDLE};
    if (!resources || (unsigned)resource > AUTH_PROXY) return result;
    portENTER_CRITICAL(&status_lock);
    result = resources[resource].public;
    if (result.state == AUTH_PENDING && resources[resource].grant_deadline) {
        int64_t remaining = resources[resource].grant_deadline - esp_timer_get_time();
        result.expires_in = remaining > 0 ? (unsigned)((remaining + 999999) / 1000000) : 0;
    }
    portEXIT_CRITICAL(&status_lock);
    return result;
}
static const char *json_string(cJSON *json, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}
static void destroy_json(cJSON *json)
{
    for (cJSON *item = json ? json->child : NULL; item; item = item->next)
        if (item->valuestring) secret_zero(item->valuestring, strlen(item->valuestring));
    cJSON_Delete(json);
}
static esp_err_t save_tokens(auth_resource_t resource, cJSON *json)
{
    resource_t *r = &resources[resource];
    const char *access = json_string(json, "access_token");
    const char *refresh = json_string(json, "refresh_token");
    const char *type = json_string(json, "token_type");
    cJSON *expiry = cJSON_GetObjectItemCaseSensitive(json, "expires_in");
    if (!access || !access[0] || strlen(access) >= TOKEN_SIZE || !type || strcasecmp(type, "Bearer") ||
        !cJSON_IsNumber(expiry) || expiry->valuedouble < 120 || expiry->valuedouble > 86400 ||
        expiry->valuedouble != (double)(int64_t)expiry->valuedouble ||
        (refresh && (!refresh[0] || strlen(refresh) >= TOKEN_SIZE)) ||
        (!refresh && !r->refresh[0])) return ESP_ERR_INVALID_RESPONSE;
    if (refresh) {
        esp_err_t err = credential_write(refresh_key[resource], refresh);
        if (err != ESP_OK) return err;
        strcpy(r->refresh, refresh);
    }
    strcpy(r->access, access);
    r->expiry = esp_timer_get_time() + ((int64_t)expiry->valuedouble - 60) * 1000000;
    set_state(r, AUTH_AUTHORIZED);
    return ESP_OK;
}
static esp_err_t token_request(auth_resource_t resource, const char *grant,
    const char *field, const char *value, int *status, cJSON **json)
{
    char *encoded = form_encode(value), *encoded_scope = form_encode(scope(resource));
    if (!encoded || !encoded_scope) { free(encoded); free(encoded_scope); return ESP_ERR_NO_MEM; }
    size_t size = strlen(encoded) + strlen(encoded_scope) + strlen(grant) + strlen(field) +
                  strlen(CONFIG_RECORDER_CLIENT_ID) + 128;
    char *body = malloc(size);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (body) {
        snprintf(body, size, "client_id=%s&grant_type=%s&%s=%s&scope=%s",
            CONFIG_RECORDER_CLIENT_ID, grant, field, encoded, encoded_scope);
        err = https_json(AUTHORITY "token", HTTP_METHOD_POST, NULL,
            "application/x-www-form-urlencoded", body, status, json);
        secret_zero(body, size); free(body);
    }
    secret_zero(encoded, strlen(encoded)); free(encoded); free(encoded_scope);
    return err;
}
static bool delay_cancel(resource_t *r, unsigned seconds)
{
    for (unsigned i = 0; i < seconds * 10; ++i) {
        if (atomic_load(&r->cancel)) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return !atomic_load(&r->cancel);
}
static void grant_worker(void *arg)
{
    auth_resource_t resource = (auth_resource_t)(uintptr_t)arg;
    resource_t *r = &resources[resource];
    if (atomic_load(&r->cancel)) {
        set_state(r, AUTH_IDLE);
        portENTER_CRITICAL(&status_lock); r->running = false; portEXIT_CRITICAL(&status_lock);
        vTaskDelete(NULL);
        return;
    }
    char *encoded = form_encode(scope(resource)), *device_code = NULL;
    cJSON *json = NULL;
    int status = 0;
    esp_err_t err = ESP_ERR_NO_MEM;
    if (encoded) {
        size_t n = strlen(encoded) + strlen(CONFIG_RECORDER_CLIENT_ID) + 64;
        char *body = malloc(n);
        if (body) {
            snprintf(body, n, "client_id=%s&scope=%s", CONFIG_RECORDER_CLIENT_ID, encoded);
            err = https_json(AUTHORITY "devicecode", HTTP_METHOD_POST, NULL,
                "application/x-www-form-urlencoded", body, &status, &json);
            free(body);
        }
        free(encoded);
    }
    unsigned interval = 5;
    int64_t deadline = 0;
    if (err == ESP_OK && status == 200) {
        const char *dc = json_string(json, "device_code"), *uc = json_string(json, "user_code");
        const char *uri = json_string(json, "verification_uri");
        cJSON *expires = cJSON_GetObjectItemCaseSensitive(json, "expires_in");
        cJSON *poll = cJSON_GetObjectItemCaseSensitive(json, "interval");
        if (dc && strlen(dc) <= 2048 && uc && strlen(uc) < 32 && uri &&
            (!strcmp(uri, "https://microsoft.com/devicelogin") ||
             !strcmp(uri, "https://www.microsoft.com/link")) &&
            cJSON_IsNumber(expires) && expires->valueint > 0 && expires->valueint <= 1800) {
            device_code = strdup(dc);
            if (cJSON_IsNumber(poll) && poll->valueint > 0 && poll->valueint <= 60)
                interval = poll->valueint;
            portENTER_CRITICAL(&status_lock);
            strcpy(r->public.user_code, uc);
            strcpy(r->public.verification_uri, uri);
            r->public.expires_in = expires->valueint;
            r->grant_deadline = esp_timer_get_time() + (int64_t)expires->valueint * 1000000;
            portEXIT_CRITICAL(&status_lock);
            deadline = esp_timer_get_time() + (int64_t)expires->valueint * 1000000;
        }
    }
    destroy_json(json);
    if (!device_code) set_state(r, AUTH_ERROR);
    while (device_code && delay_cancel(r, interval)) {
        if (esp_timer_get_time() >= deadline) { set_state(r, AUTH_EXPIRED); break; }
        json = NULL;
        xSemaphoreTake(lock, portMAX_DELAY);
        err = token_request(resource, "urn:ietf:params:oauth:grant-type:device_code",
                            "device_code", device_code, &status, &json);
        if (err == ESP_OK && status == 200 && !atomic_load(&r->cancel))
            err = save_tokens(resource, json);
        xSemaphoreGive(lock);
        const char *error = json_string(json, "error");
        bool again = err == ESP_OK && status == 400 && error &&
                     (!strcmp(error, "authorization_pending") || !strcmp(error, "slow_down"));
        if (again && !strcmp(error, "slow_down")) interval += 5;
        if (!again && !(err == ESP_OK && status == 200)) {
            set_state(r, error && !strcmp(error, "authorization_declined") ? AUTH_DENIED :
                error && !strcmp(error, "expired_token") ? AUTH_EXPIRED : AUTH_ERROR);
        }
        destroy_json(json);
        if (!again) break;
    }
    if (atomic_load(&r->cancel)) set_state(r, AUTH_IDLE);
    if (device_code) { secret_zero(device_code, strlen(device_code)); free(device_code); }
    portENTER_CRITICAL(&status_lock);
    r->running = false;
    portEXIT_CRITICAL(&status_lock);
    vTaskDelete(NULL);
}
esp_err_t identity_init(void)
{
    if (resources) return ESP_OK;
    if (!credential_storage_allowed()) return ESP_ERR_NOT_ALLOWED;
    resources = calloc(2, sizeof(resource_t));
    lock = xSemaphoreCreateMutex();
    if (!resources || !lock) { free(resources); resources = NULL; return ESP_ERR_NO_MEM; }
    for (unsigned i = 0; i < 2; ++i)
        credential_read(refresh_key[i], resources[i].refresh, TOKEN_SIZE);
    return ESP_OK;
}
esp_err_t identity_start(auth_resource_t resource)
{
    if (!resources || (unsigned)resource > AUTH_PROXY || !CONFIG_RECORDER_CLIENT_ID[0] ||
        (resource == AUTH_PROXY && !CONFIG_RECORDER_PROXY_SCOPE[0]) ||
        !recorder_network_ready() || !recorder_time_valid()) return ESP_ERR_INVALID_STATE;
    resource_t *r = &resources[resource];
    portENTER_CRITICAL(&status_lock);
    bool running = resources[0].running || resources[1].running;
    if (!running) {
        r->running = true; r->public.state = AUTH_PENDING; r->grant_deadline = 0;
        r->public.user_code[0] = 0; r->public.verification_uri[0] = 0; r->public.expires_in = 0;
    }
    portEXIT_CRITICAL(&status_lock);
    if (running) return ESP_ERR_INVALID_STATE;
    atomic_store(&r->cancel, false);
    if (xTaskCreate(grant_worker, "device_grant", 8192, (void *)(uintptr_t)resource, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&status_lock); r->running = false; portEXIT_CRITICAL(&status_lock);
        set_state(r, AUTH_ERROR); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void identity_cancel(auth_resource_t resource)
{
    if (resources && (unsigned)resource <= AUTH_PROXY) atomic_store(&resources[resource].cancel, true);
}
esp_err_t identity_access(auth_resource_t resource, bool force, char *token, size_t capacity)
{
    if (!resources || (unsigned)resource > AUTH_PROXY || !token) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(12000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    resource_t *r = &resources[resource];
    esp_err_t err = ESP_OK;
    if (force || !r->access[0] || esp_timer_get_time() >= r->expiry) {
        if (!r->refresh[0]) err = ESP_ERR_NOT_FOUND;
        else {
            cJSON *json = NULL; int status = 0;
            err = token_request(resource, "refresh_token", "refresh_token", r->refresh, &status, &json);
            if (err == ESP_OK && status == 200) err = save_tokens(resource, json);
            else if (err == ESP_OK) {
                const char *code = json_string(json, "error");
                if (code && !strcmp(code, "invalid_grant")) {
                    secret_zero(r->refresh, sizeof(r->refresh));
                    secret_zero(r->access, sizeof(r->access));
                    credential_write(refresh_key[resource], "");
                    set_state(r, AUTH_EXPIRED);
                }
                err = ESP_ERR_INVALID_STATE;
            }
            destroy_json(json);
        }
    }
    if (err == ESP_OK && strlen(r->access) >= capacity) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) strcpy(token, r->access);
    xSemaphoreGive(lock);
    return err;
}
esp_err_t identity_unlink(void)
{
    if (!resources) return ESP_ERR_INVALID_STATE;
    identity_cancel(AUTH_GRAPH); identity_cancel(AUTH_PROXY);
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = credential_erase_all();
    if (err == ESP_OK) {
        for (unsigned i = 0; i < 2; ++i) {
            secret_zero(resources[i].access, TOKEN_SIZE);
            secret_zero(resources[i].refresh, TOKEN_SIZE);
            resources[i].expiry = 0;
            set_state(&resources[i], AUTH_IDLE);
        }
    }
    xSemaphoreGive(lock);
    return err;
}
