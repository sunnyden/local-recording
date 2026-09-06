# Azure infrastructure

PowerShell 7.2+, Azure CLI with Bicep, a signed-in tenant, and permission to
create applications, resources and scoped role assignments are required.
The scripts never call `az login`, change your active subscription, grant
tenant-wide admin consent, or create client secrets.

All scripts default to preview/read-only behavior unless `-Apply` is supplied.
App registrations are tenant-level. All Azure resource-group services created
here use `copilot-test`. Deployment identity state is written to ignored
`.state\` files; keep these files to ensure reruns address the same app objects.
They contain IDs and endpoints, not tokens.

## Identity

```powershell
.\scripts\Register-Identity.ps1 -TenantId <tenant-guid>
.\scripts\Register-Identity.ps1 -TenantId <tenant-guid> -Apply
```

This creates public device client A and resource API B, both capable of personal
and organizational Microsoft accounts. A requests delegated Graph
`Files.ReadWrite` and B's `access_as_user`. The device uses the `consumers`
authority initially. No Graph application permissions are granted.

The caller still needs to sign in/consent on the device's verification page.
Declaring both APIs does not create a token valid for both: Graph and the proxy
have distinct audiences and access-token caches. Registering the apps also
does not populate the proxy's authorized-user allowlist.

The script refuses to adopt an unrelated application by display name. If a
creation was interrupted before its ID was saved, reconcile that exact object
with the state file instead of creating duplicates. Do not delete identity
state and rerun to "fix" a permission error.

## Foundation

```powershell
.\scripts\Test-Preflight.ps1 `
  -TenantId <tenant-guid> -SubscriptionId <subscription-guid> `
  -Location eastus2 -ModelVersion 2026-05-06

.\scripts\Deploy-Foundation.ps1 `
  -TenantId <tenant-guid> -SubscriptionId <subscription-guid> `
  -Location eastus2 -ModelVersion 2026-05-06 -ModelCapacity 1 -Apply
```

The BYOM path requires the exact `gpt-realtime-2` version, deployment SKU and
available quota. Catalog presence alone is insufficient; ARM validates quota
before deployment. This creates a new Foundry resource and project, model
deployment, private Basic ACR, user-assigned proxy identity, resource-scoped
roles, and a Consumption Container Apps environment.

Voice Live's native managed-model path does not allocate a dedicated Azure
OpenAI deployment:

```powershell
.\scripts\Deploy-Foundation.ps1 `
  -TenantId <tenant-guid> -SubscriptionId <subscription-guid> `
  -Location eastus2 -ModelVersion 2026-05-06 `
  -VoiceProfile native -NativeModelConfirmed -Apply
```

`-NativeModelConfirmed` is an explicit operator decision, not an automated
claim that the model is usable. Establish a real Voice Live session with the
exact model and requested VAD/AEC configuration before declaring AI ready.
Neither path automatically substitutes another model. On the native path,
`ModelVersion` records intent for preflight reporting; the managed service,
not an ARM model deployment, selects the served version.

The scripts fail if an existing `copilot-test` lacks their ownership tag.
Review the group rather than automatically retagging/adopting someone else's
resources. Deployments are incremental; changing from BYOM to native does not
delete an already-created model. Review obsolete resources explicitly.

## Proxy deployment and cost

`bicep\proxy.bicep` deploys an immutable private image with managed-identity ACR
pulls. It accepts only nonsecret application configuration. Configure the
device client ID, API B audience, exact authorized subject identity, and Voice
Live endpoint/profile. An empty or guessed user allowlist is not enrollment.

After the proxy's local checks pass and its Docker context is reviewed:

```powershell
.\scripts\Publish-Proxy.ps1 -ImageTag <unique-build-tag>
.\scripts\Publish-Proxy.ps1 -ImageTag <unique-build-tag> -Apply
.\scripts\New-ProxyConfiguration.ps1 -AllowedUserOid <consumer-oid-guid>
.\scripts\Deploy-Proxy.ps1 -ConfigurationPath <nonsecret-config.json>
.\scripts\Deploy-Proxy.ps1 -ConfigurationPath <nonsecret-config.json> -Apply
```

