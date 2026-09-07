#pragma once
#include "cloud_sync.h"

bool processing_versioned(cJSON *json);
bool processing_identifier(cJSON *json, const char *key, char *out);
processing_result_t processing_error_result(int http_status, cJSON *json);
cJSON *processing_parse_message(const uint8_t *bytes, size_t size);
