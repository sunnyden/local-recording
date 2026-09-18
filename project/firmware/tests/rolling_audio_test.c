#include "rolling_audio.h"
#include "audio_io.h"
#include "recording_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
typedef void *TaskHandle_t;
#include <assert.h>
#include <stdlib.h>
#include <string.h>

uint32_t esp_random(void) { return 0x12345678; }
esp_err_t audio_start(bool capture, bool playback)
{ (void)capture; (void)playback; return ESP_OK; }
esp_err_t audio_stop(void) { return ESP_OK; }
esp_err_t audio_read(int16_t *pcm, size_t samples)
{ memset(pcm, 0, samples * sizeof(*pcm)); return ESP_OK; }
uint32_t audio_overruns(void) { return 0; }
recording_encoder_t *recording_encoder_create(uint16_t *pre_skip)
{ *pre_skip = 312; return (recording_encoder_t *)1; }
esp_err_t recording_encoder_encode(recording_encoder_t *encoder, const int16_t *pcm,
    size_t samples, uint8_t *packet, size_t capacity, size_t *bytes)
{
    (void)encoder; (void)pcm; (void)samples;
    assert(capacity >= 60); memset(packet, 0, 60); packet[0] = 0x48; *bytes = 60;
    return ESP_OK;
}
void recording_encoder_destroy(recording_encoder_t *encoder) { (void)encoder; }
BaseType_t xTaskCreate(TaskFunction_t task, const char *name, unsigned stack,
    void *arg, unsigned priority, void *handle)
{
    (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks) { (void)ticks; }
void vTaskDelete(void *task) { (void)task; }

#include "../components/rolling_audio/rolling_audio.c"

int main(void)
{
    assert(rolling_audio_init() == ESP_OK);
    ring.pre_skip = 312;
    uint8_t packet[200];
    for (unsigned frame = 0; frame < 25; ++frame) {
        memset(packet, (uint8_t)frame, 60); packet[0] = 0x48;
        assert(append(packet, 60));
    }
    rolling_snapshot_t warm;
    assert(rolling_audio_take(&warm) == ESP_OK);
    assert(warm.frames == 25 && warm.pre_skip == 312);
    uint8_t *warm_ogg = NULL; size_t warm_bytes = 0;
    assert(rolling_snapshot_ogg(&warm, &warm_ogg, &warm_bytes) == ESP_OK);
    FILE *warm_file = fopen("rolling-warm.opus", "wb+");
    assert(warm_file && fwrite(warm_ogg, 1, warm_bytes, warm_file) == warm_bytes &&
           !fflush(warm_file));
    ogg_opus_info_t warm_info;
    assert(ogg_opus_parse(warm_file, 0, true, &warm_info));
    assert(warm_info.pre_skip == 312 &&
           warm_info.samples == 25 * PCM_SAMPLES - 104);
    fclose(warm_file); remove("rolling-warm.opus");
    memset(warm_ogg, 0, warm_bytes); free(warm_ogg);
    rolling_snapshot_release(&warm);
    for (unsigned frame = 0; frame < ROLLING_MAX_FRAMES + 37; ++frame) {
        size_t bytes = 40 + frame % 161;
        memset(packet, (uint8_t)frame, bytes);
        packet[0] = 0x48;
        assert(append(packet, bytes));
    }
    assert(ring.packet_count == ROLLING_MAX_FRAMES);
    assert(!ring.origin);
    rolling_snapshot_t snapshot;
    assert(rolling_audio_take(&snapshot) == ESP_OK);
    assert(snapshot.frames == ROLLING_MAX_FRAMES);
    assert(snapshot.samples == 30u * PCM_RATE);
    assert(snapshot.pre_skip == 0);
    for (uint32_t i = 1; i < snapshot.frames; ++i)
        assert(snapshot.packets[i].offset ==
               snapshot.packets[i - 1].offset + snapshot.packets[i - 1].bytes);
    uint8_t *ogg = NULL;
    size_t ogg_bytes = 0;
    assert(rolling_snapshot_ogg(&snapshot, &ogg, &ogg_bytes) == ESP_OK);
    assert(ogg && ogg_bytes <= 256u * 1024u);
    FILE *file = fopen("rolling-context.opus", "wb+");
    assert(file && fwrite(ogg, 1, ogg_bytes, file) == ogg_bytes && !fflush(file));
    ogg_opus_info_t info;
    assert(ogg_opus_parse(file, 0, true, &info));
    assert(info.samples == 30u * PCM_RATE && info.pre_skip == 0);
    fclose(file); remove("rolling-context.opus");
    memset(ogg, 0, ogg_bytes); free(ogg);
    rolling_snapshot_release(&snapshot);
    assert(!snapshot.data && !snapshot.packets);
    puts("PASS: rolling Opus ring eviction, chronology, 30-second bound and Ogg snapshot");
}
