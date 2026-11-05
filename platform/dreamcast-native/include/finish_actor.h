#ifndef DAYTONA_NATIVE_FINISH_ACTOR_H
#define DAYTONA_NATIVE_FINISH_ACTOR_H
#include "force_math.h"
#include <stdalign.h>
#include "nm_mem.h"
#ifdef __DREAMCAST__
#include <sh4zam/shz_sh4zam.h>
typedef shz_vec3_t nm_finish_vec3_t;
#else
typedef struct { float x,y,z; } nm_finish_vec3_t;
#endif

/* 44dc..4b3c authored finish actor, not a sampled animation. Offset28 is
 * inherited and advanced, but rendering reads2c, initialized to constant0.
 * Raw subnormal50 stays integer bits: SH4 DN must not erase the TGP state. */
typedef struct {
    nm_finish_vec3_t approach, other_x;
    uint32_t tiny_y_bits;
    uint16_t timer, inherited_angle;
} nm_finish_actor_t;

static inline int nm_finish_actor_init(nm_finish_actor_t *s, uint16_t inherited) {
    if(!s) return 0;
#ifdef __DREAMCAST__
    s->approach=shz_vec3_init(nm_force_float(0x40966666),5.0f,5.5f);
    s->other_x=shz_vec3_init(nm_force_float(0xc0a33333),nm_force_float(0x40a33333),nm_force_float(0xc0966666));
#else
    s->approach=(nm_finish_vec3_t){nm_force_float(0x40966666),5.0f,5.5f};
    s->other_x=(nm_finish_vec3_t){nm_force_float(0xc0a33333),nm_force_float(0x40a33333),nm_force_float(0xc0966666)};
#endif
    s->tiny_y_bits=0;s->timer=256;s->inherited_angle=inherited;return 1;
}

/* Returns1 moving,2 settled,0 invalid (unchanged). Caller passes this tick's
 * branch to pose: testing the decremented timer would be one tick early.
 * No arbitrary external transforms: state belongs to the initialized actor. */
static inline int nm_finish_actor_step(nm_finish_actor_t *s) {
    if(!s || !isfinite(s->approach.x) || !isfinite(s->approach.y) || !isfinite(s->approach.z) ||
       !isfinite(s->other_x.x) || !isfinite(s->other_x.y) || !isfinite(s->other_x.z) ||
       s->tiny_y_bits>0x7fffffu-655u) return 0;
    const int moving=s->timer>=57 && s->timer<32768;
    if(moving) {
        const float step=nm_force_float(0x3ccccccd);
#ifdef __DREAMCAST__
        s->approach=shz_vec3_sub(s->approach,shz_vec3_init(step,step,step));
        s->other_x=shz_vec3_add(s->other_x,shz_vec3_init(step,-step,-step));
#else
        s->approach.x-=step;s->approach.y-=step;s->approach.z-=step;
        s->other_x.x+=step;s->other_x.y-=step;s->other_x.z-=step;
#endif
        s->tiny_y_bits+=655;s->inherited_angle=(uint16_t)(s->inherited_angle+655u);
    }
    --s->timer;return moving ? 1 : 2;
}

/* Four lists in order288b460,288b488,288b44c,288b474. Rotation2c is zero;
 * only the first moving part omits scale2. XMTRX is clobbered on target.
 * Copies preserve subnormal translation bits without FP arithmetic. */
static inline int nm_finish_actor_pose(const nm_finish_actor_t *s, unsigned part,
        int branch, float out[12]) {
    if(!s || !out || part>=4 || (branch!=1 && branch!=2) || s->tiny_y_bits>0x7fffff) return 0;
    static const uint32_t settled_x[]={0xbe99999a,0xbdcccccd,0x3dcccccd,0x3e99999a};
    const float x=branch==2 ? nm_force_float(settled_x[part]) :
        part==0 ? s->approach.x : part==1 ? s->other_x.x : part==2 ? s->other_x.y : s->other_x.z;
    const float y=branch==1 && (part==0 || part==3) ? s->approach.y : nm_force_float(s->tiny_y_bits);
    const float z=branch==1 ? s->approach.z : 0.5f;
    if(!isfinite(x) || !isfinite(y) || !isfinite(z)) return 0;
    const float scale=branch==1 && part==0 ? 1.0f : 2.0f;
#ifdef __DREAMCAST__
    alignas(32) shz_mat3x4_t matrix;
    shz_xmtrx_init_scale(scale,scale,scale);
    shz_xmtrx_set_translation(x,y,z);
    shz_xmtrx_store_3x4(&matrix);
    shz_memcpy4(out,matrix.elem,sizeof(matrix));
#else
    const float matrix[12]={scale,0,0,0,scale,0,0,0,scale,x,y,z};
    nm_memcpy(out,matrix,sizeof(matrix));
#endif
    return 1;
}
#endif
