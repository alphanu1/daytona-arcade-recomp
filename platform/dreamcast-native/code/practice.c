// Gameplay/mode owner: input, fixed ticks, race lifecycle and asset lifetime.
// Per-frame presentation is queued through render/scene/practice.h, never drawn here.
#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <kos/mm.h>
#include <arch/stack.h>
#ifdef NATIVE_FRAME_COMPARE
#include <dc/video.h>
#endif
#include "practice.h"
#include "sample_profile.h"
#include "heap.h"
#include "gameplay/title_mode.h"
#include "audio/sound.h"
#include "audio/race_sound_hooks.h"
#include "audio/music_select.h"
#include "render/ui/title_render.h"
#include "race_scene.h"
#include "shifter.h"
#include "steer_curve.h"
#if !defined(NATIVE_RACE_SCRIPT) && !defined(NATIVE_DRIVING_TEST)
static uint8_t practice_steer_table[256]; /* the pad's stick to the wheel's ADC byte (steer_curve.h), built once */
#endif
#include "race_intent_data.h"
#include "race_record_data.h"
#include "game_save.h"
#include "gameplay/select_mode.h"
#include "frontend.h"
#include "nm_mem.h"
/* The records live in the game's save state (include/game_save.h: the original's three backup-RAM tables, their
 * defaults over erased RAM, the VMU), outside race_scene, so session resets do not erase achieved records. The
 * race's table is its course's (practice.course; 23b544[501460] names table N for slot N). */
static unsigned practice_record_selected=UINT32_MAX;
static uint32_t practice_record_notification;
static int practice_record_published;
#include "intro_scene.h"
#include "podium_data.h"
#include "race_start_data.h"
#include "props_scene.h"
#include "render/scene/slot_glass.h"
#include "render/ui/podium_render.h"
#include "render/ui/attract_2d_render.h"
#include "race_camera.h"
#include "course_assets.h"
#ifndef NM_RACE_START_COURSES
#error "race_start_data.h predates nm_race_course_starts (prepare_intro.py --race-start --courses)"
#endif
_Static_assert(NM_RACE_START_COURSES>>NM_COURSE&1u,
    "NATIVE_COURSE names a course the race-start header does not hold (prepare_intro.py --race-start --courses)");
#ifndef NM_PODIUM_COURSES
#error "podium_data.h predates nm_podium_courses (prepare_intro.py --podium --courses)"
#endif
#ifndef NM_ATTRACT_COURSES
#error "podium_data.h predates nm_attract_courses (prepare_intro.py --podium --courses)"
#endif
#include "attract.h"
#include "vr_demo.h"
#include "scene_loader.h"
#include "render/materials/palette_ram.h"
#include "visibility.h"
#include "pose.h"
#include "geometry.h"

#include "render/scene/practice.h"
#include "render/ui/hud_render.h" /* HUD */
#include "hud_scene.h"
/* Palette shading: result palettes are written at the phase25 hand-off. */
#include "render/materials/result_materials.h"
#include "render/ui/result_labels.h"
alignas(32) static nm_practice_scene_t practice;
static const char *practice_root;
static void practice_update(void *unused);
#ifdef NATIVE_SAMPLE_PROFILE
static void practice_update_sampled(void *unused);
static enj_mode_t practice_mode={.name="Native driving practice",.mode_updater=practice_update_sampled};
#elif defined(NM_DIAG_DROP_LOG) || defined(NM_DIAG_PMU)
static void practice_update_measured(void *unused);
static enj_mode_t practice_mode={.name="Native driving practice",.mode_updater=practice_update_measured};
#else
static enj_mode_t practice_mode={.name="Native driving practice",.mode_updater=practice_update};
#endif
/* The cinematic stage graph runs through phase 3 (1602 iterations: its entry, the scene start, then 1601 ticks)
 * and on under the ranking overlay of phases 4/5 (1 + 320 ticks, the latched flags 1<<4 and 1<<5): 1922 ticks
 * after the scene start, then the V.R. demo (phases 6/7, 1 + 834 iterations: practice_demo_start, vr_demo.h, on
 * the race scene, drawn by the race's presentation without its HUD) and the title (phases 8-10).
 * 5010b0 = intro.object_clock + intro.ticks (include/attract.h). */
static const uint32_t attract_tick_limit=NM_ATTRACT_TICKS;
static int practice_credit_pending;
/* The carried part of the original's car pool (frontend_pool.h): from power-on (nm_practice_run), each scene's exit
 * stores its cars, each scene's start reads it. */
static nm_entity_pool_t practice_pool;
static int practice_demo_start(void);
/* 501460 of the attract loop: the course of the next (or running) cinematic. Course 0 from power-on, the next
 * one (attract.h nm_attract_next_course) at the end of each cinematic (the original's: at the V.R. demo's end,
 * before the title), and the raced course after a game. */
static unsigned practice_attract_course;
/* The attract scene of practice_attract_course is loaded (nm_practice_attract_preload in the title's frames). */
static int practice_attract_ready;
/* The operator difficulty 53e59c (include/settings.h): the native settings' value when the race starts (the
 * original reads it at the start, 3860, and at every award, 13574; it changes only in test mode). A scripted
 * race replays a NORMAL race, whatever the settings say. */
static unsigned practice_difficulty=NM_DIFFICULTY_NORMAL;
#ifdef NATIVE_RACE_SCRIPT
/* Diagnostic replay: per-tick decoded controls generated locally by
 * tests/trace_race_native.c (--script-header); never tracked. */
#include "race_script_data.h"
#endif
/* The transmission 501464 (the original's transmission select 2256fc; no
 * native select screen yet): the build's default, NATIVE_TRANSMISSION=manual
 * (local.cfg.mk) or a manual race script; holding PRACTICE_TRANSMISSION_CHORD
 * at the START that begins a race picks the other one. */
#if defined(NATIVE_RACE_SCRIPT) ? defined(NM_RACE_SCRIPT_MANUAL) : defined(NATIVE_TRANSMISSION_MANUAL)
enum { PRACTICE_TRANSMISSION=1 };
#else
enum { PRACTICE_TRANSMISSION=0 };
#endif
/* Pad mapping of the manual shifter (include/shifter.h, sequential): a proposal
 * that leaves Y free for the view change and B as it is. */
#define PRACTICE_SHIFT_UP(c) ((c)->button.A==ENJ_BUTTON_DOWN_THIS_FRAME)
#define PRACTICE_SHIFT_DOWN(c) ((c)->button.X==ENJ_BUTTON_DOWN_THIS_FRAME)
#define PRACTICE_TRANSMISSION_CHORD(c) ((c)->button.X&1u)
static unsigned practice_shift_requests; /* Pad shift requests fed to race ticks (diagnostic). */
#ifdef NATIVE_RACE_CHECKPOINTS
/* Console-versus-host agreement: same rows as tests/trace_race_native.c, from
 * race start through podium and results. ~13000 ticks/50 plus phase changes. */
#include "race_checkpoint.h"
static uint32_t checkpoints[512][NM_RACE_CHECKPOINT_WORDS];
static unsigned checkpoint_count;
static uint32_t checkpoint_tick;
static uint8_t checkpoint_phase;
static void practice_checkpoint(int32_t timer) {
    if(!nm_race_checkpoint_due(checkpoint_tick,practice.race.phase,&checkpoint_phase) ||
       checkpoint_count>=sizeof(checkpoints)/sizeof(checkpoints[0])) return;
    const nm_player_t *cars[44];
    /* attract_active is set exactly while the podium scene's cars are live. */
    const unsigned count=practice.attract_active ? practice.intro.count : practice.race.count;
    for(unsigned i=0;i<count && i<44;++i)
        cars[i]=practice.attract_active ? practice.intro.cars+i : &practice.race.cars[i].car;
    nm_race_checkpoint_row(checkpoints[checkpoint_count++],checkpoint_tick,&practice.race,timer,cars,count);
}
#endif
#ifdef NATIVE_FRAME_PROFILE
static uint32_t frame_budget_samples,frame_over_18ms,frame_over_25ms;
static uint64_t frame_steady_max_us;
static size_t frame_last_flip,frame_last_vblank;
static uint64_t frame_flip_sum_ns,frame_flip_max_ns;
static uint32_t frame_flip_samples,frame_flip_over_20ms;
static size_t frame_flips,frame_vblanks;
#endif

/* Reuse the verified follow-view and borrowed-owner detail path. Intro storage
 * here owns only presentation arrays; race.cars is the sole simulation owner.
 * ponytail: split that presentation storage once the cinematic/race transition
 * owns both scenes; the unused intro players currently cost main RAM only. */
/* The live tick's step that rejected and, inside practice_race_detail, its call
 * (diagnostic: "Native practice rejected" names them). */
static const char *practice_reject_step,*practice_detail_reject;
/* The view buttons held (53e120 layout, race_view.h): the pad's (practice_view_buttons) or the script's. */
static uint32_t practice_view_held;
/* The keyed cameras of the race's course (nm_race_course_starts[course], bound by practice_reset_session). */
static nm_race_cameras_t practice_cameras={nm_race_keyed_tracks,
#ifdef NM_RACE_VIEW_DATA
    nm_race_pit_view_tracks,
#else
    NULL,
#endif
    nm_race_crash_keys,nm_race_crash_times,NM_RACE_CRASH_KEYS};
/* The course slot 501460 the next race starts on (nm_practice_course_select; NATIVE_COURSE at boot). The
 * scene's practice.course is the slot of the course packs now loaded (the attract's is 0). */
static unsigned practice_race_course=NM_COURSE;
static int practice_tunnel_warned; /* the car pack lacks the windows' tunnel bindings: said once */
/* Pad mapping of the views (a proposal; shifting keeps A and X, B stays the session reset, START+B the
 * inspector): the D-pad holds
 * VR1..VR4 as the cabinet's four buttons (up: driver's view, right: hood, down: behind, left: behind and
 * high), START held keeps the view; Y steps to the next view (VR1, VR2, VR3, VR4 round) with one tick of
 * that view's button. START + right, down, left: the CONDITION panel's zoom, START + up + right, up alone,
 * up + left: the map's size (hud.h nm_hud_overlay). */
#define PRACTICE_VIEW_NEXT(c) ((c)->button.Y==ENJ_BUTTON_DOWN_THIS_FRAME)
static uint32_t practice_view_pad,practice_view_step;
static void practice_view_buttons(const enj_ctrlr_state_t *c) {
    practice_view_pad=(c->button.START&1u ? NM_VIEW_HELD_START : 0u)|(c->button.UP&1u ? NM_VIEW_HELD_VR1 : 0u)|
        (c->button.RIGHT&1u ? NM_VIEW_HELD_VR2 : 0u)|(c->button.DOWN&1u ? NM_VIEW_HELD_VR3 : 0u)|
        (c->button.LEFT&1u ? NM_VIEW_HELD_VR4 : 0u);
    /* The HUD chord 129d0: a D-pad has no VR1 + VR3, the middle map size; START + up alone (no chord in the original) stands for it. */
    if((practice_view_pad&(NM_VIEW_HELD_START|NM_VIEW_HELD_VR1|NM_VIEW_HELD_VR2|NM_VIEW_HELD_VR3|NM_VIEW_HELD_VR4))==
       (NM_VIEW_HELD_START|NM_VIEW_HELD_VR1)) practice_view_pad|=NM_VIEW_HELD_VR3;
    if(PRACTICE_VIEW_NEXT(c)) practice_view_step=1;
}
/* The held word of the next race tick: the pad's, plus the next view's button on the first tick after Y. */
static uint32_t practice_view_tick(void) {
    static const uint32_t button[4]={NM_VIEW_HELD_VR1,NM_VIEW_HELD_VR2,NM_VIEW_HELD_VR3,NM_VIEW_HELD_VR4};
    uint32_t held=practice_view_pad;
    if(practice_view_step && !(held&NM_VIEW_HELD_START)) held=(held&NM_VIEW_HELD_START)|button[(practice.race.view+1u)&3u];
    practice_view_step=0;
    return held;
}
/* The camera heading 5014a8 the reflection 6f24 reads (after this tick's view, before 8814): the in-car views'
 * (race_view.h), else the follow view's (the keyed cameras keep it too). */
static int16_t practice_reflection_heading(const nm_player_t *car) {
    int16_t angles[3],heading;
    if(car->camera_mode==2 && nm_race_view_angles(car,angles,&heading)) return heading;
    return nm_follow_heading(car);
}
static int practice_race_detail(void) {
    practice_detail_reject=nm_race_camera_pass(&practice.race,&practice.intro,practice.math,&practice.scenery,
        &practice_cameras,practice_view_held);
    if(practice_detail_reject) return 0;
    practice_detail_reject="";
    return 1;
}

/* A live tick rejected: every rejection is a port bug (the original never
 * rejects a tick), so name the step and the state that leads to it. */
static void practice_report_reject(void) {
    const nm_race_scene_t *r=&practice.race;const nm_player_t *p=&r->cars[0].car;
    uint32_t speed;nm_memcpy(&speed,&p->boundary.speed,4);
    printf("Native practice rejected tick %lu: step=%s%s%s stage=%s car=%u phase=%u/%u\n",
        (unsigned long)practice.state.ticks,practice_reject_step ? practice_reject_step : "?",
        practice_reject_step && !strcmp(practice_reject_step,"race detail") ? " / " : "",
        practice_reject_step && !strcmp(practice_reject_step,"race detail") ? practice_detail_reject : "",
        nm_race_stage_name(r->stage),r->shared.updating,r->active_phase,r->phase);
    printf("  player: road=%lu/%u profile=%u/%u lap=%u/%u speed=%08lx speed-index=%ld gear=%lu rpm=%ld"
        " damage=%08lx flags=%08lx spin=%d recovery=%d camera=%ld/%u mask=%x\n",
        (unsigned long)p->boundary.ground.road,practice.contact.quad_count,(uint16_t)p->phase[0],
        practice.road.profile_count,p->lap.completed,p->checkpoints.total_laps,(unsigned long)speed,
        (long)p->speed_index,(unsigned long)p->gear,(long)p->rpm,(unsigned long)p->boundary.damage.damage,
        (unsigned long)p->boundary.damage.flags,(int)p->boundary.damage.spin_ticks,(int)p->boundary.recovery_ticks,
        (long)p->camera_mode,(unsigned)p->camera_transition,(unsigned)p->boundary.ground.combined_mask);
    printf("  race: ticks=%lu remaining=%lu clock=%lu marks=%u/%u start-edges=%u rolling=%u\n",
        (unsigned long)r->ticks,(unsigned long)r->award.remaining,(unsigned long)r->shared.clock,
        practice.marks.count,practice.road.profile_count,practice.start_edges,r->rolling[0]);
}

/* The original's music choice (501a94, audio/music_select.h): no VR buttons in the
 * native race, so the course's own. */
#define practice_music_select (practice.course)

/* The race HUD's state (include/hud.h): computed here, in a unit built with the gameplay's float flags
 * (the CONDITION panel's zoom, radius and circle test are the original's exactly), drawn by
 * code/render/ui/hud_render.c, whose units are built with -ffast-math. */
static nm_hud_t practice_hud;
static nm_hud_cars_t practice_hud_cars;
static void practice_hud_reset(void) {
    nm_hud_race_t race;
    nm_memset(&practice_hud_cars,0,sizeof(practice_hud_cars));practice_hud_cars.math=practice.math;
    practice_hud_cars.manual=(uint8_t)practice.transmission; /* b0c0, the gear indicator */
    nm_hud_observe(&practice.race,&race,&practice_hud_cars);nm_hud_reset(&practice_hud,&race,&practice_hud_cars);
    practice_hud.course=(uint8_t)practice.course;
    nm_hud_render_reset(&practice_hud);
}
static void practice_hud_tick(void) {
    nm_hud_race_t race;
    nm_hud_observe(&practice.race,&race,&practice_hud_cars);race.held=practice_view_held; /* 129d0 */
    nm_hud_tick(&practice_hud,&race,&practice_hud_cars);
    nm_hud_render_tick(&practice_hud);
}

/* The end of one results tick (gameplay/results.c, after its phase handler):
 * the race field's pass unless the results follow a podium, then the
 * checkpoint row of this tick (the same order as tests/trace_race_native.c). */
int nm_practice_results_tick(nm_practice_scene_t *scene,unsigned results_phase,
        unsigned collision_phase,nm_race_intent_output_t *out) {
    out->count=0;
    if(!scene->results.after_podium &&
       (!nm_race_scene_results_field(&scene->race,scene->math,&scene->road,results_phase,scene->results.penalty,
            collision_phase,0,scene->transmission) ||
        !nm_race_scene_events(&scene->race,nm_race_intent_rows,57,nm_race_intent_alternate[scene->course],nm_race_intent_special[NM_RACE_INTENT_UNIT],
            nm_race_announcement_rows+nm_race_announcement_offsets[scene->course],nm_race_announcement_counts[scene->course],
            nm_race_announcement_ranks,scene->results.penalty,out))) return 0;
    /* The world actors of the race's pool (Expert's, include/expert_actors.h) run on through these results, START edges
     * included; after a podium no task reads them. */
    const uint8_t start=scene->start_pending;scene->start_pending=0;
    if(!scene->results.after_podium) {
        scene->start_edges+=start;
        nm_props_results_step(&scene->race,scene->results.ticks,results_phase,scene->math,&scene->contact);
    }
#ifdef NATIVE_RACE_CHECKPOINTS
    ++checkpoint_tick;practice_checkpoint((int32_t)scene->results.timer);
#endif
    return 1;
}

static int practice_reset_session(const nm_player_t *previous) {
    practice.results.mode=practice.results.ticks=0;practice.results.timer=practice.results.penalty=0;practice.results.scroll=0;
    practice.results.show_labels=0;practice.results.after_podium=0;practice.results.podium_ticks=0;
    practice.results.announcement_cursor=practice.results.commands=0;practice.results.command_hash=UINT32_C(2166136261);
    practice_record_published=0;practice.podium=(nm_podium_t){0};
#ifndef NATIVE_RACE_SCRIPT
    practice_difficulty=nm_game_settings()->difficulty;
#endif
    nm_memset(&practice.state,0,sizeof(practice.state));nm_memset(&practice.intro,0,sizeof(practice.intro));
    /* The loaded course's start camera, pit cameras and pit stop (race_start_data.h nm_race_course_starts). */
    const nm_race_course_start_t *tables=nm_race_course_starts+practice.course;
    if(practice.course>2 || !tables->tracks || practice.road.slot!=practice.course) return 0;
    practice_cameras.tracks=tables->tracks;practice_cameras.tunnel=tables->tunnel;
#ifdef NM_RACE_VIEW_DATA
    practice_cameras.pit_views=tables->pit_views;
#endif
    /* e5ac: the grid's body list 286fab0[501464]. */
    if(!nm_race_scene_start_carried(&practice.race,practice.math,&practice.contact,&practice.road,
        practice.intro_paths,practice_difficulty,practice.transmission,&practice.marks,practice.road.profile_count,tables->pit,previous)) return 0;
#ifdef NM_RACE_VIEW_DATA
    practice.race.view_presets=nm_race_view_presets; /* VR1..VR4 (race_view.h) */
#endif
    practice_view_held=practice_view_pad=practice_view_step=0;
    /* 3980[course] cars (40, 30, 20) from the 40-entry catalogue of 286fac0 rows every course shares. */
    if(practice.race.count!=practice.road.grid_count || practice.race.count>40 ||
       sizeof(nm_intro_body_models)/sizeof(nm_intro_body_models[0])!=40) return 0;
    practice.intro.count=practice.race.count;
    practice.race_bodies=0;
    nm_spin_smoke_reset(&practice.spin_smoke);
    practice.race_commands=0;
    practice.race_command_hash=UINT32_C(2166136261);
    nm_race_sound_hook_reset(0);
    nm_race_sound_hook_course(practice.road.awards); /* the announcer's prefetch: the lap awards */
    practice.start_pending=0;practice.start_edges=0;practice.shift_pending=0;
    nm_props_start_edges(&practice.start_edges); /* the START edges Expert's actors see (props_scene.c) */
    for(unsigned i=0;i<practice.race.count;++i) {
        practice.intro.detail_order[i]=(uint8_t)i;practice.intro.detail_depth[i]=INT16_MAX;
    }
    practice_hud_reset(); /* HUD: race entry state */
    (void)nm_practice_render_tunnel(&practice,0,0); /* a session reset in a tunnel: the windows' own texture again */
    NM_COPY(&practice.state.car,&practice.race.cars[0].car);return 1;
}