The configuration JSON contains `authorizedUserConfigured: true` and an
`environment` object whose names match `proxy\README.md`. Set that confirmation
only after configuring the real authorized subject; it is not an authentication
bypass. The script supplies the foundation's `AZURE_CLIENT_ID` automatically.
`Publish-Proxy.ps1` uploads only `proxy\` to your private ACR build service and
records a digest-pinned image. It does not push to a public registry.

The generator fills `PUBLIC_CLIENT_A_ID`, `API_B_CLIENT_ID`,
`ALLOWED_USER_OID`, `AZURE_CLIENT_ID`, `VOICELIVE_ENDPOINT`,
`VOICELIVE_MODEL`, `VOICELIVE_PROFILE`, `VOICELIVE_API_VERSION`, and
`MAX_SESSION_SECONDS` from the deployment state and your explicit enrollment.
Use the `oid` from a validated consumer token for API B, not the Azure CLI
work-account object ID or an unverified JWT. No user token belongs in this
configuration file. Startup/readiness probe `/readyz`; liveness probes
`/healthz`.

If you do not yet know that consumer-user `oid`, perform deliberate operator
enrollment from the repository root:

```powershell
.\project\proxy\.venv\Scripts\python.exe .\project\infra\scripts\enroll_user.py
```

This separate operator utility displays a Microsoft device code and requires
human sign-in. It requests a short-lived API B token, verifies its signature,
consumer issuer, audience, authorized client and scope using the proxy's real
validator, and outputs only the verified user ID. It does not request offline
access, persist tokens, change the allowlist, or enroll the first caller to
the public service. Do not run it unattended. The BLE companion still never
receives OAuth tokens; the device later obtains its own authorization.

The proxy has HTTPS/WSS ingress, one worker/active revision, zero minimum and
one maximum replica. The initial container budget is 0.25 vCPU and 0.5 GiB.
Measure actual consumption before changing these budgets.

The Foundry account disables local/API-key authentication. The proxy identity
has Cognitive Services User and Foundry User scoped to that resource, and
AcrPull scoped to its registry. No subscription-wide runtime role is granted.
Managed identity avoids an API key even for upstream WebSocket handshakes.

Scale-to-zero is not zero total cost: Basic ACR has a standing cost, active
containers and Voice Live consume usage, and any subsequently enabled logs
may be billable. No dedicated workload profile, NAT gateway, database or
logging workspace is provisioned. Budgets/alerts are not spending limits.

Existing long-lived WebSocket sessions can be terminated by maintenance,
revision changes or authentication expiry. The firmware must reconnect into
a new conversation and must not replay old microphone buffers.

## Local infrastructure checks

```powershell
.\tests\Test-Scripts.ps1
az bicep build --file .\bicep\base.bicep --stdout
az bicep build --file .\bicep\proxy.bicep --stdout
```

These checks do not create resources or authorize a user.

## GitHub deployment

The repository workflow and trust model are documented in
`..\docs\ci-cd.md`. `scripts\Configure-GitHubOidc.ps1` creates an idempotent
federated credential for the exact `production` environment subject and grants
only resource-group Contributor plus registry AcrPush. It creates no secret.

Production deployment uses `bicep\proxy.bicep` against the existing foundation;
it does not recreate Foundry, app registrations, managed identities, or the
Container Apps environment.

After registration, `scripts\Test-DeviceGrant.ps1 -Resource graph` (or `proxy`)
checks that the consumer endpoint accepts that API's device-grant request.
It intentionally does not display/redeem a device code or authorize a user.
`scripts\Test-VoiceLive.ps1` verifies the native model's required session
configuration using your developer identity without sending audio.

After publishing an image, `scripts\Test-ManagedIdentity.ps1 -Apply` deploys
and executes a manual, ingress-free Container Apps job with the actual
production managed identity. It checks registry pulls, container startup and
the exact Voice Live configuration without requiring a consumer user or sending
user audio. Execution is capped at 90 seconds with no retries or schedule.
Inspect the saved `.state\identity-probe.json` execution before retrying an
interrupted CLI operation. It does not deploy or bypass authentication on the
public proxy.
