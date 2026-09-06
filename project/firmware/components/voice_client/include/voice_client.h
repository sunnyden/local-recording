#pragma once
#include "esp_err.h"
#include <stdbool.h>
typedef struct { bool active; esp_err_t error; const char *state; } voice_status_t;
esp_err_t voice_client_start(void);
void voice_client_stop(void);
voice_status_t voice_client_status(void);
