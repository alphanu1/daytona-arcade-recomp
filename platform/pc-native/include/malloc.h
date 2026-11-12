/* PC: <malloc.h> with KOS's memalign and mallinfo on every host. */
#ifndef PC_MALLOC_H
#define PC_MALLOC_H
#include <stdlib.h>
#if defined(__GLIBC__) || (defined(__has_include_next) && defined(__linux__))
#include_next <malloc.h>
#else
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *memalign(size_t alignment, size_t size);
struct mallinfo { size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
static inline struct mallinfo mallinfo(void) { struct mallinfo m = {0}; return m; }
#ifdef __cplusplus
}
#endif
#endif
#endif
