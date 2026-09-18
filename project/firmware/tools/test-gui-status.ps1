param([string]$Compiler = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build-gui-status-host"
New-Item -ItemType Directory -Force $build | Out-Null
if (!$Compiler) { $Compiler = Join-Path $root ".host-tools\ziglang\zig.exe" }
if (!(Test-Path $Compiler)) { throw "Host compiler missing: use the existing ziglang tool or pass -Compiler" }
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $build "zig-global-cache"
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $build "zig-local-cache"
$cjson = Join-Path $root "managed_components\espressif__cjson\cJSON"
$flags = @("-std=c11", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-D_GNU_SOURCE",
    "-DRECORDER_HOST_TEST", "-I", "$root\tests\gui_status_host", "-I", "$root\tests\host",
    "-I", "$root\components\rolling_audio\include",
    "-I", "$root\components\recorder_core\include", "-I", "$root\components\recorder\include",
    "-I", "$root\components\audio_io\include", "-I", "$root\components\identity\include",
    "-I", "$root\components\voice_client\include", "-I", "$root\components\provisioning\include",
    "-I", $cjson)
Push-Location $build
try {
    & $Compiler cc @flags -include "$root\tests\gui_status_host\time_compat.h" "$root\tests\gui_status_local_test.c" `
        "$root\components\recorder\recording_name.c" -o gui-status-local.exe
    if ($LASTEXITCODE) { throw "Local status compile failed ($LASTEXITCODE)" }
    & .\gui-status-local.exe
    if ($LASTEXITCODE) { throw "Local status test failed ($LASTEXITCODE)" }
    & $Compiler cc @flags -Dcalloc=voice_test_calloc "$root\tests\gui_status_voice_test.c" `
        "$root\components\recorder_core\voice_frame.c" "$cjson\cJSON.c" -o gui-status-voice.exe
    if ($LASTEXITCODE) { throw "Voice status compile failed ($LASTEXITCODE)" }
    & .\gui-status-voice.exe
    if ($LASTEXITCODE) { throw "Voice status test failed ($LASTEXITCODE)" }
    & $Compiler cc @flags "$root\tests\gui_status_provisioning_test.c" -o gui-status-provisioning.exe
    if ($LASTEXITCODE) { throw "Provisioning display compile failed ($LASTEXITCODE)" }
    & .\gui-status-provisioning.exe
    if ($LASTEXITCODE) { throw "Provisioning display test failed ($LASTEXITCODE)" }
    $source = Get-Content "$root\components\provisioning\provisioning.c" -Raw
    if ($source -match 'ESP_LOG|printf\s*\(') {
        # snprintf only formats private, bounded transient values.
        if (($source -replace 'snprintf\s*\(', '') -match 'ESP_LOG|printf\s*\(') {
            throw "Unexpected logging/output in the private setup source"
        }
    }
    Write-Output "PASS: provisioning source has no credential logging/output sinks"
} finally { Pop-Location }
