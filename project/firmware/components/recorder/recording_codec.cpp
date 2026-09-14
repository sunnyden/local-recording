#include "recording_codec.h"
#include "recorder_core.h"
#include "micro_opus/ogg_opus_decoder.h"
#include "opus.h"
#include <new>

struct recording_encoder {
    OpusEncoder *opus;
};

struct recording_decoder {
    micro_opus::OggOpusDecoder opus;
    uint8_t input[4096];
    size_t offset;
    size_t available;
    bool eof;

    recording_decoder() : opus(true, PCM_RATE, 1), offset(0), available(0), eof(false) {}
};

extern "C" recording_encoder_t *recording_encoder_create(uint16_t *pre_skip_48k)
{
    if (!pre_skip_48k) return nullptr;
    recording_encoder *encoder = new (std::nothrow) recording_encoder{};
    if (!encoder) return nullptr;
    int error = OPUS_OK;
    encoder->opus = opus_encoder_create(PCM_RATE, 1, OPUS_APPLICATION_AUDIO, &error);
    opus_int32 lookahead = 0;
    if (!encoder->opus || error != OPUS_OK ||
        opus_encoder_ctl(encoder->opus, OPUS_SET_BITRATE(24000)) != OPUS_OK ||
        opus_encoder_ctl(encoder->opus, OPUS_SET_VBR(1)) != OPUS_OK ||
        opus_encoder_ctl(encoder->opus, OPUS_SET_VBR_CONSTRAINT(1)) != OPUS_OK ||
        opus_encoder_ctl(encoder->opus, OPUS_SET_COMPLEXITY(2)) != OPUS_OK ||
        opus_encoder_ctl(encoder->opus, OPUS_SET_INBAND_FEC(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder->opus, OPUS_SET_DTX(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder->opus, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK ||
        lookahead <= 0 || lookahead > UINT16_MAX / (OPUS_RATE / PCM_RATE)) {
        if (encoder->opus) opus_encoder_destroy(encoder->opus);
        delete encoder;
        return nullptr;
    }
    *pre_skip_48k = (uint16_t)(lookahead * (OPUS_RATE / PCM_RATE));
    return encoder;
}

extern "C" esp_err_t recording_encoder_encode(recording_encoder_t *encoder,
    const int16_t *pcm, size_t samples, uint8_t *packet, size_t capacity, size_t *bytes)
{
    if (!encoder || !encoder->opus || !pcm || samples != PCM_SAMPLES || !packet ||
        capacity < OPUS_PACKET_MAX || !bytes) return ESP_ERR_INVALID_ARG;
    int result = opus_encode(encoder->opus, pcm, (int)samples, packet, (opus_int32)capacity);
    if (result <= 0 || result > (int)OPUS_PACKET_MAX) return ESP_FAIL;
    *bytes = (size_t)result;
    return ESP_OK;
}

extern "C" void recording_encoder_destroy(recording_encoder_t *encoder)
{
    if (!encoder) return;
    if (encoder->opus) opus_encoder_destroy(encoder->opus);
    delete encoder;
}

extern "C" recording_decoder_t *recording_decoder_create(void)
{
    return new (std::nothrow) recording_decoder();
}

extern "C" esp_err_t recording_decoder_read(recording_decoder_t *decoder, FILE *file,
    int16_t *pcm, size_t capacity, size_t *samples)
{
    if (!decoder || !file || !pcm || capacity < PCM_SAMPLES || !samples)
        return ESP_ERR_INVALID_ARG;
    *samples = 0;
    for (;;) {
        if (decoder->offset == decoder->available && !decoder->eof) {
            decoder->available = fread(decoder->input, 1, sizeof(decoder->input), file);
            decoder->offset = 0;
            if (decoder->available < sizeof(decoder->input)) {
                if (ferror(file)) return ESP_FAIL;
                decoder->eof = true;
            }
        }
        if (decoder->offset == decoder->available)
            return decoder->eof ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        size_t consumed = 0, decoded = 0;
        auto result = decoder->opus.decode(
            decoder->input + decoder->offset, decoder->available - decoder->offset,
            reinterpret_cast<uint8_t *>(pcm), capacity * sizeof(*pcm), consumed, decoded);
        if (result != micro_opus::OGG_OPUS_OK) return ESP_ERR_INVALID_RESPONSE;
        if (!consumed && !decoded) return ESP_ERR_INVALID_RESPONSE;
        decoder->offset += consumed;
        if (decoded) {
            if (decoded > capacity) return ESP_ERR_INVALID_SIZE;
            *samples = decoded;
            return ESP_OK;
        }
    }
}

extern "C" void recording_decoder_destroy(recording_decoder_t *decoder)
{
    delete decoder;
}
