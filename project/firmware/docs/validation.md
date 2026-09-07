# Firmware validation record

## Initial HTTP timestamp/processing extension validation (historical images)

For the later separate processing WebSocket's current build artifacts, tests,
and acceptance gates, see [processing stream validation](recording-processing.md).

The owner confirmed the existing record/play/Sync/AI behavior before this
extension. That confirmation supersedes the historical baseline's "not run"
entries below, but does not validate the new processing path on hardware.

* Existing `tools\test-core.ps1` regression suite passes, including unchanged
  voice/audio/identity/upload tests and new HTTPS deadline, cancellation,
  bounded-request/response, slow-drip receive abort, and origin-validation cases.
* `tools\test-intelligence.ps1` passes actual storage/name/checkpoint/outbox and
  actual cloud Sync/processing/UI host tests. It covers timestamp/offset/offline
  names, collision avoidance, old recovery journals, torn outbox updates,
  all upload-success branches, multiple durable jobs, replay without WAV files,
  status-first reconciliation, consent, cancellation, Retry-After and long-WAV
  skips. Network, scheduler and SHA-1-provider boundaries are synthetic.
* Final isolated ESP-IDF 6.1 builds pass: safe image `0x16a720` bytes
  (1,484,576) and owner-approved PoC profile `0x175640` bytes (1,529,408).
  Both fit the existing 3 MiB application capacity. Final incremental checks
  used `ninja -j1`. No source compiler warnings were reported.
* Source changes leave audio/voice buffers, codec, partition definitions, and
  existing generated sdkconfigs untouched. No flash/reset/erase/eFuse or
  credential reads were performed.

Build outputs are in `build-intelligence-safe` and `build-intelligence-poc`;
their generated partition tables are **not** the device's installed layout.
The parent must preserve installed 24 KiB NVS and factory app at `0x10000`
before any later app-only flash. See
[implementation and acceptance checklist](recording-processing.md).
Real SD power-fail behavior, cold-start TLS/HTTP timing, task stack margins,
live device-to-backend sidecars, and acoustic regression remain hardware gates.

## Historical baseline validation

Implementation validation on 2026-09-06, Windows, ESP-IDF tag `v6.1`.

Coordinator-reported cloud prerequisites on 2026-09-06: the actual consumer
`devicecode` endpoint accepted separate Graph `Files.ReadWrite` and API-B
`access_as_user` requests, each including `offline_access`. No user codes were
displayed and no token polling/enrollment occurred. The coordinator also reports
that native Voice Live `gpt-realtime-2` accepted 16 kHz input, service AEC, and
server VAD. These are cloud configuration checks, not ESP-to-cloud or acoustic
acceptance tests. Firmware still keeps credential persistence disabled.

| Check | Result |
| --- | --- |
| Safe-default IDF build | Passed; all local/network/identity/BLE/sync/voice modules linked |
| LCD-enabled isolated IDF build | Passed; real ST7789 driver branch compiled |
| Portable C core tests | Passed via Zig 0.14.1 host compiler, `-Wall -Wextra -Werror` |
| Actual identity/BLE control sources with host mocks | Passed: grant pacing/lifecycle, audiences, rotation/revocation, storage failures, malformed requests |
| Actual uploader with HTTP transport mock | Passed: partial writes, range/offset recovery, throttling, expiration/quota, cancellation |
| Actual voice client with transport/audio mocks | Passed: token boundary, fragmentation, duplex capture, clear/stale-epoch behavior, malformed frame teardown |
| Shared voice-v1 JSON fixtures | Passed: canonical encoder bytes, decoded fields/extreme signed samples, all malformed wire fixtures rejected |
| Actual audio_io source with simulated DMA callbacks | Passed: 50-frame ring, 500 ms prefill, combined 16000-sample cap, completion credits, clearing and short tails |
| Local recording/playback on board | Not run |
| BLE companion against ESP | Not run |
| Personal Microsoft device grant / refresh | Not run |
| OneDrive upload/download equality and faults | Not run |
| Voice fake-proxy/device integration and acoustics | Not run |
| Parent ROM identification | ESP32-S3 QFN56 rev0.2; embedded PSRAM 8 MB AP3.3V; flash16 MB quad3.3V |
| Flash/reset/eFuse operations | Parent ROM probe opened COM3 and caused two normal RTS resets; no flash writes/erase, eFuse writes, or credential reads |

