#include "rolling_audio.h"
#include "audio_io.h"
#include "recording_codec.h"
#include "recorder_core.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    rolling_packet_t *packets;
    uint32_t data_head, data_used, packet_head, packet_count;
    uint16_t pre_skip;
    bool origin;
} packet_ring_t;

static packet_ring_t ring;
static _Atomic bool enabled, active;
static _Atomic bool handoff_requested;
static _Atomic int error_code;
static _Atomic uint32_t queue_peak, generation;
static TaskHandle_t worker_task;
static portMUX_TYPE ring_lock = portMUX_INITIALIZER_UNLOCKED;
static recording_encoder_t *handoff_encoder;

static void evict(void)
{
    rolling_packet_t *packet = &ring.packets[ring.packet_head];
    ring.data_head = (packet->offset + packet->bytes) % ROLLING_DATA_BYTES;
    ring.data_used -= packet->bytes;
    ring.packet_head = (ring.packet_head + 1) % ROLLING_MAX_FRAMES;
    --ring.packet_count;
    ring.origin = false;
}

static bool append(const uint8_t *data, size_t bytes)
{
    if (!data || !bytes || bytes > UINT16_MAX || bytes > ROLLING_DATA_BYTES)
        return false;
    portENTER_CRITICAL(&ring_lock);
    while (ring.packet_count >= ROLLING_MAX_FRAMES ||
           bytes > ROLLING_DATA_BYTES - ring.data_used)
        evict();
    uint32_t offset = (ring.data_head + ring.data_used) % ROLLING_DATA_BYTES;
    size_t first = bytes;
    if (first > ROLLING_DATA_BYTES - offset) first = ROLLING_DATA_BYTES - offset;
    memcpy(ring.data + offset, data, first);
    memcpy(ring.data, data + first, bytes - first);
    uint32_t tail = (ring.packet_head + ring.packet_count) % ROLLING_MAX_FRAMES;
    ring.packets[tail] = (rolling_packet_t){.offset = offset, .bytes = (uint16_t)bytes};
    ring.data_used += bytes;
    ++ring.packet_count;
    portEXIT_CRITICAL(&ring_lock);
    return true;
}

static void clear_ring(void)
{
    portENTER_CRITICAL(&ring_lock);
    ring.data_head = ring.data_used = ring.packet_head = ring.packet_count = 0;
    ring.pre_skip = 0;
    ring.origin = true;
    portEXIT_CRITICAL(&ring_lock);
    if (ring.data) memset(ring.data, 0, ROLLING_DATA_BYTES);
    if (ring.packets) memset(ring.packets, 0,
        ROLLING_MAX_FRAMES * sizeof(*ring.packets));
}

