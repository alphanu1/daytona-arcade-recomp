#ifndef DAYTONA_NATIVE_FRAME_TRACE_H
#define DAYTONA_NATIVE_FRAME_TRACE_H
/* NATIVE_DIAG_FRAME_TRACE=1 (measurement build, needs NATIVE_DIAG_DROP_LOG=1; local.cfg.mk): every displayed frame's
 * updater, wait, registration, render and registration end after the previous flip ("FT-RUN" lines), and for the
 * frames of the run ticks NATIVE_DIAG_FRAME_TRACE_LO..HI what those were spent on ("FT-WIN" lines): the updater's
 * scopes, each list's time, TA bytes, triangles, headers and vertices at its end, the model renderer's groups,
 * the paths its batches took. tools/frame_trace_report.py reads them.
 * The clock is TMU2's counter (one uncached register read, 80 ns a count, down from a reload a second), not
 * timer_ns_gettime64, whose cost would add up over a frame's objects. */
#ifdef NM_DIAG_FRAME_TRACE
#include <stdint.h>
enum { NM_FT_WORLD_PREP,NM_FT_PLAYER_PREP,NM_FT_OPP_PREP,NM_FT_WORLD_OP,NM_FT_WORLD_PT,NM_FT_PLAYER_OP,NM_FT_PLAYER_PT,
    NM_FT_OPP_OP,NM_FT_OPP_PT,NM_FT_TRANSLUCENT,
    /* inside the prepare steps: */
    NM_FT_P_CLASSIFY,NM_FT_P_MARKS,NM_FT_P_POINTS,NM_FT_P_WALK,NM_FT_P_WIDE,
    /* inside the emitters: the general (clipping) path */
    NM_FT_GENERAL,
    /* the updater's scene setup (practice_prepare) */
    NM_FT_S_PARTS,NM_FT_S_PLAYER,NM_FT_S_SHADOWS,NM_FT_S_SCENERY,NM_FT_S_CLOCKS,NM_FT_S_FRAME,NM_FT_S_LIGHT,
    NM_FT_GROUPS };
#define NM_FT_NAMES "wprep","pprep","oprep","wop","wpt","pop","ppt","oop","opt","tr", \
    "classify","marks","points","walk","wide","general","sparts","splayer","sshadows","sscenery","sclocks","sframe","slight"
#define NM_FT_MARK(group,start) do { const uint32_t ft_now=nm_ft_clock(); \
    nm_ft_group[group]+=nm_ft_delta(start,ft_now);++nm_ft_calls[group]; } while(0)
extern uint32_t nm_ft_group[NM_FT_GROUPS];  /* this scene: counts of the clock */
extern uint32_t nm_ft_calls[NM_FT_GROUPS];
extern uint32_t nm_ft_emit[2][4];           /* this scene, per source: SQ_PATH_EMIT's faces (kind 3: batches) */
extern uint32_t nm_ft_opponents;            /* opponents queued (opaque) */
enum { NM_FT_OBJECTS=32,NM_FT_PARTS=96,NM_FT_CARS=24 };
/* This scene, per world batch: mesh, its faces and vertices, clock counts, triangles on each list. */
typedef struct { uint16_t mesh,faces,vertices,tri_op,tri_pt,prep,op,pt; } nm_ft_object_t;
/* Per opponent: first instance, the arcade's level, the level drawn, rows tall at its origin, its work (counts). */
typedef struct { uint16_t first;uint8_t level,drawn,rows,pad;uint16_t prep,op,pt,tri; } nm_ft_car_t;
typedef struct { uint16_t prep,op,pt,tri; } nm_ft_part_t; /* per first instance of an opponent */
extern nm_ft_object_t nm_ft_objects[NM_FT_OBJECTS];
extern nm_ft_car_t nm_ft_cars[NM_FT_CARS];
extern nm_ft_part_t nm_ft_parts[NM_FT_PARTS];
extern uint32_t nm_ft_object_count,nm_ft_car_count;
static inline uint32_t nm_ft_clock(void) { return *(volatile uint32_t *)0xffd80024u; }
static inline uint32_t nm_ft_delta(uint32_t from,uint32_t to) {
    const uint32_t d=from-to;
    return (int32_t)d<0 ? d+*(volatile uint32_t *)0xffd80020u : d;
}
void nm_ft_render_counts(uint32_t out[3]);  /* model_renderer.c: triangles, headers, TA vertices so far */
#else
#define NM_FT_BEGIN(name) ((void)0)
#define NM_FT_END(group,name) ((void)0)
#endif
#ifdef NM_DIAG_FRAME_TRACE
#define NM_FT_BEGIN(name) const uint32_t name=nm_ft_clock()
#define NM_FT_END(group,name) NM_FT_MARK(group,name)
#endif
#endif
