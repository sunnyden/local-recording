#pragma once
#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    bool active;
    esp_err_t error;
    unsigned files_done;
    uint32_t confirmed_bytes, total_bytes;
} sync_status_t;
esp_err_t cloud_sync_start(void);
void cloud_sync_cancel(void);
sync_status_t cloud_sync_status(void);
bool cloud_sync_cancelled(void);
void cloud_sync_progress(uint32_t bytes, uint32_t total);
esp_err_t upload_file_ranges(FILE *file, uint32_t total, const char *upload_url,
                            cJSON **completed_item);
