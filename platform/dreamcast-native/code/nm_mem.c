/* The memory dispatchers of include/nm_mem.h: one copy in the program, the SH4ZAM forms chosen from the addresses
 * and the size (the header says which form each case takes). Console only; a host build has nothing here (the
 * header maps the calls to the C library). The Makefile compiles this unit with -DNDEBUG: SH4ZAM's inline forms
 * assert their alignment and size requirements, which hold by construction below (a #define here would come too
 * late: the build's -include of finite.h has read the SH4ZAM headers already).
 * The C library's memmove below is the one call of this file that must stay the library's: the wrappers of the
 * header would come back here. */
#if defined(__DREAMCAST__) || defined(NM_MEM_SH4ZAM)
#include "nm_mem.h"

/* The forward copy of the common alignment of dst and src, bytes in the tail. allow32: the MOVCA.L form over the
 * 32-byte part (not for overlapping moves: it allocates the destination's cache line before the source bytes in it
 * are read). */
static inline __attribute__((always_inline)) void nm_copy_aligned(uint8_t *d,const uint8_t *s,size_t n,int allow32) {
    const uintptr_t both=(uintptr_t)d|(uintptr_t)s;
    size_t body;
    if(!(both&7u)) {
        if(allow32 && n>=32u && !((uintptr_t)d&31u)) {
            body=n&~(size_t)31u; shz_memcpy32(d,s,body); d+=body; s+=body; n-=body;
        }
        body=n&~(size_t)7u;
        if(body) { shz_memcpy8(d,s,body); d+=body; s+=body; n-=body; }
    } else if(!(both&3u)) {
        body=n&~(size_t)3u;
        if(body) { shz_memcpy4(d,s,body); d+=body; s+=body; n-=body; }
    } else if(!(both&1u)) {
        body=n&~(size_t)1u;
        if(body) { shz_memcpy2(d,s,body); d+=body; s+=body; n-=body; }
    }
    if(n) shz_memcpy1(d,s,n);
}

/* Bytes up to the first boundary the two addresses share (none when their misalignment differs: all bytes). */
static inline __attribute__((always_inline)) size_t nm_prelude(const uint8_t *d,const uint8_t *s,size_t n) {
    const uintptr_t relative=(uintptr_t)d^(uintptr_t)s;
    size_t prelude;
    if(!(relative&7u)) prelude=(size_t)(-(uintptr_t)d)&7u;
    else if(!(relative&3u)) prelude=(size_t)(-(uintptr_t)d)&3u;
    else if(!(relative&1u)) prelude=(size_t)(-(uintptr_t)d)&1u;
    else prelude=n;
    return prelude<n ? prelude : n;
}

static __attribute__((noinline)) void nm_copy_forward(uint8_t *d,const uint8_t *s,size_t n,int allow32) {
    const size_t prelude=nm_prelude(d,s,n);
    if(prelude) { shz_memcpy1(d,s,prelude); d+=prelude; s+=prelude; n-=prelude; }
    nm_copy_aligned(d,s,n,allow32);
}

void *nm_memcpy_(void *__restrict__ dst,const void *__restrict__ src,size_t bytes) {
    nm_copy_forward((uint8_t *)dst,(const uint8_t *)src,bytes,1);
    return dst;
}

void *nm_memmove_(void *dst,const void *src,size_t bytes) {
    const uintptr_t d=(uintptr_t)dst,s=(uintptr_t)src;
    if(d-s>=bytes) nm_copy_forward((uint8_t *)dst,(const uint8_t *)src,bytes,d<s ? 0 : 1); /* below the source, or apart */
    else memmove(dst,src,bytes);                                                                /* a backward overlap */
    return dst;
}

void *nm_memset_(void *dst,int value,size_t bytes) {
    uint8_t *d=(uint8_t *)dst;
    const uint8_t byte=(uint8_t)value;
    if(bytes>=16u) {
        size_t prelude=(size_t)(-(uintptr_t)d)&7u;
        bytes-=prelude;
        while(prelude--) *d++=byte;
        const size_t body=bytes&~(size_t)7u;
        if(body) { shz_memset8(d,0x0101010101010101ull*byte,body); d+=body; bytes-=body; }
    }
    while(bytes--) *d++=byte;
    return dst;
}
#else
typedef int nm_mem_host_build;
#endif
