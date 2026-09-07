#include "processing_client.h"
#include "https_operation.h"
#include "identity.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *string(cJSON *json, const char *key)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}
static bool identifier(cJSON *json, const char *key, char *out)
{
    const char *value = string(json, key);
    if (!value || !*value || strlen(value) > 128) return false;
    for (const char *p = value; *p; ++p)
        if ((unsigned char)*p < 32 || (unsigned char)*p > 126) return false;
    if (out) strcpy(out, value);
    return true;
}
static bool versioned(cJSON *json)
{
    if (!cJSON_IsObject(json)) return false;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(json, "v");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1) return false;
    for (cJSON *a = json->child; a; a = a->next)
        for (cJSON *b = a->next; b; b = b->next)
            if (!strcmp(a->string, b->string)) return false;
    return true;
}
static processing_result_t error_result(int status, cJSON *json)
{
    if (!versioned(json)) return PROCESS_BAD_RESPONSE;
    cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
    const char *code = string(error, "code");
    if (!code || !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(error, "retryable")))
        return PROCESS_BAD_RESPONSE;
    static const struct { int status; const char *code; processing_result_t result; } codes[] = {
        {400, "invalid_request", PROCESS_INVALID_REQUEST},
        {401, "authentication_required", PROCESS_AUTHENTICATION},
        {403, "consent_required", PROCESS_CONSENT},
        {403, "item_not_allowed", PROCESS_NOT_ALLOWED},
        {404, "source_not_found", PROCESS_NOT_FOUND},
        {409, "source_changed", PROCESS_SOURCE_CHANGED},
        {409, "processing_in_progress", PROCESS_WAITING},
        {409, "output_conflict", PROCESS_CONFLICT},
        {415, "unsupported_audio", PROCESS_UNSUPPORTED},
        {429, "busy", PROCESS_BUSY},
        {503, "temporarily_unavailable", PROCESS_UNAVAILABLE},
        {504, "processing_deadline", PROCESS_TIMEOUT}
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(*codes); ++i)
        if (status == codes[i].status && !strcmp(code, codes[i].code)) return codes[i].result;
    return PROCESS_BAD_RESPONSE;
}
static processing_result_t transport_result(esp_err_t err)
{
    if (cloud_sync_cancelled()) return PROCESS_CANCELLED;
    if (err == ESP_ERR_TIMEOUT) return PROCESS_TIMEOUT;
    if (err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_SIZE) return PROCESS_BAD_RESPONSE;
    return PROCESS_UNAVAILABLE;
}
static processing_result_t identity_result(esp_err_t err)
{
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE) return PROCESS_CONSENT;
    return transport_result(err);
}
static processing_result_t warm(void)
{
    char url[320];
    if (!recorder_proxy_endpoint(CONFIG_RECORDER_PROXY_URL, "/readyz", url, sizeof(url)))
        return PROCESS_UNAVAILABLE;
    cloud_sync_phase(SYNC_WARMING);
    int64_t deadline = esp_timer_get_time() + 120000000LL;
    while (!cloud_sync_cancelled()) {
        int64_t remaining = (deadline - esp_timer_get_time()) / 1000;
        if (remaining <= 0) return PROCESS_TIMEOUT;
        https_operation_t operation = {.timeout_ms = remaining < 10000 ? (unsigned)remaining : 10000,
            .response_limit = 4096, .cancelled = cloud_sync_cancelled};
        int status = 0; cJSON *json = NULL; unsigned retry_after = 0;
        esp_err_t err = https_json_operation(url, HTTP_METHOD_GET, NULL, NULL, NULL,
            &status, &json, &retry_after, &operation);
        bool ready = err == ESP_OK && status == 200 && cJSON_IsObject(json);
        cJSON_Delete(json);
        if (cloud_sync_cancelled()) return PROCESS_CANCELLED;
        if (ready) return PROCESS_OK;
        if (status >= 300 && status < 500 && status != 429) return PROCESS_UNAVAILABLE;
        unsigned delay = retry_after ? retry_after : 2;
        if ((int64_t)delay * 1000000 >= deadline - esp_timer_get_time()) return PROCESS_TIMEOUT;
        for (unsigned i = 0; i < delay * 10 && !cloud_sync_cancelled(); ++i)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
    return PROCESS_CANCELLED;
}
static char *request_body(const processing_job_t *job)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    bool ok = cJSON_AddNumberToObject(json, "v", 1) &&
        cJSON_AddStringToObject(json, "drive_id", job->drive_id) &&
        cJSON_AddStringToObject(json, "item_id", job->item_id) &&
        cJSON_AddStringToObject(json, "source_sha1", job->source_sha1) &&
        cJSON_AddNumberToObject(json, "source_size", job->source_size);
    if (ok && job->recorded_at[0])
        ok = cJSON_AddStringToObject(json, "recorded_at", job->recorded_at) != NULL;
    char *body = ok ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (body && strlen(body) > 4096) { free(body); return NULL; }
    return body;
}
static processing_result_t save_result(processing_job_t *job, uint32_t state)
{
    job->state = state;
    if (processing_outbox_save(job) != ESP_OK) {
        job->state = PROCESS_PENDING;
        return PROCESS_STORAGE_ERROR;
    }
    cloud_sync_phase(state == PROCESS_COMPLETED ? SYNC_TRANSCRIBED : SYNC_SKIPPED);
    return PROCESS_OK;
}
processing_result_t processing_run(processing_job_t *job)
{
    if (!processing_job_valid(job)) return PROCESS_BAD_RESPONSE;
    if (job->state != PROCESS_PENDING) return PROCESS_OK;
    if (cloud_sync_cancelled()) return PROCESS_CANCELLED;
    if (job->retry_not_before > (int64_t)time(NULL)) return PROCESS_BUSY;
    char *token = calloc(1, 8193), *body = request_body(job);
    if (!token || !body) { free(token); free(body); return PROCESS_UNAVAILABLE; }
    esp_err_t err = identity_access(AUTH_PROXY, false, token, 8193);
    processing_result_t result = err == ESP_OK ? warm() : identity_result(err);
    if (result != PROCESS_OK) {
        secret_zero(token, 8193); free(token); free(body);
        return result;
    }
    /* Status first also reconciles a process whose HTTP response was lost.
       At most one Speech-triggering POST is made per recording per Sync. */
    for (unsigned step = 0; err == ESP_OK && step < 2; ++step) {
        if (cloud_sync_cancelled()) { result = PROCESS_CANCELLED; break; }
        /* Warming can consume the cached token's remaining validity. */
        err = identity_access(AUTH_PROXY, false, token, 8193);
        if (err != ESP_OK) { result = identity_result(err); break; }
        char url[320];
        if (!recorder_proxy_endpoint(CONFIG_RECORDER_PROXY_URL,
            step ? "/v1/recordings/process" : "/v1/recordings/status", url, sizeof(url))) {
            result = PROCESS_UNAVAILABLE; break;
        }
        cloud_sync_phase(step ? SYNC_TRANSCRIBING : SYNC_CHECKING);
        https_operation_t operation = {.timeout_ms = step ? 210000 : 10000,
            .response_limit = 4096, .cancelled = cloud_sync_cancelled};
        int status = 0; cJSON *json = NULL; unsigned retry_after = 0;
        err = https_json_operation(url, HTTP_METHOD_POST, token, "application/json", body,
            &status, &json, &retry_after, &operation);
        if (err != ESP_OK) {
            result = transport_result(err); cJSON_Delete(json); break;
        }
        if (status == 413 && versioned(json) &&
            cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(
                cJSON_GetObjectItemCaseSensitive(json, "error"), "retryable")) &&
            string(cJSON_GetObjectItemCaseSensitive(json, "error"), "code") &&
            !strcmp(string(cJSON_GetObjectItemCaseSensitive(json, "error"), "code"), "recording_too_long")) {
            result = save_result(job, PROCESS_TOO_LONG);
            cJSON_Delete(json); break;
        }
        if (status != 200) {
            result = error_result(status, json);
            if ((status == 429 || status == 503) && retry_after) {
                job->retry_not_before = (int64_t)time(NULL) + retry_after;
                if (processing_outbox_save(job) != ESP_OK) result = PROCESS_STORAGE_ERROR;
            }
            cJSON_Delete(json); break;
        }
        const char *state = string(json, "status");
        if (!versioned(json) || !state || !identifier(json, "operation_id", NULL)) {
            result = PROCESS_BAD_RESPONSE; cJSON_Delete(json); break;
        }
        if (!strcmp(state, "completed") || !strcmp(state, "already_completed")) {
            if (identifier(json, "json_item_id", job->json_item_id) &&
                identifier(json, "text_item_id", job->text_item_id))
                result = save_result(job, PROCESS_COMPLETED);
            else result = PROCESS_BAD_RESPONSE;
            cJSON_Delete(json); break;
        }
        bool start = !step && (!strcmp(state, "not_started") || !strcmp(state, "retry_required"));
        result = !strcmp(state, "processing") ? PROCESS_WAITING : PROCESS_BAD_RESPONSE;
        cJSON_Delete(json);
        if (!start) break;
    }
    secret_zero(token, 8193); free(token); free(body);
    return result;
}
