#pragma once
#include <stddef.h>

#define RECORDER_HTTP_MAX_URL 4096u
#define RECORDER_HTTP_MAX_BEARER 8192u

static inline size_t recorder_http_length(const char *value, size_t maximum)
{
    size_t length = 0;
    if (value) {
        while (length <= maximum && value[length]) ++length;
    }
    return length;
}

/* ESP-IDF cannot split one oversized header or request line across TX buffers. */
static inline int recorder_http_tx_size(const char *url, const char *bearer)
{
    size_t url_length = recorder_http_length(url, RECORDER_HTTP_MAX_URL);
    size_t token_length = recorder_http_length(bearer, RECORDER_HTTP_MAX_BEARER);
    if (!url_length || url_length > RECORDER_HTTP_MAX_URL ||
        token_length > RECORDER_HTTP_MAX_BEARER) return 0;
    size_t required = url_length + token_length + 512u;
    if (required < 1024u) required = 1024u;
    return (int)required; /* Bounded above by 12800 bytes. */
}
