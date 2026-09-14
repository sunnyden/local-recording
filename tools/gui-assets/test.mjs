import assert from 'node:assert/strict';
import test from 'node:test';
import { readFileSync, readdirSync } from 'node:fs';
import path from 'node:path';
import { Resvg } from '@resvg/resvg-js';
import { assets, fonts, fontSource } from './config.mjs';
import { generate, hash, here, output, packAlpha, quantize } from './generate.mjs';

const manifest = JSON.parse(readFileSync(path.join(output, 'manifest.json')));
const artC = readFileSync(path.join(output, 'gui_assets.c'), 'utf8');
const fontC = readFileSync(path.join(output, 'gui_fonts.c'), 'utf8');
const values = (c, name) => {
  const body = c.match(new RegExp(`\\b${name}\\[\\] = \\{([\\s\\S]*?)\\n\\};`));
  assert(body, `Missing array ${name}`);
  return [...body[1].matchAll(/0x([a-f0-9]+)/g)].map(x => parseInt(x[1], 16));
};
const artAlpha = Buffer.from(values(artC, 'art_alpha')), artRgb = values(artC, 'art_rgb');
const fontAlpha = Buffer.from(values(fontC, 'font_alpha'));
const unpack = (bytes, index) => (bytes[index >> 1] >> ((index & 1) ? 0 : 4)) & 15;
const to565Buffer = values => {
  const b = Buffer.alloc(values.length * 2);
  values.forEach((v, i) => b.writeUInt16LE(v, i * 2));
  return b;
};

test('regeneration is byte-for-byte identical, including provenance hashes', () => {
  for (const [name, data] of Object.entries(generate())) {
    assert.equal(readFileSync(path.join(output, name), 'utf8'), data, name);
  }
  assert.equal(hash(artC), manifest.generated['gui_assets.c']);
  assert.equal(hash(fontC), manifest.generated['gui_fonts.c']);
});

test('continuous high-first A4 packs odd-width rows with a zero final nibble', () => {
  assert.deepEqual([...packAlpha([0, 255, 17, 34, 51, 68])], [0x0f, 0x12, 0x34]);
  assert.deepEqual([...packAlpha([255, 0, 128])], [0xf0, 0x80]);
  assert.equal(packAlpha([]).length, 0);
  assert.throws(() => packAlpha([256]));
  assert.throws(() => packAlpha([-1]));
});

test('raw resvg alpha is unpremultiplied before RGB565, never matted', () => {
  const raw = new Resvg('<svg xmlns="http://www.w3.org/2000/svg" width="3" height="1"><path fill="#f00" fill-opacity=".5" d="M0 0h1v1H0z"/><path fill="#0f0" d="M1 0h1v1H1z"/></svg>',
    { font: { loadSystemFonts: false } }).render().pixels;
  assert.deepEqual([...raw.slice(0, 4)], [128, 0, 0, 128]);
  const { rgb, alpha } = quantize(raw, true);
  assert.deepEqual(rgb, [0xf800, 0x07e0, 0]);
  assert.deepEqual([...alpha], [0x8f, 0x00]);
  for (const background of [[255, 255, 255], [234, 243, 255]]) {
    const reconstructed = [255, 0, 0].map((v, c) => Math.round((v * 8 + background[c] * 7) / 15));
    const expected = [128, 0, 0].map((v, c) => Math.round(v + background[c] * 127 / 255));
    reconstructed.forEach((v, c) => assert(Math.abs(v - expected[c]) <= 9));
  }
});

