#include "contact.h"
#include <stdio.h>
#include <stdlib.h>
#include "nm_mem.h"
#ifdef __DREAMCAST__
/* Existing libgcc binary32 helpers use the normal m4-single float ABI and
 * integer arithmetic internally. No hardware-double or FPSCR mode change. */
extern float __mulsf3(float,float);
extern float __addsf3(float,float);
extern float __subsf3(float,float);
#endif

_Static_assert(sizeof(nm_contact_quad_t) == 72, "DNC3 quad layout");

void nm_contact_unload(nm_contact_pack_t *p) {
    if (p) { free(p->storage); nm_memset(p,0,sizeof(*p)); }
}

int nm_contact_load(nm_contact_pack_t *p, const char *path) {
    const uint32_t endian = 1;
    if (!p || p->storage || !path || *(const uint8_t *)&endian != 1) return 0;
    FILE *f = fopen(path,"rb");
    if (!f) return 0;
    nm_contact_pack_t next = {0};
    uint32_t h[8];
    int ok = !setvbuf(f,NULL,_IOFBF,64*1024) && fread(h,1,sizeof(h),f) == sizeof(h) &&
        h[0] == 0x33434e44 && h[2] < 4 && h[3] && h[3] <= 65536 &&
        h[4] && h[4] <= 8192 && h[5] && h[5] <= 33 && !h[6] && !h[7];
    if (ok) {
        const uint32_t quad_offset = (32768+2*h[3]+3)&~3u;
        const uint32_t bytes = quad_offset+sizeof(nm_contact_quad_t)*h[4];
        ok = h[1] == sizeof(h)+bytes;
        if (ok) {
            next.storage = nm_alloc(bytes);
            ok = next.storage && fread(next.storage,1,bytes,f) == bytes &&
                fgetc(f) == EOF && !ferror(f);
            next.payload_bytes = bytes;
            next.slot = h[2]; next.list_words = h[3]; next.quad_count = h[4]; next.max_candidates = h[5];
            next.grid = next.storage;
            if (next.storage) {
                next.lists = next.grid+16384;
                next.quads = (const nm_contact_quad_t *)((const uint8_t *)next.storage+quad_offset);
            }
        }
    }
    if (fclose(f)) ok = 0;
    if (ok) {
        /* Validate starts, not just ranges: an ID word is not a list header. */
        uint8_t starts[8192] = {0};
        unsigned at = 0, peak = 0;
        while (at < next.list_words && ok) {
            starts[at>>3] |= (uint8_t)(1u<<(at&7));
            const unsigned count = next.lists[at++];
            if (count > next.max_candidates || count > next.list_words-at) { ok = 0; break; }
            if (count > peak) peak = count;
            for (unsigned i = 0; i < count; ++i)
                if (next.lists[at++] >= next.quad_count) ok = 0;
        }
        if (peak != next.max_candidates) ok = 0;
        for (unsigned i = 0; i < 16384; ++i) {
            unsigned start = next.grid[i];
            if (start >= next.list_words || !(starts[start>>3]&(1u<<(start&7)))) ok = 0;
        }
        if ((next.list_words&1) && next.lists[next.list_words]) ok = 0;
        for (unsigned i = 0; i < next.quad_count; ++i) {
            if (next.quads[i].surface > 255) ok = 0;
            if (!isfinite(next.quads[i].recovery_height)) ok = 0;
            for (unsigned a = 0; a < 3; ++a) if (!isfinite(next.quads[i].recovery_center[a])) ok = 0;
            for (unsigned v = 0; v < 4; ++v) for (unsigned a = 0; a < 3; ++a)
                if (!isfinite(next.quads[i].xyz[v][a])) ok = 0;
        }
    }
    if (!ok) { nm_contact_unload(&next); return 0; }
    *p = next;
    return 1;
}

int nm_contact_cell(float x, float z, unsigned *cell) {
    int32_t ix, iz;
    if (!cell || !isfinite(x) || !isfinite(z) ||
        !nm_drive_round_i32(x,&ix) || !nm_drive_round_i32(z,&iz)) return 0;
    *cell = ((((uint32_t)ix+1024)>>4)&127) | (((((uint32_t)iz+1024)>>4)&127)<<7);
    return 1;
}

