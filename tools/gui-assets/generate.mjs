import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { Resvg } from '@resvg/resvg-js';
import { openSync } from 'fontkit';
import { assets, fonts, fontSource, budgets } from './config.mjs';

export const here = path.dirname(fileURLToPath(import.meta.url));
export const output = path.resolve(here, '../../project/firmware/components/gui/assets');
export const hash = data => createHash('sha256').update(data).digest('hex');
const sourcePath = name => path.join(here, 'sources', name);
const source = name => readFileSync(sourcePath(name));
const json = data => `${JSON.stringify(data, null, 2)}\n`;
const alignAllowance = 12; // At most three bytes before each of four 32-bit-aligned symbols.

export function packAlpha(alpha) {
  const result = Buffer.alloc(Math.ceil(alpha.length / 2));
  alpha.forEach((value, i) => {
    assert(Number.isInteger(value) && value >= 0 && value <= 255);
    result[i >> 1] |= Math.round(value * 15 / 255) << ((i & 1) ? 0 : 4);
  });
  return result;
}

export function quantize(pixels, color) {
  assert.equal(pixels.length % 4, 0);
  const alpha = [], rgb = [];
  for (let i = 0; i < pixels.length; i += 4) {
    const a = pixels[i + 3];
    alpha.push(a);
    if (color) {
      // resvg's raw pixels are premultiplied. Undo that BEFORE RGB565 conversion;
      // never flatten onto white, or pale-blue cards acquire white/dark halos.
      const channel = j => a ? Math.min(255, Math.round(pixels[i + j] * 255 / a)) : 0;
      rgb.push((Math.round(channel(0) * 31 / 255) << 11) |
        (Math.round(channel(1) * 63 / 255) << 5) | Math.round(channel(2) * 31 / 255));
    }
  }
  return { alpha: packAlpha(alpha), rgb };
}

function render(svg, size) {
  return new Resvg(svg, {
    ...(size ? { fitTo: { mode: 'width', value: size } } : {}),
    font: { loadSystemFonts: false },
  }).render();
}

function assetRaster(asset, size) {
  const file = asset.source ?? asset.sources[size];
  const svg = source(file).toString('utf8');
  assert(!/<(?:image|text)\b|(?:href|xlink:href)\s*=\s*["'](?!#)/i.test(svg),
    `${file}: external images, text and links are not allowed`);
  const viewBox = svg.match(/viewBox=["']([^"']+)["']/);
  assert(viewBox, `${file}: viewBox required`);
  const [, , naturalWidth, naturalHeight] = viewBox[1].trim().split(/[\s,]+/).map(Number);
  assert(naturalWidth > 0 && naturalHeight > 0);
  // Size is a maximum bounding dimension, except the explicitly wide wordmark.
  const width = Math.round(size * naturalWidth / Math.max(naturalWidth, naturalHeight));
  const normalized = svg.replace(/<svg\b[^>]*>/, root =>
    root.replace(/\s(?:width|height)=["'][^"']*["']/g, '')
      .replace('<svg', `<svg width="${naturalWidth}" height="${naturalHeight}"`));
  const image = render(normalized, width);
  const { alpha, rgb } = quantize(image.pixels, asset.color);
  assert.equal(image.width, width);
  assert(Math.abs(image.height - width * naturalHeight / naturalWidth) <= 1);
  return { id: asset.id, size, source: file, sourceSha256: hash(source(file)),
    width: image.width, height: image.height, alpha, rgb,
    naturalWidth, naturalHeight, provenance: asset.provenance };
}

function glyphRaster(font, character, pixels) {
  const glyph = font.glyphForCodePoint(character), scale = pixels / font.unitsPerEm;
  assert(glyph.id !== 0, `Missing font glyph ${character}`);
  const advance = Math.round(glyph.advanceWidth * scale);
  if (character === 32) {
    return { character, width: 0, height: 0, advance, offsetX: 0, offsetY: 0, alpha: Buffer.alloc(0) };
  }
  const box = glyph.bbox;
  const left = Math.floor(box.minX * scale), right = Math.ceil(box.maxX * scale);
  const top = Math.floor(-box.maxY * scale), bottom = Math.ceil(-box.minY * scale);
  const width = right - left, height = bottom - top;
  assert(width > 0 && height > 0);
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}"><path fill="#000" transform="translate(${-left} ${-top}) scale(${scale} ${-scale})" d="${glyph.path.toSVG()}"/></svg>`;
  const alpha = quantize(render(svg).pixels, false).alpha;
  return { character, width, height, advance, offsetX: left, offsetY: top, alpha };
}

