/* Expert's world actors with state or a placement of their own (the course's actor table 236940[1]; the actors of
 * the file courseN-actors.dna, include/clock_actors.h nm_clock_actor_t). The others (22108c's figures, 22214c,
 * 222594/222678/22275c/222840, 223ce0) are frame-counter animations nm_clock_frame decides alone.
 *   BEARING   223bc0: in the draw bitmap, list arg[(angle(d, 220) * sign >> 6) & 63] in world coordinates, d the
 *             distance (command 43) of the camera entity 501240 (+1c, +24) from the point 223cc8 (sign 1), or from
 *             223cd4 (sign -1) while the player's flags bit 28 (wrong way) is set; angle: command 0a
 *   BIRDS     22430c: in the draw bitmap, five birds 2368c0[..] circling the actor: T(p) Ry(a) T(15, 0, 0) and
 *             Ry(a + 8000) (frame f(c)), after T(5, 2, 0) three at a + 500, 2800, 3800 (frame f(c/2 + 14)),
 *             a = (5010b0 << 7) + heading; in the podium (501090 bit 28; or with 501a80, the time attack, which this
 *             port never sets) after T(-12, -1, 0) two more at a' + 6000 and 6800, a' = ((c + 31) << 7) + heading,
 *             frame min((c + 31) & 3f, 31); f(x) = (x & 7f) > 5f ? 31 : x & 1f
 *   BIG_BIRD  224730: the scale (+34, 26 at the scene start) grows by 0.5 on each tick with a START edge (53e584
 *             bit 4) while it stays at most 100; in the draw bitmap T(p) Ry(heading) S(scale) 2368c0[c & 1f]
 *   HORSE     224854: a state machine (+c): circling (22487c: in the draw bitmap, the offset (25 sin b, 0, 35 cos b)
 *             of b = (n << 6) + heading, n the drawn ticks +40, drawn as T(p) Rz(222) T(offset) Ry(c000 - b) with
 *             236840[n & 1f]) until the player (501220 +1c, +24) comes within 8 (command 43) of where it was drawn;
 *             then it runs (224ab0, 224b1c, 224b60) from there along the player's heading +48 at 1.5 times the
 *             player's speed +34 (command 47), on the ground (11620), the heading and speed taken again whenever
 *             the player comes within 8, its cell from where it is; out of the draw bitmap it waits 2560 ticks
 *             (224c94, 224ca8) and starts again (224854)
 *   PAIR      224cd0: in the podium only (501090 bit 28), no cell: T(p) Ry(8000) 236840[c & 1f] and the same after
 *             T(1 + z, 0, 1 + z); then z (+24) -= 0.2
 *   STATUE    222264/222288, run by Expert's sound manager 22302c each tick: while the podium's timer 5010a8 is
 *             in 1016..1211 (501090 bit 28) or the player's profile +128 is in 242..320: on entering, 5036fe bit 0
 *             set and the entries 5036fc counted (both kept from power-on, never reset); a START edge while the
 *             player's speed index +18 is 0 counts +34; drawn as T(p) R 288f614, R (2223a8, racing 501090 bit 22
 *             only): with 501a8c 1 (the race's reverse finish) Ry(e000) Rx(8000), on every 64th entry
 *             ((5036fc + 1) & 3f == 0) Ry(2000) Rx(8000), else +34 at most 15 Ry(+34 << 12), at most 31 the same and
 *             Rx(8000), none beyond; leaving clears 5036fe bit 0 and +34
 * c is 5010b0; angles are 1/65536 turn, the TGP's commands take their low 16 bits. Float operations are the TGP's
 * commands, one at a time (force_math.h: 43 the length of (a - c, d - b), 0a the angle of (a, b), 47 the move,
 * 1d/1e r sin/cos, 0 add, 1 subtract, 2 multiply), so the states and the conditions are the arcade's bit for bit.
 * Presentation only: nothing here is read by a car, the race or the random number. The draws of a tick: per group
 * of the actor (in the tick's order) a frame (-1 none) and the placement program (T, Rx, Ry, Rz, S in the TGP's
 * order) the renderer composes (nm_xa_pose_matrix) for a PLACED group. Code: code/expert_actors.c, outside the
 * LTO unit, without fast math (Makefile). */
#ifndef DAYTONA_NATIVE_EXPERT_ACTORS_H
#define DAYTONA_NATIVE_EXPERT_ACTORS_H
#include <stdint.h>
#include "clock_actors.h"
#include "contact.h"
#ifdef __DREAMCAST__
#include <sh4zam/shz_trig.h>
#else
#include <math.h>
#endif

#define NM_XA_OPS 6u
enum { NM_XA_T, NM_XA_RX, NM_XA_RY, NM_XA_RZ, NM_XA_S };
typedef struct { uint8_t op; uint16_t angle; float v[3]; } nm_xa_op_t;
typedef struct {
    int16_t frame;     /* -1: not drawn */
    uint8_t count;     /* placement operations */
    nm_xa_op_t op[NM_XA_OPS];
} nm_xa_draw_t;

