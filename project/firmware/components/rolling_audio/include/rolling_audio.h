#pragma once
#include "esp_err.h"
#include "recorder.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROLLING_MAX_FRAMES 1500u
#define ROLLING_DATA_BYTES (240u * 1024u)

typedef struct {
    bool active;
    bool ready;
    esp_err_t error;
    uint32_t samples;
    uint32_t frames;
    uint32_t bytes;
    uint32_t queue_peak;
    uint32_t overruns;
    uint32_t generation;
} rolling_status_t;

esp_err_t rolling_audio_init(void);
esp_err_t rolling_audio_set_enabled(bool enabled);
esp_err_t rolling_audio_take(rolling_snapshot_t *snapshot);
void rolling_snapshot_release(rolling_snapshot_t *snapshot);
esp_err_t rolling_snapshot_ogg(const rolling_snapshot_t *snapshot,
                               uint8_t **data, size_t *bytes);
rolling_status_t rolling_audio_status(void);
