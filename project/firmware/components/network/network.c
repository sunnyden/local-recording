#include "recorder_network.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include <stdatomic.h>
#include <time.h>

static _Atomic bool connected;
static bool initialized, sntp_started;
static unsigned retries;
static bool wifi_has_config(void)
{
    wifi_config_t config;
    bool configured = esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK &&
                      config.sta.ssid[0] != 0;
    secret_zero(&config, sizeof(config));
    return configured;
}
static void connect_configured_wifi(void)
{
    if (!wifi_has_config()) return;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
        ESP_LOGE("network", "Wi-Fi connection request failed: %s", esp_err_to_name(err));
}
static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        retries = 0;
        connect_configured_wifi();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        atomic_store(&connected, true);
        retries = 0;
        if (!sntp_started) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.cloudflare.com");
            esp_err_t err = esp_netif_sntp_init(&cfg);
            if (err == ESP_OK) sntp_started = true;
            else ESP_LOGE("network", "Time synchronization startup failed: %s", esp_err_to_name(err));
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&connected, false);
        /* Provisioning temporarily clears the RAM configuration intentionally. */
        if (wifi_has_config()) {
            if (++retries <= 5) connect_configured_wifi();
            else ESP_LOGW("network", "Wi-Fi reconnect attempts exhausted");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        atomic_store(&connected, false);
    }
}
esp_err_t recorder_network_init(void)
{
    if (initialized) return ESP_OK;
    esp_err_t err = credential_storage_init();
    if (err != ESP_OK) return err;
    if ((err = esp_netif_init()) != ESP_OK) return err;
    if ((err = esp_event_loop_create_default()) != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if (!esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&cfg)) != ESP_OK) return err;
    if ((err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL)) != ESP_OK) return err;
    if ((err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL)) != ESP_OK) return err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    if ((err = esp_wifi_start()) != ESP_OK) return err;
    initialized = true;
    return ESP_OK;
}
bool recorder_network_ready(void) { return atomic_load(&connected); }
bool recorder_time_valid(void) { return time(NULL) > 1735689600; }
