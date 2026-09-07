# Cloud setup and remaining enrollment

The owner has since completed both OneDrive and proxy authorization and
confirmed working device conversation. Some diagnostics below describe the
earlier bring-up sequence. The current extension is documented in
[`intelligent-recording.md`](intelligent-recording.md): personal-account
secretless OBO is proven, and Fast Transcription uses the same existing
Foundry account's Cognitive Services endpoint, not a new Speech resource.

## Created configuration

The following resources were created in `copilot-test`, East US 2, in the
active Visual Studio Enterprise subscription. Deployment state and full
resource IDs are in ignored `infra\.state\` JSON files.

| Resource | Name |
| --- | --- |
| Foundry resource | `recorder-ai-das7i6efgflpa` |
| Foundry project | `recorder` |
| Container Apps Consumption environment | `recorder-environment` |
| Private Basic ACR | `recorderdas7i6efgflpa` |
| Proxy user-assigned managed identity | `recorder-proxy` |
| Manual private identity-validation job | `recorder-identity-probe` |

Tenant-level app registrations were also created:

| Registration | Public application/client ID |
| --- | --- |
| A: `embedded-recorder-device` | `bdbb7442-7892-471e-9ee4-95d4e9c02a37` |
| B: `embedded-recorder-api` | `30f56876-0c90-48b8-aff0-654b5000fd43` |

These IDs are public identifiers, not credentials. Both registrations support
personal and organizational Microsoft accounts. A is a public client, with
delegated Graph `Files.ReadWrite` and
`api://30f56876-0c90-48b8-aff0-654b5000fd43/access_as_user`. No client secrets,
application permissions or tenant-wide consent grants were created.

Microsoft's consumer device-authorization endpoint accepted independent
requests for both delegated scopes. The diagnostic did not display or redeem
the returned device codes, so this is not evidence of completed user consent
or acquired access tokens.

## Voice Live

The exact native managed model `gpt-realtime-2` accepted a keyless Voice Live
session at:

```text
wss://recorder-ai-das7i6efgflpa.services.ai.azure.com/voice-live/realtime
    ?api-version=2026-07-15&model=gpt-realtime-2
```

The actual URL must be on one line. Do not append an API key.

The service confirmed 16000 Hz PCM16 input, `server_echo_cancellation`, and
`azure_semantic_vad_multilingual` with interruption enabled. The proxy uses
`create_response:false`: service VAD detects a completed turn, and the proxy
creates the response after outstanding playback clearing/truncation completes.
The service reports the canonical native name `gpt-realtime-2-global-standard`;
the adapter permits that specific alias, not another model.

An attempted BYOM ARM validation found no remaining East US 2 GlobalStandard
quota for the dedicated `gpt-realtime-2` deployment (10 used of 10). No
dedicated model was created and no existing deployment was changed. The
native Voice Live path uses the same requested model without allocating that
separate deployment.

Run `infra\scripts\Test-VoiceLive.ps1` using a permitted developer identity to
repeat the bounded, no-audio configuration diagnostic.

The actual proxy session also completed paced, locally synthesized 16 kHz
microphone input and real-time simulated speaker playback through the live
service. Normal playback completed 59,200 output samples without error; a
second synthetic phrase interrupted the first response, producing one
playback clear and two output epochs without error. These tests used developer
credentials and a simulated device, not physical-board acoustics or consumer
authentication.

## Private image and managed identity

The current firmware-paced image is ACR build `ch2`, tagged
`prototype-20260906-2`, with its immutable digest recorded in
`infra\.state\image.json`. The earlier build was `ch1`.
The local CLI log streamer encountered a Windows encoding error, but the remote
build succeeded; its digest was recovered without starting a duplicate build.
Future builds suppress the Unicode progress stream.

A manual Container Apps job ran the updated immutable image with the actual
proxy managed identity and completed successfully
(`recorder-identity-probe-3yuct9d`). This verifies private-registry pulling,
Linux container startup and the exact Voice Live configuration with production
managed identity/RBAC. The job has no public ingress or schedule, no retries,
and a 90-second execution limit. It sends no user audio.

The public proxy Container App was deployed after explicit consumer enrollment.
Physical-device authorization and conversation/acoustic behavior remain
separate acceptance gates.

### GitHub production deployment

The consumer API-B user was explicitly enrolled and validated. GitHub Actions
now deploys the proxy at:

```text
wss://recorder-proxy.grayplant-a3ab794a.eastus2.azurecontainerapps.io/v1/voice
```

Successful run:
`https://github.com/sunnyden/local-recording/actions/runs/34045328790`

That run authenticated with immutable-ID GitHub OIDC, built/pushed an immutable
private ACR image, passed the critical/high vulnerability gate, deployed the
0-1 replica Container App, and passed `/readyz`. The image digest was
`sha256:25c13a439c10f6b5a0a84cb5ec7c7d87f8698e12c665a4b40253bf2b95959525`.

## Not automatically enrolled

The owner completed personal-account OneDrive consent through the device's
BLE-assisted flow and confirmed that all recordings uploaded successfully.
The ESP stores and refreshes that Graph authorization using the explicitly
approved ordinary-NVS PoC policy. No client secret or eFuse key was created.

No proxy consumer identity was guessed from the active Azure work account.
The owner completed separate explicit authorized-user enrollment before
public deployment; Graph authorization is not proxy API authorization.

The optional `infra\scripts\enroll_user.py` operator utility can obtain a
cryptographically verified consumer `oid` through an explicit browser device
flow. It prints no OAuth token,
requests no offline access, saves no credentials and does not configure the
public proxy automatically.

The cloud scripts do not provision Wi-Fi passwords or user refresh tokens.
Those were entered/authorized through the hidden-input companion and Microsoft
browser flow after owner approval. Do not use a first-connection-wins enrollment
scheme for a publicly reachable proxy.

## Cost and lifecycle

The Foundry account has local/API-key authentication disabled. The proxy
managed identity has resource-scoped Cognitive Services User and Foundry User
roles and registry-scoped AcrPull. The environment contains no dedicated
workload profile and no Log Analytics workspace was added.

Basic ACR remains billable even without running containers. Active proxy
sessions and Voice Live usage are also billable. A minimum-zero replica
setting does not eliminate these other charges.
The manual identity job has no running replica between explicitly requested
executions; executing it incurs Consumption usage.

Deployment scripts do not delete resources. Review the exact resource
inventory before cleanup; do not delete the group if it later contains
unrelated resources. The two app registrations are tenant-level and are not
removed by deleting `copilot-test`.
