#include "processing_outbox.h"
#include "recorder.h"
#include "board.h"
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

/* Two independently checksummed slots: a torn update leaves the previous
   pending source identity intact. Completion is a retained nonsecret receipt. */
#define OUTBOX_MAGIC 0x31424f52u
typedef struct {
    uint32_t magic, generation;
    processing_job_t job;
    uint32_t checksum;
} slot_t;
static uint32_t checksum(const slot_t *slot)
{
    const unsigned char *p = (const unsigned char *)slot;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < offsetof(slot_t, checksum); ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static bool bounded(const char *s, size_t n, bool required)
{
    const char *end = memchr(s, 0, n);
    if (!end || (required && end == s)) return false;
    for (const char *p = s; p < end; ++p)
        if ((unsigned char)*p < 32 || (unsigned char)*p > 126) return false;
    return true;
}
bool processing_job_valid(const processing_job_t *job)
{
    if (!job || !bounded(job->name, sizeof(job->name), true) ||
        !storage_valid_name(job->name) || job->source_size < 44 ||
        job->state > PROCESS_REMOTE_MISSING || job->retry_not_before < 0 ||
        !bounded(job->drive_id, sizeof(job->drive_id), true) ||
        !bounded(job->item_id, sizeof(job->item_id), true) ||
        !bounded(job->source_sha1, sizeof(job->source_sha1), true) ||
        strlen(job->source_sha1) != 40 ||
        !bounded(job->recorded_at, sizeof(job->recorded_at), false) ||
        !bounded(job->json_item_id, sizeof(job->json_item_id), job->state == PROCESS_COMPLETED) ||
        !bounded(job->text_item_id, sizeof(job->text_item_id), job->state == PROCESS_COMPLETED))
        return false;
    for (unsigned i = 0; i < 40; ++i)
        if (!((job->source_sha1[i] >= '0' && job->source_sha1[i] <= '9') ||
            (job->source_sha1[i] >= 'a' && job->source_sha1[i] <= 'f'))) return false;
    return true;
}
static bool path_for(const char *name, char path[128])
{
    if (!storage_valid_name(name)) return false;
    snprintf(path, 128, RECORDING_DIR "/%s", name);
    strcpy(strrchr(path, '.'), ".job");
    return true;
}
static esp_err_t read_latest(FILE *file, slot_t *latest, unsigned *selected)
{
    slot_t slot;
    bool found = false;
    for (unsigned i = 0; i < 2; ++i) {
        if (fseek(file, (long)(i * sizeof(slot)), SEEK_SET)) return ESP_FAIL;
        if (fread(&slot, 1, sizeof(slot), file) != sizeof(slot)) {
            if (ferror(file)) return ESP_FAIL;
            clearerr(file); continue;
        }
        if (slot.magic != OUTBOX_MAGIC || slot.checksum != checksum(&slot) ||
            !processing_job_valid(&slot.job)) continue;
        if (!found || (int32_t)(slot.generation - latest->generation) > 0) {
            *latest = slot; *selected = i; found = true;
        }
    }
    return found ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
esp_err_t processing_outbox_load(const char *name, processing_job_t *job)
{
    char path[128];
    if (!job || !path_for(name, path)) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    slot_t slot = {0}; unsigned selected = 0;
    esp_err_t err = read_latest(file, &slot, &selected);
    if (fclose(file)) return ESP_FAIL;
    if (err == ESP_OK && strcmp(slot.job.name, name)) return ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) *job = slot.job;
    return err;
}
esp_err_t processing_outbox_save(const processing_job_t *job)
{
    char path[128];
    if (!processing_job_valid(job) || !path_for(job->name, path)) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "rb+");
    slot_t slot = {0}; unsigned selected = 1;
    if (file) {
        esp_err_t err = read_latest(file, &slot, &selected);
        if (err != ESP_OK) { fclose(file); return err; }
        if (strcmp(slot.job.name, job->name) || strcmp(slot.job.drive_id, job->drive_id)) {
            fclose(file); return ESP_ERR_INVALID_STATE;
        }
    } else {
        if (errno != ENOENT) return ESP_FAIL;
        file = fopen(path, "wb+");
        if (!file) return ESP_FAIL;
    }
    slot.magic = OUTBOX_MAGIC;
    ++slot.generation;
    slot.job = *job;
    slot.checksum = checksum(&slot);
    bool ok = fseek(file, (long)((selected ^ 1u) * sizeof(slot)), SEEK_SET) == 0 &&
        fwrite(&slot, 1, sizeof(slot), file) == sizeof(slot) &&
        fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file)) ok = false;
    return ok ? ESP_OK : ESP_FAIL;
}
esp_err_t processing_outbox_catalog(size_t index, char name[65], size_t *count)
{
    if (!name || !count) return ESP_ERR_INVALID_ARG;
    *count = 0; name[0] = 0;
    DIR *dir = opendir(RECORDING_DIR);
    if (!dir) return ESP_FAIL;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        size_t n = strlen(entry->d_name);
        if (n < 5 || n > 64 || strcmp(entry->d_name + n - 4, ".job")) continue;
        char candidate[65];
        strcpy(candidate, entry->d_name);
        strcpy(candidate + n - 4, ".wav");
        if (!storage_valid_name(candidate)) continue;
        if ((*count)++ == index) strcpy(name, candidate);
    }
    closedir(dir);
    return ESP_OK;
}