static int practice_reset_attract(void) {
    practice.transmission=0; /* The attract's car is the automatic one (intro_scene.h curve 0). */
    nm_memset(&practice.state,0,sizeof(practice.state));
    nm_memset(&practice.race,0,sizeof(practice.race));
    nm_memset(&practice.results,0,sizeof(practice.results));practice.podium=(nm_podium_t){0};
    const nm_podium_course_t *graph=nm_attract_courses+practice.course; /* 2865a60[501460] */
    if(practice.course>2 || !graph->stages ||
       !nm_intro_scene_start_attract(&practice.intro,graph->stages,graph->count,practice.math,&practice.road,practice.intro_paths) ||
       !nm_attract_carry_into_cinematic(&practice.intro,&practice_pool,&practice.road)) return 0;
    nm_intro_scene_light(&practice.intro,nm_race_course_starts[practice.course].tunnel); /* the course's tunnel ranges */
    if(!nm_intro_scene_detail(&practice.intro,practice.math,&practice.scenery)) return 0;
    practice.race_bodies=0;
    NM_COPY(&practice.state.car,&practice.intro.cars[0]);
    practice.state.ticks=practice.intro.ticks;
    practice.attract_active=1;
    return 1;
}

/* The course packs (course_assets.h): contact, road, the eight routes, the visibility pack and the road
 * marks, all of course slot `practice.course`. */
static void practice_course_unload(void) {
    free(practice.marks.values);practice.marks.values=NULL;practice.marks.count=0;
    nm_scenery_unload(&practice.scenery);
    for(unsigned i=1;i<8;++i) nm_path_unload(practice.intro_paths+i);
    nm_path_unload(&practice.path);nm_memset(practice.intro_paths,0,sizeof(practice.intro_paths[0]));
    nm_road_unload(&practice.road);nm_contact_unload(&practice.contact);
    practice.course_loaded=0;
}
/* Loads the packs of COURSE in place of the loaded ones. A different course is loaded only between scenes,
 * with the scene's render packs released (nm_practice_render_unload), so that every block above the course
 * packs is free and the new packs take the top of the heap (heap.h). */
static int practice_course_load(unsigned course) {
    if(practice.course_loaded && practice.course==course) return 1;
    if(course>2 || !practice_root) return 0;
    nm_practice_render_unload(&practice);
    practice_course_unload();
    char file[256];
    const char *stage=file;
#define ASSET(kind,route) (nm_course_asset(file,sizeof(file),practice_root,course,kind,route) ? file : NULL)
    int ok=nm_contact_load(&practice.contact,ASSET(NM_COURSE_CONTACT,0)) &&
        nm_road_load(&practice.road,ASSET(NM_COURSE_ROAD,0)) &&
        nm_path_load(&practice.path,ASSET(NM_COURSE_ROUTE,0));
    if(ok) {
        practice.intro_paths[0]=practice.path; /* Borrow route0; cleanup owns it once. */
        for(unsigned i=1;ok && i<8;++i) ok=nm_path_load(practice.intro_paths+i,ASSET(NM_COURSE_ROUTE,i));
    }
    if(ok) ok=nm_scenery_load(&practice.scenery,ASSET(NM_COURSE_VISIBILITY,0));
#undef ASSET
    if(ok && (practice.road.slot!=course || practice.contact.slot!=course || practice.scenery.slot!=course)) {
        stage="course slot";ok=0;
    }
    if(ok) {
        stage="road marks";
        practice.marks.count=practice.road.profile_count-1;
        ok=(practice.marks.values=nm_calloc(practice.road.profile_count,1))!=NULL;
    }
    if(!ok) { printf("FAIL: course %u packs at %s\n",course,stage);practice_course_unload();return 0; }
    practice.course=course;practice.course_loaded=1;
    printf("Native course %u: packs loaded (%lu bytes of contact and road)\n",course,
        (unsigned long)(practice.contact.payload_bytes+practice.road.payload_bytes));
    return 1;
}

/* The front end's course choice: the slot (0 Beginner, 1 Expert, 2 Advanced) the next race starts on.
 * Refuses a course the race-start header does not hold; the packs are checked when that race loads. */
int nm_practice_course_select(unsigned course) {
    if(course>2 || !(NM_RACE_START_COURSES>>course&1u) || !nm_race_course_starts[course].tracks ||
       !(NM_PODIUM_COURSES>>course&1u) || !nm_podium_courses[course].stages) return 0;
    practice_race_course=course;return 1;
}

/* A scene's own first pass draws no car (no previous cells), but the load-time
 * SH4 self-checks (FTRV matrix and lighting, nm_pack_check_math and
 * nm_pack_check_lighting) run over the instances of the view: give them a
 * populated one from this view's own cells, then restore the gameplay selection. */
static int practice_render_load_populated(const nm_player_t *const *players,unsigned count,const char **stage) {
    int16_t depth[44];uint8_t order[44],level[44];
    nm_memcpy(depth,practice.intro.detail_depth,sizeof(depth));
    nm_memcpy(order,practice.intro.detail_order,sizeof(order));
    nm_memcpy(level,practice.intro.detail_level,sizeof(level));
    const int loaded=nm_race_detail_step(
            practice.intro.view,&practice.intro.cells,players,count,
            practice.intro.detail_depth,practice.intro.detail_order,practice.intro.detail_level) &&
        nm_practice_render_load(&practice,practice_root,stage);
    nm_memcpy(practice.intro.detail_depth,depth,sizeof(depth));
    nm_memcpy(practice.intro.detail_order,order,sizeof(order));
    nm_memcpy(practice.intro.detail_level,level,sizeof(level));
    return loaded;
}

const char *nm_practice_root(void) { return practice.failed ? NULL : practice_root; }
/* The game start's two iterations on the live scene (gameplay/select_mode.h): the pool as they leave it. */
int nm_practice_game_start(int scene,uint32_t counter) {
    if(scene==NM_CREDIT_CINEMATIC)
        return nm_frontend_game_start(&practice.intro,practice.math,&practice.contact,&practice.road,practice.intro_paths,&practice_pool);
#ifdef NM_RACE_VIEW_DATA
    if(scene==NM_CREDIT_DEMO) {
        if(!nm_demo_game_start(&practice.race,practice.math,&nm_intro_speeds,&practice.contact,&practice.road,
            practice.intro_paths,&practice.marks,practice.road.profile_count,counter+1u,
            nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count)) return 0;
        nm_frontend_pool_from_race(&practice_pool,&practice.race,&practice.intro.cells);
    }
#else
    (void)counter;
#endif
    return 1;
}

/* gameplay/results.c: the results' last iteration has run (phase 29 follows, in the name mode). */
void nm_practice_game_over(void) {
    nm_frontend_pool_from_game(&practice_pool,&practice.race,&practice.intro.cells,
        practice.results.after_podium ? &practice.intro : NULL);
    nm_frontend_counter_set(practice.race.counter_base+practice.race.ticks+practice.results.podium_ticks+practice.results.ticks-1u);
}

static int practice_race_load(const char *stage);
int nm_practice_restart_from_title(void) {
    const char *stage="title restart";
    if(!practice_root || practice.failed) return 0;
    practice.last_us=practice.update_max_us=practice.frame_window_us=0;practice.frame_clock=(nm_frame_clock_t){0};
    practice.intervals=practice.frame_tenths_ms=0;practice.inspect_paused=0;
    practice.work_last_us=practice.work_window_us=practice.render_window_us=0;
    practice.work_tenths_ms=practice.render_tenths_ms=practice.late_frames=0;practice.late_flip=practice.late_vblank=0;
    practice.attract_active=0;
    stage="race transmission";
    practice.transmission=PRACTICE_TRANSMISSION;
#if !defined(NATIVE_RACE_SCRIPT) && !defined(NATIVE_DRIVING_TEST)
    {
        enj_ctrlr_state_t **controllers=enj_ctrl_get_states();
        for(size_t i=0;i<enj_ctrl_states_length();++i) if(controllers[i]) {
            if(PRACTICE_TRANSMISSION_CHORD(controllers[i])) practice.transmission^=1u;
            break;
        }
    }
#endif
    /* From the title: the course chosen last (the select's, else NATIVE_COURSE's). */
    nm_sound_command(NM_MUSIC_GAME_START); /* The credit's game start (phase 12), which no select sent. */
    return practice_race_load(stage);
fail:
    printf("FAIL: native title restart at %s\n",stage);
    nm_practice_render_release();return 0;
}

/* The select's race entry (gameplay/select_mode.c): the chosen transmission 501464 and course 501460 (raced
 * when this build carries its data), and the start state the front end carried. */
int nm_practice_race_from_select(unsigned course,unsigned transmission) {
    if(!practice_root || practice.failed) return 0;
    practice.last_us=practice.update_max_us=practice.frame_window_us=0;practice.frame_clock=(nm_frame_clock_t){0};
    practice.intervals=practice.frame_tenths_ms=0;practice.inspect_paused=0;
    practice.work_last_us=practice.work_window_us=practice.render_window_us=0;
    practice.work_tenths_ms=practice.render_tenths_ms=practice.late_frames=0;practice.late_flip=practice.late_vblank=0;
    practice.attract_active=0;
    practice.transmission=transmission&1u;
    /* The course raced: the chosen one when this build carries its data (select_mode.c NATIVE_COURSES) and its
     * race-start and podium tables, else course 0. */
    const unsigned raced=nm_select_course_raced(course);
    if(!nm_practice_course_select(raced)) {
        printf("Native select: course %u has no race-start or podium tables in this build; racing course 0\n",raced);
        if(!nm_practice_course_select(0)) return 0;
    }
    return practice_race_load("select race entry");
}

static int practice_race_load(const char *stage) {
    const nm_player_t *players[44];
    /* The pad's shifter values must be the gears the loaded table 234abc names. */
    if(practice.transmission && !nm_shifter_check(practice.math->gear_selector)) goto fail;
    printf("Native race transmission: %s, course %u\n",practice.transmission ? "manual" : "automatic",practice_race_course);
    stage="race course packs";
    practice_attract_ready=0; /* a preloaded attract scene is replaced */
    if(!practice_course_load(practice_race_course)) goto fail;
    practice_attract_course=practice_race_course; /* 501460 stays the raced course: the attract after the game */
    stage="race session";
    /* Through the select: the start state the scenes before carried (frontend.h). */
    uint32_t counter_base=0;
    const int selected=nm_select_race_state(&counter_base);
    if(!practice_reset_session(selected && practice_pool.raced ? &practice_pool.player : NULL)) goto fail;
    if(selected && !nm_frontend_race_entry(&practice.race,&practice_pool,counter_base)) goto fail;
    stage="race initial follow-view/detail selection";
    if(!practice_race_detail() || (selected && !nm_frontend_race_guided(&practice.race,practice.intro.view,&practice_pool))) goto fail;
    stage="race presentation";
    for(unsigned i=0;i<practice.race.count;++i) players[i]=&practice.race.cars[i].car;
    if(!practice_render_load_populated(players,practice.race.count,&stage)) goto fail;
    stage="race HUD";
    if(!nm_hud_render_load(practice_root)) goto fail; /* HUD: resident sheets for this race */
    stage="podium screen";
    if(!nm_podium_render_load(practice_root)) goto fail; /* podium_render.h: resident, banks 60..61 at the cinematic */
    /* The rolling start's fade and race music (phase 19) at the scene's loading pause. */
    nm_sound_command(NM_MUSIC_FADE);
    nm_sound_command(nm_music_race[practice_music_select]);
    nm_sound_settle();
    nm_heap_report("after the race scene load");
    enj_mode_set(&practice_mode);return 1;
fail:
    printf("FAIL: native race start at %s\n",stage);
    nm_practice_render_release();return 0;
}

/* The attract scene of practice_attract_course: its course packs, the cinematic's start and the presentation; in
 * the title's frames (nm_practice_attract_preload: scene_loader.h, the title draws on while it loads) or at once
 * (nm_practice_attract_start without a preload: the boot, the attract after a game). */
static const char *practice_attract_stage="attract";
static int practice_attract_prepare(void *unused) {
    (void)unused;
    const nm_player_t *players[44];
    practice_attract_ready=0;
    practice_attract_stage="attract course packs"; /* The loop's course (include/attract.h); one this build cannot show: course 0. */
    if(practice_attract_course>2 || !(NM_ATTRACT_COURSES>>practice_attract_course&1u) ||
       !nm_attract_courses[practice_attract_course].stages) practice_attract_course=0;
    if(!practice_course_load(practice_attract_course)) return 0;
    practice_attract_stage="attract restart";
    if(!practice_reset_attract()) return 0;
    practice_attract_stage="attract presentation";
    for(unsigned i=0;i<practice.intro.count;++i) players[i]=practice.intro.cars+i;
    if(!practice_render_load_populated(players,practice.intro.count,&practice_attract_stage)) return 0;
    nm_heap_report("after the attract scene load");
    practice_attract_ready=1;return 1;
}

/* The title's start (gameplay/title_mode.c): the next attract scene loads in its frames. Its palettes wait for the
 * title's release (nm_practice_attract_settle). 0: not started (it then loads at the attract's start). */
int nm_practice_attract_preload(void) {
    if(!practice_root || practice.failed || practice_attract_ready || nm_loader_busy()) return 0;
    nm_palette_ram_defer();
    if(nm_loader_start(practice_attract_prepare,NULL,"attract")) return 1;
    (void)nm_palette_ram_flush();return 0;
}
int nm_practice_attract_loading(void) { return nm_loader_busy(); }
/* After the title's release: the rest of a preload at once, its palettes stored. */
void nm_practice_attract_settle(void) {
    if(nm_loader_busy()) (void)nm_loader_finish();
    (void)nm_palette_ram_flush();
}
/* The title left for another screen (a credit: the select, the settings): the preload ends as above and is given
 * up, since that screen writes palette RAM and the race load replaces the packs: the attract loads again. */
void nm_practice_attract_abandon(void) {
    nm_practice_attract_settle();
    nm_practice_render_release(); /* what it loaded of the renderer goes (released already at the attract's end) */
    practice_attract_ready=0;
}

int nm_practice_attract_start(void) {
    if(!practice_root || practice.failed) return 0;
#ifdef NM_ATTRACT_LOOPS
    /* make NATIVE_ATTRACT_LOOPS=N (a measurement build): N cinematics with their titles, then the run ends. */
    static unsigned starts;
    if(++starts>NM_ATTRACT_LOOPS) {
        nm_practice_attract_settle();nm_loader_report();
        printf("Native attract: %u loops done; run ends\n",(unsigned)NM_ATTRACT_LOOPS);
        enj_state_flag_shutdown(NULL);return 1;
    }
#endif
    practice.last_us=practice.update_max_us=practice.frame_window_us=0;practice.frame_clock=(nm_frame_clock_t){0};
    practice.intervals=practice.frame_tenths_ms=0;practice.inspect_paused=0;
    practice.work_last_us=practice.work_window_us=practice.render_window_us=0;
    practice.work_tenths_ms=practice.render_tenths_ms=practice.late_frames=0;practice.late_flip=practice.late_vblank=0;
    const int preloaded=nm_loader_busy() || practice_attract_ready;
    nm_practice_attract_settle();
    if(preloaded) nm_loader_report();
    if(preloaded && !practice_attract_ready) nm_practice_render_release(); /* a failed preload: again from scratch */
    if(!practice_attract_ready && !practice_attract_prepare(NULL)) goto fail;
    practice_attract_ready=0;
    /* The cinematic's first tick is the iteration after phase 2's (5010b0 = object_clock + ticks). */
    practice.intro.object_clock=nm_frontend_counter()+1u;practice_credit_pending=0;
    practice.intro.phase_clock=practice.intro.object_clock-3u; /* 503690/503692 (vr_demo.h; the first loop's 0) */
    /* The 2D layer of phase 2 (5010b0 = the counter now) and of the scene start (phase 3's first iteration). */
    nm_attract_2d_phase2(practice.course,nm_game_records(practice.course),practice_pool.view);
    nm_attract_2d_cinematic(3,practice.intro.object_clock,0);
    enj_mode_set(&practice_mode);return 1;
fail:
    printf("FAIL: native attract start at %s\n",practice_attract_stage);
    nm_practice_render_release();return 0;
}

/* The cinematic attract's ticks, outlined so that practice_update keeps the race's code and its placement
 * (the front end's credit and counter live here). 0: the mode changed (the updater returns), 1: draw. */
static __attribute__((noinline)) int practice_attract_update(uint32_t elapsed_us) {
    if(practice.intro.ticks>=(DAYTONA_RACE_TICK_LIMIT ? DAYTONA_RACE_TICK_LIMIT : attract_tick_limit)) {
        /* Phases 6 (1 iteration) and 7 (the V.R. demo, 834): practice_demo_start. A build without the race's
         * view presets counts them with phase 8 (1) instead. The demo's end steps the course (2058): the title
         * is followed by the next course's cinematic. */
        if(DAYTONA_RACE_TICK_LIMIT) { nm_practice_render_release();enj_state_flag_shutdown(NULL);return 0; }
        if(practice_demo_start() || practice.failed) return 0;
        nm_frontend_pool_from_intro(&practice_pool,&practice.intro,&practice.road);
        nm_frontend_counter_set(practice.intro.object_clock+practice.intro.ticks+NM_ATTRACT_DEMO_ITERATIONS);
        practice_attract_course=nm_attract_next_course(practice.course);
        nm_practice_render_release();
        if(!nm_title_mode_start()) enj_state_flag_shutdown(NULL);
        return 0;
    }
    uint32_t dt=elapsed_us;
    if(dt>4*NM_PRACTICE_TICK_US) { practice.state.dropped_us+=dt-4*NM_PRACTICE_TICK_US;dt=4*NM_PRACTICE_TICK_US; }
    practice.state.remainder_us+=dt;
    int advanced=1;
    while(advanced && practice.state.remainder_us>=NM_PRACTICE_TICK_US && practice.intro.ticks<attract_tick_limit) {
        /* The scene start is phase 3's first iteration, where 2b04/1108 do not run: every tick may take the
         * credit but the ranking overlay's entry (phase 4, 1d8c has no credit check). */
        const unsigned phase=nm_attract_phase(practice.intro.ticks);
        const int credit=practice_credit_pending && phase!=4;
        practice.intro.flags=UINT32_C(1)<<phase;
        advanced=nm_intro_scene_step(&practice.intro,practice.math,&practice.contact,&practice.road,practice.intro_paths);
        if(advanced) advanced=nm_intro_scene_detail(&practice.intro,practice.math,&practice.scenery);
        if(advanced) nm_props_intro_step(&practice.intro,practice.math,&practice.contact);
        if(advanced) nm_attract_2d_cinematic(phase,practice.intro.object_clock+practice.intro.ticks,
            practice.intro.ticks==NM_ATTRACT_TICKS);
        if(advanced && practice.intro.window_action) { /* 6fe0 / 7008 in a tunnel of Advanced, as the race's */
            (void)nm_practice_render_tunnel(&practice,practice.intro.window_action==NM_REFLECTION_TUNNEL,practice.intro.window_others);
            practice.intro.window_action=0;
        }
        practice.state.remainder_us-=NM_PRACTICE_TICK_US;
        if(advanced && credit) {
            /* That tick was the credit's iteration (an ordinary cinematic tick); the game start and the
             * select follow (phases 12, 17, 18) with this scene's pool. */
            practice_credit_pending=0;
            if(!nm_select_credit(practice.intro.object_clock+practice.intro.ticks)) enj_state_flag_shutdown(NULL);
            return 0;
        }
    }
    NM_COPY(&practice.state.car,&practice.intro.cars[0]);practice.state.ticks=practice.intro.ticks;
    if(!advanced) {
        practice.failed=1;
        printf("Native attract rejected tick %lu\n",(unsigned long)practice.state.ticks);
    }
    (void)nm_attract_2d_render_enqueue();
    return 1;
}

#ifdef NATIVE_STAGE_TRACE
/* Diagnostic (make NATIVE_STAGE_TRACE=<race tick>): from that race tick until
 * 40 updates after the podium scene started, one line per step of the updater
 * and of the frame's list callbacks, so that a freeze names its place: the
 * last line printed is the last thing that completed. Printing slows these
 * frames. Not a game build. */
