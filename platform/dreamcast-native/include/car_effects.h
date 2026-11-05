#ifndef DAYTONA_NATIVE_CAR_EFFECTS_H
#define DAYTONA_NATIVE_CAR_EFFECTS_H
/* The player's car effects (daytona93, spec build/dc-native-effects.11xv92/spec-effects.md): presentation only.
 *
 * Contact effects 9088 (called by the player's draw 6c0c at 6da4 / 6f00, after 9014's timer decrement, which is
 * nm_player_effect_timers): a pure function of gameplay fields the native holds bit-exact, so there is no state of
 * its own. In call order (t = the signed timer byte after 9014):
 *   9500  +91 (surface_ticks[0]) != 0: dust 231f06[t & 31], T(-0.7, 0, -1.75) S(0.5 + (+34)/4)    punch-through
 *   95c4  +92 (surface_ticks[1]) != 0: dust 231f06[t & 31], T( 0.7, 0, -1.75) S(0.5 + (+34)/4)    punch-through
 *   9688/9740 +93/+94: matrix work, no draw (and the mask bit never occurs on course 0)
 *   97f8  +95 (force.trigger_count[0]) != 0: smoke 231f86[t & 31], S(1, 0.3 + (1 - (+180)), 1)   checker (translucent)
 *   9888  +96 (force.trigger_count[1]) != 0: smoke 231f86[t & 31], the same scale                 checker (translucent)
 *   9148  for +98 (response_ticks) and again for +99 (boundary.condition) when nonzero: r = +98 ? +98 : +99,
 *         nothing once 9148 cleared both (|+d6| > 0x1000: nm_player_contact_clear, gameplay, already applied to
 *         the state this reads), else spark 232006[(r + 8) & 31] at a front corner of the contact side +ed
 *         (boundary.damage.side; 0 left, 3 right, any other side nothing) by the view preset 501710: 1 (VR2, hood)
 *         T(-+1.6, 0.065, 2.9) Rx(0xeeee) Ry(+-0x1555) S(0.5); 2 (VR3, the port's view) T(-+1.3, 0.2, 3.4);
 *         3 (VR4) T(-+1.0, 0.2, 2.4); 0 (VR1, driver's view) nothing. Both timers set: the same spark twice.
 * TGP conventions as pose.h (M := M * R, Rx/Ry/Rz as nm_body_pose's angles 0/1/2). The frame is the car frame of
 * the player's draw with the wheel routine's undo Rx(-(+52)) Rz(-(+56)) when 7384 ran (7424..7458, outside its
 * push/pop): nm_car_effect_frame. Matched against the original's logs on every 9088 call (spec section 8).
 *
 * Models: one model per table entry (each entry is a list of one record). The rules reach dust 1..31 (t = 31..1),
 * smoke 1..31, sparks 9..24 (r = 1..16) and spin smoke 0..26 (four models): 82 lists, 82 models, 15 texture regions
 * of 64x64. The car pack carries all of them (tools/append_player_parts.py --effects, before the player's six
 * parts): dust and sparks (47 models, 10 texture regions) and smoke and spin smoke (35 models, 5 texture regions).
 *
 * Lists: dust and sparks (header 6409: texel 15 transparent, no checker) go through the model renderer's
 * punch-through path like every car cutout. Smoke and spin smoke (header e409: checker) are the arcade's
 * screen-door half coverage; DNT6 gives them the translucent class (tools/prepare_palette_materials.py: vertex
 * alpha 0x80, entry 15 alpha 0) and the model renderer draws them on the translucent list (alpha 0.5, depth
 * compare, no depth write, pre-sorted back to front by each polygon's nearest depth: model_submission.h,
 * "Translucent effects"). nm_car_effect_t.translucent marks them. The spin smoke (task 9e2c) is the one effect
 * with state of its own: nm_spin_smoke_t below, stepped once per gameplay tick. */
#include "player.h"
#include "pose.h"
#include <stdint.h>
#include <string.h>
#include "nm_mem.h"

