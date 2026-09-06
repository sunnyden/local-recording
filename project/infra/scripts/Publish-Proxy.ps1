#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$FoundationStatePath = (Join-Path $PSScriptRoot '..\.state\foundation.json'),
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9][a-z0-9._-]{0,63}$')][string]$ImageTag,
    [ValidatePattern('^[a-zA-Z0-9]+$')][string]$CompletedRunId,
    [switch]$Apply
)
. (Join-Path $PSScriptRoot 'Common.ps1')
$state = Get-Content -LiteralPath $FoundationStatePath -Raw | ConvertFrom-Json -AsHashtable
$null = Assert-AzureContext -TenantId $state.tenantId -SubscriptionId $state.subscriptionId
if ($state.resourceGroup -ne 'copilot-test') {
    throw 'Foundation state must refer to copilot-test.'
}
$registry = $state.outputs.registryName.value
$image = "recorder-proxy:$ImageTag"
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\proxy'))
if (-not (Test-Path -LiteralPath (Join-Path $source 'Dockerfile'))) {
    throw 'Proxy Dockerfile is missing.'
}
if (-not (Test-Path -LiteralPath (Join-Path $source '.dockerignore'))) {
    throw 'A .dockerignore file is required before uploading the build context.'
}
Write-Host "Build context: $source"
Write-Host "Private registry image: $registry/$image"
if ($CompletedRunId) {
    Write-Host "Recovering completed remote run $CompletedRunId; no new source upload or build."
} else {
    Write-Host 'This uploads the proxy source to your Azure registry build service. Build and registry storage charges can apply.'
}
if (-not $Apply) {
    Write-Host 'Preview only. Use -Apply after reviewing the Docker build context.'
    return
}
if ($CompletedRunId) {
    $run = Invoke-AzureJson -Arguments @('acr', 'task', 'show-run', '--registry', $registry, '--run-id', $CompletedRunId)
} else {
    $run = Invoke-AzureJson -Arguments @(
        'acr', 'build', '--registry', $registry, '--image', $image,
        '--file', (Join-Path $source 'Dockerfile'), $source, '--no-logs'
    )
}
if ($run.status -ne 'Succeeded') {
    throw 'The remote build has not succeeded; no image state will be recorded. Inspect the existing run before rebuilding.'
}
$outputs = @($run.outputImages | Where-Object {
    $_.registry -eq $state.outputs.registryServer.value -and
    $_.repository -eq 'recorder-proxy' -and $_.tag -eq $ImageTag
})
if ($outputs.Count -ne 1) {
    throw 'The completed run does not identify exactly the requested private image.'
}
$digest = $outputs[0].digest
if ($digest -notmatch '^sha256:[a-f0-9]{64}$') {
    throw 'Registry did not return a valid immutable image digest.'
}
Write-JsonAtomic -Path (Join-Path $PSScriptRoot '..\.state\image.json') -Value @{
    schemaVersion = 1
    registryServer = $state.outputs.registryServer.value
    tag = $ImageTag
    buildRunId = $run.runId
    image = "$($state.outputs.registryServer.value)/recorder-proxy@$digest"
}
Write-Host 'Build complete. Immutable image reference saved to .state\image.json.'
