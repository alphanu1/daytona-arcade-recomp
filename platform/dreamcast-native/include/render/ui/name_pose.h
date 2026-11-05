#ifndef DAYTONA_NATIVE_NAME_POSE_H
#define DAYTONA_NATIVE_NAME_POSE_H
#include "pose.h"

/*227210..227264 /2272b8..227314: accepted initials use spin=0;
 * current-slot preview uses the selected glyph's spin. Not a ring pose. */
static inline int nm_name_initial_pose(const nm_drive_params_t *p,float x,
        uint16_t spin,float out[12]) {
    if(!p || !out || !isfinite(x)) return 0;
#ifdef __DREAMCAST__
    alignas(32) shz_mat3x4_t matrix;
    shz_xmtrx_init_rotation_y(nm_pose_radians((uint16_t)(-(int)spin)));
    shz_xmtrx_set_translation(x,-0.2f,1.0f);
    shz_xmtrx_scale(0.075f,0.075f,0.075f);
    shz_xmtrx_store_3x4(&matrix);
    shz_memcpy4(out,matrix.elem,sizeof(matrix));
#else
    const float origin[3]={x,-0.2f,1.0f};
    const int16_t yaw[3]={0,nm_yaw_angle(spin),0};
    if(!nm_body_pose_reference(p,origin,yaw,out)) return 0;
    for(unsigned i=0;i<9;++i) out[i]*=0.075f;
#endif
    for(unsigned i=0;i<12;++i) if(!isfinite(out[i])) return 0;
    return 1;
}

/* Ring and marker share the radial chain, not depth or animation state. */
static inline int nm_name_radial_pose(const nm_drive_params_t *p,float radius,
        int16_t angle,uint16_t spin,float depth,float out[12]) {
    if(!p || !out || !isfinite(radius) || !isfinite(depth)) return 0;
#ifdef __DREAMCAST__
    alignas(32) shz_mat3x4_t matrix;
    shz_xmtrx_init_rotation_z(nm_pose_radians((uint16_t)(-(int)angle)));
    shz_xmtrx_set_translation(0,-0.6f,depth);
    shz_xmtrx_translate(0,radius,0);
    shz_xmtrx_rotate_z(nm_pose_radians((uint16_t)angle));
    shz_xmtrx_rotate_y(nm_pose_radians((uint16_t)(-(int)spin)));
    shz_xmtrx_scale(0.02f,0.02f,0.02f);
    shz_xmtrx_store_3x4(&matrix);
    shz_memcpy4(out,matrix.elem,sizeof(matrix));
#else
    const float origin[3]={0,-0.6f,depth},offset[3]={0,radius,0},zero[3]={0};
    const int16_t base[3]={0,0,angle},none[3]={0},undo[3]={0,0,nm_yaw_angle((uint32_t)(-(int)angle))};
    const int16_t yaw[3]={0,nm_yaw_angle(spin),0};
    float part[12];
    if(!nm_body_pose_reference(p,origin,base,out) ||
       !nm_body_pose_reference(p,offset,none,part) || !nm_pose_compose_reference(out,part,out) ||
       !nm_body_pose_reference(p,zero,undo,part) || !nm_pose_compose_reference(out,part,out) ||
       !nm_body_pose_reference(p,zero,yaw,part) || !nm_pose_compose_reference(out,part,out)) return 0;
    for(unsigned i=0;i<9;++i) out[i]*=0.02f;
#endif
    for(unsigned i=0;i<12;++i) if(!isfinite(out[i])) return 0;
    return 1;
}

/*226c30..226d30 /227558..227610. Radius includes countdown*.03 for intro. */
static inline int nm_name_ring_pose(const nm_drive_params_t *p,float radius,
        int16_t angle,uint16_t spin,float out[12]) {
    return nm_name_radial_pose(p,radius,angle,spin,1,out);
}

typedef struct { float radius; int16_t angle; } nm_name_marker_t;

/*2273cc..227488: signed arithmetic half-difference, not shortest-arc wrap.
 * Radius follows the original TGP accumulator operation order. */
static inline int nm_name_marker_step(nm_name_marker_t *s,float radius,int16_t angle) {
    if(!s || !isfinite(s->radius) || !isfinite(radius)) return 0;
    const float target=0.008f+radius;
#ifdef __DREAMCAST__
    const float next=shz_lerpf(s->radius,target,0.5f);
#else
    const float next=s->radius+0.5f*(target-s->radius);
#endif
    if(!isfinite(next)) return 0;
    const int delta=(int)angle-s->angle;
    s->angle=(int16_t)(s->angle+(delta>=0 ? delta/2 : -((-delta+1)/2)));
    s->radius=next;return 1;
}
static inline int nm_name_marker_pose(const nm_drive_params_t *p,
        const nm_name_marker_t *s,uint32_t frame,float out[12]) {
    return s && nm_name_radial_pose(p,s->radius,s->angle,(uint16_t)((frame&31u)<<11),1.035f,out);
}
#endif
