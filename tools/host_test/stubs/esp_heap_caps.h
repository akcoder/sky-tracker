#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
extern size_t host_psram_allocs;
inline void *heap_caps_malloc(size_t n, int) { host_psram_allocs++; return malloc(n); }
inline void *heap_caps_realloc(void *p, size_t n, int) { return realloc(p, n); }
inline void heap_caps_free(void *p) { free(p); }
inline void *heap_caps_aligned_alloc(size_t a, size_t n, int) { host_psram_allocs++; return aligned_alloc(a, (n + a - 1) / a * a); }
inline size_t heap_caps_get_free_size(int) { return 6u << 20; }
inline size_t heap_caps_get_largest_free_block(int) { return 4u << 20; }
