/* NATIVE_PAD_MONKEY (local.cfg.mk): a scripted pad on port A in place of the real one, for an unattended console
 * run through several games on the production paths (attract, credit, select, race, results, name entry, attract
 * again). Linked with --wrap=enj_ctrl_get_states and --wrap=enj_ctrl_map_states; enDjinn and the product's sources
 * are unchanged.
 *
 * The script steps once a frame, in enDjinn's own pad refresh (enj_ctrl_map_states: the first thing of a frame in
 * enj_state_run_frame, before the mode's updater). The game reads the pad from several places in one frame (the
 * updater, the render's preparation), and a step keyed on a vblank or flip counter could change between two of
 * them, since those advance in an interrupt; this one cannot. The engine's own getter call, before its first
 * frame, is served too (the array is static). Button edges are those of enDjinn's two-bit states (enj_ctrl.h:
 * DOWN_THIS_FRAME on the step a button goes down, UP_THIS_FRAME on the step it goes up, one frame each), as a
 * real pad's. The run's end prints the step count against the PVR's flips, as the check of the once-a-frame key. */
#include <enDjinn/enj_enDjinn.h>
#include <dc/pvr.h>
#include <stdio.h>
#include <string.h>

#ifndef NATIVE_PAD_MONKEY_FRAMES
#define NATIVE_PAD_MONKEY_FRAMES 60000
#endif
#define MONKEY_FRAMES ((unsigned)(NATIVE_PAD_MONKEY_FRAMES))
/* NATIVE_PAD_MONKEY_SCHEDULE: 1 (default) the schedule below; 2 its variant whose credits land in the title and the
 * V.R. demo too and whose select picks every course, Expert among them. */
#ifndef NATIVE_PAD_MONKEY_SCHEDULE
#define NATIVE_PAD_MONKEY_SCHEDULE 1
#endif
#define MONKEY_SCHEDULE ((unsigned)(NATIVE_PAD_MONKEY_SCHEDULE))
#if NATIVE_PAD_MONKEY_SCHEDULE != 1 && NATIVE_PAD_MONKEY_SCHEDULE != 2
#error NATIVE_PAD_MONKEY_SCHEDULE is 1 or 2
#endif

/* Schedule 1, in frames. */
enum {
    MONKEY_GAS_PERIOD=330,   /* the accelerator's cycle: held at 255, then released for MONKEY_GAS_RELEASE frames
                                (the select screens confirm on the trigger's edge: under 96, then 160 or more) */
    MONKEY_GAS_RELEASE=40,
    MONKEY_STICK_PERIOD=720, /* one swing from the left lock to the right and back (a triangle wave) */
    MONKEY_START_HOLD=4,     /* frames START stays down at a press */
    MONKEY_START_GAP=600,    /* frames from one START press to the next, plus the drift */
    MONKEY_START_DRIFT=43,   /* the drift grows by this at every press, modulo MONKEY_START_SPAN: the presses walk
                                through the attract loop (cinematic, ranking, title, V.R. demo) */
    MONKEY_START_SPAN=200,
};

/* Schedule 2: schedule 1's gas and stick, with the credits and the select scripted.
 * Credits. On the console (build/hw-tools/hw/console-at3-att.log, dcload) the attract loop is the cinematic for
 * about 2000 frames (1922 ticks, the ranking overlay in them), the V.R. demo 870 (834 ticks) and the title 485 to
 * 598 (386 ticks, then held until the next scene's load is done); schedule 1's first press after the attract's
 * start comes within 600 frames, so its ten credits all landed in the cinematic (console-monkey1.log). Here each
 * credit is pressed a fixed number of frames into its scene, the scene cycling cinematic, demo, title; the mode's
 * name tells the scene, and as the race and the cinematic share one, the attract is known from the way in (the
 * boot, the title, the name entry: not the select). Outside the attract and the select START keeps schedule 1's
 * cadence (a slot reel stop in the race); in the select it is not pressed (START held at the transmission's
 * confirm is the time attack).
 * The select (gameplay/select_mode.c pad(), include/select.h). The stick moves the highlighted slot one step per
 * excursion past 64, re-armed inside 32; the course of slot 0/1/2 is Beginner/Advanced/Expert, and the slot is
 * 0 again at the transmission's state 8. Schedule 1's swing steps up then down and alternates slots 0 and 1, so
 * it never raced Expert. Here the stick is centred in the select, apart from MONKEY2_SLOT_OF[credit] excursions to
 * the right in its first frames, the accelerator held meanwhile (the select arms the trigger on a release, so no
 * edge confirms before they are done); the gas cycle's first edge after the hold then confirms the course, and
 * for a manual game one more excursion MONKEY2_MANUAL_AT frames after it: state 8 is 161 iterations after the
 * course's confirm (states 2 to 7), the transmission's edge counts from 177, the next gas edge comes 330 frames
 * after the first. */