function fontRaster(spec, original) {
  const font = original.getVariation({ wght: spec.weight, wdth: 100 });
  const supported = spec.characters ? [...spec.characters].map(c => c.charCodeAt(0)) :
    Array.from({ length: spec.last - spec.first + 1 }, (_, i) => spec.first + i);
  const glyphs = supported.map(c => glyphRaster(font, c, spec.pixels));
  const baseline = Math.max(...glyphs.map(g => -g.offsetY));
  const height = baseline + Math.max(...glyphs.map(g => g.offsetY + g.height));
  for (const glyph of glyphs) {
    for (const value of [glyph.width, glyph.height, glyph.advance]) assert(value >= 0 && value <= 255);
    for (const value of [glyph.offsetX, glyph.offsetY]) assert(value >= -128 && value <= 127);
    assert(baseline + glyph.offsetY >= 0 && baseline + glyph.offsetY + glyph.height <= height);
  }
  assert(height > 0 && height <= 255 && baseline <= 255);
  return { ...spec, baseline, height, glyphs };
}

function array(name, values, type = 'uint8_t') {
  const digits = type === 'uint16_t' ? 4 : 2;
  const lines = [];
  for (let i = 0; i < values.length; i += 16) {
    lines.push(`    ${Array.from(values.slice(i, i + 16), x => `0x${x.toString(16).padStart(digits, '0')}`).join(', ')},`);
  }
  return `static const ${type} ${name}[] = {\n${lines.join('\n')}\n};\n`;
}

const banner = '/* Generated by tools/gui-assets/generate.mjs; do not edit. See manifest.json and licenses/. */\n#include "gui_assets.h"\n\n';
const littleEndian565 = values => {
  const buffer = Buffer.alloc(values.length * 2);
  values.forEach((value, i) => buffer.writeUInt16LE(value, i * 2));
  return buffer;
};

function emitAssets(variants) {
  const rgb = [], alpha = [], rows = [];
  const manifest = variants.map(v => {
    const rgbOffset = rgb.length, alphaOffset = alpha.length;
    rgb.push(...v.rgb); alpha.push(...v.alpha);
    rows.push(`    { GUI_ICON_${v.id}, ${v.size}, { ${v.width}, ${v.height}, ${v.rgb.length ? `art_rgb + ${rgbOffset}` : 'NULL'}, art_alpha + ${alphaOffset} } },`);
    return { id: `GUI_ICON_${v.id}`, size: v.size, width: v.width, height: v.height,
      format: v.rgb.length ? 'RGB565+A4' : 'A4-mask', alphaOffset, rgbOffset,
      alphaBytes: v.alpha.length, rgbBytes: v.rgb.length * 2,
      pixelBytes: v.alpha.length + v.rgb.length * 2,
      pixelSha256: hash(Buffer.concat([littleEndian565(v.rgb), v.alpha])),
      source: v.source, sourceSha256: v.sourceSha256,
      viewBoxSize: [v.naturalWidth, v.naturalHeight], provenance: v.provenance };
  });
  const payloadBytes = rgb.length * 2 + alpha.length, tableBytes = variants.length * 20;
  const code = banner + array('art_rgb', rgb, 'uint16_t') + '\n' + array('art_alpha', alpha) +
    `\ntypedef struct {\n    gui_icon_id_t id;\n    unsigned size;\n    gui_bitmap_t bitmap;\n} asset_variant_t;\n\n` +
    `static const asset_variant_t variants[] = {\n${rows.join('\n')}\n};\n\n` +
    `const gui_bitmap_t *gui_asset_get(gui_icon_id_t icon, unsigned size)\n{\n` +
    `    for (size_t i = 0; i < sizeof variants / sizeof variants[0]; ++i) {\n` +
    `        if (variants[i].id == icon && variants[i].size == size) return &variants[i].bitmap;\n` +
    `    }\n    return NULL;\n}\n\n` +
    `size_t gui_assets_bytes(void)\n{\n    return sizeof art_rgb + sizeof art_alpha + sizeof variants;\n}\n` +
    `\n_Static_assert(sizeof art_rgb + sizeof art_alpha + sizeof variants <= ${budgets.artwork}, "GUI art budget exceeded");\n`;
  return { code, variants: manifest, bytes: { rgb565: rgb.length * 2, alpha4: alpha.length,
    payload: payloadBytes, tables32: tableBytes, symbols32: payloadBytes + tableBytes,
    alignmentAllowance32: alignAllowance, budgeted32: payloadBytes + tableBytes + alignAllowance,
    limit: budgets.artwork } };
}

