#include "processing_stream.h"
#include "identity.h"
#include "recorder_network.h"
#include "esp_websocket_client.h"
#include "freertos/task.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum test_case { SLOW, STALL, PINGS, CANCEL, CONNECT_STALL, SHORT_SEND, BINARY,
    DUP_STARTED, DUP_RESULT, WRONG_ID, MISSING_ID, FRAG_BAD, NUL_MESSAGE, DUP_FIELDS,
    ERROR_MESSAGE, DUP_ERROR_FIELDS, FRACTION, UNKNOWN_PHASE, EARLY_RESULT,
    EARLY_PROGRESS, OVERSIZE, DEEP, GARBAGE, WRONG_STARTED, RAW_NUL, OVERSIZED_ID, RETRYABLE_ERROR };
struct host_websocket { host_ws_callback_t callback; void *arg; bool connected; };
static enum test_case test;
static struct host_websocket *socket;
static uint64_t now;
static bool cancelled, inside_callback;
static unsigned starts, sends, cancels, heartbeats, refreshes;
static sync_phase_t phase;
static bool bytes_known, observed_progress;
static uint32_t completed_bytes, total_bytes;
static const char *started = "{\"v\":1,\"type\":\"started\",\"operation_id\":\"operation\"}";
static const char *result = "{\"v\":1,\"type\":\"result\",\"operation_id\":\"operation\","
    "\"status\":\"completed\",\"json_item_id\":\"json\",\"text_item_id\":\"text\"}";

