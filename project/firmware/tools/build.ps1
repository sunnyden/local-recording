param([string]$ActivationScript = "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1")
$ErrorActionPreference = "Stop"
. $ActivationScript
$env:PYTHONUTF8 = "1"
$python = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"
$idf = Join-Path $env:IDF_PATH "tools\idf.py"
if (!(Test-Path $python) -or !(Test-Path $idf)) {
    throw "Activated SDK Python or idf.py is missing; check the selected installation profile"
}
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    & $python $idf build
    if ($LASTEXITCODE) { throw "Firmware build failed ($LASTEXITCODE)" }
} finally { Pop-Location }
