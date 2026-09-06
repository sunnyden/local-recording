#pragma once
#include "esp_err.h"
#include <stdbool.h>
esp_err_t board_speaker(bool);
esp_err_t board_codec_init(void);
