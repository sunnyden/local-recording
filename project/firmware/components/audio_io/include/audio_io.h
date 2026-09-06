#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define AUDIO_VOICE_MAX_PENDING_SAMPLES 8000u
esp_err_t audio_init(void);
esp_err_t audio_start(bool capture, bool playback);
esp_err_t audio_stop(void);
esp_err_t audio_read(int16_t *mono, size_t samples);
esp_err_t audio_write(const int16_t *mono, size_t samples);
uint32_t audio_overruns(void);
esp_err_t audio_voice_start(void);
esp_err_t audio_voice_epoch(uint32_t epoch);
esp_err_t audio_voice_enqueue(uint32_t epoch, const int16_t *pcm, size_t samples);
esp_err_t audio_voice_clear(uint32_t epoch, uint64_t *played);
uint64_t audio_voice_played(uint32_t epoch);