static unsigned stage_trace_frames,stage_trace_after;
static void stage_trace_mark(void *text) { if(stage_trace_frames) { puts((const char *)text); fflush(stdout); } }
#define STAGE_TRACE(text) stage_trace_mark((void *)(text))
#define STAGE_TRACE_LIST(list,text) enj_render_list_add((list),stage_trace_mark,(void *)(text))
#else
#define STAGE_TRACE(text) ((void)0)
#define STAGE_TRACE_LIST(list,text) ((void)0)
#endif
#ifdef NM_DIAG_RENDER_STATS
/* make NATIVE_DIAG_RENDER_STATS=1 (measurement, not a game build): the PVR's
 * render time of every frame of the run. The "Practice PVR" line covers the
 * updater intervals 5..600 only, the first ten seconds (the rolling start);
 * this adds the same reading (pvr_get_stats, the render that completed last,
 * once per updater interval) for the whole race and the podium, a histogram
 * and the worst frames with their race tick. Printed after the run. */
enum { DIAG_START,DIAG_RACE,DIAG_PODIUM,DIAG_ATTRACT,DIAG_SEGMENTS };
static struct { uint32_t n,render_max,register_max,over[3]; uint64_t render_sum,register_sum; } diag_render[DIAG_SEGMENTS];
static uint32_t diag_render_histogram[DIAG_SEGMENTS][26];
static struct { uint32_t us,tick,segment; } diag_render_worst[12];
static void practice_diag_render(const pvr_stats_t *stats) {
    if(practice.intervals<5) return;
    const unsigned segment=practice.attract_active ? (practice.podium.phase ? DIAG_PODIUM : DIAG_ATTRACT) :
        practice.intervals<=600 ? DIAG_START : DIAG_RACE;
    const uint32_t render=(uint32_t)(stats->rnd_last_time/1000),registration=(uint32_t)(stats->reg_last_time/1000);
    ++diag_render[segment].n;
    diag_render[segment].render_sum+=render;diag_render[segment].register_sum+=registration;
    if(render>diag_render[segment].render_max) diag_render[segment].render_max=render;
    if(registration>diag_render[segment].register_max) diag_render[segment].register_max=registration;
    diag_render[segment].over[0]+=render>12000;diag_render[segment].over[1]+=render>14000;diag_render[segment].over[2]+=render>16667;
    ++diag_render_histogram[segment][render/1000<25 ? render/1000 : 25];
    unsigned at=12;
    while(at && render>diag_render_worst[at-1].us) --at;
    /* One entry per race tick: a late frame reads the same render twice. */
    for(unsigned i=0;i<at;++i) if(diag_render_worst[i].tick==practice.race.ticks && diag_render_worst[i].segment==segment) return;
    if(at<12) {
        for(unsigned i=11;i>at;--i) diag_render_worst[i]=diag_render_worst[i-1];
        diag_render_worst[at].us=render;diag_render_worst[at].tick=practice.race.ticks;diag_render_worst[at].segment=segment;
    }
}
/* The frames that drew the pit crew (practice.crew_drawn grew in their updater): the updater's work (updater
 * start to enqueue end) and the registration of the same frame, their sum, and a histogram of the sum (the
 * frame is on time while the sum stays below a vblank period). */