test('all agreed variants exist exactly once with bounded real pixel storage', () => {
  const expected = assets.flatMap(a => a.sizes.map(s => `GUI_ICON_${a.id}:${s}`)).sort();
  assert.deepEqual(manifest.variants.map(v => `${v.id}:${v.size}`).sort(), expected);
  assert.equal(new Set(expected).size, 54);
  for (const v of manifest.variants) {
    const pixels = v.width * v.height;
    assert(v.width > 0 && v.height > 0 && Math.max(v.width, v.height) <= v.size);
    assert.equal(v.alphaBytes, Math.ceil(pixels / 2));
    assert.equal(v.rgbBytes, v.format === 'RGB565+A4' ? pixels * 2 : 0);
    assert.equal(v.pixelBytes, v.alphaBytes + v.rgbBytes);
    const alpha = artAlpha.subarray(v.alphaOffset, v.alphaOffset + v.alphaBytes);
    const rgb = artRgb.slice(v.rgbOffset, v.rgbOffset + v.rgbBytes / 2);
    assert.equal(alpha.length, v.alphaBytes); assert.equal(rgb.length * 2, v.rgbBytes);
    assert.equal(hash(Buffer.concat([to565Buffer(rgb), alpha])), v.pixelSha256);
    assert(alpha.some(b => b !== 0), `${v.id}: empty raster`);
    for (let i = 0; i < pixels; ++i) assert(unpack(alpha, i) >= 0 && unpack(alpha, i) <= 15);
    if (pixels & 1) assert.equal(alpha.at(-1) & 15, 0);
  }
  assert.equal(manifest.artworkBytes.rgb565, artRgb.length * 2);
  assert.equal(manifest.artworkBytes.alpha4, artAlpha.length);
  assert.equal(manifest.artworkBytes.payload, manifest.variants.reduce((sum, v) => sum + v.pixelBytes, 0));
});

test('all five brands retain proportions, transparent edges and gradient colors', () => {
  for (const id of ['COPILOT', 'ONEDRIVE', 'EXPLORER', 'SETTINGS', 'MICROSOFT']) {
    for (const v of manifest.variants.filter(v => v.id === `GUI_ICON_${id}`)) {
      assert.equal(v.provenance.kind, 'user-provided');
      assert.notEqual(v.provenance.license, 'MIT');
      assert(Math.abs(v.height - v.width * v.viewBoxSize[1] / v.viewBoxSize[0]) <= 1);
      const alpha = artAlpha.subarray(v.alphaOffset, v.alphaOffset + v.alphaBytes);
      assert(alpha.some(b => (b >> 4) > 0 && (b >> 4) < 15), `${id}: missing antialiased edge`);
      assert(alpha.some(b => (b >> 4) === 0), `${id}: missing transparency`);
      const rgb = artRgb.slice(v.rgbOffset, v.rgbOffset + v.rgbBytes / 2);
      if (id !== 'MICROSOFT') assert(new Set(rgb).size > 24, `${id}: lost color/gradients`);
    }
  }
  const wordmark = manifest.variants.find(v => v.id === 'GUI_ICON_MICROSOFT');
  assert.deepEqual(wordmark.viewBoxSize, [337.6, 72]);
  assert.deepEqual([wordmark.width, wordmark.height], [88, 19]);
  const alpha = artAlpha.subarray(wordmark.alphaOffset, wordmark.alphaOffset + wordmark.alphaBytes);
  assert(Array.from({ length: 19 }, (_, y) => unpack(alpha, y * 88 + 86)).some(a => a > 0),
    'Right-hand wordmark lettering must not be square-cropped');
});

test('actual wordmark edges composite without a matte on white and pale blue', () => {
  const v = manifest.variants.find(v => v.id === 'GUI_ICON_MICROSOFT');
  const svg = readFileSync(path.join(here, 'sources', v.source), 'utf8');
  const raw = new Resvg(svg, { fitTo: { mode: 'width', value: 88 },
    font: { loadSystemFonts: false } }).render().pixels;
  const alpha = artAlpha.subarray(v.alphaOffset, v.alphaOffset + v.alphaBytes);
  const rgb = artRgb.slice(v.rgbOffset, v.rgbOffset + v.rgbBytes / 2);
  let edges = 0;
  for (let i = 0; i < v.width * v.height; ++i) {
    const a = raw[i * 4 + 3], a4 = unpack(alpha, i), color = rgb[i];
    if (a > 0 && a < 255) edges++;
    const channels = [((color >> 11) & 31) * 255 / 31,
      ((color >> 5) & 63) * 255 / 63, (color & 31) * 255 / 31];
    for (const background of [[255, 255, 255], [234, 243, 255]]) {
      for (let c = 0; c < 3; ++c) {
        const expected = raw[i * 4 + c] + background[c] * (255 - a) / 255;
        const actual = (channels[c] * a4 + background[c] * (15 - a4)) / 15;
        assert(Math.abs(actual - expected) <= 13, `Wordmark halo: pixel ${i}, channel ${c}`);
      }
    }
  }
  assert(edges > 50);
});

