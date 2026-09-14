#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "recorder_core.h"
#include "recording_name.h"

typedef struct {
    FILE *file;
    FILE *journal;
    char path[128];
    uint32_t bytes;
    uint32_t samples;
    uint32_t checkpoint;
    uint32_t checkpoint_page;
    unsigned journal_slot;
    ogg_opus_writer_t *writer;
} recording_file_t;
esp_err_t storage_recover(unsigned *repaired, unsigned *failed);
esp_err_t storage_begin(recording_file_t *recording, uint16_t pre_skip);
esp_err_t storage_append(recording_file_t *recording, const void *packet, size_t bytes,
                         uint32_t samples);
esp_err_t storage_append_padding(recording_file_t *recording, const void *packet,
                                 size_t bytes, uint32_t samples);
esp_err_t storage_finish(recording_file_t *recording);
esp_err_t storage_abort(recording_file_t *recording);
esp_err_t storage_catalog(size_t index, char *name, size_t capacity, size_t *count);
bool storage_valid_name(const char *name);
esp_err_t storage_recording_time(const char *name, recording_time_t *stamp);

typedef enum { LOCAL_IDLE, LOCAL_RECORD, LOCAL_PLAY, LOCAL_STOPPING } local_mode_t;
/* Read-only copy. Activity is an integer peak (0..255), not a waveform;
   it expires after 250ms without PCM. Generation changes on a new start.
   total_samples is zero until the playback owner parses a known Opus length. */
typedef struct {
    local_mode_t mode;
    esp_err_t error;
    uint32_t samples;
    uint32_t queue_peak;
    uint32_t overruns;
    uint32_t total_samples, generation, file_bytes;
    uint32_t codec_us_average, codec_us_max;
    uint8_t activity_level;
    char filename[65];
} local_status_t;
esp_err_t local_audio_init(void);
esp_err_t local_record_start(void);
esp_err_t local_play_start(const char *name);
void local_stop(void);
local_status_t local_status(void);
