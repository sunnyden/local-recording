#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][guid]$TenantId,
    [ValidatePattern('^[A-Za-z0-9-]{3,40}$')][string]$NamePrefix = 'embedded-recorder',
    [string]$StatePath = (Join-Path $PSScriptRoot '..\.state\identity.json'),
    [switch]$Apply
)
. (Join-Path $PSScriptRoot 'Common.ps1')
$null = Assert-AzureContext -TenantId $TenantId

$state = if (Test-Path -LiteralPath $StatePath) {
    Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json -AsHashtable
} else {
    @{
        schemaVersion = 1
        tenantId = $TenantId.ToString()
        namePrefix = $NamePrefix
        scopeId = [guid]::NewGuid().ToString()
    }
}
if ($state.tenantId -ne $TenantId.ToString() -or $state.namePrefix -ne $NamePrefix) {
    throw 'Identity state belongs to a different tenant or name prefix.'
}

Write-Host "Tenant: $TenantId"
Write-Host "Public client A: $NamePrefix-device; resource API B: $NamePrefix-api"
Write-Host 'Permissions: Graph Files.ReadWrite and API B access_as_user; personal and organizational accounts.'
Write-Host 'No client secrets, admin-consent grants, or application permissions will be created.'
if (-not $Apply) {
    Write-Host 'Preview only. Rerun with -Apply to register/update the explicitly owned applications.'
    return
}
Write-JsonAtomic -Path $StatePath -Value $state

function Get-OrCreateApplication {
    param([string]$StateKey, [string]$DisplayName, [hashtable]$Properties)
    if ($state.ContainsKey($StateKey)) {
        $id = [guid]$state[$StateKey]
        $application = Invoke-GraphJson -Method GET -Path "/applications/$id"
        Assert-OwnedApplication -Application $application
        return $application
    }
    $matches = Invoke-AzureJson -Arguments @('ad', 'app', 'list', '--filter', "displayName eq '$DisplayName'")
    if (@($matches).Count -gt 0) {
        throw "An application named $DisplayName already exists without a recorded ownership ID. Reconcile the state file explicitly; it will not be adopted or duplicated."
    }
    $application = Invoke-GraphJson -Method POST -Path '/applications' -Body ($Properties + @{
        displayName = $DisplayName
        tags = @('embedded-recorder')
    })
    $state[$StateKey] = $application.id
    Write-JsonAtomic -Path $StatePath -Value $state
    return $application
}

$api = Get-OrCreateApplication -StateKey 'apiObjectId' -DisplayName "$NamePrefix-api" -Properties @{
    signInAudience = 'AzureADandPersonalMicrosoftAccount'
    api = @{ requestedAccessTokenVersion = 2 }
}
$apiUri = "api://$($api.appId)"
$null = Invoke-GraphJson -Method PATCH -Path "/applications/$($api.id)" -Body @{
    signInAudience = 'AzureADandPersonalMicrosoftAccount'
    identifierUris = @($apiUri)
    api = @{
        requestedAccessTokenVersion = 2
        oauth2PermissionScopes = @(@{
            id = $state.scopeId
            value = 'access_as_user'
            type = 'User'
            isEnabled = $true
            adminConsentDisplayName = 'Use the recorder voice proxy'
            adminConsentDescription = 'Allow the recorder to use the voice proxy on behalf of the signed-in user.'
            userConsentDisplayName = 'Use the recorder voice proxy'
            userConsentDescription = 'Allow this recorder to start voice conversations as you.'
        })
    }
}

$graphAppId = '00000003-0000-0000-c000-000000000000'
$graphPrincipals = @(Invoke-AzureJson -Arguments @('ad', 'sp', 'list', '--filter', "appId eq '$graphAppId'"))
if ($graphPrincipals.Count -ne 1) {
    throw 'Cannot resolve the Microsoft Graph service principal in the selected tenant.'
}
$fileScopes = @($graphPrincipals[0].oauth2PermissionScopes | Where-Object { $_.value -eq 'Files.ReadWrite' -and $_.isEnabled })
if ($fileScopes.Count -ne 1) {
    throw 'Cannot resolve the delegated Graph Files.ReadWrite scope.'
}
$client = Get-OrCreateApplication -StateKey 'clientObjectId' -DisplayName "$NamePrefix-device" -Properties @{
    signInAudience = 'AzureADandPersonalMicrosoftAccount'
    isFallbackPublicClient = $true
}
$null = Invoke-GraphJson -Method PATCH -Path "/applications/$($client.id)" -Body @{
    signInAudience = 'AzureADandPersonalMicrosoftAccount'
    isFallbackPublicClient = $true
    api = @{ requestedAccessTokenVersion = 2 }
    requiredResourceAccess = @(
        @{ resourceAppId = $graphAppId; resourceAccess = @(@{ id = $fileScopes[0].id; type = 'Scope' }) }
        @{ resourceAppId = $api.appId; resourceAccess = @(@{ id = $state.scopeId; type = 'Scope' }) }
    )
}
foreach ($application in @($api, $client)) {
    $principals = @(Invoke-AzureJson -Arguments @('ad', 'sp', 'list', '--filter', "appId eq '$($application.appId)'"))
    if ($principals.Count -eq 0) {
        $null = Invoke-GraphJson -Method POST -Path '/servicePrincipals' -Body @{ appId = $application.appId }
    }
}
$state.apiClientId = $api.appId
$state.deviceClientId = $client.appId
$state.proxyScope = "$apiUri/access_as_user"
$state.graphScope = 'https://graph.microsoft.com/Files.ReadWrite'
$state.authority = 'https://login.microsoftonline.com/consumers'
Write-JsonAtomic -Path $StatePath -Value $state
Write-Host "Registration complete. Nonsecret IDs and scopes saved to $StatePath."
Write-Host 'User consent is still required. The two APIs use separate access tokens.'
