#include "recorder_network.h"
#include "nvs_flash.h"
#include "nvs_sec_provider.h"
#include "nvs.h"
#include "esp_efuse.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *scenario;
static unsigned registrations, key_reads, hmac_calls, secure_inits, plain_inits, deregistrations, nvs_opens;
static unsigned forbidden_generations;
static nvs_sec_scheme_t default_scheme;
static char stored[64];

static bool is(const char *value) { return !strcmp(scenario, value); }
static void check_block(esp_efuse_block_t block) { assert(block == EFUSE_BLK_KEY0 + 2); }
esp_efuse_purpose_t esp_efuse_get_key_purpose(esp_efuse_block_t block)
{
    check_block(block);
    return is("missing") || is("wrong-purpose") ? 0 : ESP_EFUSE_KEY_PURPOSE_HMAC_UP;
}
bool esp_efuse_get_key_dis_read(esp_efuse_block_t block)
{
    check_block(block); return !is("readable-key");
}
bool esp_efuse_get_key_dis_write(esp_efuse_block_t block)
{
    check_block(block); return !is("writable-key");
}
bool esp_efuse_get_keypurpose_dis_write(esp_efuse_block_t block)
{
    check_block(block); return !is("writable-purpose");
}
esp_err_t esp_hmac_calculate(hmac_key_id_t key, const void *input, size_t length, uint8_t *output)
{
    assert(key == 2 && length == 32 && !default_scheme.nvs_flash_key_gen);
    const uint8_t *bytes = input;
    const uint8_t expected[2][4] = {{0x5a,0x5a,0xbe,0xae},{0xa5,0xa5,0xde,0xce}};
    assert(hmac_calls < 2);
    for (unsigned i = 0; i < length; ++i) assert(bytes[i] == expected[hmac_calls][i % 4]);
    ++hmac_calls;
    if (is("derive-fails")) return ESP_FAIL;
    if (hmac_calls == 2 && is("derive-second-fails")) return ESP_FAIL;
    memset(output, hmac_calls == 1 || is("equal-derived-keys") ? 0xa5 : 0x5a, 32);
    return ESP_OK;
}
esp_err_t nvs_flash_register_security_scheme(nvs_sec_scheme_t *scheme)
{
    ++registrations;
    assert(!scheme->nvs_flash_key_gen && scheme->scheme_id == NVS_SEC_SCHEME_HMAC);
    assert(((nvs_sec_config_hmac_t *)scheme->scheme_data)->hmac_key_id == 2);
    if (is("readonly-registration-fails")) return ESP_FAIL;
    default_scheme = *scheme;
    return ESP_OK;
}
esp_err_t nvs_sec_provider_register_hmac(const nvs_sec_config_hmac_t *config, nvs_sec_scheme_t **handle)
{
    (void)config; (void)handle; ++forbidden_generations;
    assert(!"Write-capable SDK provider must not be installed");
    return ESP_FAIL;
}
void nvs_flash_deregister_security_scheme(void)
{
    ++deregistrations;
    memset(&default_scheme, 0, sizeof(default_scheme));
}
esp_err_t nvs_flash_read_security_cfg_v2(nvs_sec_scheme_t *scheme, nvs_sec_cfg_t *keys)
{
    assert(!scheme->nvs_flash_key_gen && !default_scheme.nvs_flash_key_gen);
    ++key_reads;
    return scheme->nvs_flash_read_cfg(scheme->scheme_data, keys);
}
esp_err_t nvs_flash_secure_init(nvs_sec_cfg_t *keys)
{
    assert(!default_scheme.nvs_flash_key_gen);
    for (unsigned i = 0; i < sizeof(keys->eky); ++i) {
        assert(keys->eky[i] == 0xa5 && keys->tky[i] == 0x5a);
    }
    ++secure_inits;
    return is("secure-init-fails") ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_flash_init_partition(const char *name)
{
    assert(!strcmp(name, "nvs"));
    ++plain_inits;
    return is("plain-init-fails") ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_flash_init(void)
{
    assert(!"Generic nvs_flash_init may auto-generate keys and must never run");
    return ESP_FAIL;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(credential_storage_allowed() && !strcmp(name, "recorder_auth"));
    assert(mode == NVS_READONLY || mode == NVS_READWRITE);
    ++nvs_opens; *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *size)
{
    assert(handle == 1 && !strcmp(key, "graph_refresh"));
    size_t needed = strlen(stored) + 1;
    if (value) { assert(*size >= needed); memcpy(value, stored, needed); }
    *size = needed; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    assert(handle == 1 && !strcmp(key, "graph_refresh") && strlen(value) < sizeof(stored));
    strcpy(stored, value); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { assert(handle == 1); return ESP_OK; }
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }
esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    assert(handle == 1); memset(stored, 0, sizeof(stored)); return ESP_OK;
}
int main(int argc, char **argv)
{
    assert(argc == 2); scenario = argv[1];
    char value[64];
    assert(!credential_storage_allowed());
    assert(credential_read("graph_refresh", value, sizeof(value)) == ESP_ERR_NOT_ALLOWED);
    assert(credential_write("graph_refresh", "SYNTHETIC_REFRESH") == ESP_ERR_NOT_ALLOWED);
    assert(credential_erase_all() == ESP_ERR_NOT_ALLOWED && nvs_opens == 0);
    esp_err_t result = credential_storage_init();
    if (is("hmac-good") || is("plain-good")) {
        assert(result == ESP_OK && credential_storage_allowed());
        assert(credential_write("graph_refresh", "SYNTHETIC_REFRESH") == ESP_OK);
        assert(credential_read("graph_refresh", value, sizeof(value)) == ESP_OK);
        assert(!strcmp(value, "SYNTHETIC_REFRESH"));
        char short_buffer[2] = {'x','x'};
        assert(credential_read("graph_refresh", short_buffer, sizeof(short_buffer)) == ESP_ERR_INVALID_SIZE);
        assert(short_buffer[0] == 0);
        assert(credential_erase_all() == ESP_OK && !stored[0]);
        assert(credential_storage_init() == ESP_OK);
        if (is("hmac-good")) {
            assert(registrations == 1 && key_reads == 1 && hmac_calls == 2 && secure_inits == 1 && !plain_inits);
            assert(!default_scheme.nvs_flash_key_gen);
        } else assert(plain_inits == 1 && !registrations && !secure_inits);
    } else {
        assert(result != ESP_OK && !credential_storage_allowed());
        assert(credential_write("graph_refresh", "SYNTHETIC_REFRESH") == ESP_ERR_NOT_ALLOWED);
        assert(credential_erase_all() == ESP_ERR_NOT_ALLOWED && nvs_opens == 0);
        if (is("missing") || is("wrong-purpose") || is("readable-key") ||
            is("writable-key") || is("writable-purpose") || is("disabled"))
            assert(!registrations && !key_reads && !secure_inits && !plain_inits);
        if (is("derive-fails") || is("derive-second-fails") || is("equal-derived-keys") ||
            is("secure-init-fails") || is("readonly-registration-fails"))
            assert(deregistrations == 1 && !default_scheme.nvs_flash_key_gen && !plain_inits);
    }
    assert(!forbidden_generations);
    printf("PASS: credential storage boundary %s; no key generation or generic init\n", scenario);
    return 0;
}
