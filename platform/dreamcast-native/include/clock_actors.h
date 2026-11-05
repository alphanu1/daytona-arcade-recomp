/* The frame-counter animations of Advanced: the world actors 220304, 2209e4, 220a7c, 221f10, 222030, 222090,
 * 222c48 and 222cf0 of the course's actor table 236940[2]. Each draws list[(5010b0 >> shift) & (frames-1)] of its
 * own table in world coordinates when its condition holds (tools/prepare_world_objects.py, "frame-counter
 * animations": the code of each handler):
 *   RUN        one of its cells (one to three: 238b14/238b16, 7a/6a, 99/89, 2393c0/c2/c4, or its own) in the
 *              draw bitmap 501520 (near 0) or the near bitmap 501540 (near 1)
 *   ALWAYS     222cf0
 *   PROFILE    221f10: while the latched flags 501090 select phase 22 (0x400000, racing) the player's (501220)
 *              profile +128 in window[0]..window[1] (unsigned, both ends included), else its own cell in the draw
 *              bitmap
 *   LAPS_LOW / LAPS_HIGH  222090: its cell in the near bitmap and the player's completed laps +6d 0..2 / above 2
 * Expert's (include/expert_actors.h) add:
 *   FAR_PROFILE  22108c's thirteen figures: its own cell not in the near bitmap and the player's profile in
 *              window[0]..window[1] (unsigned, no phase test)
 *   LAPS_NONE / LAPS_SOME  22214c: its cell in the near bitmap and the player's completed laps 0 / not 0
 *   WINDOW     223ce0: the player's profile in window[0]..window[1], no cell (frames 1: a static list)
 *   ACTOR      the actors with state, the bearing and the birds (expert_actors.h): the actor's tick decides the
 *              frame and, for a PLACED group, the placement; nm_clock_frame returns -1
 * The bitmaps are those of the pass before (cells_previous), as for every world actor; the counter 5010b0 is the
 * one of nm_practice_render's world objects (render/scene/practice.c). These actors keep no state of their own:
 * what they draw is a function of this tick's values, so nm_clock_frame is the whole of their behaviour.
 *
 * What a frame draws (DNA1, tools/prepare_world_objects.py clock, the course's courseN-actors.dna): one base
 * mesh a group in the world pack (an instance of kind NM_OBJECT_CLOCK, meshpack.h) and, per frame, what changes
 * in it, in the pack's own vertex and face order:
 *   positions of the points that move, int16 per axis (origin + q * step, step about 1/32000 of the extent),
 *     stored every `rate` frames (1 or 8: 220304, whose interpolation errs 0.067 units at most) and interpolated
 *     linearly in between (cyclic); rate 0: no point moves; a vertex record of a still point keeps its position
 *   normals (int16 / NM_CLOCK_NORMAL_SCALE) of the faces whose side a frame may test, at the positions' rate
 *   the state of each face that changes: its texture header and luma per side (the binding of the world's
 *     material pack the renderer draws it with), at every frame
 *   u, v of the vertex records whose u, v change: one of a few sets, at every frame
 * A PLACED group (flags bit 0, Expert's birds, horses and statue) is in model coordinates: its instance is drawn
 * with the view times the placement its actor's tick gives, and its lumas are evaluated for that placement every
 * frame it is drawn (render/models/model_renderer.c, lighting.h's terms): its normals are stored for every face.
 * Groups may share tables (equal offsets): the frames of one list table drawn by several instances.
 * With header flags bit 0 the file also holds the actors (nm_clock_actor_t, reserved[0] of them at offset
 * reserved[1]): each one's kind, position, heading, cell and its groups (first_group, groups: one a draw of its tick,
 * in the tick's order).
 * Little endian, 4-byte aligned tables at the offsets of the group record. */
#ifndef DAYTONA_NATIVE_CLOCK_ACTORS_H
#define DAYTONA_NATIVE_CLOCK_ACTORS_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "meshpack.h"
#ifdef __DREAMCAST__
#include <sh4zam/shz_scalar.h>
#endif

