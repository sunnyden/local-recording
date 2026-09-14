#include "../components/recorder/recording.c"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <windows.h>

struct host_queue { unsigned head, count, capacity, size; uint8_t *data; };
static struct host_queue host_queue;
static TaskFunction_t capture_fn, worker_fn;
static jmp_buf capture_return;
static unsigned notifications, reads, writes, appends, aborts;
static unsigned decoder_reads;
static int64_t clock_us;
static bool fail_task, fail_audio, fail_storage;
static const char *record_path = "./AudioRecording_UNTIMED_1234567800000001.part";
static _Atomic bool snapshot_done;

int64_t esp_timer_get_time(void) { assert(!gui_status_lock_depth); return clock_us; }
void *heap_caps_malloc(size_t bytes, unsigned flags)
{
    assert(flags == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(bytes == 96 * PCM_BYTES);
    return malloc(bytes);
}
void heap_caps_free(void *memory) { free(memory); }
QueueHandle_t xQueueCreateStatic(unsigned count, unsigned size, uint8_t *data, StaticQueue_t *control)
{
    (void)control;
    host_queue = (struct host_queue){.capacity = count, .size = size, .data = data};
    return &host_queue;
}
BaseType_t xQueueReset(QueueHandle_t q) { q->count = q->head = 0; return pdTRUE; }
BaseType_t xQueueSend(QueueHandle_t q, const void *data, TickType_t timeout)
{
    assert(!timeout && q->count < q->capacity);
    memcpy(q->data + ((q->head + q->count) % q->capacity) * q->size, data, q->size);
    ++q->count;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *data, TickType_t timeout)
{
    (void)timeout;
    if (!q->count) return 0;
    memcpy(data, q->data + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->capacity;
    --q->count;
    return pdTRUE;
}
unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return q->count; }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, unsigned stack, void *arg,
                       unsigned priority, void *handle)
{
    (void)stack; (void)arg; (void)priority;
    if (!strcmp(name, "capture")) {
        capture_fn = fn;
        *(TaskHandle_t *)handle = &host_queue;
    } else {
        if (fail_task) return 0;
        assert(!worker_fn);
        worker_fn = fn;
    }
    return pdPASS;
}
unsigned ulTaskNotifyTake(BaseType_t clear, TickType_t timeout)
{
    (void)clear; (void)timeout;
    if (notifications++) longjmp(capture_return, 1);
    return 1;
}
void xTaskNotifyGive(TaskHandle_t handle)
{
    assert(handle == &host_queue);
    notifications = 0;
    if (!setjmp(capture_return)) capture_fn(NULL);
}
void vTaskDelay(TickType_t ticks) { clock_us += (int64_t)ticks * 1000; }
void vTaskDelete(void *handle) { (void)handle; }
esp_err_t audio_init(void) { return ESP_OK; }
esp_err_t audio_start(bool microphone, bool speaker)
{
    assert(!gui_status_lock_depth);
    assert(microphone != speaker);
    return fail_audio ? ESP_FAIL : ESP_OK;
}
esp_err_t audio_stop(void)
{
    assert(!gui_status_lock_depth);
    local_status_t status = local_status();
    assert(!status.activity_level);
    return ESP_OK;
}
uint32_t audio_overruns(void) { assert(!gui_status_lock_depth); return 0; }
esp_err_t audio_read(int16_t *pcm, size_t count)
{
    assert(!gui_status_lock_depth);
    assert(count == PCM_SAMPLES);
    clock_us += 20000;
    if (reads) assert(local_status().activity_level == 255);
    for (size_t i = 0; i < count; ++i) pcm[i] = i == 0 ? INT16_MIN : (int16_t)i;
    if (++reads == 6) local_stop();
    return ESP_OK;
}
esp_err_t audio_write(const int16_t *pcm, size_t count)
{
    assert(!gui_status_lock_depth);
    assert(count == PCM_SAMPLES || count == 17);
    local_status_t status = local_status();
    assert(status.mode == LOCAL_PLAY && status.total_samples == PCM_SAMPLES + 17);
    assert(status.samples == writes * PCM_SAMPLES);
    assert(pcm[0] == INT16_MIN);
    for (size_t i = 1; i < count; ++i) assert(pcm[i] == (int16_t)i);
    if (writes++) assert(status.activity_level == 255);
    clock_us += 20000;
    return ESP_OK;
}
bool storage_valid_name(const char *name)
{
    return recording_name_valid(name, false);
}
esp_err_t storage_begin(recording_file_t *file, uint16_t pre_skip)
{
    assert(pre_skip == 312);
    snprintf(file->path, sizeof(file->path), "%s", record_path);
    return fail_storage ? ESP_FAIL : ESP_OK;
}
esp_err_t storage_append(recording_file_t *file, const void *data, size_t bytes,
                         uint32_t samples)
{
    assert(!gui_status_lock_depth);
    (void)file;
    assert(bytes == 60 && samples == PCM_SAMPLES && ((const uint8_t *)data)[0] == 0x48);
    ++appends;
    return ESP_OK;
}
esp_err_t storage_append_padding(recording_file_t *file, const void *data, size_t bytes,
                                 uint32_t samples)
{
    assert(file && data && bytes == 60 && samples == PCM_SAMPLES);
    return ESP_OK;
}
esp_err_t storage_finish(recording_file_t *file) { (void)file; return ESP_OK; }
esp_err_t storage_abort(recording_file_t *file) { (void)file; ++aborts; return ESP_OK; }
recording_encoder_t *recording_encoder_create(uint16_t *pre_skip)
{
    *pre_skip = 312;
    return (recording_encoder_t *)&host_queue;
}
esp_err_t recording_encoder_encode(recording_encoder_t *value, const int16_t *pcm,
    size_t samples, uint8_t *packet, size_t capacity, size_t *bytes)
{
    assert(value == (recording_encoder_t *)&host_queue && samples == PCM_SAMPLES);
    assert(capacity >= 60);
    if (pcm[0] == INT16_MIN) {
        for (size_t i = 1; i < samples; ++i) assert(pcm[i] == (int16_t)i);
    } else {
        for (size_t i = 0; i < samples; ++i) assert(!pcm[i]);
    }
    memset(packet, 0, 60); packet[0] = 0x48; *bytes = 60;
    return ESP_OK;
}
void recording_encoder_destroy(recording_encoder_t *value)
{
    assert(value == (recording_encoder_t *)&host_queue);
}
bool ogg_opus_parse(FILE *file, uint32_t limit, bool require_eos, ogg_opus_info_t *info)
{
    assert(file && !limit && require_eos);
    *info = (ogg_opus_info_t){.samples = PCM_SAMPLES + 17, .eos = true};
    return true;
}
recording_decoder_t *recording_decoder_create(void)
{
    decoder_reads = 0;
    return (recording_decoder_t *)&host_queue;
}
esp_err_t recording_decoder_read(recording_decoder_t *value, FILE *file, int16_t *pcm,
    size_t capacity, size_t *samples)
{
    assert(value == (recording_decoder_t *)&host_queue && file && capacity == PCM_SAMPLES);
    if (decoder_reads >= 2) return ESP_ERR_NOT_FOUND;
    *samples = decoder_reads++ ? 17 : PCM_SAMPLES;
    for (size_t i = 0; i < *samples; ++i) pcm[i] = i ? (int16_t)i : INT16_MIN;
    return ESP_OK;
}
void recording_decoder_destroy(recording_decoder_t *value)
{
    assert(value == (recording_decoder_t *)&host_queue);
}
static void run_worker(void)
{
    assert(worker_fn);
    TaskFunction_t fn = worker_fn;
    worker_fn = NULL;
    fn(NULL);
}
static void test_meter(void)
{
    int16_t pcm[] = {0, INT16_MIN, INT16_MAX, -1, 16384, -16384};
    int16_t before[6];
    memcpy(before, pcm, sizeof(pcm));
    assert(audio_meter_peak(pcm, 6) == 255);
    assert(audio_meter_peak(pcm + 2, 1) == 254);
    assert(audio_meter_peak(pcm + 4, 2) == 127);
    assert(audio_meter_peak(pcm, 1) == 0);
    assert(audio_meter_peak(pcm, 0) == 0);
    assert(!memcmp(pcm, before, sizeof(pcm)));
    audio_meter_t meter = {0};
    audio_meter_observe(&meter, 12, 0);
    audio_meter_observe(&meter, 200, 20);
    assert(audio_meter_level(&meter, 20) == 12);
    audio_meter_observe(&meter, 0, 100);
    assert(audio_meter_level(&meter, 100) == 200);
    audio_meter_observe(&meter, 0, 200);
    assert(audio_meter_level(&meter, 200) == 0);
    audio_meter_observe(&meter, 255, 300);
    assert(audio_meter_level(&meter, 549) == 255);
    assert(audio_meter_level(&meter, 550) == 0);
    audio_meter_observe(&meter, 255, 320);
    audio_meter_observe(&meter, 0, 600);
    assert(audio_meter_level(&meter, 600) == 0);
    meter = (audio_meter_t){0};
    audio_meter_observe(&meter, 100, UINT32_MAX - 10);
    assert(audio_meter_level(&meter, 20) == 100);
    assert(audio_meter_level(&meter, 240) == 0);
}
static DWORD WINAPI snapshot_writer(void *unused)
{
    (void)unused;
    for (uint32_t i = 1; i < 20000; ++i) {
        char name[65] = {0};
        memset(name, i & 1 ? 'a' : 'b', sizeof(name) - 1);
        portENTER_CRITICAL(&status_lock);
        generation = i;
        memcpy(status_name, name, sizeof(name));
        portEXIT_CRITICAL(&status_lock);
    }
    atomic_store(&snapshot_done, true);
    return 0;
}
static void test_snapshot_race(void)
{
    portENTER_CRITICAL(&status_lock);
    generation = 0;
    memset(status_name, 'b', sizeof(status_name) - 1);
    status_name[64] = 0;
    portEXIT_CRITICAL(&status_lock);
    HANDLE thread = CreateThread(NULL, 0, snapshot_writer, NULL, 0, NULL);
    assert(thread);
    do {
        local_status_t status = local_status();
        assert(status.filename[64] == 0);
        for (unsigned i = 0; i < 64; ++i)
            assert(status.filename[i] == (status.generation & 1 ? 'a' : 'b'));
    } while (!atomic_load(&snapshot_done));
    assert(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
    assert(CloseHandle(thread));
}
int main(void)
{
    test_meter();
    assert(local_audio_init() == ESP_OK);
    assert(local_status().generation == 0 && !local_status().filename[0]);
    assert(local_record_start() == ESP_OK);
    local_status_t first = local_status();
    assert(first.mode == LOCAL_RECORD && first.generation == 1 && !first.activity_level);
    assert(!strcmp(first.filename, "AudioRecording_UNTIMED_1234567800000001.opus"));
    assert(local_record_start() == ESP_ERR_INVALID_STATE);
    assert(local_status().generation == first.generation);
    run_worker();
    local_status_t saved = local_status();
    assert(reads == 6 && appends == 6 && saved.samples == 6 * PCM_SAMPLES);
    assert(saved.mode == LOCAL_IDLE && !saved.activity_level && !saved.total_samples);
    assert(!strcmp(first.filename, saved.filename));

    const char *name = "rec-local-000001.opus";
    FILE *file = fopen(name, "wb");
    assert(file);
    assert(fwrite("OggS", 1, 4, file) == 4);
    assert(!fclose(file));
    assert(local_play_start(name) == ESP_OK);
    local_status_t playing = local_status();
    assert(playing.generation == first.generation + 1 && !playing.activity_level);
    assert(!playing.total_samples && !playing.samples && !strcmp(playing.filename, name));
    assert(!strcmp(first.filename, "AudioRecording_UNTIMED_1234567800000001.opus"));
    playing.filename[0] = 'X';
    assert(local_status().filename[0] == 'r');
    run_worker();
    saved = local_status();
    assert(writes == 2 && saved.total_samples == PCM_SAMPLES + 17);
    assert(saved.samples == saved.total_samples && !saved.activity_level && saved.mode == LOCAL_IDLE);
    assert(!remove(name));
    assert(local_play_start("rec-missing.opus") == ESP_OK);
    assert(!local_status().total_samples && !local_status().activity_level);
    run_worker();
    assert(local_status().error == ESP_FAIL && !local_status().activity_level);
    uint32_t previous = local_status().generation;
    fail_storage = true;
    assert(local_record_start() == ESP_FAIL && local_status().generation == previous);
    fail_storage = false;
    fail_audio = true;
    assert(local_record_start() == ESP_FAIL && !local_status().activity_level);
    assert(aborts == 1);
    fail_audio = false;
    fail_task = true;
    assert(local_record_start() == ESP_ERR_NO_MEM && local_status().mode == LOCAL_IDLE);
    assert(aborts == 2);
    assert(!local_status().activity_level);
    test_snapshot_race();
    free(host_queue.data);
    puts("PASS: actual local Opus capture/playback wiring; integer meter, cadence, staleness, generations and private names");
    return 0;
}
