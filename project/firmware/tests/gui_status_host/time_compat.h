#pragma once
#ifdef _WIN32
#include <time.h>
static inline struct tm *gui_status_gmtime_r(const time_t *clock, struct tm *out)
{
    return gmtime_s(out, clock) == 0 ? out : NULL;
}
#define gmtime_r gui_status_gmtime_r
#endif
