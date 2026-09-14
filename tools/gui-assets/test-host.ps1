param([string]$Compiler = "")
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$build = Join-Path $PSScriptRoot ".build"
New-Item -ItemType Directory -Force $build | Out-Null
if (!$Compiler) { $Compiler = Join-Path $root "project\firmware\.host-tools\ziglang\zig.exe" }
if (!(Get-Command $Compiler -ErrorAction SilentlyContinue)) {
    throw "Pass -Compiler with an existing C11 compiler (gcc/clang/zig). Node is not required."
}
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $build "zig-global-cache"
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $build "zig-local-cache"
$flags = @("-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic")
if ((Split-Path $Compiler -Leaf) -match "^zig(\.exe)?$") { $flags = @("cc") + $flags }
$gui = Join-Path $root "project\firmware\components\gui"
& $Compiler @flags -I "$gui\include" "$PSScriptRoot\test-host.c" `
    "$gui\assets\gui_assets.c" "$gui\assets\gui_fonts.c" -o "$build\gui-assets-test.exe"
if ($LASTEXITCODE) { throw "Asset host compile failed ($LASTEXITCODE)" }
& "$build\gui-assets-test.exe"
if ($LASTEXITCODE) { throw "Asset host tests failed ($LASTEXITCODE)" }
