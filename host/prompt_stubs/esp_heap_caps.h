/* Host stub: there is no PSRAM here, and the allocation is plain malloc. */
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
static inline void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return malloc(size);
}
