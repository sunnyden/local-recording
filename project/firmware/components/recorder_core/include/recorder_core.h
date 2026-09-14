#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PCM_RATE 16000u
#define PCM_SAMPLES 320u
#define PCM_BYTES (PCM_SAMPLES * 2u)
#define OPUS_RATE 48000u
#define OPUS_PACKET_MAX 1275u
#define OPUS_PAGE_PACKETS 50u
#define OGG_BODY_CAPACITY 8192u
#define OPUS_MAX_FILE_BYTES (16u * 1024u * 1024u)
#define OPUS_MAX_SAMPLES (PCM_RATE * 1800u)
#define VOICE_HEADER_SIZE 24u
#define VOICE_MAX_PACKET (VOICE_HEADER_SIZE + PCM_BYTES)
#define UPLOAD_RANGE_SIZE (320u * 1024u)

typedef struct {
    FILE *file;
    uint32_t serial, sequence, last_page_offset;
    uint64_t granule, committed_granule;
    uint16_t pre_skip, segment_count, packet_count;
    size_t body_size;
    uint8_t segments[255];
    uint8_t body[OGG_BODY_CAPACITY];
} ogg_opus_writer_t;

typedef struct {
    uint32_t serial, next_sequence, bytes, last_page_offset;
    uint64_t granule;
    uint16_t pre_skip;
    uint32_t samples;
    bool eos;
} ogg_opus_info_t;

bool ogg_opus_writer_begin(ogg_opus_writer_t *writer, FILE *file, uint32_t serial,
                           uint16_t pre_skip);
bool ogg_opus_writer_packet(ogg_opus_writer_t *writer, const uint8_t *packet,
                            size_t bytes, uint32_t samples);
bool ogg_opus_writer_finish(ogg_opus_writer_t *writer, uint32_t output_samples);
bool ogg_opus_parse(FILE *file, uint32_t limit, bool require_eos, ogg_opus_info_t *info);
bool ogg_opus_repair(FILE *file, uint32_t safe_bytes, uint32_t last_page_offset);
void recording_checkpoint_encode(uint8_t out[24], uint32_t bytes, uint32_t page_offset);
bool recording_checkpoint_decode(const uint8_t data[24], uint32_t *bytes,
                                 uint32_t *page_offset);
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
