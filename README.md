# Local recording

ESP32-S3 WAV recorder with local playback, BLE-assisted Wi-Fi and Microsoft
sign-in, direct OneDrive synchronization, and a hands-free Azure Voice Live
conversation mode.

Implementation, protocols and setup instructions are in [`project`](project/README.md).
The physical MVP has validated LCD operation, SD formatting/mounting, recording,
playback, secure BLE provisioning, Wi-Fi reconnect, Microsoft personal-account
authorization, and OneDrive upload to `local-recording/`.

The voice proxy targets native Azure Voice Live `gpt-realtime-2`. Its container,
managed identity and session contract have passed cloud diagnostics. Public
proxy deployment and device voice acceptance are driven by the production
GitHub Actions workflow.

## Repository safety

- No Wi-Fi passwords, OAuth tokens, Azure client secrets, API keys, recordings,
  local deployment state, firmware binaries, or reference vendor sources are
  tracked.
- GitHub deploys with Azure workload identity federation; there is no Azure
  deployment secret.
- The PoC firmware stores Wi-Fi and refresh credentials in ordinary NVS only
  because the owner explicitly accepted physical extraction risk. Do not use
  that profile as a production security configuration.
- Do not run generated `idf.py flash` commands against the retained factory
  partition table. Follow [`hardware-bringup.md`](project/docs/hardware-bringup.md).

## Automation

- **PR validation** runs Python tests, Bicep validation, proxy image build/scan,
  and an ESP-IDF 6.1 safe-default firmware build.
- **Production deployment** builds an immutable proxy image in the existing
  private ACR, scans it, and updates the existing scale-to-zero Container App
  using GitHub OIDC.

See [`ci-cd.md`](project/docs/ci-cd.md) for trust boundaries and operations.
