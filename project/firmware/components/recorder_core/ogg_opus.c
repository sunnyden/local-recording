#include "recorder_core.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define OGG_HEADER 27u
typedef struct {
    uint8_t header[OGG_HEADER + 255];
    uint8_t body[OGG_BODY_CAPACITY];
} ogg_scratch_t;
static ogg_scratch_t *scratch_create(void)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(sizeof(ogg_scratch_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(sizeof(ogg_scratch_t));
#endif
}
static void scratch_destroy(void *scratch)
{
#ifdef ESP_PLATFORM
    heap_caps_free(scratch);
#else
    free(scratch);
#endif
}

static uint16_t get16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p)
{
    return get16(p) | ((uint32_t)get16(p + 2) << 16);
}
static uint64_t get64(const uint8_t *p)
{
    return get32(p) | ((uint64_t)get32(p + 4) << 32);
}
static void put16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
}
static void put32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i));
}
static void put64(uint8_t *p, uint64_t n)
{
    put32(p, (uint32_t)n); put32(p + 4, (uint32_t)(n >> 32));
}
static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t length)
{
    while (length--) {
        crc ^= (uint32_t)*data++ << 24;
        for (unsigned i = 0; i < 8; ++i)
            crc = (crc << 1) ^ (0x04c11db7u & (0u - (crc >> 31)));
    }
    return crc;
}
static uint32_t page_crc(const uint8_t *header, size_t header_size,
                         const uint8_t *body, size_t body_size)
{
    uint8_t copy[OGG_HEADER + 255];
    memcpy(copy, header, header_size);
    memset(copy + 22, 0, 4);
    return crc_update(crc_update(0, copy, header_size), body, body_size);
}
static bool write_page(ogg_opus_writer_t *w, uint8_t flags, uint64_t granule,
                       const uint8_t *segments, size_t segment_count,
                       const uint8_t *body, size_t body_size)
{
    if (!w || !w->file || segment_count > 255 || body_size > OGG_BODY_CAPACITY)
        return false;
    long offset = ftell(w->file);
    if (offset < 0 || (uint64_t)offset > UINT32_MAX) return false;
    uint8_t header[OGG_HEADER + 255] = {0};
    memcpy(header, "OggS", 4);
    header[5] = flags;
    put64(header + 6, granule);
    put32(header + 14, w->serial);
    put32(header + 18, w->sequence++);
    header[26] = (uint8_t)segment_count;
    memcpy(header + OGG_HEADER, segments, segment_count);
    size_t header_size = OGG_HEADER + segment_count;
    put32(header + 22, page_crc(header, header_size, body, body_size));
    if (fwrite(header, 1, header_size, w->file) != header_size ||
        fwrite(body, 1, body_size, w->file) != body_size) return false;
    w->last_page_offset = (uint32_t)offset;
    if (granule != UINT64_MAX) w->committed_granule = granule;
    return true;
}
static bool write_packet_page(ogg_opus_writer_t *w, uint8_t flags, uint64_t granule,
                              const uint8_t *packet, size_t bytes)
{
    uint8_t lacing[6];
    size_t count = 0, remaining = bytes;
    do {
        if (count == sizeof(lacing)) return false;
        lacing[count++] = remaining >= 255 ? 255 : (uint8_t)remaining;
        if (remaining >= 255) remaining -= 255; else remaining = 0;
    } while (remaining || (bytes && bytes % 255 == 0));
    return write_page(w, flags, granule, lacing, count, packet, bytes);
}
static bool flush_audio(ogg_opus_writer_t *w, bool eos)
{
    if (!w->packet_count) return false;
    if (!write_page(w, eos ? 4 : 0, w->granule, w->segments, w->segment_count,
                    w->body, w->body_size)) return false;
    w->packet_count = w->segment_count = 0;
    w->body_size = 0;
    return true;
}
bool ogg_opus_writer_begin(ogg_opus_writer_t *w, FILE *file, uint32_t serial,
                           uint16_t pre_skip)
{
    if (!w || !file || !serial) return false;
    memset(w, 0, sizeof(*w));
    w->file = file; w->serial = serial; w->pre_skip = pre_skip;
    uint8_t head[19] = {'O','p','u','s','H','e','a','d',1,1};
    put16(head + 10, pre_skip);
    put32(head + 12, PCM_RATE);
    if (!write_packet_page(w, 2, 0, head, sizeof(head))) return false;
    static const uint8_t tags[] = {
        'O','p','u','s','T','a','g','s', 8,0,0,0,
        'r','e','c','o','r','d','e','r', 0,0,0,0
    };
    return write_packet_page(w, 0, 0, tags, sizeof(tags));
}
bool ogg_opus_writer_packet(ogg_opus_writer_t *w, const uint8_t *packet,
                            size_t bytes, uint32_t samples)
{
    if (!w || !w->file || !packet || !bytes || bytes > OPUS_PACKET_MAX ||
        !samples || samples > 960 || OPUS_RATE % PCM_RATE) return false;
    size_t laces = bytes / 255 + 1;
    if (w->packet_count &&
        (w->packet_count >= OPUS_PAGE_PACKETS ||
         w->segment_count + laces > 255 || w->body_size + bytes > sizeof(w->body))) {
        if (!flush_audio(w, false)) return false;
    }
    if (laces > 255 || bytes > sizeof(w->body)) return false;
    size_t remaining = bytes;
    for (size_t i = 0; i < laces; ++i) {
        uint8_t lace = remaining >= 255 ? 255 : (uint8_t)remaining;
        w->segments[w->segment_count++] = lace;
        remaining -= lace;
    }
    memcpy(w->body + w->body_size, packet, bytes);
    w->body_size += bytes;
    ++w->packet_count;
    w->granule += (uint64_t)samples * (OPUS_RATE / PCM_RATE);
    return true;
}
bool ogg_opus_writer_finish(ogg_opus_writer_t *w, uint32_t output_samples)
{
    if (!w || !w->file || !w->packet_count) return false;
    uint64_t final_granule = w->pre_skip +
        (uint64_t)output_samples * (OPUS_RATE / PCM_RATE);
    if (final_granule > w->granule ||
        final_granule < w->committed_granule)
        return false;
    uint64_t encoded_granule = w->granule;
    w->granule = final_granule;
    bool ok = flush_audio(w, true);
    if (!ok) w->granule = encoded_granule;
    return ok;
}

