#include "path.h"
#include <stdio.h>
#include <stdlib.h>
#include "nm_mem.h"

_Static_assert(sizeof(nm_path_row_t) == 28, "DNP1 row layout");
void nm_path_unload(nm_path_pack_t *p) {
    if (p) { free(p->rows); nm_memset(p,0,sizeof(*p)); }
}
int nm_path_load(nm_path_pack_t *p, const char *path) {
    const uint32_t endian = 1;
    if (!p || p->rows || !path || *(const uint8_t *)&endian != 1) return 0;
    FILE *f = fopen(path,"rb");
    if (!f) return 0;
    nm_path_pack_t next = {0};
    uint32_t h[8];
    int ok = fread(h,1,sizeof(h),f) == sizeof(h) && h[0] == 0x31504e44 &&
        h[2] < 3 && h[3] < 8 && h[4] && h[4] <= 1162 && h[5] == h[4]+8 &&
        h[1] == sizeof(h)+28*h[5] && !h[6] && !h[7];
    if (ok) {
        next.slot = h[2]; next.route = h[3]; next.count = h[4]; next.records = h[5];
        const size_t bytes = 28*next.records;
        next.rows = nm_alloc(bytes);
        ok = next.rows && fread(next.rows,1,bytes,f) == bytes && fgetc(f) == EOF && !ferror(f);
    }
    if (fclose(f)) ok = 0;
    if (ok) for (unsigned i = 0; i < next.records; ++i) {
        if (!isfinite(next.rows[i].speed)) ok = 0;
        for (unsigned a = 0; a < 3; ++a) if (!isfinite(next.rows[i].xyz[a])) ok = 0;
    }
    if (!ok) { nm_path_unload(&next); return 0; }
    *p = next;
    return 1;
}
