const brand = (id, file, original, sizes) => ({
  id, sizes, color: true, source: `brands/${file}.svg`,
  provenance: { kind: 'user-provided', original: `reference/${original}`,
    license: 'No independent license declaration supplied; see PROVENANCE.md' },
});

const mask = (id, folder, stem, native16 = true) => ({
  id, sizes: [16, 20, 24], color: false,
  sources: Object.fromEntries([16, 20, 24].map(size =>
    [size, `fluent/ic_fluent_${stem}_${size === 16 && !native16 ? 20 : size}_regular.svg`])),
  provenance: { kind: 'Fluent system icons', license: 'MIT',
    upstream: `https://github.com/microsoft/fluentui-system-icons/tree/main/assets/${encodeURIComponent(folder)}/SVG` },
});

export const assets = [
  brand('COPILOT', 'copilot', 'Microsoft_Copilot_Icon.svg', [20, 32, 48]),
  brand('ONEDRIVE', 'onedrive', 'Microsoft_OneDrive_Icon_(2025_-_present).svg', [16, 20, 32]),
  brand('EXPLORER', 'explorer', 'Windows_Explorer.svg', [20, 32]),
  brand('SETTINGS', 'settings', 'Windows_Settings_icon.svg', [20, 32]),
  brand('MICROSOFT', 'microsoft', 'Microsoft_logo_(2012).svg', [88]),
  { id: 'MIC_COLOR', sizes: [32], color: true, source: 'fluent/ic_fluent_mic_32_color.svg',
    provenance: { kind: 'Fluent system icons', license: 'MIT',
      upstream: 'https://github.com/microsoft/fluentui-system-icons/tree/main/assets/Mic/SVG' } },
  mask('MIC', 'Mic', 'mic'),
  mask('RECORD', 'Record', 'record'),
  mask('PLAY', 'Play', 'play'),
  mask('STOP', 'Stop', 'stop'),
  mask('SYNC', 'Arrow Sync', 'arrow_sync'),
  mask('WIFI', 'WiFi 1', 'wifi_1', false),
  mask('WIFI_OFF', 'WiFi Off', 'wifi_off', false),
  mask('CHECK', 'Checkmark Circle', 'checkmark_circle'),
  mask('WARNING', 'Warning', 'warning'),
  mask('SPEAKER', 'Speaker 2', 'speaker_2'),
  mask('DOCUMENT', 'Document Text', 'document_text'),
  mask('BACK', 'Chevron Left', 'chevron_left'),
  mask('UP', 'Chevron Up', 'chevron_up'),
  mask('DOWN', 'Chevron Down', 'chevron_down'),
];

export const fonts = [
  { id: 'HINT', pixels: 11, weight: 400, first: 32, last: 126, fallback: 63 },
  { id: 'BODY', pixels: 14, weight: 400, first: 32, last: 126, fallback: 63 },
  { id: 'TITLE', pixels: 16, weight: 600, first: 32, last: 126, fallback: 63 },
  { id: 'TIMER', pixels: 32, weight: 500, first: 45, last: 58, fallback: 45,
    characters: '-0123456789:' },
];

export const fontSource = 'font/NotoSans.ttf';
export const budgets = { artwork: 48 * 1024, fonts: 24 * 1024 };