enum { NM_FX_DUST, NM_FX_SMOKE, NM_FX_SPARK, NM_FX_SPIN, NM_FX_KINDS };
/* 9088's draw calls, by the routine that issues them (for the log check: their return addresses). */
enum { NM_FX_CALL_DUST_LEFT, NM_FX_CALL_DUST_RIGHT, NM_FX_CALL_SMOKE_SLIP, NM_FX_CALL_SMOKE_SPIN, NM_FX_CALL_SPARK };
enum { NM_CAR_EFFECTS_MAX = 6 };   /* two dust, two smoke, two sparks */

/* Model of each table entry, 231f06 / 231f86 / 232006 (32 words each) and 232206 (27, by particle life). */
static const uint32_t nm_fx_dust_models[32]={
    0x8c705f,0x8c704a,0x8c7035,0x8c7020,0x8c6ff7,0x8c6fce,0x8c6fa5,0x8c6f7c,0x8c6f3f,0x8c6f02,0x8c6ec5,0x8c6e88,
    0x8c6e37,0x8c6de6,0x8c6d95,0x8c6d44,0x8c6cf3,0x8c6ca2,0x8c6c51,0x8c6c00,0x8c6bc3,0x8c6b86,0x8c6b49,0x8c6b0c,
    0x8c6ae3,0x8c6aba,0x8c6a91,0x8c6a68,0x8c6a53,0x8c6a3e,0x8c6a29,0x8c6a14};
static const uint32_t nm_fx_smoke_models[32]={
    0x8d52e8,0x8d52bf,0x8d5296,0x8d526d,0x8d521c,0x8d51cb,0x8d517a,0x8d5129,0x8d50b0,0x8d5037,0x8d4fbe,0x8d4f45,
    0x8d4ea4,0x8d4e03,0x8d4d62,0x8d4cc1,0x8d4c20,0x8d4b7f,0x8d4ade,0x8d4a3d,0x8d49c4,0x8d494b,0x8d48d2,0x8d4859,
    0x8d4808,0x8d47b7,0x8d4766,0x8d4715,0x8d46ec,0x8d46c3,0x8d469a,0x8d4671};
static const uint32_t nm_fx_spark_models[32]={
    0x9518dc,0x9518c7,0x9518b2,0x95189d,0x951874,0x95184b,0x951822,0x9517f9,0x9517bc,0x95177f,0x951742,0x951705,
    0x9516b4,0x951663,0x951612,0x9515c1,0x951570,0x95151f,0x9514ce,0x95147d,0x951440,0x951403,0x9513c6,0x951389,
    0x951360,0x951337,0x95130e,0x9512e5,0x9512d0,0x9512bb,0x9512a6,0x951291};
enum { NM_FX_SPIN_LIVES = 27 };
static const uint32_t nm_fx_spin_models[NM_FX_SPIN_LIVES]={
    0x8d6978,0x8d6978,0x8d6978,0x8d6978,0x8d6978,0x8d6978,0x8d6978,0x8d6978,0x8d6978,0x8d6978,
    0x8d68e1,0x8d68e1,0x8d68e1,0x8d68e1,0x8d68e1,0x8d68e1,0x8d68e1,
    0x8d684a,0x8d684a,0x8d684a,0x8d684a,0x8d684a,0x8d684a,0x8d67b3,0x8d67b3,0x8d67b3,0x8d67b3};
/* The entries the rules reach, and so the ones the car pack carries: [first, last] per kind. */
static const uint8_t nm_fx_exported[NM_FX_KINDS][2]={{1,31},{1,31},{9,24},{0,26}};
static inline uint32_t nm_fx_model(unsigned kind,unsigned index) {
    if(kind>=NM_FX_KINDS || index<nm_fx_exported[kind][0] || index>nm_fx_exported[kind][1]) return 0;
    return kind==NM_FX_DUST ? nm_fx_dust_models[index] : kind==NM_FX_SMOKE ? nm_fx_smoke_models[index] :
        kind==NM_FX_SPARK ? nm_fx_spark_models[index] : nm_fx_spin_models[index];
}

