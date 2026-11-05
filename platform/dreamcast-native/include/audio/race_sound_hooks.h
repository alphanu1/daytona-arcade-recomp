/* The race's sound commands (include/race_sound.h) as the game sends them: one call per hook,
 * in the tick's order, from code/practice.c, gameplay/results.c and gameplay/title_mode.c.
 *
 * code/audio/race_sound.c and code/audio/sfx.c are compiled without LTO (Makefile). Under
 * NATIVE_LTO=one GCC decides its inlining for the whole program at once, with budgets relative
 * to the program's size: sound code inside the unit changed what it inlined in the renderer and
 * moved every hot function to other I-cache lines (registration 7.54 -> 7.78 ms a frame on
 * hardware, 17 October 2026). Outside the unit, the game's code is the baseline's plus these
 * calls. Presentation only: nothing here writes gameplay state. */
#ifndef NM_AUDIO_RACE_SOUND_HOOKS_H
#define NM_AUDIO_RACE_SOUND_HOOKS_H
#include <stdint.h>
#include "race_scene.h"

/* The course's lap awards (the road pack's), for the announcer's prefetch hints (once per race session). */
void nm_race_sound_hook_course(const nm_road_awards_t *awards);
/* Counters to zero for a new session; boot: also 18f30's last values (once per power-on). */
void nm_race_sound_hook_reset(int boot);
/* Before the tick: ab78's start voice (it reads the stage and the profile before the tick). */
uint32_t nm_race_sound_hook_before(const nm_race_scene_t *race,unsigned profile_count);
/* After the tick: that command first, the phase handler's time-out beep (364c), then the player's lap
 * award (row 12), the spin (158c8), the player's crash and impact sounds (race_sound.h). */
void nm_race_sound_hook_tick(const nm_race_scene_t *race,uint32_t countdown);
/* After the event tail's commands (19038, row 56): its last call 19590, the ambient source's levels
 * be35/be34. frame_counter: 5010b0 (its low two bits: race ticks - 1; in the results, race ticks + results
 * ticks - 1), which the results phases keep stepping with the gate shut. */
void nm_race_sound_hook_events(const nm_race_scene_t *race,uint32_t frame_counter);
/* Finish entry 44c8/44cc: engine and slip off. */
void nm_race_sound_hook_finish(void);
/* The player's draw (9088), after the effect timers and before the 9148 contact clear: the front smoke
 * be37 (90f0), the rear tyre skid (9888), and 9148's write of the intent lane 501a04 (the wall rub;
 * gameplay state, as the original's). */
void nm_race_sound_hook_draw(nm_race_scene_t *race);
/* HUD object a7c0: the engine (19c58) and the slip (19d1c). */
void nm_race_sound_hook_hud(const nm_race_scene_t *race);
/* A command the original queues through 19bdc: played only while the gate is open. */
void nm_race_sound_hook_gated(const nm_race_scene_t *race,uint32_t command);
/* Results (b260/b264): engine and slip off, counted in the results' own counters. */
void nm_race_sound_hook_results_off(uint32_t *commands,uint32_t *hash);
/* Results: an event-tail command through 19bdc (gated off in the results phases), counted there. */
void nm_race_sound_hook_results_gated(const nm_race_scene_t *race,uint32_t *commands,uint32_t *hash,
                                      uint32_t command);
/* The title's entry (1cd4/1cd8): engine and slip off. */
void nm_race_sound_hook_engine_off(void);
/* "Native race sound" line at exit. */
void nm_race_sound_hook_report(void);

#endif
