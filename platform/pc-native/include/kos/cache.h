/* PC: no SH4 cache to manage. */
#ifndef PC_KOS_CACHE_H
#define PC_KOS_CACHE_H
#include <stdint.h>
#include <stddef.h>
static inline void dcache_flush_range(uintptr_t start, size_t count) { (void)start; (void)count; }
static inline void dcache_inval_range(uintptr_t start, size_t count) { (void)start; (void)count; }
static inline void dcache_purge_range(uintptr_t start, size_t count) { (void)start; (void)count; }
static inline void icache_flush_range(uintptr_t start, size_t count) { (void)start; (void)count; }
static inline void dcache_pref_block(const void *p) { (void)p; }
/* KOS allocates the cache line and stores the first word; on PC, the store. */
#define dcache_alloc_line_with_value(dst, value) (*(uint32_t *)(dst) = (uint32_t)(value))
#endif
