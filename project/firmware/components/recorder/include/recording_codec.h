#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct recording_encoder recording_encoder_t;
typedef struct recording_decoder recording_decoder_t;

#ifdef __cplusplus
extern "C" {
#endif
recording_encoder_t *recording_encoder_create(uint16_t *pre_skip_48k);
esp_err_t recording_encoder_encode(recording_encoder_t *encoder, const int16_t *pcm,
                                   size_t samples, uint8_t *packet, size_t capacity,
                                   size_t *bytes);
void recording_encoder_destroy(recording_encoder_t *encoder);

recording_decoder_t *recording_decoder_create(void);
esp_err_t recording_decoder_read(recording_decoder_t *decoder, FILE *file, int16_t *pcm,
                                 size_t capacity, size_t *samples);
void recording_decoder_destroy(recording_decoder_t *decoder);
#ifdef __cplusplus
}
#endif