static bool read_exact(FILE *file, void *data, size_t bytes)
{
    return fread(data, 1, bytes, file) == bytes;
}
static bool tags_valid(const uint8_t *packet, size_t bytes)
{
    if (bytes < 16 || memcmp(packet, "OpusTags", 8)) return false;
    uint32_t vendor = get32(packet + 8);
    if (vendor > bytes - 16) return false;
    size_t offset = 12 + vendor;
    uint32_t comments = get32(packet + offset);
    offset += 4;
    for (uint32_t i = 0; i < comments; ++i) {
        if (offset + 4 > bytes) return false;
        uint32_t length = get32(packet + offset);
        offset += 4;
        if (length > bytes - offset) return false;
        offset += length;
    }
    return offset == bytes;
}
static bool packet_is_20ms(const uint8_t *packet, size_t bytes)
{
    if (!bytes) return false;
    unsigned config = packet[0] >> 3, code = packet[0] & 3;
    unsigned frame = config < 12 ? (unsigned[]){480, 960, 1920, 2880}[config & 3] :
        config < 16 ? (unsigned[]){480, 960}[config & 1] :
        (unsigned[]){120, 240, 480, 960}[config & 3];
    unsigned frames = code == 0 ? 1 : code < 3 ? 2 :
        bytes >= 2 ? packet[1] & 0x3f : 0;
    return frames && frame * frames == 960;
}
bool ogg_opus_parse(FILE *file, uint32_t limit, bool require_eos, ogg_opus_info_t *info)
{
#define PARSE_CHECK(condition) do { if (!(condition)) { \
    scratch_destroy(scratch); return false; } } while (0)
    ogg_scratch_t *scratch = scratch_create();
    if (!scratch) return false;
    PARSE_CHECK(file && info && !fseek(file, 0, SEEK_END));
    long physical = ftell(file);
    PARSE_CHECK(physical >= 0 && (uint64_t)physical <= OPUS_MAX_FILE_BYTES);
    uint32_t size = limit ? limit : (uint32_t)physical;
    PARSE_CHECK((uint64_t)physical >= size && size <= OPUS_MAX_FILE_BYTES &&
                !fseek(file, 0, SEEK_SET));
    memset(info, 0, sizeof(*info));
    uint32_t offset = 0, sequence = 0, serial = 0;
    unsigned packet_index = 0;
    bool eos = false;
    uint64_t prior_granule = 0, decoded_granule = 0;
    while (offset < size) {
        uint8_t *header = scratch->header;
        uint8_t *lacing = scratch->header + OGG_HEADER;
        PARSE_CHECK(size - offset >= OGG_HEADER && read_exact(file, header, OGG_HEADER) &&
                    !memcmp(header, "OggS", 4) && !header[4] &&
                    !(header[5] & ~7u) && !(header[5] & 1u));
        uint8_t flags = header[5], segments = header[26];
        bool page_eos = (flags & 4) != 0;
        uint32_t page_serial = get32(header + 14), page_sequence = get32(header + 18);
        PARSE_CHECK(!((!sequence && !(flags & 2)) || (sequence && flags & 2) ||
                    (sequence && page_serial != serial) || page_sequence != sequence ||
                    eos) && read_exact(file, lacing, segments));
        if (!sequence) serial = page_serial;
        size_t body_size = 0;
        for (unsigned i = 0; i < segments; ++i) body_size += lacing[i];
        size_t page_size = OGG_HEADER + segments + body_size;
        PARSE_CHECK(page_size <= size - offset && body_size <= OGG_BODY_CAPACITY);
        uint8_t *body = scratch->body;
        PARSE_CHECK(read_exact(file, body, body_size) &&
                    get32(header + 22) ==
                    page_crc(header, OGG_HEADER + segments, body, body_size));
        uint64_t granule = get64(header + 6);
        size_t packet_start = 0, cursor = 0;
        for (unsigned i = 0; i < segments; ++i) {
            cursor += lacing[i];
            if (lacing[i] == 255) continue;
            const uint8_t *packet = body + packet_start;
            size_t packet_size = cursor - packet_start;
            if (packet_index == 0) {
                if (sequence != 0 || segments == 0 || i + 1 != segments ||
                    packet_size != 19 || memcmp(packet, "OpusHead", 8) ||
                    packet[8] != 1 || packet[9] != 1 ||
                    get32(packet + 12) != PCM_RATE || get16(packet + 16) != 0 ||
                    packet[18] != 0) {
                    scratch_destroy(scratch);
                    return false;
                }
                info->pre_skip = get16(packet + 10);
            } else if (packet_index == 1) {
                PARSE_CHECK(sequence == 1 && i + 1 == segments &&
                            tags_valid(packet, packet_size));
            } else {
                PARSE_CHECK(packet_size <= OPUS_PACKET_MAX &&
                            packet_is_20ms(packet, packet_size));
                decoded_granule += 960;
            }
            ++packet_index;
            packet_start = cursor;
        }
        PARSE_CHECK(packet_start == body_size);
        if (packet_index > 2) {
            uint64_t expected = decoded_granule;
            if (granule == UINT64_MAX || granule < prior_granule ||
                granule < info->pre_skip || granule - info->pre_skip >
                (uint64_t)UINT32_MAX * (OPUS_RATE / PCM_RATE) ||
                (!page_eos && granule != expected) ||
                (page_eos && (granule > decoded_granule || granule < prior_granule))) {
                scratch_destroy(scratch);
                return false;
            }
            prior_granule = granule;
            info->granule = granule;
            info->last_page_offset = offset;
        } else PARSE_CHECK(granule == 0);
        eos = page_eos;
        offset += (uint32_t)page_size;
        ++sequence;
    }
    PARSE_CHECK(packet_index >= 3 && (!require_eos || eos) &&
                info->granule > info->pre_skip);
    uint64_t output = info->granule - info->pre_skip;
    if (output % (OPUS_RATE / PCM_RATE) ||
        output / (OPUS_RATE / PCM_RATE) > UINT32_MAX) {
        scratch_destroy(scratch);
        return false;
    }
    info->serial = serial; info->next_sequence = sequence; info->bytes = size;
    info->samples = (uint32_t)(output / (OPUS_RATE / PCM_RATE)); info->eos = eos;
    scratch_destroy(scratch);