bool processing_job_valid(const processing_job_t *job) { return job && job->source_size == 32044; }
bool cloud_sync_cancelled(void) { return cancelled; }
void cloud_sync_phase(sync_phase_t next) { phase = next; bytes_known = false; }
void cloud_sync_processing_progress(sync_phase_t next, bool known, uint32_t bytes, uint32_t total)
{
    phase = next; bytes_known = known; completed_bytes = bytes; total_bytes = total;
    if (known) observed_progress = true;
}
int64_t esp_timer_get_time(void) { return (int64_t)now; }
int esp_crt_bundle_attach(void *arg) { (void)arg; return 0; }
void secret_zero(void *p, size_t n) { memset(p, 0, n); }
esp_err_t identity_access(auth_resource_t resource, bool force, char *token, size_t capacity)
{
    assert(resource == AUTH_PROXY && force && capacity == 8193);
    ++refreshes; strcpy(token, "FRESH_API_B"); return ESP_OK;
}
static void data(uint8_t opcode, bool fin, const char *bytes, int length, int total, int offset)
{
    esp_websocket_event_data_t event = {.op_code = opcode, .fin = fin, .data_ptr = bytes,
        .data_len = length, .payload_len = total, .payload_offset = offset};
    inside_callback = true;
    socket->callback(socket->arg, "WS", WEBSOCKET_EVENT_DATA, &event);
    inside_callback = false;
}
static void text(const char *value) { data(1, true, value, strlen(value), strlen(value), 0); }
void vTaskDelay(TickType_t ticks)
{
    uint64_t before = now;
    now += (uint64_t)ticks * 1000;
    if (test == CANCEL && now >= 2000000) cancelled = true;
    if (socket && sends && before / 10000000 != now / 10000000) {
        if (test == SLOW) {
            ++heartbeats;
            if (heartbeats < 25) text("{\"v\":1,\"type\":\"heartbeat\",\"phase\":\"transcribing\"}");
            else text(result);
        } else if (test == PINGS) data(9, true, "", 0, 0, 0);
    }
}
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *config)
{
    assert(!strcmp(config->uri, "wss://proxy.invalid/v1/recordings/process-stream"));
    assert(!strcmp(config->subprotocol, "recorder.processing.v1"));
    assert(!strcmp(config->headers, "Authorization: Bearer FRESH_API_B\r\n"));
    assert(config->crt_bundle_attach && config->disable_auto_reconnect);
    assert(config->buffer_size <= 4096 && config->network_timeout_ms <= 30000);
    socket = calloc(1, sizeof(*socket)); return socket;
}
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t ws, int id,
    host_ws_callback_t callback, void *arg)
{
    assert(id == WEBSOCKET_EVENT_ANY); ws->callback = callback; ws->arg = arg; return ESP_OK;
}
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t ws)
{
    ++starts;
    if (test != CONNECT_STALL) {
        ws->connected = true;
        ws->callback(ws->arg, "WS", WEBSOCKET_EVENT_CONNECTED, NULL);
    }
    return ESP_OK;
}
int esp_websocket_client_send_text(esp_websocket_client_handle_t ws, const char *body, int length, TickType_t ticks)
{
    (void)ws; (void)ticks;
    assert(!inside_callback);
    if (!strcmp(body, "{\"v\":1,\"type\":\"cancel\"}")) { ++cancels; return length; }
    ++sends;
    assert(!strcmp(body, "{\"v\":1}"));
    if (test == SHORT_SEND) return length - 1;
    if (test == EARLY_RESULT) { text(result); return length; }
    if (test == EARLY_PROGRESS) {
        text("{\"v\":1,\"type\":\"progress\",\"phase\":\"resolving\"}"); return length;
    }
    if (test == WRONG_STARTED) {
        text("{\"v\":1,\"type\":\"started\",\"operation_id\":\"other\"}"); return length;
    }
    text(started);
    switch (test) {
    case SLOW: {
        text("{\"v\":1,\"type\":\"progress\",\"phase\":\"resolving\"}");
        assert(phase == SYNC_RESOLVING && !bytes_known);
        text("{\"v\":1,\"type\":\"progress\",\"phase\":\"validating\"}");
        assert(phase == SYNC_VALIDATING && !bytes_known);
        text("{\"v\":1,\"type\":\"progress\",\"phase\":\"saving\",\"completed_bytes\":0,\"total_bytes\":0}");
        assert(phase == SYNC_SAVING && bytes_known && !total_bytes);
        text("{\"v\":1,\"type\":\"progress\",\"phase\":\"verifying\"}");
        assert(phase == SYNC_VERIFYING && !bytes_known);
        const char *progress = "{\"v\":1,\"type\":\"progress\",\"phase\":\"downloading\","
            "\"completed_bytes\":12,\"total_bytes\":100}";
        int n = strlen(progress);
        data(1, false, progress, 5, 20, 0);
        data(1, false, progress + 5, 15, 20, 5);
        data(0, true, progress + 20, 8, n - 20, 0);
        data(0, true, progress + 28, n - 28, n - 20, 8);
        assert(phase == SYNC_DOWNLOADING && bytes_known && completed_bytes == 12 && total_bytes == 100);
        text("{\"v\":1,\"type\":\"progress\",\"phase\":\"transcribing\"}");
        assert(phase == SYNC_TRANSCRIBING && !bytes_known);
        break;
    }
    case STALL: case PINGS: case CANCEL: break;
    case BINARY: data(2, true, "audio", 5, 5, 0); break;
    case DUP_STARTED: text(started); break;
    case DUP_RESULT: text(result); text(result); break;
    case WRONG_ID:
        text("{\"v\":1,\"type\":\"result\",\"operation_id\":\"other\",\"status\":\"completed\","
            "\"json_item_id\":\"j\",\"text_item_id\":\"t\"}"); break;
    case MISSING_ID:
        text("{\"v\":1,\"type\":\"result\",\"operation_id\":\"operation\",\"status\":\"completed\","
            "\"json_item_id\":\"j\"}"); break;
    case FRAG_BAD: data(1, false, "{", 1, 4, 0); data(0, true, "}", 1, 1, 0); break;
    case NUL_MESSAGE: text("{\"v\":1,\"type\":\"heartbeat\\u0000\",\"phase\":\"transcribing\"}"); break;
    case RAW_NUL: data(1, true, "{\"v\":1}\0tail", 12, 12, 0); break;
    case DUP_FIELDS: text("{\"v\":1,\"type\":\"heartbeat\",\"phase\":\"saving\",\"phase\":\"transcribing\"}"); break;
    case ERROR_MESSAGE:
        text("{\"v\":1,\"type\":\"error\",\"error\":{\"code\":\"source_not_found\",\"retryable\":false}}"); break;
    case DUP_ERROR_FIELDS:
        text("{\"v\":1,\"type\":\"error\",\"error\":{\"code\":\"source_not_found\","
            "\"code\":\"consent_required\",\"retryable\":false}}"); break;
    case FRACTION:
        text("{\"v\":1,\"type\":\"progress\",\"phase\":\"downloading\",\"completed_bytes\":0.5,\"total_bytes\":100}"); break;
    case UNKNOWN_PHASE: text("{\"v\":1,\"type\":\"heartbeat\",\"phase\":\"guessing\"}"); break;
    case OVERSIZE: data(1, true, "{", 1, 4097, 0); break;
    case DEEP: text("{\"v\":1,\"type\":\"heartbeat\",\"x\":[[[[[[[[[]]]]]]]]],\"phase\":\"saving\"}"); break;
    case GARBAGE: text("{\"v\":1,\"type\":\"heartbeat\",\"phase\":\"saving\"}garbage"); break;
    case OVERSIZED_ID: {
        char id[130], json[512];
        memset(id, 'x', 129); id[129] = 0;
        snprintf(json, sizeof(json), "{\"v\":1,\"type\":\"result\",\"operation_id\":\"operation\","
            "\"status\":\"completed\",\"json_item_id\":\"json\",\"text_item_id\":\"%s\"}", id);
        text(json); break;
    }
    case RETRYABLE_ERROR:
        text("{\"v\":1,\"type\":\"error\",\"error\":{\"code\":\"temporarily_unavailable\",\"retryable\":true}}"); break;
    default: assert(!"unexpected test case");
    }
    return length;
}
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t ws) { return ws->connected; }
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t ws)
{
    ws->connected = false;
    ws->callback(ws->arg, "WS", WEBSOCKET_EVENT_DISCONNECTED, NULL);
    return ESP_OK;
}
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t ws)
{ assert(!ws->connected); free(ws); socket = NULL; return ESP_OK; }
int main(void)
{
    for (test = SLOW; test <= RETRYABLE_ERROR; ++test) {
        now = 0; cancelled = false; starts = sends = cancels = heartbeats = refreshes = 0;
        observed_progress = false;
        processing_job_t job = {.source_size = 32044};
        bool uncertain = false;
        processing_result_t outcome = processing_stream_run("{\"v\":1}", "operation", &job, &uncertain);
        assert(starts == 1 && refreshes == 1 && !socket);
        if (test == SLOW) {
            assert(outcome == PROCESS_OK && job.state == PROCESS_COMPLETED && !uncertain);
            assert(now >= 250000000 && heartbeats == 25 && observed_progress);
            assert(!strcmp(job.json_item_id, "json") && !strcmp(job.text_item_id, "text"));
        } else if (test == CANCEL) {
            assert(outcome == PROCESS_CANCELLED && cancels == 1 && !uncertain);
        } else if (test == CONNECT_STALL) {
            assert(outcome == PROCESS_WAITING && !sends && !uncertain && now <= 30020000);
        } else if (test == STALL || test == PINGS) {
            assert(outcome == PROCESS_WAITING && uncertain && now <= 30020000);
        } else if (test == SHORT_SEND) assert(outcome == PROCESS_UNAVAILABLE && uncertain);
        else if (test == ERROR_MESSAGE) assert(outcome == PROCESS_NOT_FOUND && !uncertain);
        else if (test == RETRYABLE_ERROR) assert(outcome == PROCESS_UNAVAILABLE && uncertain);
        else assert(outcome == PROCESS_BAD_RESPONSE && uncertain);
        if (test != SLOW) assert(job.state == PROCESS_PENDING);
    }
    puts("PASS: actual separate processing WebSocket, 250s heartbeats, fragments, cancellation, stalls and strict terminal validation");
}
