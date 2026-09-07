#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bound parser stack use before invoking cJSON on processing messages. */
static inline bool recorder_json_depth_ok(const uint8_t *bytes, size_t size, unsigned maximum)
{
    bool quoted = false, escaped = false;
    unsigned depth = 0;
    for (size_t i = 0; i < size; ++i) {
        char c = (char)bytes[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > maximum) return false; }
        else if (c == '}' || c == ']') { if (!depth) return false; --depth; }
    }
    return !quoted && !depth;
}
