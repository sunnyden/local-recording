param(
    [string]$ActivationScript = "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1",
    [ValidateSet("default", "local-bringup", "hmac-validation")]
    [string]$Profile = "default"
)
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
    $idfArguments = @("build")
    if ($Profile -ne "default") {
        $idfArguments = @("-B", "build-$Profile", "-D", "SDKCONFIG=sdkconfig.$Profile",
            "-D", "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.$Profile.defaults", "build")
    }
    & $python $idf @idfArguments
    if ($LASTEXITCODE) { throw "Firmware build failed ($LASTEXITCODE)" }
    if ($Profile -eq "local-bringup") {
        $header = "build-local-bringup\config\sdkconfig.h"
        if (!(Select-String $header -Pattern "^#define CONFIG_RECORDER_SPI_LCD_CONFIRMED 1$" -Quiet)) {
            throw "Local bring-up build did not enable the confirmed LCD"
        }
        $forbidden = "^#define CONFIG_(RECORDER_DEVELOPMENT_CREDENTIAL_RISK|RECORDER_NVS_HMAC_EXISTING|RECORDER_TEST_FORMAT_SD_ON_MOUNT_FAILURE|NVS_ENCRYPTION|SECURE_FLASH_ENC_ENABLED|SECURE_BOOT)\b"
        if (Select-String $header -Pattern $forbidden -Quiet) {
            throw "Local bring-up build enabled a forbidden credential, SD-format or fuse-provisioning option"
        }
    }
} finally { Pop-Location }