#define NM_CLOCK_MAGIC 0x31414e44u /* "DNA1" */
#define NM_CLOCK_GROUPS_MAX 64u
#define NM_CLOCK_ACTORS_MAX 32u
#define NM_CLOCK_NORMAL_SCALE 8192.0f
#define NM_CLOCK_STILL 0xffffu     /* vertex_point of a vertex record whose point does not move */
#define NM_CLOCK_MOVED_POINTS 320u /* a frame's points on the stack (model_renderer.c); the packs' largest group: 264 */
enum { NM_CLOCK_RUN, NM_CLOCK_ALWAYS, NM_CLOCK_PROFILE, NM_CLOCK_LAPS_LOW, NM_CLOCK_LAPS_HIGH,
       NM_CLOCK_FAR_PROFILE, NM_CLOCK_LAPS_NONE, NM_CLOCK_LAPS_SOME, NM_CLOCK_WINDOW, NM_CLOCK_ACTOR };
#define NM_CLOCK_GROUP_PLACED 1u   /* group flags (nm_clock_group_t.flags) */
#define NM_CLOCK_HAS_ACTORS 1u     /* header flags */
/* Actor kinds (include/expert_actors.h): the handler whose tick the actor's groups draw. */
enum { NM_CLOCK_BEARING=1, NM_CLOCK_BIRDS, NM_CLOCK_BIG_BIRD, NM_CLOCK_HORSE, NM_CLOCK_PAIR, NM_CLOCK_STATUE,
       NM_CLOCK_KINDS };
enum { NM_CLOCK_VERTEX_POINT, NM_CLOCK_POSITIONS, NM_CLOCK_STATES, NM_CLOCK_STATE_FACES, NM_CLOCK_FACE_STATES,
       NM_CLOCK_NORMAL_FACES, NM_CLOCK_NORMALS, NM_CLOCK_UV_VERTICES, NM_CLOCK_UV_SETS, NM_CLOCK_UV_FRAMES,
       NM_CLOCK_TABLES };

typedef struct { uint32_t magic, size, groups, flags, reserved[4]; } nm_clock_header_t;
typedef struct {
    uint32_t mesh;                  /* the world pack's mesh */
    uint8_t cond, near, shift, cell_count;
    uint8_t cell[4];                /* cell[0]: the instance's (its own, or the first the handler tests) */
    uint16_t window[2];             /* PROFILE */
    uint16_t frames, rate, points, vertices, faces, states, state_faces, normal_faces, uv_vertices, uv_sets;
    uint16_t variant, flags;        /* variant: the instance's place in its selection run; NM_CLOCK_GROUP_* */
    float origin[3], step[3];
    uint32_t offset[NM_CLOCK_TABLES];
} nm_clock_group_t;
typedef struct { uint16_t header[4]; uint8_t luma[2]; uint16_t reserved; } nm_clock_state_t;
/* An actor: handler (its init entry), position and heading (the actor record: task +1c..+24, +2a), cell (16e10 of
 * the position), kind (NM_CLOCK_BEARING..), its groups, param: BEARING the two points of 223cc8 and 223cd4. */
typedef struct {
    uint32_t handler;
    float position[3];
    int16_t heading;
    uint16_t cell;
    uint8_t kind, groups;
    uint16_t first_group;
    float param[6];
} nm_clock_actor_t;
_Static_assert(sizeof(nm_clock_header_t)==32 && sizeof(nm_clock_group_t)==104 && sizeof(nm_clock_state_t)==12 &&
    sizeof(nm_clock_actor_t)==48, "DNA1 records");
/* The groups each kind's tick draws, in its order (expert_actors.h). */
static inline unsigned nm_clock_kind_groups(unsigned kind) {
    static const uint8_t count[NM_CLOCK_KINDS]={0,1,7,1,1,2,1};
    return kind<NM_CLOCK_KINDS ? count[kind] : 0u;
}

/* Inputs of one tick, as the actors read them. */
typedef struct {
    const uint32_t *bitmaps;        /* 16 words: the draw bitmap 501520, then the near bitmap 501540 */
    uint32_t counter;               /* 5010b0 */
    uint32_t flags;                 /* the latched flags 501090 */
    uint16_t profile;               /* the player's +128 */
    uint8_t laps;                   /* the player's +6d */
} nm_clock_input_t;

