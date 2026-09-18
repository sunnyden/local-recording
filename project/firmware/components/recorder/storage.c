#include "recorder.h"
#include "board.h"
#include "esp_heap_caps.h"
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
    long end = ftell(r->file);
    if (end <= 0 || (uint64_t)end > UINT32_MAX || !r->writer ||
        r->writer->last_page_offset >= (uint32_t)end) return ESP_FAIL;
    r->bytes = (uint32_t)end;
    uint8_t data[24];
    recording_checkpoint_encode(data, r->bytes, r->writer->last_page_offset);
    if (durable(r->file) != ESP_OK || !r->journal ||
        fseek(r->journal, (r->journal_slot++ % 2) * sizeof(data), SEEK_SET) ||
        fwrite(data, 1, sizeof(data), r->journal) != sizeof(data) ||
        durable(r->journal) != ESP_OK)
        return ESP_FAIL;
    r->checkpoint = r->bytes;
    r->checkpoint_page = r->writer->last_page_offset;
    return ESP_OK;
}
esp_err_t storage_begin(recording_file_t *r, uint16_t pre_skip)
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
        if (!r->file) {
            close(fd);
            storage_abort(r);
            return ESP_FAIL;
        }
        int metadata_fd = open(meta, O_CREAT | O_EXCL | O_WRONLY, 0600);
        FILE *metadata = metadata_fd < 0 ? NULL : fdopen(metadata_fd, "wb");
        if (!metadata) {
            if (metadata_fd >= 0) close(metadata_fd);
            storage_abort(r);
            return ESP_FAIL;
        }
        bool saved = fprintf(metadata, "RMT1 %d %lld %d\n", stamp.clock_valid,
            (long long)stamp.utc, stamp.offset_minutes) > 0 && durable(metadata) == ESP_OK;
        if (fclose(metadata)) saved = false;
        if (!saved) {
            storage_abort(r);
            return ESP_FAIL;
        }
        r->writer = heap_caps_calloc(1, sizeof(*r->writer),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!r->writer ||
            !ogg_opus_writer_begin(r->writer, r->file, (uint32_t)random_id | 1u, pre_skip) ||
            durable(r->file) != ESP_OK) {
            storage_abort(r);
            return ESP_FAIL;
        }
        char path[128];
        journal_path(r->path, path);
        r->journal = fopen(path, "wb+");
        if (!r->journal) {
            storage_abort(r);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}
