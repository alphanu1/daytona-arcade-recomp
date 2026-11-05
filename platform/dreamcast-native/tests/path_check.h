#ifndef DAYTONA_NATIVE_PATH_CHECK_H
#define DAYTONA_NATIVE_PATH_CHECK_H
#include "../include/path.h"

static inline unsigned nm_path_test_count(const nm_path_pack_t *p) { return (p->count+1)*20; }
static inline void nm_path_test_input(const nm_path_pack_t *p, unsigned i,
                                      float *x, float *z, int *reverse, nm_path_cursor_t *s) {
    const unsigned row = i/20, variant = i%10;
    *reverse = i%20 >= 10;
    const unsigned test = *reverse ? (row == 0 ? p->count : row-1) : row;
    const nm_path_row_t *r = p->rows+test;
    *x = r->xyz[0]+(float)((int)(variant%3)-1);
    *z = r->xyz[2]+(float)((int)(variant/3)-1);
    if (variant == 9) { *x = nextafterf(r->xyz[0],INFINITY); *z = nextafterf(r->xyz[2],-INFINITY); }
    *s = (nm_path_cursor_t){(uint16_t)(row*28),0xa5a5,0xa5a50000u|((i*73)&65535)};
}
static inline uint32_t nm_path_hash_state(uint32_t hash, const nm_path_cursor_t *s) {
    const unsigned char *bytes = (const unsigned char *)s;
    for (unsigned i = 0; i < sizeof(*s); ++i) hash = (hash^bytes[i])*16777619u;
    return hash;
}
static inline uint32_t nm_path_test_hash(const nm_path_pack_t *p, const nm_drive_params_t *math) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < nm_path_test_count(p); ++i) {
        nm_path_cursor_t s; float x,z; int reverse;
        nm_path_test_input(p,i,&x,&z,&reverse,&s);
        if (!nm_path_cursor_step(p,math,x,z,reverse,&s)) return 0;
        hash = nm_path_hash_state(hash,&s);
    }
    return hash;
}
static inline uint32_t nm_path_expected_hash(unsigned slot, unsigned route) {
    const uint32_t hashes[3][8] = {
        {0x01c6fb89,0xaef11e41,0x361b28d1,0xaef11e41,0x01c6fb89,0x50af5fc5,0x01c6fb89,0x964bbd21},
        {0x26cc4345,0xff66a835,0x15b505f5,0xff66a835,0x178048f9,0x7bcf11e1,0x228bde71,0x7bcf11e1},
        {0x41587251,0xee70d751,0xce124336,0x6258ee99,0x820948d1,0x28d7eb1a,0xa102397d,0x7806c122}
    };
    return slot < 3 && route < 8 ? hashes[slot][route] : 0;
}
#endif
