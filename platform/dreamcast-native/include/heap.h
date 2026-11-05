#ifndef DAYTONA_NATIVE_HEAP_H
#define DAYTONA_NATIVE_HEAP_H
/* Named allocations of load time and the heap report.
 *
 * The console has 16 MiB: the image at 0x8c010000, then one heap that grows
 * by sbrk up to 64 KiB (THD_KERNEL_STACK_SIZE) below 0x8d000000. KOS's malloc
 * is dlmalloc 2.7: memory freed below a live block stays inside the heap, and
 * memalign() of a block that was just freed does not fit the hole it left
 * (it asks for the size plus the alignment plus a minimal chunk), so a table
 * that is freed and built again grows the heap by its whole size.
 * Rules that follow, kept by the loaders:
 * - a table is allocated once, at its final size;
 * - scratch memory of a load step is small and freed before the step returns;
 * - nothing large is held only until the end of the load.
 * tools/heap_budget.py computes the heap of a build from its packs with the
 * same order and sizes and fails above a stated budget; nm_heap_report prints
 * the measured numbers in the form that tool prints its expectation.
 *
 * nm_heap_alloc returns memory like memalign, 32-byte aligned at least (an
 * alignment below NM_ALLOC_ALIGN, 0 included, is raised to it: every block
 * starts on an O-cache line and the 32-byte SH4ZAM forms apply to it; a larger
 * one is kept); a refusal names the owner, the size and the state of the heap.
 * Free with free(). Host builds allocate from the C library and report nothing.
 * make NATIVE_HEAP_TRACE=1 prints every named allocation of 16 KiB or more
 * with its address. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "nm_mem.h"
#ifdef __DREAMCAST__
#include <malloc.h>
#include <arch/arch.h>
#include <arch/stack.h>
#include <kos/mm.h>
/* One object in the program: the highest heap end any named allocation or
 * report has seen. */
__attribute__((weak)) uintptr_t nm_heap_high;
typedef struct { uintptr_t top, limit, high; size_t in_use, free_inside, uncommitted; } nm_heap_state_t;
static inline nm_heap_state_t nm_heap_state(void) {
    const struct mallinfo info=mallinfo();
    nm_heap_state_t state;
    state.top=(uintptr_t)mm_sbrk(0);
    /* The ceiling of KOS's mm_sbrk. */
    state.limit=(uintptr_t)_arch_mem_top-THD_KERNEL_STACK_SIZE;
    if(state.top>nm_heap_high) nm_heap_high=state.top;
    state.high=nm_heap_high;
    state.in_use=(size_t)info.uordblks; state.free_inside=(size_t)info.fordblks;
    state.uncommitted=state.top<state.limit ? state.limit-state.top : 0;
    return state;
}
/* "free" is what a request can still get: the heap's own free blocks (in
 * pieces: fragmentation applies) plus the memory the heap has not taken. */
static inline void nm_heap_report(const char *label) {
    const nm_heap_state_t state=nm_heap_state();
    printf("Native heap %s: top=%08lx high-water=%08lx limit=%08lx; in use=%lu free=%lu (inside the heap %lu, never taken %lu); "
        "least ever free above the heap=%lu\n",label,(unsigned long)state.top,(unsigned long)state.high,(unsigned long)state.limit,
        (unsigned long)state.in_use,(unsigned long)(state.free_inside+state.uncommitted),(unsigned long)state.free_inside,
        (unsigned long)state.uncommitted,(unsigned long)(state.limit-state.high));
}
static __attribute__((noinline,unused)) void nm_heap_refused(const char *owner,size_t alignment,size_t bytes) {
    const nm_heap_state_t state=nm_heap_state();
    printf("FAIL: out of memory: %s needs %lu bytes (alignment %lu); heap top=%08lx limit=%08lx, in use=%lu, "
        "free inside the heap=%lu (fragmented), never taken=%lu\n",owner,(unsigned long)bytes,(unsigned long)alignment,
        (unsigned long)state.top,(unsigned long)state.limit,(unsigned long)state.in_use,(unsigned long)state.free_inside,
        (unsigned long)state.uncommitted);
}
static inline void *nm_heap_alloc(const char *owner,size_t alignment,size_t bytes) {
    if(alignment<NM_ALLOC_ALIGN) alignment=NM_ALLOC_ALIGN;
    void *const memory=memalign(alignment,bytes ? bytes : alignment);
    if(!memory) { nm_heap_refused(owner,alignment,bytes); return NULL; }
    const uintptr_t top=(uintptr_t)mm_sbrk(0);
    if(top>nm_heap_high) nm_heap_high=top;
#ifdef NATIVE_HEAP_TRACE
    if(bytes>=16u*1024u) printf("HEAP %08lx %lu %s\n",(unsigned long)(uintptr_t)memory,(unsigned long)bytes,owner);
#endif
    return memory;
}
#else
static inline void nm_heap_report(const char *label) { (void)label; }
static inline void *nm_heap_alloc(const char *owner,size_t alignment,size_t bytes) {
    (void)owner;
    if(alignment<NM_ALLOC_ALIGN) alignment=NM_ALLOC_ALIGN;
    return aligned_alloc(alignment,((bytes ? bytes : alignment)+alignment-1)&~(alignment-1));
}
#endif
/* Zeroed, like calloc. */
static inline void *nm_heap_zeroed(const char *owner,size_t alignment,size_t bytes) {
    void *const memory=nm_heap_alloc(owner,alignment,bytes);
    if(memory) nm_memset(memory,0,bytes);
    return memory;
}
#endif
