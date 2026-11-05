#include "road.h"
#include <stdio.h>
#include <stdlib.h>
#include "nm_mem.h"

_Static_assert(sizeof(nm_road_profile_t) == 164 && sizeof(nm_road_map_t) == 4,"DNR2 record layout");
_Static_assert(sizeof(nm_road_awards_t) == 4224,"DNR3 award layout");
_Static_assert(sizeof(nm_road_grid_t) == 32,"DNR5 grid layout");
_Static_assert(sizeof(nm_road_start_t) == 32,"DNR6 start layout");
void nm_road_unload(nm_road_pack_t *p) {
    if (p) { free(p->storage); nm_memset(p,0,sizeof(*p)); }
}
int nm_road_load(nm_road_pack_t *p, const char *path) {
    const uint32_t endian = 1;
    if (!p || p->storage || !path || *(const uint8_t *)&endian != 1) return 0;
    FILE *f = fopen(path,"rb");
    if (!f) return 0;
    nm_road_pack_t next = {0}; uint32_t h[8];
    int ok = fread(h,1,sizeof(h),f) == sizeof(h) &&
        ((h[0] == 0x33524e44 && !h[6] && !h[7]) ||
         (h[0] == 0x34524e44 && h[6] && h[6] <= 255 && !h[7]) ||
         ((h[0] == 0x35524e44 || h[0] == 0x36524e44 || h[0] == 0x37524e44) && h[6] && h[6] <= 255 && h[7] && h[7] <= 44)) && h[2] < 4 &&
        h[3] >= 2 && h[3] <= 4097 && h[4] && h[4] <= 65536 &&
        h[5] <= 31;
    uint32_t grid_offset=0;
    if (ok) {
        const uint32_t base=164*h[3]+4*h[4]+4224+2*h[5];
        grid_offset=(base+31u)&~31u;
        uint32_t payload=h[7] ? grid_offset+32*h[7] : base;
        if(h[0]==0x36524e44 || h[0]==0x37524e44) {
            payload=(payload+4*h[7]+31u)&~31u;
            if(h[0]==0x37524e44) payload=(payload+4*h[7]+31u)&~31u;
            payload+=32;
        }
        ok=h[1]==sizeof(h)+payload;
    }
    if (ok) {
        next.slot = h[2]; next.profile_count = h[3]; next.map_count = h[4]; next.payload_bytes = h[1]-sizeof(h);
        next.checkpoint_count = h[5];
        next.total_laps = h[6];
        next.grid_count = h[7];
        next.storage = nm_alloc((next.payload_bytes+31u)&~31u);
        ok = next.storage && fread(next.storage,1,next.payload_bytes,f) == next.payload_bytes && fgetc(f) == EOF && !ferror(f);
    }
    if (fclose(f)) ok = 0;
    if (ok) {
        next.profiles = next.storage;
        next.mapping = (const nm_road_map_t *)((const uint8_t *)next.storage+164*next.profile_count);
        next.awards = (const nm_road_awards_t *)(next.mapping+next.map_count);
        next.checkpoints = (const uint16_t *)(next.awards+1);
        if (next.grid_count) {
            next.grid=(const nm_road_grid_t *)((const uint8_t *)next.storage+grid_offset);
            const uint32_t base=164*h[3]+4*h[4]+4224+2*h[5];
            for (uint32_t i=base;i<grid_offset;++i)
                if (((const uint8_t *)next.storage)[i]) ok=0;
            for (unsigned i=0;i<next.grid_count;++i) {
                const nm_road_grid_t *g=next.grid+i;
                float speed; nm_memcpy(&speed,&g->speed_parameter,4);
                if (!isfinite(speed) || g->profile>=next.profile_count-1 || g->previous>65535 ||
                    g->path_offset!=((g->profile*28u)&65535) || g->lateral!=(i&1 ? 384u : 1408u) ||
                    g->lap!=(g->profile>=(next.profile_count-1)/2 ? -1 : 0) ||
                    g->route>=8 || g->style>255) ok=0;
            }
        }
        if(h[0]==0x36524e44 || h[0]==0x37524e44) {
            const uint32_t speeds=grid_offset+32*next.grid_count;
            uint32_t start=(speeds+4*next.grid_count+31u)&~31u;
            next.grid_speeds=(const float *)((const uint8_t *)next.storage+speeds);
            for(unsigned i=0;i<next.grid_count;++i) if(!isfinite(next.grid_speeds[i])) ok=0;
            for(uint32_t i=speeds+4*next.grid_count;i<start;++i)
                if(((const uint8_t *)next.storage)[i]) ok=0;
            if(h[0]==0x37524e44) {
                const uint32_t guided=start;
                next.grid_guided_scales=(const float *)((const uint8_t *)next.storage+guided);
                start=(guided+4*next.grid_count+31u)&~31u;
                for(unsigned i=0;i<next.grid_count;++i) if(!isfinite(next.grid_guided_scales[i])) ok=0;
                for(uint32_t i=guided+4*next.grid_count;i<start;++i)
                    if(((const uint8_t *)next.storage)[i]) ok=0;
            }
            next.start=(const nm_road_start_t *)((const uint8_t *)next.storage+start);
            for(unsigned i=0;i<4;++i) if(!next.start->body_model[i]) ok=0;
            if(!isfinite(next.start->default_speed) || !isfinite(next.start->rolling_speed) ||
               next.start->reserved[0] || next.start->reserved[1]) ok=0;
        }
        for (unsigned i = 0; i < next.checkpoint_count; ++i)
            if (next.checkpoints[i] >= next.profile_count) ok = 0;
        for (unsigned i = 0; i < next.map_count; ++i)
            if (next.mapping[i].profile >= next.profile_count) ok = 0;
        for (unsigned i = 0; i < next.profile_count; ++i) {
            const nm_road_profile_t *r = next.profiles+i;
            if (r->first_lane < 0 || r->last_lane > 7 || r->first_lane > r->last_lane) ok = 0;
            for (unsigned lane = 0; lane < 8; ++lane) for (unsigned axis = 0; axis < 3; ++axis)
                if (!isfinite(r->lanes[lane].xyz[axis])) ok = 0;
        }
    }
    if (!ok) { nm_road_unload(&next); return 0; }
    *p = next;
    return 1;
}
