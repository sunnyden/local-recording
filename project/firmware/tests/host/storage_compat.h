#pragma once
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <time.h>
static inline struct tm *recording_gmtime_r(const time_t *time, struct tm *out)
{
    return gmtime_s(out, time) == 0 ? out : NULL;
}
#define gmtime_r recording_gmtime_r
#define fsync _commit
#define ftruncate _chsize
#define open(path, flags, mode) _open(path, (flags) | _O_BINARY, mode)
#endif
#define RECORDING_DIR "recording-test-data"