static void worker(void *unused)
{
    (void)unused;
    uint16_t pre_skip = 0;
    recording_encoder_t *encoder = recording_encoder_create(&pre_skip);
    esp_err_t err = encoder ? audio_start(true, false) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        ring.pre_skip = pre_skip;
        uint8_t packet[OPUS_PACKET_MAX];
        int16_t pcm[PCM_SAMPLES];
        while (atomic_load(&enabled)) {
            err = audio_read(pcm, PCM_SAMPLES);
            if (err != ESP_OK || audio_overruns()) {
                if (err == ESP_OK) err = ESP_ERR_INVALID_STATE;
                break;
            }
            size_t bytes = 0;
            err = recording_encoder_encode(encoder, pcm, PCM_SAMPLES,
                                           packet, sizeof(packet), &bytes);
            if (err != ESP_OK || !append(packet, bytes)) {
                if (err == ESP_OK) err = ESP_ERR_INVALID_SIZE;
                break;
            }
        }
        esp_err_t stopped = audio_stop();
        if (err == ESP_OK) err = stopped;
    }
    if (atomic_load(&handoff_requested) && err == ESP_OK) {
        if (handoff_encoder) recording_encoder_destroy(handoff_encoder);
        handoff_encoder = encoder;
    } else
        recording_encoder_destroy(encoder);
    if (err != ESP_OK) clear_ring();
    atomic_store(&error_code, err);
    atomic_store(&enabled, false);
    atomic_store(&active, false);
    worker_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t rolling_audio_init(void)
{
    memset(&ring, 0, sizeof(ring));
    ring.data = heap_caps_malloc(ROLLING_DATA_BYTES,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ring.packets = heap_caps_calloc(ROLLING_MAX_FRAMES, sizeof(*ring.packets),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring.data || !ring.packets) {
        if (ring.data) heap_caps_free(ring.data);
        if (ring.packets) heap_caps_free(ring.packets);
        memset(&ring, 0, sizeof(ring));
        return ESP_ERR_NO_MEM;
    }
    clear_ring();
    atomic_store(&error_code, ESP_OK);
    return ESP_OK;
}

esp_err_t rolling_audio_set_enabled(bool value)
{
    if (!ring.data || !ring.packets) return ESP_ERR_INVALID_STATE;
    if (value) {
        if (atomic_load(&active)) return ESP_OK;
        clear_ring();
        atomic_store(&error_code, ESP_OK);
        atomic_store(&enabled, true);
        atomic_store(&active, true);
        atomic_fetch_add(&generation, 1);
        if (xTaskCreate(worker, "rolling_audio", 6144, NULL, 8, &worker_task) != pdPASS) {
            worker_task = NULL;
            atomic_store(&enabled, false);
            atomic_store(&active, false);
            atomic_store(&error_code, ESP_ERR_NO_MEM);
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }
    if (!atomic_load(&active) && !atomic_load(&enabled)) return ESP_OK;
    atomic_store(&enabled, false);
    for (unsigned i = 0; atomic_load(&active) && i < 50; ++i)
        vTaskDelay(pdMS_TO_TICKS(20));
    if (atomic_load(&active)) return ESP_ERR_TIMEOUT;
    clear_ring();
    return ESP_OK;
}

esp_err_t rolling_audio_take(rolling_snapshot_t *snapshot)
{
    if (!snapshot) return ESP_ERR_INVALID_ARG;
    memset(snapshot, 0, sizeof(*snapshot));
    if (!ring.data || !ring.packets) return ESP_OK;
    atomic_store(&handoff_requested, true);
    atomic_store(&enabled, false);
    for (unsigned i = 0; atomic_load(&active) && i < 50; ++i)
        vTaskDelay(pdMS_TO_TICKS(20));
    if (atomic_load(&active)) {
        atomic_store(&handoff_requested, false);
        while (atomic_load(&active)) vTaskDelay(pdMS_TO_TICKS(20));
        if (handoff_encoder) {
            recording_encoder_destroy(handoff_encoder);
            handoff_encoder = NULL;
        }
        clear_ring();
        return ESP_ERR_TIMEOUT;
    }
    snapshot->encoder = handoff_encoder;
    handoff_encoder = NULL;
    snapshot->encoder_pre_skip = ring.pre_skip;
    atomic_store(&handoff_requested, false);
    if (!ring.packet_count) {
        clear_ring();
        return ESP_OK;
    }
    snapshot->data = heap_caps_malloc(ring.data_used,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snapshot->packets = heap_caps_malloc(
        ring.packet_count * sizeof(*snapshot->packets),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot->data || !snapshot->packets) {
        rolling_snapshot_release(snapshot);
        clear_ring();
        return ESP_ERR_NO_MEM;
    }
    uint32_t used = 0;
    for (uint32_t i = 0; i < ring.packet_count; ++i) {
        rolling_packet_t source =
            ring.packets[(ring.packet_head + i) % ROLLING_MAX_FRAMES];
        size_t first = source.bytes;
        if (first > ROLLING_DATA_BYTES - source.offset)
            first = ROLLING_DATA_BYTES - source.offset;
        memcpy(snapshot->data + used, ring.data + source.offset, first);
        memcpy(snapshot->data + used + first, ring.data, source.bytes - first);
        snapshot->packets[i] =
            (rolling_packet_t){.offset = used, .bytes = source.bytes};
        used += source.bytes;
    }
    snapshot->frames = ring.packet_count;
    snapshot->samples = ring.packet_count * PCM_SAMPLES;
    snapshot->data_bytes = used;
    snapshot->pre_skip = ring.origin ? ring.pre_skip : 0;
    clear_ring();
    return ESP_OK;
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}
static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
static void put64(uint8_t *p, uint64_t value)
{
    put32(p, (uint32_t)value); put32(p + 4, (uint32_t)(value >> 32));
}
static uint32_t ogg_crc(uint32_t crc, const uint8_t *data, size_t bytes)
{
    while (bytes--) {
        crc ^= (uint32_t)*data++ << 24;
        for (unsigned i = 0; i < 8; ++i)
            crc = (crc << 1) ^ (0x04c11db7u & (0u - (crc >> 31)));
    }
    return crc;
}
static bool page(uint8_t *output, size_t capacity, size_t *used,
                 uint32_t serial, uint32_t sequence, uint8_t flags,
                 uint64_t granule, const uint8_t *segments, size_t segment_count,
                 const uint8_t *body, size_t body_bytes)
{
    size_t total = 27 + segment_count + body_bytes;
    if (!output || !used || segment_count > 255 || total > capacity - *used)
        return false;
    uint8_t *header = output + *used;
    memset(header, 0, 27);
    memcpy(header, "OggS", 4); header[5] = flags;
    put64(header + 6, granule); put32(header + 14, serial);
    put32(header + 18, sequence); header[26] = (uint8_t)segment_count;
    memcpy(header + 27, segments, segment_count);
    memcpy(header + 27 + segment_count, body, body_bytes);
    put32(header + 22, ogg_crc(ogg_crc(0, header, 27 + segment_count),
                               body, body_bytes));
    *used += total;
    return true;
}
static bool single_page(uint8_t *output, size_t capacity, size_t *used,
                        uint32_t serial, uint32_t sequence, uint8_t flags,
                        const uint8_t *body, size_t body_bytes)
{
    uint8_t segments[6];
    size_t count = 0, remaining = body_bytes;
    do {
        if (count == sizeof(segments)) return false;
        segments[count++] = remaining >= 255 ? 255 : (uint8_t)remaining;
        if (remaining >= 255) remaining -= 255; else remaining = 0;
    } while (remaining || (body_bytes && body_bytes % 255 == 0));
    return page(output, capacity, used, serial, sequence, flags, 0,
                segments, count, body, body_bytes);
}
esp_err_t rolling_snapshot_ogg(const rolling_snapshot_t *snapshot,
                               uint8_t **data, size_t *bytes)
{
    if (!snapshot || !data || !bytes || snapshot->frames > ROLLING_MAX_FRAMES ||
        snapshot->samples != snapshot->frames * PCM_SAMPLES ||
        snapshot->data_bytes > ROLLING_DATA_BYTES) return ESP_ERR_INVALID_ARG;
    *data = NULL; *bytes = 0;
    if (!snapshot->frames) return ESP_OK;
    size_t capacity = snapshot->data_bytes + 16384;
    if (capacity > 256u * 1024u) return ESP_ERR_INVALID_SIZE;
    uint8_t *output = heap_caps_malloc(capacity,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!output) return ESP_ERR_NO_MEM;
    uint32_t serial = esp_random() | 1u, sequence = 0;
    uint8_t head[19] = {'O','p','u','s','H','e','a','d',1,1};
    put16(head + 10, snapshot->pre_skip); put32(head + 12, PCM_RATE);
    static const uint8_t tags[] = {
        'O','p','u','s','T','a','g','s', 8,0,0,0,
        'r','e','c','o','r','d','e','r', 0,0,0,0
    };
    size_t used = 0;
    bool ok = single_page(output, capacity, &used, serial, sequence++, 2,
                          head, sizeof(head)) &&
        single_page(output, capacity, &used, serial, sequence++, 0,
                    tags, sizeof(tags));
    for (uint32_t first = 0; ok && first < snapshot->frames;) {
        uint32_t count = 0;
        uint8_t segments[255];
        size_t segment_count = 0, body_bytes = 0;
        size_t body_offset = snapshot->packets[first].offset;
        while (first + count < snapshot->frames &&
               count < OPUS_PAGE_PACKETS && ok) {
            rolling_packet_t packet = snapshot->packets[first + count];
            if (!packet.bytes || packet.offset > snapshot->data_bytes ||
                packet.bytes > snapshot->data_bytes - packet.offset ||
                packet.bytes > OPUS_PACKET_MAX) { ok = false; break; }
            size_t packet_laces = packet.bytes / 255u + 1u;
            if (count && (segment_count + packet_laces > sizeof(segments) ||
                          body_bytes + packet.bytes > OGG_BODY_CAPACITY))
                break;
            size_t remaining = packet.bytes;
            for (size_t lace = 0; lace < packet_laces; ++lace) {
                if (segment_count == sizeof(segments)) { ok = false; break; }
                segments[segment_count++] =
                    remaining >= 255 ? 255 : (uint8_t)remaining;
                if (remaining >= 255) remaining -= 255; else remaining = 0;
            }
            if (packet.offset != body_offset + body_bytes ||
                packet.bytes > OGG_BODY_CAPACITY - body_bytes) { ok = false; break; }
            body_bytes += packet.bytes;
            ++count;
        }
        if (!count) ok = false;
        uint64_t granule = (uint64_t)(first + count) * 960u;
        ok = ok && page(output, capacity, &used, serial, sequence++,
                        first + count == snapshot->frames ? 4 : 0,
                        granule, segments, segment_count,
                        snapshot->data + body_offset, body_bytes);
        first += count;
    }
    if (!ok || used > 256u * 1024u) {
        memset(output, 0, capacity); heap_caps_free(output);
        return ESP_ERR_INVALID_SIZE;
    }
    *data = output; *bytes = used;
    return ESP_OK;
}

void rolling_snapshot_release(rolling_snapshot_t *snapshot)
{
    if (!snapshot) return;
    if (snapshot->data) {
        memset(snapshot->data, 0, snapshot->data_bytes);
        heap_caps_free(snapshot->data);
    }
    if (snapshot->packets) {
        memset(snapshot->packets, 0,
               snapshot->frames * sizeof(*snapshot->packets));
        heap_caps_free(snapshot->packets);
    }
    if (snapshot->encoder)
        recording_encoder_destroy((recording_encoder_t *)snapshot->encoder);
    memset(snapshot, 0, sizeof(*snapshot));
}

rolling_status_t rolling_audio_status(void)
{
    portENTER_CRITICAL(&ring_lock);
    rolling_status_t status = {
        .active = atomic_load(&active),
        .ready = ring.packet_count == ROLLING_MAX_FRAMES,
        .error = atomic_load(&error_code),
        .samples = ring.packet_count * PCM_SAMPLES,
        .frames = ring.packet_count,
        .bytes = ring.data_used,
        .queue_peak = atomic_load(&queue_peak),
        .overruns = audio_overruns(),
        .generation = atomic_load(&generation),
    };
    portEXIT_CRITICAL(&ring_lock);
    return status;
}
