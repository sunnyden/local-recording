# Recorder GUI

The optional light GUI runs only in the existing main task. It renders native
RGB565 into the board driver's existing 10,240-byte DMA allocation. Each main
iteration routes keys first and submits at most one clipped strip, at most
320 × 8 pixels. A timed-out or failed transfer permanently quarantines its
storage until reboot; no GUI task, framebuffer, PSRAM allocation or IRAM code
is added.

`gui_assets.h` is the generated artwork/font interface. Rendering never parses
SVGs. Library names and last-known receipts are cached three at a time, only on
entry/page changes; rendering never performs storage or network I/O.

Setup credentials live in a separate private buffer, not `gui_model_t`.
Presentation must complete before the provisioning callback succeeds. Back is
polled between setup strips. Exit clears the private buffer and repaints before
returning. Display failure prevents starting setup. If clearing fails, the
existing LCD reset pin is asserted without reusing pending DMA storage. If
that also fails, telemetry conservatively keeps the sensitive-display flag set.

## Profiles and host validation

Keep `sdkconfig.defaults` unchanged. Use a separate SDKCONFIG in each build
directory and explicitly combine defaults:

| Build | Defaults, in order |
| --- | --- |
| `build-gui-poc` | `sdkconfig.defaults;sdkconfig.poc-sync.defaults;sdkconfig.gui.defaults` |
| `build-gui-test` | Above plus `sdkconfig.gui-test.defaults` |
| `build-gui-safe` | `sdkconfig.defaults` only (LCD, GUI and test bridge off) |
| `build-gui-baseline` | `sdkconfig.defaults;sdkconfig.poc-sync.defaults;sdkconfig.gui-test.defaults` (text GUI, same backend policy, test bridge) |

Run `tools\test-gui.ps1` with the existing `.host-tools` Zig installation.
The native tests exercise clipping, partial-strip equivalence, asset budgets,
all screen/controller paths, DMA ownership/timeout quarantine, secret clearing,
stale audio generations and synthetic-demo isolation. Seven 320 × 240 PPMs
are written to `build-host-gui`; these contain synthetic data only.

The actual installed application slot is **3 MiB at `0x10000`**. The generated
partition table describes a different layout. **Do not use generated flash
arguments or flash the partition table.** Hardware flashing and camera
acceptance are separate operator tasks.

## Measured implementation

The final local GUI build was measured against the final safe/default build:

| Measurement | Safe/default | Normal GUI | Delta |
| --- | ---: | ---: | ---: |
| Application binary | 1,511,968 bytes | 1,598,464 bytes | 86,496 bytes |
| Link-map total | baseline | baseline + 86,496 bytes | 86,496 bytes |
| Link-map DIRAM used | 183,134 bytes | 186,114 bytes | 2,980 bytes |

The normal build remains below the 192 KiB flash and 8 KiB static internal-RAM
delta targets, with 1,547,264 bytes left in the installed 3 MiB application
slot. Generated artwork reports 34,624 bytes and fonts 15,571 bytes on the
ESP's 32-bit ABI. The existing LCD DMA allocation remains 10,240 bytes.

On the physical board, the serial-test build reported 878 bytes of fixed
GUI/test state, 168,503-168,567 bytes of free internal heap, an approximately
98 KiB observed internal-heap low-water mark, a 63,488-byte largest DMA block,
and zero audio overruns during synthetic-screen validation. These are
point-in-time measurements, not guarantees for every real network/audio load.

Camera-assisted checks covered calibration, all seven synthetic screens,
selection navigation, secure synthetic setup, and multiple AI animation
frames. That process exposed and fixed repaint starvation: activity updates
are now deferred until an outstanding full repaint finishes. Header, tile,
list-row and button text use line-box centering against their icons. The
normal GUI image was then installed app-only and confirmed to have no serial
test bridge.

Normal GUI image used for this acceptance:

```text
size:   1,598,464 bytes
SHA256: 9a50b641c62969918b00f0487591ff2e4ba5d8c7d08a196297038422895e899f
```

Camera images remain private session artifacts because the camera sees more
than the LCD. They are not repository fixtures. Host-rendered synthetic PPMs
remain the reproducible source-level visual fixtures.

## Opt-in test bridge

`CONFIG_RECORDER_GUI_TEST_INPUT` defaults off and is entirely omitted from a
normal release. It depends on the LCD safety gate, not on the GUI, permitting a
text-mode resource baseline with an explicit test overlay.

UART or other configured stdin transport is polled nonblocking with a 96-byte
ASCII parser. Commands are only `ui ping`, `ui status`,
`ui key up|down|enter|back`, and
`ui demo home|recording|recordings|playback|sync|ai-listening|ai-speaking|setup|error|calibration|off`.
Calibration is a synthetic RGB565 color/gray fixture with a one-pixel screen
border and corner marks; Back returns to demo home.
Key injection follows exactly the physical-key controller. Key/status/demo
commands have independent 80 ms rate limits; there is no unbounded event queue.

Responses start with `UI_TEST ` and a version-1 JSON object. Events are `pong`,
`status`, `key`, `demo`, or the non-reflective `error`. Status never includes
filenames, free text, credentials or tokens. `sensitive_setup` explicitly marks
a real private setup display. Demo fixtures are visibly marked and never call
recording, playback, network, sync or provisioning operations. Real operations
must be idle before entering a demo. Text-only firmware rejects demo commands.

The reported GUI RAM count is fixed GUI/console state, not whole-system stack
or heap usage. Main's existing 8 KiB stack is unchanged. Runtime heap, DMA block,
audio-overrun and visual/color acceptance still require the actual board.
