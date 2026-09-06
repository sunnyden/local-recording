#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][guid]$AllowedUserOid,
    [string]$FoundationStatePath = (Join-Path $PSScriptRoot '..\.state\foundation.json'),
    [string]$IdentityStatePath = (Join-Path $PSScriptRoot '..\.state\identity.json'),
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\.state\proxy-configuration.json'),
    [ValidateRange(1, 900)][int]$MaxSessionSeconds = 900
)
. (Join-Path $PSScriptRoot 'Common.ps1')
if ($AllowedUserOid -eq [guid]::Empty) {
    throw 'Supply the explicitly authorized consumer-user oid, not a placeholder or the Azure work account.'
}
$foundation = Get-Content -LiteralPath $FoundationStatePath -Raw | ConvertFrom-Json -AsHashtable
$identity = Get-Content -LiteralPath $IdentityStatePath -Raw | ConvertFrom-Json -AsHashtable
if ($foundation.tenantId -ne $identity.tenantId -or $foundation.resourceGroup -ne 'copilot-test') {
    throw 'Foundation and identity state must describe the same tenant and copilot-test deployment.'
}
Write-JsonAtomic -Path $OutputPath -Value @{
    authorizedUserConfigured = $true
    environment = @{
        API_B_CLIENT_ID = $identity.apiClientId
        PUBLIC_CLIENT_A_ID = $identity.deviceClientId
        ALLOWED_USER_OID = $AllowedUserOid.ToString()
        AZURE_CLIENT_ID = $foundation.outputs.proxyIdentityClientId.value
        VOICELIVE_ENDPOINT = $foundation.outputs.voiceLiveEndpoint.value
        VOICELIVE_MODEL = $foundation.outputs.voiceLiveModel.value
        VOICELIVE_PROFILE = $foundation.outputs.voiceLiveProfile.value
        VOICELIVE_API_VERSION = '2026-07-15'
        MAX_SESSION_SECONDS = $MaxSessionSeconds.ToString([Globalization.CultureInfo]::InvariantCulture)
    }
}
Write-Host "Nonsecret proxy configuration saved to $OutputPath. This does not sign in or authorize a Microsoft account."
