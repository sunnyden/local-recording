#include "cloud_sync.h"
#include "recorder_network.h"
#include "identity.h"
#include "recorder.h"
#include "board.h"
#include "esp_random.h"
#include "esp_log.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define GRAPH "https://graph.microsoft.com/v1.0/me/drive"
static _Atomic bool active, cancelled;
static _Atomic int error_code;
static _Atomic uint32_t confirmed, total_bytes;
static _Atomic unsigned done_count;
bool cloud_sync_cancelled(void) { return atomic_load(&cancelled); }
void cloud_sync_cancel(void) { atomic_store(&cancelled, true); }
void cloud_sync_progress(uint32_t bytes, uint32_t total)
{
    atomic_store(&confirmed, bytes); atomic_store(&total_bytes, total);
}
sync_status_t cloud_sync_status(void)
{
    return (sync_status_t){.active = atomic_load(&active), .error = atomic_load(&error_code),
        .confirmed_bytes = atomic_load(&confirmed), .total_bytes = atomic_load(&total_bytes),
        .files_done = atomic_load(&done_count)};
}
static const char *get_string(cJSON *json, const char *name)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(json, name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static esp_err_t graph_request(const char *phase, const char *url, esp_http_client_method_t method,
    const char *body, int *status, cJSON **json)
{
    *status = 0;
    *json = NULL;
    char *token = malloc(8193);
    if (!token) return ESP_ERR_NO_MEM;
    esp_err_t err = identity_access(AUTH_GRAPH, false, token, 8193);
    unsigned retry_after = 0;
    for (unsigned attempt = 0; err == ESP_OK && attempt < 4; ++attempt) {
        err = https_json_retry_info(url, method, token, "application/json", body,
                                   status, json, &retry_after);
        if (err == ESP_OK && *status != 429 && *status < 500) break;
        if (cloud_sync_cancelled() || attempt == 3 || retry_after > 120) break;
        unsigned wait = retry_after ? retry_after : (1u << (attempt + 1)) + esp_random() % 3;
        cJSON_Delete(*json); *json = NULL;
        for (unsigned i = 0; i < wait * 10 && !cloud_sync_cancelled(); ++i)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (cloud_sync_cancelled()) { err = ESP_ERR_INVALID_STATE; break; }
        err = ESP_OK;
    }
    if (err == ESP_OK && *status == 401) {
        cJSON_Delete(*json); *json = NULL;
        err = identity_access(AUTH_GRAPH, true, token, 8193);
        if (err == ESP_OK) err = https_json(url, method, token, "application/json", body, status, json);
    }
    if (err != ESP_OK || (*status >= 400 && *status != 404)) {
        ESP_LOGE("sync", "%s failed: transport=%s HTTP=%d",
                 phase, esp_err_to_name(err), *status);
    }
    secret_zero(token, 8193); free(token);
    return err;
}
static esp_err_t ensure_folder(char drive[129])
{
    cJSON *json = NULL; int status;
    esp_err_t err = graph_request("drive lookup", GRAPH "?$select=id", HTTP_METHOD_GET, NULL, &status, &json);
    const char *id = get_string(json, "id");
    if (err != ESP_OK || status != 200 || !id || strlen(id) > 128) {
        cJSON_Delete(json); return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }
    strcpy(drive, id); cJSON_Delete(json); json = NULL;
    err = graph_request("folder lookup", GRAPH "/root:/local-recording", HTTP_METHOD_GET, NULL, &status, &json);
    if (err == ESP_OK && status == 404) {
        cJSON_Delete(json); json = NULL;
        err = graph_request("folder creation", GRAPH "/root/children", HTTP_METHOD_POST,
            "{\"name\":\"local-recording\",\"folder\":{},\"@microsoft.graph.conflictBehavior\":\"fail\"}",
            &status, &json);
    }
    bool valid = err == ESP_OK && (status == 200 || status == 201) &&
                 cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(json, "folder"));
    cJSON_Delete(json);
    return valid ? ESP_OK : (err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE);
}
static esp_err_t hash_file(FILE *file, char hex[41], uint32_t *size)
{
    if (fseek(file, 0, SEEK_END)) return ESP_FAIL;
    long length = ftell(file);
    if (length < 44 || (uint64_t)length > WAV_MAX_DATA + 44ULL || fseek(file, 0, SEEK_SET))
        return ESP_ERR_INVALID_SIZE;
    *size = length;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash, PSA_ALG_SHA_1) != PSA_SUCCESS) return ESP_FAIL;
    uint8_t bytes[4096], digest[20];
    esp_err_t err = ESP_OK;
    for (;;) {
        if (cloud_sync_cancelled()) { err = ESP_ERR_INVALID_STATE; break; }
        size_t n = fread(bytes, 1, sizeof(bytes), file);
        if (n && psa_hash_update(&hash, bytes, n) != PSA_SUCCESS) { err = ESP_FAIL; break; }
        if (n < sizeof(bytes)) { if (ferror(file)) err = ESP_FAIL; break; }
    }
    size_t out = 0;
    if (err == ESP_OK && psa_hash_finish(&hash, digest, sizeof(digest), &out) != PSA_SUCCESS)
        err = ESP_FAIL;
    psa_hash_abort(&hash);
    if (err == ESP_OK) {
        for (unsigned i = 0; i < sizeof(digest); ++i) sprintf(hex + i * 2, "%02x", digest[i]);
    }
    return err;
}
static bool completed_item(cJSON *json, const char *name, uint32_t size)
{
    const char *remote_name = get_string(json, "name"), *id = get_string(json, "id");
    cJSON *remote_size = cJSON_GetObjectItemCaseSensitive(json, "size");
    return id && remote_name && !strcmp(remote_name, name) &&
        cJSON_IsNumber(remote_size) && remote_size->valuedouble == size &&
        cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(json, "file"));
}
static bool matching_item(cJSON *json, const char *name, uint32_t size, const char *hash)
{
    if (!completed_item(json, name, size)) return false;
    cJSON *file = cJSON_GetObjectItemCaseSensitive(json, "file");
    const char *remote_hash = get_string(cJSON_GetObjectItemCaseSensitive(file, "hashes"), "sha1Hash");
    return remote_hash && !strcasecmp(remote_hash, hash);
}
static esp_err_t sync_one(const char *name, const char *drive)
{
    char path[128], url[256], hash[41];
    snprintf(path, sizeof(path), RECORDING_DIR "/%s", name);
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_FAIL;
    wav_info_t wave;
    if (!wav_parse(file, &wave)) { fclose(file); return ESP_ERR_NOT_SUPPORTED; }
    uint32_t size = 0;
    esp_err_t err = hash_file(file, hash, &size);
    if (err != ESP_OK) { fclose(file); return err; }
    cloud_sync_progress(0, size);
    snprintf(url, sizeof(url), GRAPH "/root:/local-recording/%s", name);
    cJSON *json = NULL; int status;
    err = graph_request("recording lookup", url, HTTP_METHOD_GET, NULL, &status, &json);
    if (err == ESP_OK && status == 200) {
        bool match = matching_item(json, name, size, hash);
        cJSON_Delete(json); fclose(file);
        if (match) cloud_sync_progress(size, size);
        return match ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    cJSON_Delete(json); json = NULL;
    if (err != ESP_OK || status != 404) {
        fclose(file); return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }
    char session_url[300];
    snprintf(session_url, sizeof(session_url), "%s:/createUploadSession", url);
    err = graph_request("upload session", session_url, HTTP_METHOD_POST,
        "{\"item\":{\"@microsoft.graph.conflictBehavior\":\"fail\"}}", &status, &json);
    const char *capability = get_string(json, "uploadUrl");
    if (err != ESP_OK || status != 200 || !capability || strlen(capability) > 4096 ||
        strncmp(capability, "https://", 8)) {
        cJSON_Delete(json); fclose(file);
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }
    char *upload_url = strdup(capability);
    secret_zero((void *)capability, strlen(capability));
    cJSON_Delete(json); json = NULL;
    if (!upload_url) { fclose(file); return ESP_ERR_NO_MEM; }
    err = upload_file_ranges(file, size, upload_url, &json);
    secret_zero(upload_url, strlen(upload_url)); free(upload_url);
    fclose(file);
    bool complete = err == ESP_OK && completed_item(json, name, size);
    cJSON_Delete(json); json = NULL;
    if (!complete && !cloud_sync_cancelled()) {
        /* Lost final response: verify immutable remote identity, never rename or overwrite. */
        err = graph_request("completed recording verification", url, HTTP_METHOD_GET, NULL, &status, &json);
        complete = err == ESP_OK && status == 200 && matching_item(json, name, size, hash);
    }
    cJSON_Delete(json);
    if (!complete) return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    /* A compact protected journal binds the latest confirmed completion to drive.
       Older entries are reconciled by remote hash rather than trusting SD marks. */
    char journal[256];
    snprintf(journal, sizeof(journal), "%s|%s|%s", drive, name, hash);
    err = credential_write("last_sync", journal);
    if (err == ESP_OK) cloud_sync_progress(size, size);
    return err;
}
static void sync_worker(void *arg)
{
    (void)arg;
    char drive[129], name[65]; size_t count = 0;
    esp_err_t err = ensure_folder(drive);
    for (size_t index = 0; err == ESP_OK && !cloud_sync_cancelled(); ++index) {
        err = storage_catalog(index, name, sizeof(name), &count);
        if (err != ESP_OK || index >= count) break;
        err = sync_one(name, drive);
        if (err == ESP_OK) atomic_fetch_add(&done_count, 1);
    }
    atomic_store(&error_code, cloud_sync_cancelled() ? ESP_ERR_INVALID_STATE : err);
    atomic_store(&active, false);
    vTaskDelete(NULL);
}
esp_err_t cloud_sync_start(void)
{
    if (!recorder_network_ready() || !recorder_time_valid() || !credential_storage_allowed())
        return ESP_ERR_INVALID_STATE;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&active, &expected, true)) return ESP_ERR_INVALID_STATE;
    atomic_store(&cancelled, false); atomic_store(&done_count, 0);
    atomic_store(&error_code, ESP_OK); cloud_sync_progress(0, 0);
    if (xTaskCreate(sync_worker, "cloud_sync", 12288, NULL, 4, NULL) != pdPASS) {
        atomic_store(&active, false); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
