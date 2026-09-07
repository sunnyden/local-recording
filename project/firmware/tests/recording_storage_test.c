#include "recorder.h"
#include "processing_outbox.h"
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
    assert(!strcmp(name, "AudioRecording_20260907_220536.wav"));
    assert(recording_timestamp(&stamp, timestamp));
    assert(!strcmp(timestamp, "2026-09-07T22:05:36+08:00"));
    stamp.offset_minutes = -720;
    assert(recording_timestamp(&stamp, timestamp));
    assert(!strcmp(timestamp, "2026-09-07T02:05:36-12:00"));
    stamp.offset_minutes = 841;
    assert(!recording_timestamp(&stamp, timestamp));
    const char *invalid[] = {"../rec-a.wav", "rec-a.wav/xx", "AudioRecording_.wav",
        "AudioRecording_20260907_220536_000.wav", "AudioRecording_20260907_220536.wav.meta",
        "AudioRecording_20261307_220536.wav", "AudioRecording_20260229_220536.wav",
        "AudioRecording_20260907_246000.wav",
        "evil.wav", "rec-foo..wav", "rec-x\\a.wav", "AudioRecording_UNTIMED_z123456789abcdef.wav"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) assert(!storage_valid_name(invalid[i]));
    assert(storage_valid_name("rec-old-file.wav"));
    recording_file_t first, second;
    assert(storage_begin(&first) == ESP_OK);
    assert(storage_begin(&second) == ESP_OK);
    assert(strstr(first.path, "AudioRecording_20260907_220536.part"));
    assert(strstr(second.path, "AudioRecording_20260907_220536_001.part"));
    uint8_t pcm[32000] = {0};
    assert(storage_append(&first, pcm, sizeof(pcm)) == ESP_OK);
    assert(storage_finish(&first) == ESP_OK);
    assert(storage_finish(&second) == ESP_OK);
    assert(storage_begin(&first) == ESP_OK);
    assert(strstr(first.path, "_002.part"));
    assert(storage_append(&first, pcm, sizeof(pcm)) == ESP_OK);
    assert(storage_append(&first, pcm, 200) == ESP_OK);
    fclose(first.file); fclose(first.journal); /* Simulate reset after a committed checkpoint. */
    unsigned repaired, failed;
    assert(storage_recover(&repaired, &failed) == ESP_OK && repaired == 1 && failed == 0);
    assert(storage_recording_time("AudioRecording_20260907_220536_002.wav", &stamp) == ESP_OK);
    assert(stamp.utc == clock_now && stamp.clock_valid && stamp.offset_minutes == 480);
    FILE *f = fopen(RECORDING_DIR "/AudioRecording_20260907_220536_002.wav", "rb");
    wav_info_t info;
    assert(f && wav_parse(f, &info) && info.bytes == 32000);
    fclose(f);
    /* Legacy 16-byte journal recovery remains accepted without metadata. */
    f = fopen(RECORDING_DIR "/rec-legacy.part", "wb");
    uint8_t header[44], checkpoint[16];
    wav_header(header, 0); recording_checkpoint_encode(checkpoint, 32000);
    assert(f && fwrite(header, 1, 44, f) == 44 && fwrite(pcm, 1, sizeof(pcm), f) == sizeof(pcm));
    fclose(f);
    f = fopen(RECORDING_DIR "/rec-legacy.ckp", "wb");
    assert(f && fwrite(checkpoint, 1, 16, f) == 16); fclose(f);
    assert(storage_recover(&repaired, &failed) == ESP_OK && repaired == 1);
    assert(storage_recording_time("rec-legacy.wav", &stamp) == ESP_ERR_NOT_FOUND);
    clock_now = 0;
    assert(storage_begin(&first) == ESP_OK && strstr(first.path, "UNTIMED_1234567812345678"));
    assert(storage_finish(&first) == ESP_OK);
    assert(storage_recording_time("AudioRecording_UNTIMED_1234567812345678.wav", &stamp) == ESP_OK);
    assert(!stamp.clock_valid && !recording_timestamp(&stamp, timestamp));
    size_t count;
    assert(storage_catalog(0, name, sizeof(name), &count) == ESP_OK && count == 5);
    processing_job_t job = job_for("rec-legacy.wav"), loaded;
    assert(processing_outbox_save(&job) == ESP_OK);
    assert(processing_outbox_load(job.name, &loaded) == ESP_OK && !memcmp(&job, &loaded, sizeof(job)));
    strcpy(loaded.drive_id, "different-user-drive");
    assert(processing_outbox_save(&loaded) == ESP_ERR_INVALID_STATE);
    job.state = PROCESS_COMPLETED;
    strcpy(job.json_item_id, "json"); strcpy(job.text_item_id, "text");
    assert(processing_outbox_save(&job) == ESP_OK);
    assert(processing_outbox_load(job.name, &loaded) == ESP_OK && loaded.state == PROCESS_COMPLETED);
    f = fopen(RECORDING_DIR "/rec-legacy.job", "rb+");
    assert(f && !fseek(f, 0, SEEK_END));
    long end = ftell(f);
    assert(!fseek(f, end / 2, SEEK_SET) && fwrite("TORN", 1, 4, f) == 4);
    fclose(f);
    assert(processing_outbox_load(job.name, &loaded) == ESP_OK && loaded.state == PROCESS_PENDING);
    assert(processing_outbox_catalog(0, name, &count) == ESP_OK && count == 1 && !strcmp(name, job.name));
    assert(storage_catalog(0, name, sizeof(name), &count) == ESP_OK && count == 5);
    clear_files(); _rmdir(RECORDING_DIR);
    puts("PASS: actual SD storage, timestamps/collisions/offline, legacy recovery, durable two-slot outbox");
}
