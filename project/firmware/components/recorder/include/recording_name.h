#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int64_t utc;
    int offset_minutes;
    bool clock_valid;
} recording_time_t;

bool recording_name_valid(const char *name, bool partial);
bool recording_name_format(char *out, size_t capacity, const recording_time_t *time,
                           uint64_t random_id, unsigned collision);
bool recording_timestamp(const recording_time_t *time, char out[36]);
