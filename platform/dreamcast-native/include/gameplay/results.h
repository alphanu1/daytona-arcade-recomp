#ifndef DAYTONA_GAMEPLAY_RESULTS_H
#define DAYTONA_GAMEPLAY_RESULTS_H
#include "result_actor.h"
#include "race_intents.h"

typedef struct {
    nm_result_playback_t playback;
    unsigned mode,ticks;
    uint32_t timer,penalty;
    int16_t scroll;
    uint32_t announcement_cursor,commands,command_hash;
    unsigned show_labels;
    uint8_t after_podium; /* Results entered from the podium (27/28): the pool holds no race field. */
    uint8_t start_held;   /* 53e120 bit 4 (the pad's START, code/practice.c): the result actor's skip, b680. */
    uint32_t podium_ticks; /* Iterations of phases 27/28, for 5010b0 at the game's end. */
} nm_results_t;

struct nm_practice_scene;
/* Returns 1 if the result phase handled this updater, including shutdown.
 * Returns 0 while the race still owns it. Audio events only update a digest. */
int nm_results_update(struct nm_practice_scene *scene,uint32_t elapsed_us);
/* The end of one results tick (code/practice.c, which owns the event tables and the
 * checkpoint rows): the race field (race_scene.h nm_race_scene_results_field, then the
 * event tail 19038, sound commands in out) unless after_podium, then the checkpoint.
 * Returns 0 on a rejected step. */
int nm_practice_results_tick(struct nm_practice_scene *scene,unsigned results_phase,
    unsigned collision_phase,nm_race_intent_output_t *out);
#endif
