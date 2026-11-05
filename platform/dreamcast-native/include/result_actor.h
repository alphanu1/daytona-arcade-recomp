#ifndef DAYTONA_NATIVE_RESULT_ACTOR_H
#define DAYTONA_NATIVE_RESULT_ACTOR_H
#include "nm_mem.h"
#include <stdint.h>
#include <math.h>
#include "pose.h"
#ifdef __DREAMCAST__
#include <sh4zam/shz_sh4zam.h>
typedef shz_vec2_t nm_result_vec2_t;
#else
typedef struct { float x,y; } nm_result_vec2_t;
#endif

/* b70c..b848: eight authored moving parts. No guest pointers, draw calls or
 * sound. Caller selects a validated offline target row before this update. */
typedef struct {
    nm_result_vec2_t position[8];
    int16_t angle[8],other_angle[8];
} nm_result_actor_t;

/* b84c..b944: T(x,-.01,y) * S(1.2) * Ry(other) * Rz(angle).
 * Owns/clobbers XMTRX; no callbacks or other matrix owners inside the chain. */
static inline int nm_result_actor_pose(const nm_drive_params_t *p,
        const nm_result_actor_t *s,unsigned part,float out[12]) {
    if(!p || !s || part>=8 || !out || !isfinite(s->position[part].x) ||
       !isfinite(s->position[part].y)) return 0;
#ifdef __DREAMCAST__
    alignas(32) shz_mat3x4_t matrix;
    shz_xmtrx_init_scale(1.2f,1.2f,1.2f);
    shz_xmtrx_rotate_y(nm_pose_radians((uint16_t)(-(int)s->other_angle[part])));
    shz_xmtrx_rotate_z(nm_pose_radians((uint16_t)(-(int)s->angle[part])));
    shz_xmtrx_set_translation(s->position[part].x,-0.01f,s->position[part].y);
    shz_xmtrx_store_3x4(&matrix);
    shz_memcpy4(out,matrix.elem,sizeof(matrix));
#else
    const float xyz[3]={s->position[part].x,-0.01f,s->position[part].y};
    const int16_t angles[3]={0,s->other_angle[part],s->angle[part]};
    if(!nm_body_pose_reference(p,xyz,angles,out)) return 0;
    for(unsigned i=0;i<9;++i) out[i]*=1.2f;
#endif
    return 1;
}
typedef struct {
    nm_result_vec2_t position;
    float rate;
    int16_t angle,other_angle;
} nm_result_target_t;

typedef struct { int32_t tick; uint32_t target; } nm_result_key_t;
typedef struct { int32_t countdown; uint32_t command; } nm_result_announcement_t;

/* 18544: eight large-font glyph codes, left to right: " m'ss\"hh".
 * Preserve the original low32 multiply before shifting, even for signed lap
 * values passed as uint32. Exact integer div/mod: no SH4ZAM integer variant.
 * The renderer owns glyph atlas/placement; no guest tile RAM here. */
static inline void nm_result_time_glyphs(uint32_t ticks,uint16_t out[8]) {
    uint32_t n=(ticks*UINT32_C(100))>>6;
    out[7]=(uint16_t)(0x8030+n%10);n/=10;
    out[6]=(uint16_t)(0x8030+n%10);n/=10;
    out[5]=0x8022;
    out[4]=(uint16_t)(0x8030+n%10);n/=10;
    out[3]=(uint16_t)(0x8030+n%6);n/=6;
    out[2]=0x8027;
    out[1]=(uint16_t)(0x8030+n%10);n/=10;
    out[0]=(uint16_t)(n ? 0x8030+n%10 : 0x8020);
}

/* b364..b40c: ordered entry intents, after the separate audio reset prefix.
 * Mode1 stops nine channels; both entry branches then stop three more.
 * Zero terminates the sequence. No audio device access. */
static inline uint32_t nm_result_entry_command(unsigned mode,unsigned index) {
    static const uint32_t commands[]={0xb00700,0xb10700,0xb20700,0xb30700,
        0xb40700,0xb50700,0xb60700,0xb70700,0xb80700,0xbe3500,0xbe3400,0xbe3700};
    if(mode==2) return index<3 ? commands[9+index] : 0;
    return mode==1 && index<12 ? commands[index] : 0;
}

/* b484: exact countdown equality, one row only. Penalty suppresses without
 * advancing; missed thresholds are NOT caught up. Output is a silent intent. */
static inline int nm_result_announcement_step(const nm_result_announcement_t *rows,unsigned count,
        uint32_t penalty,int32_t countdown,uint32_t *cursor,uint32_t *command) {
    if(!cursor || !command) return 0;
    if(penalty) { *command=0;return 1; }
    if(!rows || !count || *cursor>=count) return 0;
    *command=0;
    if(countdown==rows[*cursor].countdown) { *command=rows[*cursor].command;++*cursor; }
    return 1;
}

/* b6a4: strict signed clock>threshold; advance ONE key, even after a jump.
 * Target is an offline index, never a guest pointer. Clock advances later in
 * the actor tick, after selection, movement and rendering. */
