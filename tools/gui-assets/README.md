# Recorder GUI asset pipeline

This **developer-only** directory contains the exact artwork subset and one
source font. Production firmware compiles the checked-in `gui_assets.c` and
`gui_fonts.c` under `project\firmware\components\gui\assets`; it needs neither
Node, SVG support, fontkit, a font parser, nor the source TTF. No reference
checkout, Nx build, source download, or full icon library is needed in CI.

## Regenerate and verify

Use Node **24.16.0**, with exact dependency versions/integrities in
`package-lock.json`. From this directory:

```powershell
npm ci --ignore-scripts --no-audit --no-fund
npm run generate
npm run check
npm test
.\test-host.ps1
```

The last command requires **no Node or npm packages**. It uses the repository's
existing host Zig compiler by default, or accepts `-Compiler` for an existing
GCC/Clang/Zig C11 compiler. For any ordinary host C compiler, compile
`test-host.c`, `gui_assets.c`, and `gui_fonts.c` together with the GUI `include`
directory on the include path. Nothing from ESP-IDF is required.

`sources.lock.json` pins every vendored byte with SHA-256; regeneration fails
on an unreviewed input change. To deliberately update artwork, review its
provenance/license, replace only the selected source, update its source lock
hash, regenerate, and run both Node and host-C tests. The output manifest
records generator/config/lock hashes, source hashes, per-raster pixel hashes,
glyph metrics, and actual payload/table accounting. No timestamps or absolute
machine paths enter generated files.

## Rendering contract

- Sizes are **keys for maximum bounds**, not forced square crops. The Microsoft
  key `88` retains the complete 337.6:72 viewBox as **88×19**. Other brand
  dimensions are in the manifest. Center using actual width/height.
- Brand SVG gradients are rasterized with resvg on transparent backgrounds.
  The SVG root's viewport is normalized to its existing viewBox dimensions;
  paths, viewBox origins, gradient definitions, and proportions remain intact.
- resvg raw pixels are premultiplied; the generator unpremultiplies RGB before
  R5G6B5 quantization. The C `uint16_t` is native-endian and not SPI-byte-swapped.
  Blend against the current destination using the separate 0..15 alpha.
  Transparent artwork is never flattened onto a white or pale-blue matte.
- Monochrome Fluent controls contain only tintable alpha4. All alpha is
  continuous row-major, **high nibble first**, with no padding at odd-width row
  boundaries. An odd final pixel has a zero unused low nibble.
- HINT/BODY/TITLE use 95 ASCII characters (32..126), including lowercase and
  punctuation, from Noto Sans at nominal 11/14/16 px. HINT/BODY are weight400,
  TITLE weight600. TIMER uses nominal32 px weight500 and only `-0123456789:`.
  Actual line heights/baselines are **12/9, 15/11, 17/13, 25/24** respectively.
  The line box encloses the selected glyph ink; it is not the original font's
  larger global ascender/descender box. Offsets are relative to the baseline.
  Advances are integer, digits are tabular, and no kerning/shaping runs on ESP.
- Unsupported text uses `?`; TIMER substitutes `-`, including `.` and `/`
  holes in its contiguous lookup table. Blank space has advance but no pixels.
  Unknown icon keys/font IDs return NULL. All data is static const.

## Budgets

There are **54 bitmap variants**, 285 ASCII glyphs, and 12 timer glyph bitmaps.
The timer's 14-entry table aliases its two unused positions to the hyphen.

| 32-bit target storage | Artwork | Fonts |
| --- | ---: | ---: |
| Actual pixel bytes | 33,544 | 11,951 |
| Lookup tables, including structure padding | 1,080 | 3,620 |
| Symbols total | 34,624 | 15,571 |
| Conservative linker-alignment allowance | 12 | 12 |
| Budgeted bytes | **34,636** | **15,583** |
| Limit | 49,152 | 24,576 |

Combined budgeted read-only data is **50,219 bytes**. APIs report exact
`sizeof` sums on the compiling ABI, so a 64-bit host reports larger tables.
The manifest is explicitly for the ESP's 32-bit ABI. C lookup function code
is additional flash, not pixel bytes; final whole-GUI code/data must still
fit the parent workstream's 192 KiB incremental application limit.
There is no mutable static data, heap allocation, IRAM annotation, or steady
RAM requirement in these translation units. Verify final DROM placement and
whole-firmware limits in the ESP link map; host tests are not LCD acceptance.
Standalone ESP32-S3 GCC15.2 (`-Os -ffunction-sections -fdata-sections`) object
validation also confirms 34,624 and 15,571 read-only data bytes, **zero `.data`
and `.bss`**, and 163 combined bytes of lookup-function text/literal sections
before linker padding. These are object measurements, not a whole-image
flash delta or a claim about hardware readability.

See `project\firmware\components\gui\assets\PROVENANCE.md` and its adjacent
`licenses` directory. The standalone user-provided brand files are **not**
relicensed as Fluent MIT artwork.
