#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$FoundationStatePath = (Join-Path $PSScriptRoot '..\.state\foundation.json')
)
. (Join-Path $PSScriptRoot 'Common.ps1')
$state = Get-Content -LiteralPath $FoundationStatePath -Raw | ConvertFrom-Json -AsHashtable
$null = Assert-AzureContext -TenantId $state.tenantId -SubscriptionId $state.subscriptionId
if ($state.resourceGroup -ne 'copilot-test') {
    throw 'Deployment state must refer to copilot-test.'
}
$inventory = @(Invoke-AzureJson -Arguments @(
    'resource', 'list', '--resource-group', 'copilot-test',
    '--query', '[].{name:name,type:type,location:location}'
))
$apps = @($inventory | Where-Object { $_.type -eq 'Microsoft.App/containerApps' })
$revisions = @()
foreach ($app in $apps) {
    $active = @(Invoke-AzureJson -Arguments @(
        'containerapp', 'revision', 'list', '--resource-group', 'copilot-test',
        '--name', $app.name, '--query', '[?properties.active].{name:name,replicas:properties.replicas,health:properties.healthState}'
    ))
    $revisions += $active
}
[pscustomobject]@{
    resourceGroup = 'copilot-test'
    location = $state.location
    voiceModel = $state.outputs.voiceLiveModel.value
    voiceProfile = $state.outputs.voiceLiveProfile.value
    resources = $inventory
    activeProxyRevisions = $revisions
    proxyState = if ($apps.Count -eq 0) { 'Not deployed' } else { 'Inspect revision health and replicas' }
    costNote = 'Zero replicas removes proxy compute usage, not registry storage or other service charges.'
} | ConvertTo-Json -Depth 8
