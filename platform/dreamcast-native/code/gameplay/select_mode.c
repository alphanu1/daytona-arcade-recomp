/* Game start and select screens: include/gameplay/select_mode.h. */
#include <enDjinn/enj_enDjinn.h>
#include "gameplay/select_mode.h"
#include "gameplay/settings_mode.h"
#include "select.h"
#include "practice.h"
#include "render/scene/practice.h"
#include "render/ui/select_render.h"
#include "audio/sound.h"
#include "audio/music_select.h"
#include <stdio.h>
#include "nm_mem.h"

#ifndef NATIVE_COURSES
#define NATIVE_COURSES 7u /* every course: Beginner 0, Expert 1, Advanced 2 */
#endif

int nm_practice_race_from_select(unsigned course, unsigned transmission);

static nm_select_t select_state;
static int pending;
static uint32_t counter;
static nm_frame_clock_t frame_clock;
static uint32_t remainder_us;
static const char *root;
static unsigned frames;
/* Pad (a proposal): left/right on the stick or the D-pad move the highlighted choice, START, A or the
 * right trigger (the accelerator) confirm. The select is fed the wheel position of the choice
 * (decoded steering 0 / 64 / 127 for the courses' slots, 0 / 127 for automatic / manual: inside the
 * original's ranges <= 0, 1..108, > 108 and <= 30, > 30) and one accelerator edge per confirm. */
static unsigned wanted, stick_armed=1, confirm_pending, trigger_armed=1;
static int32_t previous_steer;

uint32_t nm_frontend_counter(void) { return counter; }
void nm_frontend_counter_set(uint32_t value) { counter=value; }

unsigned nm_select_course_raced(unsigned chosen) {
    if(chosen<3 && ((NATIVE_COURSES>>chosen)&1u)) return chosen;
    printf("Native select: course %u chosen, its data is not in this build (NATIVE_COURSES=%u); racing course 0\n",
        chosen,(unsigned)NATIVE_COURSES);
    return 0;
}

static void pad(void) {
    enj_ctrlr_state_t **controllers=enj_ctrl_get_states();
    for(size_t i=0;i<enj_ctrl_states_length();++i) if(controllers[i]) {
        const enj_ctrlr_state_t *c=controllers[i];
        int step=0;
        if(c->button.LEFT==ENJ_BUTTON_DOWN_THIS_FRAME) step=-1;
        if(c->button.RIGHT==ENJ_BUTTON_DOWN_THIS_FRAME) step=1;
        if(stick_armed && c->joyx<=-64) { step=-1;stick_armed=0; }
        if(stick_armed && c->joyx>=64) { step=1;stick_armed=0; }
        if(c->joyx>-32 && c->joyx<32) stick_armed=1;
        const unsigned choices=select_state.state<=1 ? 3u : 2u;
        if(step<0 && wanted) --wanted;
        if(step>0 && wanted+1<choices) ++wanted;
        if(c->button.START==ENJ_BUTTON_DOWN_THIS_FRAME || c->button.A==ENJ_BUTTON_DOWN_THIS_FRAME) confirm_pending=1;
        if(trigger_armed && c->rtrigger>=160) { confirm_pending=1;trigger_armed=0; }
        if(c->rtrigger<96) trigger_armed=1;
        break;
    }
}

static void finish(void) {
    if(!nm_select_render_release()) { enj_state_flag_shutdown(NULL);return; }
    const unsigned course=select_state.course,transmission=select_state.transmission;
    printf("Native select: course %u transmission %s after %u frames; race entry at 5010b0 %lu\n",course,
        transmission ? "manual" : "automatic",frames,(unsigned long)counter);
    pending=1;
    if(!nm_practice_race_from_select(course,transmission)) enj_state_flag_shutdown(NULL);
}

static void update(void *unused) {
    (void)unused;
    nm_sound_update();
    if(enj_state_get()->flags.shut_down) { nm_select_render_release();return; }
    uint32_t frame_ticks=nm_frame_ticks(&frame_clock); /* one tick per displayed frame (frame_clock.h) */
    if(frame_ticks>4) frame_ticks=4;
    remainder_us+=frame_ticks*NM_PRACTICE_TICK_US;
    pad();
    while(remainder_us>=NM_PRACTICE_TICK_US) {
        remainder_us-=NM_PRACTICE_TICK_US;
        /* The highlighted choice restarts at slot 0 where the original's slot does (state 8). */
        if(select_state.state==8) wanted=0;
        const int32_t steer=select_state.state<=1 ? (int32_t[]){0,64,127}[wanted<3 ? wanted : 0] : (wanted ? 127 : 0);
        const int accepting=select_state.state==1 || select_state.state==9;
        nm_select_input_t input={steer,previous_steer,(uint8_t)(accepting && confirm_pending),0};
        confirm_pending=0; /* a press outside the two choosing states is not kept for them */
        previous_steer=steer;
        nm_select_output_t out;
        ++counter;
        if(!nm_select_tick(&select_state,&input,counter,&out)) { puts("FAIL: native select rejected an iteration");enj_state_flag_shutdown(NULL);return; }
        for(unsigned i=0;i<out.sounds;++i) nm_sound_command(out.sound[i]);
        if(select_state.phase==19) { finish();return; }
    }
    ++frames;
    if(!nm_select_render_enqueue(&select_state,counter)) enj_state_flag_shutdown(NULL);
}

int nm_select_mode_start(int scene, uint32_t credit_counter) {
    root=nm_practice_root();
    if(!root) return 0;
    pending=0;
    counter=credit_counter;
    if(!nm_practice_game_start(scene,credit_counter)) { puts("FAIL: native game start (phases 12/17) rejected");return 0; }
    counter+=2;            /* phases 12 and 17 */
    nm_select_enter(&select_state);
    wanted=0;stick_armed=1;confirm_pending=0;trigger_armed=0;previous_steer=0;
    frame_clock=(nm_frame_clock_t){0};remainder_us=0;frames=0;
    if(!nm_select_render_load(root)) return 0;
    nm_sound_command(NM_MUSIC_GAME_START); /* 2850: phase 12 */
    nm_sound_settle();
    printf("Native game start: credit at 5010b0 %lu, scene %d\n",(unsigned long)credit_counter,scene);
    static enj_mode_t mode={.name="Course and transmission select",.mode_updater=update};
    enj_mode_set(&mode);return 1;
}

int nm_select_attract_button(unsigned start, unsigned ltrigger, unsigned rtrigger) {
    /* Both triggers and START: the settings screen, as from the title. */
    if(!start || ltrigger<128 || rtrigger<128) return 0;
    nm_practice_render_release();
    if(!nm_settings_mode_start()) enj_state_flag_shutdown(NULL);
    return 1;
}

int nm_select_credit(uint32_t credit_counter) {
    nm_practice_render_release();
    return nm_select_mode_start(NM_CREDIT_CINEMATIC,credit_counter);
}

int nm_select_race_state(uint32_t *counter_base) {
    if(!pending) return 0;
    pending=0;
    /* The race tick 0 is the phase20 iteration: two after the select's last (phase 19 in between). */
    *counter_base=counter+2u;
    return 1;
}
