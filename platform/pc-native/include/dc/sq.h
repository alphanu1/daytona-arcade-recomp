/* PC: store queues are plain memory copies. */
#ifndef PC_DC_SQ_H
#define PC_DC_SQ_H
#include <stdint.h>
#include <string.h>
#include <kos/cdefs.h>
__BEGIN_DECLS
static inline void sq_lock(void *dest) { (void)dest; }
static inline void sq_unlock(void) {}
static inline void sq_wait(void) {}
static inline void sq_flush(void *addr) { (void)addr; }
static inline void *sq_cpy(void *dest, const void *src, size_t n) { return memcpy(dest, src, n); }
static inline void *sq_set32(void *dest, uint32_t c, size_t n) {
    uint32_t *d = (uint32_t *)dest;
    for (size_t i = 0; i < n / 4; i++) d[i] = c;
    return dest;
}
__END_DECLS
#endif