/* A horse 224854 (task words). */
enum { NM_XA_HORSE_START, NM_XA_HORSE_CIRCLE, NM_XA_HORSE_FLEE_START, NM_XA_HORSE_LATCH, NM_XA_HORSE_FLEE,
       NM_XA_HORSE_WAIT_START, NM_XA_HORSE_WAIT };
typedef struct {
    float x, y, z;      /* +1c..+24: the actor record's, never moved */
    float o[3];         /* +34, +38, +3c: the circling offset, then the running position */
    uint32_t heading;   /* +48: the player's word +48 (heading +48 and +4a) when it started running */
    float speed;        /* +44 */
    uint32_t drawn;     /* +40 */
    uint32_t wait;      /* +4c */
    uint16_t cell;      /* +2e */
    uint8_t state;      /* +c */
} nm_xa_horse_t;

typedef struct {
    uint8_t started;
    uint32_t last_tick;             /* the caller's clock: a smaller one is a new scene */
    float scale;                    /* BIG_BIRD +34 */
    float pair_z;                   /* PAIR +24 */
    uint16_t statue_count;          /* STATUE +34 */
    nm_xa_horse_t horse[8];
    nm_xa_draw_t draw[NM_CLOCK_GROUPS_MAX];
    float origin[2];                /* the camera entity's x, z (BEARING) */
} nm_xa_state_t;
/* 5036fc, 5036fe: the statue's entries and its zone bit, from power-on (no scene resets them). */
typedef struct { uint16_t entries, inside; } nm_xa_shared_t;

/* What one tick reads. */
typedef struct {
    const uint32_t *bitmaps;        /* 16 words: the draw bitmap 501520, then the near bitmap 501540 (the pass before's) */
    uint32_t counter;               /* 5010b0 */
    uint32_t flags;                 /* 501090 */
    int32_t phase_timer;            /* 5010a8 */
    uint32_t reverse;               /* 501a8c */
    uint8_t start_edge;             /* 53e584 bit 4 */
    /* The player entity 501220: word 0, +1c..+24, +34, +18, +48 (and +4a), +128. */
    uint32_t player_flags;
    float player_position[3];
    float player_speed;
    int32_t player_speed_index;
    uint32_t player_heading;
    uint16_t player_profile;
    /* The camera entity 501240's x, z. */
    float camera[2];
} nm_xa_input_t;

#ifdef __cplusplus
extern "C" {
#endif
/* The actors' first tick of a scene (2002c and their init entries) and one tick of all of them in the pool's order.
 * dna: the course's DNA1 file (validated, with actors). 0: refused (an input not finite, a ground query refused). */
int nm_xa_tick(nm_xa_state_t *s, nm_xa_shared_t *shared, const void *dna, const nm_drive_params_t *math,
               const nm_contact_pack_t *contact, const nm_xa_input_t *in);
#ifdef __cplusplus
}
#endif

/* The placement (column-major 3x4, model to world) of a draw: the operations applied to the identity in order, each
 * as the TGP's command multiplies the current matrix on the right (props: practice_cone_rotate's columns).
 * Renderer math: SH4ZAM's sine on the console, libm's on the host. */
static inline void nm_xa_rotate(float m[12],unsigned axis,uint16_t angle) {
#ifdef __DREAMCAST__
    const shz_sincos_t sc=shz_sincosu16(angle);
    const float c=sc.cos,sn=sc.sin;
#else
    const float turn=(float)angle*(6.28318530717958647692f/65536.0f),c=cosf(turn),sn=sinf(turn);
#endif
    float *const a=m+(axis==0 ? 3 : 0),*const b=m+(axis==2 ? 3 : 6);   /* X: c1, c2; Y: c0, c2; Z: c0, c1 */
    for(unsigned r=0;r<3;++r) {
        const float u=a[r],v=b[r];
        if(axis==1) { a[r]=u*c+v*sn; b[r]=v*c-u*sn; }
        else { a[r]=u*c-v*sn; b[r]=u*sn+v*c; }
    }
}
static inline void nm_xa_pose_matrix(const nm_xa_draw_t *d,float m[12]) {
    for(unsigned i=0;i<12;++i) m[i]=(i==0 || i==4 || i==8) ? 1.0f : 0.0f;
    for(unsigned k=0;k<d->count && k<NM_XA_OPS;++k) {
        const nm_xa_op_t *o=d->op+k;
        if(o->op==NM_XA_T) {
            for(unsigned r=0;r<3;++r) m[9+r]+=m[r]*o->v[0]+m[3+r]*o->v[1]+m[6+r]*o->v[2];
        } else if(o->op==NM_XA_S) {
            for(unsigned c=0;c<3;++c) for(unsigned r=0;r<3;++r) m[3*c+r]*=o->v[c];
        } else nm_xa_rotate(m,(unsigned)(o->op-NM_XA_RX),o->angle);
    }
}
#endif
