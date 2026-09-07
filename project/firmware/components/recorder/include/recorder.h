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
    uint32_t checkpoint;
    unsigned journal_slot;
} recording_file_t;
esp_err_t storage_recover(unsigned *repaired, unsigned *failed);
esp_err_t storage_begin(recording_file_t *recording);
esp_err_t storage_append(recording_file_t *recording, const void *pcm, size_t bytes);
esp_err_t storage_finish(recording_file_t *recording);
esp_err_t storage_catalog(size_t index, char *name, size_t capacity, size_t *count);
bool storage_valid_name(const char *name);
esp_err_t storage_recording_time(const char *name, recording_time_t *stamp);

typedef enum { LOCAL_IDLE, LOCAL_RECORD, LOCAL_PLAY, LOCAL_STOPPING } local_mode_t;
typedef struct {
    local_mode_t mode;
    esp_err_t error;
    uint32_t samples;
    uint32_t queue_peak;
    uint32_t overruns;
} local_status_t;
esp_err_t local_audio_init(void);
esp_err_t local_record_start(void);
esp_err_t local_play_start(const char *name);
void local_stop(void);
local_status_t local_status(void);
