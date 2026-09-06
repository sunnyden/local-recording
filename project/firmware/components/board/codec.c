#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t board_codec_init(void)
{
    esp_err_t err = board_codec_write(0, 0x80);
    if (err != ESP_OK) return err;
    if ((err = board_codec_write(0, 0)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(50));
    /* ES8388 slave, Philips 16-bit stereo slots, shared LRCLK, MCLK=256*16k.
       Left microphone ADC is duplicated to both slots; no ALC/VAD/AEC. */
    const uint8_t settings[][2] = {
        {0x01,0x50}, {0x02,0xf3}, {0x00,0x06}, {0x08,0x00},
        {0x03,0x00}, {0x04,0x0c}, {0x09,0x44}, {0x0a,0x00},
        {0x0b,0x02}, {0x0c,0x4c}, {0x0d,0x02}, {0x10,0}, {0x11,0},
        {0x12,0}, {0x17,0x18}, {0x18,0x02}, {0x19,0x04},
        {0x1a,0}, {0x1b,0}, {0x26,0}, {0x27,0xb8}, {0x2a,0xb8},
        {0x2b,0x80}, {0x2e,0}, {0x2f,0}, {0x30,26}, {0x31,26},
        {0x02,0}
    };
    for (unsigned i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        err = board_codec_write(settings[i][0], settings[i][1]);
        if (err != ESP_OK) return err;
    }
    return board_speaker(false);
}
esp_err_t board_speaker(bool enabled)
{
    esp_err_t err = board_codec_write(0x19, enabled ? 0 : 4);
    if (err != ESP_OK) return err;
    return board_expander_update(0x0004, enabled ? 0 : 0x0004);
}