static struct { uint32_t n,upd_max,reg_max,sum_max,over; uint64_t upd_sum,reg_sum; uint32_t histogram[26]; } diag_crew;
enum { DIAG_CREW_BUCKET_TICKS=64, DIAG_CREW_BUCKETS=160 };
static struct { uint32_t n,sum_max,over; uint64_t sum; } diag_crew_bucket[DIAG_CREW_BUCKETS]; /* by race tick */
static uint32_t diag_crew_seen,diag_crew_tick;
static int diag_crew_previous;
static void practice_diag_crew(const pvr_stats_t *stats) {
    /* Called at the start of updater k: work_last_us is updater k-1's, reg_last_time the registration that followed. */
    if(diag_crew_previous && practice.intervals>64) {
        const uint32_t upd=(uint32_t)practice.work_last_us,reg=(uint32_t)(stats->reg_last_time/1000),sum=upd+reg;
        ++diag_crew.n;diag_crew.upd_sum+=upd;diag_crew.reg_sum+=reg;
        if(upd>diag_crew.upd_max) diag_crew.upd_max=upd;
        if(reg>diag_crew.reg_max) diag_crew.reg_max=reg;
        if(sum>diag_crew.sum_max) diag_crew.sum_max=sum;
        diag_crew.over+=sum>16683;
        ++diag_crew.histogram[sum/1000<25 ? sum/1000 : 25];
        const uint32_t b=diag_crew_tick/DIAG_CREW_BUCKET_TICKS;
        if(b<DIAG_CREW_BUCKETS) {
            ++diag_crew_bucket[b].n;diag_crew_bucket[b].sum+=sum;diag_crew_bucket[b].over+=sum>16683;
            if(sum>diag_crew_bucket[b].sum_max) diag_crew_bucket[b].sum_max=sum;
        }
    }
}
static void practice_diag_crew_mark(void) {
    diag_crew_previous=practice.crew_drawn!=diag_crew_seen;diag_crew_seen=practice.crew_drawn;
    diag_crew_tick=practice.race.ticks;
}
static void practice_diag_render_print(void) {
    if(diag_crew.n) {
        printf("DIAG pit crew frames: n=%lu updater avg/max=%lu/%lu us registration avg/max=%lu/%lu us sum max=%lu us; "
            "sum over 16.683 ms=%lu\n",(unsigned long)diag_crew.n,(unsigned long)(diag_crew.upd_sum/diag_crew.n),
            (unsigned long)diag_crew.upd_max,(unsigned long)(diag_crew.reg_sum/diag_crew.n),(unsigned long)diag_crew.reg_max,
            (unsigned long)diag_crew.sum_max,(unsigned long)diag_crew.over);
        printf("DIAG pit crew frames, updater + registration histogram, frames per millisecond 0..24 and 25+:");
        for(unsigned b=0;b<26;++b) printf(" %lu",(unsigned long)diag_crew.histogram[b]);
        putchar('\n');
        printf("DIAG pit crew frames by race tick (from tick: frames, updater + registration avg/max us, over 16.683 ms):");
        for(unsigned b=0;b<DIAG_CREW_BUCKETS;++b) if(diag_crew_bucket[b].n)
            printf(" %lu:%lu,%lu/%lu,%lu",(unsigned long)(b*DIAG_CREW_BUCKET_TICKS),(unsigned long)diag_crew_bucket[b].n,
                (unsigned long)(diag_crew_bucket[b].sum/diag_crew_bucket[b].n),(unsigned long)diag_crew_bucket[b].sum_max,
                (unsigned long)diag_crew_bucket[b].over);
        putchar('\n');
    }
    static const char *const names[DIAG_SEGMENTS]={"start (intervals 5..600)","race after it","podium","attract"};
    for(unsigned s=0;s<DIAG_SEGMENTS;++s) {
        if(!diag_render[s].n) continue;
        printf("DIAG PVR render, %s: n=%lu render avg/max=%lu/%lu us register avg/max=%lu/%lu us; render over 12 ms=%lu, over 14 ms=%lu, over 16.667 ms=%lu\n",
            names[s],(unsigned long)diag_render[s].n,(unsigned long)(diag_render[s].render_sum/diag_render[s].n),(unsigned long)diag_render[s].render_max,
            (unsigned long)(diag_render[s].register_sum/diag_render[s].n),(unsigned long)diag_render[s].register_max,
            (unsigned long)diag_render[s].over[0],(unsigned long)diag_render[s].over[1],(unsigned long)diag_render[s].over[2]);
        printf("DIAG PVR render histogram, %s, frames per millisecond 0..24 and 25+:",names[s]);
        for(unsigned b=0;b<26;++b) printf(" %lu",(unsigned long)diag_render_histogram[s][b]);
        putchar('\n');
    }
    printf("DIAG PVR render, worst frames (us@race tick, s start r race p podium a attract):");
    for(unsigned i=0;i<12 && diag_render_worst[i].us;++i)
        printf(" %lu@%lu%c",(unsigned long)diag_render_worst[i].us,(unsigned long)diag_render_worst[i].tick,"srpa"[diag_render_worst[i].segment]);
    putchar('\n');
}
#endif
static void practice_update(void *unused) {
    (void)unused;
    nm_sound_update();
    if (enj_state_get()->flags.shut_down || practice.failed) {
        nm_practice_render_release(); enj_state_flag_shutdown(NULL); return;
    }
    const uint64_t now = timer_ns_gettime64()/1000;
#if defined(NATIVE_PROFILE) || defined(NATIVE_FRAME_PROFILE)
    uint64_t simulation_done=now;
#endif
    const uint64_t elapsed = practice.last_us ? now-practice.last_us : 0;
    if (practice.last_us) {
        practice.interval_sum_us+=elapsed; ++practice.intervals;
        if (elapsed>practice.interval_max_us) practice.interval_max_us=elapsed;
#ifdef NATIVE_FRAME_PROFILE
        if(practice.intervals>64 && !practice.inspect_paused) {
            ++frame_budget_samples;
            frame_over_18ms+=elapsed>18000;
            frame_over_25ms+=elapsed>25000;
            if(elapsed>frame_steady_max_us) frame_steady_max_us=elapsed;
        }
#endif
        /* Whole update-to-update interval, including callbacks/input/waits.
         * Refresh every16 frames; integer tenths avoid float printf/doubles. */
        practice.frame_window_us+=elapsed;
        /* enDjinn runs this updater, waits for the PVR, then submits the lists
         * (scene begin..finish = registration time). The budget indicator is
         * updater + registration of the previous frame, not the vblank-locked
         * interval above, which reads 16.7 whenever no frame is late. */
        pvr_stats_t budget;
        if(!pvr_get_stats(&budget)) {
            practice.work_window_us+=practice.work_last_us+budget.reg_last_time/1000;
            practice.render_window_us+=budget.rnd_last_time/1000;
            if(practice.late_flip) {
                const size_t flips=budget.frame_count-practice.late_flip,vblanks=budget.vbl_count-practice.late_vblank;
                if(vblanks>flips) practice.late_frames+=(unsigned)(vblanks-flips);
            }
            practice.late_flip=budget.frame_count;practice.late_vblank=budget.vbl_count;
#ifdef NM_DIAG_RENDER_STATS
            practice_diag_render(&budget);
            practice_diag_crew(&budget);
#endif
        }
        if(!(practice.intervals&15u)) {
            practice.frame_tenths_ms=(unsigned)(practice.frame_window_us/1600u);
            practice.work_tenths_ms=(unsigned)(practice.work_window_us/1600u);
            practice.render_tenths_ms=(unsigned)(practice.render_window_us/1600u);
            practice.frame_window_us=practice.work_window_us=practice.render_window_us=0;
        }
    }
    practice.last_us = now;
    /* The simulation's time: one tick per displayed frame, plus the vblanks a late frame missed (frame_clock.h).
     * The wall-clock interval above is a diagnostic only. */
    const uint32_t frame_us=nm_frame_ticks(&practice.frame_clock)*NM_PRACTICE_TICK_US;
    if (practice.inspect_paused) {
        /* Keep latching input/rendering, but don't advance or reset gameplay.
         * Refresh last_us above so resuming never catches up paused time.
         * A partial enqueue failure is released by the NEXT updater. */
        if (!nm_practice_render_enqueue(&practice)) practice.failed=1;
        return;
    }
#ifdef NATIVE_FRAME_PROFILE
    if(practice.intervals>64) {
        pvr_stats_t stats;
        if(!pvr_get_stats(&stats)) {
            if(frame_last_flip) {
                frame_flips+=stats.frame_count-frame_last_flip;
                frame_vblanks+=stats.vbl_count-frame_last_vblank;
                if(stats.frame_count!=frame_last_flip) {
                    ++frame_flip_samples;frame_flip_sum_ns+=stats.frame_last_time;
                    frame_flip_over_20ms+=stats.frame_last_time>20000000;
                    if(stats.frame_last_time>frame_flip_max_ns) frame_flip_max_ns=stats.frame_last_time;
                }
            }
            frame_last_flip=stats.frame_count;frame_last_vblank=stats.vbl_count;
        }
    }
#endif
    if (practice.intervals>=5 && practice.intervals<=600) {
        pvr_stats_t stats;
        if (!pvr_get_stats(&stats)) {
            practice.pvr_register_sum+=stats.reg_last_time; practice.pvr_render_sum+=stats.rnd_last_time;
            if (stats.reg_last_time>practice.pvr_register_max) practice.pvr_register_max=stats.reg_last_time;
            if (stats.rnd_last_time>practice.pvr_render_max) practice.pvr_render_max=stats.rnd_last_time;
            ++practice.pvr_samples;
            if (practice.intervals==600) {
                /* Console/fileserver calls mask interrupts; print after exit,
                 * not inside the measured gameplay window. */
                practice.sampled_stats=stats; practice.sampled_memory=mallinfo();
                practice.sampled_vram=pvr_mem_available();
                /* Same ceiling as KOS mm_sbrk; excludes its kernel stack reserve.
                 * mallinfo.free alone omits RAM not yet given to malloc. */
                const uintptr_t heap_end=(uintptr_t)mm_sbrk(0);
                const uintptr_t heap_limit=_arch_mem_top-THD_KERNEL_STACK_SIZE;
                practice.sampled_heap_headroom=heap_end<heap_limit ? heap_limit-heap_end : 0;
            }
        }
    }
#ifdef NATIVE_DRIVING_TEST
    if (practice.failed || practice.state.ticks>=NM_PRACTICE_SCRIPT_TICKS || practice.intervals>=3600) {
        if (practice.state.ticks!=NM_PRACTICE_SCRIPT_TICKS) practice.failed=1;
        nm_practice_render_release(); enj_state_flag_shutdown(NULL); return;
    }
#endif
#ifdef NATIVE_RACE_SCRIPT
    /* Scripted run: the pad is not read here, so START/A/B do nothing; each
     * race tick takes its controls from the script below. Bounded run length. */
    nm_controls_t controls = {0};
    if(!practice.failed && practice.intervals>(NM_RACE_SCRIPT_TICKS+4000u)*5u/4u) {
        practice.failed=1;
        puts("FAIL: scripted race watchdog; the flow did not reach the result handoff");
    }
#elif !defined(NATIVE_DRIVING_TEST)
    nm_controls_t controls = {0};
    enj_ctrlr_state_t **controllers = enj_ctrl_get_states();
    for (size_t i=0; i<enj_ctrl_states_length(); ++i) if (controllers[i]) {
        const enj_ctrlr_state_t *c = controllers[i];
        /* The stick through the steering table (include/steer_curve.h: dead zone, power curve, full lock at the
         * ends) into the existing arcade ADC decoder; the triggers over their full ranges. The decoder and the
         * race after it stay the arcade's. */
        controls = nm_controls_decode(nm_steer_table_adc(practice_steer_table,c->joyx),
            (uint8_t)(56+(unsigned)c->rtrigger*128/255),
            (uint8_t)(56+(unsigned)c->ltrigger*96/255),
            practice.controller_last.accel,practice.controller_last.brake);
        ++practice.controller_samples;practice.controller_last=controls;
        if(practice.attract_active && !practice.podium.phase &&
           (c->button.START==ENJ_BUTTON_DOWN_THIS_FRAME || c->button.A==ENJ_BUTTON_DOWN_THIS_FRAME)) {
            /* Both triggers and START: the settings screen; else the credit (1108 -> 2818), taken by the next
             * cinematic tick (gameplay/select_mode.h). */
            if(nm_select_attract_button(c->button.START==ENJ_BUTTON_DOWN_THIS_FRAME,c->ltrigger,c->rtrigger)) return;
            practice_credit_pending=1;
        }
        /* The race, the podium and the results (the attract's START is the credit above).
         * Latched until a tick consumes it (frames carry 0..4 ticks). */
        if((!practice.attract_active || practice.podium.phase) && c->button.START==ENJ_BUTTON_DOWN_THIS_FRAME) practice.start_pending=1;
        practice.results.start_held=(uint8_t)(c->button.START&1u);
        /* Manual: one gear up or down per press, latched like START. */
        if(!practice.attract_active && practice.transmission) {
            if(PRACTICE_SHIFT_UP(c)) practice.shift_pending=1;
            else if(PRACTICE_SHIFT_DOWN(c)) practice.shift_pending=-1;
        }
        if(!practice.attract_active) practice_view_buttons(c);
        /* B alone: the session reset; START held + B is the inspector's toggle (code/render/scene/practice.c). */
        if (!practice.attract_active && c->button.B == ENJ_BUTTON_DOWN_THIS_FRAME && !(c->button.START&1)) {
            practice.failed = !practice_reset_session(NULL);
            if(!practice.failed) practice.failed=!practice_race_detail();
        }
        break;
    }
#endif
#ifdef NATIVE_FRAME_COMPARE
    /* Diagnostic only: identical native simulation ticks in both renderers.
     * Hold each capture pose across both framebuffers before reading display.
     * This build's timings are not real-time gameplay measurements. */
    static unsigned capture_tick=600, capture_hold;
    uint32_t elapsed_us=NM_PRACTICE_TICK_US;
    if(practice.state.ticks==capture_tick && capture_tick<=1800) {
        elapsed_us=0;
        if(++capture_hold==4) {
            int ok=pvr_wait_ready()==0 && pvr_wait_render_done()==0;
            uint8_t *pixels=NULL;
            const size_t bytes=ok ? vid_screen_shot_data(&pixels) : 0;
            char name[64]; snprintf(name,sizeof(name),"/pc/frame-%04u.rgb",capture_tick);
            FILE *file=bytes==640u*480u*3u ? fopen(name,"wbx") : NULL;
            ok=file!=NULL;
            if(file) {
                if(fwrite(pixels,1,bytes,file)!=bytes) ok=0;
                if(fclose(file)) ok=0;
            }
            free(pixels);
            printf("Native deterministic capture tick=%u digest=%08lx bytes=%lu %s\n",
                capture_tick,(unsigned long)nm_practice_script_digest(&practice.state),
                (unsigned long)bytes,ok ? "PASS" : "FAIL");
            if(!ok) { practice.failed=1; nm_practice_render_release(); enj_state_flag_shutdown(NULL); return; }
            capture_tick+=600; capture_hold=0;
        }
    }
    (void)frame_us;
#else
    const uint32_t elapsed_us=frame_us;
#endif
    /* 03b84/03f3c: a top-three finish enters the podium on the updater after
     * the final phase24 draw. Its timer gates the shared cinematic scene; the
     * race scene keeps rank/clock/lap for the results that follow. */
#ifdef NATIVE_PODIUM_TEST
    if(practice.race.phase==22 && practice.race.ticks>=300 && !practice.attract_active) {
        practice.race.phase=27;
        puts("Native podium diagnostic: injected phase27 at300; NOT a won race or finish coverage");
    }
#endif
#ifdef NATIVE_STAGE_TRACE
    if(!stage_trace_after && practice.race.phase>=20 && practice.race.phase<=28 && practice.race.ticks>=NATIVE_STAGE_TRACE) stage_trace_frames=1;
    if(practice.podium.phase && practice.attract_active && ++stage_trace_after>40) stage_trace_frames=0;
    if(stage_trace_frames) {
        printf("TRACE update: race phase %u active %u ticks %lu finish timer %ld; podium phase %u timer %ld; attract %d; remainder %lu + %lu us\n",
            (unsigned)practice.race.phase,(unsigned)practice.race.active_phase,(unsigned long)practice.race.ticks,
            (long)practice.race.finish.timer,(unsigned)practice.podium.phase,(long)practice.podium.timer,practice.attract_active,
            (unsigned long)practice.state.remainder_us,(unsigned long)elapsed_us);
        fflush(stdout);
    }
#endif
    if(practice.race.phase==27 && !practice.podium.phase && !practice.failed) {
        practice.podium.phase=27;
        nm_sound_command(nm_music_podium[practice_music_select]); /* Object b948 on the phase-27 tick. */
        nm_sound_command(NM_VOICE_YOU_MADE_IT);
    }
    if(practice.podium.phase) {
        uint32_t dt=elapsed_us;
        if(dt>4*NM_PRACTICE_TICK_US) { practice.state.dropped_us+=dt-4*NM_PRACTICE_TICK_US;dt=4*NM_PRACTICE_TICK_US; }
        practice.state.remainder_us+=dt;
        int advanced=1;
        while(advanced && practice.podium.phase!=25 && practice.state.remainder_us>=NM_PRACTICE_TICK_US) {
            /* 53e584 bit 4: the frame's press, the frame's first tick. The podium's actors read it. */
            const uint8_t start=practice.start_pending;practice.start_pending=0;
            advanced=nm_podium_step(&practice.podium);
            STAGE_TRACE(practice.podium.spawn ? "TRACE podium tick: spawn, starting the scene" : practice.podium.entered ? "TRACE podium tick: entry" : "TRACE podium tick");
            /* The entry tick only arms the graph (spawn updaters cf74/cfc0): the
             * race scene stays one more tick; spawn poses follow on the next. */
            if(advanced && practice.podium.spawn) {
                /* The course's podium graph 2865a70[501460] (podium_data.h nm_podium_courses). */
                const nm_podium_course_t *podium=practice.course<3 ? nm_podium_courses+practice.course : NULL;
                advanced=podium && podium->stages && nm_intro_scene_start_stages(&practice.intro,podium->stages,
                    podium->count,1u<<28,podium->overrun,practice.math,&practice.road,practice.intro_paths);
                if(advanced) nm_frontend_podium_start(&practice.intro,&practice.race);
                practice.intro.object_clock=practice.race.ticks+NM_PODIUM_CLOCK_OFFSET; /* 5010b0 - podium ticks */
                practice.intro.phase_clock=practice.race.counter_base+practice.intro.object_clock+1u; /* 503690/503692 */
                practice.attract_active=1;
                puts("Native phase27 ->28: podium cinematic; 2D overlay and audio pending");
                STAGE_TRACE(advanced ? "TRACE scene started" : "TRACE scene start refused");
            } else if(advanced && practice.podium.cinematic && !practice.podium.entered)
                advanced=nm_intro_scene_step(&practice.intro,practice.math,&practice.contact,&practice.road,practice.intro_paths);
            if(advanced && practice.podium.cinematic && !practice.podium.entered) {
                practice.intro.start_edge=start;practice.start_edges+=start;
                advanced=nm_intro_scene_detail(&practice.intro,practice.math,&practice.scenery);
                if(advanced) nm_props_intro_step(&practice.intro,practice.math,&practice.contact); /* the course's props */
            }
            STAGE_TRACE(advanced ? "TRACE podium tick done" : "TRACE podium tick refused");
            if(advanced && practice.podium.cleanup) {
                practice.attract_active=0;
                printf("Native phase28 cleanup: stage=%lu transitions=%lu scene-ticks=%lu pairs=%lu responses=%lu\n",
                    (unsigned long)practice.intro.stage,(unsigned long)practice.intro.transitions,
                    (unsigned long)practice.intro.ticks,(unsigned long)practice.intro.collision_pairs,
                    (unsigned long)practice.intro.collision_responses);
            }
            if(advanced) { practice.race.phase=practice.podium.phase;++practice.results.podium_ticks; }
            /* bc00 row 4 (timer 10): bit 3, bc60 queues 9e2d4f through 19bf4 (no gate). */
            if(advanced && practice.podium.phase==28 && practice.podium.timer==10) nm_sound_command(UINT32_C(0x9e2d4f));
#ifdef NATIVE_RACE_CHECKPOINTS
            if(advanced) { ++checkpoint_tick;practice_checkpoint(practice.podium.timer); }
#endif
            practice.state.remainder_us-=NM_PRACTICE_TICK_US;
        }
        if(!advanced) {
            practice.failed=1;
            printf("Native podium rejected timer %ld\n",(long)practice.podium.timer);
        }
        if(practice.podium.phase==25) { /* Results own the next updater; the pool held the podium. */
            practice.podium.phase=0;practice.results.after_podium=1;return;
        }
        if(practice.failed) return;
        /* The 2D screen and the photo (podium_render.h) over the cinematic and after it. */
        if(!nm_podium_render_enqueue(&practice.podium)) { practice.failed=1; return; }
        /* After cleanup the source has no 3D task enabled: no scene is queued. */
        if(!practice.podium.cinematic) return;
#if defined(NATIVE_PROFILE) || defined(NATIVE_FRAME_PROFILE)
        simulation_done=timer_ns_gettime64()/1000;
#endif
        goto render_frame;
    }
    if(practice.attract_active) {
        if(!practice_attract_update(elapsed_us)) return;
#if defined(NATIVE_PROFILE) || defined(NATIVE_FRAME_PROFILE)
        simulation_done=timer_ns_gettime64()/1000;
#endif
        goto render_frame;
    }
    /* Result entry occurs on the following updater, never during the final
     * phase24 draw. Builds without a result renderer stop at the handoff. */
    if(practice.race.phase==25 && !practice.failed && !practice_record_published) {
        /* Palette shading (render/materials/palette_banks.h): cars are not
         * drawn from here until the next scene load, so their palette banks
         * take the result glyph and label palettes before the first result
         * frame. Rendering only; no gameplay state is read or written. */
        if(!nm_result_materials_palettes_apply() || !nm_result_labels_palettes_apply()) {
            puts("FAIL: result palettes at the phase25 hand-off");practice.failed=1;
        }
        nm_race_record_t *records=nm_game_records(practice.course);
        const int result=nm_race_scene_record(&practice.race,records,0,
            &practice_record_selected,&practice_record_notification);
        if(result==2) nm_game_record_inserted(practice.course,practice_record_selected);
        practice_record_published=1;
        if(!result) practice.failed=1;
        practice.name_notification=practice_record_notification;
        practice.name_record=practice_record_selected<24 ? records+practice_record_selected : NULL;
        printf("Native phase25 record: result=%d selected=%u notification=%lu eligible=%lu time=%lu; course %u table (game save)\n",
            result,practice_record_selected,(unsigned long)practice_record_notification,
            (unsigned long)(practice.race.cars[0].car.checkpoints.finish_flags&2u),
            (unsigned long)practice.race.shared.clock,(unsigned)practice.course);
    }
#ifdef NATIVE_RACE_SCRIPT
    /* Scripted run ends at the result handoff: name entry and the title need
     * a pad. Same exit as the finite tests: back to the loader. */
    if(practice.race.phase==29 && practice.results.mode && !practice.failed) {
        puts("Native scripted race: result handoff (phase29) reached; returning to loader");
        nm_practice_render_release();enj_state_flag_shutdown(NULL);return;
    }
#endif
    if(nm_results_update(&practice,elapsed_us)) return; /* Checkpoint rows: nm_practice_results_tick. */
    if((DAYTONA_RACE_TICK_LIMIT && practice.race.ticks>=DAYTONA_RACE_TICK_LIMIT) ||
       practice.race.phase<20 || practice.race.phase>24 || practice.failed) { /* 20/21: race entry passes. */
        nm_practice_render_release();enj_state_flag_shutdown(NULL);return;
    }
    uint32_t dt=elapsed_us;
    if(dt>4*NM_PRACTICE_TICK_US) { practice.state.dropped_us+=dt-4*NM_PRACTICE_TICK_US;dt=4*NM_PRACTICE_TICK_US; }
    practice.state.remainder_us+=dt;
    nm_player_tick_input_t input={.active={.dynamics={.motion_scale=1}},.phase=22};
    input.active.controls=controls;input.award.difficulty=practice_difficulty;
    int advanced=1;
    while(advanced && (!DAYTONA_RACE_TICK_LIMIT || practice.race.ticks<DAYTONA_RACE_TICK_LIMIT) &&
          practice.state.remainder_us>=NM_PRACTICE_TICK_US) {
#ifdef NATIVE_RACE_FINISH_TEST
        if(practice.race.ticks==1800) {
            practice.race.cars[0].car.checkpoints.finish_flags|=1u;
            puts("Native finish diagnostic: injected completion at1800; not an actually completed course");
        }
#endif
#ifdef NATIVE_RACE_SCRIPT
        input.active.controls=(nm_controls_t){0};input.active.force_recovery=0;
        if(practice.race.ticks<NM_RACE_SCRIPT_TICKS) {
            const nm_race_script_tick_t *t=nm_race_script+practice.race.ticks;
            input.active.controls=(nm_controls_t){.steer=t->steer,.accel=t->accel,.brake=t->brake,
                .accel_high_edge=(uint8_t)(t->edges&1u),.brake_high_edge=(uint8_t)((t->edges>>1)&1u)};
            input.active.force_recovery=(uint8_t)((t->edges>>2)&1u); /* START edge, 53e584 bit4 */
        }
        /* Manual scripts: the shifter 53e120>>20&7 in edges bits 3-5 (neutral after the script). */
        input.active.dynamics.curve=practice.transmission;
        input.active.gear_selector=!practice.transmission ? 0 : practice.race.ticks<NM_RACE_SCRIPT_TICKS ?
            (uint8_t)((nm_race_script[practice.race.ticks].edges>>3)&7u) : (uint8_t)NM_SHIFTER_NEUTRAL;
#ifdef NM_RACE_SCRIPT_VIEWS
        /* The view buttons of the script (53e120 layout), from each listed tick on. NM_RACE_SCRIPT_VIEWS is a
         * macro of the script header (tests/trace_race_native.c --script-header); an enum would skip this block. */
        static unsigned view_event;
        if(!practice.race.ticks) {
            view_event=0;practice_view_held=0;
            printf("Native race script views: %u events\n",(unsigned)NM_RACE_SCRIPT_VIEWS);
        }
        while(view_event<NM_RACE_SCRIPT_VIEWS && nm_race_script_views[view_event].tick<=practice.race.ticks)
            practice_view_held=nm_race_script_views[view_event++].held;
#endif
#elif !defined(NATIVE_DRIVING_TEST)
        /* 53e584 bit4 lasts one loop iteration: the frame's press goes to its
         * first gameplay tick only (10a14 recovery and the slot machine). */
        input.active.force_recovery=practice.start_pending;practice.start_pending=0;
        /* The shifter 53e120 bits 20-22 (include/shifter.h): the frame's press goes to its first tick. */
        input.active.dynamics.curve=practice.transmission;
        input.active.gear_selector=practice.transmission ?
            nm_shifter_sequential(practice.race.cars[0].car.gear,practice.shift_pending) : 0;
        practice_shift_requests+=input.active.gear_selector!=0 && input.active.gear_selector!=NM_SHIFTER_NEUTRAL;
        practice.shift_pending=0;
        practice_view_held=practice_view_tick();
#endif
        practice.start_edges+=input.active.force_recovery;
        practice_reject_step="race scene step";
        /* 503690 = 5010b0 & 1 (5818, each frame): the crash end's lane value 41/42 (damage.h). */
        input.active.variant=(uint8_t)((practice.race.counter_base+practice.race.ticks)&1u);
        /* Race sound commands: audio/race_sound_hooks.h, one call per hook. */
        const uint32_t countdown=nm_race_sound_hook_before(&practice.race,practice.road.profile_count);
        const int result=nm_race_scene_step(&practice.race,practice.math,&nm_intro_speeds,&practice.contact,
            &practice.road,practice.intro_paths,&input,0,0,0,(practice.race.counter_base+practice.race.ticks)&3u,
            &practice.marks,practice.road.profile_count,nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count);
        if(result==2) break; /* Real phase handoff; do not invent finish/timeout driving. */
        advanced=result==1;
        if(advanced) nm_race_sound_hook_tick(&practice.race,countdown);
        if(advanced && practice.race.active_phase==23) {
            for(unsigned i=0;i<practice.race.finish.audio_count;++i) {
                ++practice.race_commands;
                practice.race_command_hash=(practice.race_command_hash^practice.race.finish.audio_commands[i])*UINT32_C(16777619);
                nm_sound_command(practice.race.finish.audio_commands[i]);
            }
            nm_sound_command(nm_music_finish[practice_music_select]); /* Object 44a8, same tick, after them. */
            nm_race_sound_hook_finish();
            puts("Native phase23 ->24: automatic finish driving");
        }
        if(advanced) {
            nm_race_intent_output_t out;
            practice_reject_step="race events";
            advanced=nm_race_scene_events(&practice.race,nm_race_intent_rows,57,
                nm_race_intent_alternate[practice.course],nm_race_intent_special[NM_RACE_INTENT_UNIT],
                nm_race_announcement_rows+nm_race_announcement_offsets[practice.course],
                nm_race_announcement_counts[practice.course],nm_race_announcement_ranks,
                input.award.penalty,&out);
            if(advanced) {
                practice.race_commands+=out.count;
                for(unsigned i=0;i<out.count;++i) {
                    practice.race_command_hash=(practice.race_command_hash^out.command[i])*UINT32_C(16777619);
                    nm_race_sound_hook_gated(&practice.race,out.command[i]); /* The event tail: 19bdc. */
                }
                nm_race_sound_hook_events(&practice.race,practice.race.ticks-1u); /* 19590 */
            }
        }
        if(advanced) {
            nm_player_effect_timers(&practice.race.cars[0].car);
            nm_race_sound_hook_draw(&practice.race); /* The player's draw (9088): be37, skid, 9148's lane. */
            /* 9148 (camera 6c0c -> 9088): clears +98/+99 while |+d6| > 0x1000. */
            (void)nm_player_contact_clear(&practice.race.cars[0].car);
            const nm_player_t *car=&practice.race.cars[0].car;
            const nm_reflection_input_t reflection={.frame=practice.race.ticks-1,
                .speed_index=(uint32_t)car->speed_index,.flags=car->path_flags,
                .previous_flags=car->previous_path_flags,.heading=practice_reflection_heading(car),
                .course=practice.course,.next_mode=practice.race.phase,.active_mode=practice.race.active_phase};
            nm_reflection_update_t update;
            practice_reject_step="reflection";
            advanced=nm_reflection_step(&reflection,&practice.intro.reflection_phase,&update);
            if(advanced && update.action==NM_REFLECTION_UV) {
                practice.intro.reflection_u=update.u_offset;practice.intro.reflection_v=update.v_offset;
            }
            /* 6fe0 / 7008: the windows' tunnel texture of Advanced (presentation; an asset set without its
             * bindings keeps the windows as they are). */
            if(advanced && (update.action==NM_REFLECTION_TUNNEL || update.action==NM_REFLECTION_RESTORE) &&
               !nm_practice_render_tunnel(&practice,update.action==NM_REFLECTION_TUNNEL,update.others_only) &&
               !practice_tunnel_warned) {
                puts("Native windows: the car pack has no tunnel bindings (tools/tunnel_windows.py); windows unchanged");
                practice_tunnel_warned=1;
            }
            /* a818 (HUD object a7c0, row 61): player flags bit 28 for the next tick. */
            if(advanced) { practice_reject_step="wrong way";advanced=nm_race_scene_wrong_way(&practice.race,&practice.contact); }
            if(advanced) nm_race_sound_hook_hud(&practice.race); /* The same HUD object a7c0. */
            if(advanced) {
                /* The pit crew rows 68..73 (pit.h): their sound cues, each once per crew, in queue order. */
                uint32_t cues[NM_PIT_CREW];unsigned cue_count=0;
                practice_reject_step="pit crew";
                advanced=nm_race_scene_pit_crew(&practice.race,cues,&cue_count);
                for(unsigned i=0;i<cue_count;++i) {
                    ++practice.race_commands;
                    practice.race_command_hash=(practice.race_command_hash^cues[i])*UINT32_C(16777619);
                    nm_race_sound_hook_gated(&practice.race,cues[i]); /* 1ad98: 19bdc */
                }
            }
            if(advanced) {
                /* The props rows 75..86 (props.h): cones the player knocks over; presentation, and their
                 * sound manager's command for 19bdc. */
                const uint32_t cone=nm_props_race_step(&practice.race,practice.math,&practice.contact);
                if(cone) {
                    ++practice.race_commands;
                    practice.race_command_hash=(practice.race_command_hash^cone)*UINT32_C(16777619);
                    nm_race_sound_hook_gated(&practice.race,cone); /* 22302c: 19bdc */
                }
            }
            if(advanced) {
                /* 2204fc row 109: this tick's START edge; its command follows
                 * the events (arcade queue order). */
                const uint32_t sound=nm_race_scene_slot(&practice.race,input.active.force_recovery);
                if(sound) {
                    ++practice.race_commands;
                    practice.race_command_hash=(practice.race_command_hash^sound)*UINT32_C(16777619);
                    nm_sound_command(sound);
                }
            }
            if(advanced) { practice_reject_step="race detail";advanced=practice_race_detail(); }
            /* Task 9e2c (row 66), presentation: the spin smoke of the car race.shared.inhibit_owner names, after
             * the camera row 60 (its circle's radius 2321f6 is by the view 501710 that 8814 has just chosen;
             * nothing in between reads what it writes). */
            if(advanced) {
                practice_reject_step="spin smoke";
                const unsigned owner=practice.race.shared.inhibit_owner<practice.race.count ? practice.race.shared.inhibit_owner : 0u;
                advanced=nm_spin_smoke_tick(practice.math,&practice.spin_smoke,practice.race.active_phase,
                    &practice.race.cars[owner].car,&practice.race.cars[0].car,practice.race.view);
            }
            if(advanced) practice_hud_tick(); /* HUD: once per gameplay tick */
        }
#ifdef NATIVE_RACE_CHECKPOINTS
        if(advanced) {
            checkpoint_tick=practice.race.ticks;
            practice_checkpoint(practice.race.active_phase==24 || practice.race.phase==24 ? (int32_t)practice.race.finish.timer : 0);
        }
#endif
        practice.state.remainder_us-=NM_PRACTICE_TICK_US;
    }
    NM_COPY(&practice.state.car,&practice.race.cars[0].car);practice.state.ticks=practice.race.ticks;
    practice.intro.ticks=practice.race.ticks;
    if (!advanced) {
        practice.failed = 1;
        practice_report_reject();
    }
#if defined(NATIVE_PROFILE) || defined(NATIVE_FRAME_PROFILE)
    simulation_done=timer_ns_gettime64()/1000;
#endif
render_frame:
    STAGE_TRACE("TRACE enqueue begins");
    STAGE_TRACE_LIST(PVR_LIST_OP_POLY,"TRACE opaque list: callbacks begin");
    STAGE_TRACE_LIST(PVR_LIST_PT_POLY,"TRACE punch-through list: callbacks begin");
    if(!nm_practice_render_enqueue(&practice)) {
        STAGE_TRACE("TRACE enqueue failed");
        /* A failed enqueue may have queued sky/models already. Let enDjinn
         * finish those callbacks; the next updater releases their resources. */
        practice.failed=1; return;
    }
    STAGE_TRACE("TRACE enqueue done");
    STAGE_TRACE_LIST(PVR_LIST_OP_POLY,"TRACE opaque list: callbacks done");
    STAGE_TRACE_LIST(PVR_LIST_TR_POLY,"TRACE translucent list: callbacks done");
    STAGE_TRACE_LIST(PVR_LIST_PT_POLY,"TRACE punch-through list: callbacks done (scene finish follows)");
    const uint64_t work = timer_ns_gettime64()/1000-now;
    practice.work_last_us=work;
#ifdef NM_DIAG_RENDER_STATS
    practice_diag_crew_mark();
#endif
#if defined(NATIVE_PROFILE) || defined(NATIVE_FRAME_PROFILE)
    /* Disjoint wall-time scopes; renderer callback work happens AFTER update.
     * Do not label the unmeasured remainder of the outer loop as GPU waiting. */
    practice.simulation_us+=simulation_done-now;
    ++practice.update_samples;
#endif
    if (work > practice.update_max_us) practice.update_max_us = work;
}
#ifdef NATIVE_SAMPLE_PROFILE
/* Sample tags only: the same split as the on-screen numbers. The marker is the
 * first opaque-list callback, so everything from scene begin to scene finish
 * counts as registration; it writes nothing to the PVR. */