static inline int nm_clock_cell(const uint32_t *bitmaps,unsigned near,unsigned cell) {
    return (int)(bitmaps[(near ? 8u : 0u)+(cell>>5)]>>(cell&31u)&1u);
}
/* The frame group g draws this tick, -1 when it draws nothing. */
static inline int nm_clock_frame(const nm_clock_group_t *g,const nm_clock_input_t *in) {
    int drawn=0;
    switch(g->cond) {
    case NM_CLOCK_ALWAYS: drawn=1; break;
    case NM_CLOCK_PROFILE:
        drawn=in->flags&UINT32_C(0x400000) ? in->profile>=g->window[0] && in->profile<=g->window[1] :
            nm_clock_cell(in->bitmaps,0,g->cell[0]);
        break;
    case NM_CLOCK_LAPS_LOW: case NM_CLOCK_LAPS_HIGH:
        drawn=nm_clock_cell(in->bitmaps,1,g->cell[0]) && (in->laps<=2u)==(g->cond==NM_CLOCK_LAPS_LOW);
        break;
    case NM_CLOCK_FAR_PROFILE:   /* 2210ac..2210c8, 2214f4..221514 */
        drawn=!nm_clock_cell(in->bitmaps,1,g->cell[0]) && in->profile>=g->window[0] && in->profile<=g->window[1];
        break;
    case NM_CLOCK_LAPS_NONE: case NM_CLOCK_LAPS_SOME:   /* 22216c..222188, 222220..222228 */
        drawn=nm_clock_cell(in->bitmaps,1,g->cell[0]) && (in->laps==0u)==(g->cond==NM_CLOCK_LAPS_NONE);
        break;
    case NM_CLOCK_WINDOW:   /* 223cfc..223d1c */
        drawn=in->profile>=g->window[0] && in->profile<=g->window[1];
        break;
    case NM_CLOCK_ACTOR: return -1;
    default:
        for(unsigned i=0;i<g->cell_count;++i) drawn|=nm_clock_cell(in->bitmaps,g->near,g->cell[i]);
    }
    return drawn ? (int)((in->counter>>g->shift)&(g->frames-1u)) : -1;
}

/* A table of group g of the file at `data`. */
static inline const void *nm_clock_table(const void *data,const nm_clock_group_t *g,unsigned table) {
    return (const uint8_t *)data+g->offset[table];
}
static inline const nm_clock_group_t *nm_clock_groups(const void *data) {
    return (const nm_clock_group_t *)(const void *)((const uint8_t *)data+sizeof(nm_clock_header_t));
}
/* The actors of the file (NULL and 0 without). */
static inline unsigned nm_clock_actor_count(const void *data) {
    const nm_clock_header_t *h=(const nm_clock_header_t *)data;
    return h->flags&NM_CLOCK_HAS_ACTORS ? h->reserved[0] : 0u;
}
static inline const nm_clock_actor_t *nm_clock_actors(const void *data) {
    const nm_clock_header_t *h=(const nm_clock_header_t *)data;
    return h->flags&NM_CLOCK_HAS_ACTORS ? (const nm_clock_actor_t *)(const void *)((const uint8_t *)data+h->reserved[1]) : NULL;
}
/* Stored frames of the positions and normals. */
static inline unsigned nm_clock_stored(const nm_clock_group_t *g) { return g->rate ? g->frames/g->rate : 0u; }

/* The file's structure: every table inside the file, 4-byte aligned, counts that fit, frames a power of two, each
 * group's mesh one of the `meshes` meshes with the group's vertex and face counts (NULL: not checked). 1 valid. */