enum {
    MONKEY2_CREDIT_CINEMATIC=600, /* frames into the cinematic (as schedule 1's first press) */
    MONKEY2_CREDIT_DEMO=300,      /* frames into the V.R. demo */
    MONKEY2_CREDIT_TITLE=200,     /* frames into the title: before its 386th tick on any host */
    MONKEY2_SELECT_HOLD=120,      /* the select's first frames: the accelerator held, the slot's excursions */
    MONKEY2_EXCURSION=40,         /* frames of one excursion, centre to the right lock and back */
    MONKEY2_MANUAL_AT=200,        /* frames after the course's confirm at which the manual excursion starts */
};
static const char *const monkey2_scene[3]={"Native driving practice","Native V.R. demo","Daytona title"};
static const unsigned monkey2_credit_at[3]={MONKEY2_CREDIT_CINEMATIC,MONKEY2_CREDIT_DEMO,MONKEY2_CREDIT_TITLE};
static const uint8_t monkey2_slot_of[4]={0,1,2,1}; /* the course slot per credit: Beginner, Advanced, Expert, Advanced */
static const uint8_t monkey2_manual[4]={0,1,0,1};  /* the transmission per credit */
#define MONKEY_MODE_SELECT "Course and transmission select"

enj_ctrlr_state_t **__real_enj_ctrl_get_states(void);
size_t __real_enj_ctrl_map_states(void);

static enj_ctrlr_state_t monkey_state;                   /* port A's state the game reads */
static enj_ctrlr_state_t *monkey_refs[MAPLE_PORT_COUNT]; /* the array handed out: port A scripted, the rest real */
static unsigned monkey_frame, monkey_presses, monkey_next_start=MONKEY_START_GAP, monkey_done;
static unsigned monkey_mode_frames, monkey_attract=1, monkey_credits, monkey_credit_hold; /* schedule 2: frames in the
                                                      mode, in the attract, credits pressed, frames left of a press */
static unsigned monkey_select_edges, monkey_select_edge_frame, monkey_gas_last; /* schedule 2's select: the gas
                                                      edges seen in it, the mode frame of the first, the last gas */
static unsigned monkey_calls, monkey_calls_max, monkey_frames_without_call;
static unsigned long monkey_calls_total;
static char monkey_mode[ENJ_MODE_NAME_CAPACITY];

/* enDjinn's table (enj_ctrl.c enj_update_button_state): bit 0 the level, bit 1 a change on this step. */
static uint32_t monkey_button(uint32_t previous, unsigned down) {
    const uint32_t level=down ? 1u : 0u;
    return level|(((previous&1u)!=level) ? 2u : 0u);
}

static const char *monkey_mode_name(void) {
    const enj_mode_t *mode=enj_mode_get();
    return mode ? mode->name : "(no mode)";
}

/* An excursion of the stick: centre to the right lock and back over MONKEY2_EXCURSION frames. */
static int8_t monkey2_excursion(unsigned phase) {
    const unsigned half=MONKEY2_EXCURSION/2u;
    return (int8_t)((phase<half ? phase : MONKEY2_EXCURSION-phase)*127u/half);
}