#undef PARSE_CHECK
    return true;
}
bool ogg_opus_repair(FILE *file, uint32_t safe_bytes, uint32_t last_page_offset)
{
    ogg_opus_info_t info;
    if (!file || !safe_bytes || !ogg_opus_parse(file, safe_bytes, false, &info) ||
        info.eos || info.last_page_offset != last_page_offset ||
        fseek(file, last_page_offset, SEEK_SET)) return false;
    ogg_scratch_t *scratch = scratch_create();
    if (!scratch) return false;
    uint8_t *header = scratch->header;
    uint8_t *lacing = scratch->header + OGG_HEADER;
    uint8_t *body = scratch->body;
    if (!read_exact(file, header, OGG_HEADER) ||
        !read_exact(file, lacing, header[26])) {
        scratch_destroy(scratch); return false;
    }
    size_t body_size = 0;
    for (unsigned i = 0; i < header[26]; ++i) body_size += lacing[i];
    if (body_size > OGG_BODY_CAPACITY || !read_exact(file, body, body_size)) {
        scratch_destroy(scratch); return false;
    }
    header[5] |= 4;
    if (info.granule <= info.pre_skip) {
        scratch_destroy(scratch); return false;
    }
    put32(header + 22, page_crc(header, OGG_HEADER + header[26], body, body_size));
    if (fseek(file, last_page_offset, SEEK_SET) ||
        fwrite(header, 1, OGG_HEADER, file) != OGG_HEADER ||
        fflush(file)) {
        scratch_destroy(scratch); return false;
    }
#ifdef _WIN32
    if (_chsize_s(_fileno(file), safe_bytes)) {
        scratch_destroy(scratch); return false;
    }
#else
    if (ftruncate(fileno(file), safe_bytes)) {
        scratch_destroy(scratch); return false;
    }
#endif
    scratch_destroy(scratch);
    return !fflush(file) && ogg_opus_parse(file, safe_bytes, true, &info);
}

