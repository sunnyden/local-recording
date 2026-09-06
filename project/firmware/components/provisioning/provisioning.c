#include "recorder_provisioning.h"
#include "board.h"
#include "identity.h"
#include "recorder_network.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "esp_srp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic bool active, ended;
static int64_t deadline;
static char *salt, *verifier;
static network_prov_security2_params_t security;
static bool manager_initialized;
static uint8_t service_uuid[16] = {
    0x07, 0xed, 0x9b, 0x2d, 0x0f, 0x06, 0x7c, 0x87,
    0x9b, 0x43, 0x43, 0x6b, 0x4d, 0x24, 0x75, 0x17
};
static void event(void *context, network_prov_cb_event_t id, void *data)
{
    (void)context; (void)data;
    if (id == NETWORK_PROV_END) atomic_store(&ended, true);
}
bool recorder_setup_active(void) { return atomic_load(&active); }
void recorder_setup_stop(void)
{
    if (!atomic_load(&active)) return;
    identity_cancel(AUTH_GRAPH);
    identity_cancel(AUTH_PROXY);
    network_prov_mgr_stop_provisioning();
}
void recorder_setup_tick(void)
{
    if (atomic_load(&active) && esp_timer_get_time() >= deadline) recorder_setup_stop();
    if (!atomic_exchange(&ended, false)) return;
    network_prov_mgr_deinit();
    manager_initialized = false;
    if (salt) { secret_zero(salt, 16); free(salt); salt = NULL; }
    if (verifier) { secret_zero(verifier, security.verifier_len); free(verifier); verifier = NULL; }
    atomic_store(&active, false);
    for (unsigned i = 8; i <= 11; ++i) board_display_line(i, "", false);
}
esp_err_t recorder_setup_start(void)
{
    if (active || manager_initialized) return ESP_ERR_INVALID_STATE;
    if (!credential_storage_allowed()) return ESP_ERR_NOT_ALLOWED;
    /* Never expose setup credentials through UART logs or advertise a password. */
    if (!board_display_available()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = recorder_network_init();
    if (err != ESP_OK) return err;
    if ((err = identity_init()) != ESP_OK) return err;
    if ((err = network_prov_scheme_ble_set_service_uuid(service_uuid)) != ESP_OK) return err;
    network_prov_mgr_config_t cfg = {.scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = {.event_cb = event}, .network_prov_wifi_conn_cfg = {.wifi_conn_attempts = 5}};
    if ((err = network_prov_mgr_init(cfg)) != ESP_OK) return err;
    manager_initialized = true;
    char username[17], password[25], name[25];
    snprintf(username, sizeof(username), "USER%08lX", (unsigned long)esp_random());
    snprintf(password, sizeof(password), "%08lX%08lX%08lX",
        (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
    snprintf(name, sizeof(name), "RECORDER_%08lX", (unsigned long)esp_random());
    int verifier_len = 0;
    err = esp_srp_gen_salt_verifier(username, strlen(username), password, strlen(password),
                                   &salt, 16, &verifier, &verifier_len);
    if (err == ESP_OK) {
        security = (network_prov_security2_params_t){.salt = salt, .salt_len = 16,
            .verifier = verifier, .verifier_len = verifier_len};
        err = network_prov_mgr_disable_auto_stop(1500);
    }
    if (err == ESP_OK) err = network_prov_mgr_endpoint_create("recorder-control");
    if (err == ESP_OK) err = network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_2, &security, name, NULL);
    if (err == ESP_OK) err = network_prov_mgr_endpoint_register("recorder-control", recorder_control, NULL);
    if (err == ESP_OK) {
        board_display_line(8, name, false);
        board_display_line(9, username, false);
        board_display_line(10, password, false);
        board_display_line(11, "SETUP 10 MIN - BACK EXIT", false);
        deadline = esp_timer_get_time() + 600LL * 1000000;
        atomic_store(&active, true);
    } else {
        network_prov_mgr_deinit();
        manager_initialized = false;
        free(salt); salt = NULL; free(verifier); verifier = NULL;
    }
    secret_zero(password, sizeof(password));
    return err;
}
