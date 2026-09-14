#pragma once
#include <stddef.h>
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
void *heap_caps_malloc(size_t size,unsigned caps);
unsigned heap_caps_get_free_size(unsigned caps);
unsigned heap_caps_get_minimum_free_size(unsigned caps);
unsigned heap_caps_get_largest_free_block(unsigned caps);
