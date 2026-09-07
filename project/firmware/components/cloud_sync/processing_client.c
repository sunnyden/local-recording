#include "processing_client.h"
#include "processing_protocol.h"
#include "processing_stream.h"
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
static processing_result_t status_once(processing_job_t *job, const char *body, char *token,
                                       char operation_id[129], bool *may_start)
{
    *may_start = false;
    if (cloud_sync_cancelled()) return PROCESS_CANCELLED;
    cloud_sync_phase(SYNC_CHECKING);
    esp_err_t err = identity_access(AUTH_PROXY, false, token, 8193);
    if (err != ESP_OK) return identity_result(err);
    char url[320];
    if (!recorder_proxy_endpoint(CONFIG_RECORDER_PROXY_URL, "/v1/recordings/status", url, sizeof(url)))
        return PROCESS_UNAVAILABLE;
    https_operation_t operation = {.timeout_ms = 45000, .response_limit = 4096,
        .cancelled = cloud_sync_cancelled};
    int status = 0; cJSON *json = NULL; unsigned retry_after = 0;
    err = https_json_operation(url, HTTP_METHOD_POST, token, "application/json", body,
        &status, &json, &retry_after, &operation);
    processing_result_t result;
    if (err != ESP_OK) result = err == ESP_ERR_TIMEOUT ? PROCESS_WAITING : transport_result(err);
    else if (status == 413 && processing_versioned(json) &&
        cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(json, "error"), "retryable")) &&
        string(cJSON_GetObjectItemCaseSensitive(json, "error"), "code") &&
        !strcmp(string(cJSON_GetObjectItemCaseSensitive(json, "error"), "code"), "recording_too_long"))
        result = save_result(job, PROCESS_TOO_LONG);
    else if (status != 200) {
        result = processing_error_result(status, json);
        if ((status == 429 || status == 503) && retry_after) {
            job->retry_not_before = (int64_t)time(NULL) + retry_after;
            if (processing_outbox_save(job) != ESP_OK) result = PROCESS_STORAGE_ERROR;
        }
    } else {
        const char *state = string(json, "status");
        char returned_id[129];
        if (!processing_versioned(json) || !state || !processing_identifier(json, "operation_id", returned_id) ||
            (operation_id[0] && strcmp(operation_id, returned_id))) result = PROCESS_BAD_RESPONSE;
        else {
            strcpy(operation_id, returned_id);
            if (!strcmp(state, "completed") || !strcmp(state, "already_completed")) {
                if (processing_identifier(json, "json_item_id", job->json_item_id) &&
                    processing_identifier(json, "text_item_id", job->text_item_id))
                    result = save_result(job, PROCESS_COMPLETED);
                else result = PROCESS_BAD_RESPONSE;
            } else {
                *may_start = !strcmp(state, "not_started") || !strcmp(state, "retry_required");
                result = *may_start || !strcmp(state, "processing") ? PROCESS_WAITING : PROCESS_BAD_RESPONSE;
            }
        }
    }
    cJSON_Delete(json);
    return result;
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
    char operation_id[129] = "";
    bool may_start = false, uncertain = false;
    result = status_once(job, body, token, operation_id, &may_start);
    if (may_start && !cloud_sync_cancelled()) {
        result = processing_stream_run(body, operation_id, job, &uncertain);
        if (result == PROCESS_OK) result = save_result(job, job->state);
    }
    /* No second start or HTTP fallback: an uncertain stream only queries the
       same operation. A still-active result stays pending, not "upload failed". */
    if (uncertain || (result == PROCESS_WAITING && !may_start)) {
        for (unsigned attempt = 0; attempt < 3 && !cloud_sync_cancelled(); ++attempt) {
            result = status_once(job, body, token, operation_id, &may_start);
            if (result != PROCESS_WAITING || may_start) break;
            for (unsigned i = 0; i < 20 && !cloud_sync_cancelled(); ++i)
                vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    secret_zero(token, 8193); free(token); free(body);
    return result;
}
