#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PCM_RATE 16000u
#define PCM_SAMPLES 320u
#define PCM_BYTES (PCM_SAMPLES * 2u)
#define WAV_HEADER_SIZE 44u
#define WAV_MAX_DATA 0x7fff0000u
#define VOICE_HEADER_SIZE 24u
#define VOICE_MAX_PACKET (VOICE_HEADER_SIZE + PCM_BYTES)
#define UPLOAD_RANGE_SIZE (320u * 1024u)

typedef struct {
    uint32_t offset;
    uint32_t bytes;
} wav_info_t;

bool wav_header(uint8_t out[WAV_HEADER_SIZE], uint32_t bytes);
bool wav_parse(FILE *file, wav_info_t *info);
bool wav_repair(FILE *file);
bool wav_repair_limit(FILE *file, uint32_t durable_bytes);
void recording_checkpoint_encode(uint8_t out[16], uint32_t bytes);
bool recording_checkpoint_decode(const uint8_t data[16], uint32_t *bytes);
uint32_t upload_range_bytes(uint32_t total, uint32_t offset);

typedef struct {
    uint32_t epoch, sequence;
    uint64_t sample;
    uint8_t kind;
    const uint8_t *pcm;
    size_t samples;
} voice_frame_t;

bool voice_decode(const uint8_t *data, size_t len, voice_frame_t *frame);
bool voice_encode(uint8_t *data, size_t capacity, const voice_frame_t *frame);
bool voice_advance(voice_frame_t *expected, const voice_frame_t *frame);
bool json_has_nul(const uint8_t *data, size_t length);
