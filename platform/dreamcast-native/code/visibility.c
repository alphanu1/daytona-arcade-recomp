#include "visibility.h"
#include <stdio.h>
#include <stdlib.h>
#include "nm_mem.h"

_Static_assert(sizeof(nm_scenery_probe_t)==12 && sizeof(nm_scenery_cell_t)==4 &&
               sizeof(nm_scenery_range_t)==36,"DNV1 record layout");
void nm_scenery_unload(nm_scenery_pack_t *p) {
    if (p) { free(p->storage); nm_memset(p,0,sizeof(*p)); }
}
int nm_scenery_load(nm_scenery_pack_t *p, const char *path) {
    const uint32_t endian=1;
    if (!p || p->storage || !path || *(const uint8_t *)&endian!=1) return 0;
    FILE *file=fopen(path,"rb");
    if (!file) return 0;
    uint32_t h[8]; nm_scenery_pack_t next={0};
    int ok=fread(h,1,sizeof(h),file)==sizeof(h) && h[0]==0x31564e44 &&
        h[2]<4 && h[3]>0 && h[3]<=4097 && h[4]<=64 && h[5]<=65535 &&
        !h[6] && !h[7] && h[1]==1684+36*h[4];
    if (ok) {
        next.storage=nm_alloc(h[1]-32);
        ok=next.storage && fread(next.storage,1,h[1]-32,file)==h[1]-32 && fgetc(file)==EOF && !ferror(file);
    }
    if (fclose(file)) ok=0;
    if (ok) {
        const uint8_t *data=next.storage;
        next.slot=h[2]; next.profiles=h[3]; next.ranges=h[4]; next.instances=h[5];
        next.orders=(const uint8_t (*)[25])data;
        next.probes=(const nm_scenery_probe_t (*)[12])(data+52);
        next.cells=(const nm_scenery_cell_t *)(data+628);
        next.exclusions=(const nm_scenery_range_t *)(data+1652);
        nm_scenery_cells_t ignored;
        ok=!data[50] && !data[51] && nm_scenery_cells(next.orders[0],0,0,0,&ignored) &&
            nm_scenery_cells(next.orders[1],0,0,0,&ignored);
        for (unsigned q=0;q<4;++q) for (unsigned i=0;i<12;++i) {
            const nm_scenery_probe_t *v=&next.probes[q][i];
            if (!isfinite(v->x) || !isfinite(v->z) || fabsf(v->x)>384 || fabsf(v->z)>384 || v->mask>>25) ok=0;
        }
        unsigned end=0;
        for (unsigned i=0;i<256;++i) {
            const nm_scenery_cell_t *c=next.cells+i;
            if (c->first!=end || end+c->count>next.instances) ok=0;
            end+=c->count;
        }
        if (end!=next.instances) ok=0;
        for (unsigned i=0;i<next.ranges;++i) {
            const nm_scenery_range_t *r=next.exclusions+i;
            if (r->low>r->high || r->high>=next.profiles || (i && r->low<=next.exclusions[i-1].high)) ok=0;
        }
    }
    if (!ok) { nm_scenery_unload(&next); return 0; }
    *p=next; return 1;
}
