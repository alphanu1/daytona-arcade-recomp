/* The props of the game's scenes (code/props_scene.c, include/props.h). */
#ifndef DAYTONA_NATIVE_PROPS_SCENE_H
#define DAYTONA_NATIVE_PROPS_SCENE_H
#include <stdint.h>
#include "race_scene.h"
#include "intro_scene.h"
#include "expert_actors.h"

/* The race's props (rows 75..86): after the pit crew (rows 68..73), before the slot machine (row 109),
 * once a race tick after nm_race_scene_step returned 1. Returns the command the sound manager hands
 * 19bdc this tick (0: none; 19bdc's own gate still applies). */
uint32_t nm_props_race_step(const nm_race_scene_t *race, const nm_drive_params_t *math, const nm_contact_pack_t *contact);
/* The cinematic scenes' props, after nm_intro_scene_detail (their sound is gated off). */
void nm_props_intro_step(const nm_intro_scene_t *intro, const nm_drive_params_t *math, const nm_contact_pack_t *contact);
/* "Native props" line at exit. */
void nm_props_report(void);
/* Expert's world actors (include/expert_actors.h), stepped with the props of each scene: the course's DNA1 file with
 * actors (kept by the caller while the course is loaded; NULL: none), and practice.c's count of the START edges its
 * race, podium and results ticks consumed (53e584 bit 4), read on each tick that steps the actors. */
void nm_props_world_actors(const void *dna, unsigned course);
/* The results of a race that ends there (code/practice.c nm_practice_results_tick; `after` its results ticks): the
 * race's Expert actors run on. */
void nm_props_results_step(const nm_race_scene_t *race, uint32_t after, unsigned results_phase,
                           const nm_drive_params_t *math, const nm_contact_pack_t *contact);
void nm_props_start_edges(const unsigned *edges);
/* The draws of the last tick of the race (0) or the cinematic scene (1), one per group of the file; NULL: none. */
const nm_xa_draw_t *nm_props_world_actor_draws(int cinematic);
#endif
