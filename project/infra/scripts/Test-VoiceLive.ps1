#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$FoundationStatePath = (Join-Path $PSScriptRoot '..\.state\foundation.json'),
    [ValidatePattern('^\d{4}-\d{2}-\d{2}(-preview)?$')][string]$ApiVersion = '2026-07-15',
    [ValidateRange(5, 120)][int]$TimeoutSeconds = 30
)
. (Join-Path $PSScriptRoot 'Common.ps1')
$state = Get-Content -LiteralPath $FoundationStatePath -Raw | ConvertFrom-Json -AsHashtable
$null = Assert-AzureContext -TenantId $state.tenantId -SubscriptionId $state.subscriptionId
$endpoint = [uri]$state.outputs.voiceLiveEndpoint.value
if ($endpoint.Scheme -ne 'https' -or $endpoint.UserInfo -or
    -not $endpoint.Host.EndsWith('.services.ai.azure.com') -or $endpoint.Query -or $endpoint.Fragment) {
    throw 'Foundation state does not contain a trusted Voice Live endpoint.'
}
$model = [uri]::EscapeDataString($state.outputs.voiceLiveModel.value)
$profile = $state.outputs.voiceLiveProfile.value
$uri = "wss://$($endpoint.Host)/voice-live/realtime?api-version=$ApiVersion&model=$model"
if ($profile -ne 'native') {
    if ($profile -ne 'byom-azure-openai-realtime') { throw 'Unknown Voice Live profile.' }
    $uri += "&profile=$profile"
}

$tokenResponse = Invoke-AzureJson -Arguments @('account', 'get-access-token', '--scope', 'https://ai.azure.com/.default')
$socket = [Net.WebSockets.ClientWebSocket]::new()
$socket.Options.SetRequestHeader('Authorization', "Bearer $($tokenResponse.accessToken)")
$tokenResponse = $null
$timeout = [Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds($TimeoutSeconds))

function Receive-Control {
    param([Net.WebSockets.ClientWebSocket]$Socket, [Threading.CancellationToken]$CancellationToken)
    $buffer = [byte[]]::new(16384)
    $message = [IO.MemoryStream]::new()
    try {
        do {
            $result = $Socket.ReceiveAsync([ArraySegment[byte]]::new($buffer), $CancellationToken).GetAwaiter().GetResult()
            if ($result.MessageType -eq [Net.WebSockets.WebSocketMessageType]::Close) {
                throw "Voice Live closed before configuration completed (status $($result.CloseStatus))."
            }
            if ($result.MessageType -ne [Net.WebSockets.WebSocketMessageType]::Text) {
                throw 'Voice Live returned an unexpected binary control message.'
            }
            if ($message.Length + $result.Count -gt 65536) {
                throw 'Voice Live control message exceeds the diagnostic limit.'
            }
            $message.Write($buffer, 0, $result.Count)
        } while (-not $result.EndOfMessage)
        return [Text.Encoding]::UTF8.GetString($message.ToArray()) | ConvertFrom-Json -AsHashtable
    }
    finally {
        $message.Dispose()
    }
}

try {
    $null = $socket.ConnectAsync([uri]$uri, $timeout.Token).GetAwaiter().GetResult()
    $created = Receive-Control -Socket $socket -CancellationToken $timeout.Token
    if ($created.type -eq 'error') {
        throw "Voice Live rejected the model/session. Provider error code: $($created.error.code)"
    }
    if ($created.type -ne 'session.created') {
        throw "Expected session.created; received $($created.type)."
    }
    $configuration = @{
        type = 'session.update'
        session = @{
            modalities = @('text', 'audio')
            input_audio_format = 'pcm16'
            input_audio_sampling_rate = 16000
            output_audio_format = 'pcm16'
            input_audio_echo_cancellation = @{ type = 'server_echo_cancellation' }
            turn_detection = @{
                type = 'azure_semantic_vad_multilingual'
                create_response = $false
                interrupt_response = $true
            }
            tools = @()
            tool_choice = 'none'
        }
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($configuration | ConvertTo-Json -Depth 8 -Compress))
    $null = $socket.SendAsync([ArraySegment[byte]]::new($bytes), [Net.WebSockets.WebSocketMessageType]::Text, $true, $timeout.Token).GetAwaiter().GetResult()
    $updated = Receive-Control -Socket $socket -CancellationToken $timeout.Token
    if ($updated.type -eq 'error') {
        throw "Voice Live rejected the required 16 kHz/AEC/VAD configuration. Provider error code: $($updated.error.code)"
    }
    if ($updated.type -ne 'session.updated') {
        throw "Expected session.updated; received $($updated.type)."
    }
    $session = $updated.session
    if ($session.input_audio_sampling_rate -ne 16000 -or
        $session.input_audio_echo_cancellation.type -ne 'server_echo_cancellation' -or
        $session.input_audio_echo_cancellation.reference_source -ne 'server' -or
        $session.input_audio_echo_cancellation.channels -ne 1 -or
        $session.turn_detection.type -ne 'azure_semantic_vad_multilingual' -or
        $session.turn_detection.create_response -ne $false -or
        $session.turn_detection.interrupt_response -ne $true -or
        $session.tools.Count -ne 0 -or $session.tool_choice -ne 'none') {
        throw 'Voice Live did not confirm the requested audio processing settings.'
    }
    $acceptedModels = @($state.outputs.voiceLiveModel.value)
    if ($profile -eq 'native' -and $state.outputs.voiceLiveModel.value -eq 'gpt-realtime-2') {
        $acceptedModels += 'gpt-realtime-2-global-standard'
    }
    if ($session.model -notin $acceptedModels) {
        throw 'Voice Live returned a different model from the explicitly requested model.'
    }
    [pscustomobject]@{
        endpoint = $endpoint.Host
        requestedModel = $state.outputs.voiceLiveModel.value
        servedModel = $session.model
        profile = $profile
        apiVersion = $ApiVersion
        inputSampleRate = $session.input_audio_sampling_rate
        echoCancellation = $session.input_audio_echo_cancellation.type
        turnDetection = $session.turn_detection.type
        result = 'Session configuration accepted; no user audio or response generation was sent.'
    }
}
finally {
    $socket.Abort()
    $socket.Dispose()
    $timeout.Dispose()
}
