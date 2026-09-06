#Requires -Version 7.2
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$scripts = Join-Path $PSScriptRoot '..\scripts'
$tokens = $null
$parseErrors = $null
foreach ($file in Get-ChildItem -LiteralPath $scripts -Filter *.ps1) {
    $null = [Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) {
        throw "Syntax errors in $($file.Name): $($parseErrors.Message -join '; ')"
    }
}
. (Join-Path $scripts 'Common.ps1')
$directory = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
$path = Join-Path $directory 'state.json'
$identityPath = Join-Path $directory 'identity.json'
$foundationPath = Join-Path $directory 'foundation.json'
$configurationPath = Join-Path $directory 'proxy-configuration.json'
try {
    Write-JsonAtomic -Path $path -Value @{ version = 1; values = @('a', 'b') }
    $actual = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ($actual.version -ne 1 -or $actual.values.Count -ne 2) {
        throw 'Atomic JSON round-trip failed.'
    }
    Write-JsonAtomic -Path $path -Value @{ version = 2 }
    $actual = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ($actual.version -ne 2) {
        throw 'Atomic JSON replacement failed.'
    }
    Assert-OwnedApplication -Application @{ tags = @('embedded-recorder') }
    $rejected = $false
    try {
        Assert-OwnedApplication -Application @{ tags = @('someone-else') }
    }
    catch {
        if ($_.Exception.Message -notlike 'Refusing to modify*') { throw }
        $rejected = $true
    }
    if (-not $rejected) {
        throw 'Unowned application was accepted.'
    }
    function Invoke-AzureJson {
        param([string[]]$Arguments)
        return @{
            tenantId = '11111111-1111-1111-1111-111111111111'
            id = '22222222-2222-2222-2222-222222222222'
        }
    }
    $context = Assert-AzureContext -TenantId '11111111-1111-1111-1111-111111111111'
    if ($context.id -ne '22222222-2222-2222-2222-222222222222') {
        throw 'Tenant-only context validation failed.'
    }
    $rejected = $false
    try {
        $null = Assert-AzureContext -TenantId '11111111-1111-1111-1111-111111111111' `
            -SubscriptionId '33333333-3333-3333-3333-333333333333'
    }
    catch {
        if ($_.Exception.Message -notlike 'The active Azure CLI subscription*') { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw 'Wrong subscription was accepted.' }
    Write-JsonAtomic -Path $identityPath -Value @{
        tenantId = '11111111-1111-1111-1111-111111111111'
        apiClientId = '22222222-2222-2222-2222-222222222222'
        deviceClientId = '33333333-3333-3333-3333-333333333333'
    }
    Write-JsonAtomic -Path $foundationPath -Value @{
        tenantId = '11111111-1111-1111-1111-111111111111'
        resourceGroup = 'copilot-test'
        outputs = @{
            proxyIdentityClientId = @{ value = '44444444-4444-4444-4444-444444444444' }
            voiceLiveEndpoint = @{ value = 'https://example.services.ai.azure.com' }
            voiceLiveModel = @{ value = 'gpt-realtime-2' }
            voiceLiveProfile = @{ value = 'native' }
        }
    }
    & (Join-Path $scripts 'New-ProxyConfiguration.ps1') `
        -AllowedUserOid '55555555-5555-5555-5555-555555555555' `
        -IdentityStatePath $identityPath -FoundationStatePath $foundationPath `
        -OutputPath $configurationPath
    $generated = Get-Content -LiteralPath $configurationPath -Raw | ConvertFrom-Json -AsHashtable
    if (-not $generated.authorizedUserConfigured -or
        $generated.environment.PUBLIC_CLIENT_A_ID -ne '33333333-3333-3333-3333-333333333333' -or
        $generated.environment.ALLOWED_USER_OID -ne '55555555-5555-5555-5555-555555555555' -or
        $generated.environment.MAX_SESSION_SECONDS -cne '900') {
        throw 'Proxy configuration generation did not match the application contract.'
    }
}
finally {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    if (Test-Path -LiteralPath $identityPath) { Remove-Item -LiteralPath $identityPath }
    if (Test-Path -LiteralPath $foundationPath) { Remove-Item -LiteralPath $foundationPath }
    if (Test-Path -LiteralPath $configurationPath) { Remove-Item -LiteralPath $configurationPath }
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory }
}
Write-Output 'Infrastructure script tests passed.'
