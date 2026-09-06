#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#define RECORDER_CONTROL_MAX_JSON 496
esp_err_t recorder_setup_start(void);
void recorder_setup_stop(void);
void recorder_setup_tick(void);
bool recorder_setup_active(void);
esp_err_t recorder_control(uint32_t session_id, const uint8_t *input, ssize_t input_len,
    uint8_t **output, ssize_t *output_len, void *context);