static inline int nm_clock_validate(const void *data,size_t size,const nm_mesh_t *mesh_table,unsigned meshes) {
    if(!data || size<sizeof(nm_clock_header_t) || ((uintptr_t)data&3u)) return 0;
    const nm_clock_header_t *h=(const nm_clock_header_t *)data;
    if(h->magic!=NM_CLOCK_MAGIC || h->size!=size || !h->groups || h->groups>NM_CLOCK_GROUPS_MAX ||
       (h->flags&~NM_CLOCK_HAS_ACTORS) || sizeof(*h)+h->groups*sizeof(nm_clock_group_t)>size) return 0;
    for(unsigned i=h->flags ? 2u : 0u;i<4;++i) if(h->reserved[i]) return 0;
    if(h->flags) {
        if(!h->reserved[0] || h->reserved[0]>NM_CLOCK_ACTORS_MAX || (h->reserved[1]&3u) ||
           h->reserved[1]<sizeof(*h)+h->groups*sizeof(nm_clock_group_t) || h->reserved[1]>size ||
           (size_t)h->reserved[0]*sizeof(nm_clock_actor_t)>size-h->reserved[1]) return 0;
        uint8_t owned[NM_CLOCK_GROUPS_MAX]={0};
        for(unsigned a=0;a<h->reserved[0];++a) {
            const nm_clock_actor_t *actor=nm_clock_actors(data)+a;
            if(!actor->kind || actor->kind>=NM_CLOCK_KINDS || actor->groups!=nm_clock_kind_groups(actor->kind) ||
               actor->first_group+actor->groups>h->groups || actor->cell>255) return 0;
            for(unsigned k=0;k<3;++k) if(!(actor->position[k]>-1e6f && actor->position[k]<1e6f)) return 0;
            for(unsigned k=0;k<6;++k) if(!(actor->param[k]>-1e6f && actor->param[k]<1e6f)) return 0;
            for(unsigned g=actor->first_group;g<actor->first_group+actor->groups;++g) {
                if(owned[g] || nm_clock_groups(data)[g].cond!=NM_CLOCK_ACTOR) return 0;
                owned[g]=1;
            }
        }
        for(unsigned g=0;g<h->groups;++g) if((nm_clock_groups(data)[g].cond==NM_CLOCK_ACTOR)!=owned[g]) return 0;
    }
    for(unsigned i=0;i<h->groups;++i) {
        const nm_clock_group_t *g=nm_clock_groups(data)+i;
        if(g->mesh>=meshes || g->cond>NM_CLOCK_ACTOR || g->near>1 || g->shift>8 || !g->cell_count ||
           g->cell_count>4 || !g->frames || g->frames>256 || (g->frames&(g->frames-1u)) ||
           (g->frames==1u && g->cond!=NM_CLOCK_WINDOW && g->cond!=NM_CLOCK_ACTOR) ||
           (g->rate && (g->rate>g->frames || (g->frames%g->rate) || (g->rate&(g->rate-1u)))) ||
           !g->vertices || !g->faces || !g->states || g->state_faces>g->faces || g->normal_faces>g->faces ||
           g->uv_vertices>g->vertices || (g->uv_vertices && !g->uv_sets) || (g->flags&~NM_CLOCK_GROUP_PLACED) ||
           ((g->flags&NM_CLOCK_GROUP_PLACED) && (g->cond!=NM_CLOCK_ACTOR || !g->rate || g->normal_faces!=g->faces)))
            return 0;
        if(mesh_table && (mesh_table[g->mesh].vertex_count!=g->vertices || mesh_table[g->mesh].face_count!=g->faces)) return 0;
        for(unsigned a=0;a<3;++a)
            if(!(g->step[a]>0 && g->step[a]<1.0f) || !(g->origin[a]>-1e6f && g->origin[a]<1e6f)) return 0;
        const unsigned stored=nm_clock_stored(g);
        const size_t bytes[NM_CLOCK_TABLES]={
            2u*g->vertices, 6u*(size_t)g->points*stored, sizeof(nm_clock_state_t)*g->states, 2u*g->state_faces,
            2u*(size_t)g->state_faces*g->frames, 2u*g->normal_faces, 8u*(size_t)g->normal_faces*stored,
            2u*g->uv_vertices, 4u*(size_t)g->uv_vertices*g->uv_sets, g->uv_vertices ? 2u*g->frames : 0u};
        if(g->normal_faces && !g->rate) return 0;
        for(unsigned t=0;t<NM_CLOCK_TABLES;++t)
            if((g->offset[t]&3u) || g->offset[t]<sizeof(*h)+h->groups*sizeof(nm_clock_group_t) ||
               g->offset[t]>size || bytes[t]>size-g->offset[t]) return 0;
        const uint16_t *vertex_point=(const uint16_t *)nm_clock_table(data,g,NM_CLOCK_VERTEX_POINT);
        for(unsigned v=0;v<g->vertices;++v)
            if(vertex_point[v]!=NM_CLOCK_STILL && (vertex_point[v]>=g->points || !g->rate)) return 0;
        const uint16_t *faces[2]={(const uint16_t *)nm_clock_table(data,g,NM_CLOCK_STATE_FACES),(const uint16_t *)nm_clock_table(data,g,NM_CLOCK_NORMAL_FACES)};
        const unsigned face_counts[2]={g->state_faces,g->normal_faces};
        for(unsigned k=0;k<2;++k) for(unsigned f=0;f<face_counts[k];++f) if(faces[k][f]>=g->faces) return 0;
        const uint16_t *states=(const uint16_t *)nm_clock_table(data,g,NM_CLOCK_FACE_STATES);
        for(size_t s=0;s<(size_t)g->state_faces*g->frames;++s) if(states[s]>=g->states) return 0;
        const uint16_t *uv_vertices=(const uint16_t *)nm_clock_table(data,g,NM_CLOCK_UV_VERTICES),*uv_frames=(const uint16_t *)nm_clock_table(data,g,NM_CLOCK_UV_FRAMES);
        for(unsigned v=0;v<g->uv_vertices;++v) if(uv_vertices[v]>=g->vertices) return 0;
        for(unsigned k=0;g->uv_vertices && k<g->frames;++k) if(uv_frames[k]>=g->uv_sets) return 0;
        const nm_clock_state_t *table=(const nm_clock_state_t *)nm_clock_table(data,g,NM_CLOCK_STATES);
        for(unsigned s=0;s<g->states;++s) if(table[s].reserved) return 0;
    }
    return 1;
}

