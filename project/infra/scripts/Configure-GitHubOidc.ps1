#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][guid]$TenantId,
    [Parameter(Mandatory)][guid]$SubscriptionId,
    [string]$Repository = 'sunnyden/local-recording',
    [string]$Environment = 'production',
    [string]$ResourceGroup = 'copilot-test',
    [string]$RegistryName = 'recorderdas7i6efgflpa',
    [string]$DisplayName = 'local-recording-github-deploy',
    [switch]$Apply
)
. (Join-Path $PSScriptRoot 'Common.ps1')
$null = Assert-AzureContext -TenantId $TenantId -SubscriptionId $SubscriptionId
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' -or
    $Environment -notmatch '^[A-Za-z0-9_.-]+$') {
    throw 'Repository or environment contains unsupported characters.'
}
$repositoryParts = $Repository.Split('/')
$metadataText = & gh api "repos/$Repository" --jq '{owner:.owner.login,owner_id:.owner.id,name:.name,repo_id:.id}'
if ($LASTEXITCODE -ne 0) { throw 'Unable to resolve immutable GitHub repository identity.' }
$metadata = $metadataText | ConvertFrom-Json -AsHashtable
if ($metadata.owner -ne $repositoryParts[0] -or $metadata.name -ne $repositoryParts[1] -or
    $metadata.owner_id -notmatch '^[0-9]+$' -or $metadata.repo_id -notmatch '^[0-9]+$') {
    throw 'GitHub returned unexpected repository identity metadata.'
}
$subject = "repo:$($metadata.owner)@$($metadata.owner_id)/$($metadata.name)@$($metadata.repo_id)`:environment:$Environment"
$groupId = "/subscriptions/$SubscriptionId/resourceGroups/$ResourceGroup"
$registryId = "$groupId/providers/Microsoft.ContainerRegistry/registries/$RegistryName"
Write-Host "GitHub OIDC subject: $subject"
Write-Host "Azure scopes: Contributor on $groupId; AcrPush on $registryId"
Write-Host 'No password, certificate, client secret, or repository secret will be created.'
if (-not $Apply) {
    Write-Host 'Preview only. Rerun with -Apply after reviewing these scopes.'
    return
}

$apps = @(Invoke-AzureJson -Arguments @('ad', 'app', 'list', '--filter', "displayName eq '$DisplayName'"))
if ($apps.Count -gt 1) { throw 'Multiple deployment applications use this display name.' }
if ($apps.Count -eq 0) {
    $app = Invoke-GraphJson -Method POST -Path '/applications' -Body @{
        displayName = $DisplayName
        signInAudience = 'AzureADMyOrg'
        tags = @('embedded-recorder')
    }
} else {
    $app = $apps[0]
    if ('embedded-recorder' -notin $app.tags) {
        throw 'Refusing to adopt an application without the embedded-recorder ownership tag.'
    }
}
$principals = @(Invoke-AzureJson -Arguments @('ad', 'sp', 'list', '--filter', "appId eq '$($app.appId)'"))
if ($principals.Count -eq 0) {
    $principal = Invoke-AzureJson -Arguments @('ad', 'sp', 'create', '--id', $app.appId)
} elseif ($principals.Count -eq 1) {
    $principal = $principals[0]
} else {
    throw 'Multiple service principals resolve to the deployment application.'
}

$credentialResponse = Invoke-GraphJson -Method GET -Path "/applications/$($app.id)/federatedIdentityCredentials"
$credentialName = 'github-production-immutable'
$matching = @($credentialResponse.value | Where-Object { $_.name -eq $credentialName })
$body = @{
    name = $credentialName
    issuer = 'https://token.actions.githubusercontent.com'
    subject = $subject
    audiences = @('api://AzureADTokenExchange')
    description = 'Immutable GitHub repository identity for the production environment'
}
if ($matching.Count -eq 0) {
    $null = Invoke-GraphJson -Method POST -Path "/applications/$($app.id)/federatedIdentityCredentials" -Body $body
} elseif ($matching.Count -gt 1 -or $matching[0].subject -ne $subject -or
          $matching[0].issuer -ne $body.issuer) {
    throw 'Existing immutable production federated credential does not match the requested trust.'
}

foreach ($assignment in @(
    @{ role = 'Contributor'; scope = $groupId },
    @{ role = 'AcrPush'; scope = $registryId }
)) {
    $existing = @(Invoke-AzureJson -Arguments @(
        'role', 'assignment', 'list', '--assignee-object-id', $principal.id,
        '--scope', $assignment.scope, '--query', "[?roleDefinitionName=='$($assignment.role)']"
    ))
    if ($existing.Count -eq 0) {
        $null = Invoke-AzureJson -Arguments @(
            'role', 'assignment', 'create', '--assignee-object-id', $principal.id,
            '--assignee-principal-type', 'ServicePrincipal', '--role', $assignment.role,
            '--scope', $assignment.scope
        )
    }
}
[pscustomobject]@{
    azureClientId = $app.appId
    tenantId = $TenantId
    subscriptionId = $SubscriptionId
    subject = $subject
    servicePrincipalObjectId = $principal.id
}
