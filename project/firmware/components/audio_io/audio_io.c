#include "audio_io.h"
#include "board.h"
#include "recorder_core.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static i2s_chan_handle_t tx, rx;
static bool rx_on, tx_on;
static _Atomic uint32_t overruns;
static DMA_ATTR int16_t receive_slots[PCM_SAMPLES * 2];
static DMA_ATTR int16_t transmit_slots[PCM_SAMPLES * 2];
#define VOICE_RING_SAMPLES (PCM_SAMPLES * 6)
#define DMA_BLOCKS 4
_Static_assert(VOICE_RING_SAMPLES + DMA_BLOCKS * PCM_SAMPLES == AUDIO_VOICE_MAX_PENDING_SAMPLES,
               "Voice budget must match six software and four DMA periods");
static portMUX_TYPE voice_lock = portMUX_INITIALIZER_UNLOCKED;
static bool voice_mode;
static uint32_t voice_epoch;
static uint64_t voice_played;
static int16_t voice_ring[VOICE_RING_SAMPLES];
static size_t voice_head, voice_count, voice_pending;
static struct { void *buffer; size_t samples; uint32_t epoch; } sent_blocks[DMA_BLOCKS];
static bool sent(i2s_chan_handle_t chan, i2s_event_data_t *event, void *ctx)
{
    (void)chan; (void)ctx;
    portENTER_CRITICAL_ISR(&voice_lock);
    unsigned slot = 0;
    while (slot < DMA_BLOCKS && sent_blocks[slot].buffer &&
           sent_blocks[slot].buffer != event->dma_buf) ++slot;
    if (slot < DMA_BLOCKS) {
        if (voice_mode && sent_blocks[slot].epoch == voice_epoch) {
            voice_played += sent_blocks[slot].samples;
            voice_pending -= sent_blocks[slot].samples;
        }
        sent_blocks[slot].buffer = event->dma_buf;
        sent_blocks[slot].samples = 0;
        sent_blocks[slot].epoch = voice_epoch;
    }
    if (event->dma_buf) {
        memset(event->dma_buf, 0, event->size);
        if (voice_mode && voice_epoch && slot < DMA_BLOCKS) {
            int16_t *stereo = event->dma_buf;
            size_t n = voice_count < PCM_SAMPLES ? voice_count : PCM_SAMPLES;
            for (size_t i = 0; i < n; ++i) {
                stereo[2 * i] = stereo[2 * i + 1] = voice_ring[voice_head];
                voice_head = (voice_head + 1) % VOICE_RING_SAMPLES;
            }
            voice_count -= n;
            sent_blocks[slot].samples = n;
        }
    }
    portEXIT_CRITICAL_ISR(&voice_lock);
    return false;
}
static bool overflow(i2s_chan_handle_t chan, i2s_event_data_t *event, void *ctx)
{
    (void)chan; (void)event; (void)ctx;
    atomic_fetch_add(&overruns, 1);
    return false;
}
esp_err_t audio_init(void)
{
    esp_err_t err = board_codec_init();
    if (err != ESP_OK) return err;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = DMA_BLOCKS;
    channel.dma_frame_num = PCM_SAMPLES;
    channel.auto_clear = false; /* Callback clears buffers before refilling. */
    if ((err = i2s_new_channel(&channel, &tx, &rx)) != ESP_OK) return err;
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PCM_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = 3, .bclk = 46, .ws = 9, .dout = 10, .din = 14},
    };
    cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if ((err = i2s_channel_init_std_mode(tx, &cfg)) != ESP_OK) return err;
    if ((err = i2s_channel_init_std_mode(rx, &cfg)) != ESP_OK) return err;
    i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = overflow};
    if ((err = i2s_channel_register_event_callback(rx, &callbacks, NULL)) != ESP_OK) return err;
    callbacks = (i2s_event_callbacks_t){.on_sent = sent};
    return i2s_channel_register_event_callback(tx, &callbacks, NULL);
}
uint32_t audio_overruns(void) { return atomic_load(&overruns); }
esp_err_t audio_start(bool capture, bool playback)
{
    if (!tx || rx_on || tx_on || (!capture && !playback)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = board_speaker(false);
    if (err != ESP_OK) return err;
    atomic_store(&overruns, 0);
    /* Enable both channels so clock ownership is identical in every mode. */
    err = i2s_channel_enable(tx);
    if (err != ESP_OK) return err;
    tx_on = true;
    if (capture) {
        err = i2s_channel_enable(rx);
        if (err != ESP_OK) { audio_stop(); return err; }
        rx_on = true;
    }
    if (playback && (err = board_speaker(true)) != ESP_OK) audio_stop();
    return err;
}
esp_err_t audio_stop(void)
{
    esp_err_t err = board_speaker(false);
    if (rx_on) {
        esp_err_t result = i2s_channel_disable(rx);
        if (err == ESP_OK) err = result;
        rx_on = false;
    }
    if (tx_on) {
        esp_err_t result = i2s_channel_disable(tx);
        if (err == ESP_OK) err = result;
        tx_on = false;
    }
    portENTER_CRITICAL(&voice_lock);
    voice_mode = false;
    voice_epoch = 0;
    voice_count = 0;
    voice_pending = 0;
    for (unsigned i = 0; i < DMA_BLOCKS; ++i) {
        if (sent_blocks[i].buffer) memset(sent_blocks[i].buffer, 0, PCM_SAMPLES * 4);
        sent_blocks[i].samples = 0;
    }
    portEXIT_CRITICAL(&voice_lock);
    return err;
}
esp_err_t audio_read(int16_t *mono, size_t samples)
{
    if (!mono || !samples || samples > PCM_SAMPLES || !rx_on) return ESP_ERR_INVALID_ARG;
    size_t bytes = 0;
    esp_err_t err = i2s_channel_read(rx, receive_slots, samples * 4, &bytes, 200);
    if (err != ESP_OK || bytes != samples * 4) return err == ESP_OK ? ESP_FAIL : err;
    for (size_t i = 0; i < samples; ++i) mono[i] = receive_slots[i * 2];
    return ESP_OK;
}
esp_err_t audio_write(const int16_t *mono, size_t samples)
{
    if (!mono || !samples || samples > PCM_SAMPLES || !tx_on) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < samples; ++i)
        transmit_slots[i * 2] = transmit_slots[i * 2 + 1] = mono[i];
    size_t bytes = 0;
    esp_err_t err = i2s_channel_write(tx, transmit_slots, samples * 4, &bytes, 200);
    return err == ESP_OK && bytes != samples * 4 ? ESP_FAIL : err;
}
esp_err_t audio_voice_start(void)
{
    portENTER_CRITICAL(&voice_lock);
    voice_mode = true; voice_epoch = 0; voice_count = 0; voice_pending = 0;
    voice_head = 0; voice_played = 0;
    portEXIT_CRITICAL(&voice_lock);
    esp_err_t err = audio_start(true, false);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&voice_lock); voice_mode = false; portEXIT_CRITICAL(&voice_lock);
    }
    return err;
}
esp_err_t audio_voice_clear(uint32_t epoch, uint64_t *played)
{
    esp_err_t err = board_speaker(false);
    if (err != ESP_OK) return err;
    if (tx_on && (err = i2s_channel_disable(tx)) != ESP_OK) return err;
    tx_on = false;
    portENTER_CRITICAL(&voice_lock);
    if (played) *played = epoch == voice_epoch ? voice_played : 0;
    voice_epoch = 0; voice_count = 0; voice_pending = 0; voice_head = 0;
    for (unsigned i = 0; i < DMA_BLOCKS; ++i) {
        if (sent_blocks[i].buffer) memset(sent_blocks[i].buffer, 0, PCM_SAMPLES * 4);
        sent_blocks[i].samples = 0;
    }
    portEXIT_CRITICAL(&voice_lock);
    err = i2s_channel_enable(tx);
    if (err == ESP_OK) tx_on = true;
    return err;
}
esp_err_t audio_voice_epoch(uint32_t epoch)
{
    if (!epoch || !voice_mode) return ESP_ERR_INVALID_STATE;
    esp_err_t err = audio_voice_clear(voice_epoch, NULL);
    if (err != ESP_OK) return err;
    portENTER_CRITICAL(&voice_lock);
    voice_epoch = epoch; voice_played = 0;
    portEXIT_CRITICAL(&voice_lock);
    return board_speaker(true);
}
esp_err_t audio_voice_enqueue(uint32_t epoch, const int16_t *pcm, size_t samples)
{
    if (!pcm || !samples || samples > PCM_SAMPLES) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&voice_lock);
    esp_err_t err = ESP_OK;
    if (epoch != voice_epoch || !voice_mode) err = ESP_ERR_INVALID_STATE;
    else if (samples > AUDIO_VOICE_MAX_PENDING_SAMPLES - voice_pending ||
             samples > VOICE_RING_SAMPLES - voice_count) err = ESP_ERR_NO_MEM;
    else {
        for (size_t i = 0; i < samples; ++i)
            voice_ring[(voice_head + voice_count + i) % VOICE_RING_SAMPLES] = pcm[i];
        voice_count += samples;
        voice_pending += samples;
    }
    portEXIT_CRITICAL(&voice_lock);
    return err;
}
uint64_t audio_voice_played(uint32_t epoch)
{
    portENTER_CRITICAL(&voice_lock);
    uint64_t count = voice_epoch == epoch ? voice_played : 0;
    portEXIT_CRITICAL(&voice_lock);
    return count;
}