/* One draw of 9088: the routine, the table entry, the contact side (sparks) and the local matrix parameters
 * T(offset) [Rx(pitch) Ry(yaw)] S(scale) in the effect frame. translucent: a checker material (smoke). */
typedef struct {
    uint8_t call, kind, index, side, translucent, rotated;
    int16_t pitch, yaw;
    float offset[3], scale[3];
} nm_car_effect_t;

/* 9088 for the player state after the tick (9014 and 9148's clear applied) and view preset 501710 (0..3; the
 * port's follow view is 2). Writes the draws in call order; returns their number (0..NM_CAR_EFFECTS_MAX). */
static inline unsigned nm_car_effects(const nm_player_t *car,unsigned view,nm_car_effect_t out[NM_CAR_EFFECTS_MAX]) {
    unsigned n=0;
    const float dust=0.5f+car->boundary.speed/4.0f;       /* 9548..9584: op 3 (+34 / 4), op 0 (+ 0.5) */
    for(unsigned w=0;w<2;++w) {
        const int8_t t=car->surface_ticks[w];
        if(!t) continue;
        nm_car_effect_t *e=out+n++;
        nm_memset(e,0,sizeof(*e));
        e->call=(uint8_t)(NM_FX_CALL_DUST_LEFT+w); e->kind=NM_FX_DUST; e->index=(uint8_t)(t&31);
        e->offset[0]=w ? 0.7f : -0.7f; e->offset[2]=-1.75f;
        e->scale[0]=e->scale[1]=e->scale[2]=dust;
    }
    const float height=0.3f+(1.0f-car->force.grip[1]);  /* 9814..9874: op 1 (1 - +180), op 0 (0.3 + it) */
    for(unsigned w=0;w<2;++w) {
        const int8_t t=car->force.trigger_count[w];
        if(!t) continue;
        nm_car_effect_t *e=out+n++;
        nm_memset(e,0,sizeof(*e));
        e->call=(uint8_t)(NM_FX_CALL_SMOKE_SLIP+w); e->kind=NM_FX_SMOKE; e->index=(uint8_t)(t&31); e->translucent=1;
        e->scale[0]=e->scale[2]=1.0f; e->scale[1]=height;
    }
    const int8_t timers[2]={car->response_ticks,car->boundary.condition};
    for(unsigned c=0;c<2;++c) {
        if(!timers[c]) continue;                                  /* 90d4 / 90e0 */
        const int8_t r=timers[0] ? timers[0] : timers[1];         /* 9148 */
        if(car->inhibit_angle<-0x1000 || car->inhibit_angle>0x1000) break; /* 9158..9168: cleared (gameplay) */
        const unsigned side=car->boundary.damage.side;
        if((side!=0 && side!=3) || view<1 || view>3) continue;   /* 9178..91cc, 501710 */
        const float sign=side ? 1.0f : -1.0f;
        nm_car_effect_t *e=out+n++;
        nm_memset(e,0,sizeof(*e));
        e->call=NM_FX_CALL_SPARK; e->kind=NM_FX_SPARK; e->index=(uint8_t)((r+8)&31); e->side=(uint8_t)side;
        static const float place[3][4]={{1.6f,0.065f,2.9f,0.5f},{1.3f,0.2f,3.4f,1.0f},{1.0f,0.2f,2.4f,1.0f}};
        const float *p=place[view-1];
        e->offset[0]=sign*p[0]; e->offset[1]=p[1]; e->offset[2]=p[2];
        e->scale[0]=e->scale[1]=e->scale[2]=p[3];
        if(view==1) { e->rotated=1; e->pitch=(int16_t)0xeeee; e->yaw=(int16_t)(side ? -0x1555 : 0x1555); }
    }
    return n;
}

/* The frame the effects are drawn in, from the body's view matrix (player_parts.h result[NM_PLAYER_PART_BODY],
 * crash rotations included): with the wheel routine (show & NM_PLAYER_SHOW_WHEELS) times its pitch/bank undo,
 * which is also the normal shadow's matrix; without it (hood or driver's view) the body matrix itself. */
