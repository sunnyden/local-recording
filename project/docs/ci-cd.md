# GitHub CI/CD

Repository: `sunnyden/local-recording`

## Validation

`.github/workflows/pr-validation.yml` runs on pull requests and manually:

1. actionlint workflow validation;
2. proxy, companion, infrastructure and shared protocol tests on Python 3.12;
3. Bicep build/lint and PowerShell infrastructure tests;
4. local proxy container build and critical/high Trivy scan;
5. ESP-IDF 6.1 safe-default ESP32-S3 build.

The firmware CI build does not enable the LCD jumper assertion, ordinary-NVS
credential risk, SD auto-formatting, or security eFuse operations. Hardware
acceptance remains separate.

## Production deployment

`.github/workflows/deploy-production.yml` runs for relevant changes on the
default branch or manually. It:

1. authenticates to Azure using GitHub OIDC;
2. builds and pushes `recorder-proxy:sha-<commit>` to the existing private ACR;
3. resolves and scans the immutable digest;
4. deploys `project/infra/bicep/proxy.bicep` into `copilot-test`;
5. verifies `/readyz` through the scale-to-zero HTTPS ingress.

Manual runs can supply an existing `sha256:` digest to redeploy without a
build. The workflow never creates/replaces the Foundry resource, identity
registrations, model selection, or broad subscription infrastructure.

## OIDC trust and permissions

Deployment application: `local-recording-github-deploy`

Federated subject:

```text
repo:sunnyden@4323095/local-recording@1359280899:environment:production
```

GitHub currently emits immutable owner/repository IDs in the OIDC subject.
`Configure-GitHubOidc.ps1` resolves these from the GitHub API and refuses
unexpected metadata. A repository rename therefore does not silently broaden
the trust.

The identity has:

- `Contributor` scoped only to resource group `copilot-test`;
- `AcrPush` scoped only to `recorderdas7i6efgflpa`.

There is no client secret or certificate. The `production` environment holds
nonsecret IDs/endpoints as GitHub environment variables. `ALLOWED_USER_OID`
must be the consumer `oid` verified through API B's real token validator; the
workflow fails closed if it is absent or malformed.

## Production guardrails

- One worker and one maximum Container Apps replica preserve the prototype's
  process-local one-session cap.
- The proxy uses managed identity for Voice Live and ACR pulls.
- The model/profile/API version are exact; no silent fallback is allowed.
- The deploy workflow updates only the proxy Container App and image.
- Registry storage, active Container Apps sessions, and Voice Live calls can
  incur cost even though the app can scale to zero.

If the OIDC subject, repository owner/name, environment name, resource group,
or registry changes, rerun `Configure-GitHubOidc.ps1` only after reviewing the
new trust and role scopes. It refuses to adopt an untagged application.
