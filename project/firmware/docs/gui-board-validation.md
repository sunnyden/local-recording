# Camera-assisted GUI bench checks

The GUI test-input build is an explicit local bench configuration, not a
production control API. Normal firmware must leave
`CONFIG_RECORDER_GUI_TEST_INPUT` disabled.

The operator helper is `tools\gui_board_check.py`. It uses the ESP-IDF Python
environment's existing pyserial package and an installed ffmpeg for
video-only DirectShow snapshots. It does not install dependencies, discover
and open arbitrary cameras, reset the board, flash firmware, or expose a
network endpoint.

## Device selection

Identify the intended device before use. The current bench has a Logitech
`c922 Pro Stream Webcam` pointed at the recorder, and CH340 on `COM3`.
Ports can change; do not assume that the earlier native `COM4` connection is
still present. The recorder's base MAC is `a4:cb:8f:d6:12:5c`.

Only application-only flashing into the installed `0x10000` / 3 MiB slot is
appropriate for this bench. Generated partition tables do not match the
installed layout. Preserve NVS, SD files and all data partitions.

## Whitelisted commands

Run from the repository root, using the selected COM port:

```powershell
$python = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe'
& $python .\project\firmware\tools\gui_board_check.py --port COM3 ping
& $python .\project\firmware\tools\gui_board_check.py --port COM3 status
& $python .\project\firmware\tools\gui_board_check.py --port COM3 key down
& $python .\project\firmware\tools\gui_board_check.py --port COM3 demo ai-listening
```

Keys are `up`, `down`, `enter`, and `back`. They follow the same controller
path as physical buttons; they are not arbitrary worker, memory or GPIO
commands. Demo scenes are synthetic and visibly marked. Use `demo off` to
leave fixture mode. The setup demo must not start real BLE provisioning.

The helper reads only bounded `UI_TEST ` JSON responses and ignores ordinary
UART logs without echoing them. The response must match the requested event.
It opens the serial port with DTR/RTS inactive and does not deliberately
reset the device.

## Guarded snapshots

Keep captures in a private session-artifact directory, not in the repository
or the public design-preview server directory:

```powershell
& $python .\project\firmware\tools\gui_board_check.py --port COM3 demo setup
& $python .\project\firmware\tools\gui_board_check.py --port COM3 capture `
  --camera 'c922 Pro Stream Webcam' --ffmpeg C:\utils\ffmpeg.exe `
  --output C:\private-bench-artifacts\setup-demo.png
```

The capture helper requires typed `demo` and `sensitive_setup` flags from
status. It persists snapshots only while synthetic demo mode is active, so
a safe-to-sensitive-to-safe transition cannot evade two point-in-time checks.
It also rechecks the display after capture, discards the temporary image if
safety/screen state changed, and refuses to overwrite an existing destination.
Only a synthetic setup fixture belongs in visual acceptance images.

Capture requests accept a conservative camera-name character set that excludes
DirectShow's `:` and `=` device-combination delimiters, select that video
device only, disable audio, and produce a single PNG. No microphone or camera
audio device is opened or recorded. An optional
`--crop width:height:x:y` restricts the image within the 1280-by-720 capture;
the current fixed-camera LCD bounding box is approximately `380:310:245:260`.
Recheck framing if the camera or board moves. A `calibration` fixture, when
available, helps identify screen bounds and color ordering.

These are cooperative bench safeguards, not a claim that serial test firmware
is a security boundary. Do not change real setup state concurrently with a
snapshot. Do not use this channel to print credentials or save camera images
to a public issue/PR without separate review.

## Acceptance scope

Camera snapshots establish native-panel layout, readability, color appearance
and visible state transitions. They do not alone establish audio fidelity,
OneDrive permissions or lossless real-time operation. Combine them with host
regressions, resource measurements and bounded operational checks.

Do not create or upload unattended ambient recordings merely to obtain a
screenshot. Prefer synthetic demo states for camera coverage. Keep real
pairing credentials and unrelated desktop/bystander content out of captures.
Finish validation by installing the normal GUI build with the serial test
bridge disabled and returning the device to the non-secret home screen.

The initial physical validation completed this sequence on COM3. It also found
a full-repaint starvation issue that host golden rendering did not expose:
periodic activity invalidation could repeatedly restart a screen transition.
The UI now defers periodic model/animation refresh until the pending dirty
region is fully submitted. Subsequent camera captures showed clean transitions.
