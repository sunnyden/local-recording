#include "recorder_network.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdatomic.h>

#if defined(CONFIG_RECORDER_NVS_HMAC_EXISTING)
#if !defined(CONFIG_NVS_ENCRYPTION) || !defined(CONFIG_NVS_SEC_KEY_PROTECT_NONE)
#error "Existing-key profile requires encrypted NVS with SDK auto-provider disabled"
#endif
#if defined(CONFIG_SECURE_FLASH_ENC_ENABLED) || defined(CONFIG_SECURE_BOOT)
#error "Existing-key profile must not enable SDK boot-time fuse provisioning"
#endif
#include "nvs_sec_provider.h"
#include "esp_efuse.h"
#include "esp_efuse_chip.h"
#include <string.h>
_Static_assert(CONFIG_RECORDER_NVS_HMAC_KEY_ID >= 0 &&
               CONFIG_RECORDER_NVS_HMAC_KEY_ID <= 5, "HMAC key ID out of range");
#elif defined(CONFIG_RECORDER_DEVELOPMENT_CREDENTIAL_RISK) && defined(CONFIG_NVS_ENCRYPTION)
#error "Development plaintext risk profile cannot be combined with NVS encryption"
#endif

static _Atomic bool ready;

bool credential_storage_allowed(void) { return atomic_load(&ready); }

#ifdef CONFIG_RECORDER_NVS_HMAC_EXISTING
static esp_err_t existing_key_check(void)
{
    const esp_efuse_block_t block = (esp_efuse_block_t)(EFUSE_BLK_KEY0 + CONFIG_RECORDER_NVS_HMAC_KEY_ID);
    if (esp_efuse_get_key_purpose(block) != ESP_EFUSE_KEY_PURPOSE_HMAC_UP ||
        !esp_efuse_get_key_dis_read(block) || !esp_efuse_get_key_dis_write(block) ||
        !esp_efuse_get_keypurpose_dis_write(block)) {
        ESP_LOGE("credentials", "Existing HMAC key purpose/protection prerequisites not satisfied");
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}
static esp_err_t read_existing_keys(const void *context, nvs_sec_cfg_t *keys)
{
    if (!context || !keys) return ESP_ERR_INVALID_ARG;
    esp_err_t err = existing_key_check();
    if (err != ESP_OK) return err;
    const nvs_sec_config_hmac_t *config = context;
    /* IDF v6.1 nvs_sec_provider EKEY_SEED/TKEY_SEED, little-endian and
       repeated eight times. These are public derivation inputs, not keys. */
    const uint8_t domains[2][4] = {{0x5a, 0x5a, 0xbe, 0xae}, {0xa5, 0xa5, 0xde, 0xce}};
    uint8_t seed[32];
    for (unsigned domain = 0; domain < 2 && err == ESP_OK; ++domain) {
        for (unsigned offset = 0; offset < sizeof(seed); offset += 4)
            memcpy(seed + offset, domains[domain], 4);
        err = esp_hmac_calculate(config->hmac_key_id, seed, sizeof(seed),
                                 domain ? keys->tky : keys->eky);
    }
    if (err == ESP_OK && !memcmp(keys->eky, keys->tky, sizeof(keys->eky)))
        err = ESP_ERR_INVALID_RESPONSE;
    if (err != ESP_OK) secret_zero(keys, sizeof(*keys));
    return err;
}
static esp_err_t existing_hmac_init(void)
{
    esp_err_t err = existing_key_check();
    if (err != ESP_OK) return err;
    static nvs_sec_config_hmac_t config = {.hmac_key_id = (hmac_key_id_t)CONFIG_RECORDER_NVS_HMAC_KEY_ID};
    static nvs_sec_scheme_t read_only_scheme = {.scheme_id = NVS_SEC_SCHEME_HMAC,
        .scheme_data = &config, .nvs_flash_key_gen = NULL, .nvs_flash_read_cfg = read_existing_keys};

    /* With NVS_SEC_KEY_PROTECT_NONE the SDK's write-capable providers are
       not linked. Install only our read/derive callback; no generation
       callback is ever exposed, even to a future generic NVS init caller. */
    err = nvs_flash_register_security_scheme(&read_only_scheme);
    nvs_sec_cfg_t keys = {0};
    if (err == ESP_OK) err = nvs_flash_read_security_cfg_v2(&read_only_scheme, &keys);
    if (err == ESP_OK) err = nvs_flash_secure_init(&keys);
    secret_zero(&keys, sizeof(keys));
    if (err != ESP_OK) {
        nvs_flash_deregister_security_scheme();
        ESP_LOGE("credentials", "Existing-key encrypted NVS initialization failed: %s", esp_err_to_name(err));
    }
    return err;
}
#endif

esp_err_t credential_storage_init(void)
{
    if (atomic_load(&ready)) return ESP_OK;
    esp_err_t err;
#ifdef CONFIG_RECORDER_NVS_HMAC_EXISTING
    err = existing_hmac_init();
#elif defined(CONFIG_RECORDER_DEVELOPMENT_CREDENTIAL_RISK)
    /* Explicit plaintext profile; bypass nvs_flash_init's encryption fallback. */
    err = nvs_flash_init_partition("nvs");
#else
    err = ESP_ERR_NOT_ALLOWED;
#endif
    if (err == ESP_OK) atomic_store(&ready, true);
    return err;
}