test('ASCII 95 glyph coverage, timer subset, and baseline-relative bounds', () => {
  assert.deepEqual(manifest.fonts.map(f => f.id), fonts.map(f => f.id));
  for (const f of manifest.fonts) {
    const expected = f.id === 'TIMER' ? [...'-0123456789:'].map(c => c.charCodeAt(0)) :
      Array.from({ length: 95 }, (_, i) => i + 32);
    assert.deepEqual(f.glyphs.map(g => g.character), expected);
    for (const g of f.glyphs) {
      assert.equal(g.alphaBytes, Math.ceil(g.width * g.height / 2));
      assert(g.advance > 0 && g.advance <= 255);
      assert(g.offsetX >= -128 && g.offsetX <= 127 && g.offsetY >= -128 && g.offsetY <= 127);
      assert(f.baseline + g.offsetY >= 0);
      assert(f.baseline + g.offsetY + g.height <= f.height);
      const alpha = fontAlpha.subarray(g.alphaOffset, g.alphaOffset + g.alphaBytes);
      assert.equal(alpha.length, g.alphaBytes);
      assert.equal(hash(alpha), g.pixelSha256);
      if (g.character !== 32) assert(alpha.some(b => b !== 0), `${f.id}: blank ${g.character}`);
      if ((g.width * g.height) & 1) assert.equal(alpha.at(-1) & 15, 0);
    }
    if (f.id !== 'TIMER') assert.notEqual(f.glyphs.find(g => g.character === 65).pixelSha256,
      f.glyphs.find(g => g.character === 97).pixelSha256);
    else assert.equal(new Set(f.glyphs.filter(g => g.character >= 48 && g.character <= 57).map(g => g.advance)).size, 1);
  }
  assert.equal(manifest.fontBytes.alpha4, fontAlpha.length);
});

test('32-bit target budgets include every pixel, lookup table and alignment allowance', () => {
  const art = manifest.artworkBytes, font = manifest.fontBytes;
  assert.equal(art.tables32, 54 * 20);
  assert.equal(font.tables32, 299 * 12 + 4 * 8);
  for (const b of [art, font]) {
    assert.equal(b.symbols32, b.payload + b.tables32);
    assert.equal(b.budgeted32, b.symbols32 + b.alignmentAllowance32);
    assert(b.budgeted32 <= b.limit);
  }
  assert.equal(art.limit, 48 * 1024); assert.equal(font.limit, 24 * 1024);
  assert(!/IRAM_ATTR|DRAM_ATTR|malloc|calloc|realloc|#include\s*[<"](?:fontkit|resvg)/.test(artC + fontC));
});

test('regeneration inputs are only the exact pinned subset, independent of reference/', () => {
  const files = readdirSync(path.join(here, 'sources'), { recursive: true, withFileTypes: true })
    .filter(x => x.isFile()).map(x => path.relative(path.join(here, 'sources'), path.join(x.parentPath, x.name)).replaceAll('\\', '/')).sort();
  const lock = JSON.parse(readFileSync(path.join(here, 'sources.lock.json')));
  assert.deepEqual(files, Object.keys(lock).sort());
  assert(files.includes(fontSource)); assert.equal(files.filter(f => f.endsWith('.ttf')).length, 1);
  assert.equal(files.filter(f => f.endsWith('.svg')).length, 46);
  assert(readFileSync(path.join(output, 'licenses/FLUENT-MIT.txt'), 'utf8').includes('Copyright (c) 2020 Microsoft Corporation'));
  assert(readFileSync(path.join(output, 'licenses/NOTO-OFL.txt'), 'utf8').includes('SIL OPEN FONT LICENSE Version 1.1'));
});
