param([string]$Compiler = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build-host"
New-Item -ItemType Directory -Force $build | Out-Null
if (!$Compiler) { $Compiler = Join-Path $root ".host-tools\ziglang\zig.exe" }
if (!(Test-Path $Compiler)) {
    throw "Host compiler missing: install ziglang==0.14.1 into .host-tools or pass -Compiler"
}
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $build "zig-global-cache"
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $build "zig-local-cache"
Push-Location $build
try {
    & $Compiler cc -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -I "$root\components\recorder_core\include" "$root\tests\core_test.c" "$root\components\recorder_core\wav.c" "$root\components\recorder_core\voice_frame.c" -o core-test.exe
    if ($LASTEXITCODE) { throw "Portable core compile failed ($LASTEXITCODE)" }
    & .\core-test.exe
    if ($LASTEXITCODE) { throw "Portable core tests failed ($LASTEXITCODE)" }
    $cjson = Join-Path $root "managed_components\espressif__cjson\cJSON"
    if (!(Test-Path "$cjson\cJSON.c")) { throw "Run the IDF build to resolve pinned cJSON before host integration tests" }
    & $Compiler cc -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -D_GNU_SOURCE `
        -I "$root\tests\host" -I "$root\components\identity\include" `
        -I "$root\components\provisioning\include" -I "$root\components\recorder_core\include" -I $cjson `
        "$root\tests\identity_control_test.c" "$root\components\identity\identity.c" `
        "$root\components\provisioning\control.c" "$root\components\recorder_core\voice_frame.c" `
        "$cjson\cJSON.c" -o identity-control-test.exe
    if ($LASTEXITCODE) { throw "Host integration compile failed ($LASTEXITCODE)" }
    & .\identity-control-test.exe
    if ($LASTEXITCODE) { throw "Host identity/control tests failed ($LASTEXITCODE)" }
    & $Compiler cc -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -D_GNU_SOURCE `
        -I "$root\tests\host" -I "$root\components\cloud_sync\include" `
        -I "$root\components\recorder_core\include" -I $cjson `
        "$root\tests\upload_test.c" "$root\components\cloud_sync\upload.c" `
        "$root\components\recorder_core\wav.c" "$root\components\recorder_core\voice_frame.c" `
        "$cjson\cJSON.c" -o upload-test.exe
    if ($LASTEXITCODE) { throw "Host uploader compile failed ($LASTEXITCODE)" }
    & .\upload-test.exe
    if ($LASTEXITCODE) { throw "Host uploader tests failed ($LASTEXITCODE)" }
    & $Compiler cc -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -D_GNU_SOURCE `
        -I "$root\tests\host" -I "$root\components\recorder_core\include" -I $cjson `
        "$root\tests\https_test.c" "$root\components\network\https.c" `
        "$root\components\recorder_core\voice_frame.c" "$cjson\cJSON.c" -o https-test.exe
    if ($LASTEXITCODE) { throw "Host HTTPS compile failed ($LASTEXITCODE)" }
    & .\https-test.exe
    if ($LASTEXITCODE) { throw "Host HTTPS tests failed ($LASTEXITCODE)" }
    & $Compiler cc -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS `
        -I "$root\tests\host" -I $cjson `
        "$root\tests\network_test.c" "$root\components\network\network.c" -o network-test.exe
    if ($LASTEXITCODE) { throw "Host network compile failed ($LASTEXITCODE)" }
    & .\network-test.exe
    if ($LASTEXITCODE) { throw "Host network tests failed ($LASTEXITCODE)" }
    & $Compiler cc -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -D_GNU_SOURCE `
        -I "$root\tests\host" -I "$root\components\voice_client\include" `
        -I "$root\components\audio_io\include" -I "$root\components\identity\include" `
        -I "$root\components\recorder_core\include" -I $cjson `
        "$root\tests\voice_client_test.c" "$root\components\voice_client\voice_client.c" `
        "$root\components\recorder_core\voice_frame.c" "$cjson\cJSON.c" -o voice-client-test.exe
    if ($LASTEXITCODE) { throw "Host voice compile failed ($LASTEXITCODE)" }
    & .\voice-client-test.exe
    if ($LASTEXITCODE) { throw "Host voice tests failed ($LASTEXITCODE)" }
    & $Compiler cc -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS `
        -I "$root\components\recorder_core\include" -I $cjson `
        "$root\tests\voice_fixtures_test.c" "$root\components\recorder_core\voice_frame.c" `
        "$cjson\cJSON.c" -o voice-fixtures-test.exe
    if ($LASTEXITCODE) { throw "Shared fixture compile failed ($LASTEXITCODE)" }
    & .\voice-fixtures-test.exe "$root\..\protocols\voice-v1-fixtures.json"
    if ($LASTEXITCODE) { throw "Shared voice fixtures failed ($LASTEXITCODE)" }
    & $Compiler cc -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS `
        -I "$root\tests\host" -I "$root\components\audio_io\include" `
        -I "$root\components\recorder_core\include" `
        "$root\tests\audio_io_test.c" "$root\components\audio_io\audio_io.c" -o audio-io-test.exe
    if ($LASTEXITCODE) { throw "Host I2S compile failed ($LASTEXITCODE)" }
    & .\audio-io-test.exe
    if ($LASTEXITCODE) { throw "Host I2S tests failed ($LASTEXITCODE)" }
    $storageSources = @("$root\tests\credential_storage_test.c",
        "$root\components\network\credential_storage.c", "$root\components\network\secure_store.c")
    $storageFlags = @("-std=c11", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
        "-I", "$root\tests\host", "-I", $cjson)
    & $Compiler cc @storageFlags @storageSources -o storage-disabled-test.exe
    if ($LASTEXITCODE) { throw "Disabled storage compile failed ($LASTEXITCODE)" }
    & .\storage-disabled-test.exe disabled
    if ($LASTEXITCODE) { throw "Disabled storage gate failed ($LASTEXITCODE)" }
    & $Compiler cc @storageFlags -DCONFIG_RECORDER_DEVELOPMENT_CREDENTIAL_RISK=1 `
        @storageSources -o storage-plaintext-test.exe
    if ($LASTEXITCODE) { throw "Development storage compile failed ($LASTEXITCODE)" }
    foreach ($case in @("plain-good", "plain-init-fails")) {
        & .\storage-plaintext-test.exe $case
        if ($LASTEXITCODE) { throw "Development storage test failed: $case" }
    }
    & $Compiler cc @storageFlags -DCONFIG_RECORDER_NVS_HMAC_EXISTING=1 -DCONFIG_NVS_ENCRYPTION=1 `
        -DCONFIG_NVS_SEC_KEY_PROTECT_NONE=1 -DCONFIG_RECORDER_NVS_HMAC_KEY_ID=2 `
        @storageSources -o storage-hmac-test.exe
    if ($LASTEXITCODE) { throw "Existing-key storage compile failed ($LASTEXITCODE)" }
    foreach ($case in @("missing", "wrong-purpose", "readable-key", "writable-key",
        "writable-purpose", "readonly-registration-fails", "derive-fails",
        "derive-second-fails", "equal-derived-keys", "secure-init-fails", "hmac-good")) {
        & .\storage-hmac-test.exe $case
        if ($LASTEXITCODE) { throw "Existing-key storage test failed: $case" }
    }
    foreach ($unsafeOption in @("CONFIG_SECURE_FLASH_ENC_ENABLED", "CONFIG_SECURE_BOOT")) {
        & $Compiler cc @storageFlags -DCONFIG_RECORDER_NVS_HMAC_EXISTING=1 -DCONFIG_NVS_ENCRYPTION=1 `
            -DCONFIG_NVS_SEC_KEY_PROTECT_NONE=1 -DCONFIG_RECORDER_NVS_HMAC_KEY_ID=2 `
            "-D$unsafeOption=1" -c "$root\components\network\credential_storage.c" `
            -o rejected-storage-profile.o 2> storage-profile-rejection.log
        if ($LASTEXITCODE -eq 0 -or !(Select-String storage-profile-rejection.log -Pattern "boot-time fuse provisioning" -Quiet)) {
            throw "Unsafe storage profile was not rejected as expected: $unsafeOption"
        }
    }
    Write-Output "PASS: encrypted profile rejects both SDK boot-time fuse-enable options"
} finally { Pop-Location }
