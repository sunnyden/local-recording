#include "cloud_sync.h"

const char *cloud_sync_phase_name(sync_phase_t phase)
{
    static const char *names[] = {"UPLOADING", "WARMING", "CHECKING TRANSCRIPT",
        "TRANSCRIBING", "UPLOADED PROCESS PENDING", "TRANSCRIBED", "UPLOADED >30MIN SKIPPED"};
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
    if (status.processing_skipped) return "SYNC DONE >30MIN SKIPPED";
    return "SYNC DONE TRANSCRIPTS OK";
}