static void practice_sample_registration(void *unused) { (void)unused;nm_sample_section(NM_SAMPLE_REGISTRATION); }
static void practice_sample_frame_end(void *unused) { (void)unused;nm_sample_section(NM_SAMPLE_OTHER); }
static void practice_update_sampled(void *unused) {
    const unsigned phase=practice.race.phase;
    nm_sample_phase(practice.attract_active && !practice.podium.phase ? NM_SAMPLE_PHASE_ATTRACT :
        phase==27 || phase==28 ? NM_SAMPLE_PHASE_PODIUM :
        phase==25 || phase==26 || phase==29 ? NM_SAMPLE_PHASE_RESULTS :
        phase==23 || phase==24 ? NM_SAMPLE_PHASE_FINISH :
        phase==21 || phase==22 ? (practice.race.cars[0].car.camera_mode==1 && practice.race.ticks<600 ?
            NM_SAMPLE_PHASE_ROLLING : NM_SAMPLE_PHASE_RACE) : NM_SAMPLE_PHASE_OTHER);
    nm_sample_section(NM_SAMPLE_UPDATER);
    enj_render_list_add(PVR_LIST_OP_POLY,practice_sample_registration,NULL);
    practice_update(unused);
    nm_sample_section(NM_SAMPLE_WAIT);
}
#endif
#if defined(NM_DIAG_DROP_LOG) || defined(NM_DIAG_PMU)
/* Measurement builds (local.cfg.mk: NATIVE_DIAG_DROP_LOG=1, NATIVE_DIAG_PMU=1), not game builds. The updater is
 * wrapped; the frame's first opaque-list callback and its last punch-through-list callback bracket registration
 * (enDjinn submits the lists in the order opaque, opaque modifier, translucent, translucent modifier,
 * punch-through). Everything here reads race state and clocks; nothing of the race is written, so the
 * checkpoints are those of the build without the option. Results are printed after the run. */
static const char *const practice_diag_phase_names[]={"other","start","race","finish","podium","results","attract"};
enum { DIAG_PHASES=7 };
/* The sample profile's scene phase (practice_update_sampled), names above. */
static unsigned practice_diag_phase(void) {
    const unsigned phase=practice.race.phase;
    return practice.attract_active && !practice.podium.phase ? NM_SAMPLE_PHASE_ATTRACT :
        phase==27 || phase==28 ? NM_SAMPLE_PHASE_PODIUM :
        phase==25 || phase==26 || phase==29 ? NM_SAMPLE_PHASE_RESULTS :
        phase==23 || phase==24 ? NM_SAMPLE_PHASE_FINISH :
        phase==21 || phase==22 ? (practice.race.cars[0].car.camera_mode==1 && practice.race.ticks<600 ?
            NM_SAMPLE_PHASE_ROLLING : NM_SAMPLE_PHASE_RACE) : NM_SAMPLE_PHASE_OTHER;
}
#endif
#ifdef NM_DIAG_PMU
/* NATIVE_DIAG_PMU=1: SH4 performance counters per frame section. Both counters are taken (PRFC0 is KOS's
 * optional perf_cntr_timer, which no code of this build reads; it is restored after the run). The event pair
 * rotates per frame: (instruction cache misses, operand cache misses), (pipeline freeze cycles by instruction
 * cache misses, by operand cache misses), (instructions issued, elapsed cycles). Sections: updater (the whole
 * mode updater: simulation, camera, scene setup, enqueue) and registration (first opaque-list callback to the
 * last punch-through-list callback). Counts include interrupts taken inside a section. After 64 intervals. */
#include <dc/perfctr.h>
#ifdef ENJ_DBG_PRINT
#error NATIVE_DIAG_PMU cannot share the performance counters with enDjinn perf_monitor
#endif
enum { PMU_PAIRS=3, PMU_EVENTS=6, PMU_SECTIONS=2 };
/* The phases of practice_diag_phase, then the frames that drew the pit crew (whatever their phase). */
enum { PMU_PHASE_CREW=DIAG_PHASES, PMU_PHASES };
static const perf_cntr_event_t pmu_event[PMU_PAIRS][2]={
    {PMCR_INSTRUCTION_CACHE_MISS_MODE,PMCR_OPERAND_CACHE_MISS_MODE},
    {PMCR_PIPELINE_FREEZE_BY_ICACHE_MISS_MODE,PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE},
    {PMCR_INSTRUCTION_ISSUED_MODE,PMCR_ELAPSED_TIME_MODE}};
static const char *const pmu_names[PMU_EVENTS]={"imiss","omiss","istall","dstall","insns","cycles"};
static uint64_t pmu_sum[PMU_PHASES][PMU_SECTIONS][PMU_EVENTS];
static uint32_t pmu_frames[PMU_PHASES][PMU_SECTIONS][PMU_PAIRS];
static unsigned pmu_pair,pmu_phase,pmu_on,pmu_reg_open;
static uint64_t pmu_reg_start[2];
/* The model renderer's groups inside registration (model_submission.h, SQ_PMU_BEGIN/END): its sums of the
 * frame, folded here per phase and event when registration ends. */
enum { PMU_RENDER_GROUPS=8 };
extern const unsigned nm_sq_pmu_groups;
extern const char *const nm_sq_pmu_names[];
extern uint64_t nm_sq_pmu_frame[][2];
extern uint32_t nm_sq_pmu_calls[];
static uint64_t pmu_group_sum[PMU_PHASES][PMU_RENDER_GROUPS][PMU_EVENTS];
static uint64_t pmu_group_calls[PMU_PHASES][PMU_RENDER_GROUPS];
static void pmu_add(unsigned section,uint64_t c0,uint64_t c1) {
    pmu_sum[pmu_phase][section][2*pmu_pair]+=c0;pmu_sum[pmu_phase][section][2*pmu_pair+1]+=c1;
    ++pmu_frames[pmu_phase][section][pmu_pair];
}
static void pmu_registration_begin(void *unused) {
    (void)unused;
    for(unsigned g=0;g<nm_sq_pmu_groups && g<PMU_RENDER_GROUPS;++g) nm_sq_pmu_frame[g][0]=nm_sq_pmu_frame[g][1]=nm_sq_pmu_calls[g]=0;
    if(!pmu_on) return;
    pmu_reg_start[0]=perf_cntr_count(PRFC0);pmu_reg_start[1]=perf_cntr_count(PRFC1);pmu_reg_open=1;
}
static void pmu_registration_end(void *unused) {
    (void)unused;
    if(!pmu_on || !pmu_reg_open) return;
    const uint64_t c0=perf_cntr_count(PRFC0),c1=perf_cntr_count(PRFC1);
    pmu_add(1,c0-pmu_reg_start[0],c1-pmu_reg_start[1]);pmu_reg_open=0;
    for(unsigned g=0;g<nm_sq_pmu_groups && g<PMU_RENDER_GROUPS;++g) {
        pmu_group_sum[pmu_phase][g][2*pmu_pair]+=nm_sq_pmu_frame[g][0];pmu_group_sum[pmu_phase][g][2*pmu_pair+1]+=nm_sq_pmu_frame[g][1];
        pmu_group_calls[pmu_phase][g]+=nm_sq_pmu_calls[g];
    }
}
static uint32_t pmu_crew_seen;
static void pmu_updater_begin(void) {
    pmu_on=practice.intervals>64 && !practice.inspect_paused;pmu_reg_open=0;
    pmu_crew_seen=practice.crew_drawn;
    if(!pmu_on) return;
    pmu_pair=(pmu_pair+1)%PMU_PAIRS;pmu_phase=practice_diag_phase();
    perf_cntr_start(PRFC0,pmu_event[pmu_pair][0],PMCR_COUNT_CPU_CYCLES);
    perf_cntr_start(PRFC1,pmu_event[pmu_pair][1],PMCR_COUNT_CPU_CYCLES);
}
static void pmu_updater_end(void) {
    if(!pmu_on) return;
    const uint64_t c0=perf_cntr_count(PRFC0),c1=perf_cntr_count(PRFC1);
    if(practice.crew_drawn!=pmu_crew_seen) pmu_phase=PMU_PHASE_CREW; /* this frame drew the crew: its own phase */
    pmu_add(0,c0,c1);
}
static void pmu_print(void) {
    perf_cntr_stop(PRFC1);perf_cntr_clear(PRFC1);perf_cntr_timer_enable();
    static const char *const sections[PMU_SECTIONS]={"updater","registration"};
    printf("PMU v1: per-frame averages after interval 64; event pairs rotate per frame, n=frames per pair; "
           "stall and cycle counts in CPU cycles (5 ns); KOS timer counter restored=%d\n",perf_cntr_timer_enabled());
    for(unsigned p=0;p<PMU_PHASES;++p) for(unsigned s=0;s<PMU_SECTIONS;++s) {
        if(!pmu_frames[p][s][0] && !pmu_frames[p][s][1] && !pmu_frames[p][s][2]) continue;
        printf("PMU phase=%s section=%s n=%lu/%lu/%lu",p==PMU_PHASE_CREW ? "pitcrew" : practice_diag_phase_names[p],sections[s],
            (unsigned long)pmu_frames[p][s][0],(unsigned long)pmu_frames[p][s][1],(unsigned long)pmu_frames[p][s][2]);
        for(unsigned e=0;e<PMU_EVENTS;++e) {
            const uint32_t n=pmu_frames[p][s][e/2];
            printf(" %s=%llu",pmu_names[e],n ? (unsigned long long)(pmu_sum[p][s][e]/n) : 0ull);
        }
        putchar('\n');
        /* Registration: the model renderer's groups (same frames, per-frame averages; calls: group instances a frame). */
        if(s==1) for(unsigned g=0;g<nm_sq_pmu_groups && g<PMU_RENDER_GROUPS;++g) {
            const uint32_t frames=pmu_frames[p][1][0]+pmu_frames[p][1][1]+pmu_frames[p][1][2];
            printf("PMU-GROUP phase=%s group=%s calls=%lu.%02lu",p==PMU_PHASE_CREW ? "pitcrew" : practice_diag_phase_names[p],nm_sq_pmu_names[g],
                (unsigned long)(frames ? pmu_group_calls[p][g]/frames : 0),
                (unsigned long)(frames ? pmu_group_calls[p][g]*100u/frames%100u : 0));
            for(unsigned e=0;e<PMU_EVENTS;++e) {
                const uint32_t n=pmu_frames[p][1][e/2];
                printf(" %s=%llu",pmu_names[e],n ? (unsigned long long)(pmu_group_sum[p][g][e]/n) : 0ull);
            }
            putchar('\n');
        }
    }
}
#endif
#ifdef NM_DIAG_DROP_LOG
/* NATIVE_DIAG_DROP_LOG=1: every displayed frame of the practice scene whose page flip came later than one
 * vblank after the previous flip. KOS's own PVR statistics hook (pvr_sync_stats, called from the PVR interrupt
 * handlers and pvr_scene_begin) is wrapped at link time (-Wl,--wrap=pvr_sync_stats; KOS is unchanged): each
 * scene's registration start/end, render start/end and page flip are timed there. The updater wrapper adds,
 * for the scene that follows it, its start and end time, the simulation ticks it ran and the camera mode.
 * Which scene an event belongs to (pvr_irq.c, pvr_scene.c, enDjinn's frame loop): registration start is the
 * next scene (pvr_scene_begin, main thread); registration end and render start belong to the oldest scene
 * whose render has not started (normally the latest begun: enDjinn's pvr_wait_ready lets a scene begin only
 * after the previous render started), render end to the render in progress, the flip to the render completed
 * last. Each event is tagged with its scene; a repeated or stray event is counted (DROP-SUMMARY anomalies) and
 * cannot shift the events of later scenes. (Version 1 counted each event kind on its own and paired the k-th
 * of each: one extra or missing event left every later scene "unmatched" until the end of the run; hardware
 * logs of 14 October 2026 lost 2,000 to 12,000 of 12,900 frames that way at a random point of the run.)
 * Lateness needs only the flips: a displayed frame is late when its flip came 2 or more vblanks after the
 * previous displayed flip, whatever else was recorded for it.
 * Cause of a late frame k (P render time, U updater, R registration, W wait before registration, all of frame k;
 * F the previous flip, V one vblank period):
 *   load: updater or loop gap over 50 ms (scene load); pvr: the render alone exceeds V, or registration was done
 *   before F (the render waited for the previous flip and still missed). Otherwise registration finished after F
 *   by more than V minus the render time: cpu-registration if the CPU had waited over 1 ms for the previous
 *   render to start before registering (the updater was not in the way: registration itself was too long for
 *   the time from that render start to F); cpu-other: over 1.5 ms between the previous registration and this
 *   updater (engine, input); else cpu-update or cpu-registration, whichever exceeds its running mean (on-time
 *   frames) by more. other: none of these (registration late but the render would still have fitted), or the
 *   frame's registration/render events are incomplete. capture (NATIVE_DIAG_LATE_CAPTURE): late while a picture
 *   copy may still delay it (from the first frame a copy can delay to the next frame on time).
 * Printed after the run: the first 200 records, counts by phase and by 500-tick range of the run's tick
 * counter, a summary line. tools/drop_log_report.py summarises a console log. */
#ifdef NM_DIAG_LATE_CAPTURE
/* NATIVE_DIAG_LATE_CAPTURE=N (include/late_capture.h): the picture of each late frame k and, when lateness is
 * known in time, of frame k-1 (shown just before it). Frames alternate between KOS's two frame buffers, so
 * scene j's picture stays in its buffer from its render's end until render j+2 starts, and render j+2 cannot
 * start before scene j+2's registration ended. The copies run in the frame's last punch-through callback (the
 * end of the CPU's registration of scene n, before the list closes): there the flip of scene n-2 has always
 * happened (scene n began after render n-1 started, which waited for that flip), so every late frame k is seen
 * by the registration of scene k+2 at the latest, with its buffer intact; frame k-1's picture is intact until
 * render k+1 starts, i.e. until the registration of scene k+1 ends: it is copied there when frame k is known to
 * be late by then (flipped late, or not flipped although the vblank after frame k-1's flip has passed). The
 * buffer of frame j: the one on display if j is the latest flip, else the one that was on display when the
 * next flip came (read from the PVR's frame buffer address register in the flip event). A copy delays the
 * registration of scene n, and its VRAM reads compete with a render still running (scene n-1's, unless that
 * frame is already known to be late): frames from there on that are late until a frame is on time again get
 * the cause "capture" and are not captured. Each copy is timed (copy_us). */
#include "late_capture.h"
#ifndef DROP_FB_ADDR
#define DROP_FB_ADDR() ((uint32_t)PVR_GET(PVR_FB_ADDR))
#endif
#ifndef NM_LATE_CAPTURE_NAME
#define NM_LATE_CAPTURE_NAME "late-capture"
#endif
#endif
enum { DROP_RING=16, DROP_KEEP=200, DROP_BUCKET_TICKS=500, DROP_BUCKETS=48, DROP_RECENT=8 };
enum { DROP_PVR,DROP_CPU_UPDATE,DROP_CPU_REGISTRATION,DROP_CPU_OTHER,DROP_LOAD,DROP_OTHER,DROP_CAPTURE,DROP_CAUSES };
/* The last name is the "cause" recorded for frames on time. */
static const char *const drop_cause_names[DROP_CAUSES+1]={"pvr","cpu-update","cpu-registration","cpu-other","load","other","capture","on-time"};
static const char *const drop_scene_names[]={"race","attract","podium"};
/* Events that fit no scene (the wrapper below): counted, never attributed elsewhere. */
enum { DROP_A_REGDONE_REPEAT,DROP_A_REGDONE_STRAY,DROP_A_RNDSTART_EARLY,DROP_A_RNDSTART_STRAY,DROP_A_RNDDONE_STRAY,DROP_A_FLIP_STRAY,
       DROP_ANOMALIES };
static const char *const drop_anomaly_names[DROP_ANOMALIES]={"regdone_repeat","regdone_stray","rndstart_early","rndstart_stray",
    "rnddone_stray","flip_stray"};
typedef struct {
    uint64_t upd_start,upd_end,reg_start,reg_done,rnd_start,rnd_done,flip;
    /* seq: set by the scene's registration start; the other tags by the event they date. */
    uint32_t seq,regdone_seq,rndstart_seq,rnddone_seq,flip_seq,cpu_seq,flip_vbl,run,tick,fb,fb_seq;
    int32_t camera_mode;
    uint16_t ticks;
    uint8_t phase,scene,camera_transition;
} drop_slot_t;
typedef struct {
    uint32_t seq,run,tick,upd,wait,reg,rnd,other,prev_rnd,flip_vbl;
    int32_t late,camera_mode;
    uint16_t ticks;
    uint8_t phase,scene,vbl,cause,camera_transition,untimed;
} drop_record_t;
static volatile drop_slot_t drop_ring[DROP_RING];
/* Interrupt side: vblanks, scenes begun, latest render started, completed and flipped, flip events. */
static volatile uint32_t drop_vbl,drop_regs,drop_rendering,drop_completed,drop_flipped,drop_flip_events;
static volatile uint32_t drop_anomaly[DROP_ANOMALIES];
static drop_record_t drop_records[DROP_KEEP];
static drop_record_t drop_recent[DROP_RECENT];
static uint32_t drop_done,drop_kept,drop_run,drop_displayed,drop_other_frames,drop_unflipped,drop_overrun,drop_unmatched;
static int drop_prev_shown;
static uint32_t drop_prev_vbl,drop_first_vbl,drop_last_vbl;
static uint64_t drop_prev_flip,drop_prev_regdone,drop_prev_rnd,drop_first_flip,drop_last_flip;
static uint32_t drop_phase_frames[DIAG_PHASES],drop_phase_late[DIAG_PHASES],drop_phase_missed[DIAG_PHASES];
static uint32_t drop_phase_cause[DIAG_PHASES][DROP_CAUSES];
static uint64_t drop_phase_upd[DIAG_PHASES],drop_phase_reg[DIAG_PHASES],drop_phase_rnd[DIAG_PHASES];
static uint32_t drop_bucket_frames[DROP_BUCKETS],drop_bucket_late[DROP_BUCKETS];
static uint16_t drop_bucket_cause[DROP_BUCKETS][DROP_CAUSES];
static uint32_t drop_mean_upd=3000,drop_mean_reg=9000; /* us, running means over on-time frames */
static uint32_t drop_tick_before;
#define DROP_PERIOD_NS UINT64_C(16683000) /* NTSC 59.94 Hz; the measured period is printed */
#ifdef NM_DIAG_LATE_CAPTURE
static struct { uint32_t seq; int slot; } drop_pending[NM_LATE_CAPTURE_SLOTS];
static unsigned drop_pending_count;
static int drop_cap_open;
static uint32_t drop_cap_from,drop_cap_last,drop_cap_late,drop_cap_eligible,drop_cap_not_kept,drop_cap_missed,drop_cap_copies,drop_cap_torn;
static uint32_t drop_cap_no_before;
static uint64_t drop_cap_us_total;
static uint32_t drop_cap_us_max;
#endif
#ifdef NM_DIAG_FRAME_TRACE
/* include/frame_trace.h: a scene's record lives in ft_ring like its slot in drop_ring; drop_frame keeps it. */
#include "frame_trace.h"
enum { FT_ROWS=6000,FT_ROW_FROM_US=15000,FT_WINDOW=NM_FT_HI>NM_FT_LO ? NM_FT_HI-NM_FT_LO+1 : 1,FT_LISTS=5 };
typedef struct { uint16_t run,upd,wait,reg,rnd,other;int16_t late;uint8_t vbl,phase; } ft_row_t;
typedef struct {
    uint32_t sim,setup,prep,opponents,world,part_count,shadows,cpu_end;
    uint32_t list[FT_LISTS],ta[FT_LISTS],counts[FT_LISTS][3],group[NM_FT_GROUPS],calls[NM_FT_GROUPS],emit[2][4];
    uint32_t object_count,car_count;
    nm_ft_object_t objects[NM_FT_OBJECTS];
    nm_ft_car_t cars[NM_FT_CARS];
} ft_scene_t;
typedef struct { uint32_t run,tick,upd,wait,reg,rnd,other,vbl;int32_t late;ft_scene_t s; } ft_win_t;
static ft_scene_t ft_ring[DROP_RING];
static ft_row_t ft_rows[FT_ROWS];
static ft_win_t ft_window[FT_WINDOW];
static unsigned ft_row_count,ft_win_count,ft_list;
static uint32_t ft_list_start,ft_reg_start;
static uint64_t ft_sim0,ft_setup0,ft_prep0;
int __real_pvr_list_begin(pvr_list_t list);
int __real_pvr_list_finish(void);
int __wrap_pvr_list_begin(pvr_list_t list) {
    ft_list=(unsigned)list<FT_LISTS ? (unsigned)list : 0;
    ft_list_start=nm_ft_clock();
    if(ft_list==PVR_LIST_OP_POLY) ft_reg_start=ft_list_start;
    return __real_pvr_list_begin(list);
}
int __wrap_pvr_list_finish(void) {
    const int result=__real_pvr_list_finish();
    ft_scene_t *f=ft_ring+drop_regs%DROP_RING;
    const uint32_t now=nm_ft_clock();
    f->list[ft_list]=nm_ft_delta(ft_list_start,now);
    f->ta[ft_list]=(uint32_t)PVR_GET(PVR_TA_VERTBUF_POS);
    nm_ft_render_counts(f->counts[ft_list]);
    if(ft_list==PVR_LIST_PT_POLY) { /* the frame's last list */
        f->cpu_end=nm_ft_delta(ft_reg_start,now);
        nm_memcpy(f->group,nm_ft_group,sizeof(f->group));nm_memcpy(f->calls,nm_ft_calls,sizeof(f->calls));
        nm_memcpy(f->emit,nm_ft_emit,sizeof(f->emit));
        f->opponents=nm_ft_opponents;
        nm_memset(nm_ft_group,0,sizeof(nm_ft_group));nm_memset(nm_ft_calls,0,sizeof(nm_ft_calls));
        nm_memset(nm_ft_emit,0,sizeof(nm_ft_emit));nm_ft_opponents=0;
        f->object_count=nm_ft_object_count;f->car_count=nm_ft_car_count;
        for(unsigned c=0;c<nm_ft_car_count && c<NM_FT_CARS;++c) {
            const nm_ft_part_t *x=nm_ft_parts+(nm_ft_cars[c].first<NM_FT_PARTS ? nm_ft_cars[c].first : 0);
            nm_ft_cars[c].prep=x->prep;nm_ft_cars[c].op=x->op;nm_ft_cars[c].pt=x->pt;nm_ft_cars[c].tri=x->tri;
        }
        nm_memcpy(f->objects,nm_ft_objects,sizeof(f->objects));nm_memcpy(f->cars,nm_ft_cars,sizeof(f->cars));
        nm_memset(nm_ft_objects,0,sizeof(nm_ft_objects));nm_memset(nm_ft_parts,0,sizeof(nm_ft_parts));
        nm_ft_object_count=nm_ft_car_count=0;
    }
    return result;
}
static inline uint32_t ft_us(uint32_t counts) { return (counts*2u+12u)/25u; } /* 80 ns a count */
#endif
/* Interrupt context (and pvr_scene_begin): the oldest scene begun whose render has not started, by the state
 * of the events already seen: the scene before the latest one only while that one began early (pvr_wait_ready
 * timed out) and the older scene still lacks the event (registration end; for render start: has it). */
