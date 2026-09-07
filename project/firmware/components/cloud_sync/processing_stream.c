#include "processing_stream.h"
#include "processing_protocol.h"
#include "https_operation.h"
#include "identity.h"
#include "recorder_core.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    _Atomic bool connected, done, sent;
    _Atomic unsigned result, last_activity;
    bool started, terminal, assembling, frame_open, frame_fin;
    uint8_t frame_opcode;
    size_t used;
    int frame_length, frame_offset;
    char operation_id[129], json_id[129], text_id[129];
    uint32_t state;
    uint8_t message[4097];
} stream_t;

static unsigned clock_ms(void) { return (unsigned)(esp_timer_get_time() / 1000); }
static void finish(stream_t *s, processing_result_t result)
{
    atomic_store(&s->result, result);
    atomic_store(&s->done, true);
}
static const char *text(cJSON *json, const char *key)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}
static bool progress(cJSON *json)
{
    const char *name = text(json, "phase");
    static const struct { const char *name; sync_phase_t phase; } phases[] = {
        {"resolving", SYNC_RESOLVING}, {"downloading", SYNC_DOWNLOADING},
        {"validating", SYNC_VALIDATING}, {"transcribing", SYNC_TRANSCRIBING},
        {"saving", SYNC_SAVING}, {"verifying", SYNC_VERIFYING}
    };
    sync_phase_t phase = SYNC_PROCESSING;
    if (!name) return false;
    for (size_t i = 0; i < sizeof(phases) / sizeof(*phases); ++i)
        if (!strcmp(name, phases[i].name)) phase = phases[i].phase;
    if (phase == SYNC_PROCESSING) return false;
    cJSON *bytes = cJSON_GetObjectItemCaseSensitive(json, "completed_bytes");
    cJSON *total = cJSON_GetObjectItemCaseSensitive(json, "total_bytes");
    bool known = bytes || total;
    if (known && (!cJSON_IsNumber(bytes) || !cJSON_IsNumber(total) ||
        bytes->valuedouble < 0 || total->valuedouble < 0 ||
        bytes->valuedouble > total->valuedouble || total->valuedouble > UINT32_MAX ||
        bytes->valuedouble != (uint32_t)bytes->valuedouble ||
        total->valuedouble != (uint32_t)total->valuedouble)) return false;
    cloud_sync_processing_progress(phase, known,
        known ? (uint32_t)bytes->valuedouble : 0, known ? (uint32_t)total->valuedouble : 0);
    return true;
}
static void message(stream_t *s)
{
    cJSON *json = processing_parse_message(s->message, s->used);
    const char *type = text(json, "type");
    bool valid = processing_versioned(json) && type && atomic_load(&s->sent) && !s->terminal;
    if (valid && !strcmp(type, "started")) {
        char operation_id[129];
        valid = !s->started && processing_identifier(json, "operation_id", operation_id) &&
            !strcmp(operation_id, s->operation_id);
        if (valid) { s->started = true; cloud_sync_phase(SYNC_PROCESSING); }
    } else if (valid && (!strcmp(type, "progress") || !strcmp(type, "heartbeat"))) {
        valid = s->started && progress(json);
    } else if (valid && !strcmp(type, "result")) {
        char operation_id[129];
        const char *status = text(json, "status");
        valid = s->started && status && (!strcmp(status, "completed") || !strcmp(status, "already_completed")) &&
            processing_identifier(json, "operation_id", operation_id) &&
            !strcmp(operation_id, s->operation_id) &&
            processing_identifier(json, "json_item_id", s->json_id) &&
            processing_identifier(json, "text_item_id", s->text_id);
        if (valid) {
            s->state = PROCESS_COMPLETED; s->terminal = true; finish(s, PROCESS_OK);
        }
    } else if (valid && !strcmp(type, "error")) {
        cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
        const char *code = text(error, "code");
        if (code && !strcmp(code, "recording_too_long") &&
            cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(error, "retryable"))) {
            s->state = PROCESS_TOO_LONG; s->terminal = true; finish(s, PROCESS_OK);
        } else {
            processing_result_t result = processing_error_result(0, json);
            valid = result != PROCESS_BAD_RESPONSE;
            if (valid) { s->terminal = true; finish(s, result); }
        }
    } else valid = false;
    if (!valid) finish(s, PROCESS_BAD_RESPONSE);
    else atomic_store(&s->last_activity, clock_ms());
    cJSON_Delete(json);
}
static void received(stream_t *s, const esp_websocket_event_data_t *e)
{
    if (!e) { finish(s, PROCESS_BAD_RESPONSE); return; }
    if (e->op_code == 9 || e->op_code == 10) return;
    if (e->op_code == 8) {
        if (!atomic_load(&s->done)) finish(s, PROCESS_UNAVAILABLE);
        return;
    }
    if (atomic_load(&s->done) || e->op_code == 2 || e->data_len < 0 ||
        e->payload_offset < 0 || e->payload_len < 0 || e->payload_offset > e->payload_len ||
        e->data_len > e->payload_len - e->payload_offset || (e->data_len && !e->data_ptr)) {
        finish(s, PROCESS_BAD_RESPONSE); return;
    }
    if (e->payload_offset == 0) {
        if (s->frame_open || (e->op_code != 1 && e->op_code != 0) ||
            (e->op_code == 1 && s->assembling) || (e->op_code == 0 && !s->assembling)) {
            finish(s, PROCESS_BAD_RESPONSE); return;
        }
        if (e->op_code == 1) { s->used = 0; s->assembling = true; }
        s->frame_open = true; s->frame_offset = 0;
        s->frame_length = e->payload_len; s->frame_opcode = e->op_code; s->frame_fin = e->fin;
        if ((size_t)e->payload_len > 4096 - s->used) { finish(s, PROCESS_BAD_RESPONSE); return; }
    }
    if (!s->frame_open || e->payload_offset != s->frame_offset ||
        e->payload_len != s->frame_length || e->op_code != s->frame_opcode || e->fin != s->frame_fin ||
        (size_t)e->data_len > 4096 - s->used) { finish(s, PROCESS_BAD_RESPONSE); return; }
    if (e->data_len) memcpy(s->message + s->used, e->data_ptr, e->data_len);
    s->used += e->data_len; s->frame_offset += e->data_len;
    if (s->frame_offset == s->frame_length) {
        s->frame_open = false;
        if (e->fin) {
            message(s);
            s->assembling = false; s->used = 0;
        }
    }
}
static void event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    stream_t *s = arg;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        if (atomic_load(&s->connected)) { finish(s, PROCESS_BAD_RESPONSE); return; }
        atomic_store(&s->last_activity, clock_ms());
        atomic_store(&s->connected, true);
    } else if (id == WEBSOCKET_EVENT_DATA) received(s, data);
    else if ((id == WEBSOCKET_EVENT_ERROR || id == WEBSOCKET_EVENT_DISCONNECTED) &&
             !atomic_load(&s->done)) finish(s, PROCESS_UNAVAILABLE);
}
processing_result_t processing_stream_run(const char *body, const char *operation_id,
                                          processing_job_t *job, bool *uncertain)
{
    if (!uncertain) return PROCESS_BAD_RESPONSE;
    *uncertain = false;
    if (!body || !*body || strlen(body) > 4096 || !operation_id || !*operation_id ||
        strlen(operation_id) > 128 || !processing_job_valid(job)) return PROCESS_BAD_RESPONSE;
    if (cloud_sync_cancelled()) return PROCESS_CANCELLED;
    char url[320];
    if (!recorder_proxy_endpoint(CONFIG_RECORDER_PROXY_URL, "/v1/recordings/process-stream", url, sizeof(url)))
        return PROCESS_UNAVAILABLE;
    stream_t *s = calloc(1, sizeof(*s));
    char *token = calloc(1, 8193), *headers = calloc(1, 8256);
    if (!s || !token || !headers) { free(s); free(token); free(headers); return PROCESS_UNAVAILABLE; }
    strcpy(s->operation_id, operation_id); s->state = PROCESS_PENDING;
    esp_err_t err = identity_access(AUTH_PROXY, true, token, 8193);
    if (err == ESP_OK && cloud_sync_cancelled()) err = ESP_ERR_INVALID_STATE;
    esp_websocket_client_handle_t ws = NULL;
    if (err == ESP_OK) {
        snprintf(headers, 8256, "Authorization: Bearer %s\r\n", token);
        esp_websocket_client_config_t config = {.uri = url, .subprotocol = "recorder.processing.v1",
            .headers = headers, .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_reconnect = true,
            .buffer_size = 2048, .task_stack = 8192, .task_prio = 5, .network_timeout_ms = 10000,
            .ping_interval_sec = 10, .pingpong_timeout_sec = 20};
        ws = esp_websocket_client_init(&config);
        if (!ws) err = ESP_ERR_NO_MEM;
    }
    secret_zero(token, 8193); free(token);
    processing_result_t result = err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE ?
        PROCESS_CONSENT : PROCESS_UNAVAILABLE;
    if (err == ESP_OK) err = esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, event, s);
    bool running = false;
    cloud_sync_phase(SYNC_PROCESS_CONNECTING);
    if (err == ESP_OK) { err = esp_websocket_client_start(ws); running = err == ESP_OK; }
    unsigned connect_start = clock_ms();
    while (err == ESP_OK && !atomic_load(&s->done)) {
        if (cloud_sync_cancelled()) { result = PROCESS_CANCELLED; break; }
        if (atomic_load(&s->connected) && !atomic_load(&s->sent)) {
            atomic_store(&s->sent, true);
            int length = (int)strlen(body);
            if (esp_websocket_client_send_text(ws, body, length, pdMS_TO_TICKS(1000)) != length) {
                result = PROCESS_UNAVAILABLE; break;
            }
        }
        unsigned last = atomic_load(&s->connected) ? atomic_load(&s->last_activity) : connect_start;
        if (clock_ms() - last >= 30000) { result = PROCESS_WAITING; break; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (atomic_load(&s->done)) result = atomic_load(&s->result);
    if (ws && running) {
        if (cloud_sync_cancelled() && esp_websocket_client_is_connected(ws)) {
            const char *cancel = "{\"v\":1,\"type\":\"cancel\"}";
            esp_websocket_client_send_text(ws, cancel, strlen(cancel), pdMS_TO_TICKS(1000));
        }
        if (esp_websocket_client_stop(ws) != ESP_OK && esp_websocket_client_is_connected(ws))
            result = PROCESS_UNAVAILABLE;
    }
    if (ws && esp_websocket_client_destroy(ws) != ESP_OK) result = PROCESS_UNAVAILABLE;
    if (s->terminal && atomic_load(&s->result) == PROCESS_OK) result = PROCESS_OK;
    if (atomic_load(&s->done) && atomic_load(&s->result) == PROCESS_BAD_RESPONSE)
        result = PROCESS_BAD_RESPONSE;
    *uncertain = atomic_load(&s->sent) && result != PROCESS_OK && result != PROCESS_CANCELLED &&
        (!s->terminal || result == PROCESS_BAD_RESPONSE || result == PROCESS_WAITING ||
         result == PROCESS_UNAVAILABLE || result == PROCESS_TIMEOUT || result == PROCESS_BUSY);
    if (result == PROCESS_OK) {
        job->state = s->state;
        if (s->state == PROCESS_COMPLETED) {
            strcpy(job->json_item_id, s->json_id); strcpy(job->text_item_id, s->text_id);
        }
    }
    secret_zero(headers, 8256); free(headers); free(s);
    return cloud_sync_cancelled() && result != PROCESS_OK ? PROCESS_CANCELLED : result;
}
