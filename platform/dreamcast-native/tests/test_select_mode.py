"""Select mode (code/gameplay/select_mode.c): the pad mapping, the tick loop and the hand-offs.

Compiles the mode's body with the real select logic (include/select.h) and stubs for the scene, the renderer and
the sound, then plays it: the game start takes two iterations (phases 12, 17), stick and D-pad steps move the
choice (the wheel fed is 0/64/127 and 0/127), START, A and the right trigger confirm with one accelerator edge
each, a confirm outside the choosing states is dropped, the race entry is handed the chosen course and
transmission at the select's last iteration, and the game start's scene and the counter reach their owners (once).
The attract's buttons open the settings (both triggers and START) or give the credit. A course without data in the
build races course 0 (NATIVE_COURSES 7u since Expert: every course carries its own; the harness builds with 5u to
cover that path)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
code = (root / 'code/gameplay/select_mode.c').read_text()
body = '#define NATIVE_COURSES 5u\n' + code[code.index('#ifndef NATIVE_COURSES'):]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "select.h"
#define ENJ_BUTTON_DOWN_THIS_FRAME 3
typedef struct { unsigned START,A,LEFT,RIGHT; } buttons_t;
typedef struct { buttons_t button; int joyx; uint8_t ltrigger,rtrigger; } enj_ctrlr_state_t;
typedef struct { void (*mode_updater)(void *); const char *name; } enj_mode_t;
typedef struct { struct { int shut_down; } flags; } enj_state_t;
static enj_state_t state;
static enj_state_t *enj_state_get(void) { return &state; }
static enj_ctrlr_state_t controller;
static enj_ctrlr_state_t *controllers[1]={&controller};
static enj_ctrlr_state_t **enj_ctrl_get_states(void) { return controllers; }
static size_t enj_ctrl_states_length(void) { return 1; }
static uint64_t now_us;
static uint64_t timer_ns_gettime64(void) { return now_us*1000; }
static enj_mode_t *active;
static void enj_mode_set(enj_mode_t *mode) { active=mode; }
static unsigned shutdowns;
static void enj_state_flag_shutdown(void *unused) { (void)unused;++shutdowns; }
#define NM_PRACTICE_TICK_US 17384u
/* frame_clock.h: the console counts one tick per displayed frame; here the ticks of the stub clock. */
typedef struct { uint64_t last_us; } nm_frame_clock_t;
static uint32_t nm_frame_ticks(nm_frame_clock_t *clock) {
    const uint64_t now=now_us;
    if(!clock->last_us) { clock->last_us=now;return 0; }
    const uint32_t ticks=(uint32_t)((now-clock->last_us)/NM_PRACTICE_TICK_US);
    clock->last_us+=(uint64_t)ticks*NM_PRACTICE_TICK_US;return ticks;
}
#define NM_MUSIC_GAME_START 0xae1001u
static unsigned sounds,game_starts;
static void nm_sound_update(void) {}
static void nm_sound_command(uint32_t c) { ++sounds;game_starts+=c==NM_MUSIC_GAME_START; }
static void nm_sound_settle(void) {}
static const char *nm_practice_root(void) { return "/pc/"; }
static unsigned render_loads,render_releases,enqueued;
static int nm_select_render_load(const char *r) { (void)r;++render_loads;return 1; }
static int nm_select_render_release(void) { ++render_releases;return 1; }
static int nm_select_render_enqueue(const nm_select_t *s,uint32_t c) { (void)s;(void)c;++enqueued;return 1; }
/* The scene side, stubbed. */
enum { NM_CREDIT_TITLE=0, NM_CREDIT_CINEMATIC=1, NM_CREDIT_DEMO=2 };
static unsigned game_start_calls,render_releases_scene,settings_starts;
static int game_start_scene=-1;static uint32_t game_start_counter;
static int nm_practice_game_start(int scene,uint32_t c) { ++game_start_calls;game_start_scene=scene;game_start_counter=c;return 1; }
static void nm_practice_render_release(void) { ++render_releases_scene; }
static int nm_settings_mode_start(void) { ++settings_starts;return 1; }
static unsigned race_course=99,race_transmission=99,races;
unsigned nm_select_course_raced(unsigned chosen);
static int nm_practice_race_from_select(unsigned course,unsigned transmission) {
    race_course=nm_select_course_raced(course);race_transmission=transmission;++races;return 1;
}
'''
checks = r'''
static unsigned frame_ticks(unsigned ticks) { /* one updater call per tick */
    unsigned before=races;
    for(unsigned i=0;i<ticks && races==before;++i) { now_us+=NM_PRACTICE_TICK_US;active->mode_updater(NULL); }
    return races!=before;
}
static void press(unsigned *button) { *button=ENJ_BUTTON_DOWN_THIS_FRAME;frame_ticks(1);*button=0; }
int main(void) {
    /* The attract's buttons: both triggers and START open the settings, START alone or A is a credit. */
    assert(nm_select_attract_button(1,200,255)==1 && settings_starts==1 && render_releases_scene==1);
    assert(nm_select_attract_button(1,200,0)==0 && nm_select_attract_button(0,255,255)==0 && settings_starts==1);
    /* Attract credit at 5010b0 1109 (the race_to_end run): phases 12 and 17, then the select. */
    assert(nm_select_credit(1109) && render_releases_scene==2 && game_start_calls==1 && render_loads==1 && game_starts==1 && counter==1111);
    assert(game_start_scene==NM_CREDIT_CINEMATIC && game_start_counter==1109);
    assert(select_state.phase==18 && select_state.state==0 && select_state.phase_timer==1599);
    now_us=1;active->mode_updater(NULL); /* first call: no time yet */
    frame_ticks(5);assert(select_state.state==1 && select_state.slot==0);
    /* Stick right twice (re-armed by the centre), D-pad left once: slot 1 (Advanced, course 2). */
    controller.joyx=100;frame_ticks(1);frame_ticks(1);assert(wanted==1);
    controller.joyx=0;frame_ticks(1);controller.joyx=127;frame_ticks(1);assert(wanted==2);controller.joyx=0;
    press(&controller.button.LEFT);assert(wanted==1);
    frame_ticks(2);assert(select_state.slot==1 && select_state.course==2);
    /* A confirms: one accelerator edge, the course select ends. */
    press(&controller.button.A);assert(select_state.state==2);
    /* A confirm during the blink is dropped: the transmission select starts at slot 0 and waits. */
    press(&controller.button.START);
    for(unsigned i=0;i<200 && select_state.state!=9;++i) frame_ticks(1);
    assert(select_state.state==9 && select_state.slot==0 && select_state.timer==640);
    /* Right: manual. The right trigger confirms within the lockout (ignored), then again after it. */
    press(&controller.button.RIGHT);frame_ticks(1);assert(select_state.slot==1 && select_state.transmission==1);
    controller.rtrigger=255;frame_ticks(1);controller.rtrigger=0;frame_ticks(1);assert(select_state.state==9);
    frame_ticks(20);controller.rtrigger=255;frame_ticks(1);controller.rtrigger=0;assert(select_state.state==10);
    /* To the race entry: course 2 (NATIVE_COURSES 5u: Beginner and Advanced) is raced; manual kept. */
    assert(frame_ticks(1000) && races==1 && render_releases==1 && race_course==2 && race_transmission==1 && !shutdowns);
    /* A course without data (Expert, 1) races course 0. */
    assert(nm_select_course_raced(1)==0 && nm_select_course_raced(0)==0 && nm_select_course_raced(2)==2);
    uint32_t base=0;
    assert(nm_select_race_state(&base) && base==counter+2);
    /* Consumed: a later race (title restart) gets nothing. */
    assert(!nm_select_race_state(&base));
    /* From the title: its game start (the pool stands), timeouts: Beginner, automatic, the race's base from the counter. */
    assert(nm_select_mode_start(NM_CREDIT_TITLE,500) && game_start_calls==2 && game_start_scene==NM_CREDIT_TITLE && counter==502);
    now_us+=1;active->mode_updater(NULL);
    assert(frame_ticks(2000) && races==2 && race_course==0 && race_transmission==0);
    assert(nm_select_race_state(&base) && base==counter+2);
    /* From the V.R. demo: the game start on its cars, with the credit's counter. */
    assert(nm_select_mode_start(NM_CREDIT_DEMO,700) && game_start_calls==3 && game_start_scene==NM_CREDIT_DEMO &&
        game_start_counter==700 && counter==702);
    printf("PASS: select mode: settings chord and credit from the attract, game start (2 iterations), stick/D-pad "
        "steps, A/START/trigger confirms with the original's lockout, dropped stray confirm, Advanced raced, Expert "
        "(no data) races course 0, the game start's scene to the pool's owner, counter to the race entry (consumed once)\n");
}
'''
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'check.c'
    source.write_text(harness + body + checks)
    binary = Path(directory) / 'check'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I', str(root / 'include'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
