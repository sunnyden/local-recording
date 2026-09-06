# Credential storage profiles

The default firmware initializes neither NVS nor networking for credentials.
There is no universal provisioning password, automatic key installation, or
implicit plaintext fallback. Real credential enrollment remains a separate
owner-approved activity.

## Profiles

| Profile | Conditions | Behavior |
| --- | --- | --- |
| Default | Both recorder credential options disabled | All reads/writes/unlink and network initialization fail with `ESP_ERR_NOT_ALLOWED` |
| Plaintext development | Explicit `CONFIG_RECORDER_DEVELOPMENT_CREDENTIAL_RISK=y`, NVS encryption disabled | Uses the normal `nvs` partition after successful initialization; accepts documented physical-access risk |
| Existing HMAC | `CONFIG_RECORDER_NVS_HMAC_EXISTING=y`, NVS encryption enabled, SDK key-protection scheme **None** | Uses an already-provisioned hardware HMAC key; initializes the default `nvs` partition with AES-XTS keys derived in RAM |

The encrypted profile covers **both Wi-Fi credentials and application refresh
tokens**, because it securely initializes the default `nvs` partition before
Wi-Fi or identity initialization. Credential operations remain blocked until
initialization succeeds. Errors never trigger NVS erase, generation, or downgrade.

### Required existing hardware state

Choose `CONFIG_RECORDER_NVS_HMAC_KEY_ID` from 0–5 for the already-provisioned
ESP32-S3 HMAC key slot. This profile requires:

* eFuse key purpose `ESP_EFUSE_KEY_PURPOSE_HMAC_UP`;
* key read protection;
* key write protection;
* key-purpose write protection;
* successful hardware HMAC derivation and secure NVS initialization.

Missing, differently purposed, readable, or writable keys fail closed. External
provisioning must have installed a suitably random root key. The firmware cannot
verify a protected key's entropy and never attempts to read its bytes.

**No key/fuse provisioning procedure is executed or supplied here.** Installing a
key, locking its purpose/protections, secure boot, debug/download restrictions,
flash encryption, and migration of an existing plaintext NVS image each require
separate explicit owner approval and a reviewed external process.

## Why not call `nvs_flash_init()`?

The pinned ESP-IDF v6.1 implementation (`fff9895c82d744c7237be8847347bdd1b07c6643`)
does the following when encryption is enabled:

1. `nvs_flash_init()` calls `nvs_flash_read_security_cfg_v2()`.
2. Any read failure falls back to `nvs_flash_generate_keys_v2()`.
3. The SDK HMAC provider's generation callback can call `esp_efuse_write_key()`
   when its requested key is missing.

That path is explicitly unsuitable for this application's no-fuse-write policy.
Simply pre-checking the key and then invoking generic initialization would retain
an unnecessary generation fallback.

The recorder instead selects `CONFIG_NVS_SEC_KEY_PROTECT_NONE=y`. The SDK then
**does not compile/link its automatic NVS providers**. The recorder registers
its own small `nvs_sec_scheme_t` through the public NVS security-scheme API:

* `nvs_flash_key_gen` is **NULL from the moment of registration**;
* the sole read callback verifies the existing key protections and calls the
  public `esp_hmac_calculate()` API;
* derivation matches IDF v6.1's public protocol constants: eight little-endian
  repetitions of `0xAEBE5A5A` for the XTS encryption key and `0xCEDEA5A5` for the
  tweak key. These inputs are not secret. No custom cryptographic primitive is
  implemented;
* `nvs_flash_read_security_cfg_v2()` followed by `nvs_flash_secure_init()` mounts
  the default partition; derived key buffers are zeroed after use;
* a failure removes the registered scheme and leaves credential access blocked.

The scheme is read/derive-only; it does not create root keys or write a separate
`nvs_keys` partition. The default NVS partition should **not** receive the
partition-table `encrypted` flag: that flag denotes flash encryption, not NVS
AES-XTS. The existing partition table is unchanged.

## Build-only validation profile

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
$py = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe'
& $py C:\esp\v6.1\esp-idf\tools\idf.py `
  -B build-hmac-validation -D SDKCONFIG=sdkconfig.hmac-validation `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.hmac-validation.defaults" build
```

Run this from `project\firmware`. The validation profile chooses key ID **0 only
as a compile-time example**. It does not assert that the connected board contains
that key, and does not authorize flashing or enabling real credentials.
Flash-encryption and secure-boot enablement remain disabled in this profile.
Kconfig and a compile-time guard reject combining this profile with either SDK
boot-time security-enable option, since their first-boot provisioning behavior
can itself modify eFuses. Future production hardening needs its own explicitly
approved provisioning/signing process, not a silent flag change here.

Host tests compile the actual storage implementation for all three profiles,
using only fake eFuse/HMAC/NVS boundaries and synthetic strings. They verify
failed prerequisites, both derivation failures, invalid equal derived keys,
NVS failure, read limits, success, and no generic initializer or write-capable
provider registration. The isolated encrypted image was also symbol-inspected:
`nvs_flash_init`, `nvs_flash_generate_keys*`, `generate_keys_hmac`, and
`esp_efuse_write_key` were absent from its defined application symbols.

## Security and migration limits

Encrypted NVS is **not** a complete physical-access defense. Without separately
approved secure boot and debug/download restrictions, an attacker able to run
arbitrary firmware may use the HMAC peripheral to derive the same keys. Tokens
also necessarily exist in RAM while in use. This profile does not claim to solve
those threats or silently lock the board to address them.

Do not toggle an existing real-credential plaintext installation to encrypted
mode without an approved backup/migration plan. No automatic migration or
formatting is implemented. SDK NVS recovery/metadata writes are distinct from
key provisioning; encrypted initialization and normal credential writes may
modify NVS only when the profile is deliberately enabled and its prerequisites
are met. No encrypted-profile code has been run on the connected hardware.
