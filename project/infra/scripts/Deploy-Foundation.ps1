#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][guid]$TenantId,
    [Parameter(Mandatory)][guid]$SubscriptionId,
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9]+$')][string]$Location,
    [ValidatePattern('^[a-z][a-z0-9-]{2,17}$')][string]$NamePrefix = 'recorder',
    [ValidateSet('native', 'byom-azure-openai-realtime')][string]$VoiceProfile = 'byom-azure-openai-realtime',
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9._-]+$')][string]$ModelVersion,
    [ValidateRange(1, 100)][int]$ModelCapacity = 1,
    [ValidateSet('GlobalStandard', 'DataZoneStandard', 'Standard')][string]$ModelSku = 'GlobalStandard',
    [string]$StatePath = (Join-Path $PSScriptRoot '..\.state\foundation.json'),
    [switch]$NativeModelConfirmed,
    [switch]$Apply
)
. (Join-Path $PSScriptRoot 'Common.ps1')
$preflight = & (Join-Path $PSScriptRoot 'Test-Preflight.ps1') `
    -TenantId $TenantId -SubscriptionId $SubscriptionId -Location $Location `
    -VoiceProfile $VoiceProfile -ModelVersion $ModelVersion -ModelSku $ModelSku `
    -NativeModelConfirmed:$NativeModelConfirmed
$preflight | Format-List
Write-Host 'Creates Foundry resource/project, optional exact-model deployment, Basic private ACR, managed identity/RBAC and a Consumption Container Apps environment.'
Write-Host 'Registry storage and AI usage are billable. There is no hard spending cap.'
if (-not $Apply) {
    Write-Host 'Preview only. No resources were changed. Supply -Apply only after reviewing this inventory.'
    return
}

$group = 'copilot-test'
$exists = Invoke-AzureJson -Arguments @('group', 'exists', '--name', $group)
if ($exists) {
    $resourceGroup = Invoke-AzureJson -Arguments @('group', 'show', '--name', $group)
    if (-not $resourceGroup.ContainsKey('tags') -or -not $resourceGroup.tags -or
        -not $resourceGroup.tags.ContainsKey('application') -or
        $resourceGroup.tags.application -ne 'embedded-recorder') {
        throw 'Existing copilot-test is not marked as owned by this deployment. Review its contents and ownership explicitly; no changes were made.'
    }
} else {
    $null = Invoke-AzureJson -Arguments @('group', 'create', '--name', $group, '--location', $Location, '--tags', 'application=embedded-recorder')
}

if (Test-Path -LiteralPath $StatePath) {
    $previous = Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json -AsHashtable
    if ($previous.subscriptionId -ne $SubscriptionId.ToString() -or
        $previous.location -ne $Location -or $previous.namePrefix -ne $NamePrefix) {
        throw 'Saved foundation state belongs to a different subscription, region or name prefix. Use a separate state file for another deployment.'
    }
}
$parameters = @{
    location = @{ value = $Location }
    namePrefix = @{ value = $NamePrefix }
    voiceProfile = @{ value = $VoiceProfile }
    modelName = @{ value = 'gpt-realtime-2' }
    modelVersion = @{ value = $ModelVersion }
    modelCapacity = @{ value = $ModelCapacity }
    modelSku = @{ value = $ModelSku }
}
$temporary = [IO.Path]::GetTempFileName()
try {
    Write-JsonAtomic -Path $temporary -Value @{
        '$schema' = 'https://schema.management.azure.com/schemas/2019-04-01/deploymentParameters.json#'
        contentVersion = '1.0.0.0'
        parameters = $parameters
    }
    $arguments = @(
        '--resource-group', $group, '--name', "$NamePrefix-foundation",
        '--template-file', (Join-Path $PSScriptRoot '..\bicep\base.bicep'),
        '--parameters', "@$temporary"
    )
    $null = Invoke-AzureJson -Arguments (@('deployment', 'group', 'validate') + $arguments)
    $deployment = Invoke-AzureJson -Arguments (@('deployment', 'group', 'create') + $arguments)
    $state = @{
        schemaVersion = 1
        tenantId = $TenantId.ToString()
        subscriptionId = $SubscriptionId.ToString()
        resourceGroup = $group
        location = $Location
        namePrefix = $NamePrefix
        outputs = $deployment.properties.outputs
    }
    Write-JsonAtomic -Path $StatePath -Value $state
}
finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary
    }
}
Write-Host "Foundation deployed. Nonsecret outputs saved to $StatePath."
Write-Host 'The voice proxy is not deployed yet. Build/publish its private image and configure its user allowlist next.'
