#ifndef DAYTONA_NATIVE_FINITE_H
#define DAYTONA_NATIVE_FINITE_H
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "nm_mem.h"

/* isfinite/isnan on a float compile to an unordered compare, which the SH4 has
 * no instruction for: every use became a call to libgcc's __unordsf2 (about
 * 0.5 ms per frame in the sampled profile). The exponent/mantissa bit tests
 * below return the same answer for every one of the 2^32 bit patterns
 * (tests/test_finite_bits.c checks all of them) without any float operation.
 * Other argument types keep the compiler's own classification. C only: C++
 * tools use std::isfinite. */
static inline int nm_float_is_finite(float value) {
    uint32_t bits; nm_memcpy(&bits,&value,sizeof(bits));
    return (bits&UINT32_C(0x7f800000))!=UINT32_C(0x7f800000);
}
static inline int nm_float_is_nan(float value) {
    uint32_t bits; nm_memcpy(&bits,&value,sizeof(bits));
    return (bits&UINT32_C(0x7fffffff))>UINT32_C(0x7f800000);
}
/* value<0 and value>0 exactly (false for zeros and NaN, true for the matching
 * infinity). Where the compiler inverts such a compare it needs "unordered or
 * ...", which is the same libgcc call; these never need it. */
static inline int nm_float_negative(float value) {
    uint32_t bits; nm_memcpy(&bits,&value,sizeof(bits));
    return bits-UINT32_C(0x80000001)<UINT32_C(0x7f800000);
}
static inline int nm_float_positive(float value) {
    uint32_t bits; nm_memcpy(&bits,&value,sizeof(bits));
    return bits-UINT32_C(1)<UINT32_C(0x7f800000);
}
#ifndef __cplusplus
#undef isfinite
#define isfinite(x) (sizeof(x)==sizeof(float) ? nm_float_is_finite((float)(x)) : __builtin_isfinite(x))
#undef isnan
#define isnan(x) (sizeof(x)==sizeof(float) ? nm_float_is_nan((float)(x)) : __builtin_isnan(x))
#endif
#endif
