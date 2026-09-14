#include "recorder.h"
#include "board.h"
#include "audio_io.h"
#include "audio_meter.h"
#include "recording_codec.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdatomic.h>
#include <string.h>

#define CAPTURE_FRAMES 96
#define LOCAL_WORKER_STACK 12288
typedef struct { int16_t pcm[PCM_SAMPLES]; } pcm_block_t;
static QueueHandle_t queue;
static StaticQueue_t queue_control;
static TaskHandle_t capture_task;
static _Atomic bool stop_requested, capture_done;
static _Atomic int mode, error_code;
static _Atomic uint32_t samples, peak;
static _Atomic uint32_t codec_us_total, codec_frames, codec_us_max;
static _Atomic uint32_t file_bytes;
static char play_name[65];
static recording_file_t recording;
static recording_encoder_t *encoder;
static uint32_t encoder_padding_frames;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_meter_t activity;
static uint32_t generation, total_samples;
static char status_name[65];

local_status_t local_status(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t overruns = audio_overruns();
    portENTER_CRITICAL(&status_lock);
    local_status_t status = {.mode = atomic_load(&mode), .error = atomic_load(&error_code),
        .samples = atomic_load(&samples), .queue_peak = atomic_load(&peak),
        .overruns = overruns, .generation = generation, .total_samples = total_samples,
        .file_bytes = atomic_load(&file_bytes)};
    uint32_t frames = atomic_load(&codec_frames);
    status.codec_us_average = frames ? atomic_load(&codec_us_total) / frames : 0;
    status.codec_us_max = atomic_load(&codec_us_max);
    memcpy(status.filename, status_name, sizeof(status.filename));
    if ((status.mode == LOCAL_RECORD || status.mode == LOCAL_PLAY) &&
        !atomic_load(&stop_requested)) status.activity_level = audio_meter_level(&activity, now);
    portEXIT_CRITICAL(&status_lock);
    return status;
}
static void observe(const int16_t *pcm, size_t count)
{
    uint8_t level = audio_meter_peak(pcm, count);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&status_lock);
    audio_meter_observe(&activity, level, now);
    portEXIT_CRITICAL(&status_lock);
}
void local_stop(void) { atomic_store(&stop_requested, true); }
static void fail(esp_err_t err)
{
    atomic_store(&error_code, err);
    atomic_store(&stop_requested, true);
}
static void observe_codec(uint32_t elapsed)
{
    atomic_fetch_add(&codec_us_total, elapsed);
    atomic_fetch_add(&codec_frames, 1);
    uint32_t maximum = atomic_load(&codec_us_max);
    while (elapsed > maximum &&
           !atomic_compare_exchange_weak(&codec_us_max, &maximum, elapsed)) {}
}
static void capture(void *unused)
{
    (void)unused;
    pcm_block_t block;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (!atomic_load(&stop_requested)) {
            esp_err_t err = audio_read(block.pcm, PCM_SAMPLES);
            if (err != ESP_OK) { fail(err); break; }
            if (audio_overruns()) { fail(ESP_ERR_INVALID_STATE); break; }
            if (xQueueSend(queue, &block, 0) != pdTRUE) { fail(ESP_ERR_NO_MEM); break; }
            observe(block.pcm, PCM_SAMPLES);
            uint32_t depth = uxQueueMessagesWaiting(queue);
            if (depth > atomic_load(&peak)) atomic_store(&peak, depth);
        }
        esp_err_t stop_err = audio_stop();
        if (stop_err != ESP_OK && atomic_load(&error_code) == ESP_OK)
            atomic_store(&error_code, stop_err);
        atomic_store(&capture_done, true);
    }
}
static void record_worker(void)
{
    pcm_block_t block;
    uint8_t packet[OPUS_PACKET_MAX];
    uint32_t encoded_frames = 0;
    xTaskNotifyGive(capture_task);
    while (!atomic_load(&capture_done) || uxQueueMessagesWaiting(queue)) {
        if (xQueueReceive(queue, &block, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        size_t bytes = 0;
        int64_t started = esp_timer_get_time();
        esp_err_t err = recording_encoder_encode(encoder, block.pcm, PCM_SAMPLES,
                                                 packet, sizeof(packet), &bytes);
        observe_codec((uint32_t)(esp_timer_get_time() - started));
        if (err == ESP_OK)
            err = storage_append(&recording, packet, bytes, PCM_SAMPLES);
        if (err != ESP_OK) {
            fail(err);
            while (!atomic_load(&capture_done)) vTaskDelay(pdMS_TO_TICKS(10));
            xQueueReset(queue);
            break;
        }
        ++encoded_frames;
        atomic_fetch_add(&samples, PCM_SAMPLES);
    }
    if (!encoded_frames && atomic_load(&error_code) == ESP_OK) {
        memset(&block, 0, sizeof(block));
        size_t bytes = 0;
        esp_err_t err = recording_encoder_encode(encoder, block.pcm, PCM_SAMPLES,
                                                 packet, sizeof(packet), &bytes);
        if (err == ESP_OK)
            err = storage_append(&recording, packet, bytes, PCM_SAMPLES);
        if (err != ESP_OK) fail(err);
        else {
            ++encoded_frames;
            atomic_fetch_add(&samples, PCM_SAMPLES);
        }
    }
    for (uint32_t i = 0; i < encoder_padding_frames &&
         atomic_load(&error_code) == ESP_OK; ++i) {
        memset(&block, 0, sizeof(block));
        size_t bytes = 0;
        esp_err_t err = recording_encoder_encode(encoder, block.pcm, PCM_SAMPLES,
                                                 packet, sizeof(packet), &bytes);
        if (err == ESP_OK)
            err = storage_append_padding(&recording, packet, bytes, PCM_SAMPLES);
        if (err != ESP_OK) fail(err);
    }
    esp_err_t err = storage_finish(&recording);
    atomic_store(&file_bytes, recording.bytes);
    if (err != ESP_OK) fail(err);
    recording_encoder_destroy(encoder);
    encoder = NULL;
}
static void play_worker(void)
{
    char path[128];
    snprintf(path, sizeof(path), RECORDING_DIR "/%s", play_name);
    FILE *f = fopen(path, "rb");
    if (!f) { fail(ESP_FAIL); return; }
    ogg_opus_info_t info;
    if (!ogg_opus_parse(f, 0, true, &info) || fseek(f, 0, SEEK_SET)) {
        fclose(f); fail(ESP_ERR_NOT_SUPPORTED); return;
    }
    recording_decoder_t *decoder = recording_decoder_create();
    if (!decoder) { fclose(f); fail(ESP_ERR_NO_MEM); return; }
    portENTER_CRITICAL(&status_lock);
    total_samples = info.samples;
    atomic_store(&file_bytes, info.bytes);
    portEXIT_CRITICAL(&status_lock);
    int16_t pcm[PCM_SAMPLES];
    uint32_t remaining = info.samples;
    while (remaining && !atomic_load(&stop_requested)) {
        size_t decoded = 0;
        int64_t started = esp_timer_get_time();
        esp_err_t err = recording_decoder_read(decoder, f, pcm, PCM_SAMPLES, &decoded);
        if (err == ESP_OK) observe_codec((uint32_t)(esp_timer_get_time() - started));
        if (err != ESP_OK) {
            if (err != ESP_ERR_NOT_FOUND) fail(err);
            else if (remaining) fail(ESP_ERR_INVALID_SIZE);
            break;
        }
        if (decoded > remaining) decoded = remaining;
        err = audio_write(pcm, decoded);
        if (err != ESP_OK) { fail(err); break; }
        observe(pcm, decoded);
        remaining -= decoded;
        atomic_fetch_add(&samples, decoded);
    }
    /* DMA contains at most eight 20ms blocks; allow tail to drain on EOF only. */
    if (!atomic_load(&stop_requested)) vTaskDelay(pdMS_TO_TICKS(180));
    recording_decoder_destroy(decoder);
    fclose(f);
}
static void worker(void *unused)
{
    (void)unused;
    if (atomic_load(&mode) == LOCAL_RECORD) record_worker(); else play_worker();
    portENTER_CRITICAL(&status_lock);
    activity = (audio_meter_t){0};
    atomic_store(&mode, LOCAL_STOPPING);
    portEXIT_CRITICAL(&status_lock);
    esp_err_t err = audio_stop();
    if (err != ESP_OK) fail(err);
    portENTER_CRITICAL(&status_lock);
    atomic_store(&mode, LOCAL_IDLE);
    portEXIT_CRITICAL(&status_lock);
    vTaskDelete(NULL);
}
esp_err_t local_audio_init(void)
{
    esp_err_t err = audio_init();
    if (err != ESP_OK) return err;
    uint8_t *memory = heap_caps_malloc(CAPTURE_FRAMES * sizeof(pcm_block_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) return ESP_ERR_NO_MEM;
    queue = xQueueCreateStatic(CAPTURE_FRAMES, sizeof(pcm_block_t), memory, &queue_control);
    if (!queue) { heap_caps_free(memory); return ESP_ERR_NO_MEM; }
    if (xTaskCreate(capture, "capture", 4096, NULL, 12, &capture_task) != pdPASS) {
        heap_caps_free(memory);
        queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
static esp_err_t start(local_mode_t next, const char *name)
{
    if (atomic_load(&mode) != LOCAL_IDLE) return ESP_ERR_INVALID_STATE;
    char filename[65] = {0};
    snprintf(filename, sizeof(filename), "%.64s", name);
    char *extension = strrchr(filename, '.');
    if (next == LOCAL_RECORD && extension && !strcmp(extension, ".part"))
        strcpy(extension, ".opus");
    portENTER_CRITICAL(&status_lock);
    ++generation;
    total_samples = 0;
    activity = (audio_meter_t){0};
    memcpy(status_name, filename, sizeof(status_name));
    atomic_store(&stop_requested, false);
    atomic_store(&capture_done, false);
    atomic_store(&samples, 0);
    atomic_store(&peak, 0);
    atomic_store(&error_code, ESP_OK);
    atomic_store(&file_bytes, 0);
    atomic_store(&codec_us_total, 0);
    atomic_store(&codec_frames, 0);
    atomic_store(&codec_us_max, 0);
    portEXIT_CRITICAL(&status_lock);
    xQueueReset(queue);
    esp_err_t err = audio_start(next == LOCAL_RECORD, next == LOCAL_PLAY);
    if (err != ESP_OK) return err;
    portENTER_CRITICAL(&status_lock);
    atomic_store(&mode, next);
    portEXIT_CRITICAL(&status_lock);
    if (xTaskCreate(worker, "local_audio", LOCAL_WORKER_STACK, NULL, 8, NULL) != pdPASS) {
        audio_stop();
        portENTER_CRITICAL(&status_lock);
        atomic_store(&mode, LOCAL_IDLE);
        portEXIT_CRITICAL(&status_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t local_record_start(void)
{
    if (atomic_load(&mode) != LOCAL_IDLE) return ESP_ERR_INVALID_STATE;
    uint16_t pre_skip = 0;
    encoder = recording_encoder_create(&pre_skip);
    if (!encoder) return ESP_ERR_NO_MEM;
    uint32_t lookahead = (pre_skip + OPUS_RATE / PCM_RATE - 1) /
        (OPUS_RATE / PCM_RATE);
    encoder_padding_frames = (lookahead + PCM_SAMPLES - 1) / PCM_SAMPLES;
    esp_err_t err = storage_begin(&recording, pre_skip);
    if (err != ESP_OK) {
        recording_encoder_destroy(encoder); encoder = NULL;
        return err;
    }
    const char *name = strrchr(recording.path, '/');
    err = start(LOCAL_RECORD, name ? name + 1 : recording.path);
    if (err != ESP_OK) {
        storage_abort(&recording);
        recording_encoder_destroy(encoder); encoder = NULL;
    }
    return err;
}
esp_err_t local_play_start(const char *name)
{
    if (!storage_valid_name(name) || atomic_load(&mode) != LOCAL_IDLE)
        return ESP_ERR_INVALID_ARG;
    strcpy(play_name, name);
    return start(LOCAL_PLAY, play_name);
}
