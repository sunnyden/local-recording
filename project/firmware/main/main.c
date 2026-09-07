#include "board.h"
#include "audio_io.h"
#include "recorder.h"
#include "recorder_network.h"
#include "recorder_provisioning.h"
#include "identity.h"
#include "cloud_sync.h"
#include "voice_client.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *menu[] = {"RECORD", "RECORDINGS", "SYNC", "AI CONVERSATION", "SETUP"};
static void show(unsigned row, const char *text, bool selected)
{
    esp_err_t err = board_display_line(row, text, selected);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        ESP_LOGE("ui", "Display failed: %s", esp_err_to_name(err));
}
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("recorder", ESP_LOG_INFO);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI("recorder", "Flash=%lu PSRAM=%u internal=%u DMA largest=%u",
        (unsigned long)flash_size, heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    esp_err_t err = board_init();
    if (err != ESP_OK) { ESP_LOGE("recorder", "Board init: %s", esp_err_to_name(err)); return; }
    esp_err_t sd = board_sd_mount();
    esp_err_t display = board_display_init();
    if (display != ESP_OK) ESP_LOGW("recorder", "LCD disabled/error: %s; check P5 safety gate",
                                   esp_err_to_name(display));
    unsigned repaired = 0, failed = 0;
    if (sd == ESP_OK) storage_recover(&repaired, &failed);
    esp_err_t audio = local_audio_init();
    esp_err_t network = recorder_network_init();
    if (network == ESP_OK) identity_init();
    ESP_LOGI("recorder", "SD=%s audio=%s recovered=%u failed=%u", esp_err_to_name(sd),
        esp_err_to_name(audio), repaired, failed);
    show(0, "EMBEDDED RECORDER", false);
    char status[64] = "";
    snprintf(status, sizeof(status), "SD %s FIX %u BAD %u", sd == ESP_OK ? "OK" : "ERROR", repaired, failed);
    show(12, status, false);
    int selected = 0;
    bool catalog = false, redraw = true;
    size_t index = 0, count = 0;
    char filename[65] = "";
    uint32_t last_seconds = UINT32_MAX;
    local_mode_t previous = LOCAL_IDLE;
    bool previous_sync = false;
    bool previous_voice = false;
    const char *last_network_status = NULL;
    for (;;) {
        bool was_setup = recorder_setup_active();
        recorder_setup_tick();
        if (was_setup && !recorder_setup_active()) redraw = true;
        local_status_t state = local_status();
        sync_status_t sync = cloud_sync_status();
        voice_status_t voice = voice_client_status();
        board_key_t key;
        err = board_key_read(&key);
        if (err != ESP_OK) { key = KEY_NONE; show(13, "KEY I2C ERROR", false); }
        if (voice.active) {
            if (key == KEY_BACK) voice_client_stop();
            static const char *last_voice_state;
            if (voice.state != last_voice_state) {
                show(10, voice.state, false);
                last_voice_state = voice.state;
            }
        } else if (sync.active) {
            if (key == KEY_BACK) cloud_sync_cancel();
            cloud_sync_format_status(sync, status, sizeof(status));
            static char previous_status[64];
            if (!previous_sync || strcmp(previous_status, status)) {
                show(10, status, false);
                strcpy(previous_status, status);
            }
            char detail[64] = "";
            if (sync.processing_pending)
                snprintf(detail, sizeof(detail), "PENDING %s", cloud_sync_result_name(sync.processing_result));
            else if (sync.processing_missing)
                strcpy(detail, "DELETED REMOTE SKIPPED");
            static char previous_detail[64];
            if (!previous_sync || strcmp(previous_detail, detail)) {
                show(11, detail, false);
                strcpy(previous_detail, detail);
            }
        } else if (recorder_setup_active()) {
            if (key == KEY_BACK) recorder_setup_stop();
        } else if (state.mode != LOCAL_IDLE) {
            if (key == KEY_BACK || key == KEY_ENTER) local_stop();
            uint32_t sec = state.samples / PCM_RATE;
            if (sec != last_seconds || state.mode != previous) {
                snprintf(status, sizeof(status), "%s %lu:%02lu",
                    state.mode == LOCAL_RECORD ? "RECORDING" : "PLAYING",
                    (unsigned long)(sec / 60), (unsigned long)(sec % 60));
                show(10, status, false);
                last_seconds = sec;
            }
        } else {
            if (previous != LOCAL_IDLE) {
                snprintf(status, sizeof(status), "%s", state.error == ESP_OK ? "SAVED / FINISHED" : esp_err_to_name(state.error));
                show(10, status, false);
                redraw = true;
            }
            if (key == KEY_BACK) { catalog = false; redraw = true; }
            if (key == KEY_UP || key == KEY_DOWN) {
                if (catalog && count) index = (index + count + (key == KEY_UP ? -1 : 1)) % count;
                else selected = (selected + 5 + (key == KEY_UP ? -1 : 1)) % 5;
                redraw = true;
            }
            if (key == KEY_ENTER) {
                err = ESP_OK;
                if (catalog) err = filename[0] ? local_play_start(filename) : ESP_ERR_NOT_FOUND;
                else if (selected == 0) err = sd == ESP_OK && audio == ESP_OK ? local_record_start() : ESP_ERR_INVALID_STATE;
                else if (selected == 1) { catalog = true; index = 0; redraw = true; }
                else if (selected == 2) err = sd == ESP_OK ? cloud_sync_start() : ESP_ERR_INVALID_STATE;
                else if (selected == 3) err = audio == ESP_OK ? voice_client_start() : ESP_ERR_INVALID_STATE;
                else if (selected == 4) err = recorder_setup_start();
                else { show(10, "NETWORK NOT CONFIGURED", false); }
                if (err != ESP_OK) show(10, esp_err_to_name(err), false);
            }
        }
        if (redraw) {
            for (unsigned i = 0; i < 5; ++i) show(i + 2, catalog ? "" : menu[i], !catalog && selected == (int)i);
            if (catalog) {
                err = storage_catalog(index, filename, sizeof(filename), &count);
                show(2, "RECORDINGS", false);
                show(4, err == ESP_OK && count ? filename : "NO RECORDINGS / SD ERROR", true);
                snprintf(status, sizeof(status), "%u OF %u", (unsigned)(count ? index + 1 : 0), (unsigned)count);
                show(6, status, false);
            }
            show(14, "3 UP 1 DOWN 0 OK 2 BACK", false);
            redraw = false;
        }
        previous = state.mode;
        if (previous_sync && !sync.active) {
            show(10, cloud_sync_summary(sync), false);
            show(11, sync.processing_pending ? cloud_sync_result_name(sync.processing_result) : "", false);
        }
        previous_sync = sync.active;
        if (previous_voice && !voice.active) show(10, voice.error == ESP_OK ? "VOICE STOPPED" : esp_err_to_name(voice.error), false);
        previous_voice = voice.active;
        if (!recorder_setup_active()) {
            const char *net_status = !credential_storage_allowed() ? "WIFI SETUP DISABLED" :
                !recorder_network_ready() ? "WIFI CONNECTING / OFFLINE" :
                !recorder_time_valid() ? "WIFI OK TIME WAIT" : "WIFI OK TIME OK";
            if (net_status != last_network_status) {
                show(13, net_status, false);
                last_network_status = net_status;
            }
        } else {
            last_network_status = NULL;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
