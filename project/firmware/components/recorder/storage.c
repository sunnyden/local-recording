#include "recorder.h"
#include "board.h"
#include "esp_random.h"
#include "sdkconfig.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#ifndef CONFIG_RECORDER_TIMEZONE_OFFSET_MINUTES
#define CONFIG_RECORDER_TIMEZONE_OFFSET_MINUTES 480
#endif

bool storage_valid_name(const char *name)
{
    return recording_name_valid(name, false);
}
static esp_err_t durable(FILE *f)
{
    return fflush(f) == 0 && fsync(fileno(f)) == 0 ? ESP_OK : ESP_FAIL;
}
static esp_err_t finalize(recording_file_t *r)
{
    uint8_t header[44];
    if (fflush(r->file) || ftruncate(fileno(r->file), r->bytes + 44) ||
        !wav_header(header, r->bytes) || fseek(r->file, 0, SEEK_SET) ||
        fwrite(header, 1, 44, r->file) != 44) return ESP_FAIL;
    return durable(r->file);
}
static void journal_path(const char *path, char output[128])
{
    snprintf(output, 128, "%s", path);
    char *ext = strrchr(output, '.');
    if (ext) strcpy(ext, ".ckp");
}
static void metadata_path(const char *path, char output[128])
{
    journal_path(path, output);
    strcpy(strrchr(output, '.'), ".meta");
}
esp_err_t storage_recording_time(const char *name, recording_time_t *stamp)
{
    if (!storage_valid_name(name) || !stamp) return ESP_ERR_INVALID_ARG;
    memset(stamp, 0, sizeof(*stamp));
    char path[128], meta[128];
    snprintf(path, sizeof(path), RECORDING_DIR "/%s", name);
    metadata_path(path, meta);
    FILE *f = fopen(meta, "rb");
    if (!f) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    char data[80] = {0};
    size_t length = fread(data, 1, sizeof(data) - 1, f);
    bool read_ok = !ferror(f) && feof(f);
    if (fclose(f)) read_ok = false;
    long long utc = 0;
    int valid = 0, offset = 0;
    int consumed = 0;
    int fields = sscanf(data, "RMT1 %d %lld %d%n", &valid, &utc, &offset, &consumed);
    bool ok = read_ok && fields == 3 && consumed > 0 && (size_t)consumed + 1 == length &&
        data[consumed] == '\n' && (valid == 0 || valid == 1) &&
        offset >= -720 && offset <= 840 && (!valid || (utc > 1735689600 && utc <= 253402214399LL));
    if (!ok) return ESP_ERR_INVALID_RESPONSE;
    *stamp = (recording_time_t){.utc = utc, .offset_minutes = offset, .clock_valid = valid};
    return ESP_OK;
}
static esp_err_t checkpoint(recording_file_t *r)
{
    uint8_t data[16];
    recording_checkpoint_encode(data, r->bytes);
    if (durable(r->file) != ESP_OK || !r->journal ||
        fseek(r->journal, (r->journal_slot++ % 2) * 16, SEEK_SET) ||
        fwrite(data, 1, 16, r->journal) != 16 || durable(r->journal) != ESP_OK)
        return ESP_FAIL;
    r->checkpoint = r->bytes;
    return ESP_OK;
}
esp_err_t storage_begin(recording_file_t *r)
{
    if (!r) return ESP_ERR_INVALID_ARG;
    memset(r, 0, sizeof(*r));
    recording_time_t stamp = {.utc = time(NULL),
        .offset_minutes = CONFIG_RECORDER_TIMEZONE_OFFSET_MINUTES};
    stamp.clock_valid = stamp.utc > 1735689600;
    uint64_t random_id = ((uint64_t)esp_random() << 32) | esp_random();
    for (unsigned attempt = 0; attempt <= 999; ++attempt) {
        char name[65], final[128], meta[128], journal[128];
        if (!recording_name_format(name, sizeof(name), &stamp, random_id, attempt))
            return ESP_ERR_INVALID_ARG;
        snprintf(final, sizeof(final), RECORDING_DIR "/%s", name);
        snprintf(r->path, sizeof(r->path), "%s", final);
        strcpy(strrchr(r->path, '.'), ".part");
        metadata_path(r->path, meta);
        journal_path(r->path, journal);
        struct stat st;
        if (!stat(final, &st) || !stat(meta, &st) || !stat(journal, &st)) continue;
        int fd = open(r->path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) { if (errno == EEXIST) continue; return ESP_FAIL; }
        r->file = fdopen(fd, "wb+");
        if (!r->file) { close(fd); return ESP_FAIL; }
        int metadata_fd = open(meta, O_CREAT | O_EXCL | O_WRONLY, 0600);
        FILE *metadata = metadata_fd < 0 ? NULL : fdopen(metadata_fd, "wb");
        if (!metadata) {
            if (metadata_fd >= 0) close(metadata_fd);
            fclose(r->file); r->file = NULL; return ESP_FAIL;
        }
        bool saved = fprintf(metadata, "RMT1 %d %lld %d\n", stamp.clock_valid,
            (long long)stamp.utc, stamp.offset_minutes) > 0 && durable(metadata) == ESP_OK;
        if (fclose(metadata)) saved = false;
        if (!saved) { fclose(r->file); r->file = NULL; return ESP_FAIL; }
        uint8_t header[44];
        wav_header(header, 0);
        if (fwrite(header, 1, 44, r->file) != 44 || durable(r->file) != ESP_OK) {
            fclose(r->file); r->file = NULL; return ESP_FAIL;
        }
        char path[128];
        journal_path(r->path, path);
        r->journal = fopen(path, "wb+");
        if (!r->journal || checkpoint(r) != ESP_OK) {
            if (r->journal) fclose(r->journal);
            fclose(r->file); r->file = r->journal = NULL; return ESP_FAIL;
        }
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}
esp_err_t storage_append(recording_file_t *r, const void *pcm, size_t bytes)
{
    if (!r || !r->file || !pcm || !bytes || (bytes & 1)) return ESP_ERR_INVALID_ARG;
    if (bytes > WAV_MAX_DATA - r->bytes) return ESP_ERR_INVALID_SIZE;
    size_t written = fwrite(pcm, 1, bytes, r->file);
    r->bytes += written & ~1u;
    if (written != bytes) return ESP_FAIL;
    if (r->bytes - r->checkpoint >= PCM_RATE * 2) {
        esp_err_t err = checkpoint(r);
        if (err != ESP_OK) return err;
        r->checkpoint = r->bytes;
    }
    return ESP_OK;
}
esp_err_t storage_finish(recording_file_t *r)
{
    if (!r || !r->file) return ESP_ERR_INVALID_STATE;
    esp_err_t err = finalize(r);
    if (fclose(r->file) && err == ESP_OK) err = ESP_FAIL;
    r->file = NULL;
    if (r->journal) {
        if (fclose(r->journal) && err == ESP_OK) err = ESP_FAIL;
        r->journal = NULL;
    }
    if (err != ESP_OK) return err; /* Leave .part visibly recoverable. */
    char final[128];
    snprintf(final, sizeof(final), "%s", r->path);
    char *extension = strrchr(final, '.');
    if (!extension) return ESP_FAIL;
    strcpy(extension, ".wav");
    struct stat st;
    if (stat(final, &st) == 0) return ESP_ERR_INVALID_STATE;
    if (rename(r->path, final)) return ESP_FAIL;
    char journal[128];
    journal_path(r->path, journal);
    if (unlink(journal) && errno != ENOENT) return ESP_FAIL;
    return ESP_OK;
}
esp_err_t storage_recover(unsigned *repaired, unsigned *failed)
{
    *repaired = *failed = 0;
    DIR *dir = opendir(RECORDING_DIR);
    if (!dir) return ESP_FAIL;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!recording_name_valid(entry->d_name, true)) continue;
        recording_file_t r = {0};
        snprintf(r.path, sizeof(r.path), RECORDING_DIR "/%.64s", entry->d_name);
        r.file = fopen(r.path, "rb+");
        if (!r.file) { ++*failed; continue; }
        char journal[128];
        journal_path(r.path, journal);
        FILE *checkpoints = fopen(journal, "rb");
        uint32_t safe_bytes = 0;
        bool found = false;
        if (checkpoints) {
            uint8_t data[16];
            for (unsigned i = 0; i < 2 && fread(data, 1, 16, checkpoints) == 16; ++i) {
                uint32_t bytes;
                if (recording_checkpoint_decode(data, &bytes) && (!found || bytes > safe_bytes)) {
                    safe_bytes = bytes; found = true;
                }
            }
            fclose(checkpoints);
        }
        if (!found || !wav_repair_limit(r.file, safe_bytes) || durable(r.file) != ESP_OK) {
            fclose(r.file); ++*failed; continue;
        }
        wav_info_t info;
        if (!wav_parse(r.file, &info)) { fclose(r.file); ++*failed; continue; }
        r.bytes = info.bytes;
        if (storage_finish(&r) == ESP_OK) ++*repaired; else ++*failed;
    }
    closedir(dir);
    return *failed ? ESP_FAIL : ESP_OK;
}
esp_err_t storage_catalog(size_t index, char *name, size_t capacity, size_t *count)
{
    if (!name || !capacity || !count) return ESP_ERR_INVALID_ARG;
    DIR *dir = opendir(RECORDING_DIR);
    if (!dir) return ESP_FAIL;
    *count = 0;
    name[0] = '\0';
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!storage_valid_name(entry->d_name)) continue;
        if ((*count)++ == index) {
            if (strlen(entry->d_name) >= capacity) { closedir(dir); return ESP_ERR_INVALID_SIZE; }
            strcpy(name, entry->d_name);
        }
    }
    closedir(dir);
    return ESP_OK;
}
