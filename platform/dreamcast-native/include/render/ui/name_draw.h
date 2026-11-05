#ifndef DAYTONA_NATIVE_NAME_DRAW_H
#define DAYTONA_NATIVE_NAME_DRAW_H
#include <stdalign.h>
#include "name_entry.h"
#include "render/ui/name_pose.h"

typedef struct {
    float velocity[3],scale_rate;
    int16_t spin[3];
    uint16_t reserved[5];
} nm_name_exit_t;
_Static_assert(sizeof(nm_name_exit_t)==32,"one exit record per cache line");

typedef struct {
    float radius[29],initial_x[3];
    int16_t angle[29];
    alignas(32) nm_name_exit_t exit[29];
} nm_name_layout_t;

/* Original226e1c..227034 /2270c8..2271cc, excluding model submission.
 * Phase7 deliberately uses NEGATIVE scale until its twentieth tick. */
static inline int nm_name_exit_pose(const nm_drive_params_t *math,
        const nm_name_layout_t *layout,unsigned glyph,unsigned phase,
        uint16_t tick,float out[12]) {
    if(!math || !layout || !out || glyph>=29 || (phase!=5 && phase!=7) ||
       (phase==7 && tick>20)) return 0;
    const nm_name_exit_t *e=layout->exit+glyph;
    const float t=(float)tick;
    const float scale=phase==5 ? t*e->scale_rate+0.02f : t*0.001f-0.02f;
    const float height=phase==5 ? t*0.02f-0.6f : -0.6f;
    const int16_t angle=layout->angle[glyph];
    int16_t spin[3]={0};
    if(phase==5) for(unsigned i=0;i<3;++i) spin[i]=nm_yaw_angle((uint32_t)((int)e->spin[i]*(int)tick));
#ifdef __DREAMCAST__
    shz_vec3_t offset=phase==5 ? shz_vec3_scale(shz_vec3_init(e->velocity[0],e->velocity[1],e->velocity[2]),t) :
        shz_vec3_init(0,0,0);
    offset.y=layout->radius[glyph]-offset.y;
    alignas(32) shz_mat3x4_t pose;
    shz_xmtrx_init_rotation_z(nm_pose_radians((uint16_t)(-(int)angle)));
    shz_xmtrx_set_translation(0,height,1);
    shz_xmtrx_translate(offset.x,offset.y,offset.z);
    shz_xmtrx_rotate_z(nm_pose_radians((uint16_t)angle));
    if(phase==5) {
        shz_xmtrx_rotate_x(nm_pose_radians((uint16_t)(-(int)spin[0])));
        shz_xmtrx_rotate_y(nm_pose_radians((uint16_t)(-(int)spin[1])));
        shz_xmtrx_rotate_z(nm_pose_radians((uint16_t)(-(int)spin[2])));
    }
    shz_xmtrx_scale(scale,scale,scale);
    shz_xmtrx_store_3x4(&pose);shz_memcpy4(out,pose.elem,sizeof(pose));
#else
    const float origin[3]={0,height,1},zero[3]={0};
    const float offset[3]={phase==5 ? t*e->velocity[0] : 0,
        layout->radius[glyph]-(phase==5 ? t*e->velocity[1] : 0),phase==5 ? t*e->velocity[2] : 0};
    const int16_t base[3]={0,0,angle},none[3]={0},undo[3]={0,0,nm_yaw_angle((uint32_t)(-(int)angle))};
    float part[12];
    if(!nm_body_pose_reference(math,origin,base,out) ||
       !nm_body_pose_reference(math,offset,none,part) || !nm_pose_compose_reference(out,part,out) ||
       !nm_body_pose_reference(math,zero,undo,part) || !nm_pose_compose_reference(out,part,out)) return 0;
    if(phase==5) for(unsigned axis=0;axis<3;++axis) {
        int16_t rotation[3]={0};rotation[axis]=spin[axis];
        if(!nm_body_pose_reference(math,zero,rotation,part) || !nm_pose_compose_reference(out,part,out)) return 0;
    }
    for(unsigned i=0;i<9;++i) out[i]*=scale;
#endif
    for(unsigned i=0;i<12;++i) if(!isfinite(out[i])) return 0;
    return 1;
}

/* Active actor226b9c: ring, accepted initials, marker, current-slot preview.
 * At most33 model draws; a renderer calls this inside its queued callback.
 * Marker smoothing belongs to the tick owner, never to repeated render passes.
 */
static inline unsigned nm_name_draw_count(const nm_name_entry_t *entry) {
    if(!entry || entry->count>3 || entry->index>=29) return 0;
    return 30+entry->count+(entry->count<3);
}

static inline int nm_name_draw_pose(const nm_drive_params_t *math,
        const nm_name_layout_t *layout,const nm_name_entry_t *entry,
        const nm_name_marker_t *marker,uint32_t frame,unsigned ordinal,
        unsigned *binding,float out[12]) {
    if(!math || !layout || !marker || !binding || !out ||
       ordinal>=nm_name_draw_count(entry)) return 0;
    const unsigned cycle=frame&31u;
    const uint16_t spin=cycle<16 ? (uint16_t)(cycle<<12) : 0;
    if(ordinal<29) {
        *binding=ordinal;
        return nm_name_ring_pose(math,layout->radius[ordinal],layout->angle[ordinal],
                                 ordinal==entry->index ? spin : 0,out);
    }
    ordinal-=29;
    if(ordinal<entry->count) {
        const uint8_t glyph=entry->name[ordinal];
        if(glyph=='.') *binding=26;
        else if(glyph>='A' && glyph<='Z') *binding=glyph-'A';
        else return 0;
        return nm_name_initial_pose(math,layout->initial_x[ordinal],0,out);
    }
    if(ordinal==entry->count) {
        *binding=29;
        return nm_name_marker_pose(math,marker,frame,out);
    }
    *binding=entry->index;
    return nm_name_initial_pose(math,layout->initial_x[entry->count],spin,out);
}
#endif
