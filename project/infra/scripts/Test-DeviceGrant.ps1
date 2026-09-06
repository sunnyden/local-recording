#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$IdentityStatePath = (Join-Path $PSScriptRoot '..\.state\identity.json'),
    [ValidateSet('graph', 'proxy')][string]$Resource = 'proxy'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$state = Get-Content -LiteralPath $IdentityStatePath -Raw | ConvertFrom-Json -AsHashtable
$clientId = [guid]$state.deviceClientId
$scope = if ($Resource -eq 'graph') { $state.graphScope } else { $state.proxyScope }
$response = Invoke-RestMethod -Method Post `
    -Uri 'https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode' `
    -ContentType 'application/x-www-form-urlencoded' `
    -Body @{ client_id = $clientId.ToString(); scope = "$scope offline_access" } `
    -TimeoutSec 30
if (-not $response.device_code -or -not $response.user_code -or
    $response.expires_in -le 0 -or $response.interval -le 0) {
    throw 'Microsoft did not return a valid device-authorization response.'
}
$response = $null
[pscustomobject]@{
    resource = $Resource
    scope = $scope
    result = 'Consumer device grant accepted. No code was displayed, no token was requested, and no user consent occurred.'
}
