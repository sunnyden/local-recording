#Requires -Version 7.2
[CmdletBinding()]
param(
    [ValidatePattern('^COM[0-9]+$')][string]$Port,
    [string]$ActivationScript = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1',
    [switch]$AllowReset
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $Port) {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.Class -eq 'Ports' -and $_.FriendlyName -match 'USB-SERIAL CH340 \(COM[0-9]+\)'
    })
    if ($devices.Count -ne 1) {
        throw 'Exactly one CH340 must be connected for automatic discovery; otherwise pass -Port explicitly.'
    }
    $null = $devices[0].FriendlyName -match '\((COM[0-9]+)\)'
    $Port = $Matches[1]
}
Write-Host "Selected serial port: $Port"
if (-not $AllowReset) {
    Write-Host 'Discovery only. -AllowReset permits ROM chip/flash identification and RTS resets, but no flash writes, erasure or eFuse changes.'
    return
}
# The vendor profile probes optional command properties under non-strict semantics.
Set-StrictMode -Off
try {
    . $ActivationScript
} finally {
    Set-StrictMode -Version Latest
}
$env:PYTHONUTF8 = '1'
python -m esptool --chip esp32s3 --port $Port --no-stub chip-id
if ($LASTEXITCODE -ne 0) { throw "ROM chip identification failed ($LASTEXITCODE)." }
python -m esptool --chip esp32s3 --port $Port --no-stub flash-id
if ($LASTEXITCODE -ne 0) { throw "ROM flash identification failed ($LASTEXITCODE)." }
