#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
/* Read-only activity (0..255), published at most every 100ms. Output is gated
   by actual played-sample advancement, not queued prefill or the wire state.
   Stale levels expire after 250ms; stop/inactive/new generations clear them. */
typedef struct {
    bool active;
    esp_err_t error;
    const char *state;
    uint32_t generation;
    uint8_t microphone_level, speaker_level;
    bool playback_active;
} voice_status_t;
esp_err_t voice_client_start(void);
void voice_client_stop(void);
voice_status_t voice_client_status(void);
