#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef enum { UI_INPUT_NONE, UI_INPUT_PING, UI_INPUT_STATUS, UI_INPUT_UP,
    UI_INPUT_DOWN, UI_INPUT_ENTER, UI_INPUT_BACK, UI_INPUT_DEMO, UI_INPUT_INVALID } ui_input_kind_t;
typedef struct { ui_input_kind_t kind; unsigned demo; } ui_input_event_t;
typedef struct { char line[96]; unsigned used; bool discard; } ui_input_parser_t;
bool ui_input_feed(ui_input_parser_t *p, unsigned char ch, ui_input_event_t *event);
/* Polls at most 96 available bytes and produces at most one command per call. */
bool gui_console_poll(ui_input_event_t *event);
void gui_console_reply(const char *constant_result);
size_t gui_console_storage_bytes(void);
