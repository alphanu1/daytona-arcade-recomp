"""Title mode (the original's phase 9) and the attract loop's entry (phase 2): queue title frames for 386 ticks,
then phase 2; START or A is a credit (the select); both triggers and START the settings."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
code=(root/'code/gameplay/title_mode.c').read_text()
body=code[code.index('enum { TITLE_TICKS'):]
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define printf(...) ((void)0)
#define ENJ_BUTTON_DOWN_THIS_FRAME 3
typedef struct { unsigned START,A; } buttons_t;
typedef struct { buttons_t button; uint8_t ltrigger,rtrigger; } enj_ctrlr_state_t;
typedef struct { void (*mode_updater)(void *);void *data;void *activation;unsigned flags;char name[32]; } enj_mode_t;
static enj_ctrlr_state_t controller;
static enj_ctrlr_state_t *controllers[1]={&controller};
static unsigned loads,releases,restarts,attracts,queued,shutdowns,sound_updates;
static void nm_sound_update(void) { ++sound_updates; } /* audio/sound.h: every updater call */
#define NM_MUSIC_ADVERTISE 0xae1008u /* audio/music_select.h */
#define NM_MUSIC_FADE 0xbe1a10u
static unsigned adverts,fades,settles,sound_calls,engine_offs;
static void nm_sound_command(uint32_t command) { ++sound_calls; adverts+=command==NM_MUSIC_ADVERTISE; fades+=command==NM_MUSIC_FADE; }
/* game_save.h: the live settings (ADVERTISE SOUND, include/settings.h) */
#define NM_ADVERTISE_ON 0
#define NM_ADVERTISE_OFF 1
typedef struct { uint8_t difficulty,advertise_sound; } nm_settings_t;
static nm_settings_t settings;
static const nm_settings_t *nm_game_settings(void) { return &settings; }
/* audio/race_sound_hooks.h: be1700, be1900 */
static void nm_race_sound_hook_engine_off(void) { ++engine_offs;nm_sound_command(0xbe1700u);nm_sound_command(0xbe1900u); }
static void nm_sound_settle(void) { assert(sound_calls); ++settles; }
static int sound_held; /* audio/sound.h nm_sound_hold: the attract's music held over its scene start */
static void nm_sound_hold(int on) { sound_held=on; }
static uint64_t now_us;
static enj_ctrlr_state_t **enj_ctrl_get_states(void) { return controllers; }
static size_t enj_ctrl_states_length(void) { return 1; }
static uint64_t timer_ns_gettime64(void) { return now_us*1000; }
static uint64_t title_tick(uint64_t elapsed) { return elapsed/17384; }
static int nm_title_render_load(void) { ++loads;return 1; }
static int nm_title_render_release(void) { ++releases;return 1; }
static int nm_title_render_enqueue(uint64_t tick) { queued+=(unsigned)(tick+1);return 1; }
#define NM_PRACTICE_TICK_US 17384u
/* frame_clock.h: the console counts one tick per displayed frame; here the ticks of the stub clock. */
typedef struct { uint64_t last_us; } nm_frame_clock_t;
static uint32_t nm_frame_ticks(nm_frame_clock_t *clock) {
    const uint64_t now=now_us;
    if(!clock->last_us) { clock->last_us=now;return 0; }
    const uint32_t ticks=(uint32_t)((now-clock->last_us)/NM_PRACTICE_TICK_US);
    clock->last_us+=(uint64_t)ticks*NM_PRACTICE_TICK_US;return ticks;
}
static uint32_t counter=10;
static uint32_t nm_frontend_counter(void) { return counter; }
static void nm_frontend_counter_set(uint32_t value) { counter=value; }
static uint32_t select_counter;
int nm_attract_mode_start(void);
static int nm_select_mode_start(void *attract,uint32_t value) { assert(!attract);select_counter=value;++restarts;return 1; }
static int nm_practice_attract_start(void) { ++attracts;return 1; }
/* practice.h: the next attract scene loads in the title's frames */
static unsigned preloads,settles_scene;static int loading;
static int nm_practice_attract_preload(void) { ++preloads;return 1; }
static int nm_practice_attract_loading(void) { return loading; }
static void nm_practice_attract_settle(void) { ++settles_scene;loading=0; }
static void nm_practice_attract_abandon(void) { nm_practice_attract_settle(); }
static unsigned settings_starts;
static int nm_settings_mode_start(void) { ++settings_starts;return 1; } /* gameplay/settings_mode.h */
static enj_mode_t *active;
static void enj_mode_set(enj_mode_t *mode) { active=mode; }
static void enj_state_flag_shutdown(void *unused) { (void)unused;++shutdowns; }
'''
checks=r'''
static void tick_frames(unsigned n) { for(unsigned i=0;i<n;++i) { now_us+=17384;active->mode_updater(NULL); } }
int main(void) {
    /* Phase 2: advertisement music (or the fade), engine off, then the cinematic. */
    assert(nm_attract_mode_start() && attracts==1 && adverts==1 && settles==1 && engine_offs==1 && counter==11);
    assert(nm_title_mode_start());assert(loads==1 && active && settles==2 && adverts==1 && engine_offs==2); /* phase 8 */
    now_us=1000;active->mode_updater(NULL);assert(queued==1 && !restarts);
    tick_frames(1);assert(queued==3);
    controller.button.START=ENJ_BUTTON_DOWN_THIS_FRAME;active->mode_updater(NULL);
    assert(releases==1 && restarts==1 && !shutdowns && select_counter==11+1+1);
    controller.button.START=0;controller.button.A=ENJ_BUTTON_DOWN_THIS_FRAME;
    assert(nm_title_mode_start());now_us+=17384;active->mode_updater(NULL);assert(releases==2 && restarts==2 && !shutdowns);
    controller.button.A=0;
    /* 386 ticks, then phase 10 and phase 2: the counter counts them. */
    assert(nm_title_mode_start());now_us=5;active->mode_updater(NULL);
    tick_frames(385);assert(attracts==1);
    tick_frames(1);assert(attracts==2 && releases==3 && counter==11+386+1+1 && adverts==2);
    /* Still loading at the last tick: the title goes on (no iteration counted), then phase 2. */
    assert(nm_title_mode_start());now_us=5;active->mode_updater(NULL);loading=1;
    tick_frames(386);assert(attracts==2 && releases==3);
    const unsigned before=queued;tick_frames(5);assert(attracts==2 && queued>before+5*387 && counter==11+386+1+1);
    loading=0;tick_frames(1);assert(attracts==3 && releases==4 && counter==11+2*(386+1+1) && adverts==3);
    /* A credit while it loads: the rest of the load after the title's release, then the select. */
    assert(nm_title_mode_start());now_us=5;active->mode_updater(NULL);loading=1;
    controller.button.START=ENJ_BUTTON_DOWN_THIS_FRAME;active->mode_updater(NULL);controller.button.START=0;
    assert(settles_scene==3 && !loading && restarts==3 && releases==5);
    assert(preloads==5);
    settings.advertise_sound=NM_ADVERTISE_OFF;assert(nm_attract_mode_start() && adverts==3 && fades==1);
    /* Both triggers and START: the settings screen, not the select; one trigger is a credit. */
    assert(nm_title_mode_start());now_us=1;active->mode_updater(NULL);
    controller.button.START=ENJ_BUTTON_DOWN_THIS_FRAME;controller.ltrigger=255;controller.rtrigger=200;
    active->mode_updater(NULL);assert(settings_starts==1 && restarts==3);
    assert(nm_title_mode_start());controller.rtrigger=0;active->mode_updater(NULL);assert(settings_starts==1 && restarts==4);
    controller.button.START=0;controller.ltrigger=0;
    puts("PASS: title (phase 9) 386 ticks then phase 2 with its music; START/A start the select; triggers+START open the settings; the attract's load in its frames, the title on while it loads");
}
'''
with tempfile.TemporaryDirectory() as directory:
    source=Path(directory)/'check.c';source.write_text(harness+body+checks)
    binary=Path(directory)/'check'
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
