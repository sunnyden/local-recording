#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$IdentityStatePath = (Join-Path $PSScriptRoot '..\.state\identity.json'),
    [string]$FoundationStatePath = (Join-Path $PSScriptRoot '..\.state\foundation.json'),
    [switch]$Apply
)
. (Join-Path $PSScriptRoot 'Common.ps1')
$identity = Get-Content -LiteralPath $IdentityStatePath -Raw | ConvertFrom-Json -AsHashtable
$foundation = Get-Content -LiteralPath $FoundationStatePath -Raw | ConvertFrom-Json -AsHashtable
$null = Assert-AzureContext -TenantId $identity.tenantId -SubscriptionId $foundation.subscriptionId
if ($identity.tenantId -ne $foundation.tenantId -or $foundation.resourceGroup -ne 'copilot-test') {
    throw 'Identity and foundation must belong to the same copilot-test tenant.'
}
$application = Invoke-GraphJson -Method GET -Path "/applications/$($identity.apiObjectId)"
Assert-OwnedApplication -Application $application
if ($application.appId -ne $identity.apiClientId) { throw 'API B ID mismatch.' }
$managed = Invoke-AzureJson -Arguments @('identity', 'show', '--ids', $foundation.outputs.proxyIdentityId.value)
if ($managed.tenantId -ne $identity.tenantId) { throw 'Managed identity must share API B home tenant.' }
$credential = @{
    name = 'recorder-runtime-obo'
    issuer = "https://login.microsoftonline.com/$($identity.tenantId)/v2.0"
    subject = $managed.principalId
    audiences = @('api://AzureADTokenExchange')
    description = 'Runtime managed identity authenticates API B for delegated Graph OBO.'
}
Write-Host 'Configure API B with runtime managed-identity federation and delegated Graph Files.ReadWrite.'
Write-Host 'No secret, certificate, app-only Graph permission or tenant-wide admin consent is created.'
Write-Host 'This does not prove consumer OBO works; a live delegated exchange is still required.'
if (-not $Apply) { return }
$credentials = Invoke-GraphJson -Method GET -Path "/applications/$($application.id)/federatedIdentityCredentials"
$existing = @($credentials.value | Where-Object { $_.name -eq $credential.name })
if ($existing.Count -eq 0) {
    $null = Invoke-GraphJson -Method POST -Path "/applications/$($application.id)/federatedIdentityCredentials" -Body $credential
} elseif ($existing.Count -ne 1 -or $existing[0].subject -ne $credential.subject -or
          $existing[0].issuer -ne $credential.issuer -or
          (@($existing[0].audiences) -join ',') -ne 'api://AzureADTokenExchange') {
    throw 'Existing OBO federation differs from the expected runtime identity.'
}
$graphId = '00000003-0000-0000-c000-000000000000'
$graphPrincipals = @(Invoke-AzureJson -Arguments @('ad', 'sp', 'list', '--filter', "appId eq '$graphId'"))
if ($graphPrincipals.Count -ne 1) { throw 'Cannot resolve Graph service principal.' }
$permissions = @($graphPrincipals[0].oauth2PermissionScopes | Where-Object { $_.value -eq 'Files.ReadWrite' -and $_.isEnabled })
if ($permissions.Count -ne 1) { throw 'Cannot resolve Files.ReadWrite delegated scope.' }
$required = @($application.requiredResourceAccess)
$graphEntry = @($required | Where-Object { $_.resourceAppId -eq $graphId })
if ($graphEntry.Count -gt 1) { throw 'Duplicate Graph permission entries must be reconciled.' }
if ($graphEntry.Count -eq 0) {
    $required += @{ resourceAppId = $graphId; resourceAccess = @(@{ id = $permissions[0].id; type = 'Scope' }) }
} elseif (-not @($graphEntry[0].resourceAccess | Where-Object { $_.id -eq $permissions[0].id -and $_.type -eq 'Scope' }).Count) {
    $graphEntry[0].resourceAccess = @($graphEntry[0].resourceAccess) + @{ id = $permissions[0].id; type = 'Scope' }
}
$knownClients = @($application.api.knownClientApplications)
if ($identity.deviceClientId -notin $knownClients) { $knownClients += $identity.deviceClientId }
$null = Invoke-GraphJson -Method PATCH -Path "/applications/$($application.id)" -Body @{
    requiredResourceAccess = $required
    api = @{ knownClientApplications = $knownClients }
}
Write-Host 'OBO registration configured. Fresh combined user consent and live proof remain required.'