Build commands:

```powershell
.\tools\build.ps1
.\tools\test-core.ps1
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
$env:PYTHONUTF8 = "1"
idf.py -B build-lcd-validation -D SDKCONFIG=sdkconfig.validation `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.lcd-validation.defaults" build
```

The build initially exposed and then fixed a missing explicit `spi_flash`
dependency, IDF 6's managed cJSON name, a path formatting bound, and Security 2
salt/verifier signedness. The old generic `export.ps1` looked in a nonexistent
Python 3.14 environment; the installed Espressif activation profile correctly
selects its preinstalled `C:\Espressif\tools\python\v6.1\venv` and toolchain.
Direct inspection confirms that venv reports Python 3.14.5 and contains
esptool 5.3.1. The failure was environment path selection, not that version of
Python itself. The build helper now invokes the venv's absolute executable.
No SDK dependencies were
installed to work around that environment-selection issue.

The final safe-default and LCD branches produced loadable ESP32-S3 images in
their ignored build directories. Image sizes depend on subsequent changes;
the authoritative partition check is printed by each build. Flash is configured
16 MiB, octal PSRAM 80 MHz. The later parent ROM probe reports 16 MB flash and
8 MB embedded PSRAM; runtime PSRAM initialization/timing and LCD/audio operation
remain hardware gates.

## Remaining engineering/acceptance limitations

* An optional existing-HMAC encrypted-NVS profile is implemented and build/
  host-boundary validated. It requires an already-provisioned protected key and
  explicit owner approval; no external key installation, production secure-boot/
  debug hardening, real credential enrollment, or encrypted hardware test ran.
* No hardware fault/soak testing has established worst-case SD latency or
  FreeRTOS stack margins with Wi-Fi/TLS/BLE. Buffers are bounded, not measured.
* DMA-complete playback progress conservatively excludes a partially played
  20 ms block at interruption. Validate whether finer reporting is needed.
* Current firmware/proxy budget: 16000 samples/one second outstanding, 50
  software frames, 500 ms prefill, and PCM-rate pacing after prefill.
  Verify normal latency, DMA priming latency, and that new output epochs do not
  silently truncate previous tails on the real device.
* Sync uses Graph SHA-1 metadata for restart/ambiguous-final reconciliation.
  Drives without that metadata fail closed rather than claiming byte equality.
* Signed upload URLs do not survive reboot; expired sessions are restarted by a
  later explicit Sync, not transparently renewed forever.
* BLE protocol validation is implemented, but WinRT MTU/long-write interoperability
  still needs the companion's actual Security 2 exchange against hardware.
* Host integration tests exercise the actual identity, uploader, BLE endpoint,
  and voice-client C source against deterministic boundary mocks. They do not
  use the real ESP HTTP/TLS/FreeRTOS/Bluetooth drivers or a networked fake service.
  Live cloud and hardware acceptance are not implied by those results.
* A gated serial diagnostic command interface is not implemented; initial
  mode control uses the physical buttons and LCD, with redacted startup UART
  telemetry. Do not treat a generated digital fixture as microphone validation.

## Offline regression findings fixed

* JSON `\u0000` escapes could make a C-string comparison accept a valid command
  prefix followed by hidden data. BLE, voice, and HTTPS JSON now reject both
  raw and decoded NULs; literal escaped-backslash text remains distinct.
* Empty access tokens and initial grants without durable refresh tokens are
  rejected before credential writes or authorized status.
* Upload `nextExpectedRanges` previously accepted trailing nonnumeric garbage.
  The parser now accepts only a suffix range with an optional valid final bound.
* Cancelling before the grant task starts now avoids the device-code HTTP call.
* The BLE application endpoint enforces 496 plaintext bytes in both directions
  (512-byte NimBLE characteristic minus the 16-byte AES-GCM tag). Boundary tests
  accept 496 and reject 497 bytes; this replaces the original 4096-byte logical
  application ceiling to align with the actual transport and companion.
* Explicit `setup.finish` calls `network_prov_mgr_stop_provisioning()` with the
  manager's configured 1500 ms cleanup delay. In pinned provisioning 1.2.4,
  `src\manager.c` schedules `cleanup_delay_timer` asynchronously using that
  value; `network_prov_mgr_disable_auto_stop(1500)` both disables Wi-Fi auto-stop
  and sets the explicit-stop grace. This is not an assumption that disabling
  auto-stop alone postpones teardown. Real Windows ACK timing remains untested.
* Repeating `auth.start` for an already authorized resource returns authorized
  status rather than launching another account grant. Pending grant expiry
  returned to the companion reflects remaining lifetime.
* Voice control sends are queued out of the receive callback, avoiding the
  WebSocket SDK's receive-lock/send-lock inversion. Host tests assert that no
  outbound send executes inside a transport callback.
* Normal wire epochs wait for the prior end position to drain; a new epoch
  before clear acknowledgment remains supported. A dedicated 127-sample final
  tail test verifies exact final progress continues after `playback.end`, so
  the proxy can release its bounded backlog without discarding the old tail.
* The coordinator's later TCP-coalescing decision supersedes only the six-frame
  software allocation: 50 software frames now absorb the entire legal 16000-
  sample hard credit. Four DMA periods remain allocated, and combined pending
  accounting prevents overcommit beyond one second.

The proxy implementer subsequently reported a live Voice Live session driven by
paced synthetic 16 kHz microphone input, with 37,600 output samples played by its
simulated speaker and no session errors. This is coordinator-reported proxy/
provider evidence, not a physical ESP microphone, DAC, or acoustic test.

### Credit versus actual firmware DMA pipeline

The `audio_io_test.c` host test now drives the production `audio_io.c` callback
through four rotating 20 ms DMA blocks, 80 ms progress reports, PCM-rate input,
and no more than three frames (60 ms) per startup/catch-up burst. Over 100 DMA
periods, after excluding the first four priming periods:

| Proxy credit | Empty steady-state DMA periods |
| --- | --- |
| 1600 samples / 100 ms | 36 |
| 16000 samples / one second, 500 ms prefill | 0 |

Thus the old request for 1600-sample credit should not be used: it fits memory
but is too small for this pipeline and report cadence in the deterministic model.
Use 16000-sample hard credit, 50 software frames, and 500 ms prefill; combined
pending accounting includes the four DMA periods.
This is an actual-firmware callback model, not a physical timing/acoustic test.

### Final current-source handoff (2026-09-06 23:34 local time)

* Current production proxy `Settings.max_unplayed_samples` is **16000**.
  The parent's `project\tests\test_audio_budget.py` passes **3/3** using the
  existing `project\proxy\.venv\Scripts\python.exe`. No new Python environment
  or dependencies were installed.
* All **eight** firmware host-test executables pass, including the actual DMA
  callback model and the additional HTTPS/Wi-Fi lifecycle regressions.
* Current safe-default IDF 6.1 build passes via `tools\build.ps1`, which invokes
  `C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe` explicitly.
* App image: `project\firmware\build\embedded_recorder.bin`, **1,480,592 bytes**.
  SHA-256: `d57671df2f7ca04890162b52c8107d41b09286a8dd9edae8bcaa942bedf726a3`.
  This identifies the app binary only; flashing requires its matching bootloader
  and partition-table artifacts and separate parent authorization.
* Current generated configuration retains an empty proxy WSS URL, disabled
  credential-risk opt-in, and disabled LCD hardware-confirmation flag. Public
  client/scope defaults are present. The firmware implementation agent did not
  open COM3 during this handoff; the later parent ROM probe is recorded separately.

### Optional encrypted-NVS follow-up

The existing-HMAC profile now closes the previously documented implementation
gap. Both safe-default and isolated HMAC configurations compile. Eleven host
executables pass, including fourteen credential-policy scenarios across denied,
plaintext-development, and existing-key encrypted profiles.

SDK inspection found that generic encrypted `nvs_flash_init()` may generate keys
and its HMAC provider can burn a missing eFuse key. The final implementation
therefore excludes SDK automatic providers (`NVS_SEC_KEY_PROTECT_NONE`) and
registers only a read/derive callback, with generation callback NULL throughout.
The application symbol audit confirms absence of generic NVS initialization,
NVS generation APIs, and `esp_efuse_write_key` from the isolated encrypted image.
See [credential profiles](credential-profiles.md) for exact prerequisites,
derivation compatibility, commands, and remaining physical-security limitations.

Latest artifacts after this follow-up (supersede the earlier app hashes):

| Profile | App bytes | SHA-256 |
| --- | --- | --- |
| LCD local-only (`build-local-bringup`) | 1,480,208 | `79d32f62c6366f6f3a8c2385af989966cdaf3e1e4ffdc2661f37e914cde92f29` |
| Existing-HMAC compile-only (`build-hmac-validation`) | 1,483,296 | `032114e2c6d1ef516effa42f2c486032d6e18afba2ae853e08de0e42b69e0e2f` |

The final symbol audit also found no defined eFuse write/batch-write/read-protect/
write-protect setter symbols in the isolated encrypted application. Host
compile-negative tests reject attempts to combine the profile with either
SDK secure-boot or flash-encryption boot-time enablement. The default generated
configuration still disables NVS encryption, the HMAC profile, and plaintext-risk
acceptance. The firmware implementation agent did not open a serial port or run
the encrypted profile on hardware. Separately, the parent later performed ROM
identification through COM3 with two RTS resets and no flash/erase/eFuse writes
or credential reads; this does not validate encrypted-NVS operation.

The latest hardware-informed capacity decision allocates **50 software frames**
with a **combined 16000-sample limit**, 500 ms prefill, and nominal 80 ms
progress. This handles Wi-Fi jitter and coalesced delivery without changing
the credential-store implementation.

### Parent ROM probe and remaining hardware gates

On 2026-09-07 the parent reported successful esptool 5.3.1 `--no-stub` chip-ID
and flash-ID using `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`.
Identified device: ESP32-S3 QFN56 revision 0.2, embedded 8 MB AP 3.3 V PSRAM,
16 MB quad 3.3 V flash, COM3. Two normal RTS resets occurred. No firmware was
flashed, no flash was erased, no eFuses were written, and no credentials were
read. Runtime PSRAM, physical LCD/P5 routing, recording/playback, and acoustics
remain unverified by this application. The user subsequently confirmed factory/
reference LCD operation; the coordinator accepts the routing gate for local-only
bring-up. Credentials remain off and the parent coordinates any rollback-safe flash.

### LCD-confirmed local-only handoff

The user confirmed factory/reference LCD operation on 2026-09-07. The isolated
`local-bringup` profile enables the LCD and explicitly disables both credential
profiles, NVS encryption, SD formatting on mount failure, flash-encryption
enablement, and secure boot. `tools\build.ps1 -Profile local-bringup` builds and
checks these guards; it never invokes flashing. This application still needs
its own LCD/audio/PSRAM runtime acceptance, and the parent owns rollback/flash.

The authoritative `build-local-bringup\flasher_args.json` identifies only these
matching build artifacts, with no NVS payload:

| Offset | Relative artifact | SHA-256 |
| --- | --- | --- |
| `0x0` | `bootloader\bootloader.bin` | `36e6731ad66f07411c31ce309334998a629c5b230357540af465f23b469b3d1c` |
| `0x8000` | `partition_table\partition-table.bin` | `b16b1b27ccfeecedd026631c54e77065d3913bca810cad5cfe77525fc9938f75` |
| `0x20000` | `embedded_recorder.bin` | `79d32f62c6366f6f3a8c2385af989966cdaf3e1e4ffdc2661f37e914cde92f29` |

The final actual-driver host regression accepts 50 initial software frames,
rejects the next sample, grants no credit merely for DMA transfer, and grants
capacity only after completion. Both local-only and
existing-HMAC IDF builds and all eleven host executables pass. No serial or
hardware action was performed by this firmware work.