/* A moving point of group g between its stored frames pa and pb at weight t of pb (renderer math: SH4ZAM's on the
 * console; host tools the plain expression). */
static inline void nm_clock_point(const nm_clock_group_t *g,const int16_t *pa,const int16_t *pb,float t,unsigned p,float out[3]) {
    for(unsigned axis=0;axis<3;++axis) {
        const float qa=(float)pa[3u*p+axis],qb=(float)pb[3u*p+axis];
#ifdef __DREAMCAST__
        out[axis]=shz_fmaf(shz_lerpf(qa,qb,t),g->step[axis],g->origin[axis]);
#else
        out[axis]=(qa+(qb-qa)*t)*g->step[axis]+g->origin[axis];
#endif
    }
}
/* The same on a stored frame (rate 1: every frame is stored, weight 0): the lerp's result without its second read. */
static inline void nm_clock_point_stored(const nm_clock_group_t *g,const int16_t *pa,unsigned p,float out[3]) {
    for(unsigned axis=0;axis<3;++axis) {
#ifdef __DREAMCAST__
        out[axis]=shz_fmaf((float)pa[3u*p+axis],g->step[axis],g->origin[axis]);
#else
        out[axis]=(float)pa[3u*p+axis]*g->step[axis]+g->origin[axis];
#endif
    }
}
/* A stored normal component between two stored frames. */
static inline float nm_clock_normal(int16_t a,int16_t b,float t) {
#ifdef __DREAMCAST__
    return shz_lerpf((float)a,(float)b,t)*(1.0f/NM_CLOCK_NORMAL_SCALE);
#else
    return ((float)a+((float)b-(float)a)*t)*(1.0f/NM_CLOCK_NORMAL_SCALE);
#endif
}
/* The same on a stored frame. */
static inline float nm_clock_normal_stored(int16_t a) { return (float)a*(1.0f/NM_CLOCK_NORMAL_SCALE); }

/* Where frame `frame` lies between the stored frames: a, b and the weight t of b. */
static inline void nm_clock_between(const nm_clock_group_t *g,unsigned frame,unsigned *a,unsigned *b,float *t) {
    const unsigned stored=nm_clock_stored(g);
    *a=frame/g->rate; *b=(*a+1u)%stored;
    *t=(float)(frame%g->rate)/(float)g->rate;
}
#endif
