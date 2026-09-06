#include "recorder.h"
#include "board.h"
#include "audio_io.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdatomic.h>
#include <string.h>

#define CAPTURE_FRAMES 96
typedef struct { int16_t pcm[PCM_SAMPLES]; } pcm_block_t;
static QueueHandle_t queue;
static StaticQueue_t queue_control;
static TaskHandle_t capture_task;
static _Atomic bool stop_requested, capture_done;
static _Atomic int mode, error_code;
static _Atomic uint32_t samples, peak;
static char play_name[65];
static recording_file_t recording;

local_status_t local_status(void)
{
    return (local_status_t){.mode = atomic_load(&mode), .error = atomic_load(&error_code),
        .samples = atomic_load(&samples), .queue_peak = atomic_load(&peak),
        .overruns = audio_overruns()};
}
void local_stop(void) { atomic_store(&stop_requested, true); }
static void fail(esp_err_t err)
{
    atomic_store(&error_code, err);
    atomic_store(&stop_requested, true);
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
            uint32_t depth = uxQueueMessagesWaiting(queue);
            if (depth > atomic_load(&peak)) atomic_store(&peak, depth);
        }
        atomic_store(&capture_done, true);
    }
}
static void record_worker(void)
{
    pcm_block_t block;
    xTaskNotifyGive(capture_task);
    while (!atomic_load(&capture_done) || uxQueueMessagesWaiting(queue)) {
        if (xQueueReceive(queue, &block, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        esp_err_t err = storage_append(&recording, block.pcm, PCM_BYTES);
        if (err != ESP_OK) {
            fail(err);
            while (!atomic_load(&capture_done)) vTaskDelay(pdMS_TO_TICKS(10));
            xQueueReset(queue);
            break;
        }
        atomic_fetch_add(&samples, PCM_SAMPLES);
    }
    esp_err_t err = storage_finish(&recording);
    if (err != ESP_OK) fail(err);
}
static void play_worker(void)
{
    char path[128];
    snprintf(path, sizeof(path), RECORDING_DIR "/%s", play_name);
    FILE *f = fopen(path, "rb");
    if (!f) { fail(ESP_FAIL); return; }
    wav_info_t info;
    if (!wav_parse(f, &info) || fseek(f, info.offset, SEEK_SET)) {
        fclose(f); fail(ESP_ERR_NOT_SUPPORTED); return;
    }
    int16_t pcm[PCM_SAMPLES];
    uint32_t remaining = info.bytes;
    while (remaining && !atomic_load(&stop_requested)) {
        size_t n = remaining < PCM_BYTES ? remaining : PCM_BYTES;
        if (fread(pcm, 1, n, f) != n) { fail(ESP_FAIL); break; }
        esp_err_t err = audio_write(pcm, n / 2);
        if (err != ESP_OK) { fail(err); break; }
        remaining -= n;
        atomic_fetch_add(&samples, n / 2);
    }
    /* DMA contains at most eight 20ms blocks; allow tail to drain on EOF only. */
    if (!atomic_load(&stop_requested)) vTaskDelay(pdMS_TO_TICKS(180));
    fclose(f);
}
static void worker(void *unused)
{
    (void)unused;
    if (atomic_load(&mode) == LOCAL_RECORD) record_worker(); else play_worker();
    atomic_store(&mode, LOCAL_STOPPING);
    esp_err_t err = audio_stop();
    if (err != ESP_OK) fail(err);
    atomic_store(&mode, LOCAL_IDLE);
    vTaskDelete(NULL);
}
esp_err_t local_audio_init(void)
{
    uint8_t *memory = heap_caps_malloc(CAPTURE_FRAMES * sizeof(pcm_block_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) return ESP_ERR_NO_MEM;
    queue = xQueueCreateStatic(CAPTURE_FRAMES, sizeof(pcm_block_t), memory, &queue_control);
    if (!queue) { heap_caps_free(memory); return ESP_ERR_NO_MEM; }
    if (xTaskCreate(capture, "capture", 4096, NULL, 12, &capture_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    return audio_init();
}
static esp_err_t start(local_mode_t next)
{
    if (atomic_load(&mode) != LOCAL_IDLE) return ESP_ERR_INVALID_STATE;
    atomic_store(&stop_requested, false);
    atomic_store(&capture_done, false);
    atomic_store(&samples, 0);
    atomic_store(&peak, 0);
    atomic_store(&error_code, ESP_OK);
    xQueueReset(queue);
    esp_err_t err = audio_start(next == LOCAL_RECORD, next == LOCAL_PLAY);
    if (err != ESP_OK) return err;
    atomic_store(&mode, next);
    if (xTaskCreate(worker, "local_audio", 6144, NULL, 8, NULL) != pdPASS) {
        audio_stop(); atomic_store(&mode, LOCAL_IDLE); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t local_record_start(void)
{
    if (atomic_load(&mode) != LOCAL_IDLE) return ESP_ERR_INVALID_STATE;
    esp_err_t err = storage_begin(&recording);
    if (err != ESP_OK) return err;
    err = start(LOCAL_RECORD);
    if (err != ESP_OK) storage_finish(&recording);
    return err;
}
esp_err_t local_play_start(const char *name)
{
    if (!storage_valid_name(name) || atomic_load(&mode) != LOCAL_IDLE)
        return ESP_ERR_INVALID_ARG;
    strcpy(play_name, name);
    return start(LOCAL_PLAY);
}
