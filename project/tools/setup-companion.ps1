#Requires -Version 7.2
param([switch]$SkipReadyPrompt)
$ErrorActionPreference = 'Stop'
$companion = Join-Path (Split-Path $PSScriptRoot -Parent) 'companion'
$python = Join-Path $companion '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python)) {
    throw 'Companion virtual environment is missing; follow project\companion\README.md first.'
}
Write-Host 'Select SETUP on the recorder. From the initial main menu: KEY3, then KEY0.'
Write-Host 'Enter the displayed setup username/password only in the hidden terminal prompts.'
Write-Host 'Then use: wifi -> graph -> status -> finish.'
Write-Host 'Your Microsoft password belongs only on the Microsoft browser page, never in this terminal or chat.'
if (-not $SkipReadyPrompt) {
    $null = Read-Host 'Press Enter once the recorder shows its setup username/password'
}
Push-Location $companion
try {
    & $python -m recorder_companion setup
    if ($LASTEXITCODE -ne 0) {
        throw "Companion exited with code $LASTEXITCODE. Check its message and reconnect; commands were not replayed."
    }
} finally {
    Pop-Location
}