static uint32_t drop_unrendered(int registered) {
    const uint32_t regs=drop_regs,older=drop_rendering+1;
    if(older+1==regs) {
        const volatile drop_slot_t *d=drop_ring+older%DROP_RING;
        if(d->seq==older && (d->regdone_seq==older)==(registered!=0)) return older;
    }
    return regs;
}
void __real_pvr_sync_stats(int event);
void __wrap_pvr_sync_stats(int event);
void __wrap_pvr_sync_stats(int event) {
    __real_pvr_sync_stats(event);
    if(event==1) { ++drop_vbl;return; }            /* PVR_SYNC_VBLANK (kernel/arch/dreamcast/hardware/pvr/pvr_internal.h) */
    const uint64_t t=timer_ns_gettime64();
    switch(event) {
    case 4: {                                      /* REGSTART */
        const uint32_t k=drop_regs+1;volatile drop_slot_t *d=drop_ring+k%DROP_RING;
        d->reg_start=t;d->seq=k;drop_regs=k;break;
    }
    case 5: {                                      /* REGDONE: a repeat before the render starts moves the time */
        const uint32_t k=drop_unrendered(0);volatile drop_slot_t *d=drop_ring+k%DROP_RING;
        if(!k || d->seq!=k || d->rndstart_seq==k) { ++drop_anomaly[DROP_A_REGDONE_STRAY];break; }
        if(d->regdone_seq==k) ++drop_anomaly[DROP_A_REGDONE_REPEAT];
        d->reg_done=t;d->regdone_seq=k;break;
    }
    case 6: {                                      /* RNDSTART */
        const uint32_t k=drop_unrendered(1);volatile drop_slot_t *d=drop_ring+k%DROP_RING;
        if(!k || d->seq!=k || d->rndstart_seq==k) { ++drop_anomaly[DROP_A_RNDSTART_STRAY];break; }
        if(d->regdone_seq!=k) ++drop_anomaly[DROP_A_RNDSTART_EARLY];
        d->rnd_start=t;d->rndstart_seq=k;drop_rendering=k;break;
    }
    case 7: {                                      /* RNDDONE */
        const uint32_t k=drop_rendering;volatile drop_slot_t *d=drop_ring+k%DROP_RING;
        if(!k || d->rndstart_seq!=k || d->rnddone_seq==k) { ++drop_anomaly[DROP_A_RNDDONE_STRAY];break; }
        d->rnd_done=t;d->rnddone_seq=k;drop_completed=k;break;
    }
    case 8: {                                      /* PAGEFLIP: the frame buffer address still shows the old frame */
        const uint32_t k=drop_completed;volatile drop_slot_t *d=drop_ring+k%DROP_RING;
        ++drop_flip_events;
        if(!k || d->rnddone_seq!=k || d->flip_seq==k) { ++drop_anomaly[DROP_A_FLIP_STRAY];break; }
#ifdef NM_DIAG_LATE_CAPTURE
        {
            const uint32_t shown=drop_flipped;volatile drop_slot_t *s=drop_ring+shown%DROP_RING;
            if(shown && s->flip_seq==shown) { s->fb=DROP_FB_ADDR();s->fb_seq=shown; }
        }
#endif
        d->flip=t;d->flip_vbl=drop_vbl;d->flip_seq=k;drop_flipped=k;break;
    }
    default: break;
    }
}
/* Simulation ticks run so far, over every scene of the run: race ticks, podium ticks (its timer runs 1600 down
 * to -1), result ticks, cinematic attract ticks (race state is zero there). */
static uint32_t drop_tick_clock(void) {
    const uint32_t podium=practice.podium.phase==28 || practice.podium.timer<0 ? (uint32_t)(1600-practice.podium.timer) : 0;
    return practice.race.ticks+podium+practice.results.ticks+(practice.race.phase==0 ? practice.intro.ticks : 0);
}
static unsigned drop_scene(void) {
    return !practice.attract_active ? 0 : practice.podium.phase || practice.race.phase==27 || practice.race.phase==28 ? 2 : 1;
}
static uint32_t drop_us(uint64_t from,uint64_t to) { return to>from ? (uint32_t)((to-from)/1000) : 0; }
#ifdef NM_DIAG_LATE_CAPTURE
static uint32_t drop_score(const drop_record_t *r) {
    const uint32_t work=r->reg+r->rnd;
    return (uint32_t)r->vbl<<20|(work<0xfffffu ? work : 0xfffffu);
}
static void drop_fill(nm_late_record_t *x,const drop_record_t *r) {
    x->run=r->run;x->tick=r->tick;x->phase=r->phase;x->scene=r->scene;x->vbl=r->vbl;x->cause=r->cause;
    x->upd_us=r->upd;x->ticks=r->ticks;x->wait_us=r->wait;x->reg_us=r->reg;x->rnd_us=r->rnd;x->late_us=r->late;
    x->other_us=r->other;x->prev_rnd_us=r->prev_rnd;x->camera_mode=r->camera_mode;x->camera_transition=r->camera_transition;
    x->flip_vbl=r->flip_vbl;
    if(r->untimed) x->flags|=NM_LATE_UNTIMED;
}
/* A processed practice frame: a late one is offered a slot; a "before" picture taken for a frame that is
 * not kept after all is given back. */
static void drop_capture_frame(const drop_record_t *r) {
    if(!nm_late_ready()) return;
    int slot=-1;
    if(r->vbl>=2) {
        ++drop_cap_late;
        if(r->cause!=DROP_LOAD && r->cause!=DROP_CAPTURE) {
            ++drop_cap_eligible;
            slot=nm_late_take_late(r->seq,drop_score(r));
            if(slot<0) ++drop_cap_not_kept;
            else {
                drop_fill(nm_late_record(slot),r);
                if(drop_pending_count<NM_LATE_CAPTURE_SLOTS) {
                    drop_pending[drop_pending_count].seq=r->seq;drop_pending[drop_pending_count].slot=slot;++drop_pending_count;
                }
            }
        }
    }
    if(slot<0) {
        const int before=nm_late_find(r->seq-1,1);
        if(before>=0 && nm_late_record(before)->pair_seq==r->seq) nm_late_release(before);
    }
}
#endif
static void drop_frame(uint32_t k,const volatile drop_slot_t *d,int timed) {
    const uint32_t vbl=d->flip_vbl-drop_prev_vbl;
    const unsigned phase=d->phase;
    const uint32_t upd=drop_us(d->upd_start,d->upd_end),wait=drop_us(d->upd_end,d->reg_start);
    const uint32_t reg=d->regdone_seq==k ? drop_us(d->reg_start,d->reg_done) : 0,rnd=timed ? drop_us(d->rnd_start,d->rnd_done) : 0;
    const uint32_t other=drop_prev_regdone ? drop_us(drop_prev_regdone,d->upd_start) : 0;
    const unsigned bucket=d->run/DROP_BUCKET_TICKS<DROP_BUCKETS ? d->run/DROP_BUCKET_TICKS : DROP_BUCKETS-1;
    const int64_t late_ns=d->regdone_seq==k ? (int64_t)(d->reg_done-drop_prev_flip) : 0;
    ++drop_phase_frames[phase];++drop_bucket_frames[bucket];
    drop_phase_upd[phase]+=upd;drop_phase_reg[phase]+=reg;drop_phase_rnd[phase]+=rnd;
    drop_record_t r={.seq=k,.run=d->run,.tick=d->tick,.upd=upd,.wait=wait,.reg=reg,.rnd=rnd,.other=other,
        .prev_rnd=(uint32_t)(drop_prev_rnd/1000),.flip_vbl=d->flip_vbl,.late=(int32_t)(late_ns/1000),.camera_mode=d->camera_mode,
        .ticks=d->ticks,.phase=(uint8_t)phase,.scene=d->scene,.vbl=(uint8_t)(vbl<255 ? vbl : 255),.cause=DROP_CAUSES,
        .camera_transition=d->camera_transition,.untimed=(uint8_t)!timed};
    if(vbl>=2) {
        unsigned cause;
        if(upd>50000 || other>50000) cause=DROP_LOAD;
        else if(!timed) cause=DROP_OTHER;
        else if((uint64_t)(d->rnd_done-d->rnd_start)>DROP_PERIOD_NS || late_ns<=0) cause=DROP_PVR;
        else if(d->rnd_done-d->rnd_start+(uint64_t)late_ns<=DROP_PERIOD_NS) cause=DROP_OTHER;
        else if(wait>1000) cause=DROP_CPU_REGISTRATION;
        else if(other>1500) cause=DROP_CPU_OTHER;
        else cause=(int32_t)(upd-drop_mean_upd)>(int32_t)(reg-drop_mean_reg) ? DROP_CPU_UPDATE : DROP_CPU_REGISTRATION;
#ifdef NM_DIAG_LATE_CAPTURE
        if(drop_cap_open && k>=drop_cap_from && cause!=DROP_LOAD) cause=DROP_CAPTURE;
#endif
        r.cause=(uint8_t)cause;
        ++drop_phase_late[phase];drop_phase_missed[phase]+=vbl-1;++drop_phase_cause[phase][cause];
        ++drop_bucket_late[bucket];++drop_bucket_cause[bucket][cause];
        if(drop_kept<DROP_KEEP) drop_records[drop_kept++]=r;
    } else {
        if(timed && upd<50000 && other<50000) {
            drop_mean_upd=drop_mean_upd+((int32_t)(upd-drop_mean_upd)>>4);
            drop_mean_reg=drop_mean_reg+((int32_t)(reg-drop_mean_reg)>>4);
        }
#ifdef NM_DIAG_LATE_CAPTURE
        if(drop_cap_open && k>=drop_cap_last) drop_cap_open=0;   /* on time after the last copy's first frame */
#endif
    }
    drop_recent[k%DROP_RECENT]=r;
#ifdef NM_DIAG_LATE_CAPTURE
    drop_capture_frame(&r);
#endif
#ifdef NM_DIAG_FRAME_TRACE
    if(ft_row_count<FT_ROWS && (upd+reg>=FT_ROW_FROM_US || vbl>=2)) {
        ft_row_t *row=ft_rows+ft_row_count++;
        row->run=(uint16_t)r.run;row->upd=(uint16_t)(upd<65535 ? upd : 65535);row->wait=(uint16_t)(wait<65535 ? wait : 65535);
        row->reg=(uint16_t)(reg<65535 ? reg : 65535);row->rnd=(uint16_t)(rnd<65535 ? rnd : 65535);
        row->other=(uint16_t)(other<65535 ? other : 65535);
        row->late=(int16_t)(r.late>32767 ? 32767 : r.late<-32768 ? -32768 : r.late);row->vbl=r.vbl;row->phase=r.phase;
    }
    if(r.run>=NM_FT_LO && r.run<=NM_FT_HI && ft_win_count<FT_WINDOW) {
        ft_win_t *w=ft_window+ft_win_count++;
        w->run=r.run;w->tick=r.tick;w->upd=upd;w->wait=wait;w->reg=reg;w->rnd=rnd;w->other=other;w->vbl=r.vbl;w->late=r.late;
        nm_memcpy(&w->s,ft_ring+k%DROP_RING,sizeof(w->s));
    }
#endif
}
/* Every scene up to the latest flip, in order. A scene the ring no longer holds (processing fell 16 scenes
 * behind) is "overrun"; one that was never flipped (rendered to a texture, or its events went astray) is
 * "unflipped"; the interval of a displayed frame runs from the previous displayed flip. */
static void drop_process(void) {
    const uint32_t last=drop_flipped;
    while(drop_done<last) {
        const uint32_t k=++drop_done;
        const volatile drop_slot_t *d=drop_ring+k%DROP_RING;
        if(d->seq!=k) { ++drop_overrun;drop_prev_shown=0;continue; }
        if(d->flip_seq!=k) { ++drop_unflipped;continue; }
        ++drop_displayed;
        const int timed=d->regdone_seq==k && d->rndstart_seq==k && d->rnddone_seq==k && d->reg_done>=d->reg_start &&
            d->rnd_start>=d->reg_done && d->rnd_done>=d->rnd_start && d->flip>=d->rnd_done;
        if(!timed) ++drop_unmatched;
        if(d->cpu_seq!=k) ++drop_other_frames;
        else if(drop_prev_shown) drop_frame(k,d,timed);
        if(!drop_first_flip) { drop_first_flip=d->flip;drop_first_vbl=d->flip_vbl; }
        drop_last_flip=d->flip;drop_last_vbl=d->flip_vbl;
        drop_prev_shown=1;drop_prev_flip=d->flip;drop_prev_vbl=d->flip_vbl;
        drop_prev_regdone=d->regdone_seq==k ? d->reg_done : 0;
        drop_prev_rnd=timed ? d->rnd_done-d->rnd_start : 0;
    }
}
static void drop_updater_begin(void) {
    drop_process();
    const uint32_t k=drop_regs+1;  /* the scene enDjinn begins after this updater */
    volatile drop_slot_t *d=drop_ring+k%DROP_RING;
    d->cpu_seq=0;d->upd_start=timer_ns_gettime64();
    drop_tick_before=drop_tick_clock();
#ifdef NM_DIAG_FRAME_TRACE
    ft_sim0=practice.simulation_us;ft_setup0=practice.scene_setup_us;ft_prep0=practice.render_prepare_us;
#endif
}
static void drop_updater_end(void) {
    const uint32_t k=drop_regs+1;
    volatile drop_slot_t *d=drop_ring+k%DROP_RING;
    d->upd_end=timer_ns_gettime64();
    const uint32_t ticks=drop_tick_clock()-drop_tick_before;
    const uint16_t clamped=ticks<1000 ? (uint16_t)ticks : 0;
    drop_run+=clamped;
    d->ticks=clamped;d->run=drop_run;d->tick=practice.race.ticks;
    d->phase=(uint8_t)practice_diag_phase();d->scene=(uint8_t)drop_scene();
    d->camera_mode=practice.race.cars[0].car.camera_mode;d->camera_transition=practice.race.cars[0].car.camera_transition;
    d->cpu_seq=k;
#ifdef NM_DIAG_FRAME_TRACE
    ft_scene_t *f=ft_ring+k%DROP_RING;
    f->sim=(uint32_t)(practice.simulation_us-ft_sim0);f->setup=(uint32_t)(practice.scene_setup_us-ft_setup0);
    f->prep=(uint32_t)(practice.render_prepare_us-ft_prep0);
    f->world=practice.world_header.instances;f->part_count=practice.intro_header.instances;f->shadows=practice.shadow_count;
#endif
}
#ifdef NM_DIAG_LATE_CAPTURE
/* The frame buffer holding scene j's picture: the one on display if j is the latest flip (read together with
 * the flip count: both change in the same interrupt), else the one recorded at the flip after j. */
static int drop_fb_of(uint32_t j,uint32_t *fb,uint32_t *source) {
    uint32_t shown,addr;
    do { shown=drop_flipped;addr=DROP_FB_ADDR(); } while(shown!=drop_flipped);
    if(j==shown) { *fb=addr;*source=0;return 1; }
    const volatile drop_slot_t *s=drop_ring+j%DROP_RING;
    if(j<shown && s->fb_seq==j) { *fb=s->fb;*source=1;return 1; }
    return 0;
}
/* Copy scene j's picture into the slot during the registration of scene n; 0 if the buffer may no longer
 * hold it (render j+2 started) or is unknown. */