static inline int nm_car_effect_frame(const nm_drive_params_t *p,const nm_player_t *car,unsigned wheels,
        const float body[12],float out[12]) {
    if(!wheels) { nm_memcpy(out,body,12*sizeof(float)); return 1; }
    const float zero[3]={0};
    const int16_t undo[3]={nm_yaw_angle(0u-(uint32_t)(int32_t)car->filtered_pitch),0,
        nm_yaw_angle(0u-(uint32_t)(int32_t)car->bank)};
    float part[12];
    return nm_body_pose(p,zero,undo,part) && nm_pose_compose(body,part,out);
}

/* The effect's matrix: frame * T(offset) [* Rx(pitch) * Ry(yaw)] * S(scale). */
static inline int nm_car_effect_matrix(const nm_drive_params_t *p,const float frame[12],const nm_car_effect_t *e,
        float out[12]) {
    float local[12]={e->scale[0],0,0, 0,e->scale[1],0, 0,0,e->scale[2], e->offset[0],e->offset[1],e->offset[2]};
    if(e->rotated) {
        const float zero[3]={0};
        const int16_t pitch[3]={e->pitch,0,0},yaw[3]={0,e->yaw,0};
        float a[12],b[12],scale[12]={e->scale[0],0,0, 0,e->scale[1],0, 0,0,e->scale[2], 0,0,0};
        if(!nm_body_pose(p,e->offset,pitch,a) || !nm_body_pose(p,zero,yaw,b) ||
           !nm_pose_compose(a,b,a) || !nm_pose_compose(a,scale,local)) return 0;
    }
    return nm_pose_compose(frame,local,out);
}

/* Spin smoke, task 9e2c (pool row 66, after the HUD rule a818; spec section 5): 8 world-space particles of the car
 * that last started a spin (5014e4 = race.shared.inhibit_owner: the player at the scene start, then the opponent
 * whose spin start 15b5c/15b80 wrote it; the player's own spin start does not), stepped once per gameplay tick of
 * the race phases 20..24 (created by the race setup 31b4, stopped by time up 422c and the podium), reset with the
 * scene. Layout of the original's object (+34 + 24i: x, y, z, vx, vz, s; lives +f8; ring +f4), the twelve angles
 * 501800.. and the counter 501836. Presentation state: nothing reads it back, and no random number is drawn.
 * Per tick (9f28..a134, a1b8, a258), c = the source car, p = the player:
 *   emit  |c.+d4| > 10 and (c.+d4 & 7) == 0: slots ring (life 26) and ring+1 (life 13), ring += 2;
 *         points T(c.x, c.y, c.z) Ry(c.+58 + c.+d6) Rz(c.+2c) (0, 0.33, -+2.7) (behind, in front), velocity
 *         (c.+9c - 0.5 c.+9c, c.+a4 - 0.5 c.+a4), scale 0.4
 *   the twelve angle halfwords += 512; the circle of radius 2321f6[view] around (p.x, p.z) (6.5 for preset 2)
 *   each slot with life: life -= 1; inside the circle (before the move): g3 := 0, counter -= 1, g3 := -1 unless
 *         the counter was above 1 (so the first two inside particles in slot order are hidden, a later one is
 *         drawn, one outside repeats the previous slot's visibility); scale += 0.2, x += vx, y -= 0.08, z += vz;
 *         drawn (g3 == 0) as T(x, y, z) S(scale) Rx Ry Rz (the angle triple of slot & 3) with 232206[life].
 * The particle arithmetic is plain float32 (bit-exact against the logs); the emission point is the TGP transform
 * (nm_body_pose: within 1e-4). Matched against the original's Q/G records on every logged tick (spec section 8). */
