# GUI artwork and font provenance

## Standalone user-supplied artwork

The following originals were supplied in this repository's local `reference`
directory. Exact byte-for-byte copies are vendored in
`tools\gui-assets\sources\brands` solely for reproducible generation of the
approved interface. Original hashes are pinned in `sources.lock.json` and
repeated for each raster in `manifest.json`.

| Vendored name | User-provided original |
| --- | --- |
| copilot.svg | Microsoft_Copilot_Icon.svg |
| microsoft.svg | Microsoft_logo_(2012).svg |
| onedrive.svg | Microsoft_OneDrive_Icon_(2025_-_present).svg |
| explorer.svg | Windows_Explorer.svg |
| settings.svg | Windows_Settings_icon.svg |

No separate license declaration was supplied with these five standalone
files. **Do not assume they are covered by the Fluent MIT license.** They
contain no explicit copyright or license text; Settings includes RDF metadata
describing an SVG still image and an empty title. This document does not invent
authors, copyright notices, redistribution permission, or brand usage rights.
Their use follows the user's requested assets; distribution/brand permission
review remains separate from this technical conversion. Original embedded
metadata is retained unchanged in the vendored sources.

The full Microsoft wordmark keeps its original `viewBox="0 0 337.6 72"`.
All rasterizations keep the source viewBox, paths and gradients; only the
rendering viewport is normalized to natural proportions, scaled and quantized.
No white background is baked into transparency.

## Selected Microsoft Fluent system icons

Source: <https://github.com/microsoft/fluentui-system-icons>, local supplied
checkout `reference\fluentui-system-icons\assets`. Only the selected SVG
files are vendored; their exact revisions are identified by SHA-256 rather
than depending on the moving upstream branch. Source folder URLs are recorded
per icon in the manifest. The original repository's MIT license is copied
verbatim to `licenses\FLUENT-MIT.txt`.

Selection: Mic color32; Mic, Record, Play, Stop, Arrow Sync, WiFi 1, WiFi Off,
Checkmark Circle, Warning, Speaker 2, Document Text, Chevron Left, Chevron Up,
Chevron Down regular controls. Native16/20/24 drawings are used when present;
WiFi 1 and WiFi Off lack native16, so their native20 drawings also supply16.
WiFi 1 is a generic connected marker, not a measured signal-strength claim.

The license's existing notice is **Copyright (c) 2020 Microsoft Corporation**.
It applies to this selected Fluent artwork, not automatically to the five
standalone brand originals above.

## Noto Sans subset

The approved preview supplied `NotoSans.ttf` and its `NOTO-OFL.txt`. Those
exact bytes are retained as the developer-only font source and the adjacent
`licenses\NOTO-OFL.txt`. The source font identifies itself as Noto Sans Regular,
with weight100..900 and width62.5..100 variation axes. The font's own notice
matches the supplied license:

> Copyright 2022 The Noto Project Authors
> (https://github.com/notofonts/latin-greek-cyrillic)

The font and generated bitmap font subset are under **SIL Open Font License
1.1**; the license is included verbatim. Firmware contains only the selected
glyph masks and metrics, never the source TTF or a font engine. No source font
metadata, outlines, or naming tables are modified. The generated firmware
subset is identified by its project GUI font enums, not presented as a new
upstream font release.

## Reproducibility and runtime scope

`manifest.json` records source and output hashes, sizes, bitmap pixel bytes,
font glyph coverage, baseline metrics, and 32-bit lookup table budgets.
`tools\gui-assets` provides the pinned offline-after-install developer
generator and validation fixtures. C data is const/read-only and the generated
lookup code allocates no memory. Licenses accompany redistribution; developer
libraries and source SVG/TTF files are not compiled into the device.
