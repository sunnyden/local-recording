#include "recorder_provisioning.h"
#include "recorder_network.h"
#include "identity.h"
#include "recorder_core.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static const char *string_field(cJSON *json, const char *name)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, name);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}
static void error(cJSON *out, const char *code)
{
    cJSON_AddBoolToObject(out, "ok", false);
    cJSON_AddStringToObject(out, "code", code);
    cJSON_AddStringToObject(out, "message", "Request rejected; check device status");
}
static bool valid_id(const char *id)
{
    if (!id || !id[0] || strlen(id) > 64) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (*p < 33 || *p > 126) return false;
    return true;
}
static bool duplicate_keys(cJSON *json)
{
    for (cJSON *a = json->child; a; a = a->next)
        for (cJSON *b = a->next; b; b = b->next)
            if (a->string && b->string && !strcmp(a->string, b->string)) return true;
    return false;
}
static void dispatch(cJSON *request, cJSON *out)
{
    const char *type = string_field(request, "type");
    const char *res = string_field(request, "resource");
    if (!type) { error(out, "invalid_request"); return; }
    if (!strcmp(type, "status")) {
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "wifi", recorder_network_ready() ? "connected" : "disconnected");
        cJSON_AddBoolToObject(out, "time_valid", recorder_time_valid());
        cJSON *auth = cJSON_AddObjectToObject(out, "auth");
        cJSON_AddStringToObject(auth, "graph", identity_state_name(identity_status(AUTH_GRAPH).state));
        cJSON_AddStringToObject(auth, "proxy", identity_state_name(identity_status(AUTH_PROXY).state));
        return;
    }
    if (!strcmp(type, "setup.finish")) {
        cJSON_AddBoolToObject(out, "ok", true);
        recorder_setup_stop();
        return;
    }
    if (!strcmp(type, "auth.unlink")) {
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "confirm")))
            error(out, "confirmation_required");
        else if (identity_unlink() != ESP_OK) error(out, "storage_error");
        else cJSON_AddBoolToObject(out, "ok", true);
        return;
    }
    if (strcmp(type, "auth.start") && strcmp(type, "auth.status") && strcmp(type, "auth.cancel")) {
        error(out, "unknown_type"); return;
    }
    if (!res || (strcmp(res, "graph") && strcmp(res, "proxy"))) {
        error(out, "invalid_resource"); return;
    }
    auth_resource_t resource = !strcmp(res, "graph") ? AUTH_GRAPH : AUTH_PROXY;
    auth_status_t status = identity_status(resource);
    if (!strcmp(type, "auth.cancel")) {
        identity_cancel(resource);
        cJSON_AddBoolToObject(out, "ok", true);
        return;
    }
    if (!strcmp(type, "auth.start")) {
        if (status.state != AUTH_PENDING && status.state != AUTH_AUTHORIZED) {
            esp_err_t err = identity_start(resource);
            if (err != ESP_OK) { error(out, "auth_unavailable"); return; }
            status = identity_status(resource);
        }
    }
    if (status.state == AUTH_PENDING && status.user_code[0]) {
        cJSON_AddStringToObject(out, "user_code", status.user_code);
        cJSON_AddStringToObject(out, "verification_uri", status.verification_uri);
        cJSON_AddNumberToObject(out, "expires_in", status.expires_in);
    }
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "state", identity_state_name(status.state));
}
esp_err_t recorder_control(uint32_t session_id, const uint8_t *input, ssize_t input_len,
    uint8_t **output, ssize_t *output_len, void *context)
{
    (void)session_id; (void)context;
    if (!output || !output_len) return ESP_ERR_INVALID_ARG;
    *output = NULL; *output_len = 0;
    if (!input || input_len <= 0 || input_len > RECORDER_CONTROL_MAX_JSON)
        return ESP_ERR_INVALID_SIZE;
    char *copy = malloc(input_len + 1);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, input, input_len); copy[input_len] = 0;
    const char *end = NULL;
    cJSON *request = cJSON_ParseWithLengthOpts(copy, input_len + 1, &end, true);
    cJSON *out = cJSON_CreateObject();
    if (!out) { cJSON_Delete(request); free(copy); return ESP_ERR_NO_MEM; }
    cJSON_AddNumberToObject(out, "v", 1);
    const char *id = string_field(request, "id");
    if (valid_id(id)) cJSON_AddStringToObject(out, "id", id);
    cJSON *version = cJSON_GetObjectItemCaseSensitive(request, "v");
    if (json_has_nul(input, input_len) || !cJSON_IsObject(request) ||
        end != copy + input_len || !valid_id(id) || duplicate_keys(request) ||
        !cJSON_IsNumber(version) || version->valuedouble != 1)
        error(out, "invalid_request");
    else dispatch(request, out);
    char *encoded = cJSON_PrintUnformatted(out);
    cJSON_Delete(request); cJSON_Delete(out);
    secret_zero(copy, input_len); free(copy);
    if (!encoded) return ESP_ERR_NO_MEM;
    if (strlen(encoded) > RECORDER_CONTROL_MAX_JSON) {
        free(encoded);
        return ESP_ERR_INVALID_SIZE;
    }
    *output = (uint8_t *)encoded; *output_len = strlen(encoded);
    return ESP_OK;
}
