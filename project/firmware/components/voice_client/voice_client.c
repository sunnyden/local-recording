#include "voice_client.h"
#include "audio_io.h"
#include "audio_meter.h"
#include "board.h"
#include "recorder_core.h"
#include "rolling_audio.h"
#include "recorder_network.h"
#include "identity.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_log.h"
#ifndef RECORDER_HOST_TEST
#include "esp_heap_caps.h"
#else
#define heap_caps_free free
#define heap_caps_malloc(size, caps) malloc(size)
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#ifndef RECORDER_HOST_TEST
#include "psa/crypto.h"
#endif
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef RECORDER_HOST_TEST
esp_err_t rolling_snapshot_ogg(const rolling_snapshot_t *snapshot,
                               uint8_t **data, size_t *bytes)
{
    (void)snapshot;
    *data = NULL; *bytes = 0;
    return ESP_OK;
}
void rolling_snapshot_release(rolling_snapshot_t *snapshot)
{
    if (snapshot) memset(snapshot, 0, sizeof(*snapshot));
}
#endif

enum { CONNECTING, LISTENING, SPEAKING, STOPPING };
static _Atomic bool active, stopping, ready;
static _Atomic bool remote_stopping;
static _Atomic uint32_t remote_stop_ms;
static _Atomic int state, error_code;
static _Atomic uint32_t current_epoch;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t generation, meter_epoch, playback_ms, playback_poll_ms;
static uint64_t meter_played;
static audio_meter_t microphone_meter, speaker_meter;
static bool playback_active;
static uint8_t *voice_pending_context;
static size_t voice_pending_context_bytes;
static char voice_pending_context_sha256[65];
typedef struct {
    esp_websocket_client_handle_t ws;
    QueueHandle_t controls;
    QueueHandle_t microphone;
#ifndef RECORDER_HOST_TEST
    StaticQueue_t microphone_queue;
    uint8_t *microphone_storage;
#endif
    uint8_t message[4097];
    uint8_t *context;
    size_t context_bytes;
    char context_sha256[65];
    size_t used;
    int frame_offset;
    uint8_t opcode;
    bool assembling, ended, audio_started;
    _Atomic bool connected;
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
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&status_lock);
    voice_status_t status = {.active = atomic_load(&active), .error = atomic_load(&error_code),
        .state = names[atomic_load(&state)], .generation = generation};
    if (status.active && !atomic_load(&stopping) && !atomic_load(&remote_stopping)) {
        status.microphone_level = audio_meter_level(&microphone_meter, now);
        status.playback_active = playback_active && now - playback_ms < 250;
        if (status.playback_active) status.speaker_level = audio_meter_level(&speaker_meter, now);
    }
    portEXIT_CRITICAL(&status_lock);
    return status;
}
static void reset_playback_meter(uint32_t epoch)
{
    portENTER_CRITICAL(&status_lock);
    meter_epoch = epoch;
    meter_played = 0;
    playback_ms = playback_poll_ms = 0;
    playback_active = false;
    speaker_meter = (audio_meter_t){0};
    portEXIT_CRITICAL(&status_lock);
}
static void observe_playback(void)
{
    uint32_t epoch = atomic_load(&current_epoch);
    uint64_t played = epoch ? audio_voice_played(epoch) : 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&status_lock);
    if (epoch == meter_epoch && now - playback_poll_ms >= 100) {
        playback_active = epoch && played > meter_played;
        if (playback_active) playback_ms = now;
        meter_played = played;
        playback_poll_ms = now;
    }
    portEXIT_CRITICAL(&status_lock);
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
    char buffer[320] = {0};
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
    snprintf(json, sizeof(json), "{\"v\":2,\"type\":\"playback.%s\",\"epoch\":%lu,\"played_samples\":%llu}",
        cleared ? "cleared" : "progress", (unsigned long)epoch, (unsigned long long)played);
    if (cleared) queue_text(s, json); else send_text(s, json);
}
static void send_pending(voice_session_t *s)
{
    char text[320];
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
    uint8_t level = audio_meter_peak(microphone, PCM_SAMPLES);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&status_lock);
    audio_meter_observe(&microphone_meter, level, now);
    portEXIT_CRITICAL(&status_lock);
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
        !cJSON_IsNumber(version) || version->valuedouble != 2 || !type) {
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
            reset_playback_meter(epoch);
            atomic_store(&current_epoch, epoch);
        }
    } else if (!strcmp(type, "playback.clear")) {
        uint64_t played = 0;
        if (!get_epoch(json, &epoch) || epoch != atomic_load(&current_epoch) ||
            audio_voice_clear(epoch, &played) != ESP_OK) fail(ESP_ERR_INVALID_RESPONSE);
        else {
            s->cleared_epoch = epoch;
            atomic_store(&current_epoch, 0);
            reset_playback_meter(0);
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
    else {
        uint8_t level = audio_meter_peak(pcm, frame.samples);
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        portENTER_CRITICAL(&status_lock);
        if (meter_epoch == frame.epoch) audio_meter_observe(&speaker_meter, level, now);
        portEXIT_CRITICAL(&status_lock);
    }
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
        atomic_store(&s->connected, true);
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
    if (s) s->controls = xQueueCreate(8, 320);
    if (s) {
        s->context = voice_pending_context;
        s->context_bytes = voice_pending_context_bytes;
        memcpy(s->context_sha256, voice_pending_context_sha256,
               sizeof(s->context_sha256));
        voice_pending_context = NULL;
        voice_pending_context_bytes = 0;
    }
    if (!s && voice_pending_context) {
        secret_zero(voice_pending_context, voice_pending_context_bytes);
        heap_caps_free(voice_pending_context);
        voice_pending_context = NULL;
        voice_pending_context_bytes = 0;
    }
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
            .subprotocol = "recorder.voice.v2", .headers = headers,
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
    bool context_sent = false;
    bool hello_sent = false;
    uint8_t *context_wire = s && s->context_bytes ? heap_caps_malloc(
        4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (s && s->context_bytes && !context_wire) fail(ESP_ERR_NO_MEM);
    while (!atomic_load(&ready) && !atomic_load(&stopping)) {
        if (draining_remote_stop()) continue;
        send_pending(s);
        if (!hello_sent && atomic_load(&s->connected)) {
            char hello[320];
            snprintf(hello, sizeof(hello),
                "{\"v\":2,\"type\":\"hello\",\"sample_rate\":16000,\"channels\":1,"
                "\"format\":\"pcm16\",\"frame_samples\":320,\"context\":{"
                "\"format\":\"ogg_opus\",\"length\":%u,\"sha256\":\"%s\"}}",
                (unsigned)s->context_bytes, s->context_sha256);
            hello_sent = send_text(s, hello);
        }
        if (hello_sent && !context_sent) {
            size_t offset = 0;
            uint32_t sequence = 0;
            while (offset < s->context_bytes && !atomic_load(&stopping)) {
                uint8_t *wire = context_wire;
                memset(wire, 0, 20);
                memcpy(wire, "ERC2", 4); wire[4] = 2; wire[5] = 1;
                size_t chunk = s->context_bytes - offset;
                if (chunk > 4076) chunk = 4076;
                memcpy(wire + 8, &sequence, 4);
                uint32_t wire_offset = (uint32_t)offset;
                uint32_t wire_length = (uint32_t)chunk;
                memcpy(wire + 12, &wire_offset, 4);
                memcpy(wire + 16, &wire_length, 4);
                memcpy(wire + 20, s->context + offset, chunk);
                int sent = esp_websocket_client_send_bin(
                    s->ws, (const char *)wire, (int)(20 + chunk),
                    pdMS_TO_TICKS(1000));
                if (sent != (int)(20 + chunk)) { fail(ESP_ERR_TIMEOUT); break; }
                offset += chunk;
                ++sequence;
            }
            context_sent = true;
        }
        if (esp_timer_get_time() > ready_deadline) { fail(ESP_ERR_TIMEOUT); break; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (context_wire) {
        secret_zero(context_wire, 4096);
        heap_caps_free(context_wire);
        context_wire = NULL;
    }
    if (s && s->context) {
        secret_zero(s->context, s->context_bytes);
        heap_caps_free(s->context);
        s->context = NULL;
        s->context_bytes = 0;
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
        observe_playback();
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
            send_text(s, "{\"v\":2,\"type\":\"stop\",\"reason\":\"device_exit\"}");
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
    portENTER_CRITICAL(&status_lock);
    microphone_meter = (audio_meter_t){0};
    speaker_meter = (audio_meter_t){0};
    playback_active = false;
    atomic_store(&active, false);
    portEXIT_CRITICAL(&status_lock);
    vTaskDelete(NULL);
}
esp_err_t voice_client_start(void)
{
    rolling_snapshot_t empty = {0};
    return voice_client_start_with_context(&empty);
}
esp_err_t voice_client_start_with_context(rolling_snapshot_t *snapshot)
{
    if (!snapshot) return ESP_ERR_INVALID_ARG;
    uint8_t *context = NULL;
    size_t context_bytes = 0;
    esp_err_t context_err = rolling_snapshot_ogg(snapshot, &context, &context_bytes);
    rolling_snapshot_release(snapshot);
    if (context_err != ESP_OK) return context_err;
    uint8_t digest[32];
#ifndef RECORDER_HOST_TEST
    size_t digest_bytes = 0;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    psa_status_t hash_status = psa_hash_setup(&hash, PSA_ALG_SHA_256);
    if (hash_status == PSA_SUCCESS)
        hash_status = psa_hash_update(&hash, context, context_bytes);
    if (hash_status == PSA_SUCCESS)
        hash_status = psa_hash_finish(&hash, digest, sizeof(digest), &digest_bytes);
    psa_hash_abort(&hash);
    if (hash_status != PSA_SUCCESS || digest_bytes != sizeof(digest)) {
        if (context) { secret_zero(context, context_bytes); heap_caps_free(context); }
        return ESP_FAIL;
    }
#else
    static const uint8_t empty_digest[32] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
    };
    if (context_bytes) {
        secret_zero(context, context_bytes); heap_caps_free(context);
        return ESP_ERR_NOT_SUPPORTED;
    }
    memcpy(digest, empty_digest, sizeof(digest));
#endif
    char hex[65];
    for (unsigned i = 0; i < sizeof(digest); ++i)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    const char *url = CONFIG_RECORDER_PROXY_URL;
    if (strncmp(url, "wss://", 6) || strchr(url, '@') || strchr(url, '?') || strchr(url, '#') ||
        !recorder_network_ready() || !recorder_time_valid()) {
        if (context) { secret_zero(context, context_bytes); heap_caps_free(context); }
        return ESP_ERR_INVALID_STATE;
    }
    bool expected = false;
    portENTER_CRITICAL(&status_lock);
    if (!atomic_compare_exchange_strong(&active, &expected, true)) {
        portEXIT_CRITICAL(&status_lock);
        if (context) { secret_zero(context, context_bytes); heap_caps_free(context); }
        return ESP_ERR_INVALID_STATE;
    }
    voice_pending_context = context;
    voice_pending_context_bytes = context_bytes;
    memcpy(voice_pending_context_sha256, hex, sizeof(hex));
    ++generation;
    microphone_meter = (audio_meter_t){0};
    speaker_meter = (audio_meter_t){0};
    playback_active = false;
    meter_epoch = 0;
    meter_played = 0;
    playback_ms = playback_poll_ms = 0;
    atomic_store(&stopping, false); atomic_store(&ready, false);
    atomic_store(&remote_stopping, false);
    atomic_store(&error_code, ESP_OK); atomic_store(&state, CONNECTING);
    atomic_store(&current_epoch, 0);
    portEXIT_CRITICAL(&status_lock);
    if (xTaskCreate(conversation, "voice", 8192, NULL, 9, NULL) != pdPASS) {
        atomic_store(&active, false);
        if (voice_pending_context) {
            secret_zero(voice_pending_context, voice_pending_context_bytes);
            heap_caps_free(voice_pending_context);
            voice_pending_context = NULL; voice_pending_context_bytes = 0;
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
