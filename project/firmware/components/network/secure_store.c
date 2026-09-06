#include "recorder_network.h"
#include "nvs.h"
#include <string.h>

void secret_zero(void *memory, size_t length)
{
    volatile unsigned char *p = memory;
    while (length--) *p++ = 0;
}
esp_err_t credential_read(const char *key, char *value, size_t capacity)
{
    if (!credential_storage_allowed()) return ESP_ERR_NOT_ALLOWED;
    if (!key || !value || !capacity) return ESP_ERR_INVALID_ARG;
    value[0] = 0;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("recorder_auth", NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    size_t needed = 0;
    err = nvs_get_str(nvs, key, NULL, &needed);
    if (err == ESP_OK && needed > capacity) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) err = nvs_get_str(nvs, key, value, &capacity);
    nvs_close(nvs);
    return err;
}
esp_err_t credential_write(const char *key, const char *value)
{
    if (!credential_storage_allowed()) return ESP_ERR_NOT_ALLOWED;
    if (!key || !value || strlen(value) > 8192) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("recorder_auth", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, key, value);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
esp_err_t credential_erase_all(void)
{
    if (!credential_storage_allowed()) return ESP_ERR_NOT_ALLOWED;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("recorder_auth", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
