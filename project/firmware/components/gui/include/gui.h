#pragma once
#include "gui_assets.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum { GUI_HOME, GUI_RECORDING, GUI_RECORDINGS, GUI_PLAYBACK,
    GUI_SYNC, GUI_AI, GUI_SETUP } gui_screen_t;
typedef struct { int x, y, w, h; } gui_rect_t;
typedef struct { uint16_t *pixels; gui_rect_t clip; } gui_canvas_t;
typedef struct {
    gui_screen_t screen;
    unsigned selected, page_selected, count, page_start;
    bool demo, online, sd_ok, time_ok, active, error, playback, pressure, calibration;
    uint8_t level;
    uint32_t seconds, total_seconds, pre_roll_seconds, rolling_seconds, generation;
    bool rolling_active;
    unsigned progress, expires;
    char title[40], detail[80], note[80], filename[65];
    char names[3][65], receipts[3][40];
} gui_model_t;
/* This buffer is private and never part of telemetry/model snapshots. */
typedef struct { bool active; char service[33], username[25], password[25]; } gui_secret_t;
typedef struct { gui_rect_t rect; int next_y; bool dirty; } gui_dirty_t;

uint16_t gui_rgb(unsigned rgb);
bool gui_clip(gui_rect_t *rect, gui_rect_t bounds);
void gui_fill(gui_canvas_t *c, gui_rect_t r, uint16_t color);
void gui_outline(gui_canvas_t *c, gui_rect_t r, uint16_t color);
int gui_text_width(gui_font_id_t font, const char *text);
void gui_text(gui_canvas_t *c, int x, int y, int width, gui_font_id_t font,
              const char *text, uint16_t color);
void gui_icon(gui_canvas_t *c, int x, int y, gui_icon_id_t icon, unsigned size, uint16_t tint);
void gui_render(gui_canvas_t *c, const gui_model_t *model, const gui_secret_t *secret);
void gui_invalidate(gui_dirty_t *d, gui_rect_t r);
bool gui_next_strip(const gui_dirty_t *d, gui_rect_t *r);
void gui_strip_done(gui_dirty_t *d, gui_rect_t r);
void gui_secret_clear(gui_secret_t *secret);
bool gui_secret_set(gui_secret_t *secret, const char *service, const char *user, const char *pass);
unsigned gui_move(unsigned selected, unsigned count, bool up);
const char *gui_screen_name(gui_screen_t screen);
void gui_app_run(int sd_error, int audio_error, unsigned repaired, unsigned failed);