static esp_err_t append_packet(recording_file_t *r, const void *packet, size_t bytes,
                               uint32_t samples, bool output, bool commit)
{
    if (!r || !r->file || !r->writer || !packet || !bytes || samples != PCM_SAMPLES)
        return ESP_ERR_INVALID_ARG;
    long current = ftell(r->file);
    if (current < 0 || (uint64_t)current + r->writer->body_size + bytes +
        27u + 255u > OPUS_MAX_FILE_BYTES) return ESP_ERR_INVALID_SIZE;
    if (output && samples > UINT32_MAX - r->samples) return ESP_ERR_INVALID_SIZE;
    uint32_t previous_page = r->writer->last_page_offset;
    if (!ogg_opus_writer_packet(r->writer, packet, bytes, samples)) return ESP_FAIL;
    if (output) r->samples += samples;
    if (commit && r->writer->last_page_offset != previous_page &&
        r->writer->last_page_offset != r->checkpoint_page) {
        return checkpoint(r);
    }
    return ESP_OK;
}
esp_err_t storage_append(recording_file_t *r, const void *packet, size_t bytes,
                         uint32_t samples)
{
    return append_packet(r, packet, bytes, samples, true, true);
}
esp_err_t storage_append_padding(recording_file_t *r, const void *packet,
                                 size_t bytes, uint32_t samples)
{
    return append_packet(r, packet, bytes, samples, false, true);
}
esp_err_t storage_append_history(recording_file_t *r, const void *packet,
                                 size_t bytes, uint32_t samples)
{
    return append_packet(r, packet, bytes, samples, true, false);
}
esp_err_t storage_history_done(recording_file_t *r)
{
    if (!r || !r->writer)
        return ESP_ERR_INVALID_STATE;
    if (!r->writer->committed_granule) return ESP_OK;
    return checkpoint(r);
}
esp_err_t storage_finish(recording_file_t *r)
{
    if (!r || !r->file || !r->writer) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ogg_opus_writer_finish(r->writer, r->samples) &&
        durable(r->file) == ESP_OK
        ? ESP_OK : ESP_FAIL;
    long end = ftell(r->file);
    if (end <= 0 || (uint64_t)end > UINT32_MAX) err = ESP_FAIL;
    else r->bytes = (uint32_t)end;
    heap_caps_free(r->writer);
    r->writer = NULL;
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
    strcpy(extension, ".opus");
    struct stat st;
    if (stat(final, &st) == 0) return ESP_ERR_INVALID_STATE;
    if (rename(r->path, final)) return ESP_FAIL;
    char journal[128];
    journal_path(r->path, journal);
    if (unlink(journal) && errno != ENOENT) return ESP_FAIL;
    return ESP_OK;
}
esp_err_t storage_abort(recording_file_t *r)
{
    if (!r) return ESP_ERR_INVALID_ARG;
    bool ok = true;
    if (r->writer) {
        heap_caps_free(r->writer);
        r->writer = NULL;
    }
    if (r->file) {
        if (fclose(r->file)) ok = false;
        r->file = NULL;
    }
    if (r->journal) {
        if (fclose(r->journal)) ok = false;
        r->journal = NULL;
    }
    char journal[128], metadata[128];
    journal_path(r->path, journal);
    metadata_path(r->path, metadata);
    if (unlink(r->path) && errno != ENOENT) ok = false;
    if (unlink(journal) && errno != ENOENT) ok = false;
    if (unlink(metadata) && errno != ENOENT) ok = false;
    return ok ? ESP_OK : ESP_FAIL;
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
        uint32_t safe_bytes = 0, safe_page = 0;
        bool found = false;
        if (checkpoints) {
            uint8_t data[24];
            for (unsigned i = 0; i < 2 &&
                 fread(data, 1, sizeof(data), checkpoints) == sizeof(data); ++i) {
                uint32_t bytes, page;
                if (recording_checkpoint_decode(data, &bytes, &page) &&
                    (!found || bytes > safe_bytes)) {
                    safe_bytes = bytes; safe_page = page; found = true;
                }
            }
            fclose(checkpoints);
        }
        if (!found || !ogg_opus_repair(r.file, safe_bytes, safe_page) ||
            durable(r.file) != ESP_OK) {
            fclose(r.file); ++*failed; continue;
        }
        if (fclose(r.file)) { ++*failed; continue; }
        r.file = NULL;
        char final[128];
        snprintf(final, sizeof(final), "%s", r.path);
        strcpy(strrchr(final, '.'), ".opus");
        struct stat st;
        if (stat(final, &st) == 0 || rename(r.path, final)) { ++*failed; continue; }
        if (unlink(journal) && errno != ENOENT) { ++*failed; continue; }
        ++*repaired;
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
esp_err_t storage_open_recording(const char *name, FILE **file, recording_info_t *info)
{
    if (!storage_valid_name(name) || !file || !info) return ESP_ERR_INVALID_ARG;
    *file = NULL;
    memset(info, 0, sizeof(*info));
    char path[128];
    snprintf(path, sizeof(path), RECORDING_DIR "/%s", name);
    FILE *opened = fopen(path, "rb");
    if (!opened) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    ogg_opus_info_t opus;
    if (!ogg_opus_parse(opened, 0, true, &opus) || fseek(opened, 0, SEEK_SET)) {
        fclose(opened);
        return ESP_ERR_NOT_SUPPORTED;
    }
    info->bytes = opus.bytes;
    info->samples = opus.samples;
    recording_time_t stamp;
    esp_err_t time_err = storage_recording_time(name, &stamp);
    if (time_err == ESP_OK) {
        info->has_time = true;
        info->time = stamp;
    } else if (time_err != ESP_ERR_NOT_FOUND) {
        fclose(opened);
        return time_err;
    }
    *file = opened;
    return ESP_OK;
}
esp_err_t storage_recording_info(const char *name, recording_info_t *info)
{
    FILE *file = NULL;
    esp_err_t err = storage_open_recording(name, &file, info);
    if (file && fclose(file) && err == ESP_OK) err = ESP_FAIL;
    return err;
}
esp_err_t storage_catalog_info(size_t index, char *name, size_t capacity,
                               recording_info_t *info, size_t *count)
{
    if (!name || !capacity || !info || !count) return ESP_ERR_INVALID_ARG;
    DIR *dir = opendir(RECORDING_DIR);
    if (!dir) return ESP_FAIL;
    *count = 0;
    name[0] = 0;
    memset(info, 0, sizeof(*info));
    esp_err_t err = ESP_OK;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!storage_valid_name(entry->d_name)) continue;
        recording_info_t candidate;
        esp_err_t candidate_err = storage_recording_info(entry->d_name, &candidate);
        if (candidate_err == ESP_ERR_NOT_SUPPORTED || candidate_err == ESP_ERR_NOT_FOUND)
            continue;
        if (candidate_err != ESP_OK) { err = candidate_err; break; }
        if ((*count)++ == index) {
            if (strlen(entry->d_name) >= capacity) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            strcpy(name, entry->d_name);
            *info = candidate;
        }
    }
    closedir(dir);
    return err;
}
esp_err_t storage_catalog_page(size_t offset, size_t limit,
                               recording_catalog_item_t *items, size_t capacity,
                               size_t *count, size_t *total)
{
    if (!items || !capacity || !count || !total || limit > capacity)
        return ESP_ERR_INVALID_ARG;
    DIR *dir = opendir(RECORDING_DIR);
    if (!dir) return ESP_FAIL;
    *count = *total = 0;
    esp_err_t err = ESP_OK;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!storage_valid_name(entry->d_name)) continue;
        recording_info_t info;
        esp_err_t item_err = storage_recording_info(entry->d_name, &info);
        if (item_err == ESP_ERR_NOT_SUPPORTED || item_err == ESP_ERR_NOT_FOUND)
            continue;
        if (item_err != ESP_OK) { err = item_err; break; }
        size_t valid_index = (*total)++;
        if (valid_index < offset || *count >= limit) continue;
        recording_catalog_item_t *item = &items[(*count)++];
        strcpy(item->name, entry->d_name);
        item->info = info;
    }
    closedir(dir);
    return err;
}