enum { NM_SPIN_SMOKE_PARTICLES = 8 };
typedef struct {
    float particle[NM_SPIN_SMOKE_PARTICLES][6];      /* x, y, z, vx, vz, scale */
    uint8_t life[NM_SPIN_SMOKE_PARTICLES], ring;
    uint8_t drawn;             /* this tick's drawn slots (bit i: slot i), a258's g3 rule */
    uint16_t angles[12];       /* the halfwords 501800..501816, += 512 a tick (nm_spin_smoke_reset) */
    uint16_t counter;          /* 501836 */
    uint32_t ticks, emitted;   /* statistics */
} nm_spin_smoke_t;

/* What the step reads of the source car (5014e4), by the original's offsets. */
typedef struct {
    int8_t timer;              /* +d4 input_inhibit */
    int16_t spin_angle;        /* +d6 inhibit_angle */
    int16_t yaw, roll;         /* +58, +2c: boundary.ground.yaw, .roll */
    float position[3];         /* +1c..+24: boundary.ground.position */
    float delta[2];            /* +9c, +a4: the tick's x / z displacement */
} nm_spin_smoke_source_t;
static inline nm_spin_smoke_source_t nm_spin_smoke_source(const nm_player_t *car) {
    nm_spin_smoke_source_t s;
    s.timer=car->input_inhibit; s.spin_angle=car->inhibit_angle;
    s.yaw=car->boundary.ground.yaw; s.roll=car->boundary.ground.roll;
    nm_memcpy(s.position,car->boundary.ground.position,sizeof(s.position));
    nm_memcpy(s.delta,car->delta,sizeof(s.delta));
    return s;
}

/* 9e2c..9f24: the task's first tick of a scene, which falls through into the step 9f28 (no particle yet, so of
 * the step only a1b8's +512 shows): it runs on the original's tick before the native race's first one (the state
 * of trace row 0), so the reset holds the angles after it. The logs' first 9f28 entry is the native tick 1's. The init stores the twelve angles (0, 4000, 8000), (4000, 8000,
 * c000), (8000, c000, 0), (c000, 0, 4000) as halfwords at WORD stride (stis 501800, 501804, ..., 50182c), but a1b8
 * adds 512 to the twelve consecutive halfwords 501800..501816 and a3bc reads slot i's triple from halfword 3*(i & 3)
 * of 501800: the angles that count are the first six values interleaved with the halfwords between them, which
 * the init leaves as they were: 0xaa00 at the race entry in all eight logs. */
static inline void nm_spin_smoke_reset(nm_spin_smoke_t *s) {
    static const uint16_t angles[12]={0,0xaa00,0x4000, 0xaa00,0x8000,0xaa00, 0x4000,0xaa00,0x8000, 0xaa00,0xc000,0xaa00};
    nm_memset(s,0,sizeof(*s));
    for(unsigned i=0;i<12;++i) s->angles[i]=(uint16_t)(angles[i]+512u);
}
/* The race phases the task runs in (20..24; created by the race setup, stopped at 25 and in the podium). */
static inline int nm_spin_smoke_phase(unsigned phase) { return phase>=20 && phase<=24; }

