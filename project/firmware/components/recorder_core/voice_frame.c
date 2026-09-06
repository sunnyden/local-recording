#include "recorder_core.h"
#include <string.h>

static uint64_t getle(const uint8_t *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= (uint64_t)p[i] << (i * 8);
    return value;
}
static void putle(uint8_t *p, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) p[i] = value >> (i * 8);
}
bool voice_decode(const uint8_t *p, size_t len, voice_frame_t *f)
{
    if (!p || !f || len <= 24 || len > VOICE_MAX_PACKET || (len & 1) ||
        memcmp(p, "ERV1", 4) || p[4] != 1 || (p[5] != 1 && p[5] != 2) ||
        p[6] || p[7]) return false;
    *f = (voice_frame_t){.kind = p[5], .epoch = getle(p + 8, 4),
        .sequence = getle(p + 12, 4), .sample = getle(p + 16, 8),
        .pcm = p + 24, .samples = (len - 24) / 2};
    return (f->kind == 1 && f->epoch == 0) || (f->kind == 2 && f->epoch != 0);
}
bool voice_encode(uint8_t *p, size_t capacity, const voice_frame_t *f)
{
    if (!p || !f || !f->pcm || !f->samples || f->samples > PCM_SAMPLES ||
        capacity < 24 + f->samples * 2 ||
        !((f->kind == 1 && !f->epoch) || (f->kind == 2 && f->epoch))) return false;
    memcpy(p, "ERV1", 4);
    p[4] = 1; p[5] = f->kind; p[6] = 0; p[7] = 0;
    putle(p + 8, f->epoch, 4);
    putle(p + 12, f->sequence, 4);
    putle(p + 16, f->sample, 8);
    memmove(p + 24, f->pcm, f->samples * 2);
    return true;
}
bool voice_advance(voice_frame_t *expected, const voice_frame_t *frame)
{
    if (!expected || !frame || expected->kind != frame->kind ||
        expected->epoch != frame->epoch || expected->sequence != frame->sequence ||
        expected->sample != frame->sample || frame->samples == 0 ||
        frame->samples > PCM_SAMPLES || expected->sequence == UINT32_MAX ||
        expected->sample > UINT64_MAX - frame->samples) return false;
    ++expected->sequence;
    expected->sample += frame->samples;
    return true;
}
bool json_has_nul(const uint8_t *data, size_t length)
{
    if (!data) return true;
    for (size_t i = 0; i < length; ++i) {
        if (!data[i]) return true;
        if (data[i] != '\\' || ++i >= length) continue;
        if (data[i] == 'u' && length - i >= 5 &&
            !memcmp(data + i + 1, "0000", 4)) return true;
    }
    return false;
}