static unsigned drop_copy(int slot,uint32_t j,uint32_t n) {
    nm_late_record_t *rec=nm_late_record(slot);
    uint32_t fb=0,source=0;
    if(drop_rendering>=j+2 || !drop_fb_of(j,&fb,&source)) {
        ++drop_cap_missed;
        if(rec->kind!=NM_LATE_BEFORE) {   /* its "before" picture goes too */
            const int before=nm_late_find(j-1,1);
            if(before>=0 && nm_late_record(before)->pair_seq==j) nm_late_release(before);
        }
        nm_late_release(slot);return 0;
    }
    const uint64_t start=timer_ns_gettime64();
    nm_late_copy(slot,fb);
    const uint64_t end=timer_ns_gettime64();
    const volatile drop_slot_t *d=drop_ring+j%DROP_RING;
    rec->copy_us=drop_us(start,end);rec->copy_scene=n;rec->source=source;
    rec->since_flip_us=d->flip_seq==j ? drop_us(d->flip,start) : 0;
    if(drop_rendering>=j+2) { rec->flags|=NM_LATE_TORN;++drop_cap_torn; }
    ++drop_cap_copies;drop_cap_us_total+=rec->copy_us;
    if(rec->copy_us>drop_cap_us_max) drop_cap_us_max=rec->copy_us;
    return 1;
}
/* End of the CPU's registration of scene n (its last punch-through callback). */
static void drop_capture_point(void) {
    drop_process();
    if(!nm_late_ready()) return;
    const uint32_t n=drop_regs;
    unsigned copies=0;
    /* Frames a copy may delay: this scene's, and a render still running (the copy's VRAM reads compete with it),
     * unless that frame is already known to be late. */
    uint32_t affected=drop_completed+1;
    /* Late frames kept since the last call: their picture (every one is at most two scenes old here). */
    for(unsigned i=0;i<drop_pending_count;++i) {
        const uint32_t k=drop_pending[i].seq;const int slot=drop_pending[i].slot;
        const nm_late_record_t *rec=nm_late_record(slot);
        if(rec && rec->seq==k && (rec->kind==NM_LATE_FIRST || rec->kind==NM_LATE_WORST) && !(rec->flags&NM_LATE_COPIED))
            copies+=drop_copy(slot,k,n);
    }
    drop_pending_count=0;
    /* Frame n-1 late (flipped late and kept, or still not flipped a vblank after frame n-2's flip): the
     * picture shown before it, frame n-2, whose buffer render n will be the first to overwrite. */
    if(n>=3) {
        const uint32_t late=n-1,prev=n-2;
        const volatile drop_slot_t *dl=drop_ring+late%DROP_RING,*dp=drop_ring+prev%DROP_RING;
        int want=0;
        if(nm_late_find(prev,1)<0 && dl->seq==late && dl->cpu_seq==late && dp->flip_seq==prev &&
           !(drop_cap_open && late>=drop_cap_from)) {
            if(drop_done>=late) want=nm_late_find(late,0)>=0;
            else {
                uint32_t shown,vbl;
                do { shown=drop_flipped;vbl=drop_vbl; } while(shown!=drop_flipped);
                want=shown==prev && vbl>=dp->flip_vbl+1 && drop_us(dl->upd_start,dl->upd_end)<=50000;
                if(want && affected<=late) affected=late+1;
            }
        }
        if(want) {
            const int slot=nm_late_take_before(prev,late);
            if(slot<0) ++drop_cap_no_before;
            else {
                const drop_record_t *r=drop_recent+prev%DROP_RECENT;
                if(r->seq==prev) drop_fill(nm_late_record(slot),r);
                copies+=drop_copy(slot,prev,n);
            }
        }
    }
    if(affected>n) affected=n;
    if(copies) {
        if(!drop_cap_open || affected<drop_cap_from) drop_cap_from=affected;
        if(!drop_cap_open || affected>drop_cap_last) drop_cap_last=affected;
        drop_cap_open=1;
    }
}
static const char *const drop_capture_kinds[NM_LATE_KINDS]={"none","first","worst","before"};
static const char *const drop_capture_sources[]={"front","back"};
static void drop_capture_print(void) {
    if(!nm_late_ready()) { puts("LATE-CAPTURE v1: no pool (refused at startup)");return; }
    unsigned images=0,before=0,uncopied=0;
    uint32_t affected=0;
    for(int s=0;s<NM_LATE_CAPTURE_SLOTS;++s) {
        const nm_late_record_t *r=nm_late_record(s);
        if(r->kind==NM_LATE_FREE) continue;
        if(!(r->flags&NM_LATE_COPIED)) { ++uncopied;continue; }
        ++images;before+=r->kind==NM_LATE_BEFORE;
    }
    for(unsigned p=0;p<DIAG_PHASES;++p) affected+=drop_phase_cause[p][DROP_CAPTURE];
    printf("LATE-CAPTURE v1: slots=%u first_n=%u %ux%u images=%u before=%u late=%lu eligible=%lu not_kept=%lu evicted=%lu "
           "missed=%lu uncopied=%u no_before_slot=%lu copies=%lu copy_us_mean=%lu copy_us_max=%lu torn=%lu affected=%lu\n",
        (unsigned)NM_LATE_CAPTURE_SLOTS,(unsigned)NM_LATE_CAPTURE_FIRST,(unsigned)NM_LATE_W,(unsigned)NM_LATE_H,images,before,
        (unsigned long)drop_cap_late,(unsigned long)drop_cap_eligible,(unsigned long)drop_cap_not_kept,(unsigned long)nm_late_evicted(),
        (unsigned long)drop_cap_missed,uncopied,(unsigned long)drop_cap_no_before,(unsigned long)drop_cap_copies,
        (unsigned long)(drop_cap_copies ? drop_cap_us_total/drop_cap_copies : 0),(unsigned long)drop_cap_us_max,
        (unsigned long)drop_cap_torn,(unsigned long)affected);
    for(int s=0;s<NM_LATE_CAPTURE_SLOTS;++s) {
        const nm_late_record_t *r=nm_late_record(s);
        if(r->kind==NM_LATE_FREE || !(r->flags&NM_LATE_COPIED)) continue;
        printf("LATE-IMG kind=%s seq=%lu pair=%lu run=%lu tick=%lu phase=%s scene=%s vbl=%lu cause=%s upd=%lu ticks=%lu wait=%lu "
               "reg=%lu rnd=%lu late=%ld camera=%ld/%lu copy_us=%lu copy_scene=%lu since_flip_us=%lu source=%s torn=%u\n",
            drop_capture_kinds[r->kind],(unsigned long)r->seq,(unsigned long)r->pair_seq,(unsigned long)r->run,(unsigned long)r->tick,
            practice_diag_phase_names[r->phase<DIAG_PHASES ? r->phase : 0],drop_scene_names[r->scene<3 ? r->scene : 0],
            (unsigned long)r->vbl,drop_cause_names[r->cause<=DROP_CAUSES ? r->cause : DROP_OTHER],(unsigned long)r->upd_us,
            (unsigned long)r->ticks,(unsigned long)r->wait_us,(unsigned long)r->reg_us,(unsigned long)r->rnd_us,(long)r->late_us,
            (long)r->camera_mode,(unsigned long)r->camera_transition,(unsigned long)r->copy_us,(unsigned long)r->copy_scene,
            (unsigned long)r->since_flip_us,drop_capture_sources[r->source&1],(unsigned)(r->flags&NM_LATE_TORN ? 1 : 0));
    }
}
/* After the run: the pictures and their index to the asset root (dcload /pc/). */
static void drop_capture_write(const char *root) {
    if(!nm_late_ready() || !root) return;
    nm_late_header_t header={0};
    const uint32_t vbls=drop_last_vbl-drop_first_vbl;
    header.period_ns=vbls && drop_first_flip ? (uint32_t)((drop_last_flip-drop_first_flip)/vbls) : 0;
    header.late=drop_cap_late;header.eligible=drop_cap_eligible;header.not_kept=drop_cap_not_kept;header.missed=drop_cap_missed;
    header.copies=drop_cap_copies;header.copy_us_total=(uint32_t)drop_cap_us_total;header.copy_us_max=drop_cap_us_max;
    for(unsigned p=0;p<DIAG_PHASES;++p) header.affected+=drop_phase_cause[p][DROP_CAPTURE];
    header.torn=drop_cap_torn;header.run_ticks=drop_run;
    for(int s=0;s<NM_LATE_CAPTURE_SLOTS;++s) {
        const nm_late_record_t *r=nm_late_record(s);
        header.before_images+=r->kind==NM_LATE_BEFORE && (r->flags&NM_LATE_COPIED);
    }
    const nm_late_names_t names[5]={{drop_capture_kinds,NM_LATE_KINDS},{practice_diag_phase_names,DIAG_PHASES},
        {drop_scene_names,3},{drop_cause_names,DROP_CAUSES+1},{drop_capture_sources,2}};
    nm_late_write(root,NM_LATE_CAPTURE_NAME,&header,names);
}
#endif
static void drop_print(void) {
    drop_process();
    uint32_t frames=0,late=0,missed=0,causes[DROP_CAUSES]={0};
    for(unsigned p=0;p<DIAG_PHASES;++p) {
        frames+=drop_phase_frames[p];late+=drop_phase_late[p];missed+=drop_phase_missed[p];
        for(unsigned c=0;c<DROP_CAUSES;++c) causes[c]+=drop_phase_cause[p][c];
    }
    const uint32_t vbls=drop_last_vbl-drop_first_vbl;
    const uint32_t period=vbls && drop_first_flip ? (uint32_t)((drop_last_flip-drop_first_flip)/vbls) : 0;
    printf("DROP-LOG v2: frame=flip interval of the practice scene; vbl=vblanks since the previous flip; times in us; "
           "period_ns=%lu (measured over %lu vblanks); records kept=%lu of %lu\n",
        (unsigned long)period,(unsigned long)vbls,(unsigned long)drop_kept,(unsigned long)late);
    for(unsigned i=0;i<drop_kept;++i) {
        const drop_record_t *r=drop_records+i;
        printf("DROP seq=%lu run=%lu tick=%lu phase=%s scene=%s vbl=%u cause=%s upd=%lu ticks=%u wait=%lu reg=%lu rnd=%lu late=%ld other=%lu prev_rnd=%lu camera=%ld/%u",
            (unsigned long)r->seq,(unsigned long)r->run,(unsigned long)r->tick,practice_diag_phase_names[r->phase],
            drop_scene_names[r->scene],(unsigned)r->vbl,drop_cause_names[r->cause],(unsigned long)r->upd,(unsigned)r->ticks,
            (unsigned long)r->wait,(unsigned long)r->reg,(unsigned long)r->rnd,(long)r->late,(unsigned long)r->other,
            (unsigned long)r->prev_rnd,(long)r->camera_mode,(unsigned)r->camera_transition);
#ifdef NM_DIAG_LATE_CAPTURE
        {
            const int slot=nm_late_find(r->seq,0);
            const nm_late_record_t *x=slot<0 ? NULL : nm_late_record(slot);
            printf(" img=%s",x && (x->flags&NM_LATE_COPIED) ? drop_capture_kinds[x->kind] : "none");
        }
#endif
        printf("%s\n",r->untimed ? " untimed=1" : "");
    }
    for(unsigned p=0;p<DIAG_PHASES;++p) {
        if(!drop_phase_frames[p]) continue;
        const uint32_t n=drop_phase_frames[p];
        printf("DROP-PHASE phase=%s frames=%lu late=%lu missed=%lu avg_upd=%lu avg_reg=%lu avg_rnd=%lu",
            practice_diag_phase_names[p],(unsigned long)n,(unsigned long)drop_phase_late[p],(unsigned long)drop_phase_missed[p],
            (unsigned long)(drop_phase_upd[p]/n),(unsigned long)(drop_phase_reg[p]/n),(unsigned long)(drop_phase_rnd[p]/n));
        for(unsigned c=0;c<DROP_CAUSES;++c) printf(" %s=%lu",drop_cause_names[c],(unsigned long)drop_phase_cause[p][c]);
        putchar('\n');
    }
    for(unsigned b=0;b<DROP_BUCKETS;++b) {
        if(!drop_bucket_frames[b]) continue;
        printf("DROP-TICKS run=%lu-%s frames=%lu late=%lu",(unsigned long)(b*DROP_BUCKET_TICKS),
            b==DROP_BUCKETS-1 ? "end" : "",(unsigned long)drop_bucket_frames[b],(unsigned long)drop_bucket_late[b]);
        if(b<DROP_BUCKETS-1) printf(" last=%lu",(unsigned long)((b+1)*DROP_BUCKET_TICKS-1));
        for(unsigned c=0;c<DROP_CAUSES;++c) printf(" %s=%u",drop_cause_names[c],(unsigned)drop_bucket_cause[b][c]);
        putchar('\n');
    }
    printf("DROP-SUMMARY frames=%lu late=%lu late_permille=%lu missed_vblanks=%lu",(unsigned long)frames,(unsigned long)late,
        (unsigned long)(frames ? (uint64_t)late*1000u/frames : 0),(unsigned long)missed);
    for(unsigned c=0;c<DROP_CAUSES;++c) printf(" %s=%lu",drop_cause_names[c],(unsigned long)causes[c]);
    printf(" unmatched=%lu run_ticks=%lu flips=%lu displayed=%lu other_mode=%lu unflipped=%lu overrun=%lu vblanks=%lu",
        (unsigned long)drop_unmatched,(unsigned long)drop_run,(unsigned long)drop_flip_events,(unsigned long)drop_displayed,
        (unsigned long)drop_other_frames,(unsigned long)drop_unflipped,(unsigned long)drop_overrun,(unsigned long)vbls);
    for(unsigned a=0;a<DROP_ANOMALIES;++a) printf(" %s=%lu",drop_anomaly_names[a],(unsigned long)drop_anomaly[a]);
    putchar('\n');
#ifdef NM_DIAG_LATE_CAPTURE
    drop_capture_print();
#endif
#ifdef NM_DIAG_FRAME_TRACE
    printf("FT-RUN v1: rows=%u window=%u: run upd wait reg rnd late vbl other phase\n",ft_row_count,ft_win_count);
    for(unsigned i=0;i<ft_row_count;++i) {
        const ft_row_t *r=ft_rows+i;
        printf("FT-R %u %u %u %u %u %d %u %u %u\n",(unsigned)r->run,(unsigned)r->upd,(unsigned)r->wait,(unsigned)r->reg,(unsigned)r->rnd,
            (int)r->late,(unsigned)r->vbl,(unsigned)r->other,(unsigned)r->phase);
    }
    static const char *const ft_names[NM_FT_GROUPS]={NM_FT_NAMES};
    for(unsigned i=0;i<ft_win_count;++i) {
        const ft_win_t *w=ft_window+i;
        const ft_scene_t *f=&w->s;
        printf("FT-W run=%lu tick=%lu upd=%lu sim=%lu setup=%lu prep=%lu wait=%lu reg=%lu cpu=%lu rnd=%lu late=%ld vbl=%lu other=%lu "
            "opp=%lu world=%lu parts=%lu shadows=%lu",(unsigned long)w->run,(unsigned long)w->tick,(unsigned long)w->upd,
            (unsigned long)f->sim,(unsigned long)f->setup,(unsigned long)f->prep,(unsigned long)w->wait,(unsigned long)w->reg,
            (unsigned long)ft_us(f->cpu_end),(unsigned long)w->rnd,(long)w->late,(unsigned long)w->vbl,(unsigned long)w->other,
            (unsigned long)f->opponents,(unsigned long)f->world,(unsigned long)f->part_count,(unsigned long)f->shadows);
        for(unsigned l=0;l<FT_LISTS;++l) printf(" l%u=%lu ta%u=%lu tri%u=%lu hdr%u=%lu vtx%u=%lu",l,(unsigned long)ft_us(f->list[l]),l,
            (unsigned long)f->ta[l],l,(unsigned long)f->counts[l][0],l,(unsigned long)f->counts[l][1],l,(unsigned long)f->counts[l][2]);
        for(unsigned g=0;g<NM_FT_GROUPS;++g) printf(" %s=%lu/%lu",ft_names[g],(unsigned long)ft_us(f->group[g]),(unsigned long)f->calls[g]);
        printf(" ce=%lu,%lu,%lu,%lu/%lu,%lu,%lu,%lu\n",(unsigned long)f->emit[0][0],(unsigned long)f->emit[0][1],(unsigned long)f->emit[0][2],
            (unsigned long)f->emit[0][3],(unsigned long)f->emit[1][0],(unsigned long)f->emit[1][1],(unsigned long)f->emit[1][2],
            (unsigned long)f->emit[1][3]);
        printf("FT-O run=%lu",(unsigned long)w->run);
        for(unsigned o=0;o<f->object_count && o<NM_FT_OBJECTS;++o) {
            const nm_ft_object_t *x=f->objects+o;
            printf(" %u:%u:%u:%lu:%lu:%lu:%u:%u",(unsigned)x->mesh,(unsigned)x->faces,(unsigned)x->vertices,(unsigned long)ft_us(x->prep),
                (unsigned long)ft_us(x->op),(unsigned long)ft_us(x->pt),(unsigned)x->tri_op,(unsigned)x->tri_pt);
        }
        printf("\nFT-C run=%lu",(unsigned long)w->run);
        for(unsigned o=0;o<f->car_count && o<NM_FT_CARS;++o) {
            const nm_ft_car_t *c=f->cars+o;
            printf(" %u:%u:%u:%u:%lu:%lu:%lu:%u",(unsigned)c->first,(unsigned)c->level,(unsigned)c->drawn,(unsigned)c->rows,
                (unsigned long)ft_us(c->prep),(unsigned long)ft_us(c->op),(unsigned long)ft_us(c->pt),(unsigned)c->tri);
        }
        putchar('\n');
    }
#endif
}
#endif
#if defined(NM_DIAG_DROP_LOG) || defined(NM_DIAG_PMU)
static void practice_diag_registration_begin(void *unused) {
#ifdef NM_DIAG_PMU
    pmu_registration_begin(unused);
#endif
    (void)unused;
}
static void practice_diag_registration_end(void *unused) {
#ifdef NM_DIAG_PMU
    pmu_registration_end(unused);
#endif
#ifdef NM_DIAG_LATE_CAPTURE
    drop_capture_point();   /* picture copies: here, before the punch-through list closes (see the drop log) */
#endif
    (void)unused;
}
static void practice_update_measured(void *unused) {
#ifdef NM_DIAG_DROP_LOG
    drop_updater_begin();
#endif
#ifdef NM_DIAG_PMU
    pmu_updater_begin();
#endif
    enj_render_list_add(PVR_LIST_OP_POLY,practice_diag_registration_begin,NULL);
    practice_update(unused);
#ifdef NM_DIAG_PMU
    pmu_updater_end();
#endif
#ifdef NM_DIAG_DROP_LOG
    drop_updater_end();
#endif
    enj_render_list_add(PVR_LIST_PT_POLY,practice_diag_registration_end,NULL);
}
static void practice_diag_print(void) {
#ifdef NM_DIAG_PMU
    pmu_print();
#endif
#ifdef NM_DIAG_DROP_LOG
    drop_print();
#endif
}
#endif
/* The V.R. demo (phases 6/7, vr_demo.h) after the cinematic: the cinematic's cars (its last stage's count) on the
 * race grid of its course, in the race scene, drawn by the race's presentation (attract_active 0; the HUD is the
 * race phases' only). Its own mode: practice_update and the race's code stay as they are. Each tick: the demo's
 * objects (nm_demo_step), then what the race's tick runs of the presentation that the demo's phase runs too (the
 * windows' reflection, the props, the camera row with the view presets, the spin smoke). No sound: the original's
 * demo sends none. START or A: the credit (that tick runs, then the game start on the demo's cars, phases 12/17);
 * both triggers and START: the settings. */
