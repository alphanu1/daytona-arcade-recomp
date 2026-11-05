/* The inputs of the frame-counter animations (include/clock_actors.h) from the scene the frame draws: the race
 * (nm_race_scene_t after its tick) or a cinematic scene (the podium, the attract). Shared by
 * render/scene/practice.c and tests/trace_race_native.c (--clock).
 *   counter 5010b0: in the race counter_base + ticks - 1 (race_scene.h), plus the ticks after the race's last one
 *     that the race scene still draws (`after`: the results ticks of a race that ends there, gameplay/results.c);
 *     on the podium's entry tick no actor draws (the original's draws: none; the callers skip it); in the podium the
 *     race's counter_base +
 *     object_clock + ticks (the SCREEN kind's value, practice_object_variants, with the base the race started
 *     from); in the attract object_clock + ticks
 *   flags: the latched 501090 of the scene (race: shared.flags; cinematic: its 1<<3 or 1<<28)
 *   profile, laps: the player entity's +128 and +6d; in the podium that entity still counts its passes of the
 *     line (practice_board, render/scene/practice.c); a cinematic scene's profile is its car 0's */
#ifndef DAYTONA_NATIVE_CLOCK_SCENE_H
#define DAYTONA_NATIVE_CLOCK_SCENE_H
#include "clock_actors.h"
#include "race_scene.h"
#include "intro_scene.h"

/* 0: the race scene has no cars (nothing to draw from). */
static inline int nm_clock_scene_input(nm_clock_input_t *in,const nm_race_scene_t *race,const nm_intro_scene_t *intro,
        int attract_active,uint32_t after,const uint32_t bitmaps[16]) {
    const int podium=attract_active && (intro->flags&(UINT32_C(1)<<28));
    const nm_player_t *player=race->count ? &race->cars[0].car : NULL;
    in->bitmaps=bitmaps;
    if(!attract_active) {
        if(!player) return 0;
        in->counter=race->counter_base+race->ticks-1u+after; in->flags=race->shared.flags;
        in->laps=player->lap.completed;
    } else {
        in->counter=(podium ? race->counter_base : 0u)+intro->object_clock+intro->ticks;
        in->flags=intro->flags;
        const unsigned passes=podium ? intro->cars[0].lap.lap : 0u;
        in->laps=player ? (passes ? (uint8_t)(player->lap.lap+passes) : player->lap.completed) : 0u;
    }
    /* 501220 of a cinematic scene is its own car 0 (the podium's: the race's player entity, driven by the podium). */
    in->profile=attract_active ? (uint16_t)intro->cars[0].phase[0] : player ? (uint16_t)player->phase[0] : 0u;
    return 1;
}
#endif
