#ifndef DAYTONA_NATIVE_FRONTEND_POOL_H
#define DAYTONA_NATIVE_FRONTEND_POOL_H
#include "collision.h"
#include "player.h"
#include "visibility.h"
#include "nm_mem.h"

/* The part of the original's car pool (504e00 + id*0x300, 44 entities) one scene leaves the next, and what goes with
 * it (include/frontend.h has the rules that fill and apply it). The original has one pool, never cleared: each
 * scene's exit stores its cars here and leaves the other entities as they were, so this lives from power-on. */
typedef struct {
    nm_collision_travel_t motion[44]; /* +28c/+290 previous, +294/+298 travel */
    uint32_t relative[44];            /* +2a4 */
    uint8_t side[44];                 /* +27d */
    uint8_t remembered[44];           /* +244, the neighbour 136e8 keeps avoiding (0: none; here its id, 255: none) */
    int16_t rank_key[44];             /* +60 (the grid reset e3f4 keeps it; the race start rewrites it) */
    uint8_t rank[44];                 /* +6a, a race's: the opponents' first step reads it (10e10), the sort pass after */
    uint16_t rank_order[44], track_order[44]; /* 1224c's lists (the race start rebuilds them, the demo start does not) */
    /* Of an entity the next race does not grid: what 136e8 reads of it through +244 (+1c, +24, +13e). */
    float position[44][2];
    int16_t lateral[44];
    /* Entity 0's +b0, a 32-bit word the race's player filters from on its first tick where no rolling start rewrites
     * it (e93c): the race's whole word, then the cinematic's low half (caf4). */
    int32_t steer;
    int8_t leading_lap;               /* 5014ba */
    uint8_t view;                     /* 501710: the last demo's or race's, into the next demo's camera reset (637c) */
    uint8_t crash_counter;            /* 503680, the crash orientation 15e54 steps: every scene's, never reset */
    uint8_t raced;                    /* player holds a race's car */
    uint32_t clock;                   /* 5010cc: the race clock stays through the attract; its spin rule (15ae4) reads it */
    /* The visible cells behind the bitmap 501520 as the last scene's scenery object 5890 left them:
     * the race's first detail pass reads them (nm_frontend_race_guided). */
    nm_scenery_cells_t cells;
    /* Entity 0 as the last race left it: the race entry's resets keep what they do not write (nm_player_carried,
     * race_scene.h); the cinematic's and the demo's player updaters write none of that (m2segment, the words the
     * race reads before writing them: docs/NEXT_LLM_HANDOFF.md, 30 October). */
    nm_player_t player;
} nm_entity_pool_t;

/* Power-on. The cinematic's leading lap starts from -1 (its laps count from 0 where the original's spawns write 255). */
static inline void nm_frontend_pool_reset(nm_entity_pool_t *pool) {
    nm_memset(pool,0,sizeof(*pool));
    nm_memset(pool->remembered,255,sizeof(pool->remembered));
    pool->leading_lap=-1;
}

#endif
