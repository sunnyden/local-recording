#include "cloud_sync.h"
#include "processing_client.h"
#include "https_operation.h"
#include "recorder.h"
#include "identity.h"
#include "freertos/task.h"
#include <assert.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

enum scenario { SUCCESS, LOST_UPLOAD, PROCESS_TIMEOUT_CASE, PROCESS_MALFORMED,
    MISSING_CONSENT, SERVER_CONSENT, WARM_TIMEOUT, CANCEL_WARM, ALREADY_COMPLETE,
    SERVER_TOO_LONG, SERVER_BUSY, STATUS_PROCESSING, WRONG_RESPONSE_VERSION, CANCEL_PROCESS };
static enum scenario scenario;
static bool remote_exists;
static unsigned uploads, graph_requests, process_requests, status_requests, warm_requests;
static unsigned graph_tokens, proxy_tokens;
static int64_t now;
static const char *current_name = "rec-test.wav";
static uint32_t source_size = 32044;
static const char *sha1 = "abababababababababababababababababababab";
static bool saw_transcribing, saw_warming;
static bool expect_timestamp;
bool recorder_network_ready(void) { return true; }
bool recorder_time_valid(void) { return true; }
bool credential_storage_allowed(void) { return true; }
uint32_t esp_random(void) { return 1; }
int64_t esp_timer_get_time(void) { return now; }
time_t processing_test_time(time_t *out)
{
    time_t stamp = (time_t)(1788789936 + now / 1000000);
    if (out) *out = stamp;
    return stamp;
}
void secret_zero(void *p, size_t n) { memset(p, 0, n); }
void vTaskDelay(TickType_t ticks) { now += ticks * 1000LL; }
void vTaskDelete(void *task) { (void)task; }
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, unsigned stack,
    void *arg, unsigned priority, void *handle)
{
    (void)name; (void)stack; (void)priority; (void)handle;
    function(arg); return pdPASS;
}
esp_err_t identity_access(auth_resource_t resource, bool refresh, char *token, size_t capacity)
{
    (void)refresh;
    assert(capacity >= 16);
    if (resource == AUTH_GRAPH) { ++graph_tokens; strcpy(token, "GRAPH-ONLY"); }
    else {
        ++proxy_tokens;
        if (scenario == MISSING_CONSENT) return ESP_ERR_INVALID_STATE;
        strcpy(token, "API-B-ONLY");
    }
    return ESP_OK;
}
static cJSON *remote_item(void)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id", "remote-item");
    cJSON_AddStringToObject(item, "name", current_name);
    cJSON_AddNumberToObject(item, "size", source_size);
    cJSON *file = cJSON_AddObjectToObject(item, "file");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(file, "hashes"), "sha1Hash", sha1);
    return item;
}
esp_err_t https_json_retry_info(const char *url, esp_http_client_method_t method,
    const char *token, const char *type, const char *body, int *status, cJSON **json, unsigned *retry)
{
    (void)type; (void)body;
    assert(!strcmp(token, "GRAPH-ONLY"));
    assert(!strncmp(url, "https://graph.microsoft.com/", 28));
    ++graph_requests; if (retry) *retry = 0;
    *status = 200;
    if (strstr(url, "?$select=id")) *json = cJSON_Parse("{\"id\":\"drive-A\"}");
    else if (strstr(url, "createUploadSession")) {
        assert(method == HTTP_METHOD_POST);
        *json = cJSON_Parse("{\"uploadUrl\":\"https://upload.invalid/capability\"}");
    } else if (strstr(url, ".wav")) {
        if (remote_exists) *json = remote_item();
        else { *status = 404; *json = cJSON_CreateObject(); }
    } else *json = cJSON_Parse("{\"folder\":{}}");
    return ESP_OK;
}
esp_err_t https_json(const char *url, esp_http_client_method_t method, const char *token,
    const char *type, const char *body, int *status, cJSON **json)
{ return https_json_retry_info(url, method, token, type, body, status, json, NULL); }
esp_err_t upload_file_ranges(FILE *file, uint32_t total, const char *url, cJSON **item)
{
    assert(file && total == source_size && !strcmp(url, "https://upload.invalid/capability"));
    ++uploads; remote_exists = true;
    if (scenario == LOST_UPLOAD) { *item = NULL; return ESP_ERR_TIMEOUT; }
    *item = remote_item(); return ESP_OK;
}
static const char *complete_json =
    "{\"v\":1,\"status\":\"completed\",\"operation_id\":\"operation\","
    "\"json_item_id\":\"json-sidecar\",\"text_item_id\":\"text-sidecar\"}";