static inline int nm_result_target_select(const nm_result_key_t *keys,unsigned count,
        int32_t clock,uint32_t *cursor,uint32_t *target) {
    if(!keys || !cursor || !target || *cursor>=count) return 0;
    if(clock>keys[*cursor].tick) {
        *target=keys[*cursor].target;
        ++*cursor;
    }
    return 1;
}

static inline int16_t nm_result_angle_step(int16_t at,int16_t target) {
    const int delta=(int)target-at;
    /* Original arithmetic shift rounds negative deltas down, not toward0. */
    return (int16_t)(at+(delta>=0 ? delta/4 : -((-delta+3)/4)));
}

static inline int nm_result_actor_step(nm_result_actor_t *s,const nm_result_target_t target[8]) {
    if(!s || !target) return 0;
    nm_result_actor_t next; NM_COPY(&next,s);
    for(unsigned i=0;i<8;++i) {
        if(!isfinite(s->position[i].x) || !isfinite(s->position[i].y) ||
           !isfinite(target[i].position.x) || !isfinite(target[i].position.y) ||
           !isfinite(target[i].rate)) return 0;
        const float t=0.125f*target[i].rate;
#ifdef __DREAMCAST__
        next.position[i]=shz_vec2_lerp(s->position[i],target[i].position,t);
#else
        next.position[i].x=s->position[i].x+t*(target[i].position.x-s->position[i].x);
        next.position[i].y=s->position[i].y+t*(target[i].position.y-s->position[i].y);
#endif
        if(!isfinite(next.position[i].x) || !isfinite(next.position[i].y)) return 0;
        next.angle[i]=nm_result_angle_step(s->angle[i],target[i].angle);
        next.other_angle[i]=nm_result_angle_step(s->other_angle[i],target[i].other_angle);
    }
    NM_COPY(s,&next);return 1;
}

typedef struct {
    nm_result_actor_t actor;
    uint32_t clock,cursor,target;
} nm_result_playback_t;

/* b268..b3dc animation-owned entry state. Return1 selects b414 (labels/scroll),
 * 2 selects b4dc (no labels/scroll). Other-angle lanes are deliberately inherited.
 * Caller separately owns player speed, UI reset and silent audio intents. */
static inline int nm_result_playback_init(nm_result_playback_t *s,
        const nm_result_vec2_t initial[8],int8_t rank,uint32_t penalty,int16_t special) {
    if(!s || !initial) return 0;
    nm_result_playback_t next; NM_COPY(&next,s);
    for(unsigned i=0;i<8;++i) {
        if(!isfinite(initial[i].x) || !isfinite(initial[i].y)) return 0;
        next.actor.position[i]=initial[i];
        next.actor.angle[i]=0;
    }
    next.clock=next.cursor=next.target=0;
    NM_COPY(s,&next);
    return rank<=2 && !penalty && special ? 2 : 1;
}

/* b414/b4dc waiting decision: entering animation takes effect NEXT call.
 * This tick still requests labels/rank announcement. No countdown decrement.
 * Return bits:1 labels,2 rank-announcement callback,4 start sound ae1002.
 * mode1/2 are waiting, mode3 is active. Active calls do nothing here. */
static inline unsigned nm_result_wait_step(unsigned *mode,int32_t countdown,int16_t *scroll) {
    if(!mode || !scroll || (*mode!=1 && *mode!=2)) return 0;
    unsigned effects=2;
    if(*mode==1) {
        const int value=(int)*scroll-16;
        *scroll=(int16_t)(value<0 ? 0 : value);
        effects|=1;
    }
    if(countdown<=576) { *mode=3;effects|=4; }
    return effects;
}

/* b660..b6a0, AFTER this tick's movement/draw. The scene owns countdown
 * decrement; held input shortens it only at/above192, using signed compare. */
static inline void nm_result_clock_step(uint32_t *clock,int32_t *countdown,int skip) {
    ++*clock;
    if(*countdown>=192 && skip) { *countdown=192;*clock=1000; }
}

/* b654 composition, native indices into an offline target bank. Returned
 * pose is this tick's draw state, even when skip changes NEXT tick's clock.
 * Caller owns waiting/scene entry, countdown and queued rendering. */
static inline int nm_result_playback_step(nm_result_playback_t *s,
        const nm_result_key_t *keys,unsigned key_count,
        const nm_result_target_t targets[][8],unsigned target_count,
        int32_t *countdown,int skip) {
    if(!s || !countdown || !targets) return 0;
    nm_result_playback_t next; NM_COPY(&next,s);
    const int32_t signed_clock=next.clock<=INT32_MAX ? (int32_t)next.clock :
        -1-(int32_t)(UINT32_MAX-next.clock);
    if(!nm_result_target_select(keys,key_count,signed_clock,&next.cursor,&next.target) ||
       next.target>=target_count || !nm_result_actor_step(&next.actor,targets[next.target])) return 0;
    nm_result_clock_step(&next.clock,countdown,skip);
    NM_COPY(s,&next);return 1;
}
#endif
