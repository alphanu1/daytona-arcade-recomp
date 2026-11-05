#ifndef DAYTONA_NATIVE_VISIBILITY_H
#define DAYTONA_NATIVE_VISIBILITY_H
#include "drive.h"
#include "nm_mem.h"

/* Offline-decoded 1727c probe tables: four camera quadrants, two groups of
 * six points/masks per quadrant. No guest addresses or TGP FIFO on target. */
typedef struct { float x, z; uint32_t mask; } nm_scenery_probe_t;
typedef struct { uint8_t draw[25], near[10], draw_count, near_count; } nm_scenery_cells_t;
/* Original501520..50155f: eight draw words followed by eight near words,
 * not one512-cell visibility map. Existing selection has already clipped the
 * ordered lists. Integer set membership only; no presentation approximation. */
static inline int nm_scenery_bitmaps(const nm_scenery_cells_t *cells,uint32_t out[16]) {
    if(!cells || !out || cells->draw_count>25 || cells->near_count>10) return 0;
    uint32_t masks[16]={0};
    for(unsigned i=0;i<cells->draw_count;++i) {
        const unsigned cell=cells->draw[i];masks[cell>>5]|=1u<<(cell&31);
    }
    for(unsigned i=0;i<cells->near_count;++i) {
        const unsigned cell=cells->near[i];
        if(!(masks[cell>>5]&(1u<<(cell&31)))) return 0;
        masks[8+(cell>>5)]|=1u<<(cell&31);
    }
    nm_memcpy(out,masks,sizeof(masks));return 1;
}
typedef struct { uint16_t low, high, rows[16]; } nm_scenery_range_t;
typedef struct { uint16_t first, count; } nm_scenery_cell_t;
typedef struct {
    void *storage;
    uint32_t slot, profiles, ranges, instances;
    const uint8_t (*orders)[25];
    const nm_scenery_probe_t (*probes)[12];
    const nm_scenery_cell_t *cells;
    const nm_scenery_range_t *exclusions;
} nm_scenery_pack_t;
int nm_scenery_load(nm_scenery_pack_t *pack, const char *path);
void nm_scenery_unload(nm_scenery_pack_t *pack);

/* 17230..17274. Offline profile ranges choose one16-row exclusion bitmap,
 * or NULL outside an authored range. Boundary clipping is separate below. */
static inline uint32_t nm_scenery_profile_mask(const uint16_t rows[16], unsigned cell) {
    if (!rows) return 0;
    uint32_t mask=0;
    for (unsigned i=0;i<5;++i) {
        const unsigned row=((cell>>4)+i-2)&15;
        mask|=((((uint32_t)rows[row]<<2)>>(cell&15))&31)<<(5*i);
    }
    return mask;
}

/* 16ed0..16f5c boundary exclusion and16f74..17070 ordered lists. The caller
 * chooses the offline source order (alternate only course2/cells72,88) and
 * supplies the profile exclusion mask from171f8. Near means the first TEN
 * order slots, not ten accepted cells. Center coordinates wrap upstream;
 * neighbors never wrap across a grid edge. Atomic failure, no allocation. */
static inline int nm_scenery_cells(const uint8_t order[25], unsigned cell,
    uint32_t excluded, uint32_t visible, nm_scenery_cells_t *out) {
    if (!order || !out || cell>255 || (excluded|visible)>>25) return 0;
    uint32_t seen=0;
    for (unsigned i=0;i<25;++i) {
        if (order[i]>=25 || (seen&(1u<<order[i]))) return 0;
        seen|=1u<<order[i];
    }
    nm_scenery_cells_t result; nm_memset(&result,0,sizeof(result));
    for (unsigned i=0;i<25;++i) {
        const unsigned bit=order[i];
        const int x=(int)(cell&15)+(int)(bit%5)-2;
        const int z=(int)(cell>>4)+(int)(bit/5)-2;
        if (x<0 || x>15 || z<0 || z>15 || (excluded&(1u<<bit)) || !(visible&(1u<<bit))) continue;
        const uint8_t neighbor=(uint8_t)(x+16*z);
        result.draw[result.draw_count++]=neighbor;
        if (i<10) result.near[result.near_count++]=neighbor;
    }
    *out=result;
    return 1;
}