function emitFonts(rasters) {
  const alpha = [], glyphRows = [], fontRows = [];
  const manifest = rasters.map(f => {
    const byCharacter = new Map();
    const glyphs = f.glyphs.map(g => {
      const { alpha: pixels, ...metrics } = g;
      const record = { ...metrics, alphaOffset: alpha.length, alphaBytes: pixels.length,
        pixelSha256: hash(pixels) };
      alpha.push(...pixels); byCharacter.set(g.character, record);
      return record;
    });
    fontRows.push(`    { ${f.height}, ${f.baseline}, ${f.first}, ${f.last}, glyphs + ${glyphRows.length} },`);
    for (let c = f.first; c <= f.last; ++c) {
      const g = byCharacter.get(c) ?? byCharacter.get(f.fallback);
      glyphRows.push(`    { ${g.width}, ${g.height}, ${g.advance}, ${g.offsetX}, ${g.offsetY}, ${g.alphaBytes ? `font_alpha + ${g.alphaOffset}` : 'NULL'} },`);
    }
    const { glyphs: unused, ...metrics } = f;
    return { ...metrics, glyphs };
  });
  const code = banner + array('font_alpha', alpha) +
    `\nstatic const gui_glyph_t glyphs[] = {\n${glyphRows.join('\n')}\n};\n\n` +
    `static const gui_font_t fonts[] = {\n${fontRows.join('\n')}\n};\n\n` +
    `const gui_font_t *gui_font_get(gui_font_id_t font)\n{\n` +
    `    if ((unsigned)font >= sizeof fonts / sizeof fonts[0]) return NULL;\n    return &fonts[font];\n}\n\n` +
    `const gui_glyph_t *gui_font_glyph(const gui_font_t *font, unsigned character)\n{\n` +
    `    if (!font || !font->glyphs || font->first > font->last) return NULL;\n` +
    `    if (character < font->first || character > font->last) {\n` +
    `        character = font == &fonts[GUI_FONT_TIMER] ? '-' : '?';\n` +
    `        if (character < font->first || character > font->last) return NULL;\n` +
    `    }\n    return &font->glyphs[character - font->first];\n}\n\n` +
    `size_t gui_fonts_bytes(void)\n{\n    return sizeof font_alpha + sizeof glyphs + sizeof fonts;\n}\n` +
    `\n_Static_assert(sizeof font_alpha + sizeof glyphs + sizeof fonts <= ${budgets.fonts}, "GUI font budget exceeded");\n`;
  const tableBytes = glyphRows.length * 12 + rasters.length * 8;
  return { code, fonts: manifest, bytes: { alpha4: alpha.length, payload: alpha.length,
    glyphTableEntries: glyphRows.length, fontTableEntries: rasters.length, tables32: tableBytes,
    symbols32: alpha.length + tableBytes, alignmentAllowance32: alignAllowance,
    budgeted32: alpha.length + tableBytes + alignAllowance, limit: budgets.fonts } };
}

export function generate() {
  assert.equal(process.versions.node, '24.16.0', 'Regenerate with the pinned Node 24.16.0 toolchain');
  const lock = JSON.parse(readFileSync(path.join(here, 'sources.lock.json')));
  const used = new Set([fontSource, ...assets.flatMap(a => a.source ? [a.source] : Object.values(a.sources))]);
  assert.deepEqual(Object.keys(lock).sort(), [...used].sort(), 'Vendored source lock differs from used subset');
  for (const [file, digest] of Object.entries(lock)) assert.equal(hash(source(file)), digest, `Source changed: ${file}`);
  const art = emitAssets(assets.flatMap(a => a.sizes.map(size => assetRaster(a, size))));
  const font = emitFonts(fonts.map(f => fontRaster(f, openSync(sourcePath(fontSource)))));
  assert(art.bytes.budgeted32 <= budgets.artwork, 'Art budget exceeded');
  assert(font.bytes.budgeted32 <= budgets.fonts, 'Font budget exceeded');
  const inputs = Object.fromEntries(['config.mjs', 'generate.mjs', 'package.json', 'package-lock.json', 'sources.lock.json']
    .map(file => [file, hash(readFileSync(path.join(here, file)))]));
  const manifest = {
    schema: 1, generator: 'tools/gui-assets/generate.mjs',
    toolchain: { node: '24.16.0', resvg: '2.6.2', fontkit: '2.0.4' }, inputs,
    encoding: { rgb565: 'Native uint16_t R5G6B5, straight (not premultiplied); hashes use little-endian uint16_t',
      alpha4: 'Row-major continuous nibbles, high nibble first; odd final low nibble zero; no row padding',
      fontMetrics: 'Integer advance; bitmap offsets relative to pen baseline; no runtime kerning' },
    artworkBytes: art.bytes, fontBytes: font.bytes,
    sourceFont: { file: fontSource, sha256: lock[fontSource], license: 'SIL-OFL-1.1' },
    variants: art.variants, fonts: font.fonts,
    generated: { 'gui_assets.c': hash(art.code), 'gui_fonts.c': hash(font.code) },
  };
  return { 'gui_assets.c': art.code, 'gui_fonts.c': font.code, 'manifest.json': json(manifest) };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const files = generate(), check = process.argv.includes('--check');
  if (!check) mkdirSync(output, { recursive: true });
  for (const [name, content] of Object.entries(files)) {
    if (check) assert.equal(readFileSync(path.join(output, name), 'utf8'), content, `${name} is stale`);
    else writeFileSync(path.join(output, name), content);
  }
  const manifest = JSON.parse(files['manifest.json']);
  console.log(`${check ? 'Verified' : 'Generated'} ${manifest.variants.length} variants; ` +
    `art ${manifest.artworkBytes.budgeted32}/${budgets.artwork}, fonts ${manifest.fontBytes.budgeted32}/${budgets.fonts} bytes (32-bit, tables/alignment included).`);
}
