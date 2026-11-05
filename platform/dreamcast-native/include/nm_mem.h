#ifndef NM_MEM_H
#define NM_MEM_H
/* Block copies, fills and moves through SH4ZAM's memory routines on the console (sh4zam/shz_mem.h), the C library
 * on the host. Only bytes move: the same bytes land in the same places, so no result of the gameplay or of the
 * renderer can change (the host builds, whose traces and streams are the references, keep memcpy/memset).
 *
 * nm_memcpy(dst, src, bytes), nm_memset(dst, value, bytes), nm_memmove(dst, src, bytes), nm_zero(dst, bytes): any
 * size, any alignment, the same contract as the C library's. A constant size GCC moves in registers (NM_MEM_INLINE
 * bytes or fewer: a bit cast, a 3-vector, a 4x4 of shorts) stays a built-in; every other call goes to one copy of
 * the dispatcher in code/nm_mem.c, which picks the SH4ZAM form from the addresses and the size at run time:
 *   both 8-byte aligned     shz_memcpy32 over the 32-byte part when the destination is 32-byte aligned (FMOV.D
 *                           pairs, MOVCA.L allocation, prefetch), shz_memcpy8 over the rest of the 8-byte part;
 *   both 4-byte aligned     shz_memcpy4 (32 bytes a block);   both 2-byte aligned   shz_memcpy2;
 *   the same misalignment   bytes up to the first common boundary, then one of the above;
 *   the tail (the size modulo the step) and relatively misaligned buffers   shz_memcpy1.
 *   fills: bytes up to an 8-byte boundary, shz_memset8 with the byte repeated in a 64-bit word, the tail in bytes;
 *   moves: forward (destination below the source, or no overlap) through the copy forms that store after they load
 *   (never the MOVCA.L one); only a backward overlap is the C library's memmove, which SH4ZAM has no form of.
 * libgcc's __movmem_i4_even, GCC's form of a large struct assignment, moves 4 bytes a load and a store, 16 bytes in
 * 13 instructions; the C library's memcpy and memset are byte and 4-byte loops.
 * One copy of each dispatcher in the program, out of line: the instruction cache keeps one routine for every call
 * site, and only code/nm_mem.c compiles SH4ZAM's inline forms (with NDEBUG: their alignment asserts hold by
 * construction there).
 *
 * nm_copy_words(dst, src, bytes): the older name, 4-byte aligned buffers, bytes a multiple of 4, no overlap: nm_memcpy.
 * NM_COPY(dst, src): *dst = *src for one type; types of NM_COPY_LARGE bytes or more (and a multiple of 4, 4-byte
 * aligned) through the dispatcher, smaller ones by GCC's own assignment as before.
 * nm_zero8(dst, bytes): dst 8-byte aligned, bytes a multiple of 8, straight to shz_memset8 (inline: one hot caller).
 * NM_MEM_SH4ZAM: the SH4ZAM forms in an SH4 build without __DREAMCAST__ (an SH simulator build that links libsh4zam
 * and code/nm_mem.c).
 *
 * nm_alloc(bytes), nm_calloc(count, size), nm_alloc_zeroed(bytes): the allocations of the console code, every one
 * NM_ALLOC_ALIGN (32) bytes aligned (memalign on the console: a whole O-cache line, the alignment the 32-byte SH4ZAM
 * forms, the store queues and the PVR want; aligned_alloc on the host, the size rounded up as C11 requires). Freed
 * with free(). include/heap.h's named allocations (nm_heap_alloc) are aligned the same way and keep their accounting. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#define NM_MEM_INLINE 16u
#define NM_ALLOC_ALIGN 32u
#if defined(__DREAMCAST__) || defined(NM_MEM_SH4ZAM)
#include <sh4zam/shz_mem.h>
#include <malloc.h>
static inline __attribute__((unused,malloc)) void *nm_alloc(size_t bytes) { return memalign(NM_ALLOC_ALIGN,bytes ? bytes : 1u); }
void *nm_memcpy_(void *__restrict__ dst,const void *__restrict__ src,size_t bytes);
void *nm_memset_(void *dst,int value,size_t bytes);
void *nm_memmove_(void *dst,const void *src,size_t bytes);
static inline __attribute__((always_inline,unused)) void *nm_memcpy(void *__restrict__ dst,const void *__restrict__ src,size_t bytes) {
    if(__builtin_constant_p(bytes) && bytes<=NM_MEM_INLINE) return __builtin_memcpy(dst,src,bytes);
    return nm_memcpy_(dst,src,bytes);
}
static inline __attribute__((always_inline,unused)) void *nm_memset(void *dst,int value,size_t bytes) {
    if(__builtin_constant_p(bytes) && bytes<=2u*NM_MEM_INLINE) return __builtin_memset(dst,value,bytes);
    return nm_memset_(dst,value,bytes);
}
static inline __attribute__((always_inline,unused)) void *nm_memmove(void *dst,const void *src,size_t bytes) {
    if(__builtin_constant_p(bytes) && bytes<=NM_MEM_INLINE) return __builtin_memmove(dst,src,bytes);
    return nm_memmove_(dst,src,bytes);
}
/* dst 8-byte aligned, bytes a multiple of 8. */
static inline __attribute__((always_inline,unused)) void nm_zero8(void *dst,size_t bytes) { shz_memset8(dst,0,bytes); }
#else
static inline __attribute__((always_inline,unused)) void *nm_memcpy(void *__restrict__ dst,const void *__restrict__ src,size_t bytes) { return memcpy(dst,src,bytes); }
static inline __attribute__((always_inline,unused)) void *nm_memset(void *dst,int value,size_t bytes) { return memset(dst,value,bytes); }
static inline __attribute__((always_inline,unused)) void *nm_memmove(void *dst,const void *src,size_t bytes) { return memmove(dst,src,bytes); }
static inline __attribute__((always_inline,unused)) void nm_zero8(void *dst,size_t bytes) { memset(dst,0,bytes); }
static inline __attribute__((unused,malloc)) void *nm_alloc(size_t bytes) { return aligned_alloc(NM_ALLOC_ALIGN,((bytes ? bytes : 1u)+NM_ALLOC_ALIGN-1u)&~(size_t)(NM_ALLOC_ALIGN-1u)); }
#endif
static inline __attribute__((unused,malloc)) void *nm_alloc_zeroed(size_t bytes) { void *const memory=nm_alloc(bytes); if(memory) nm_memset(memory,0,bytes); return memory; }
static inline __attribute__((unused,malloc)) void *nm_calloc(size_t count,size_t size) {
    if(size && count>SIZE_MAX/size) return NULL;
    return nm_alloc_zeroed(count*size);
}
static inline __attribute__((always_inline,unused)) void *nm_zero(void *dst,size_t bytes) { return nm_memset(dst,0,bytes); }
static inline __attribute__((always_inline,unused)) void nm_copy_words(void *__restrict__ dst,const void *__restrict__ src,size_t bytes) { nm_memcpy(dst,src,bytes); }
#define NM_COPY_LARGE 64u
#define NM_COPY(dst,src) do { \
    _Static_assert(sizeof(*(dst))==sizeof(*(src)),"NM_COPY: one type"); \
    if(sizeof(*(dst))>=NM_COPY_LARGE && !(sizeof(*(dst))&3u) && __alignof__(__typeof__(*(dst)))>=4u) \
        nm_memcpy((dst),(src),sizeof(*(dst))); \
    else *(dst)=*(src); } while(0)
/* NM_ZERO(ptr): *ptr = {0} for one object, through the dispatcher when it is large. */
#define NM_ZERO(ptr) nm_zero((ptr),sizeof(*(ptr)))
#endif
