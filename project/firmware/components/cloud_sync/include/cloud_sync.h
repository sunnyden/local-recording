#pragma once
#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    SYNC_UPLOADING, SYNC_WARMING, SYNC_CHECKING, SYNC_TRANSCRIBING,
    SYNC_UPLOADED_PENDING, SYNC_TRANSCRIBED, SYNC_SKIPPED, SYNC_REMOTE_MISSING,
    SYNC_RESOLVING, SYNC_DOWNLOADING, SYNC_VALIDATING, SYNC_SAVING, SYNC_VERIFYING,
    SYNC_PROCESS_CONNECTING, SYNC_PROCESSING
} sync_phase_t;
typedef enum {
    PROCESS_OK, PROCESS_WAITING, PROCESS_CONSENT, PROCESS_AUTHENTICATION,
    PROCESS_UNAVAILABLE, PROCESS_TIMEOUT, PROCESS_BAD_RESPONSE, PROCESS_SOURCE_CHANGED,
    PROCESS_NOT_ALLOWED, PROCESS_NOT_FOUND, PROCESS_CONFLICT, PROCESS_UNSUPPORTED,
    PROCESS_BUSY, PROCESS_INVALID_REQUEST, PROCESS_CANCELLED, PROCESS_STORAGE_ERROR
} processing_result_t;
typedef struct {
    bool active;
    esp_err_t error;
    unsigned files_done;
    uint32_t confirmed_bytes, total_bytes;
    sync_phase_t phase;
    processing_result_t processing_result;
    unsigned processing_pending, processing_completed, processing_skipped, processing_missing;
    bool processing_bytes_known;
    uint32_t processing_bytes, processing_total;
} sync_status_t;
esp_err_t cloud_sync_start(void);
void cloud_sync_cancel(void);
sync_status_t cloud_sync_status(void);
bool cloud_sync_cancelled(void);
void cloud_sync_progress(uint32_t bytes, uint32_t total);
void cloud_sync_phase(sync_phase_t phase);
void cloud_sync_processing_progress(sync_phase_t phase, bool known, uint32_t bytes, uint32_t total);
const char *cloud_sync_phase_name(sync_phase_t phase);
const char *cloud_sync_result_name(processing_result_t result);
const char *cloud_sync_summary(sync_status_t status);
void cloud_sync_format_status(sync_status_t status, char *text, size_t capacity);
esp_err_t upload_file_ranges(FILE *file, uint32_t total, const char *upload_url,
                            cJSON **completed_item);