int nm_contact_contains(const float xyz[4][3], float x, float z) {
    if (!xyz || !isfinite(x) || !isfinite(z)) return -1;
#ifdef __DREAMCAST__
    const uint32_t xb=nm_force_bits(x)&0x7fffffffu,zb=nm_force_bits(z)&0x7fffffffu;
    if((xb && xb<0x00800000u) || (zb && zb<0x00800000u)) {
        /* DN=1 loses tiny edge signs. Preserve the SAME ordered binary32
         * operations for subnormal queries, not an epsilon or a new polygon
         * predicate. Normal gameplay coordinates keep the fast path below. */
        for(unsigned i=0;i<4;++i) {
            const float *a=xyz[i],*b=xyz[(i+1)&3];
            float edge=__subsf3(__mulsf3(z,a[0]),__mulsf3(x,a[2]));
            edge=__subsf3(edge,__mulsf3(z,b[0]));edge=__addsf3(edge,__mulsf3(x,b[2]));
            edge=__subsf3(edge,__mulsf3(b[2],a[0]));edge=__addsf3(edge,__mulsf3(b[0],a[2]));
            const uint32_t bits=nm_force_bits(edge),magnitude=bits&0x7fffffffu;
            if(magnitude>=0x7f800000u) return -1;
            if((bits>>31) && magnitude) return 0;
        }
        return 1;
    }
#endif
    for (unsigned i = 0; i < 4; ++i) {
        const float *a = xyz[i], *b = xyz[(i+1)&3];
        /* Commands32/36 product/subtraction order is observable at boundaries. */
        float edge = z*a[0] - x*a[2];
        edge -= z*b[0]; edge += x*b[2];
        edge -= b[2]*a[0]; edge += b[0]*a[2];
        if (!isfinite(edge)) return -1;
        if (edge < 0) return 0;
    }
    return 1;
}

static float height(const nm_contact_quad_t *q, const nm_drive_params_t *p, float x, float z) {
    const float *v0 = q->xyz[0], *v1 = q->xyz[1], *v2 = q->xyz[2];
    const float a = v1[0]-v2[0], b = v0[0]-v2[0];
    const float c = v1[2]-v2[2], d = v0[2]-v2[2];
    const float u = x-v2[0], v = z-v2[2];
    const float beta = d != 0 ? nm_force_divide(p,d*u-v*b,d*a-c*b) : nm_force_divide(p,v,c);
    const float alpha = b != 0 ? nm_force_divide(p,u-a*beta,b) : nm_force_divide(p,v*a-u*c,d*a);
    const uint32_t signs = nm_force_bits(alpha)|nm_force_bits(beta);
    /* The bitwise combination can be NaN even for finite coefficients.
     * Reference branch uses sign/nonzero bits. Adding+0 only changes -0
     * (already excluded) or quiets NaN on the reference. On SH4 it instead
     * canonicalizes NaN and can lose the sign that selects the triangle. */
    if ((signs&0x80000000u) && (signs&0x7fffffffu)) {
        v1 = q->xyz[2]; v2 = q->xyz[3];
    }
    const float nx = (v2[2]-v0[2])*(v1[1]-v0[1]) - (v2[1]-v0[1])*(v1[2]-v0[2]);
    const float ny = (v2[0]-v0[0])*(v1[2]-v0[2]) - (v2[2]-v0[2])*(v1[0]-v0[0]);
    const float nz = (v2[1]-v0[1])*(v1[0]-v0[0]) - (v2[0]-v0[0])*(v1[1]-v0[1]);
    if (ny+0.0f == 0) return -0.1f;
    float numerator = nx*x + nz*z;
    numerator -= nx*v0[0]; numerator -= ny*v0[1]; numerator -= nz*v0[2];
    return nm_force_divide(p,numerator,ny)*-1.0f;
}

int nm_contact_query(const nm_contact_pack_t *p, const nm_drive_params_t *math,
                     float x, float z, nm_contact_result_t *state) {
    unsigned cell;
    if (!p || !p->storage || !math || !state || !isfinite(state->height) ||
        !nm_contact_cell(x,z,&cell)) return 0;
    const uint16_t *list = p->lists+p->grid[cell];
    float best = 0, distance = 0;
    unsigned selected = 0, hits = 0;
    for (unsigned i = 0; i < list[0]; ++i) {
        unsigned id = list[i+1];
        const int inside = nm_contact_contains(p->quads[id].xyz,x,z);
        if (inside < 0) return 0;
        if (!inside) continue;
        const float h = height(p->quads+id,math,x,z);
        const float delta = fabsf(state->height-fabsf(h));
        if (!isfinite(h) || !isfinite(delta)) return 0;
        /* Equal distances choose the LAST candidate, preserving source order. */
        if (!hits || delta-distance <= 0) { best = h; selected = id; distance = delta; }
        ++hits;
    }
    nm_contact_result_t result = *state;
    if (nm_force_bits(best)) {
        result.height = best; result.surface = selected;
        result.mask = 1u<<(p->quads[selected].surface&15);
        if (result.mask == 2) result.road = selected;
    } else { result.height = -0.1f; result.mask = 1; }
    *state = result;
    return 1;
}