/* One tick of 9f28 (view preset 501710: 0..3). Returns 0 on a non-finite emission point. */
static inline int nm_spin_smoke_step(const nm_drive_params_t *p,nm_spin_smoke_t *s,const nm_spin_smoke_source_t *c,
        float player_x,float player_z,unsigned view) {
    static const float radius[4]={9.0f,8.0f,6.5f,5.0f};          /* 2321f6 */
    const int timer=c->timer;
    if((timer>10 || timer<-10) && (timer&7)==0) {                  /* 9f30..9f48 */
        const unsigned a=s->ring,b=(s->ring+1u)&7u;
        s->life[a]=26; s->life[b]=13; s->ring=(uint8_t)((s->ring+2u)&7u);
        float frame[12];
        const int16_t angles[3]={0,(int16_t)(uint16_t)((uint16_t)c->yaw+(uint16_t)c->spin_angle),c->roll};
        if(!nm_body_pose(p,c->position,angles,frame)) return 0;     /* T Ry Rz (op 10, 12, 15, 16, 1a) */
        const float vx=c->delta[0]-0.5f*c->delta[0],vz=c->delta[1]-0.5f*c->delta[1];   /* a0a4..a130 */
        for(unsigned k=0;k<2;++k) {
            float *q=s->particle[k ? b : a];
            const float z=k ? 2.7f : -2.7f;
            for(unsigned r=0;r<3;++r) q[r]=frame[3+r]*0.33f+frame[6+r]*z+frame[9+r];
            if(!isfinite(q[0]) || !isfinite(q[1]) || !isfinite(q[2])) return 0;
            q[3]=vx; q[4]=vz; q[5]=0.4f;
        }
        s->emitted+=2;
    }
    for(unsigned i=0;i<12;++i) s->angles[i]=(uint16_t)(s->angles[i]+512u);  /* a1b8 */
    const float r=radius[view&3];                                   /* a14c..a190: TGP 40 */
    s->counter=1;
    unsigned g3=0,drawn=0;                                          /* g3 left from earlier code: 0 on every logged tick */
    for(unsigned i=0;i<NM_SPIN_SMOKE_PARTICLES;++i) {               /* a258 */
        if(!s->life[i]) continue;
        --s->life[i];
        float *q=s->particle[i];
        const float dx=q[0]-player_x,dz=q[2]-player_z;
        if(dx*dx+dz*dz<r*r) {                                       /* a284..a2c0: TGP 41 bit 31 */
            const unsigned n=s->counter;
            g3=0; s->counter=(uint16_t)(n-1u);
            if(!((int)n-1>0)) g3=1;
        }
        q[5]=q[5]+0.2f; q[0]=q[0]+q[3]; q[1]=q[1]-0.08f; q[2]=q[2]+q[4];   /* a2c4..a358 */
        if(!g3) drawn|=1u<<i;
    }
    s->drawn=(uint8_t)drawn;
    ++s->ticks;
    return 1;
}

/* The scene's tick of the task (code/practice.c and tests/trace_race_native.c, after the wrong-way rule a818):
 * `phase` the race's active phase, `source` the car race.shared.inhibit_owner names, `player` race.cars[0]. Outside
 * phases 20..24 the task does not run and draws nothing. */
static inline int nm_spin_smoke_tick(const nm_drive_params_t *p,nm_spin_smoke_t *s,unsigned phase,
        const nm_player_t *source,const nm_player_t *player,unsigned view) {
    if(!nm_spin_smoke_phase(phase)) { s->drawn=0; return 1; }
    const nm_spin_smoke_source_t c=nm_spin_smoke_source(source);
    return nm_spin_smoke_step(p,s,&c,player->boundary.ground.position[0],player->boundary.ground.position[2],view);
}

/* A drawn particle's matrix: view * T(x, y, z) * S(scale) * Rx(a0) * Ry(a1) * Rz(a2) (a35c..a42c; the angle triple
 * of slot & 3), through pose.h (SH4ZAM on the console). Its model is nm_fx_spin_models[life]. */
static inline int nm_spin_smoke_matrix(const nm_drive_params_t *p,const float view[12],const nm_spin_smoke_t *s,
        unsigned slot,float out[12]) {
    const float zero[3]={0};
    const uint16_t *a=s->angles+3u*(slot&3u);
    const int16_t ax[3]={(int16_t)a[0],0,0},ay[3]={0,(int16_t)a[1],0},az[3]={0,0,(int16_t)a[2]};
    float rx[12],ry[12],rz[12];
    if(!nm_body_pose(p,zero,ax,rx) || !nm_body_pose(p,zero,ay,ry) || !nm_body_pose(p,zero,az,rz) ||
       !nm_pose_compose(rx,ry,rx) || !nm_pose_compose(rx,rz,rx)) return 0;
    const float *q=s->particle[slot];
    for(unsigned i=0;i<9;++i) rx[i]*=q[5];
    rx[9]=q[0]; rx[10]=q[1]; rx[11]=q[2];
    return nm_pose_compose(view,rx,out);
}
#endif
