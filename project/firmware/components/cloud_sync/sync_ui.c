#include "cloud_sync.h"

const char *cloud_sync_phase_name(sync_phase_t phase)
{
    static const char *names[] = {"UPLOADING", "WARMING", "CHECKING TRANSCRIPT",
        "TRANSCRIBING", "UPLOADED PROCESS PENDING", "TRANSCRIBED", "UPLOADED >30MIN SKIPPED",
        "REMOTE DELETED SKIPPED", "RESOLVING", "DOWNLOADING", "VALIDATING",
        "SAVING TRANSCRIPT", "VERIFYING", "PROCESS CONNECTING", "PROCESSING"};
    return (unsigned)phase < sizeof(names) / sizeof(*names) ? names[phase] : "SYNC";
}
const char *cloud_sync_result_name(processing_result_t result)
{
    static const char *names[] = {"completed", "processing_in_progress", "consent_required",
        "authentication_required", "temporarily_unavailable", "processing_deadline",
        "invalid_response", "source_changed", "item_not_allowed", "source_not_found",
        "output_conflict", "unsupported_audio", "busy", "invalid_request", "cancelled",
        "outbox_storage_error"};
    return (unsigned)result < sizeof(names) / sizeof(*names) ? names[result] : "invalid_response";
}
const char *cloud_sync_summary(sync_status_t status)
{
    if (status.error != ESP_OK) return "SYNC INCOMPLETE LOCAL SAFE";
    if (status.processing_pending) return "UPLOADED PROCESS PENDING";
    if (status.processing_missing) return "SYNC DONE DELETED SKIPPED";
    if (status.processing_skipped) return "SYNC DONE >30MIN SKIPPED";
    return "SYNC DONE TRANSCRIPTS OK";
}
void cloud_sync_format_status(sync_status_t status, char *text, size_t capacity)
{
    const char *label = status.phase == SYNC_TRANSCRIBING && status.processing_bytes_known ?
        "SPEECH UPLOAD" : cloud_sync_phase_name(status.phase);
    if (status.phase == SYNC_UPLOADING)
        snprintf(text, capacity, "%s %u %lu%%", label, status.files_done,
            (unsigned long)(status.total_bytes ? (uint64_t)status.confirmed_bytes * 100 / status.total_bytes : 0));
    else if (status.processing_bytes_known && status.processing_total)
        snprintf(text, capacity, "%s %lu%%", label,
            (unsigned long)((uint64_t)status.processing_bytes * 100 / status.processing_total));
    else snprintf(text, capacity, "%s", label);
}
