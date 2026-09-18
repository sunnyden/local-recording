#define main voice_regression_main
#include "voice_client_test.c"
#undef main
#include "../components/voice_client/voice_client.c"

static void deliver_control(voice_session_t *session, const char *json)
{
    session->used = strlen(json);
    memcpy(session->message, json, session->used);
    control(session);
}
int main(void)
{
    assert(voice_regression_main() == 0);
    uint32_t previous = voice_client_status().generation;
    clock_us = 0;
    epoch = 0;
    assert(voice_client_start() == ESP_OK);
    assert(voice_client_status().generation == previous + 1);
    assert(!voice_client_status().microphone_level && !voice_client_status().speaker_level);
    assert(voice_client_start() == ESP_ERR_INVALID_STATE);
    assert(voice_client_status().generation == previous + 1);
    /* Run the actual task-context receive/capture/observation paths with an idle DAC. */
    voice_session_t session = {0};
    session.controls = xQueueCreate(8, 160);
    session.microphone = xQueueCreate(50, PCM_BYTES);
    audio_running = true;
    atomic_store(&ready, true);
    deliver_control(&session, "{\"v\":2,\"type\":\"playback.start\",\"epoch\":1}");
    int16_t pcm[PCM_SAMPLES] = {123, INT16_MIN};
    int16_t before[PCM_SAMPLES];
    memcpy(before, pcm, sizeof(pcm));
    voice_frame_t frame = {.kind = 2, .epoch = 1, .samples = PCM_SAMPLES,
        .pcm = (const uint8_t *)pcm};
    assert(voice_encode(session.message, sizeof(session.message), &frame));
    session.used = VOICE_MAX_PACKET;
    binary(&session);
    assert(!memcmp(before, pcm, sizeof(pcm)));
    clock_us = 100000;
    observe_playback();
    assert(!voice_client_status().playback_active && !voice_client_status().speaker_level);
    /* Prefill is queued, not played; only measured sample advancement enables output. */
    played = 1;
    clock_us = 200000;
    observe_playback();
    assert(voice_client_status().playback_active && voice_client_status().speaker_level == 255);
    clock_us = 300000;
    observe_playback();
    assert(!voice_client_status().playback_active && !voice_client_status().speaker_level);
    played = 2;
    clock_us = 400000;
    observe_playback();
    assert(voice_client_status().playback_active);
    clock_us = 650000;
    assert(!voice_client_status().playback_active);
    played = 320; queued = 0;
    deliver_control(&session, "{\"v\":2,\"type\":\"playback.clear\",\"epoch\":1}");
    assert(!voice_client_status().playback_active && !voice_client_status().speaker_level);
    deliver_control(&session, "{\"v\":2,\"type\":\"playback.start\",\"epoch\":2}");
    assert(!voice_client_status().playback_active && !voice_client_status().speaker_level);
    session.used = VOICE_MAX_PACKET;
    assert(voice_encode(session.message, sizeof(session.message), &frame));
    binary(&session);
    assert(!voice_client_status().speaker_level);
    assert(capture_once(&session));
    assert(voice_client_status().microphone_level == audio_meter_peak((int16_t[]){319}, 1));
    int16_t captured[PCM_SAMPLES];
    assert(xQueueReceive(session.microphone, captured, 0) == pdTRUE);
    for (size_t i = 0; i < PCM_SAMPLES; ++i) assert(captured[i] == (int16_t)i);
    clock_us += 250000;
    assert(!voice_client_status().microphone_level);
    assert(capture_once(&session) && voice_client_status().microphone_level);
    deliver_control(&session, "{\"v\":2,\"type\":\"state\",\"state\":\"stopping\"}");
    assert(!voice_client_status().microphone_level && !voice_client_status().playback_active);
    assert(!strcmp(voice_client_status().state, "STOPPING"));
    voice_client_stop();
    assert(!voice_client_status().speaker_level && !voice_client_status().microphone_level);
    audio_running = false;
    vQueueDelete(session.controls);
    vQueueDelete(session.microphone);
    atomic_store(&active, false);
    task = NULL;
    run(false, false, false, false);
    assert(voice_client_status().generation == previous + 2);
    assert(!voice_client_status().active && !voice_client_status().microphone_level &&
           !voice_client_status().speaker_level && !voice_client_status().playback_active);
    puts("PASS: actual voice PCM unchanged; idle prefill gated by played samples, epoch clear, stale levels, generations and STOPPING suppression");
    return 0;
}
