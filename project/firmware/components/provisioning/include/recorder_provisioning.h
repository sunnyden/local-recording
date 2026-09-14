#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#define RECORDER_CONTROL_MAX_JSON 496
typedef struct {
    /* Copy full values if retained. Return ESP_OK only after the credentials
       have been drawn successfully; BLE advertising starts afterwards. */
    esp_err_t (*show)(void *context, const char *service, const char *username,
                      const char *password, unsigned expires_seconds);
    void (*clear)(void *context);
    void *context;
} recorder_setup_display_t;
/* Call set_display/start/stop/tick only from the main/UI owner. Both callbacks
   are required; the descriptor is copied, context must live through cleanup.
   Registration is rejected until setup is fully inactive; NULL restores the
   legacy display adapter. clear also runs after a partially failed show. */
esp_err_t recorder_setup_set_display(const recorder_setup_display_t *display);
esp_err_t recorder_setup_start(void);
void recorder_setup_stop(void);
void recorder_setup_tick(void);
bool recorder_setup_active(void);
esp_err_t recorder_control(uint32_t session_id, const uint8_t *input, ssize_t input_len,
    uint8_t **output, ssize_t *output_len, void *context);
