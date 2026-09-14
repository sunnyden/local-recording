#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    GUI_ICON_MIC_COLOR, GUI_ICON_MIC, GUI_ICON_RECORD, GUI_ICON_PLAY,
    GUI_ICON_STOP, GUI_ICON_SYNC, GUI_ICON_WIFI, GUI_ICON_WIFI_OFF,
    GUI_ICON_CHECK, GUI_ICON_WARNING, GUI_ICON_SPEAKER, GUI_ICON_DOCUMENT,
    GUI_ICON_BACK, GUI_ICON_UP, GUI_ICON_DOWN,
    GUI_ICON_COPILOT, GUI_ICON_ONEDRIVE, GUI_ICON_EXPLORER,
    GUI_ICON_SETTINGS, GUI_ICON_MICROSOFT
} gui_icon_id_t;

typedef struct {
    uint16_t width, height;
    const uint16_t *rgb565; /* Native-endian, straight R5G6B5; NULL for a tintable mask. */
    const uint8_t *alpha4; /* Continuous row-major nibbles, high first; NULL means opaque. */
} gui_bitmap_t;

typedef enum {
    GUI_FONT_HINT, GUI_FONT_BODY, GUI_FONT_TITLE, GUI_FONT_TIMER
} gui_font_id_t;

typedef struct {
    uint8_t width, height, advance;
    int8_t offset_x, offset_y; /* Bitmap origin relative to glyph baseline. */
    const uint8_t *alpha4;
} gui_glyph_t;

typedef struct {
    uint8_t height, baseline, first, last; /* Inclusive character range; baseline from line top. */
    const gui_glyph_t *glyphs;
} gui_font_t;

/* Sizes are manifest keys, not square dimensions. Missing IDs/sizes return NULL. */
const gui_bitmap_t *gui_asset_get(gui_icon_id_t icon, unsigned size);
/* Invalid font IDs return NULL. All data has static, read-only lifetime. */
const gui_font_t *gui_font_get(gui_font_id_t font);
/* Unsupported characters use '?', or '-' for TIMER (including its '.'/'/' holes).
 * NULL/malformed font input returns NULL. No shaping or kerning is performed. */
const gui_glyph_t *gui_font_glyph(const gui_font_t *font, unsigned character);
/* Exact sizeof sums for pixels and lookup tables on the compiling ABI.
 * Excludes code and linker padding; manifest budgets reserve alignment separately. */
size_t gui_assets_bytes(void);
size_t gui_fonts_bytes(void);