static void monkey_step(void) {
    const unsigned frame=monkey_frame++;
    if(frame) {
        if(monkey_calls>monkey_calls_max) monkey_calls_max=monkey_calls;
        if(!monkey_calls) ++monkey_frames_without_call;
    }
    monkey_calls=0;
    const char *mode=monkey_mode_name();
    const int in_select=!strcmp(mode,MONKEY_MODE_SELECT);
    if(strcmp(mode,monkey_mode)) {
        /* Schedule 2's attract: entered at the boot, from the title or the name entry, left by the select (the race
         * follows the select only). */
        if(in_select) { monkey_attract=0;monkey_select_edges=0; }
        else if(strcmp(mode,monkey2_scene[0]) || strcmp(monkey_mode,MONKEY_MODE_SELECT)) monkey_attract=1;
        monkey_mode_frames=0;
        if(!frame) printf("Pad monkey: schedule %u\n",MONKEY_SCHEDULE);
        snprintf(monkey_mode,sizeof(monkey_mode),"%s",mode);
        printf("Pad monkey: frame %u, mode %s\n",frame,mode);
    } else ++monkey_mode_frames;
    unsigned start=0;
    int8_t joyx=0;
    uint8_t rtrigger=0;
    if(frame<MONKEY_FRAMES) {
        rtrigger=frame%MONKEY_GAS_PERIOD<MONKEY_GAS_RELEASE ? 0u : 255u;
        const unsigned half=MONKEY_STICK_PERIOD/2u,phase=frame%MONKEY_STICK_PERIOD;
        const unsigned ramp=(phase<half ? phase : MONKEY_STICK_PERIOD-phase)*254u/half; /* 0..254 */
        joyx=(int8_t)((int)ramp-127);
        if(MONKEY_SCHEDULE==2 && in_select) {
            const unsigned credit=monkey_credits ? (monkey_credits-1u)%4u : 0;
            const unsigned slot=monkey2_slot_of[credit],manual=monkey2_manual[credit];
            joyx=0;
            if(monkey_mode_frames<MONKEY2_SELECT_HOLD) {
                rtrigger=255;
                if(!monkey_mode_frames)
                    printf("Pad monkey: select after credit %u: slot %u, %s\n",monkey_credits,slot,manual ? "manual" : "automatic");
                if(monkey_mode_frames/MONKEY2_EXCURSION<slot) joyx=monkey2_excursion(monkey_mode_frames%MONKEY2_EXCURSION);
            } else {
                if(rtrigger>=160 && monkey_gas_last<96) {
                    if(!monkey_select_edges++) monkey_select_edge_frame=monkey_mode_frames;
                }
                if(manual && monkey_select_edges) {
                    const unsigned since=monkey_mode_frames-monkey_select_edge_frame;
                    if(since>=MONKEY2_MANUAL_AT && since<MONKEY2_MANUAL_AT+MONKEY2_EXCURSION)
                        joyx=monkey2_excursion(since-MONKEY2_MANUAL_AT);
                }
            }
        }
        if(monkey_credit_hold) { /* the rest of a credit's press, whatever the mode became */
            start=1;--monkey_credit_hold;
        } else if(MONKEY_SCHEDULE==2 && monkey_attract) {
            const unsigned scene=monkey_credits%3u,at=monkey2_credit_at[scene];
            if(!strcmp(mode,monkey2_scene[scene]) && monkey_mode_frames==at) {
                start=1;monkey_credit_hold=MONKEY_START_HOLD-1u;
                ++monkey_credits;++monkey_presses;
                printf("Pad monkey: credit press %u at frame %u, %u frames into %s\n",monkey_credits,frame,at,mode);
            }
        }
        if(MONKEY_SCHEDULE==2 && (monkey_attract || in_select)) {
            /* The cadence goes on without a press, so that it does not fire as the select or the race starts. */
            if(frame>=monkey_next_start) monkey_next_start+=MONKEY_START_GAP+(monkey_presses*MONKEY_START_DRIFT)%MONKEY_START_SPAN;
        } else if(frame>=monkey_next_start) {
            if(frame<monkey_next_start+MONKEY_START_HOLD) {
                start=1;
                if(frame==monkey_next_start)
                    printf("Pad monkey: START press %u at frame %u in %s\n",monkey_presses+1u,frame,mode);
            } else {
                monkey_next_start+=MONKEY_START_GAP+(monkey_presses*MONKEY_START_DRIFT)%MONKEY_START_SPAN;
                ++monkey_presses;
            }
        }
    }
    monkey_gas_last=rtrigger;
    monkey_state.button.START=monkey_button(monkey_state.button.START,start);
    monkey_state.joyx=joyx;monkey_state.joyy=0;
    monkey_state.ltrigger=0;monkey_state.rtrigger=rtrigger; /* the left trigger never: the settings chord */
    monkey_state.portnum=0;monkey_state.state=NULL;
    if(frame>=MONKEY_FRAMES && !monkey_done) {
        monkey_done=1;
        pvr_stats_t stats;
        const int have_stats=pvr_get_stats(&stats)==0;
        printf("Pad monkey: done at frame %u in %s: schedule %u, %u START presses (%u credit presses), %lu pad reads (at most %u a frame, "
            "%u frames without one), PVR flips %lu vblanks %lu; pad released, returning to loader\n",frame,mode,MONKEY_SCHEDULE,
            monkey_presses,monkey_credits,monkey_calls_total,
            monkey_calls_max,monkey_frames_without_call,have_stats ? (unsigned long)stats.frame_count : 0ul,
            have_stats ? (unsigned long)stats.vbl_count : 0ul);
        enj_state_flag_shutdown(NULL);
    }
}

size_t __wrap_enj_ctrl_map_states(void) {
    const size_t mapped=__real_enj_ctrl_map_states();
    monkey_step();
    return mapped;
}

enj_ctrlr_state_t **__wrap_enj_ctrl_get_states(void) {
    enj_ctrlr_state_t **real=__real_enj_ctrl_get_states();
    for(size_t i=1;i<MAPLE_PORT_COUNT;++i) monkey_refs[i]=real[i];
    monkey_refs[0]=&monkey_state;
    ++monkey_calls;++monkey_calls_total;
    return monkey_refs;
}
