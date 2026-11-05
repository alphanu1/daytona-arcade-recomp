#ifndef DAYTONA_NATIVE_REFLECTION_H
#define DAYTONA_NATIVE_REFLECTION_H
#include <stdint.h>
#include "meshpack.h"

/* Window texture control6f24..7020. The phase is entity+6, NOT wheel phase.
 * Advance once per source tick, independently of presentation cadence.
 * Upload intents replace guest texture writes; immutable mesh/base UVs belong
 * in the offline pack. Course2 header changes and neighbor restore are distinct. */
typedef struct {
    uint32_t frame, speed_index;
    uint16_t flags, previous_flags, neighbor_flags, neighbor_previous_flags;
    int16_t heading;
    uint8_t course, next_mode, active_mode; //5010a4/5010a0, not VR camera selection.
} nm_reflection_input_t;
enum { NM_REFLECTION_NONE, NM_REFLECTION_UV, NM_REFLECTION_TUNNEL, NM_REFLECTION_RESTORE };
/* Source texture pairs are (V,U): heading offsets V, phase offsets U. */
typedef struct { uint16_t v_offset, u_offset; uint8_t action, others_only; } nm_reflection_update_t;

static inline int nm_reflection_step(const nm_reflection_input_t *in,
        uint16_t *phase, nm_reflection_update_t *out) {
    if (!in || !phase || !out) return 0;
    nm_reflection_update_t next={0,0,0,0};
    if (in->flags & (uint16_t)~in->previous_flags & 256) {
        if(in->course==2) next.action=NM_REFLECTION_TUNNEL;
    } else if (in->previous_flags & (uint16_t)~in->flags & 256) {
        next.action=NM_REFLECTION_RESTORE;
    } else if (in->neighbor_previous_flags & (uint16_t)~in->neighbor_flags & 256) {
        next.action=NM_REFLECTION_RESTORE;next.others_only=1;
    } else {
        uint32_t speed=in->speed_index;
        if(in->flags&256) speed<<=2; // Preserve32-bit wrap BEFORE logical shift.
        *phase=(uint16_t)((uint32_t)*phase+((in->frame&7)==0)-(speed>>6));
        if(in->next_mode==in->active_mode) {
            next.action=NM_REFLECTION_UV;
            next.v_offset=(uint16_t)(((uint32_t)(int32_t)in->heading>>4)&32767);
            next.u_offset=*phase&32767;
        }
    }
    *out=next;return 1;
}

/*19ee0 uploads base halfwords plus offsets; the texture RAM keeps low16 bits. */
static inline uint16_t nm_reflection_uv(uint16_t base, uint16_t offset) {
    return (uint16_t)((uint32_t)base+offset);
}
/* Owned CPU mesh only; the renderer copies UVs into its SQ command buffer.
 * Base coordinates are separate, so repeated presentation never accumulates. */
static inline int nm_reflection_apply(nm_vertex_t *vertices,const uint16_t *base,
        unsigned count,uint16_t u,uint16_t v) {
    if(!vertices || !base || !count || count>36) return 0;
    for(unsigned i=0;i<count;++i) {
        vertices[i].u=nm_reflection_uv(base[2*i],u);
        vertices[i].v=nm_reflection_uv(base[2*i+1],v);
    }
    return 1;
}
#endif
