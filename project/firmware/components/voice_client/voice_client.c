#include "voice_client.h"
#include "audio_io.h"
#include "board.h"
#include "recorder_core.h"
#include "recorder_network.h"
#include "identity.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_log.h"
#ifndef RECORDER_HOST_TEST
#include "esp_heap_caps.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { CONNECTING, LISTENING, SPEAKING, STOPPING };
static _Atomic bool active, stopping, ready;
static _Atomic bool remote_stopping;
static _Atomic uint32_t remote_stop_ms;
static _Atomic int state, error_code;
static _Atomic uint32_t current_epoch;
typedef struct {
    esp_websocket_client_handle_t ws;
    QueueHandle_t controls;
    QueueHandle_t microphone;
#ifndef RECORDER_HOST_TEST
    StaticQueue_t microphone_queue;
    uint8_t *microphone_storage;
#endif
    uint8_t message[4097];
    size_t used;
    int frame_offset;
    uint8_t opcode;
    bool assembling, ended, audio_started;
    _Atomic bool capture_done;
    voice_frame_t expected;
    uint32_t last_epoch;
    uint32_t cleared_epoch;
    int64_t deadline;
} voice_session_t;

#define MIC_QUEUE_FRAMES 50

voice_status_t voice_client_status(void)
{
    static const char *names[] = {"CONNECTING", "LISTENING", "SPEAKING", "STOPPING"};
    return (voice_status_t){.active = atomic_load(&active), .error = atomic_load(&error_code),
        .state = names[atomic_load(&state)]};
}
void voice_client_stop(void) { atomic_store(&stopping, true); }
static bool draining_remote_stop(void)
{
    if (!atomic_load(&remote_stopping)) return false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    /* Preserve the proxy's following error/stop frame, but do not wait for
       a lost close notification until the full voice-session deadline. */
    if (now - atomic_load(&remote_stop_ms) >= 250) atomic_store(&stopping, true);
    else vTaskDelay(pdMS_TO_TICKS(20));
    return true;
}
static void fail(esp_err_t err)
{
    int expected = ESP_OK;
    if (atomic_compare_exchange_strong(&error_code, &expected, err))
        ESP_LOGE("voice", "Session stopping: %s", esp_err_to_name(err));
    atomic_store(&stopping, true);
}
static void queue_text(voice_session_t *s, const char *text)
{
    char buffer[160] = {0};
    if (strlen(text) >= sizeof(buffer)) { fail(ESP_ERR_INVALID_SIZE); return; }
    strcpy(buffer, text);
    if (xQueueSend(s->controls, buffer, 0) != pdTRUE) fail(ESP_ERR_NO_MEM);
}
static bool send_text(voice_session_t *s, const char *text)
{
    int len = strlen(text);
    if (esp_websocket_client_send_text(s->ws, text, len, pdMS_TO_TICKS(500)) != len) {
        fail(ESP_ERR_TIMEOUT); return false;
    }
    return true;
}
static void progress(voice_session_t *s, uint32_t epoch, uint64_t played, bool cleared)
{
    char json[160];
    snprintf(json, sizeof(json), "{\"v\":1,\"type\":\"playback.%s\",\"epoch\":%lu,\"played_samples\":%llu}",
        cleared ? "cleared" : "progress", (unsigned long)epoch, (unsigned long long)played);
    if (cleared) queue_text(s, json); else send_text(s, json);
}
static void send_pending(voice_session_t *s)
{
    char text[160];
    while (xQueueReceive(s->controls, text, 0) == pdTRUE)
        if (!send_text(s, text)) break;
}
static bool capture_once(voice_session_t *s)
{
    int16_t microphone[PCM_SAMPLES];
    esp_err_t err = audio_read(microphone, PCM_SAMPLES);
    if (err != ESP_OK || audio_overruns()) {
        fail(err == ESP_OK ? ESP_FAIL : err);
        return false;
    }
    if (xQueueSend(s->microphone, microphone, 0) != pdTRUE) {
        fail(ESP_ERR_NO_MEM);
        return false;
    }
    return true;
}
#ifndef RECORDER_HOST_TEST
static void capture(void *arg)
{
    voice_session_t *s = arg;
    while (!atomic_load(&stopping) && !atomic_load(&remote_stopping) && capture_once(s)) {}
    atomic_store(&s->capture_done, true);
    vTaskDelete(NULL);
}
#endif
static const char *text(cJSON *json, const char *name)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, name);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}
static bool get_epoch(cJSON *json, uint32_t *epoch)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, "epoch");
    if (!cJSON_IsNumber(value) || value->valuedouble < 1 || value->valuedouble > UINT32_MAX ||
        (double)(uint32_t)value->valuedouble != value->valuedouble) return false;
    *epoch = value->valuedouble;
    return true;
}
static void control(voice_session_t *s)
{
    s->message[s->used] = 0;
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts((char *)s->message, s->used + 1, &end, true);
    cJSON *version = cJSON_GetObjectItemCaseSensitive(json, "v");
    const char *type = text(json, "type");
    if (json_has_nul(s->message, s->used) || !cJSON_IsObject(json) || end != (char *)s->message + s->used ||
        !cJSON_IsNumber(version) || version->valuedouble != 1 || !type) {
        cJSON_Delete(json); fail(ESP_ERR_INVALID_RESPONSE); return;
    }
    uint32_t epoch = 0;
    if (!strcmp(type, "ready")) {
        cJSON *cap = cJSON_GetObjectItemCaseSensitive(json, "max_session_seconds");
        if (atomic_load(&ready) || !text(json, "session_id") || !cJSON_IsNumber(cap) ||
            cap->valueint <= 0 || cap->valueint > 900) fail(ESP_ERR_INVALID_RESPONSE);
        else if (audio_voice_start() != ESP_OK) fail(ESP_ERR_INVALID_STATE);
        else {
            s->audio_started = true;
            s->deadline = esp_timer_get_time() + (int64_t)cap->valueint * 1000000;
            atomic_store(&state, LISTENING);
            atomic_store(&ready, true);
        }
    } else if (!strcmp(type, "state")) {
        const char *value = text(json, "state");
        if (!value) fail(ESP_ERR_INVALID_RESPONSE);
        else if (!strcmp(value, "connecting")) atomic_store(&state, CONNECTING);
        else if (!strcmp(value, "listening")) atomic_store(&state, LISTENING);
        else if (!strcmp(value, "speaking")) atomic_store(&state, SPEAKING);
        else if (!strcmp(value, "stopping")) {
            if (!atomic_load(&remote_stopping))
                atomic_store(&remote_stop_ms, (uint32_t)(esp_timer_get_time() / 1000));
            atomic_store(&remote_stopping, true);
            atomic_store(&state, STOPPING);
        }
        else fail(ESP_ERR_INVALID_RESPONSE);
    } else if (!strcmp(type, "playback.start")) {
        uint32_t previous_epoch = atomic_load(&current_epoch);
        if (!atomic_load(&ready) || !get_epoch(json, &epoch) || epoch <= s->last_epoch ||
            (previous_epoch && audio_voice_played(previous_epoch) < s->expected.sample) ||
            audio_voice_epoch(epoch) != ESP_OK) fail(ESP_ERR_INVALID_RESPONSE);
        else {
            s->expected = (voice_frame_t){.kind = 2, .epoch = epoch};
            s->last_epoch = epoch;
            s->ended = false;
            atomic_store(&current_epoch, epoch);
        }
    } else if (!strcmp(type, "playback.clear")) {
        uint64_t played = 0;
        if (!get_epoch(json, &epoch) || epoch != atomic_load(&current_epoch) ||
            audio_voice_clear(epoch, &played) != ESP_OK) fail(ESP_ERR_INVALID_RESPONSE);
        else {
            s->cleared_epoch = epoch;
            atomic_store(&current_epoch, 0);
            progress(s, epoch, played, true);
        }
    } else if (!strcmp(type, "playback.end")) {
        if (!get_epoch(json, &epoch) || epoch != atomic_load(&current_epoch)) fail(ESP_ERR_INVALID_RESPONSE);
        else s->ended = true;
    } else if (!strcmp(type, "stop")) {
        atomic_store(&stopping, true);
    } else if (!strcmp(type, "error")) {
        /* Provider messages may contain sensitive diagnostics; never render verbatim. */
        fail(ESP_FAIL);
    } else fail(ESP_ERR_NOT_SUPPORTED);
    cJSON_Delete(json);
}
static void binary(voice_session_t *s)
{
    voice_frame_t frame;
    if (!voice_decode(s->message, s->used, &frame) || frame.kind != 2) {
        fail(ESP_ERR_INVALID_RESPONSE); return;
    }
    if (frame.epoch <= s->cleared_epoch || frame.epoch < s->last_epoch) return;
    if (!atomic_load(&ready) || s->ended || frame.epoch != atomic_load(&current_epoch) ||
        !voice_advance(&s->expected, &frame)) { fail(ESP_ERR_INVALID_RESPONSE); return; }
    int16_t pcm[PCM_SAMPLES];
    memcpy(pcm, frame.pcm, frame.samples * 2);
    esp_err_t err = audio_voice_enqueue(frame.epoch, pcm, frame.samples);
    if (err != ESP_OK) fail(err);
}
static void received(voice_session_t *s, esp_websocket_event_data_t *event)
{
    if (event->op_code == 8) { atomic_store(&stopping, true); return; }
    if (event->op_code == 9 || event->op_code == 10) return;
    if (event->data_len < 0 || event->payload_len < 0 || event->payload_offset < 0 ||
        event->data_len > event->payload_len - event->payload_offset) {
        fail(ESP_ERR_INVALID_RESPONSE); return;
    }
    if (event->payload_offset == 0) {
        s->frame_offset = 0;
        if (event->op_code == 1 || event->op_code == 2) {
            if (s->assembling) { fail(ESP_ERR_INVALID_RESPONSE); return; }
            s->opcode = event->op_code;
            s->used = 0;
            s->assembling = true;
        } else if (event->op_code != 0 || !s->assembling) {
            fail(ESP_ERR_INVALID_RESPONSE); return;
        }
    }
    size_t limit = s->opcode == 2 ? VOICE_MAX_PACKET : 4096;
    if (!s->assembling || event->payload_offset != s->frame_offset ||
        (size_t)event->data_len > limit - s->used) { fail(ESP_ERR_INVALID_SIZE); return; }
    memcpy(s->message + s->used, event->data_ptr, event->data_len);
    s->used += event->data_len;
    s->frame_offset += event->data_len;
    if (s->frame_offset == event->payload_len && event->fin) {
        if (s->opcode == 1) control(s); else binary(s);
        s->assembling = false; s->used = 0;
    }
}
static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    voice_session_t *s = arg;
    if (id == WEBSOCKET_EVENT_CONNECTED)
        queue_text(s, "{\"v\":1,\"type\":\"hello\",\"sample_rate\":16000,\"channels\":1,\"format\":\"pcm16\",\"frame_samples\":320}");
    else if (id == WEBSOCKET_EVENT_DATA && !atomic_load(&stopping)) received(s, data);
    else if ((id == WEBSOCKET_EVENT_ERROR || id == WEBSOCKET_EVENT_DISCONNECTED) &&
             !atomic_load(&stopping))
        fail(ESP_ERR_INVALID_STATE);
}
static void conversation(void *unused)
{
    (void)unused;
    voice_session_t *s = calloc(1, sizeof(*s));
    if (s) atomic_store(&s->capture_done, true);
    if (s) s->controls = xQueueCreate(8, 160);
#ifdef RECORDER_HOST_TEST
    if (s) s->microphone = xQueueCreate(MIC_QUEUE_FRAMES, PCM_BYTES);
#else
    if (s) {
        s->microphone_storage = heap_caps_malloc(
            MIC_QUEUE_FRAMES * PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s->microphone_storage) {
            s->microphone = xQueueCreateStatic(
                MIC_QUEUE_FRAMES, PCM_BYTES, s->microphone_storage,
                &s->microphone_queue);
        }
    }
#endif
    char *token = malloc(8193), *headers = malloc(8256);
    esp_err_t err = s && s->controls && s->microphone && token && headers ?
                    identity_access(AUTH_PROXY, false, token, 8193) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        snprintf(headers, 8256, "Authorization: Bearer %s\r\n", token);
        esp_websocket_client_config_t cfg = {.uri = CONFIG_RECORDER_PROXY_URL,
            .subprotocol = "recorder.voice.v1", .headers = headers,
            .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_reconnect = true,
            .buffer_size = 2048, .task_stack = 8192, .task_prio = 7,
            .network_timeout_ms = 30000, .ping_interval_sec = 10, .pingpong_timeout_sec = 20};
        s->ws = esp_websocket_client_init(&cfg);
        if (!s->ws) err = ESP_ERR_NO_MEM;
    }
    if (token) { secret_zero(token, 8193); free(token); }
    if (err == ESP_OK) err = esp_websocket_register_events(s->ws, WEBSOCKET_EVENT_ANY, ws_event, s);
    if (err == ESP_OK) err = esp_websocket_client_start(s->ws);
    if (err != ESP_OK) fail(err);
    int64_t ready_deadline = esp_timer_get_time() + 60000000;
    while (!atomic_load(&ready) && !atomic_load(&stopping)) {
        if (draining_remote_stop()) continue;
        send_pending(s);
        if (esp_timer_get_time() > ready_deadline) { fail(ESP_ERR_TIMEOUT); break; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#ifndef RECORDER_HOST_TEST
    if (!atomic_load(&stopping)) {
        atomic_store(&s->capture_done, false);
        if (xTaskCreate(capture, "voice_capture", 4096, s, 10, NULL) != pdPASS) {
            atomic_store(&s->capture_done, true);
            fail(ESP_ERR_NO_MEM);
        }
    }
#endif
    uint8_t packet[VOICE_MAX_PACKET];
    int16_t microphone[PCM_SAMPLES];
    voice_frame_t mic = {.kind = 1, .pcm = (const uint8_t *)microphone, .samples = PCM_SAMPLES};
    unsigned progress_tick = 0;
    while (!atomic_load(&stopping)) {
        if (draining_remote_stop()) continue;
        send_pending(s);
        if (esp_timer_get_time() >= s->deadline) break;
#ifdef RECORDER_HOST_TEST
        if (!capture_once(s)) break;
#endif
        if (xQueueReceive(s->microphone, microphone, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        voice_encode(packet, sizeof(packet), &mic);
        int n = esp_websocket_client_send_bin(s->ws, (const char *)packet, sizeof(packet), pdMS_TO_TICKS(500));
        if (n != sizeof(packet)) { fail(ESP_ERR_TIMEOUT); break; }
        ++mic.sequence; mic.sample += PCM_SAMPLES;
        if (++progress_tick == 4) {
            send_pending(s);
            uint32_t epoch = atomic_load(&current_epoch);
            if (epoch && !uxQueueMessagesWaiting(s->controls))
                progress(s, epoch, audio_voice_played(epoch), false);
            progress_tick = 0;
        }
    }
    atomic_store(&stopping, true);
    atomic_store(&state, STOPPING);
    ESP_LOGI("voice", "Cleanup: microphone quiesce");
    for (unsigned i = 0; s && !atomic_load(&s->capture_done) && i < 50; ++i)
        vTaskDelay(pdMS_TO_TICKS(20));
    if (s && s->audio_started) {
        esp_err_t muted = board_speaker(false);
        if (muted != ESP_OK) fail(muted);
    }
    if (s && s->ws) {
        if (esp_websocket_client_is_connected(s->ws))
            send_text(s, "{\"v\":1,\"type\":\"stop\",\"reason\":\"device_exit\"}");
        ESP_LOGI("voice", "Cleanup: websocket stop");
        esp_err_t stopped = esp_websocket_client_stop(s->ws);
        if (stopped != ESP_OK && esp_websocket_client_is_connected(s->ws)) fail(stopped);
        ESP_LOGI("voice", "Cleanup: websocket destroy");
        stopped = esp_websocket_client_destroy(s->ws);
        if (stopped != ESP_OK) fail(stopped);
    }
    if (s && s->audio_started) {
        ESP_LOGI("voice", "Cleanup: audio stop");
        esp_err_t stopped = audio_stop();
        if (stopped != ESP_OK) fail(stopped);
    }
    if (headers) { secret_zero(headers, 8256); free(headers); }
    if (s && s->controls) vQueueDelete(s->controls);
    if (s && s->microphone) vQueueDelete(s->microphone);
#ifndef RECORDER_HOST_TEST
    if (s && s->microphone_storage) {
        secret_zero(s->microphone_storage, MIC_QUEUE_FRAMES * PCM_BYTES);
        heap_caps_free(s->microphone_storage);
    }
#endif
    free(s);
    ESP_LOGI("voice", "Cleanup complete");
    atomic_store(&active, false);
    vTaskDelete(NULL);
}
esp_err_t voice_client_start(void)
{
    const char *url = CONFIG_RECORDER_PROXY_URL;
    if (strncmp(url, "wss://", 6) || strchr(url, '@') || strchr(url, '?') || strchr(url, '#') ||
        !recorder_network_ready() || !recorder_time_valid()) return ESP_ERR_INVALID_STATE;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&active, &expected, true)) return ESP_ERR_INVALID_STATE;
    atomic_store(&stopping, false); atomic_store(&ready, false);
    atomic_store(&remote_stopping, false);
    atomic_store(&error_code, ESP_OK); atomic_store(&state, CONNECTING);
    atomic_store(&current_epoch, 0);
    if (xTaskCreate(conversation, "voice", 8192, NULL, 9, NULL) != pdPASS) {
        atomic_store(&active, false); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