static uint32_t checkpoint_crc(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    while (length--) {
        crc ^= *data++;
        for (unsigned i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
void recording_checkpoint_encode(uint8_t out[24], uint32_t bytes, uint32_t page_offset)
{
    memcpy(out, "RCP2", 4);
    put32(out + 4, bytes); put32(out + 8, page_offset);
    put32(out + 12, ~bytes); put32(out + 16, ~page_offset);
    put32(out + 20, checkpoint_crc(out, 20));
}
bool recording_checkpoint_decode(const uint8_t data[24], uint32_t *bytes,
                                 uint32_t *page_offset)
{
    if (!data || !bytes || !page_offset || memcmp(data, "RCP2", 4) ||
        get32(data + 4) != ~get32(data + 12) ||
        get32(data + 8) != ~get32(data + 16) ||
        get32(data + 20) != checkpoint_crc(data, 20) ||
        get32(data + 4) > OPUS_MAX_FILE_BYTES ||
        get32(data + 8) >= get32(data + 4)) return false;
    *bytes = get32(data + 4); *page_offset = get32(data + 8);
    return true;
}

uint32_t upload_range_bytes(uint32_t total, uint32_t offset)
{
    if (offset >= total) return 0;
    uint32_t remaining = total - offset;
    return remaining < UPLOAD_RANGE_SIZE ? remaining : UPLOAD_RANGE_SIZE;
}