esp_err_t https_json_operation(const char *url, esp_http_client_method_t method,
    const char *token, const char *type, const char *body, int *status, cJSON **json,
    unsigned *retry, const https_operation_t *operation)
{
    assert(!strncmp(url, "https://proxy.invalid/", 22));
    assert(operation && operation->response_limit == 4096 && operation->cancelled);
    if (retry) *retry = 0;
    *status = 200; *json = NULL;
    if (strstr(url, "/readyz")) {
        ++warm_requests;
        assert(method == HTTP_METHOD_GET && !token && !body);
        saw_warming |= cloud_sync_status().phase == SYNC_WARMING;
        if (scenario == CANCEL_WARM) cloud_sync_cancel();
        if (scenario == WARM_TIMEOUT) {
            now += operation->timeout_ms * 1000LL;
            *status = 503; *json = cJSON_Parse("{\"status\":\"warming\"}");
        } else *json = cJSON_Parse("{\"status\":\"ready\"}");
        return ESP_OK;
    }
    assert(method == HTTP_METHOD_POST && !strcmp(token, "API-B-ONLY"));
    assert(!strcmp(type, "application/json") && body && strlen(body) <= 4096);
    cJSON *request = cJSON_Parse(body);
    assert(request && cJSON_GetArraySize(request) == (expect_timestamp ? 6 : 5));
    assert(cJSON_GetObjectItemCaseSensitive(request, "v")->valueint == 1);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(request, "drive_id")->valuestring, "drive-A"));
    assert(!strncmp(cJSON_GetObjectItemCaseSensitive(request, "item_id")->valuestring, "remote-item", 11));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(request, "source_sha1")->valuestring, sha1));
    assert(cJSON_GetObjectItemCaseSensitive(request, "source_size")->valuedouble == source_size);
    if (expect_timestamp)
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(request, "recorded_at")->valuestring,
            "2026-09-07T22:05:36+08:00"));
    cJSON_Delete(request);
    if (strstr(url, "/status")) {
        ++status_requests;
        assert(operation->timeout_ms == 10000);
        if (scenario == ALREADY_COMPLETE) *json = cJSON_Parse(complete_json);
        else if (scenario == STATUS_PROCESSING)
            *json = cJSON_Parse("{\"v\":1,\"status\":\"processing\",\"operation_id\":\"operation\"}");
        else *json = cJSON_Parse("{\"v\":1,\"status\":\"not_started\",\"operation_id\":\"operation\"}");
        return ESP_OK;
    }
    assert(strstr(url, "/process"));
    assert(operation->timeout_ms == 210000);
    ++process_requests;
    saw_transcribing |= cloud_sync_status().phase == SYNC_TRANSCRIBING;
    if (scenario == PROCESS_TIMEOUT_CASE) { now += 210000000LL; return ESP_ERR_TIMEOUT; }
    if (scenario == CANCEL_PROCESS) { cloud_sync_cancel(); return ESP_ERR_INVALID_STATE; }
    if (scenario == PROCESS_MALFORMED) {
        *json = cJSON_Parse("{\"v\":1,\"status\":\"completed\",\"operation_id\":\"op\",\"json_item_id\":\"json\"}");
    } else if (scenario == WRONG_RESPONSE_VERSION) {
        *json = cJSON_Parse("{\"v\":2,\"status\":\"completed\",\"operation_id\":\"op\",\"json_item_id\":\"j\",\"text_item_id\":\"t\"}");
    } else if (scenario == SERVER_CONSENT) {
        *status = 403; *json = cJSON_Parse("{\"v\":1,\"error\":{\"code\":\"consent_required\",\"retryable\":false}}");
    } else if (scenario == SERVER_TOO_LONG) {
        *status = 413; *json = cJSON_Parse("{\"v\":1,\"error\":{\"code\":\"recording_too_long\",\"retryable\":false}}");
    } else if (scenario == SERVER_BUSY) {
        *status = 429; if (retry) *retry = 60;
        *json = cJSON_Parse("{\"v\":1,\"error\":{\"code\":\"busy\",\"retryable\":true}}");
    } else *json = cJSON_Parse(complete_json);
    return ESP_OK;
}
static void clear_files(void)
{
    DIR *dir = opendir(RECORDING_DIR); assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[256]; snprintf(path, sizeof(path), RECORDING_DIR "/%s", entry->d_name);
        assert(!unlink(path));
    }
    closedir(dir);
}
static void create_wave(void)
{
    char path[128]; snprintf(path, sizeof(path), RECORDING_DIR "/%s", current_name);
    FILE *f = fopen(path, "wb"); assert(f);
    uint8_t header[44]; assert(wav_header(header, source_size - 44));
    assert(fwrite(header, 1, 44, f) == 44);
    assert(!fseek(f, source_size - 1, SEEK_SET) && fputc(0, f) == 0);
    fclose(f);
}
static void reset(enum scenario next, bool exists)
{
    clear_files(); scenario = next; remote_exists = exists;
    uploads = graph_requests = process_requests = status_requests = warm_requests = 0;
    graph_tokens = proxy_tokens = 0; now = 0; saw_warming = saw_transcribing = false;
    source_size = 32044; current_name = "rec-test.wav";
    expect_timestamp = false;
    create_wave();
}
static processing_job_t receipt(void)
{
    processing_job_t job;
    assert(processing_outbox_load(current_name, &job) == ESP_OK);
    assert(!strcmp(job.drive_id, "drive-A") && !strcmp(job.item_id, "remote-item"));
    assert(!strcmp(job.source_sha1, sha1) && job.source_size == source_size);
    return job;
}
int main(void)
{
    _mkdir(RECORDING_DIR);
    for (unsigned path = 0; path < 3; ++path) {
        reset(path == 2 ? LOST_UPLOAD : SUCCESS, path == 1);
        assert(cloud_sync_start() == ESP_OK);
        sync_status_t status = cloud_sync_status();
        assert(!status.active && status.error == ESP_OK && status.files_done == 1);
        assert(status.processing_pending == 0 && status.processing_completed == 1);
        assert(uploads == (path == 1 ? 0u : 1u) && process_requests == 1 && status_requests == 1);
        assert(saw_warming && saw_transcribing && graph_tokens && proxy_tokens);
        assert(receipt().state == PROCESS_COMPLETED);
        unsigned previous_uploads = uploads, previous_process = process_requests;
        assert(cloud_sync_start() == ESP_OK);
        assert(uploads == previous_uploads && process_requests == previous_process);
    }
    const enum scenario pending[] = {PROCESS_TIMEOUT_CASE, PROCESS_MALFORMED, MISSING_CONSENT,
        SERVER_CONSENT, WARM_TIMEOUT, CANCEL_WARM, SERVER_BUSY, STATUS_PROCESSING,
        WRONG_RESPONSE_VERSION, CANCEL_PROCESS};
    for (size_t i = 0; i < sizeof(pending) / sizeof(*pending); ++i) {
        reset(pending[i], false);
        assert(cloud_sync_start() == ESP_OK);
        sync_status_t status = cloud_sync_status();
        if (status.processing_pending != 1 || status.processing_completed)
            fprintf(stderr, "Pending scenario %d: pending=%u completed=%u error=%d result=%d\n",
                pending[i], status.processing_pending, status.processing_completed,
                status.error, status.processing_result);
        assert(status.processing_pending == 1 && !status.processing_completed);
        assert(receipt().state == PROCESS_PENDING);
        assert(strcmp(cloud_sync_summary(status), "SYNC DONE TRANSCRIPTS OK"));
        if (pending[i] == WARM_TIMEOUT) assert(now <= 120000000LL && !process_requests);
        if (pending[i] == MISSING_CONSENT || pending[i] == SERVER_CONSENT)
            assert(status.processing_result == PROCESS_CONSENT);
        assert(!unlink(RECORDING_DIR "/rec-test.wav")); /* Boot/next Sync needs only the durable job. */
        scenario = ALREADY_COMPLETE;
        unsigned previous_uploads = uploads, previous_process = process_requests;
        if (pending[i] == SERVER_BUSY) {
            unsigned previous_status = status_requests;
            assert(cloud_sync_start() == ESP_OK && status_requests == previous_status);
            assert(receipt().state == PROCESS_PENDING);
            now += 60000000LL;
        }
        assert(cloud_sync_start() == ESP_OK);
        assert(receipt().state == PROCESS_COMPLETED);
        assert(uploads == previous_uploads && process_requests == previous_process);
        assert(!cloud_sync_status().processing_pending);
    }
    reset(SERVER_TOO_LONG, false);
    assert(cloud_sync_start() == ESP_OK);
    assert(receipt().state == PROCESS_TOO_LONG && cloud_sync_status().processing_skipped == 1);
    assert(!strcmp(cloud_sync_summary(cloud_sync_status()), "SYNC DONE >30MIN SKIPPED"));
    reset(SUCCESS, false);
    source_size = 16000u * 2 * 1800 + 46; create_wave();
    assert(cloud_sync_start() == ESP_OK);
    assert(uploads == 1 && !warm_requests && !process_requests && receipt().state == PROCESS_TOO_LONG);
    reset(SUCCESS, true);
    processing_job_t foreign = {.source_size = source_size};
    strcpy(foreign.name, current_name); strcpy(foreign.drive_id, "another-drive");
    strcpy(foreign.item_id, "private-item"); strcpy(foreign.source_sha1, sha1);
    assert(processing_outbox_save(&foreign) == ESP_OK);
    assert(cloud_sync_start() == ESP_OK);
    assert(!warm_requests && !status_requests && !process_requests && !uploads);
    assert(cloud_sync_status().error != ESP_OK);
    reset(SUCCESS, false);
    assert(!unlink(RECORDING_DIR "/rec-test.wav"));
    for (unsigned i = 0; i < 3; ++i) {
        processing_job_t job = {.source_size = source_size};
        snprintf(job.name, sizeof(job.name), "rec-pending-%u.wav", i);
        snprintf(job.item_id, sizeof(job.item_id), "remote-item-%u", i);
        strcpy(job.drive_id, "drive-A"); strcpy(job.source_sha1, sha1);
        assert(processing_outbox_save(&job) == ESP_OK);
    }
    scenario = PROCESS_TIMEOUT_CASE;
    assert(cloud_sync_start() == ESP_OK);
    assert(!uploads && process_requests == 3 && cloud_sync_status().processing_pending == 3);
    scenario = SUCCESS;
    assert(cloud_sync_start() == ESP_OK);
    assert(!uploads && process_requests == 6 && cloud_sync_status().processing_completed == 3);
    for (unsigned i = 0; i < 3; ++i) {
        char name[65], id[129]; processing_job_t job;
        snprintf(name, sizeof(name), "rec-pending-%u.wav", i);
        snprintf(id, sizeof(id), "remote-item-%u", i);
        assert(processing_outbox_load(name, &job) == ESP_OK);
        assert(job.state == PROCESS_COMPLETED && !strcmp(job.item_id, id));
    }
    reset(SUCCESS, false);
    assert(!unlink(RECORDING_DIR "/rec-test.wav"));
    current_name = "AudioRecording_20260907_220536.wav"; expect_timestamp = true;
    recording_file_t recording;
    assert(storage_begin(&recording) == ESP_OK);
    uint8_t pcm[32000] = {0};
    assert(storage_append(&recording, pcm, sizeof(pcm)) == ESP_OK);
    assert(storage_finish(&recording) == ESP_OK);
    assert(cloud_sync_start() == ESP_OK && process_requests == 1 && receipt().state == PROCESS_COMPLETED);
    clear_files(); _rmdir(RECORDING_DIR);
    puts("PASS: actual cloud sync, all upload success paths, API-B processing, reboot outbox, UI, deadlines/consent");
}
