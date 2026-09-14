#include <assert.h>
#include <stdlib.h>
#include <string.h>
static void checked_free(void *pointer);
#define free checked_free
#include "../components/provisioning/provisioning.c"
#undef free

enum { NONE, NETWORK, IDENTITY, UUID, INIT, SRP, AUTO_STOP, CREATE, SHOW, ADVERTISE, REGISTER };
static int failure;
static int64_t clock_us;
static unsigned shows, clears, advertisements, deinitializations, stops, cancellations;
static unsigned password_wipes, username_wipes, salt_wipes, verifier_wipes, lcd_writes;
static bool storage_allowed = true, lcd_available = true, sdk_callback, drawn, advertised;
static char expected_user[17], expected_password[25], expected_service[25];
static char *allocated_salt, *allocated_verifier;
static void (*manager_event)(void *, network_prov_cb_event_t, void *);
const void *network_prov_scheme_ble;

static esp_err_t result(int stage) { return failure == stage ? ESP_FAIL : ESP_OK; }
bool credential_storage_allowed(void) { return storage_allowed; }
int64_t esp_timer_get_time(void) { return clock_us; }
uint32_t esp_random(void) { return 0xa1b2c3d4; }
esp_err_t recorder_network_init(void) { return result(NETWORK); }
esp_err_t identity_init(void) { return result(IDENTITY); }
void identity_cancel(auth_resource_t resource)
{
    assert(resource == AUTH_GRAPH || resource == AUTH_PROXY);
    ++cancellations;
}
void secret_zero(void *data, size_t bytes)
{
    if (data == allocated_salt) { assert(bytes == 16); ++salt_wipes; }
    else if (data == allocated_verifier) { assert(bytes == 32); ++verifier_wipes; }
    else if (bytes == 25) ++password_wipes;
    else if (bytes == 17) ++username_wipes;
    memset(data, 0, bytes);
}
static void checked_free(void *pointer)
{
    if (!pointer) return;
    size_t bytes = pointer == allocated_salt ? 16 : 32;
    assert(pointer == allocated_salt || pointer == allocated_verifier);
    for (size_t i = 0; i < bytes; ++i) assert(((uint8_t *)pointer)[i] == 0);
    if (pointer == allocated_salt) allocated_salt = NULL;
    else allocated_verifier = NULL;
    free(pointer);
}
esp_err_t esp_srp_gen_salt_verifier(const char *user, int user_len, const char *password,
                                   int password_len, char **out_salt, int salt_len,
                                   char **out_verifier, int *verifier_len)
{
    assert(user_len == 12 && password_len == 24 && salt_len == 16);
    memcpy(expected_user, user, (size_t)user_len + 1);
    memcpy(expected_password, password, (size_t)password_len + 1);
    assert(!strcmp(user, "USERA1B2C3D4"));
    assert(!strcmp(password, "A1B2C3D4A1B2C3D4A1B2C3D4"));
    allocated_salt = *out_salt = malloc(16);
    allocated_verifier = *out_verifier = malloc(32);
    assert(allocated_salt && allocated_verifier);
    memset(allocated_salt, 0x55, 16);
    memset(allocated_verifier, 0xaa, 32);
    *verifier_len = 32;
    return result(SRP);
}
esp_err_t network_prov_scheme_ble_set_service_uuid(uint8_t *uuid)
{
    assert(uuid);
    return result(UUID);
}
esp_err_t network_prov_mgr_init(network_prov_mgr_config_t config)
{
    assert(config.network_prov_wifi_conn_cfg.wifi_conn_attempts == 5);
    manager_event = config.app_event_handler.event_cb;
    return result(INIT);
}
void network_prov_mgr_deinit(void)
{
    assert(!sdk_callback);
    advertised = false;
    ++deinitializations;
}
static void signal_end(void)
{
    unsigned before = clears;
    sdk_callback = true;
    manager_event(NULL, NETWORK_PROV_END, NULL);
    sdk_callback = false;
    assert(clears == before);
}
void network_prov_mgr_stop_provisioning(void)
{
    assert(!drawn);
    ++stops;
    signal_end();
}
esp_err_t network_prov_mgr_disable_auto_stop(unsigned delay)
{
    assert(delay == 1500);
    return result(AUTO_STOP);
}
esp_err_t network_prov_mgr_endpoint_create(const char *name)
{
    assert(!strcmp(name, "recorder-control"));
    return result(CREATE);
}
esp_err_t network_prov_mgr_start_provisioning(int scheme, const void *params,
                                             const char *name, const char *key)
{
    assert(scheme == NETWORK_PROV_SECURITY_2 && params == &security && !key);
    assert(drawn && password_wipes == 1);
    assert(!strcmp(name, expected_service));
    ++advertisements;
    advertised = failure != ADVERTISE;
    return result(ADVERTISE);
}
esp_err_t network_prov_mgr_endpoint_register(const char *name,
    esp_err_t (*handler)(uint32_t, const uint8_t *, ssize_t, uint8_t **, ssize_t *, void *),
    void *context)
{
    assert(advertised && !strcmp(name, "recorder-control") && handler == recorder_control && !context);
    return result(REGISTER);
}
esp_err_t recorder_control(uint32_t session_id, const uint8_t *input, ssize_t input_len,
                           uint8_t **output, ssize_t *output_len, void *context)
{
    (void)session_id; (void)input; (void)input_len; (void)output; (void)output_len; (void)context;
    return ESP_OK;
}
static esp_err_t show(void *context, const char *service, const char *user,
                      const char *password, unsigned seconds)
{
    assert(context == &shows && !sdk_callback && !advertised && seconds == 600);
    assert(!strcmp(user, expected_user) && !strcmp(password, expected_password));
    snprintf(expected_service, sizeof(expected_service), "%s", service);
    assert(!strcmp(service, "RECORDER_A1B2C3D4"));
    assert(recorder_setup_set_display(NULL) == ESP_ERR_INVALID_STATE);
    drawn = true;
    ++shows;
    return result(SHOW);
}
static void clear(void *context)
{
    assert(context == &shows && !sdk_callback && drawn);
    ++clears;
    drawn = false;
}
bool board_display_available(void) { return lcd_available; }
esp_err_t board_display_line(unsigned row, const char *text, bool selected)
{
    assert(!sdk_callback && !selected && row >= 8 && row <= 11);
    ++lcd_writes;
    if (*text) {
        if (row == 8) snprintf(expected_service, sizeof(expected_service), "%s", text);
        if (row == 9) assert(!strcmp(text, expected_user));
        if (row == 10) assert(!strcmp(text, expected_password));
        if (row == 11) drawn = true;
        return result(SHOW);
    }
    if (row == 11) drawn = false;
    return ESP_OK;
}
static void reset(int fail)
{
    assert(!recorder_setup_active() && !manager_initialized && !allocated_salt && !allocated_verifier);
    failure = fail;
    shows = clears = advertisements = deinitializations = stops = cancellations = 0;
    password_wipes = username_wipes = salt_wipes = verifier_wipes = lcd_writes = 0;
    drawn = advertised = false;
    clock_us = 0;
    recorder_setup_display_t adapter = {.show = show, .clear = clear, .context = &shows};
    assert(recorder_setup_set_display(&adapter) == ESP_OK);
}
static void check_cleanup(void)
{
    assert(!recorder_setup_active() && !manager_initialized && !drawn && !advertised);
    assert(!allocated_salt && !allocated_verifier);
    assert(!security.salt && !security.verifier && !security.salt_len && !security.verifier_len);
}
int main(void)
{
    reset(NONE);
    recorder_setup_display_t incomplete = {.show = show};
    assert(recorder_setup_set_display(&incomplete) == ESP_ERR_INVALID_ARG);
    incomplete = (recorder_setup_display_t){.clear = clear};
    assert(recorder_setup_set_display(&incomplete) == ESP_ERR_INVALID_ARG);
    storage_allowed = false;
    assert(recorder_setup_start() == ESP_ERR_NOT_ALLOWED && !advertisements && !shows);
    storage_allowed = true;
    /* A registered GUI owns display availability; no legacy LCD writes on this path. */
    lcd_available = false;
    assert(recorder_setup_start() == ESP_OK && recorder_setup_active());
    assert(shows == 1 && advertisements == 1 && !lcd_writes);
    assert(password_wipes == 1 && username_wipes == 1);
    assert(recorder_setup_start() == ESP_ERR_INVALID_STATE);
    assert(recorder_setup_set_display(NULL) == ESP_ERR_INVALID_STATE);
    recorder_setup_stop();
    assert(clears == 1 && stops == 1 && cancellations == 2);
    assert(recorder_setup_set_display(NULL) == ESP_ERR_INVALID_STATE);
    recorder_setup_stop();
    assert(stops == 1);
    recorder_setup_tick();
    check_cleanup();
    assert(clears == 1 && salt_wipes == 1 && verifier_wipes == 1);
    reset(NONE);
    assert(recorder_setup_start() == ESP_OK);
    clock_us = 599999999;
    recorder_setup_tick();
    assert(recorder_setup_active() && !clears);
    clock_us = 600000000;
    recorder_setup_tick();
    check_cleanup();
    assert(clears == 1 && stops == 1);
    reset(NONE);
    assert(recorder_setup_start() == ESP_OK);
    signal_end();
    assert(!clears && recorder_setup_active());
    recorder_setup_tick();
    check_cleanup();
    assert(clears == 1);
    for (int stage = NETWORK; stage <= REGISTER; ++stage) {
        reset(stage);
        assert(recorder_setup_start() == ESP_FAIL);
        check_cleanup();
        assert(stage < ADVERTISE ? advertisements == 0 : advertisements == 1);
        assert(stage < SHOW ? !shows && !clears : shows == 1 && clears == 1);
        if (stage >= SRP) {
            assert(password_wipes == 1 && username_wipes == 1);
            assert(salt_wipes == 1 && verifier_wipes == 1);
        }
        assert(!lcd_writes);
        recorder_setup_tick();
        check_cleanup();
    }
    reset(NONE);
    assert(recorder_setup_set_display(NULL) == ESP_OK);
    assert(recorder_setup_start() == ESP_ERR_INVALID_STATE);
    assert(!advertisements);
    check_cleanup();
    reset(NONE);
    assert(recorder_setup_set_display(NULL) == ESP_OK);
    lcd_available = true;
    assert(recorder_setup_start() == ESP_OK && lcd_writes == 4);
    recorder_setup_stop();
    recorder_setup_tick();
    check_cleanup();
    assert(lcd_writes == 8);
    /* The fallback adapter must preserve full values and case, too. Synthetic values only. */
    strcpy(expected_user, "UserAbCd1234");
    strcpy(expected_password, "aBcDeFgHiJkLmNoPqRsTuVwX");
    assert(legacy_show(NULL, "TestService", expected_user, expected_password, 600) == ESP_OK);
    legacy_clear(NULL);
    memset(expected_password, 0, sizeof(expected_password));
    memset(expected_user, 0, sizeof(expected_user));
    puts("PASS: actual provisioning source with synthetic host BLE/display mocks; draw-before-advertise, all failures, expiry, callback context, full case and secret cleanup");
    return 0;
}
