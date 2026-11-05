/* ROM-free branch, signed-zero and rejection checks. Original i960/TGP
 * parity with owned tables: m2segment --check-native-visibility drive.dnd. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../include/visibility.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc==2) {
        nm_scenery_pack_t pack={0};
        if (!nm_scenery_load(&pack,argv[1])) { puts("REJECT: invalid scenery pack"); return 1; }
        const nm_scenery_pack_t saved=pack;
        assert(!nm_scenery_load(&pack,argv[1]) && !memcmp(&pack,&saved,sizeof(pack)));
        printf("PASS scenery pack: slot=%u profiles=%u ranges=%u instances=%u\n",
            (unsigned)pack.slot,(unsigned)pack.profiles,(unsigned)pack.ranges,(unsigned)pack.instances);
        nm_scenery_unload(&pack); nm_scenery_unload(&pack);
        return 0;
    }
    assert(argc==1);
    nm_drive_params_t *p=calloc(1,sizeof(*p)); assert(p);
    const float diagonal=0.70710677f, one=1;
    memcpy(p->sine+8192,&diagonal,4); memcpy(p->sine+16383,&one,4);
    assert(!nm_scenery_wedge(p,0,0,-0.f,1,-8192)); // NOT equivalent to >=0.
    assert(nm_scenery_wedge(p,0,0,0,1,-8192));
    nm_scenery_probe_t probes[4][12]={0};
    for (unsigned q=0;q<4;++q) for (unsigned i=0;i<12;++i)
        probes[q][i]=(nm_scenery_probe_t){0,128,1u<<i};
    uint32_t mask=0;
    assert(nm_scenery_view_mask(p,probes,0,0,0,&mask) && mask==0x1041);
    for (unsigned i=0;i<12;++i) probes[0][i].z=-128;
    probes[0][4].z=probes[0][10].z=128;
    assert(nm_scenery_view_mask(p,probes,0,0,0,&mask) && mask==0x1010);
    probes[0][4].z=-128;
    assert(nm_scenery_view_mask(p,probes,0,0,0,&mask) && mask==0x1400);
    probes[0][10].z=-128;
    assert(nm_scenery_view_mask(p,probes,0,0,0,&mask) && mask==0x1000);
    uint32_t before=mask;
    /* Outside int32 the masked cvtri saturates (nm_drive_round_i32, 80960KB PRM 11-50). */
    assert(nm_scenery_view_mask(p,probes,2147483648.f,0,0,&mask) && nm_scenery_view_mask(p,probes,0,-2147483904.f,0,&mask));
    mask=before;
    const float invalid[]={NAN,INFINITY,-INFINITY};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        assert(!nm_scenery_view_mask(p,probes,invalid[i],0,0,&mask) && mask==before);
        assert(!nm_scenery_view_mask(p,probes,0,invalid[i],0,&mask) && mask==before);
    }
    assert(!nm_scenery_view_mask(NULL,probes,0,0,0,&mask));
    assert(!nm_scenery_view_mask(p,NULL,0,0,0,&mask));
    assert(!nm_scenery_view_mask(p,probes,0,0,0,NULL));
    uint8_t order[25];
    for (unsigned i=0;i<25;++i) order[i]=(uint8_t)i;
    nm_scenery_cells_t cells;
    for (unsigned c=0;c<256;++c) {
        assert(nm_scenery_cells(order,c,0,0x1ffffff,&cells));
        const unsigned x=c&15,z=c>>4;
        const unsigned width=1+(x<2?x:2)+(15-x<2?15-x:2);
        const unsigned height=1+(z<2?z:2)+(15-z<2?15-z:2);
        assert(cells.draw_count==width*height && cells.near_count<=10);
        uint32_t masks[16];assert(nm_scenery_bitmaps(&cells,masks));
        for(unsigned bit=0;bit<256;++bit) {
            unsigned draw=0,near=0;
            for(unsigned i=0;i<cells.draw_count;++i) draw|=cells.draw[i]==bit;
            for(unsigned i=0;i<cells.near_count;++i) near|=cells.near[i]==bit;
            assert(((masks[bit>>5]>>(bit&31))&1)==draw);
            assert(((masks[8+(bit>>5)]>>(bit&31))&1)==near);
        }
        for (unsigned i=0;i<cells.draw_count;++i) {
            assert(abs((int)(cells.draw[i]&15)-(int)x)<=2);
            assert(abs((int)(cells.draw[i]>>4)-(int)z)<=2);
            if (i) assert(cells.draw[i]>cells.draw[i-1]);
        }
    }
    assert(nm_scenery_cells(order,119,0x1ffffff,0x1ffffff,&cells) && !cells.draw_count && !cells.near_count);
    const nm_scenery_cells_t saved=cells;
    uint32_t masks[16],untouched[16];memset(masks,0xa5,sizeof(masks));memcpy(untouched,masks,sizeof(masks));
    assert(!nm_scenery_bitmaps(NULL,masks) && !memcmp(masks,untouched,sizeof(masks)));
    assert(!nm_scenery_bitmaps(&cells,NULL));
    cells.draw_count=26;assert(!nm_scenery_bitmaps(&cells,masks) && !memcmp(masks,untouched,sizeof(masks)));
    cells=saved;cells.near_count=11;assert(!nm_scenery_bitmaps(&cells,masks) && !memcmp(masks,untouched,sizeof(masks)));
    cells=saved;cells.near_count=1;cells.near[0]=255;
    assert(!nm_scenery_bitmaps(&cells,masks) && !memcmp(masks,untouched,sizeof(masks)));
    cells=saved;assert(nm_scenery_bitmaps(&cells,masks));
    for(unsigned i=0;i<16;++i) assert(!masks[i]);
    order[0]=25; assert(!nm_scenery_cells(order,119,0,0,&cells) && !memcmp(&saved,&cells,sizeof(cells)));
    order[0]=1; assert(!nm_scenery_cells(order,119,0,0,&cells) && !memcmp(&saved,&cells,sizeof(cells)));
    order[0]=0; assert(!nm_scenery_cells(order,256,0,0,&cells));
    assert(!nm_scenery_cells(order,0,1u<<25,0,&cells));
    assert(!nm_scenery_cells(order,0,0,1u<<25,&cells));
    uint16_t rows[16];
    assert(!nm_scenery_profile_mask(NULL,0));
    for (unsigned c=0;c<256;++c) for (unsigned bit=0;bit<256;++bit) {
        memset(rows,0,sizeof(rows)); rows[bit>>4]=(uint16_t)(1u<<(bit&15));
        uint32_t expected=0;
        for (unsigned i=0;i<25;++i) {
            int x=(int)(c&15)+(int)(i%5)-2;
            unsigned z=((c>>4)+i/5-2)&15;
            if (x==(int)(bit&15) && z==bit>>4) expected|=1u<<i;
        }
        assert(nm_scenery_profile_mask(rows,c)==expected);
    }
    free(p);
    puts("PASS native scenery: view branches/sign bits, ordered bounded lists, 65536 profile-bit placements, atomic rejection");
    return 0;
}
