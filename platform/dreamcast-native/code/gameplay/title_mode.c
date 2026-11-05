#include "gameplay/title_mode.h"
#include "gameplay/settings_mode.h"
#include "gameplay/select_mode.h"
#include "render/ui/title_render.h"
#include "practice.h"
#include "audio/sound.h"
#include "audio/music_select.h"
#include "audio/race_sound_hooks.h"
#include "game_save.h"
#include <enDjinn/enj_enDjinn.h>
#include <stdio.h>

/* The title is the original's phase 9 (2338): 386 iterations after phase 8's one, then phase 10 (one)
 * returns to phase 2 (nm_attract_mode_start). START in it is a credit (1108 -> 2818): the select, the race
 * inheriting the pool the V.R. demo before left (NM_CREDIT_TITLE, 0: nothing moves it in phases 8 to 18).
 * The next attract scene (its course's packs: the original steps the course before the title) loads in the
 * title's frames (nm_practice_attract_preload); should it not be loaded at the title's last tick, the title goes on
 * until it is, adding no iteration (as the original's phase 2 holds its frame while it loads), then cuts to it. */
enum { TITLE_TICKS=386 };
/* Title frames past its last tick after which the attract's load is taken as stuck (or the host as far too slow): it then runs at once, with a line. */
enum { TITLE_HOLD_LIMIT=1800 };
static uint64_t last_us;
static nm_frame_clock_t frame_clock;
static uint32_t remainder_us,ticks,pose_ticks,held;
static uint32_t frames,late,longest_us; /* the title's frames while the attract loads in them (one line at its end) */

static void title_update(void *unused) {
    (void)unused;
    nm_sound_update();
    const uint64_t now=timer_ns_gettime64()/1000;
    uint64_t elapsed=last_us ? now-last_us : 0;last_us=now;
    ++frames;
    if(elapsed>longest_us) longest_us=(uint32_t)elapsed;
    late+=elapsed>25000u; /* a frame shown twice or more */
    uint32_t frame_ticks=nm_frame_ticks(&frame_clock); /* one tick per displayed frame (frame_clock.h) */
    if(frame_ticks>4) frame_ticks=4;
    remainder_us+=frame_ticks*NM_PRACTICE_TICK_US;
    enj_ctrlr_state_t **controllers=enj_ctrl_get_states();
    for(size_t i=0;i<enj_ctrl_states_length();++i) if(controllers[i]) {
        const enj_ctrlr_state_t *c=controllers[i];
        /* Both triggers held and START: the settings screen (gameplay/settings_mode.h), as the cabinet's TEST. */
        if(c->button.START==ENJ_BUTTON_DOWN_THIS_FRAME && c->ltrigger>=128 && c->rtrigger>=128) {
            if(!nm_title_render_release()) { enj_state_flag_shutdown(NULL);return; }
            nm_practice_attract_abandon();
            if(!nm_settings_mode_start()) enj_state_flag_shutdown(NULL);
            return;
        }
        if(c->button.START==ENJ_BUTTON_DOWN_THIS_FRAME || c->button.A==ENJ_BUTTON_DOWN_THIS_FRAME) {
            const uint32_t counter=nm_frontend_counter()+ticks+1u;
            if(!nm_title_render_release()) { enj_state_flag_shutdown(NULL);return; }
            nm_practice_attract_abandon();
            if(!nm_select_mode_start(0,counter)) enj_state_flag_shutdown(NULL);
            return;
        }
        break;
    }
    while(remainder_us>=NM_PRACTICE_TICK_US) { remainder_us-=NM_PRACTICE_TICK_US;++ticks;++pose_ticks; }
    if(ticks>TITLE_TICKS) ticks=TITLE_TICKS; /* a held frame adds no iteration (the original's loading frames) */
    if(ticks>=TITLE_TICKS && !nm_practice_attract_loading()) {
        printf("Native title: %u frames, %u late (longest %u us), %u frames past its last tick for the attract's load\n",
            (unsigned)frames,(unsigned)late,(unsigned)longest_us,(unsigned)held);
        nm_frontend_counter_set(nm_frontend_counter()+TITLE_TICKS+1u); /* phases 9 and 10 */
        if(!nm_title_render_release() || !nm_attract_mode_start()) enj_state_flag_shutdown(NULL);
        return;
    }
    if(ticks>=TITLE_TICKS && ++held==TITLE_HOLD_LIMIT) { /* the attract still loads: the title goes on, counting nothing */
        printf("FAIL: the attract's load not done %u title frames past its last tick (stuck, or a very slow host); the rest runs at once\n",
            (unsigned)held);
        nm_practice_attract_settle(); /* the rest at once, its palettes stored (the title ends at the next frame) */
    }
    if(!nm_title_render_enqueue(pose_ticks)) /* the pose and the prompt's blink follow the ticks, on past the hold */
        enj_state_flag_shutdown(NULL);
}

int nm_title_mode_start(void) {
    static enj_mode_t mode={.name="Daytona title",.mode_updater=title_update};
    if(!nm_title_render_load()) return 0;
    nm_race_sound_hook_engine_off(); /* 20fc/2100 at phase 8: be1700, be1900 (m2segment --trace-sound of the attract) */
    nm_sound_settle();
    (void)nm_practice_attract_preload(); /* the next attract scene, in the title's frames */
    last_us=0;remainder_us=ticks=pose_ticks=held=frames=late=longest_us=0;frame_clock=(nm_frame_clock_t){0};
    enj_mode_set(&mode);return 1;
}

int nm_attract_mode_start(void) {
    /* Phase 2 (197c): every attract loop, after the boot and after a game; its entry starts the
     * advertisement music, or with ADVERTISE SOUND OFF fades the music (1ca8, include/settings.h). */
    nm_frontend_counter_set(nm_frontend_counter()+1u);
    /* The recording loads here (the first time, or after a race's music) but starts with the cinematic, not under
     * the scene's loading. */
    nm_sound_hold(1);
    nm_sound_command(nm_game_settings()->advertise_sound==NM_ADVERTISE_OFF ? NM_MUSIC_FADE : NM_MUSIC_ADVERTISE);
    nm_race_sound_hook_engine_off(); /* 1cd4/1cd8: the same entry turns the engine and slip layers off. */
    nm_sound_settle();
    const int started=nm_practice_attract_start();
    nm_sound_hold(0);
    return started;
}
