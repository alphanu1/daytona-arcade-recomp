#ifndef DAYTONA_NATIVE_BOUNDARY_CHECK_H
#define DAYTONA_NATIVE_BOUNDARY_CHECK_H
#include "../include/boundary.h"
#define NM_BOUNDARY_EXPECTED_HASH UINT32_C(0x2f83822f)
#define NM_BOUNDARY_OBSERVABLE_HASH UINT32_C(0xd0a0c1f6)

static inline void nm_boundary_test_input(unsigned index, float points[8]) {
    uint32_t seed = index+1;
    for (unsigned i = 0; i < 8; ++i) {
        seed = seed*1664525u+1013904223u;
        points[i] = ((int)((seed>>12)&16383)-8192)*0.125f;
    }
    switch (index%8) {
    case 0: points[2] = points[0]; break;
    case 1: points[6] = points[4]; break;
    case 2: points[2] = points[0]; points[6] = points[4]; break;
    case 3: points[3] = points[1]; points[7] = points[5]; break;
    case 4: points[4] = points[2]; points[5] = points[3]; break;
    case 5: points[0] = -0.0f; points[2] = 0.0f; points[4] = 0.0f; break;
    case 7: points[0] = 0.0f; points[2] = -0.0f; break;
    default: break;
    }
}
static inline uint32_t nm_boundary_test_hash(const nm_drive_params_t *math) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < 65536; ++i) {
        float points[8]; nm_boundary_test_input(i,points);
        nm_boundary_hit_t result = {{17,-19},0};
        const int valid = nm_boundary_intersection(math,points,&result);
        const uint32_t values[] = {(uint32_t)valid,nm_force_bits(result.point[0]),
            nm_force_bits(result.point[1]),(uint32_t)result.hit};
        for (unsigned v = 0; v < 4; ++v) for (unsigned b = 0; b < 4; ++b)
            hash = (hash^((values[v]>>(8*b))&255))*16777619u;
    }
    return hash;
}
/* Actual pivot contract: validity and hit are always consumed; coordinates
 * only on a valid hit. Also poison carried miss coordinates to prove that
 * neither the next classification nor a successful point depends on them.
 * Keep the separate raw-state digest above for forensic comparison. */
static inline uint32_t nm_boundary_observable_test_hash(const nm_drive_params_t *math) {
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<65536;++i) {
        float points[8];nm_boundary_test_input(i,points);
        nm_boundary_hit_t result={{17,-19},0};
        const int valid=nm_boundary_intersection(math,points,&result);
        for(unsigned seed=0;seed<2;++seed) {
            nm_boundary_hit_t poisoned={{nm_force_float(seed ? 1u : UINT32_C(0x7fc12345)),
                nm_force_float(seed ? UINT32_C(0xff800000) : UINT32_C(0xffc54321))},1};
            const int check=nm_boundary_intersection(math,points,&poisoned);
            if(check!=valid || (valid && (poisoned.hit!=result.hit ||
               (result.hit && memcmp(poisoned.point,result.point,sizeof(result.point)))))) return 0;
        }
        const uint32_t values[]={(uint32_t)valid,(uint32_t)(valid && result.hit),
            valid && result.hit ? nm_force_bits(result.point[0]) : 0,
            valid && result.hit ? nm_force_bits(result.point[1]) : 0};
        for(unsigned v=0;v<4;++v) for(unsigned b=0;b<4;++b)
            hash=(hash^((values[v]>>(8*b))&255))*16777619u;
    }
    return hash;
}
#endif
