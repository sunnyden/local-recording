#include "recorder_network.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

const char test_wifi_events[] = "wifi";
const char test_ip_events[] = "ip";
static bool allowed;
static unsigned nvs_calls, connect_calls, sntp_calls;
static esp_event_handler_t callback;
static wifi_config_t configuration;

bool credential_storage_allowed(void) { return allowed; }
void secret_zero(void *data, size_t size) { memset(data, 0, size); }
esp_err_t credential_storage_init(void)
{
    if (!allowed) return ESP_ERR_NOT_ALLOWED;
    ++nvs_calls;
    return ESP_OK;
}
esp_err_t esp_netif_init(void) { return ESP_OK; }
void *esp_netif_create_default_wifi_sta(void) { return &configuration; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_err_t esp_wifi_init(const wifi_init_config_t *config) { assert(config); return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t mode) { assert(mode == WIFI_MODE_STA); return ESP_OK; }
esp_err_t esp_wifi_get_config(wifi_interface_t interface, wifi_config_t *out)
{
    assert(interface == WIFI_IF_STA);
    *out = configuration;
    return ESP_OK;
}
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id,
                                    esp_event_handler_t handler, void *context)
{
    assert(base == WIFI_EVENT || base == IP_EVENT);
    assert(id == ESP_EVENT_ANY_ID || id == IP_EVENT_STA_GOT_IP);
    assert(context == NULL);
    if (callback) assert(callback == handler);
    callback = handler;
    return ESP_OK;
}
esp_err_t esp_wifi_start(void)
{
    assert(callback);
    callback(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    return ESP_OK;
}
esp_err_t esp_wifi_connect(void) { ++connect_calls; return ESP_OK; }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    assert(config->server);
    ++sntp_calls;
    return ESP_OK;
}

int main(void)
{
    assert(recorder_network_init() == ESP_ERR_NOT_ALLOWED && nvs_calls == 0);
    allowed = true;
    memcpy(configuration.sta.ssid, "TEST_SSID", 10);
    assert(recorder_network_init() == ESP_OK && nvs_calls == 1 && connect_calls == 1);
    assert(!recorder_network_ready());
    callback(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(recorder_network_ready() && sntp_calls == 1);
    callback(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(sntp_calls == 1);
    callback(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(!recorder_network_ready() && connect_calls == 2);
    for (unsigned i = 0; i < 8; ++i)
        callback(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(connect_calls == 6);
    callback(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    memset(&configuration, 0, sizeof(configuration));
    callback(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(connect_calls == 6);
    memcpy(configuration.sta.ssid, "TEST_SSID", 10);
    callback(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    assert(connect_calls == 7);
    callback(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    callback(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    assert(!recorder_network_ready());
    assert(recorder_network_init() == ESP_OK && nvs_calls == 1);
    puts("PASS: actual Wi-Fi startup, persisted configuration, retry limits and stop state");
    return 0;
}
