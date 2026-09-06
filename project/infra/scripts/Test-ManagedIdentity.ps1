#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$FoundationStatePath = (Join-Path $PSScriptRoot '..\.state\foundation.json'),
    [string]$ImageStatePath = (Join-Path $PSScriptRoot '..\.state\image.json'),
    [switch]$Apply
)
. (Join-Path $PSScriptRoot 'Common.ps1')
$state = Get-Content -LiteralPath $FoundationStatePath -Raw | ConvertFrom-Json -AsHashtable
$image = Get-Content -LiteralPath $ImageStatePath -Raw | ConvertFrom-Json -AsHashtable
$null = Assert-AzureContext -TenantId $state.tenantId -SubscriptionId $state.subscriptionId
if ($state.resourceGroup -ne 'copilot-test' -or
    $image.registryServer -ne $state.outputs.registryServer.value -or
    $image.image -notmatch "^$([regex]::Escape($image.registryServer))/recorder-proxy@sha256:[a-f0-9]{64}$") {
    throw 'The identity probe requires the owned copilot-test foundation and its immutable private image.'
}
Write-Host 'This creates/updates a manual Consumption job and runs one bounded managed-identity Voice Live configuration probe.'
Write-Host 'The job exposes no ingress, sends no user audio, has no schedule, and has a 90-second replica timeout with no retries.'
Write-Host 'It uses the actual proxy image, registry-pull identity and Voice Live roles. Execution charges may apply.'
if (-not $Apply) {
    Write-Host 'Preview only. No resources or executions changed.'
    return
}
$parameters = @{
    location = @{ value = $state.location }
    namePrefix = @{ value = $state.namePrefix }
    environmentId = @{ value = $state.outputs.environmentId.value }
    identityId = @{ value = $state.outputs.proxyIdentityId.value }
    identityClientId = @{ value = $state.outputs.proxyIdentityClientId.value }
    registryServer = @{ value = $state.outputs.registryServer.value }
    image = @{ value = $image.image }
    voiceLiveEndpoint = @{ value = $state.outputs.voiceLiveEndpoint.value }
    voiceLiveModel = @{ value = $state.outputs.voiceLiveModel.value }
    voiceLiveProfile = @{ value = $state.outputs.voiceLiveProfile.value }
}
$temporary = [IO.Path]::GetTempFileName()
try {
    Write-JsonAtomic -Path $temporary -Value @{
        '$schema' = 'https://schema.management.azure.com/schemas/2019-04-01/deploymentParameters.json#'
        contentVersion = '1.0.0.0'
        parameters = $parameters
    }
    $arguments = @(
        '--resource-group', 'copilot-test', '--name', "$($state.namePrefix)-identity-probe",
        '--template-file', (Join-Path $PSScriptRoot '..\bicep\identity-probe.bicep'),
        '--parameters', "@$temporary"
    )
    $null = Invoke-AzureJson -Arguments (@('deployment', 'group', 'validate') + $arguments)
    $deployment = Invoke-AzureJson -Arguments (@('deployment', 'group', 'create') + $arguments)
}
finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
}
$job = $deployment.properties.outputs.jobName.value
$execution = Invoke-AzureJson -Arguments @('containerapp', 'job', 'start', '--name', $job, '--resource-group', 'copilot-test')
$record = @{
    jobName = $job
    executionName = $execution.name
    image = $image.image
    status = 'Pending'
}
$recordPath = Join-Path $PSScriptRoot '..\.state\identity-probe.json'
Write-JsonAtomic -Path $recordPath -Value $record
Write-Host "Probe execution: $($execution.name)"
$clock = [Diagnostics.Stopwatch]::StartNew()
while ($clock.Elapsed.TotalSeconds -lt 300) {
    $result = Invoke-AzureJson -Arguments @(
        'containerapp', 'job', 'execution', 'show', '--name', $job,
        '--resource-group', 'copilot-test', '--job-execution-name', $execution.name
    )
    $status = $result.properties.status
    if ($status -in @('Succeeded', 'Failed', 'Stopped')) {
        $record.status = $status
        Write-JsonAtomic -Path $recordPath -Value $record
        if ($status -ne 'Succeeded') {
            throw "Managed-identity probe $status. Inspect this exact execution; do not assume registry pull or Voice Live authorization worked."
        }
        Write-Host 'Managed identity, private image startup and the exact Voice Live session configuration succeeded.'
        return
    }
    Start-Sleep -Seconds 5
}
throw "Probe status remained incomplete. Execution details are saved in $recordPath; inspect that execution before starting another."
