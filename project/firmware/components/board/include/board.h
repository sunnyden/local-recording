#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum { KEY_NONE, KEY_UP, KEY_DOWN, KEY_ENTER, KEY_BACK } board_key_t;
esp_err_t board_init(void);
esp_err_t board_sd_mount(void);
esp_err_t board_key_read(board_key_t *key);
esp_err_t board_expander_update(uint16_t mask, uint16_t value);
esp_err_t board_codec_write(uint8_t reg, uint8_t value);
esp_err_t board_codec_init(void);
esp_err_t board_speaker(bool enabled);
esp_err_t board_display_init(void);
esp_err_t board_display_line(unsigned row, const char *text, bool selected);
bool board_display_available(void);
#define RECORDING_DIR "/sdcard/recordings"
