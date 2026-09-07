param([string]$Compiler = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build-host-intelligence"
New-Item -ItemType Directory -Force $build | Out-Null
if (!$Compiler) { $Compiler = Join-Path $root ".host-tools\ziglang\zig.exe" }
if (!(Test-Path $Compiler)) { throw "Existing Zig host compiler not found" }
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root "build-host\zig-global-cache"
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $build "zig-local-cache"
$cjson = Join-Path $root "managed_components\espressif__cjson\cJSON"
$flags = @("-std=c11", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-D_GNU_SOURCE",
    "-include", "$root\tests\host\storage_compat.h", "-I", "$root\tests\host",
    "-I", "$root\components\recorder\include", "-I", "$root\components\recorder_core\include",
    "-I", "$root\components\cloud_sync\include", "-I", "$root\components\network\include",
    "-I", "$root\components\identity\include", "-I", $cjson)
$storage = @("$root\components\recorder\storage.c", "$root\components\recorder\recording_name.c",
    "$root\components\recorder_core\wav.c", "$root\components\cloud_sync\processing_outbox.c")
Push-Location $build
try {
    & $Compiler cc @flags -Dtime=recording_test_time "$root\tests\recording_storage_test.c" @storage -o recording-storage-test.exe
    if ($LASTEXITCODE) { throw "Recording storage compile failed" }
    & .\recording-storage-test.exe
    if ($LASTEXITCODE) { throw "Recording storage tests failed" }
    & $Compiler cc @flags -Wno-unused-parameter -Dtime=processing_test_time "$root\tests\cloud_processing_test.c" @storage `
        "$root\components\cloud_sync\cloud_sync.c" "$root\components\cloud_sync\processing_client.c" `
        "$root\components\cloud_sync\processing_stream.c" "$root\components\cloud_sync\processing_protocol.c" `
        "$root\components\recorder_core\voice_frame.c" `
        "$root\components\cloud_sync\sync_ui.c" "$root\components\network\proxy_endpoint.c" `
        "$cjson\cJSON.c" -o cloud-processing-test.exe
    if ($LASTEXITCODE) { throw "Cloud processing compile failed" }
    & .\cloud-processing-test.exe
    if ($LASTEXITCODE) { throw "Cloud processing tests failed" }
    & $Compiler cc @flags "$root\tests\processing_stream_test.c" `
        "$root\components\cloud_sync\processing_stream.c" "$root\components\cloud_sync\processing_protocol.c" `
        "$root\components\network\proxy_endpoint.c" "$root\components\recorder_core\voice_frame.c" `
        "$cjson\cJSON.c" -o processing-stream-test.exe
    if ($LASTEXITCODE) { throw "Processing stream compile failed" }
    & .\processing-stream-test.exe
    if ($LASTEXITCODE) { throw "Processing stream tests failed" }
} finally { Pop-Location }
