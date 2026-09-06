#include "audio_io.h"
#include "recorder_core.h"
#include "driver/i2s_std.h"
#include <assert.h>
#include <string.h>

struct host_i2s { bool enabled; i2s_event_callbacks_t callbacks; };
static struct host_i2s transmit, receive;
static int16_t dma[4][PCM_SAMPLES * 2];
static bool speaker;
static unsigned cursor;

esp_err_t board_codec_init(void) { return ESP_OK; }
esp_err_t board_speaker(bool enabled) { speaker = enabled; return ESP_OK; }
esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    assert(config->dma_desc_num == 4 && config->dma_frame_num == PCM_SAMPLES);
    assert(!config->auto_clear);
    *tx = &transmit; *rx = &receive;
    return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t channel, const i2s_std_config_t *config)
{
    (void)channel;
    assert(config->clk_cfg.sample_rate_hz == 16000 && config->clk_cfg.mclk_multiple == 256);
    assert(config->gpio_cfg.mclk == 3 && config->gpio_cfg.bclk == 46 && config->gpio_cfg.ws == 9);
    assert(config->gpio_cfg.dout == 10 && config->gpio_cfg.din == 14);
    return ESP_OK;
}
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t channel,
    const i2s_event_callbacks_t *callbacks, void *context)
{
    assert(!context); channel->callbacks = *callbacks; return ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t channel)
{
    assert(!channel->enabled); channel->enabled = true; return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t channel)
{
    assert(channel->enabled); channel->enabled = false; return ESP_OK;
}
esp_err_t i2s_channel_read(i2s_chan_handle_t channel, void *data, size_t size,
    size_t *received, uint32_t timeout)
{
    (void)timeout;
    assert(channel == &receive && channel->enabled && size <= PCM_SAMPLES * 4);
    int16_t *pcm = data;
    for (unsigned i = 0; i < size / 4; ++i) { pcm[2 * i] = i; pcm[2 * i + 1] = -(int)i; }
    *received = size; return ESP_OK;
}
esp_err_t i2s_channel_write(i2s_chan_handle_t channel, const void *data, size_t size,
    size_t *written, uint32_t timeout)
{
    (void)timeout;
    assert(channel == &transmit && channel->enabled && size <= PCM_SAMPLES * 4);
    const int16_t *pcm = data;
    for (unsigned i = 0; i < size / 4; ++i) assert(pcm[2 * i] == pcm[2 * i + 1]);
    *written = size; return ESP_OK;
}
static void dma_complete(void)
{
    assert(transmit.enabled);
    i2s_event_data_t event = {.dma_buf = dma[cursor++ % 4], .size = sizeof(dma[0])};
    assert(transmit.callbacks.on_sent);
    transmit.callbacks.on_sent(&transmit, &event, NULL);
}
static unsigned simulate_credit(unsigned credit, uint32_t stream)
{
    assert(audio_voice_start() == ESP_OK);
    assert(audio_voice_epoch(stream) == ESP_OK);
    cursor = 0;
    int16_t pcm[PCM_SAMPLES];
    for (unsigned i = 0; i < PCM_SAMPLES; ++i) pcm[i] = 123;
    uint64_t sent = 0, reported = 0;
    unsigned empty_periods = 0;
    /* One frame per 20ms, up to 60ms initial lead; device reports every 80ms.
       Exercise the real four-slot on_sent callback, not an instant speaker. */
    for (unsigned tick = 0; tick < 100; ++tick) {
        if (tick % 4 == 0) reported = audio_voice_played(stream);
        uint64_t pacing_limit = (uint64_t)(tick + 3) * PCM_SAMPLES;
        unsigned burst_frames = 0;
        while (sent < pacing_limit && sent - reported + PCM_SAMPLES <= credit && burst_frames < 3) {
            assert(audio_voice_enqueue(stream, pcm, PCM_SAMPLES) == ESP_OK);
            sent += PCM_SAMPLES;
            ++burst_frames;
        }
        uint64_t before = audio_voice_played(stream);
        dma_complete();
        if (tick >= 4 && audio_voice_played(stream) == before) ++empty_periods;
    }
    assert(audio_stop() == ESP_OK);
    return empty_periods;
}
int main(void)
{
    assert(audio_init() == ESP_OK);
    assert(audio_voice_start() == ESP_OK && receive.enabled && transmit.enabled && !speaker);
    assert(audio_voice_epoch(1) == ESP_OK && speaker);
    int16_t pcm[PCM_SAMPLES];
    for (unsigned i = 0; i < PCM_SAMPLES; ++i) pcm[i] = 123;
    for (unsigned i = 0; i < 10; ++i) assert(audio_voice_enqueue(1, pcm, PCM_SAMPLES) == ESP_OK);
    assert(audio_voice_enqueue(1, pcm, 1) == ESP_ERR_NO_MEM);
    assert(audio_voice_played(1) == 0);
    for (unsigned i = 0; i < 4; ++i) dma_complete();
    assert(audio_voice_played(1) == 0); /* Moving into DMA must not return credit. */
    /* Moving frames into DMA does not return combined pending credit. */
    assert(audio_voice_enqueue(1, pcm, 1) == ESP_ERR_NO_MEM);
    dma_complete();
    assert(audio_voice_played(1) == PCM_SAMPLES);
    assert(audio_voice_enqueue(1, pcm, PCM_SAMPLES) == ESP_OK);
    assert(audio_voice_enqueue(1, pcm, 1) == ESP_ERR_NO_MEM);
    uint64_t played = 0;
    assert(audio_voice_clear(1, &played) == ESP_OK);
    assert(played == PCM_SAMPLES && receive.enabled && transmit.enabled && !speaker);
    for (unsigned block = 0; block < 4; ++block)
        for (unsigned i = 0; i < PCM_SAMPLES * 2; ++i) assert(dma[block][i] == 0);
    assert(audio_voice_enqueue(1, pcm, 1) == ESP_ERR_INVALID_STATE);
    assert(audio_voice_epoch(2) == ESP_OK);
    assert(audio_voice_enqueue(2, pcm, 127) == ESP_OK);
    for (unsigned i = 0; i < 8; ++i) dma_complete();
    assert(audio_voice_played(2) == 127);
    assert(audio_read(pcm, PCM_SAMPLES) == ESP_OK);
    for (unsigned i = 0; i < PCM_SAMPLES; ++i) assert(pcm[i] == (int16_t)i);
    assert(audio_stop() == ESP_OK && !receive.enabled && !transmit.enabled && !speaker);
    assert(audio_start(false, true) == ESP_OK && speaker);
    assert(audio_write(pcm, PCM_SAMPLES) == ESP_OK);
    assert(audio_stop() == ESP_OK);
    unsigned gaps_100ms = simulate_credit(1600, 3);
    unsigned gaps_200ms = simulate_credit(3200, 4);
    assert(gaps_100ms > 0);
    assert(gaps_200ms == 0);
    printf("DMA timing model: 100ms credit=%u empty steady-state periods; 200ms credit=%u\n",
           gaps_100ms, gaps_200ms);
    puts("PASS: actual I2S ten-frame prefill/combined 3200-sample cap, clear and short tails");
    return 0;
}
