#pragma once
#include "esp_err.h"
#include <stdbool.h>
#define RECORDING_DIR "."
esp_err_t board_speaker(bool enabled);
esp_err_t board_display_line(unsigned row, const char *text, bool selected);
bool board_display_available(void);
