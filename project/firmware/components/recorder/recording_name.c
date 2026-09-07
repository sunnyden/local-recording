#include "recording_name.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static bool digits(const char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}
static unsigned decimal(const char *s, size_t n)
{
    unsigned value = 0;
    for (size_t i = 0; i < n; ++i) value = value * 10 + (unsigned)(s[i] - '0');
    return value;
}
static bool date_valid(const char *name)
{
    unsigned year = decimal(name + 15, 4), month = decimal(name + 19, 2);
    unsigned day = decimal(name + 21, 2);
    static const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 2024 || month < 1 || month > 12 || day < 1) return false;
    unsigned maximum = days[month - 1] +
        (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    return day <= maximum && decimal(name + 24, 2) < 24 &&
        decimal(name + 26, 2) < 60 && decimal(name + 28, 2) < 60;
}
bool recording_name_valid(const char *name, bool partial)
{
    if (!name) return false;
    size_t n = strlen(name), ext = partial ? 5 : 4;
    if (n > 64 || n <= ext || strcmp(name + n - ext, partial ? ".part" : ".wav"))
        return false;
    size_t stem = n - ext;
    if (!strncmp(name, "rec-", 4)) {
        if (stem <= 4) return false;
        for (size_t i = 4; i < stem; ++i)
            if (!((name[i] >= 'a' && name[i] <= 'z') ||
                  (name[i] >= '0' && name[i] <= '9') || name[i] == '-')) return false;
        return true;
    }
    if (strncmp(name, "AudioRecording_", 15)) return false;
    size_t base;
    if (!strncmp(name + 15, "UNTIMED_", 8)) {
        base = 39;
        if (stem < base) return false;
        for (size_t i = 23; i < base; ++i)
            if (!((name[i] >= '0' && name[i] <= '9') ||
                  (name[i] >= 'a' && name[i] <= 'f'))) return false;
    } else {
        base = 30;
        if (stem < base || !digits(name + 15, 8) || name[23] != '_' ||
            !digits(name + 24, 6) || !date_valid(name)) return false;
    }
    return stem == base || (stem == base + 4 && name[base] == '_' &&
        digits(name + base + 1, 3) && strncmp(name + base + 1, "000", 3));
}
static bool local_time(const recording_time_t *stamp, struct tm *local)
{
    if (!stamp || !stamp->clock_valid || stamp->utc <= 1735689600 ||
        stamp->utc > 253402214399LL || stamp->offset_minutes < -720 ||
        stamp->offset_minutes > 840) return false;
    time_t shifted = (time_t)(stamp->utc + stamp->offset_minutes * 60);
    return gmtime_r(&shifted, local) != NULL;
}
bool recording_name_format(char *out, size_t capacity, const recording_time_t *stamp,
                           uint64_t random_id, unsigned collision)
{
    if (!out || !stamp || collision > 999) return false;
    char stem[48], suffix[5] = "";
    struct tm local;
    if (stamp->clock_valid) {
        if (!local_time(stamp, &local) ||
            !strftime(stem, sizeof(stem), "AudioRecording_%Y%m%d_%H%M%S", &local))
            return false;
    } else {
        snprintf(stem, sizeof(stem), "AudioRecording_UNTIMED_%016llx",
                 (unsigned long long)random_id);
    }
    if (collision) snprintf(suffix, sizeof(suffix), "_%03u", collision);
    int n = snprintf(out, capacity, "%s%s.wav", stem, suffix);
    return n > 0 && (size_t)n < capacity;
}
bool recording_timestamp(const recording_time_t *stamp, char out[36])
{
    struct tm local;
    out[0] = 0;
    if (!local_time(stamp, &local)) return false;
    char date[24];
    if (!strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &local)) return false;
    int offset = stamp->offset_minutes, magnitude = offset < 0 ? -offset : offset;
    snprintf(out, 36, "%s%c%02d:%02d", date, offset < 0 ? '-' : '+',
             magnitude / 60, magnitude % 60);
    return true;
}
