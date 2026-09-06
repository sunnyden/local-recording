#include "voice_client.h"
#include "audio_io.h"
#include "recorder_core.h"
#include "recorder_network.h"
#include "identity.h"
#include "esp_websocket_client.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct host_queue { unsigned size, capacity, count, head; unsigned char data[]; };
struct host_websocket { host_ws_callback_t callback; void *arg; bool connected; };
static TaskFunction_t task;
static void *task_arg;
static int64_t clock_us;
static bool audio_running, malformed, inside_callback, handoff, early_start, short_tail;
static unsigned mic_packets, enqueues, acknowledgments, starts;
static uint32_t epoch;
static uint64_t played, queued;
static unsigned final_progress;
static voice_frame_t microphone_expected;

QueueHandle_t xQueueCreate(unsigned count, unsigned size)
{
    QueueHandle_t q = calloc(1, sizeof(*q) + count * size);
    assert(q); q->capacity = count; q->size = size; return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *data, TickType_t timeout)
{
    (void)timeout;
    if (q->count == q->capacity) return 0;
    memcpy(q->data + ((q->head + q->count) % q->capacity) * q->size, data, q->size);
    ++q->count; return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *data, TickType_t timeout)
{
    (void)timeout;
    if (!q->count) return 0;
    memcpy(data, q->data + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->capacity; --q->count; return pdTRUE;
}
unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return q->count; }
void vQueueDelete(QueueHandle_t q) { free(q); }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, unsigned stack,
    void *arg, unsigned priority, void *handle)
{
    (void)name; (void)stack; (void)priority; (void)handle;
    assert(!task); task = fn; task_arg = arg; return pdPASS;
}
void vTaskDelay(TickType_t delay) { clock_us += (int64_t)delay * 1000; }
void vTaskDelete(void *handle) { (void)handle; }
int64_t esp_timer_get_time(void) { return clock_us; }
bool recorder_network_ready(void) { return true; }
bool recorder_time_valid(void) { return true; }
void secret_zero(void *data, size_t size) { memset(data, 0, size); }
int esp_crt_bundle_attach(void *arg) { (void)arg; return 0; }
esp_err_t identity_access(auth_resource_t resource, bool force, char *token, size_t size)
{
    assert(resource == AUTH_PROXY && !force && size > 20);
    strcpy(token, "TEST_API_B_ACCESS");
    return ESP_OK;
}
esp_err_t board_speaker(bool enabled) { (void)enabled; return ESP_OK; }
esp_err_t audio_voice_start(void) { assert(!audio_running); audio_running = true; return ESP_OK; }
esp_err_t audio_stop(void) { audio_running = false; queued = 0; return ESP_OK; }
esp_err_t audio_read(int16_t *pcm, size_t samples)
{
    assert(audio_running && samples == PCM_SAMPLES);
    for (size_t i = 0; i < samples; ++i) pcm[i] = i;
    clock_us += 20000;
    if (queued) { uint64_t n = queued < 320 ? queued : 320; played += n; queued -= n; }
    return ESP_OK;
}
uint32_t audio_overruns(void) { return 0; }
esp_err_t audio_voice_epoch(uint32_t next)
{
    assert(audio_running && next > epoch); epoch = next; played = queued = 0; ++starts; return ESP_OK;
}
esp_err_t audio_voice_enqueue(uint32_t current, const int16_t *pcm, size_t samples)
{
    assert(current == epoch && samples > 0 && samples <= 320 && pcm[0] == 123);
    queued += samples; ++enqueues; return ESP_OK;
}
uint64_t audio_voice_played(uint32_t current) { return current == epoch ? played : 0; }
esp_err_t audio_voice_clear(uint32_t current, uint64_t *position)
{
    assert(current == epoch);
    if (position) *position = played;
    queued = 0; return ESP_OK;
}
static void event(esp_websocket_client_handle_t ws, uint8_t opcode, bool fin,
    const uint8_t *data, int size, int total, int offset)
{
    esp_websocket_event_data_t value = {.data_ptr = (const char *)data, .data_len = size,
        .fin = fin, .op_code = opcode, .payload_len = total, .payload_offset = offset};
    inside_callback = true;
    ws->callback(ws->arg, "WS", WEBSOCKET_EVENT_DATA, &value);
    inside_callback = false;
}
static void text_event(esp_websocket_client_handle_t ws, const char *json)
{
    int size = strlen(json); event(ws, 1, true, (const uint8_t *)json, size, size, 0);
}
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *cfg)
{
    assert(!strcmp(cfg->uri, "wss://proxy.invalid/v1/voice"));
    assert(!strcmp(cfg->subprotocol, "recorder.voice.v1"));
    assert(strstr(cfg->headers, "Bearer TEST_API_B_ACCESS"));
    assert(cfg->crt_bundle_attach && cfg->disable_auto_reconnect);
    return calloc(1, sizeof(struct host_websocket));
}
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t ws, int id,
    host_ws_callback_t callback, void *arg)
{
    assert(id == WEBSOCKET_EVENT_ANY); ws->callback = callback; ws->arg = arg; return ESP_OK;
}
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t ws)
{
    ws->connected = true; inside_callback = true;
    ws->callback(ws->arg, "WS", WEBSOCKET_EVENT_CONNECTED, NULL);
    inside_callback = false; return ESP_OK;
}
int esp_websocket_client_send_text(esp_websocket_client_handle_t ws, const char *data, int length, TickType_t timeout)
{
    (void)timeout;
    assert(!inside_callback); /* Avoid SDK receive-lock / send-lock inversion. */
    cJSON *json = cJSON_ParseWithLength(data, length);
    assert(json);
    const char *type = cJSON_GetObjectItemCaseSensitive(json, "type")->valuestring;
    if (!strcmp(type, "hello")) {
        assert(!mic_packets && !audio_running);
        text_event(ws, "{\"v\":1,\"type\":\"ready\",\"session_id\":\"TEST_SESSION\",\"max_session_seconds\":5}");
    } else if (!strcmp(type, "playback.cleared")) {
        assert(cJSON_GetObjectItemCaseSensitive(json, "epoch")->valueint == 1);
        assert(cJSON_GetObjectItemCaseSensitive(json, "played_samples")->valuedouble == 320);
        assert(!queued); ++acknowledgments;
    } else if (!strcmp(type, "playback.progress")) {
        if (short_tail) {
            assert(cJSON_GetObjectItemCaseSensitive(json, "epoch")->valueint == 1);
            assert(cJSON_GetObjectItemCaseSensitive(json, "played_samples")->valuedouble == 127);
            ++final_progress;
        }
    } else assert(!strcmp(type, "stop"));
    cJSON_Delete(json); return length;
}
int esp_websocket_client_send_bin(esp_websocket_client_handle_t ws, const char *data, int length, TickType_t timeout)
{
    (void)timeout;
    assert(!inside_callback && audio_running);
    voice_frame_t frame;
    assert(voice_decode((const uint8_t *)data, length, &frame));
    assert(voice_advance(&microphone_expected, &frame));
    ++mic_packets;
    if (mic_packets == 1) {
        text_event(ws, "{\"v\":1,\"type\":\"playback.start\",\"epoch\":1}");
        int16_t pcm[320] = {123};
        uint8_t packet[VOICE_MAX_PACKET];
        voice_frame_t output = {.kind = 2, .epoch = 1, .samples = short_tail ? 127 : 320,
            .pcm = (const uint8_t *)pcm};
        int packet_size = (int)(VOICE_HEADER_SIZE + output.samples * 2);
        assert(voice_encode(packet, sizeof(packet), &output));
        if (malformed) packet[6] = 1;
        event(ws, 2, false, packet, 10, 10, 0);
        event(ws, 0, true, packet + 10, 100, packet_size - 10, 0);
        event(ws, 0, true, packet + 110, packet_size - 110, packet_size - 10, 100);
        if (short_tail) text_event(ws, "{\"v\":1,\"type\":\"playback.end\",\"epoch\":1}");
    }
    if (mic_packets == 1 && early_start) {
        text_event(ws, "{\"v\":1,\"type\":\"playback.end\",\"epoch\":1}");
        text_event(ws, "{\"v\":1,\"type\":\"playback.start\",\"epoch\":2}");
    }
    if (mic_packets == 2 && handoff) {
        assert(played == 320 && !queued);
        text_event(ws, "{\"v\":1,\"type\":\"playback.end\",\"epoch\":1}");
        text_event(ws, "{\"v\":1,\"type\":\"playback.start\",\"epoch\":2}");
        int16_t pcm[320] = {123}; uint8_t packet[VOICE_MAX_PACKET];
        voice_frame_t next = {.kind = 2, .epoch = 2, .samples = 320, .pcm = (const uint8_t *)pcm};
        assert(voice_encode(packet, sizeof(packet), &next));
        event(ws, 2, true, packet, sizeof(packet), sizeof(packet), 0);
    } else if (mic_packets == 2 && !short_tail) {
        text_event(ws, "{\"v\":1,\"type\":\"playback.clear\",\"epoch\":1}");
        int16_t pcm[320] = {123}; uint8_t packet[VOICE_MAX_PACKET];
        voice_frame_t stale = {.kind = 2, .epoch = 1, .sequence = 1, .sample = 320,
            .samples = 320, .pcm = (const uint8_t *)pcm};
        assert(voice_encode(packet, sizeof(packet), &stale));
        event(ws, 2, true, packet, sizeof(packet), sizeof(packet), 0);
    }
    if (mic_packets == 8) text_event(ws, "{\"v\":1,\"type\":\"stop\"}");
    return length;
}
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t ws) { return ws->connected; }
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t ws)
{
    ws->connected = false;
    ws->callback(ws->arg, "WS", WEBSOCKET_EVENT_DISCONNECTED, NULL); return ESP_OK;
}
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t ws) { free(ws); return ESP_OK; }
static void run(bool invalid, bool next_epoch, bool premature, bool tail)
{
    malformed = invalid; mic_packets = enqueues = acknowledgments = starts = 0;
    handoff = next_epoch; early_start = premature;
    short_tail = tail; final_progress = 0;
    clock_us = 0; epoch = 0; played = queued = 0;
    microphone_expected = (voice_frame_t){.kind = 1};
    assert(voice_client_start() == ESP_OK);
    assert(voice_client_start() == ESP_ERR_INVALID_STATE);
    assert(task);
    TaskFunction_t fn = task; task = NULL; fn(task_arg);
    voice_status_t status = voice_client_status();
    assert(!status.active && !audio_running);
    if (invalid) assert(status.error != ESP_OK && !enqueues && mic_packets == 1);
    else if (premature) assert(status.error != ESP_OK && starts == 1 && enqueues == 1 && mic_packets == 1);
    else if (next_epoch) assert(status.error == ESP_OK && starts == 2 && enqueues == 2 && !acknowledgments && mic_packets == 8);
    else if (tail) assert(status.error == ESP_OK && enqueues == 1 && final_progress > 0 && !acknowledgments && mic_packets == 8);
    else assert(status.error == ESP_OK && enqueues == 1 && acknowledgments == 1 && mic_packets == 8);
}
int main(void)
{
    run(false, false, false, false);
    run(true, false, false, false);
    run(false, true, false, false);
    run(false, false, true, false);
    run(false, false, false, true);
    puts("PASS: actual voice client, TLS token boundary, fragmentation, duplex capture, clear ACK and stale epochs");
    return 0;
}
