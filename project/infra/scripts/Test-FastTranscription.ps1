#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$FoundationStatePath = (Join-Path $PSScriptRoot '..\.state\foundation.json'),
    [switch]$Apply
)
. (Join-Path $PSScriptRoot 'Common.ps1')
$state = Get-Content -LiteralPath $FoundationStatePath -Raw | ConvertFrom-Json -AsHashtable
$null = Assert-AzureContext -TenantId $state.tenantId -SubscriptionId $state.subscriptionId
if ($state.resourceGroup -ne 'copilot-test' -or
    $state.outputs.voiceLiveEndpoint.value -notmatch '^https://([a-z0-9-]+)\.services\.ai\.azure\.com/?$') {
    throw 'The Speech diagnostic requires the owned copilot-test Foundry foundation.'
}
$speechEndpoint = "https://$($Matches[1]).cognitiveservices.azure.com"
Write-Host 'Reuse the existing Foundry account and proxy managed identity for one second of synthetic silence.'
Write-Host 'This manual diagnostic has no ingress, schedule, retries, user audio or durable tokens.'
Write-Host 'The diagnostic execution is limited to 180 seconds. Consumption/Speech charges may apply.'
if (-not $Apply) {
    Write-Host 'Preview only. No resources or executions changed.'
    return
}
$proxy = Invoke-AzureJson -Arguments @(
    'containerapp', 'show', '--resource-group', 'copilot-test', '--name', "$($state.namePrefix)-proxy"
)
if ($proxy.tags.application -ne 'embedded-recorder') {
    throw 'Refusing to use an unowned proxy resource.'
}
$containers = @($proxy.properties.template.containers | Where-Object { $_.name -eq 'proxy' })
if ($containers.Count -ne 1 -or $containers[0].image -notmatch
    "^$([regex]::Escape($state.outputs.registryServer.value))/recorder-proxy@sha256:[a-f0-9]{64}$") {
    throw 'The active proxy must use a digest-pinned image in the selected private registry.'
}
$image = $containers[0].image
$null = Invoke-AzureJson -Arguments @(
    'deployment', 'group', 'create', '--resource-group', 'copilot-test', '--name', 'recorder-speech-probe',
    '--template-file', (Join-Path $PSScriptRoot '..\bicep\speech-probe.bicep'), '--parameters',
    "location=$($state.location)", "environmentId=$($state.outputs.environmentId.value)",
    "identityId=$($state.outputs.proxyIdentityId.value)",
    "identityClientId=$($state.outputs.proxyIdentityClientId.value)",
    "registryServer=$($state.outputs.registryServer.value)", "image=$image", "speechEndpoint=$speechEndpoint"
)
$execution = Invoke-AzureJson -Arguments @(
    'containerapp', 'job', 'start', '--resource-group', 'copilot-test', '--name', 'recorder-speech-probe'
)
$record = @{ jobName = 'recorder-speech-probe'; executionName = $execution.name; image = $image; status = 'Pending' }
$recordPath = Join-Path $PSScriptRoot '..\.state\speech-probe.json'
Write-JsonAtomic -Path $recordPath -Value $record
Write-Host "Manual Speech diagnostic started: $($execution.name)"
$clock = [Diagnostics.Stopwatch]::StartNew()
while ($clock.Elapsed.TotalSeconds -lt 300) {
    $result = Invoke-AzureJson -Arguments @(
        'containerapp', 'job', 'execution', 'show', '--resource-group', 'copilot-test',
        '--name', 'recorder-speech-probe', '--job-execution-name', $execution.name
    )
    $status = $result.properties.status
    if ($status -in @('Succeeded', 'Failed', 'Stopped')) {
        $record.status = $status
        Write-JsonAtomic -Path $recordPath -Value $record
        if ($status -ne 'Succeeded') { throw "Speech diagnostic $status. Inspect the saved execution before retrying." }
        Write-Host 'Managed-identity Fast Transcription accepted synthetic silence. This does not measure spoken accuracy.'
        return
    }
    Start-Sleep -Seconds 5
}
throw "Diagnostic status is uncertain. Inspect the execution in $recordPath before starting another."