#ifdef NM_RACE_VIEW_DATA
static nm_demo_t practice_demo;
static uint32_t practice_demo_counter; /* 5010b0 of the phase-6 iteration */
static uint32_t practice_demo_frames,practice_demo_late,practice_demo_longest_us;
/* The PVR's registration and render times of the demo's frames (pvr_get_stats: the frame that completed last). */
static uint64_t practice_demo_reg_ns,practice_demo_rnd_ns,practice_demo_reg_max_ns,practice_demo_rnd_max_ns;
static uint32_t practice_demo_pvr_samples;
static void practice_demo_end(void) {
    const uint32_t n=practice_demo_pvr_samples ? practice_demo_pvr_samples : 1u;
    printf("Native V.R. demo: course %u, %lu ticks, %u frames, %u late (longest %lu us); PVR registration avg/max "
        "%lu/%lu us, render %lu/%lu us\n",(unsigned)practice.course,
        (unsigned long)practice.race.ticks,(unsigned)practice_demo_frames,(unsigned)practice_demo_late,
        (unsigned long)practice_demo_longest_us,(unsigned long)(practice_demo_reg_ns/n/1000u),
        (unsigned long)(practice_demo_reg_max_ns/1000u),(unsigned long)(practice_demo_rnd_ns/n/1000u),
        (unsigned long)(practice_demo_rnd_max_ns/1000u));
    /* Phase 8: the objects off, the field pass over them, then what the demo leaves. */
    if(!nm_demo_field_off(&practice.race,practice.math,&practice.road,8,practice_demo_counter+1u+NM_DEMO_TICKS)) {
        practice_reject_step="demo";practice.failed=1;practice_report_reject();return;
    }
    nm_frontend_pool_from_race(&practice_pool,&practice.race,&practice.intro.cells);
    nm_attract_2d_off();
    nm_frontend_counter_set(practice_demo_counter-1u+NM_ATTRACT_DEMO_ITERATIONS); /* phases 6, 7 and 8 */
    practice_attract_course=nm_attract_next_course(practice.course);
    nm_practice_render_release();
    if(!nm_title_mode_start()) enj_state_flag_shutdown(NULL);
}
/* credit: 2028 saw one in this iteration and goes to 2818 before its timer and 2058, so the course stays. */
static __attribute__((noinline)) int practice_demo_tick(int credit) {
    const uint32_t frame=practice_demo_counter+1u+practice.race.ticks; /* this iteration's 5010b0 */
    const int last=!credit && practice.race.ticks+1u>=NM_DEMO_TICKS;
    if(!nm_demo_step(&practice.race,&practice_demo,practice.math,&nm_intro_speeds,&practice.contact,&practice.road,
        practice.intro_paths,&practice.marks,practice.road.profile_count,frame&3u,(uint8_t)(frame&1u),
        nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count,
        last ? nm_attract_next_course(practice.course) : practice.course)) { practice_reject_step="demo";return 0; }
    nm_attract_2d_demo(7,frame);
    const nm_player_t *car=&practice.race.cars[0].car;
    const nm_reflection_input_t reflection={.frame=practice.race.ticks-1,
        .speed_index=(uint32_t)car->speed_index,.flags=car->path_flags,
        .previous_flags=car->previous_path_flags,.heading=practice_reflection_heading(car),
        .course=practice.course,.next_mode=practice.race.phase,.active_mode=practice.race.active_phase};
    nm_reflection_update_t update;
    if(!nm_reflection_step(&reflection,&practice.intro.reflection_phase,&update)) { practice_reject_step="reflection";return 0; }
    if(update.action==NM_REFLECTION_UV) { practice.intro.reflection_u=update.u_offset;practice.intro.reflection_v=update.v_offset; }
    if(update.action==NM_REFLECTION_TUNNEL || update.action==NM_REFLECTION_RESTORE)
        (void)nm_practice_render_tunnel(&practice,update.action==NM_REFLECTION_TUNNEL,update.others_only);
    (void)nm_props_race_step(&practice.race,practice.math,&practice.contact); /* no sound in the demo */
    practice_view_held=0; /* 224ef4 sets the view (nm_demo_step) */
    if(!practice_race_detail()) { practice_reject_step="race detail";return 0; }
    practice.race.course=(uint8_t)practice.course; /* the last tick's camera row read the next course */
    const unsigned owner=practice.race.shared.inhibit_owner<practice.race.count ? practice.race.shared.inhibit_owner : 0u;
    if(!nm_spin_smoke_tick(practice.math,&practice.spin_smoke,practice.race.active_phase,
        &practice.race.cars[owner].car,&practice.race.cars[0].car,practice.race.view)) { practice_reject_step="spin smoke";return 0; }
    return 1;
}
static void practice_demo_update(void *unused) {
    (void)unused;
    nm_sound_update();
    if(enj_state_get()->flags.shut_down || practice.failed) { nm_practice_render_release();enj_state_flag_shutdown(NULL);return; }
    const uint64_t now=timer_ns_gettime64()/1000;
    uint64_t elapsed=practice.last_us ? now-practice.last_us : 0;practice.last_us=now;
    const uint32_t frame_us=nm_frame_ticks(&practice.frame_clock)*NM_PRACTICE_TICK_US; /* the simulation's time */
    ++practice_demo_frames;
    if(elapsed>practice_demo_longest_us) practice_demo_longest_us=(uint32_t)elapsed;
    practice_demo_late+=elapsed>25000u; /* a frame shown twice or more */
    if(practice_demo_frames>2) {
        pvr_stats_t stats;
        if(!pvr_get_stats(&stats)) {
            practice_demo_reg_ns+=stats.reg_last_time;practice_demo_rnd_ns+=stats.rnd_last_time;++practice_demo_pvr_samples;
            if(stats.reg_last_time>practice_demo_reg_max_ns) practice_demo_reg_max_ns=stats.reg_last_time;
            if(stats.rnd_last_time>practice_demo_rnd_max_ns) practice_demo_rnd_max_ns=stats.rnd_last_time;
        }
    }
    if(practice.race.ticks>=NM_DEMO_TICKS) { practice_demo_end();return; }
#if !defined(NATIVE_RACE_SCRIPT) && !defined(NATIVE_DRIVING_TEST)
    enj_ctrlr_state_t **controllers=enj_ctrl_get_states();
    for(size_t i=0;i<enj_ctrl_states_length();++i) if(controllers[i]) {
        const enj_ctrlr_state_t *c=controllers[i];
        if(c->button.START==ENJ_BUTTON_DOWN_THIS_FRAME || c->button.A==ENJ_BUTTON_DOWN_THIS_FRAME) {
            if(nm_select_attract_button(c->button.START==ENJ_BUTTON_DOWN_THIS_FRAME,c->ltrigger,c->rtrigger)) return;
            const uint32_t credit=practice_demo_counter+1u+practice.race.ticks;
            if(!practice_demo_tick(1)) { practice.failed=1;practice_report_reject();return; }
            nm_attract_2d_off();
            nm_practice_render_release();
            if(!nm_select_mode_start(NM_CREDIT_DEMO,credit)) enj_state_flag_shutdown(NULL);
            return;
        }
        break;
    }
#endif
    elapsed=frame_us;
    if(elapsed>4*NM_PRACTICE_TICK_US) { practice.state.dropped_us+=elapsed-4*NM_PRACTICE_TICK_US;elapsed=4*NM_PRACTICE_TICK_US; }
    practice.state.remainder_us+=(uint32_t)elapsed;
    while(practice.state.remainder_us>=NM_PRACTICE_TICK_US && practice.race.ticks<NM_DEMO_TICKS) {
        if(!practice_demo_tick(0)) {
            practice.failed=1;practice_report_reject();return;
        }
        practice.state.remainder_us-=NM_PRACTICE_TICK_US;
    }
    NM_COPY(&practice.state.car,&practice.race.cars[0].car);practice.state.ticks=practice.race.ticks;
    practice.intro.ticks=practice.race.ticks;
    if(!nm_practice_render_enqueue(&practice) || !nm_attract_2d_render_enqueue()) practice.failed=1;
}
#if defined(NM_DIAG_DROP_LOG) || defined(NM_DIAG_PMU)
static void practice_demo_update_measured(void *unused) {
#ifdef NM_DIAG_DROP_LOG
    drop_updater_begin();
#endif
    enj_render_list_add(PVR_LIST_OP_POLY,practice_diag_registration_begin,NULL);
    practice_demo_update(unused);
#ifdef NM_DIAG_DROP_LOG
    drop_updater_end();
#endif
    enj_render_list_add(PVR_LIST_PT_POLY,practice_diag_registration_end,NULL);
}
static enj_mode_t practice_demo_mode={.name="Native V.R. demo",.mode_updater=practice_demo_update_measured};
#else
static enj_mode_t practice_demo_mode={.name="Native V.R. demo",.mode_updater=practice_demo_update};
#endif
/* 1: the demo runs (its mode is set); 0 with practice.failed clear: this build counts it instead. */
static int practice_demo_start(void) {
    const nm_race_course_start_t *tables=nm_race_course_starts+practice.course;
    if(practice.course>2 || !tables->tracks || practice.road.slot!=practice.course) return 0;
    practice_demo_counter=practice.intro.object_clock+practice.intro.ticks+1u;
    nm_frontend_pool_from_intro(&practice_pool,&practice.intro,&practice.road);
    const unsigned count=practice.intro.count;
    practice_cameras.tracks=tables->tracks;practice_cameras.tunnel=tables->tunnel;practice_cameras.pit_views=tables->pit_views;
    const char *stage="demo start";
    if(!nm_demo_start(&practice.race,&practice_demo,practice.math,&practice.contact,&practice.road,practice.intro_paths,
        count,practice_pool.view,nm_race_view_presets,&practice.marks,practice.road.profile_count,&practice_pool,
        practice_demo_counter&3u)) goto fail;
    practice.race.counter_base=practice_demo_counter+1u; /* the clock actors' 5010b0: counter_base + ticks - 1 */
    stage=nm_demo_view_start(&practice.race,&practice.intro,practice.math,&practice.scenery,&practice_cameras,&practice_pool);
    if(stage) goto fail;
    nm_attract_2d_demo(6,practice_demo_counter);
    practice.attract_active=0;practice.race_bodies=0;practice_view_held=0;
    nm_spin_smoke_reset(&practice.spin_smoke);
    NM_COPY(&practice.state.car,&practice.race.cars[0].car);practice.state.ticks=0;
    practice_demo_frames=practice_demo_late=practice_demo_longest_us=practice_demo_pvr_samples=0;
    practice_demo_reg_ns=practice_demo_rnd_ns=practice_demo_reg_max_ns=practice_demo_rnd_max_ns=0;
    enj_mode_set(&practice_demo_mode);return 1;
fail:
    printf("FAIL: native V.R. demo start at %s\n",stage);
    practice.failed=1;return 0;
}
#else
static int practice_demo_start(void) { return 0; }
#endif
int nm_practice_run(const char *root) {
    practice.asset_root=root;
#if !defined(NATIVE_RACE_SCRIPT) && !defined(NATIVE_DRIVING_TEST)
    nm_steer_table_build(practice_steer_table,NM_STEER_CURVE,NM_STEER_DEADZONE);
#endif
    nm_frontend_pool_reset(&practice_pool);
    practice_root=root;
    if (!enj_mode_push(&practice_mode)) return 1;
    nm_sound_init(root); /* Failure is reported there; the game then runs silent. */
    nm_race_sound_hook_reset(1); /* 18f30: once per power-on, not per race. */
    /* Sample profile builds: the buffer first, below every asset (heap.h). */
    nm_sample_reserve();
#ifdef NM_DIAG_LATE_CAPTURE
    nm_late_reserve();   /* the late-capture pool likewise (late_capture.h) */
#endif
    nm_loader_init();    /* the attract's loads in the title's frames (scene_loader.h): its thread, before the assets */
    nm_heap_report("before the assets");
    char file[256]; int ok = 0;
    const char *stage="physics assets";
    if(!nm_course_asset(file,sizeof(file),root,0,NM_COURSE_DRIVE,0) || !(practice.math=nm_drive_load(file))) goto done;
    /* Settings and records: the defaults, then the VMU's save (a scripted race keeps the defaults). */
#ifdef NATIVE_RACE_SCRIPT
    nm_game_save_boot(nm_race_record_seeds,0);
#else
    nm_game_save_boot(nm_race_record_seeds,1);
#endif
    stage="attract 2D layer";
    if(!nm_attract_2d_render_load(root)) goto done; /* resident from power-on (render/ui/attract_2d_render.h) */
    /* The course packs: the race's course (an AUTO_START race) or the attract's (course 0); the scene start loads
     * the packs of another course in their place (practice_course_load). */
    stage="course packs";
#if defined(DAYTONA_AUTOSTART) && DAYTONA_AUTOSTART != 2
    if(!practice_course_load(practice_race_course)) goto done;
#else
    if(!practice_course_load(0)) goto done;
#endif
#ifdef DAYTONA_AUTOSTART
#if DAYTONA_AUTOSTART == 2
    /* The attract from power-on, as without AUTO_START: its entry also starts the advertisement music. */
    nm_frontend_counter_set(1u);
    if(!nm_attract_mode_start()) goto done;
#else
    if(!nm_practice_restart_from_title()) goto done;
#endif
#else
    /* Power-on: phase 0 is iteration 1, phase 2 (the attract loop's entry) iteration 2. */
    stage="attract presentation";
    nm_frontend_counter_set(1u);
    if(!nm_attract_mode_start()) goto done;
#endif
    printf("Native practice: persistent player, tick=%u us; asset payload=%lu bytes; PVR free=%lu\n",
        NM_PRACTICE_TICK_US,(unsigned long)(sizeof(*practice.math)+practice.contact.payload_bytes+
        practice.road.payload_bytes+practice.path.records*sizeof(*practice.path.rows)),
        (unsigned long)pvr_mem_available());
    ok = 1;
done:
    if (!ok) { printf("FAIL: native practice setup at %s (root=%s)\n",stage,root); nm_practice_render_release(); enj_state_flag_shutdown(NULL); }
#ifdef NATIVE_SAMPLE_PROFILE
    enj_render_post_callback_set(practice_sample_frame_end,NULL);
    nm_sample_start();
#elif defined(NATIVE_STAGE_TRACE)
    enj_render_post_callback_set(stage_trace_mark,(void *)"TRACE scene finished (frame handed to the PVR)");
#endif
    enj_state_run();
    nm_sound_shutdown();
    nm_sample_finish(root);
    nm_title_render_release();
    if (practice.intervals>=600 && practice.pvr_samples) {
        const pvr_stats_t *s=&practice.sampled_stats;
        const struct mallinfo *m=&practice.sampled_memory;
        printf("Practice PVR n=%u register avg/max=%llu/%llu us render avg/max=%llu/%llu us ready=%llu us TA=%lu/%lu bytes\n",
            practice.pvr_samples,(unsigned long long)(practice.pvr_register_sum/practice.pvr_samples/1000),
            (unsigned long long)(practice.pvr_register_max/1000),
            (unsigned long long)(practice.pvr_render_sum/practice.pvr_samples/1000),
            (unsigned long long)(practice.pvr_render_max/1000),(unsigned long long)(s->frame_last_time/1000),
            (unsigned long)s->vtx_buffer_used,(unsigned long)s->vtx_buffer_used_max);
#ifdef NM_DIAG_RENDER_STATS
        practice_diag_render_print();
#endif
        printf("Practice allocator at600 arena/used/free=%lu/%lu/%lu bytes; PVR free=%u (not total system free RAM)\n",
            (unsigned long)m->arena,(unsigned long)m->uordblks,(unsigned long)m->fordblks,practice.sampled_vram);
        printf("Practice heap headroom at600: uncommitted=%lu reusable-free=%lu total=%lu bytes (fragmentation applies; kernel stack reserved)\n",
            (unsigned long)practice.sampled_heap_headroom,(unsigned long)m->fordblks,
            (unsigned long)(practice.sampled_heap_headroom+m->fordblks));
    }
#if defined(NM_DIAG_DROP_LOG) || defined(NM_DIAG_PMU)
    practice_diag_print();
#endif
#ifdef NM_DIAG_LATE_CAPTURE
    drop_capture_write(root);
#endif
    printf("Native practice end: ticks=%lu dropped=%lu us update-max=%llu us failed=%d\n",
        (unsigned long)practice.state.ticks,(unsigned long)practice.state.dropped_us,
        (unsigned long long)practice.update_max_us,practice.failed);
    if(practice.attract_active) printf("Native attract end: stage=%lu transitions=%lu cars=%lu ticks=%lu pairs=%lu responses=%lu\n",
        (unsigned long)practice.intro.stage,(unsigned long)practice.intro.transitions,
        (unsigned long)practice.intro.count,(unsigned long)practice.intro.ticks,
        (unsigned long)practice.intro.collision_pairs,(unsigned long)practice.intro.collision_responses);
    unsigned released=0;
#ifdef NATIVE_RACE_CHECKPOINTS
    for(unsigned i=0;i<checkpoint_count;++i) nm_race_checkpoint_print(stdout,checkpoints[i]);
    printf("Native checkpoints: rows=%u ticks=%lu\n",checkpoint_count,(unsigned long)checkpoint_tick);
#endif
    printf("Native result silent events: rank=%u commands=%lu hash=%08lx cursor=%lu; no audio playback\n",
        practice.race.cars[0].rank,(unsigned long)practice.results.commands,(unsigned long)practice.results.command_hash,
        (unsigned long)practice.results.announcement_cursor);
    printf("Native race view ticks: %lu/%lu/%lu/%lu (phase22 only, before camera update)\n",
        (unsigned long)practice.race.view_ticks[0],(unsigned long)practice.race.view_ticks[1],
        (unsigned long)practice.race.view_ticks[2],(unsigned long)practice.race.view_ticks[3]);
    printf("Native race silent events: commands=%u hash=%08lx cursor=%lu repeats=%lu\n",practice.race_commands,
        (unsigned long)practice.race_command_hash,(unsigned long)practice.race.announcement_cursor,
        (unsigned long)practice.race.intents.repeats);
    nm_race_sound_hook_report();
    nm_props_report();
    nm_slot_glass_report();
    printf("Native race controller: samples=%u last steer/gas/brake=%ld/%u/%u start-edges=%u (physical input range/chord still require user verification)\n",
        practice.controller_samples,(long)practice.controller_last.steer,
        practice.controller_last.accel,practice.controller_last.brake,practice.start_edges);
    printf("Native race transmission at exit: %s, pad shift requests=%u\n",
        practice.transmission ? "manual" : "automatic",practice_shift_requests);
    printf("Native player end: profile=%u laps=%u/%u speed-index=%ld drag=%d finish=%08lx position-bits=%08lx/%08lx\n",
        (uint16_t)practice.state.car.phase[0],practice.state.car.lap.lap,practice.state.car.lap.completed,
        (long)practice.state.car.speed_index,(int)practice.state.car.drag_state,
        (unsigned long)practice.state.car.checkpoints.finish_flags,
        (unsigned long)nm_force_bits(practice.state.car.boundary.ground.position[0]),
        (unsigned long)nm_force_bits(practice.state.car.boundary.ground.position[2]));
    for(unsigned i=0;i<practice.race.count;++i) released+=practice.race.rolling[i]!=1;
#ifdef NATIVE_RACE_FINISH_TEST
    const int finish_matched=practice.race.ticks==2187 &&
        (practice.race.phase==25 || practice.race.phase==27
         || (practice.race.phase==29 && practice.results.ticks==674 && practice.results.playback.clock==449)
        ) && practice.race.active_phase==24 &&
        practice.race.finish.timer==UINT32_MAX && practice.race.rolling[0]==3 &&
        practice.race.shared.timer_hold==1 && practice.race.finish_branch==2 &&
        practice.race.finish_actor.timer==(uint16_t)(256-386) && practice.race.finish_actor.tiny_y_bits==0x1ffb8;
    printf("Native injected finish lifecycle2187: %s (386 coast ticks; result rendering NOT verified)\n",finish_matched ? "PASS" : "FAIL");
    if(!finish_matched) practice.failed=1;
#endif
    printf("Native race preview: phase=%u ticks=%lu released=%u clock=%lu remaining=%lu pairs=%lu responses=%lu body-ID-mask=%010llx; inputs=enDjinn controller, previous-tick scenery masks, silent events; result presentation/later phases pending\n",
        practice.race.phase,(unsigned long)practice.race.ticks,released,(unsigned long)practice.race.shared.clock,
        (unsigned long)practice.race.award.remaining,(unsigned long)practice.race.collisions.pairs,
        (unsigned long)practice.race.collisions.responses,(unsigned long long)practice.race_bodies);
    printf("Native car effects: %lu punch-through draws submitted (dust, sparks; include/car_effects.h)\n",
        (unsigned long)practice.effects_drawn);
    printf("Native car effects: %lu tyre smoke and %lu spin smoke draws submitted (translucent list, pre-sorted); "
        "spin smoke %lu ticks, %lu particles emitted\n",(unsigned long)practice.smoke_drawn,
        (unsigned long)practice.spin_drawn,(unsigned long)practice.spin_smoke.ticks,(unsigned long)practice.spin_smoke.emitted);
    printf("Native pit crew: %lu part draws submitted (include/pit_crew.h; %u models looked up), %lu with their figure's "
        "bounds hint\n",(unsigned long)practice.crew_drawn,practice.crew_model_count,(unsigned long)practice.crew_hinted);
    if (practice.intervals) printf("Native practice whole-loop: n=%u avg/max=%llu/%llu us (includes render/waits/input)\n",
        practice.intervals,(unsigned long long)(practice.interval_sum_us/practice.intervals),
        (unsigned long long)practice.interval_max_us);
#if defined(NATIVE_PROFILE) || defined(NATIVE_FRAME_PROFILE)
    if(practice.update_samples) printf("Native CPU scopes avg: n=%u bookkeeping/simulation=%llu camera/parts/scenery=%llu render-preparation=%llu total=%llu us (disjoint scopes; render scopes may execute in callbacks; excludes engine waits)\n",
        practice.update_samples,(unsigned long long)(practice.simulation_us/practice.update_samples),
        (unsigned long long)(practice.scene_setup_us/practice.update_samples),
        (unsigned long long)(practice.render_prepare_us/practice.update_samples),
        (unsigned long long)((practice.simulation_us+practice.scene_setup_us+practice.render_prepare_us)/practice.update_samples));
#endif
#ifdef NATIVE_FRAME_PROFILE
    printf("Native frame budget after64: n=%lu over18ms=%lu over25ms=%lu max=%llu us\n",
        (unsigned long)frame_budget_samples,(unsigned long)frame_over_18ms,
        (unsigned long)frame_over_25ms,(unsigned long long)frame_steady_max_us);
    if(frame_flip_samples) printf("Native displayed frames after64: flips=%lu vblanks=%lu samples=%lu avg/max=%llu/%llu us over20ms=%lu (actual PVR page flips, not CPU updater intervals)\n",
        (unsigned long)frame_flips,(unsigned long)frame_vblanks,(unsigned long)frame_flip_samples,
        (unsigned long long)(frame_flip_sum_ns/frame_flip_samples/1000),
        (unsigned long long)(frame_flip_max_ns/1000),(unsigned long)frame_flip_over_20ms);
#endif
    practice_course_unload(); free(practice.math);
    free(practice.points); nm_pack_unload(&practice.model); nm_damage_bodies_unload(&practice.damage_bodies);
    free(practice.world_instances); nm_pack_unload(&practice.world);
    return !ok || practice.failed;
}