/* 17668..17754. Preserve separate float32 operations and SIGN BITS, including
 * negative zero: source ORs the two result words before a signed comparison.
 * Existing table sine is exact; FTRV/FMA must not change boundary decisions. */
static inline int nm_scenery_wedge(const nm_drive_params_t *p, float x, float z,
                                  float point_x, float point_z, int16_t heading) {
    const float dx=point_x-x, dz=point_z-z;
    const uint16_t angle=(uint16_t)(-(int)heading-8192);
    const float sn=nm_drive_sine(p,angle), cs=nm_drive_sine(p,(uint16_t)(angle+16384));
    const float a=cs*dx, b=sn*dz, c=sn*dx, d=cs*dz;
    const float left=a-b, right=d+c;
    uint32_t lb,rb; nm_memcpy(&lb,&left,4); nm_memcpy(&rb,&right,4);
    return !((lb|rb)&0x80000000u);
}

/* 1727c..17654. Caller supplies validated finite probe offsets/25-bit masks
 * and existing validated drive tables. Coordinates must fit source CVTRI.
 * Result is a local 5x5 bitmask, NOT the final ordered/boundary-clipped list. */
static inline int nm_scenery_view_mask(const nm_drive_params_t *p,
    const nm_scenery_probe_t probes[4][12], float x, float z, int16_t heading,
    uint32_t *out) {
    int32_t ix,iz;
    if (!p || !probes || !out || !isfinite(x) || !isfinite(z) ||
        !nm_drive_round_i32(x,&ix) || !nm_drive_round_i32(z,&iz)) return 0;
    uint32_t bx=(uint32_t)ix&0xffffff80u, bz=(uint32_t)iz&0xffffff80u;
    nm_memcpy(&ix,&bx,4); nm_memcpy(&iz,&bz,4);
    const float origin_x=(float)ix, origin_z=(float)iz;
    const unsigned quadrant=((uint32_t)(int32_t)heading+8192u)>>14&3;
    uint32_t mask=0;
    for (unsigned group=0;group<2;++group) {
        const nm_scenery_probe_t *v=probes[quadrant]+group*6;
        for (unsigned i=0;i<6;++i) {
            /* These are source branches, not a union of all passing probes.
             * The second group tests the mask accumulated by BOTH groups. */
            if (i==4 && mask) break;
            const float px=v[i].x+origin_x, pz=v[i].z+origin_z;
            if (!nm_scenery_wedge(p,x,z,px,pz,heading)) continue;
            mask|=v[i].mask;
            if (i<2 || i==4) break;
        }
    }
    *out=mask|0x1000u;
    return 1;
}

static inline int nm_scenery_select(const nm_scenery_pack_t *pack,
    const nm_drive_params_t *math, unsigned cell, unsigned profile,
    float x, float z, int16_t heading, nm_scenery_cells_t *out) {
    if (!pack || !pack->storage || cell>255 || profile>=pack->profiles || !out) return 0;
    const uint16_t *rows=NULL;
    for (unsigned i=0;i<pack->ranges;++i) {
        const nm_scenery_range_t *range=pack->exclusions+i;
        if (profile>range->high) continue;
        if (profile>=range->low) rows=range->rows;
        break;
    }
    uint32_t visible;
    if (!nm_scenery_view_mask(math,pack->probes,x,z,heading,&visible)) return 0;
    const unsigned order=pack->slot==2 && (cell==72 || cell==88);
    return nm_scenery_cells(pack->orders[order],cell,nm_scenery_profile_mask(rows,cell),visible,out);
}
#endif
