#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$FoundationStatePath = (Join-Path $PSScriptRoot '..\.state\foundation.json'),
    [string]$ImageStatePath = (Join-Path $PSScriptRoot '..\.state\image.json'),
    [string]$IdentityStatePath = (Join-Path $PSScriptRoot '..\.state\identity.json'),
    [Parameter(Mandatory)][string]$ConfigurationPath,
    [ValidatePattern('^/[A-Za-z0-9/_-]*$')][string]$HealthPath = '/healthz',
    [ValidatePattern('^/[A-Za-z0-9/_-]*$')][string]$ReadinessPath = '/readyz',
    [switch]$Apply
)
. (Join-Path $PSScriptRoot 'Common.ps1')
$state = Get-Content -LiteralPath $FoundationStatePath -Raw | ConvertFrom-Json -AsHashtable
$image = Get-Content -LiteralPath $ImageStatePath -Raw | ConvertFrom-Json -AsHashtable
$identity = Get-Content -LiteralPath $IdentityStatePath -Raw | ConvertFrom-Json -AsHashtable
$configuration = Get-Content -LiteralPath $ConfigurationPath -Raw | ConvertFrom-Json -AsHashtable
$null = Assert-AzureContext -TenantId $state.tenantId -SubscriptionId $state.subscriptionId
if ($state.resourceGroup -ne 'copilot-test') {
    throw 'Foundation state must refer to copilot-test.'
}
if ($image.registryServer -ne $state.outputs.registryServer.value -or
    $image.image -notmatch "^$([regex]::Escape($image.registryServer))/recorder-proxy@sha256:[a-f0-9]{64}$") {
    throw 'Image does not belong to the selected private registry or is not digest-pinned.'
}
if (-not $configuration.ContainsKey('environment') -or $configuration.environment -isnot [hashtable]) {
    throw 'Configuration must contain an environment object with nonsecret string values.'
}
if ($identity.tenantId -ne $state.tenantId -or
    $configuration.environment.API_B_CLIENT_ID -ne $identity.apiClientId -or
    $configuration.environment.PUBLIC_CLIENT_A_ID -ne $identity.deviceClientId) {
    throw 'Proxy configuration must use the selected tenant and registered A/B client identifiers.'
}
if ([guid]$configuration.environment.ALLOWED_USER_OID -eq [guid]::Empty) {
    throw 'A real, explicitly authorized consumer-user object ID is required.'
}
if ($configuration.environment.VOICELIVE_ENDPOINT -ne $state.outputs.voiceLiveEndpoint.value -or
    $configuration.environment.VOICELIVE_MODEL -ne $state.outputs.voiceLiveModel.value -or
    $configuration.environment.VOICELIVE_PROFILE -ne $state.outputs.voiceLiveProfile.value) {
    throw 'Voice Live configuration must match the selected foundation state.'
}
if (-not $configuration.ContainsKey('authorizedUserConfigured') -or
    $configuration.authorizedUserConfigured -isnot [bool] -or
    -not $configuration.authorizedUserConfigured) {
    throw 'Explicit authorized-user configuration is required; no automatic user enrollment is performed.'
}
foreach ($entry in $configuration.environment.GetEnumerator()) {
    if ($entry.Key -notmatch '^[A-Z][A-Z0-9_]*$' -or $entry.Value -isnot [string]) {
        throw 'Environment names must be uppercase identifiers and values must be strings.'
    }
    if ($entry.Key -match '(SECRET|PASSWORD|ACCESS_TOKEN|REFRESH_TOKEN|API_KEY)$') {
        throw 'Do not deploy credentials as environment values. This application uses managed identity.'
    }
}
$configuration.environment.AZURE_CLIENT_ID = $state.outputs.proxyIdentityClientId.value
foreach ($flag in @('RECORDING_PROCESSING_ENABLED', 'ONEDRIVE_TOOLS_ENABLED')) {
    if ($configuration.environment.ContainsKey($flag) -and
        $configuration.environment[$flag] -cnotin @('true', 'false')) {
        throw "$flag must be exactly true or false."
    }
}
if ($configuration.environment.ContainsKey('RECORDING_PROCESSING_ENABLED') -and
    $configuration.environment.RECORDING_PROCESSING_ENABLED -ceq 'true') {
    $voiceEndpoint = $state.outputs.voiceLiveEndpoint.value
    if ($voiceEndpoint -notmatch '^https://([a-z0-9-]+)\.services\.ai\.azure\.com/?$') {
        throw 'Cannot resolve the Speech resource from the selected foundation.'
    }
    $expectedSpeechEndpoint = "https://$($Matches[1]).cognitiveservices.azure.com"
    if (-not $configuration.environment.ContainsKey('SPEECH_ENDPOINT') -or
        $configuration.environment.SPEECH_ENDPOINT.TrimEnd('/') -cne $expectedSpeechEndpoint) {
        throw 'Speech must use the selected Foundry custom-domain endpoint.'
    }
}
Write-Host "Proxy: $($state.namePrefix)-proxy in copilot-test, $($state.location)"
Write-Host "Image: $($image.image)"
Write-Host 'Public HTTPS/WSS ingress, managed identity, 0-1 Consumption replicas, 0.25 vCPU/0.5 GiB.'
Write-Host 'Ensure the explicit subject allowlist and identity audience/client configuration match the proxy README.'
if (-not $Apply) {
    Write-Host 'Preview only. No resources changed.'
    return
}
$parameters = @{
    location = @{ value = $state.location }
    namePrefix = @{ value = $state.namePrefix }
    environmentId = @{ value = $state.outputs.environmentId.value }
    identityId = @{ value = $state.outputs.proxyIdentityId.value }
    registryServer = @{ value = $state.outputs.registryServer.value }
    image = @{ value = $image.image }
    environmentVariables = @{ value = $configuration.environment }
    healthPath = @{ value = $HealthPath }
    readinessPath = @{ value = $ReadinessPath }
}
$temporary = [IO.Path]::GetTempFileName()
try {
    Write-JsonAtomic -Path $temporary -Value @{
        '$schema' = 'https://schema.management.azure.com/schemas/2019-04-01/deploymentParameters.json#'
        contentVersion = '1.0.0.0'
        parameters = $parameters
    }
    $arguments = @(
        '--resource-group', 'copilot-test', '--name', "$($state.namePrefix)-proxy",
        '--template-file', (Join-Path $PSScriptRoot '..\bicep\proxy.bicep'),
        '--parameters', "@$temporary"
    )
    $null = Invoke-AzureJson -Arguments (@('deployment', 'group', 'validate') + $arguments)
    $deployment = Invoke-AzureJson -Arguments (@('deployment', 'group', 'create') + $arguments)
    Write-JsonAtomic -Path (Join-Path $PSScriptRoot '..\.state\proxy.json') -Value @{
        schemaVersion = 1
        outputs = $deployment.properties.outputs
    }
}
finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
}
Write-Host 'Proxy deployed. URLs saved to .state\proxy.json. Verify readiness and authentication before using it.'
