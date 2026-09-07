#include "https_operation.h"
#include <stdio.h>
#include <string.h>

bool recorder_proxy_endpoint(const char *configured, const char *path, char *out, size_t capacity)
{
    if (!configured || !path || !out || strncmp(configured, "wss://", 6) ||
        (strcmp(path, "/readyz") && strcmp(path, "/v1/recordings/process") &&
         strcmp(path, "/v1/recordings/status"))) return false;
    const char *host = configured + 6, *slash = strchr(host, '/');
    if (!slash || strcmp(slash, "/v1/voice") || slash == host || slash - host > 253)
        return false;
    if (*host == '.' || *host == '-' || slash[-1] == '.' || slash[-1] == '-') return false;
    for (const char *p = host; p < slash; ++p) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
        if (c == '.' && p + 1 < slash && (p[1] == '.' || p[1] == '-')) return false;
    }
    int n = snprintf(out, capacity, "https://%.*s%s", (int)(slash - host), host, path);
    return n > 0 && (size_t)n < capacity;
}
