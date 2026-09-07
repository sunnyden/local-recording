#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum { PROCESS_PENDING, PROCESS_COMPLETED, PROCESS_TOO_LONG } processing_state_t;
typedef struct {
    uint32_t source_size, state;
    int64_t retry_not_before;
    char name[65], drive_id[129], item_id[129], source_sha1[41], recorded_at[36];
    char json_item_id[129], text_item_id[129];
} processing_job_t;
bool processing_job_valid(const processing_job_t *job);
esp_err_t processing_outbox_load(const char *name, processing_job_t *job);
esp_err_t processing_outbox_save(const processing_job_t *job);
esp_err_t processing_outbox_catalog(size_t index, char name[65], size_t *count);
