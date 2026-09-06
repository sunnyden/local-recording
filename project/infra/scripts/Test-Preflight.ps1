#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][guid]$TenantId,
    [Parameter(Mandatory)][guid]$SubscriptionId,
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9]+$')][string]$Location,
    [ValidateSet('native', 'byom-azure-openai-realtime')][string]$VoiceProfile = 'byom-azure-openai-realtime',
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9._-]+$')][string]$ModelVersion,
    [ValidateSet('GlobalStandard', 'DataZoneStandard', 'Standard')][string]$ModelSku = 'GlobalStandard',
    [switch]$NativeModelConfirmed
)
. (Join-Path $PSScriptRoot 'Common.ps1')
$null = Assert-AzureContext -TenantId $TenantId -SubscriptionId $SubscriptionId

$unregistered = @()
foreach ($provider in @('Microsoft.CognitiveServices', 'Microsoft.App', 'Microsoft.ContainerRegistry', 'Microsoft.ManagedIdentity')) {
    $state = Invoke-AzureJson -Arguments @('provider', 'show', '--namespace', $provider, '--query', 'registrationState')
    if ($state -ne 'Registered') {
        $unregistered += $provider
    }
}
if ($unregistered.Count) {
    throw "Required resource providers are not registered: $($unregistered -join ', '). Register them explicitly before deployment."
}

if ($VoiceProfile -eq 'native') {
    if (-not $NativeModelConfirmed) {
        throw 'Native Voice Live model availability must be explicitly confirmed for this resource region. Otherwise use BYOM preflight.'
    }
} else {
    $models = @(Invoke-AzureJson -Arguments @(
        'cognitiveservices', 'model', 'list', '--location', $Location,
        '--query', "[?model.name=='gpt-realtime-2' && model.version=='$ModelVersion']"
    ))
    if ($models.Count -eq 0) {
        throw "Exact model gpt-realtime-2/$ModelVersion is not listed in $Location. No model substitution or deployment will occur."
    }
    $matchingSku = @($models | ForEach-Object { $_.model.skus } | Where-Object { $_.name -eq $ModelSku })
    if ($matchingSku.Count -eq 0) {
        throw "The selected model deployment SKU $ModelSku is not listed for this model/region."
    }
}
[pscustomobject]@{
    subscriptionId = $SubscriptionId
    tenantId = $TenantId
    resourceGroup = 'copilot-test'
    location = $Location
    model = 'gpt-realtime-2'
    modelVersion = $ModelVersion
    voiceProfile = $VoiceProfile
    note = 'Catalog presence is not a quota guarantee. ARM validation and a real Voice Live session are still required.'
}
