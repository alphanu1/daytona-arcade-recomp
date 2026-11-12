/* PC: KOS's heap queries, for the game's heap reports.  The host heap has no
 * fixed top: mm_sbrk reports 0 and _arch_mem_top a 4 GiB ceiling. */
#ifndef PC_KOS_MM_H
#define PC_KOS_MM_H
#include <stdint.h>
#include <malloc.h>
static inline void *mm_sbrk(unsigned long increment) { (void)increment; return (void *)0; }
#define _arch_mem_top ((uintptr_t)0xffffffffu)
#endif
