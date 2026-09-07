#include "processing_protocol.h"
#include "recorder_core.h"
#include "json_limits.h"
#include <string.h>

cJSON *processing_parse_message(const uint8_t *bytes, size_t size)
{
    if (!bytes || !size || size > 4096 || json_has_nul(bytes, size) ||
        !recorder_json_depth_ok(bytes, size, 8)) return NULL;
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts((const char *)bytes, size, &end, false);
    while (json && end < (const char *)bytes + size &&
           (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    if (!json || end != (const char *)bytes + size) { cJSON_Delete(json); return NULL; }
    return json;
}
static bool unique_fields(cJSON *json)
{
    bool object = cJSON_IsObject(json);
    if (!object && !cJSON_IsArray(json)) return true;
    for (cJSON *a = json->child; a; a = a->next) {
        if (object)
            for (cJSON *b = a->next; b; b = b->next)
                if (!strcmp(a->string, b->string)) return false;
        if (!unique_fields(a)) return false;
    }
    return true;
}
bool processing_versioned(cJSON *json)
{
    cJSON *version = cJSON_GetObjectItemCaseSensitive(json, "v");
    return cJSON_IsObject(json) && cJSON_IsNumber(version) && version->valuedouble == 1 && unique_fields(json);
}
bool processing_identifier(cJSON *json, const char *key, char *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    const char *value = cJSON_IsString(item) ? item->valuestring : NULL;
    if (!value || !*value || strlen(value) > 128) return false;
    for (const char *p = value; *p; ++p)
        if ((unsigned char)*p < 32 || (unsigned char)*p > 126) return false;
    if (out) strcpy(out, value);
    return true;
}
processing_result_t processing_error_result(int status, cJSON *json)
{
    if (!processing_versioned(json)) return PROCESS_BAD_RESPONSE;
    cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
    cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
    if (!cJSON_IsString(code) || !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(error, "retryable")))
        return PROCESS_BAD_RESPONSE;
    static const struct { int status; const char *code; processing_result_t result; } codes[] = {
        {400, "invalid_request", PROCESS_INVALID_REQUEST},
        {401, "authentication_required", PROCESS_AUTHENTICATION},
        {403, "consent_required", PROCESS_CONSENT},
        {403, "item_not_allowed", PROCESS_NOT_ALLOWED},
        {404, "source_not_found", PROCESS_NOT_FOUND},
        {409, "source_changed", PROCESS_SOURCE_CHANGED},
        {409, "processing_in_progress", PROCESS_WAITING},
        {409, "output_conflict", PROCESS_CONFLICT},
        {415, "unsupported_audio", PROCESS_UNSUPPORTED},
        {429, "busy", PROCESS_BUSY},
        {503, "temporarily_unavailable", PROCESS_UNAVAILABLE},
        {504, "processing_deadline", PROCESS_TIMEOUT}
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(*codes); ++i)
        if ((!status || status == codes[i].status) && !strcmp(code->valuestring, codes[i].code))
            return codes[i].result;
    return PROCESS_BAD_RESPONSE;
}
