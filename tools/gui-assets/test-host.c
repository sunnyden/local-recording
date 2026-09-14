#include "gui_assets.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned alpha_at(const uint8_t *alpha, unsigned pixel)
{
    return (alpha[pixel / 2] >> ((pixel & 1) ? 0 : 4)) & 15;
}

static size_t test_bitmap(gui_icon_id_t id, unsigned size, int color)
{
    const gui_bitmap_t *b = gui_asset_get(id, size);
    assert(b && b->width && b->height && b->alpha4);
    assert(b->width <= size && b->height <= size);
    assert((b->rgb565 != NULL) == color);
    unsigned pixels = (unsigned)b->width * b->height, nonzero = 0;
    for (unsigned i = 0; i < pixels; ++i) nonzero += alpha_at(b->alpha4, i) != 0;
    assert(nonzero);
    if (pixels & 1) assert((b->alpha4[pixels / 2] & 15) == 0);
    return (pixels + 1) / 2 + (color ? pixels * 2 : 0);
}

int main(void)
{
    size_t art_payload = 0, font_payload = 0;
    for (int icon = GUI_ICON_MIC; icon <= GUI_ICON_DOWN; ++icon)
        for (unsigned size = 16; size <= 24; size += 4)
            art_payload += test_bitmap((gui_icon_id_t)icon, size, 0);
    const struct { gui_icon_id_t id; unsigned size; } colors[] = {
        { GUI_ICON_COPILOT, 20 }, { GUI_ICON_COPILOT, 32 }, { GUI_ICON_COPILOT, 48 },
        { GUI_ICON_ONEDRIVE, 16 }, { GUI_ICON_ONEDRIVE, 20 }, { GUI_ICON_ONEDRIVE, 32 },
        { GUI_ICON_EXPLORER, 20 }, { GUI_ICON_EXPLORER, 32 },
        { GUI_ICON_SETTINGS, 20 }, { GUI_ICON_SETTINGS, 32 },
        { GUI_ICON_MICROSOFT, 88 }, { GUI_ICON_MIC_COLOR, 32 }
    };
    for (size_t i = 0; i < sizeof colors / sizeof colors[0]; ++i)
        art_payload += test_bitmap(colors[i].id, colors[i].size, 1);
    assert(gui_asset_get(GUI_ICON_MICROSOFT, 88)->width == 88);
    assert(gui_asset_get(GUI_ICON_MICROSOFT, 88)->height == 19);
    assert(!gui_asset_get((gui_icon_id_t)-1, 20));
    assert(!gui_asset_get((gui_icon_id_t)999, 20));
    assert(!gui_asset_get(GUI_ICON_COPILOT, 21));
    assert(!gui_asset_get(GUI_ICON_COPILOT, 0));
    assert(!gui_asset_get(GUI_ICON_MICROSOFT, 32));
    assert(!gui_font_get((gui_font_id_t)-1));
    assert(!gui_font_get((gui_font_id_t)4));
    assert(!gui_font_glyph(NULL, 'A'));
    gui_font_t bad = {0};
    assert(!gui_font_glyph(&bad, 'A'));

    for (int id = GUI_FONT_HINT; id <= GUI_FONT_TIMER; ++id) {
        const gui_font_t *f = gui_font_get((gui_font_id_t)id);
        unsigned fallback = id == GUI_FONT_TIMER ? '-' : '?';
        assert(f && f->height && f->baseline <= f->height);
        assert(gui_font_glyph(f, 0) == gui_font_glyph(f, fallback));
        assert(gui_font_glyph(f, 127) == gui_font_glyph(f, fallback));
        assert(gui_font_glyph(f, 0x10ffff) == gui_font_glyph(f, fallback));
        assert(gui_font_glyph(f, UINT32_MAX) == gui_font_glyph(f, fallback));
        for (unsigned c = f->first; c <= f->last; ++c) {
            const gui_glyph_t *g = gui_font_glyph(f, c);
            assert(g && g->advance && g->offset_y + f->baseline >= 0);
            assert(g->offset_y + f->baseline + g->height <= f->height);
            unsigned pixels = (unsigned)g->width * g->height;
            if (pixels) {
                assert(g->alpha4);
                unsigned nonzero = 0;
                for (unsigned p = 0; p < pixels; ++p) nonzero += alpha_at(g->alpha4, p) != 0;
                assert(nonzero);
                if (pixels & 1) assert((g->alpha4[pixels / 2] & 15) == 0);
            } else {
                assert(c == ' ' && !g->alpha4);
            }
            if (id != GUI_FONT_TIMER || (c != '.' && c != '/')) font_payload += (pixels + 1) / 2;
        }
        if (id == GUI_FONT_TIMER) {
            assert(gui_font_glyph(f, '.')->alpha4 == gui_font_glyph(f, '-')->alpha4);
            assert(gui_font_glyph(f, '/')->alpha4 == gui_font_glyph(f, '-')->alpha4);
            for (unsigned c = '0'; c <= '9'; ++c)
                assert(gui_font_glyph(f, c)->advance == gui_font_glyph(f, '0')->advance);
        } else {
            assert(f->first == 32 && f->last == 126);
            assert(gui_font_glyph(f, 'A')->alpha4 != gui_font_glyph(f, 'a')->alpha4);
        }
    }
    struct variant_layout { gui_icon_id_t id; unsigned size; gui_bitmap_t bitmap; };
    assert(gui_assets_bytes() == art_payload + 54 * sizeof(struct variant_layout));
    assert(gui_fonts_bytes() == font_payload + 299 * sizeof(gui_glyph_t) + 4 * sizeof(gui_font_t));
    assert(gui_assets_bytes() <= 48 * 1024 && gui_fonts_bytes() <= 24 * 1024);
    printf("PASS: 54 asset variants, Latin95 x3, timer12; art=%zu fonts=%zu (host ABI, tables included)\n",
           gui_assets_bytes(), gui_fonts_bytes());
    return 0;
}
