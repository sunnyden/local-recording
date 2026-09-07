#pragma once
#include "processing_outbox.h"
#include "cloud_sync.h"

processing_result_t processing_stream_run(const char *body, const char *operation_id,
                                          processing_job_t *job, bool *uncertain);
