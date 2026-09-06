Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-AzureJson {
    param([Parameter(Mandatory)][string[]]$Arguments)
    $output = & az @Arguments --only-show-errors --output json
    if ($LASTEXITCODE -ne 0) {
        throw "Azure CLI failed: $($Arguments[0..([Math]::Min(1, $Arguments.Length - 1))] -join ' ')"
    }
    if ($output) {
        return ($output -join "`n" | ConvertFrom-Json -AsHashtable)
    }
}

function Assert-AzureContext {
    param([Parameter(Mandatory)][guid]$TenantId, [guid]$SubscriptionId = [guid]::Empty)
    $account = Invoke-AzureJson -Arguments @('account', 'show')
    if ($account.tenantId -ne $TenantId.ToString()) {
        throw 'The active Azure CLI tenant differs from the explicitly selected tenant. Use az login first.'
    }
    if ($SubscriptionId -ne [guid]::Empty -and $account.id -ne $SubscriptionId.ToString()) {
        throw 'The active Azure CLI subscription differs from the explicitly selected subscription.'
    }
    return $account
}

function Write-JsonAtomic {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)]$Value)
    $fullPath = [IO.Path]::GetFullPath($Path)
    $parent = Split-Path -Parent $fullPath
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    $temporary = Join-Path $parent ([IO.Path]::GetRandomFileName())
    try {
        $Value | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding utf8NoBOM
        [IO.File]::Move($temporary, $fullPath, $true)
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary
        }
    }
}

function Invoke-GraphJson {
    param(
        [Parameter(Mandatory)][ValidateSet('GET', 'POST', 'PATCH')][string]$Method,
        [Parameter(Mandatory)][string]$Path,
        [hashtable]$Body
    )
    if (-not $Path.StartsWith('/')) {
        throw 'Graph paths must be relative to the trusted v1.0 endpoint.'
    }
    $arguments = @('rest', '--method', $Method, '--url', "https://graph.microsoft.com/v1.0$Path")
    if (-not $Body) {
        return Invoke-AzureJson -Arguments $arguments
    }
    $temporary = [IO.Path]::GetTempFileName()
    try {
        $Body | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding utf8NoBOM
        return Invoke-AzureJson -Arguments ($arguments + @('--body', "@$temporary", '--headers', 'Content-Type=application/json'))
    }
    finally {
        Remove-Item -LiteralPath $temporary
    }
}

function Assert-OwnedApplication {
    param([Parameter(Mandatory)][hashtable]$Application)
    if ('embedded-recorder' -notin $Application.tags) {
        throw 'Refusing to modify an application without the embedded-recorder ownership tag.'
    }
}
