param([string]$Compiler = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build-host-gui"
New-Item -ItemType Directory -Force $build | Out-Null
if (!$Compiler) { $Compiler = Join-Path $root ".host-tools\ziglang\zig.exe" }
if (!(Test-Path $Compiler)) { throw "Use existing Zig 0.14.1 in .host-tools or pass -Compiler" }
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $build "zig-global-cache"
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $build "zig-local-cache"
$flags = @("-std=c11", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-D_GNU_SOURCE",
    "-I", "$root\tests\gui_host", "-I", "$root\components\gui\include", "-I", "$root\components\board\include",
    "-I", "$root\tests\host")
Push-Location $build
try {
    & $Compiler cc @flags "$root\tests\gui_host\render_test.c" `
        "$root\components\gui\gui_draw.c" "$root\components\gui\gui_screens.c" `
        "$root\components\gui\gui_test_input.c" "$root\components\gui\assets\gui_assets.c" `
        "$root\components\gui\assets\gui_fonts.c" -o render-test.exe
    if ($LASTEXITCODE) { throw "GUI render test compilation failed" }
    & .\render-test.exe
    if ($LASTEXITCODE) { throw "GUI render tests failed" }
    & $Compiler cc @flags "$root\tests\gui_host\display_test.c" -o display-test.exe
    if ($LASTEXITCODE) { throw "LCD ownership test compilation failed" }
    & .\display-test.exe
    if ($LASTEXITCODE) { throw "LCD ownership tests failed" }
    $includes = @()
    foreach ($component in @("recorder", "rolling_audio", "lan_server", "recorder_core", "cloud_sync", "voice_client", "provisioning", "network")) {
        $includes += @("-I", "$root\components\$component\include")
    }
    & $Compiler cc @includes @flags -I "$root\managed_components\espressif__cjson\cJSON" `
        "$root\tests\gui_host\app_test.c" "$root\components\gui\gui_draw.c" `
        "$root\components\gui\gui_screens.c" "$root\components\gui\assets\gui_assets.c" `
        "$root\components\gui\assets\gui_fonts.c" "$root\components\cloud_sync\sync_ui.c" -o app-test.exe
    if ($LASTEXITCODE) { throw "GUI controller test compilation failed" }
    & .\app-test.exe
    if ($LASTEXITCODE) { throw "GUI controller tests failed" }
    foreach ($profile in @("build-gui-poc", "build-gui-test")) {
        $binary = Join-Path $root "$profile\embedded_recorder.bin"
        if (Test-Path $binary) {
            $bytes = (Get-Item $binary).Length
            if ($bytes -gt 3145728 -or $bytes - 1535104 -gt 196608) {
                throw "$profile exceeds the installed 3 MiB slot or 192 KiB GUI increment"
            }
            Write-Output "PASS ${profile}: $bytes bytes; within physical slot and GUI flash increment"
        }
    }
} finally { Pop-Location }
