#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Activity only: no filtering or ownership of the original PCM. */
static inline uint8_t audio_meter_peak(const int16_t *pcm, size_t count)
{
    uint32_t peak = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t sample = pcm[i];
        uint32_t magnitude = sample < 0 ? (uint32_t)-sample : (uint32_t)sample;
        if (magnitude > peak) peak = magnitude;
    }
    return (uint8_t)(peak * 255u / 32768u);
}

typedef struct {
    uint32_t published_ms, observed_ms;
    uint8_t level, pending;
    bool initialized;
} audio_meter_t;

static inline void audio_meter_observe(audio_meter_t *meter, uint8_t peak, uint32_t now)
{
    if (meter->initialized && now - meter->observed_ms >= 250)
        *meter = (audio_meter_t){0};
    if (peak > meter->pending) meter->pending = peak;
    meter->observed_ms = now;
    if (!meter->initialized || now - meter->published_ms >= 100) {
        meter->level = meter->pending;
        meter->pending = 0;
        meter->published_ms = now;
        meter->initialized = true;
    }
}

static inline uint8_t audio_meter_level(const audio_meter_t *meter, uint32_t now)
{
    return meter->initialized && now - meter->observed_ms < 250 ? meter->level : 0;
}
