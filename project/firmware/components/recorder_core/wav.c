#include "recorder_core.h"
#include <limits.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define truncate_file(fd, n) _chsize_s(fd, n)
#else
#include <unistd.h>
#define truncate_file(fd, n) ftruncate(fd, n)
#endif

static uint16_t get16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p)
{
    return get16(p) | ((uint32_t)get16(p + 2) << 16);
}
static void put32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i));
}

bool wav_header(uint8_t out[WAV_HEADER_SIZE], uint32_t bytes)
{
    if (!out || (bytes & 1) || bytes > WAV_MAX_DATA) return false;
    memset(out, 0, WAV_HEADER_SIZE);
    memcpy(out, "RIFF", 4);
    put32(out + 4, bytes + 36);
    memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 16);
    out[20] = 1;
    out[22] = 1;
    put32(out + 24, PCM_RATE);
    put32(out + 28, PCM_RATE * 2);
    out[32] = 2;
    out[34] = 16;
    memcpy(out + 36, "data", 4);
    put32(out + 40, bytes);
    return true;
}

bool wav_parse(FILE *f, wav_info_t *info)
{
    uint8_t h[16];
    if (!f || !info || fseek(f, 0, SEEK_END)) return false;
    long end = ftell(f);
    if (end < 44 || fseek(f, 0, SEEK_SET) || fread(h, 1, 12, f) != 12)
        return false;
    uint64_t riff_end = (uint64_t)get32(h + 4) + 8;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4) ||
        riff_end > (uint64_t)end || riff_end < 44) return false;
    bool format = false;
    uint64_t pos = 12;
    for (unsigned chunks = 0; chunks < 128 && pos + 8 <= riff_end; ++chunks) {
        if (fread(h, 1, 8, f) != 8) return false;
        uint32_t size = get32(h + 4);
        pos += 8;
        if (pos + size > riff_end || pos + size > LONG_MAX) return false;
        if (!memcmp(h, "fmt ", 4)) {
            if (format || size < 16 || fread(h, 1, 16, f) != 16) return false;
            format = get16(h) == 1 && get16(h + 2) == 1 &&
                     get32(h + 4) == PCM_RATE && get32(h + 8) == PCM_RATE * 2 &&
                     get16(h + 12) == 2 && get16(h + 14) == 16;
            if (!format) return false;
        } else if (!memcmp(h, "data", 4)) {
            if (!format || (size & 1) || size > WAV_MAX_DATA) return false;
            *info = (wav_info_t){.offset = (uint32_t)pos, .bytes = size};
            return true;
        }
        pos += size + (size & 1u);
        if (pos > riff_end || fseek(f, (long)pos, SEEK_SET)) return false;
    }
    return false;
}

bool wav_repair_limit(FILE *f, uint32_t durable_bytes)
{
    uint8_t actual[44], expected[44];
    if (!f || fseek(f, 0, SEEK_END)) return false;
    long end = ftell(f);
    if (end < 44 || (uint64_t)(end - 44) > WAV_MAX_DATA) return false;
    if (fseek(f, 0, SEEK_SET) || fread(actual, 1, 44, f) != 44) return false;
    wav_header(expected, 0);
    /* Repair only our canonical temporary files, not arbitrary corrupt WAVs. */
    memcpy(actual + 4, expected + 4, 4);
    memcpy(actual + 40, expected + 40, 4);
    if (memcmp(actual, expected, 44)) return false;
    uint32_t bytes = (uint32_t)(end - 44) & ~1u;
    if (durable_bytes != UINT32_MAX) {
        if ((durable_bytes & 1) || durable_bytes > bytes) return false;
        bytes = durable_bytes;
    }
    if (fflush(f) || truncate_file(fileno(f), bytes + 44)) return false;
    wav_header(expected, bytes);
    return !fseek(f, 0, SEEK_SET) && fwrite(expected, 1, 44, f) == 44 && !fflush(f);
}
bool wav_repair(FILE *f) { return wav_repair_limit(f, UINT32_MAX); }

static uint32_t checksum(const uint8_t *data, size_t len)
{
    uint32_t crc = UINT32_MAX;
    while (len--) {
        crc ^= *data++;
        for (unsigned i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
void recording_checkpoint_encode(uint8_t out[16], uint32_t bytes)
{
    memcpy(out, "RCP1", 4);
    put32(out + 4, bytes);
    put32(out + 8, ~bytes);
    put32(out + 12, checksum(out, 12));
}
bool recording_checkpoint_decode(const uint8_t data[16], uint32_t *bytes)
{
    if (!data || !bytes || memcmp(data, "RCP1", 4) ||
        get32(data + 4) != ~get32(data + 8) ||
        get32(data + 12) != checksum(data, 12) ||
        (get32(data + 4) & 1) || get32(data + 4) > WAV_MAX_DATA) return false;
    *bytes = get32(data + 4);
    return true;
}

uint32_t upload_range_bytes(uint32_t total, uint32_t offset)
{
    if (offset >= total) return 0;
    uint32_t remaining = total - offset;
    return remaining < UPLOAD_RANGE_SIZE ? remaining : UPLOAD_RANGE_SIZE;
}
