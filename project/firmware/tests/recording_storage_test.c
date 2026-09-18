#include "recorder.h"
#include "processing_outbox.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static time_t clock_now = 1788789936; /* 2026-09-07T22:05:36+08:00 */
time_t recording_test_time(time_t *out) { if (out) *out = clock_now; return clock_now; }
uint32_t esp_random(void) { return 0x12345678; }
static void clear_files(void)
{
    DIR *dir = opendir(RECORDING_DIR);
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[256];
        snprintf(path, sizeof(path), RECORDING_DIR "/%s", entry->d_name);
        assert(!unlink(path));
    }
    closedir(dir);
}
static processing_job_t job_for(const char *name)
{
    processing_job_t job = {.source_size = 32044};
    strcpy(job.name, name);
    strcpy(job.drive_id, "drive-A"); strcpy(job.item_id, "item-A");
    memset(job.source_sha1, 'a', 40);
    strcpy(job.recorded_at, "2026-09-07T22:05:36+08:00");
    return job;
}
int main(void)
{
    _mkdir(RECORDING_DIR);
    clear_files();
    recording_time_t stamp = {.utc = clock_now, .offset_minutes = 480, .clock_valid = true};
    char name[65], timestamp[36];
    assert(recording_name_format(name, sizeof(name), &stamp, 1, 0));
    assert(!strcmp(name, "AudioRecording_20260907_220536.opus"));
    assert(recording_timestamp(&stamp, timestamp));
    assert(!strcmp(timestamp, "2026-09-07T22:05:36+08:00"));
    stamp.offset_minutes = -720;
    assert(recording_timestamp(&stamp, timestamp));
    assert(!strcmp(timestamp, "2026-09-07T02:05:36-12:00"));
    stamp.offset_minutes = 841;
    assert(!recording_timestamp(&stamp, timestamp));
    const char *invalid[] = {"../rec-a.opus", "rec-a.opus/xx", "AudioRecording_.opus",
        "AudioRecording_20260907_220536_000.opus", "AudioRecording_20260907_220536.opus.meta",
        "AudioRecording_20261307_220536.opus", "AudioRecording_20260229_220536.opus",
        "AudioRecording_20260907_246000.opus", "old.wav",
        "evil.opus", "rec-foo..opus", "rec-x\\a.opus", "AudioRecording_UNTIMED_z123456789abcdef.opus"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) assert(!storage_valid_name(invalid[i]));
    assert(storage_valid_name("rec-old-file.opus"));
    recording_file_t first, second;
    assert(storage_begin(&first, 312) == ESP_OK);
    assert(storage_begin(&second, 312) == ESP_OK);
    assert(strstr(first.path, "AudioRecording_20260907_220536.part"));
    assert(strstr(second.path, "AudioRecording_20260907_220536_001.part"));
    uint8_t packet[60] = {0x48};
    for (unsigned i = 0; i < 51; ++i)
        assert(storage_append(&first, packet, sizeof(packet), PCM_SAMPLES) == ESP_OK);
    assert(storage_append_padding(&first, packet, sizeof(packet), PCM_SAMPLES) == ESP_OK);
    assert(storage_finish(&first) == ESP_OK);
    assert(storage_append(&second, packet, sizeof(packet), PCM_SAMPLES) == ESP_OK);
    assert(storage_append_padding(&second, packet, sizeof(packet), PCM_SAMPLES) == ESP_OK);
    assert(storage_finish(&second) == ESP_OK);
    assert(storage_begin(&first, 312) == ESP_OK);
    assert(strstr(first.path, "_002.part"));
    for (unsigned i = 0; i < 51; ++i)
        assert(storage_append(&first, packet, sizeof(packet), PCM_SAMPLES) == ESP_OK);
    assert(storage_append(&first, packet, sizeof(packet), PCM_SAMPLES) == ESP_OK);
    heap_caps_free(first.writer);
    fclose(first.file); fclose(first.journal); /* Simulate reset after a committed checkpoint. */
    unsigned repaired, failed;
    assert(storage_recover(&repaired, &failed) == ESP_OK && repaired == 1 && failed == 0);
    assert(storage_recording_time("AudioRecording_20260907_220536_002.opus", &stamp) == ESP_OK);
    assert(stamp.utc == clock_now && stamp.clock_valid && stamp.offset_minutes == 480);
    FILE *f = fopen(RECORDING_DIR "/AudioRecording_20260907_220536_002.opus", "rb");
    ogg_opus_info_t info;
    assert(f && ogg_opus_parse(f, 0, true, &info) &&
           info.samples == 50 * PCM_SAMPLES - 104);
    fclose(f);
    recording_info_t recording_info;
    assert(storage_recording_info("AudioRecording_20260907_220536_002.opus",
                                  &recording_info) == ESP_OK);
    assert(recording_info.bytes > 0 &&
           recording_info.samples == 50 * PCM_SAMPLES - 104 &&
           recording_info.has_time && recording_info.time.utc == clock_now);
    FILE *opened = NULL;
    assert(storage_open_recording("AudioRecording_20260907_220536_002.opus",
                                  &opened, &recording_info) == ESP_OK);
    assert(opened && ftell(opened) == 0 && !fclose(opened));
    assert(storage_recording_info("../bad.opus", &recording_info) ==
           ESP_ERR_INVALID_ARG);
    recording_file_t aborted;
    assert(storage_begin(&aborted, 312) == ESP_OK);
    char aborted_part[128], aborted_meta[128], aborted_checkpoint[128];
    strcpy(aborted_part, aborted.path);
    strcpy(aborted_meta, aborted.path);
    strcpy(strrchr(aborted_meta, '.'), ".meta");
    strcpy(aborted_checkpoint, aborted.path);
    strcpy(strrchr(aborted_checkpoint, '.'), ".ckp");
    assert(storage_abort(&aborted) == ESP_OK);
    struct stat removed;
    assert(stat(aborted_part, &removed) && stat(aborted_meta, &removed) &&
           stat(aborted_checkpoint, &removed));
    f = fopen(RECORDING_DIR "/old.wav", "wb");
    assert(f && fwrite("RIFF", 1, 4, f) == 4); fclose(f);
    assert(storage_recover(&repaired, &failed) == ESP_OK && repaired == 0);
    assert(!storage_valid_name("old.wav"));
    clock_now = 0;
    assert(storage_begin(&first, 312) == ESP_OK && strstr(first.path, "UNTIMED_1234567812345678"));
    assert(storage_append(&first, packet, sizeof(packet), PCM_SAMPLES) == ESP_OK);
    assert(storage_append_padding(&first, packet, sizeof(packet), PCM_SAMPLES) == ESP_OK);
    assert(storage_finish(&first) == ESP_OK);
    assert(storage_recording_time("AudioRecording_UNTIMED_1234567812345678.opus", &stamp) == ESP_OK);
    assert(!stamp.clock_valid && !recording_timestamp(&stamp, timestamp));
    size_t count;
    assert(storage_catalog(0, name, sizeof(name), &count) == ESP_OK && count == 4);
    processing_job_t job = job_for("rec-old-file.opus"), loaded;
    assert(processing_outbox_save(&job) == ESP_OK);
    assert(processing_outbox_load(job.name, &loaded) == ESP_OK && !memcmp(&job, &loaded, sizeof(job)));
    strcpy(loaded.drive_id, "different-user-drive");
    assert(processing_outbox_save(&loaded) == ESP_ERR_INVALID_STATE);
    job.state = PROCESS_COMPLETED;
    strcpy(job.json_item_id, "json"); strcpy(job.text_item_id, "text");
    assert(processing_outbox_save(&job) == ESP_OK);
    assert(processing_outbox_load(job.name, &loaded) == ESP_OK && loaded.state == PROCESS_COMPLETED);
    f = fopen(RECORDING_DIR "/rec-old-file.job", "rb+");
    assert(f && !fseek(f, 0, SEEK_END));
    long end = ftell(f);
    assert(!fseek(f, end / 2, SEEK_SET) && fwrite("TORN", 1, 4, f) == 4);
    fclose(f);
    assert(processing_outbox_load(job.name, &loaded) == ESP_OK && loaded.state == PROCESS_PENDING);
    job.state = PROCESS_REMOTE_MISSING;
    assert(processing_outbox_save(&job) == ESP_OK);
    assert(processing_outbox_load(job.name, &loaded) == ESP_OK && loaded.state == PROCESS_REMOTE_MISSING);
    assert(processing_outbox_catalog(0, name, &count) == ESP_OK && count == 1 && !strcmp(name, job.name));
    assert(storage_catalog(0, name, sizeof(name), &count) == ESP_OK && count == 4);
    clear_files(); _rmdir(RECORDING_DIR);
    puts("PASS: actual Opus SD storage, timestamps/collisions/offline, page recovery, abort cleanup and durable outbox");
}
