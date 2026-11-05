/* Host-only free-running native race trace. NOT a pass/fail test and no gameplay
 * math: it runs nm_race_scene_start and then the production per-tick sequence of
 * code/practice.c (scene step, events, nm_player_effect_timers, the 9148 contact clear, reflection,
 * wrong way, slot machine, follow-view/scenery/detail publication) with per-tick player controls taken
 * from a reference trace written by `m2segment --trace-race`, and writes the
 * same text format for tools/compare_race_trace.py.
 *
 * usage: trace_race_native ASSET_DIR REFERENCE_TRACE NEW_OUTPUT FIRST_ROW MAX_TICKS MODE
 *   FIRST_ROW  reference row (n=) whose controls feed the first native tick.
 *   MODE       production : exactly what practice.c supplies (decoded controls
 *                           and the START edge `recover`, 53e584 bit4, as
 *                           practice.c latches it; every other tick input zero,
 *                           the difficulty of --difficulty (default NORMAL, 1),
 *                           timer gate/hold0, pedal edges as
 *                           practice.c derives). The slot machine counter is
 *                           counter_base+tick (2368, or the native front end's with
 *                           --frontend); `scene` is only checked.
 *              reference  : additionally feeds the original's per-tick external
 *                           inputs recorded in the trace (difficulty, timer
 *                           gate/hold, award block/penalty/suppression, gear
 *                           selector, transmission, variant, real pedal
 *                           edges). Diagnostic only; no car state is copied.
 *              seeded     : reference inputs, and the start is re-entered per car
 *                           from what the reference shows at row FIRST_ROW-1:
 *                           cars the original leaves in plain grid-reset state
 *                           (lap0, ahead of the line) get no rolling entry, and a
 *                           car whose reset profile differs is given the prior
 *                           road hint (entity+30) that reproduces it. Only native
 *                           entry routines run; no pose/speed is copied across.
 *                           Shows what happens AFTER the start-state findings.
 *              seeded+guided / seeded+guidednow : seeded, and race.shared.guided_mode
 *                           (original 5014f7, which no native code writes) is set
 *                           before each tick from the reference's value at the end
 *                           of the previous / the same frame. Hypothesis test only.
 * Build with the project's float flags (-fno-fast-math -ffp-contract=off, as
 * CMake and local.cfg.mk do): a contracting host compiler changes on-edge
 * contact results, e.g. grid cars 17/18 sit exactly on a shared quad edge.
 * Generated ROM-derived headers and asset packs stay local under build/. */
#undef NDEBUG
#include "race_scene.h"
#include "race_intent_data.h"
#include "race_start_data.h"
#include "race_record_data.h"
#include "podium.h"
#include "race_checkpoint.h"
#include "intro_scene.h"
#include "visibility.h"
#include "pose.h"
#include "podium_data.h"
#include "player_parts.h"
#include "pit_crew.h"
#include "car_effects.h"
#include "race_camera.h"
#include "settings.h"
#include "frontend.h"
#include "game_chain.h"
#include "course_assets.h"
#include "clock_scene.h"
#include "props_scene.h"
#include "result_actor.h"
#ifdef NM_TRACE_PARTS /* --parts: link code/meshpack.c and code/render/scene/damage_bodies.c (CMake target trace_race_native) */
#include "meshpack.h"
#include "render/scene/damage_bodies.h"
#endif
#include "hud_scene.h"
#include "hud_dump.h"
#include "race_sound.h"
#include "audio/music_select.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    long n, frame;
    int32_t steer;
    unsigned accel, brake, gearsel, manual, difficulty, penalty, suppress, blocked, variant, recover, gate, hold;
    unsigned guided, guided_before;
    uint32_t scene; /* 5010b0, UINT32_MAX when the trace predates the field. */
    uint32_t held;  /* The view buttons 53e120 (race_view.h layout) of the trace's vr=, 0 when it predates it. */
} reference_row_t;

static int value(const char *line,const char *key,long long *out) {
    char pattern[32];
    snprintf(pattern,sizeof(pattern)," %s=",key);
    const char *at=strstr(line,pattern);
    if(!at) return 0;
    *out=strtoll(at+strlen(pattern),NULL,10);return 1;
}
static long long need(const char *line,const char *key) {
    long long v;
    if(!value(line,key,&v)) { fprintf(stderr,"reference trace row lacks %s\n",key);exit(2); }
    return v;
}

static nm_race_scene_t race;
static nm_intro_scene_t presentation; /* Only view/cells/detail/reflection, as practice.c. */
static nm_drive_params_t *math;
static nm_contact_pack_t contact;
static nm_road_pack_t road;
static nm_path_pack_t paths[8];
static nm_scenery_pack_t scenery;
static nm_road_marks_t marks;
static nm_race_sound_t race_sound; /* From power-on, as the reference: main sets NM_RACE_SOUND_BOOT. */
static uint8_t front_smoke;        /* 501aa0, from power-on */
static nm_race_sound_beep_t beep;  /* 501880/501884, from power-on */
static nm_race_sound_cooldowns_t cooldowns; /* 501a9c/501aa4, from power-on */
static uint8_t ambient[2]; /* 503068/503069: 0 at phase 20 (code/audio/race_sound.c nm_race_sound_hook_reset) */

static uint32_t bits(float value) { uint32_t out;memcpy(&out,&value,4);return out; }

/* --frontend: the native attract, game start and select screens (frontend.h, select.h) fed by the
 * original's front-end trace (m2segment --trace-frontend): the cinematic runs to the iteration in which
 * the original saw the credit, nm_frontend_game_start takes the two iterations after it, and the select
 * runs one iteration per F row with that row's inputs. Out: the carried pool, the chosen course and
 * transmission, and 5010b0 of the race's phase20 tick (two iterations after the select's last). */
static nm_intro_scene_t frontend_attract;
/* entered: the attract ran in trace_attract_native (--enter): the pool holds what the game start left and the select
 * starts after the credit's iteration ENTERED_CREDIT. */
static int frontend_from_trace(const char *path,nm_entity_pool_t *pool,unsigned *course,unsigned *transmission,
        uint32_t *counter_base,int entered,uint32_t entered_credit) {
    FILE *in=fopen(path,"r");
    if(!in) { fprintf(stderr,"cannot open front-end trace %s\n",path);return 0; }
    static char line[4096];
    long long start=-1;int phase=0;nm_select_t select;nm_select_output_t sounds={{0},0};uint32_t counter=0;
    while(fgets(line,sizeof(line),in)) {
        if(line[0]!='F' || line[1]!=' ') continue;
        const long long b0=need(line,"b0"),active=need(line,"active"),next=need(line,"phase");
        if(!phase && entered) {
            if((uint32_t)b0!=entered_credit) continue;
            start=b0;nm_select_enter(&select);counter=(uint32_t)start+2u;phase=18;
        } else if(!phase) {
            if(active!=3 || next!=12) continue;
            /* The credit's iteration: an ordinary cinematic tick; tick k's state is iteration k+3's. */
            start=b0;
            if(start<3 || !nm_intro_scene_start(&frontend_attract,math,&road,paths) ||
               !nm_intro_scene_detail(&frontend_attract,math,&scenery)) { fputs("front end: attract start refused\n",stderr);break; }
            int ok=1;
            for(long long t=0;ok && t<start-3;++t)
                ok=nm_intro_scene_step(&frontend_attract,math,&contact,&road,paths) &&
                   nm_intro_scene_detail(&frontend_attract,math,&scenery);
            if(!ok || !nm_frontend_game_start(&frontend_attract,math,&contact,&road,paths,pool)) {
                fprintf(stderr,"front end: %s refused\n",ok ? "game start" : "attract");break;
            }
            nm_select_enter(&select);counter=(uint32_t)start+2u;phase=18;
        } else if(active==12 || active==17) continue; /* nm_frontend_game_start took them */
        else if(active==18) {
            const nm_select_input_t input={(int32_t)need(line,"steer"),(int32_t)need(line,"psteer"),
                (uint8_t)need(line,"edge"),(uint8_t)((strtoul(strstr(line," held=")+6,NULL,16)>>4)&1u)};
            if((uint32_t)b0!=++counter || !nm_select_tick(&select,&input,counter,&sounds)) {
                fprintf(stderr,"front end: select iteration 5010b0 %lld refused\n",b0);break;
            }
            if(select.phase==19) { phase=19;break; }
        } else { fprintf(stderr,"front end: active phase %lld inside the select\n",active);break; }
    }
    fclose(in);
    if(phase!=19) { fprintf(stderr,"front end: no game start and select in %s\n",path);return 0; }
    *course=select.course;*transmission=select.transmission;*counter_base=counter+2u;
    printf("front end: credit at 5010b0 %lld, select %u iterations, course %u, transmission %u, leading lap %d, "
        "phase20 tick 5010b0 %u\n",start,(unsigned)(counter-(uint32_t)start-2u),*course,*transmission,
        pool->leading_lap,(unsigned)*counter_base);
    return 1;
}

/* --leave: 5010b0 of the game's last iteration, the F row of phases 29/30 that selects phase 2, from AFTER on (a name
 * entry's length is the player's); 0: no such row. */
static uint32_t frontend_game_over(const char *path,uint32_t after) {
    FILE *in=fopen(path,"r");
    if(!in) return 0;
    static char line[4096];
    uint32_t found=0;
    while(!found && fgets(line,sizeof(line),in)) {
        if(line[0]!='F' || line[1]!=' ') continue;
        const long long b0=need(line,"b0"),active=need(line,"active");
        if((uint32_t)b0>=after && need(line,"phase")==2 && (active==29 || active==30)) found=(uint32_t)b0;
    }
    fclose(in);
    return found;
}

/* practice_race_detail() of code/practice.c: the shared camera pass (race_camera.h) with the tick's view buttons. */
static uint32_t view_held;
static nm_race_cameras_t cameras={nm_race_keyed_tracks,
#ifdef NM_RACE_VIEW_DATA
    nm_race_pit_view_tracks,
#else
    NULL,
#endif
    nm_race_crash_keys,nm_race_crash_times,NM_RACE_CRASH_KEYS};
static int race_detail(void) {
    const char *why=nm_race_camera_pass(&race,&presentation,math,&scenery,&cameras,view_held);
    if(why) printf("native camera pass rejected: %s\n",why);
    return !why;
}

/* --clock: what the frame-counter animations of the course (include/clock_actors.h, ASSET_DIR/courseN-actors.dna)
 * draw after every tick, as render/scene/practice.c picks it (clock_scene.h, nm_clock_frame on the cells the world
 * objects see): "K tick=T frame=F c=5010b0 stage=S f0 f1 ..." with each group's frame, -1 when it draws nothing (every
 * group while the pit crew suspends the world actors); S the podium's stage (-1 outside the cinematic). Only ticks
 * whose frame shows the world. For tests/test_clock_lockstep.py against the original's draws. */
static FILE *clock_out;
static void *clock_data;
/* With actors (Expert's, include/expert_actors.h): the props and the actors step as code/practice.c steps them
 * (code/props_scene.c: the race's after the pit crew, the results', the podium's after its detail), the START edges
 * the race ticks fed, and per drawn group of an actor its placement: "P tick=T g=G m0 .. m11" (nm_xa_pose_matrix,
 * column-major, hex floats) after its K row. */
static unsigned clock_start_edges;
static int clock_setup(const char *assets,unsigned course,const char *file) {
    char path[512];
    if(!nm_course_asset(path,sizeof(path),assets,course,NM_COURSE_ACTORS,0)) return 0;
    FILE *in=fopen(path,"rb");
    if(!in) { fprintf(stderr,"--clock: no %s\n",path);return 0; }
    fseek(in,0,SEEK_END);const long size=ftell(in);fseek(in,0,SEEK_SET);
    clock_data=size>0 ? aligned_alloc(32,((size_t)size+31u)&~(size_t)31u) : NULL;
    const int read=clock_data && fread(clock_data,1,(size_t)size,in)==(size_t)size;
    fclose(in);
    if(!read || !nm_clock_validate(clock_data,(size_t)size,NULL,65535u)) { fprintf(stderr,"--clock: %s is no valid DNA1 file\n",path);return 0; }
    nm_props_world_actors(clock_data,course);
    nm_props_start_edges(&clock_start_edges);
    return (clock_out=fopen(file,"wx"))!=NULL;
}
static void clock_row(long tick,long frame,int attract_active,uint32_t after,int suspended) {
    uint32_t bitmaps[16];
    nm_clock_input_t in;
    const int have=nm_scenery_bitmaps(&presentation.cells_previous,bitmaps) &&
        nm_clock_scene_input(&in,&race,&presentation,attract_active,after,bitmaps);
    fprintf(clock_out,"K tick=%ld frame=%ld c=%" PRIu32 " stage=%ld",tick,frame,have ? in.counter : 0u,
        attract_active ? (long)presentation.stage : -1L);
    const nm_clock_header_t *h=clock_data;
    const nm_xa_draw_t *draws=nm_props_world_actor_draws(attract_active);
    int frames[NM_CLOCK_GROUPS_MAX];
    for(unsigned g=0;g<h->groups;++g) {
        const nm_clock_group_t *group=nm_clock_groups(clock_data)+g;
        frames[g]=!have || suspended ? -1 : group->cond!=NM_CLOCK_ACTOR ? nm_clock_frame(group,&in) : draws ? draws[g].frame : -1;
        fprintf(clock_out," %d",frames[g]);
    }
    fputc('\n',clock_out);
    for(unsigned g=0;g<h->groups && draws;++g) {
        if(frames[g]<0 || !draws[g].count) continue;
        float m[12];nm_xa_pose_matrix(draws+g,m);
        fprintf(clock_out,"P tick=%ld g=%u",tick,g);
        for(unsigned i=0;i<12;++i) { uint32_t bits;memcpy(&bits,m+i,4);fprintf(clock_out," %08" PRIx32,bits); }
        fputc('\n',clock_out);
    }
}

/* The camera heading 5014a8 the reflection 6f24 reads (after this tick's view, before 8814): the in-car views'
 * (race_view.h), else the follow view's (the keyed cameras keep it too). */
static int16_t reflection_heading(const nm_player_t *car) {
    int16_t angles[3],heading;
    if(car->camera_mode==2 && nm_race_view_angles(car,angles,&heading)) return heading;
    return nm_follow_heading(car);
}



/* After the race: the phase timer5010a8 the active owner counts (finish 24,
 * podium 27/28, results 26), the phase25 record notification501398 and whether
 * the podium scene's cars (not the race cars) are the live entities. */
static int32_t phase_timer;
static uint32_t record_notification;
static int podium_cars;
static int left_podium; /* a podium scene ran: its cars are the pool's first entities */

/* --parts: the player's part list after every race tick as code/render/scene/practice.c submits it
 * (nm_practice_render_player_car): per drawn part its model and view-space matrix; the body slot names the body
 * practice_player_body writes into the clean body's mesh (car-damage.dnb). The wheel offsets are the car pack's (the
 * last four instances, tools/append_player_parts.py), as practice_player_bind reads them. */
static FILE *parts_out;
/* --sound: every sound command of a race tick in code/practice.c's order, keyed by reference frame. */
static FILE *sound_out,*hints_out;
static long sound_frame,sound_tick;
static void sound(uint32_t command) {
    if(sound_out && command) fprintf(sound_out,"frame=%ld tick=%ld cmd=%06" PRIx32 "\n",sound_frame,sound_tick,command);
}
#ifdef NM_TRACE_PARTS
static float parts_wheel_offsets[4][3];
static nm_damage_bodies_t parts_bodies;
#endif
static int parts_setup(const char *assets) {
#ifndef NM_TRACE_PARTS
    (void)assets;fputs("--parts needs -DNM_TRACE_PARTS (code/meshpack.c, code/render/scene/damage_bodies.c)\n",stderr);return 0;
#else
    char file[1024];nm_pack_t pack={0};
    if(snprintf(file,sizeof(file),"%s/car-damage.dnb",assets)>=(int)sizeof(file) ||
       !nm_damage_bodies_load(&parts_bodies,file) || !parts_bodies.data) return 0;
    if(snprintf(file,sizeof(file),"%s/car.dnm",assets)>=(int)sizeof(file) || !nm_pack_load(&pack,file)) return 0;
    const unsigned count=pack.header->instances;
    int ok=count>=6;
    for(unsigned w=0;ok && w<4;++w) {
        const nm_instance_t *wheel=pack.instances+count-4+w;
        ok=wheel->source_address==nm_player_part_models[NM_PLAYER_PART_WHEEL+w];
        for(unsigned a=0;a<3;++a) parts_wheel_offsets[w][a]=wheel->matrix[9+a];
    }
    nm_pack_unload(&pack);
    if(!ok) fputs("car.dnm does not end with the player's wheel bindings\n",stderr);
    return ok;
#endif
}
static int parts_dump(long n,long frame) {
#ifdef NM_TRACE_PARTS
    nm_player_t drawn;unsigned wheels_off;
    const nm_player_t *car=nm_pit_drawn_player(&race.shared.pit,&race.cars[0].car,&drawn,&wheels_off);
    const unsigned show=nm_player_show((uint32_t)race.camera_frame.mode,race.camera_frame.eye);
    unsigned mask=nm_player_part_mask(show);
    for(unsigned wheel=0;wheel<4;++wheel) /* 73b0..7664: the pit crew's 501a98 */
        if(wheels_off&(1u<<nm_pit_wheel_bits[wheel])) mask&=~(1u<<(NM_PLAYER_PART_WHEEL+wheel));
    float matrices[NM_PLAYER_PARTS][12];
    if(!nm_player_parts_shown(math,car,presentation.view,parts_wheel_offsets,show,matrices)) return 0;
    const uint32_t damage=car->boundary.damage.damage;
    const int choice=nm_player_damage_body(damage);
    fprintf(parts_out,"P n=%ld frame=%ld show=%u dmg=%" PRIx32 " parts=",n,frame,show,damage);
    for(unsigned part=0,first=1;part<NM_PLAYER_PARTS;++part) {
        if(!(mask&(1u<<part))) continue;
        uint32_t model=nm_player_part_models[part];
        if(part==NM_PLAYER_PART_BODY && (show&NM_PLAYER_SHOW_HOOD)) model=nm_player_hood_models[nm_player_hood(damage)];
        else if(part==NM_PLAYER_PART_BODY) model=nm_damage_body(&parts_bodies,choice<0 ? NM_DAMAGE_BODY_CLEAN : (unsigned)choice)->model;
        fprintf(parts_out,"%s%" PRIx32 ":",first ? "" : ",",model);first=0; /* the driver's view: the shadow alone */
        for(unsigned i=0;i<12;++i) fprintf(parts_out,"%s%08" PRIx32,i ? "." : "",bits(matrices[part][i]));
    }
    /* The pit crew's draws of this tick (pit_crew.h; render/scene/practice.c appends them to the player's range). */
    static nm_pit_crew_draw_t crew[NM_PIT_CREW_DRAWS_MAX];
    unsigned crew_count=0;
    if(race.shared.pit_data && !nm_pit_crew_draws(math,race.shared.pit_data,&race.shared.pit,presentation.view,
        presentation.heading,crew,&crew_count)) return 0;
    for(unsigned i=0;i<crew_count;++i) {
        fprintf(parts_out,"%s%" PRIx32 ":",i ? "," : " crew=",crew[i].model);
        for(unsigned e=0;e<12;++e) fprintf(parts_out,"%s%08" PRIx32,e ? "." : "",bits(crew[i].matrix[e]));
    }
    fputc('\n',parts_out);
    return 1;
#else
    (void)n;(void)frame;return 0;
#endif
}

/* --effects: the player's car effects after every race tick (include/car_effects.h), as code/render/scene/practice.c
 * places them: per draw of the rule (9088's order) its model and view-space matrix, "t" after the model for a
 * checker effect (smoke: the translucent list), then the spin smoke's drawn particles in slot order ("s": model
 * 232206[life], view * T S Rx Ry Rz). View preset 2; the frame is the body's matrix times the wheel routine's undo
 * (the follow and keyed views draw the wheels). A line "S n= frame= owner= ring= life= angles= drawn= p=" follows
 * with the spin smoke's whole state after the tick (floats as bits, for tests/test_effects_render.py). */
static FILE *effects_out;
static nm_spin_smoke_t spin_smoke; /* task 9e2c, as code/practice.c steps it */
static int effects_dump(long n,long frame) {
    const nm_player_t *car=&race.cars[0].car;
    const unsigned show=nm_player_show((uint32_t)race.camera_frame.mode,race.camera_frame.eye);
    nm_car_effect_t fx[NM_CAR_EFFECTS_MAX];
    const unsigned count=nm_car_effects(car,race.camera_frame.view,fx);
    float body[12],view_body[12],frame_matrix[12];
    if(!nm_player_part_pose(math,car,0,body) || !nm_pose_compose(presentation.view,body,view_body) ||
       !nm_car_effect_frame(math,car,show&NM_PLAYER_SHOW_WHEELS,view_body,frame_matrix)) return 0;
    fprintf(effects_out,"F n=%ld frame=%ld fx=",n,frame);
    for(unsigned i=0;i<count;++i) {
        float m[12];
        const uint32_t model=nm_fx_model(fx[i].kind,fx[i].index);
        if(!model || !nm_car_effect_matrix(math,frame_matrix,fx+i,m)) return 0;
        fprintf(effects_out,"%s%" PRIx32 "%s:",i ? "," : "",model,fx[i].translucent ? "t" : "");
        for(unsigned k=0;k<12;++k) fprintf(effects_out,"%s%08" PRIx32,k ? "." : "",bits(m[k]));
    }
    for(unsigned i=0;i<NM_SPIN_SMOKE_PARTICLES;++i) {
        if(!(spin_smoke.drawn&(1u<<i))) continue;
        float m[12];
        if(!nm_spin_smoke_matrix(math,presentation.view,&spin_smoke,i,m)) return 0;
        fprintf(effects_out,"%s%" PRIx32 "s:",count || (spin_smoke.drawn&((1u<<i)-1u)) ? "," : "",
            nm_fx_spin_models[spin_smoke.life[i]]);
        for(unsigned k=0;k<12;++k) fprintf(effects_out,"%s%08" PRIx32,k ? "." : "",bits(m[k]));
    }
    if(!count && !spin_smoke.drawn) fputc('-',effects_out);
    fprintf(effects_out,"\nS n=%ld frame=%ld owner=%u ring=%u drawn=%x life=",n,frame,race.shared.inhibit_owner,
        spin_smoke.ring,spin_smoke.drawn);
    for(unsigned i=0;i<NM_SPIN_SMOKE_PARTICLES;++i) fprintf(effects_out,"%s%u",i ? "." : "",spin_smoke.life[i]);
    fputs(" angles=",effects_out);
    for(unsigned i=0;i<12;++i) fprintf(effects_out,"%s%x",i ? "." : "",spin_smoke.angles[i]);
    fputs(" p=",effects_out);
    for(unsigned i=0;i<NM_SPIN_SMOKE_PARTICLES*6;++i) fprintf(effects_out,"%s%08" PRIx32,i ? "." : "",bits(spin_smoke.particle[i/6][i%6]));
    fputc('\n',effects_out);
    return 1;
}

static void write_rows(FILE *out,long n,long frame,const nm_controls_t *controls) {
    const nm_player_t *p=&race.cars[0].car;
    fprintf(out,"T n=%ld frame=%ld phase=%u active=%u flags=%" PRIx32 " remaining=%" PRIu32 " clock=%" PRIu32
        " steer=%" PRId32 " accel=%u brake=%u normal=%u rolling=%u count=%u laps=%u finish=%" PRIx32
        " display=%" PRIu32 " hold=%" PRIu32 " start=%" PRIx32 " ticks=%" PRIu32 " award=%" PRIu32 " guided=%u camera=%" PRId32 " ctick=%" PRIu32 " stage=%d ctr=%u aux=%u ptimer=%" PRId32 " notify=%" PRIx32 " vticks=%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32 " view=",
        n,frame,race.phase,race.active_phase,race.shared.flags,race.award.remaining,race.shared.clock,
        controls->steer,controls->accel,controls->brake,race.rolling[0]==0,race.rolling[0]==1,race.count,
        p->checkpoints.total_laps,p->checkpoints.finish_flags,p->checkpoints.display_ticks,
        race.shared.timer_hold,race.start_flags,race.ticks,race.award.calls,race.shared.guided_mode,
        p->camera_mode,presentation.camera_cursor.tick,race.start_stage,p->camera_transition,(unsigned)(p->auxiliary_flags>>31),
        phase_timer,record_notification,race.view_ticks[0],race.view_ticks[1],race.view_ticks[2],race.view_ticks[3]);
    for(unsigned i=0;i<12;++i) fprintf(out,"%s%08" PRIx32,i ? "." : "",bits(presentation.view[i]));
    fputs(" cells=",out);
    for(unsigned i=0;i<16;++i) fprintf(out,"%s%" PRIx32,i ? "." : "",race.shared.visible_cells[i]);
    /* Native only (pit.h; the reference rows lack them): 50112c, crew rows on, crew time, 501a98, rows 74..138
     * suspended, 501128, the keyed track, and the player's +5c. */
    const nm_pit_t *pit=&race.shared.pit;
    fprintf(out," pit=%" PRIu32 ".%u.%" PRIu32 ".%x.%u.%u.%u lift=%08" PRIx32,pit->ticks,pit->crew,pit->crews[0].time,
        pit->wheels_off,pit->suspended,pit->armed,pit->camera,p->boundary.damage.overlay_model);
    /* The scene light (tunnel_light.h), as trace_race.hpp's light= (camera object +60..+68, +84). */
    fprintf(out," light=%08" PRIx32 ".%08" PRIx32 ".%08" PRIx32 ".%u",bits(race.scene_light.light[0]),
        bits(race.scene_light.light[1]),bits(race.scene_light.light[2]),(unsigned)race.scene_light.index);
    fputc('\n',out);
    if(podium_cars) {
        /* Podium scene: spawn index is the entity ID; only car state exists. */
        for(unsigned id=0;id<presentation.count;++id) {
            p=presentation.cars+id;
            fprintf(out,"C n=%ld id=%u slot=%u flags=%" PRIx32 " prof=%u prev=%u lap=%u done=%u si=%" PRId32 " spd=%08" PRIx32
                " x=%08" PRIx32 " y=%08" PRIx32 " z=%08" PRIx32 " yaw=%u head=%u path=%u lat=%d want=%08" PRIx32 " road=%" PRIu32 "\n",
                n,id,id,p->boundary.damage.flags,(uint16_t)p->phase[0],(uint16_t)p->phase[1],p->lap.lap,p->lap.completed,
                p->speed_index,bits(p->boundary.speed),bits(p->boundary.ground.position[0]),bits(p->boundary.ground.position[1]),
                bits(p->boundary.ground.position[2]),(uint16_t)p->boundary.ground.yaw,(uint16_t)p->boundary.heading,
                p->path_offset,p->lateral.current,bits(p->desired_speed),p->boundary.ground.road);
        }
        return;
    }
    for(unsigned id=0;id<race.count;++id) {
        const nm_race_car_t *c=race.cars+id;p=&c->car;
        fprintf(out,"C n=%ld id=%u slot=%u mode=%u roll=%u flags=%" PRIx32 " prof=%u prev=%u far=%u lap=%u done=%u cnt=%u rank=%u"
            " key=%u keyhi=%u si=%" PRId32 " spd=%08" PRIx32 " x=%08" PRIx32 " y=%08" PRIx32 " z=%08" PRIx32
            " yaw=%u head=%u path=%u lat=%d gear=%" PRIu32 " rpm=%" PRId32 " acc=%u brk=%" PRIx32 " str=%" PRId32
            " want=%08" PRIx32 " road=%" PRIu32 " ms=%08" PRIx32 " gs=%08" PRIx32 " surf=%" PRIx32 " mask=%x sx=%08" PRIx32 " sz=%08" PRIx32 " tx=%08" PRIx32 " tz=%08" PRIx32 " cside=%u crel=%" PRIx32
            " dmg=%" PRIx32 " body=%" PRIx32 " crashes=%u tilt=%d pitch=%d\n",
            n,id,id,c->mode,race.rolling[id],p->boundary.damage.flags,(uint16_t)p->phase[0],(uint16_t)p->phase[1],
            p->lap.furthest,p->lap.lap,p->lap.completed,p->lap.counter,c->rank,
            (uint16_t)p->rank_key,p->rank_key_high,p->speed_index,bits(p->boundary.speed),
            bits(p->boundary.ground.position[0]),bits(p->boundary.ground.position[1]),bits(p->boundary.ground.position[2]),
            (uint16_t)p->boundary.ground.yaw,(uint16_t)p->boundary.heading,p->path_offset,p->lateral.current,
            p->gear,p->rpm,p->accel,p->brake,p->steer,bits(p->desired_speed),p->boundary.ground.road,
            bits(1.0f) /* practice.c motion_scale input */,bits(c->guided_scale),
            p->boundary.ground.selected_surface,p->boundary.ground.combined_mask,
            bits(c->collision_motion.previous[0]),bits(c->collision_motion.previous[1]),
            bits(c->collision_motion.travel[0]),bits(c->collision_motion.travel[1]),c->collision_side,c->relative,
            p->boundary.damage.damage,p->boundary.damage.body_model,p->boundary.damage.crash_count,p->tilt[0],p->filtered_pitch);
    }
}

static long next_logic_row(const reference_row_t *rows,long row,long available) {
    for(++row;row<available && rows[row].scene!=UINT32_MAX && rows[row].scene==rows[row-1].scene;++row) {
        long last=row;
        while(last+1<available && rows[last+1].scene==rows[row].scene) ++last;
        if(last+1<available && rows[last+1].scene-rows[row].scene>=2) break;
    }
    return row;
}

int main(int argc,char **argv) {
    /* Optional trailing arguments, any mode:
     *   --script-header NEW_FILE  the decoded controls fed on every race tick, as
     *                             race_script_data.h for a NATIVE_RACE_SCRIPT console build;
     *   --checkpoints NEW_FILE    the CHECKPOINT rows code/practice.c prints under
     *                             NATIVE_RACE_CHECKPOINTS, through podium and results;
     *   --hud NEW_FILE            what the HUD rules (include/hud.h) display after every
     *                             race tick, keyed by reference frame, for tools/check_hud.py;
     *   --parts NEW_FILE          the player's drawn parts (models, view matrices) after every race
     *                             tick, keyed by reference frame (tests/test_damage_render.py);
     *   --effects NEW_FILE        the player's car effects (models, view matrices) after every race tick,
     *                             keyed by reference frame (tests/test_effects_render.py);
     *   --clock NEW_FILE          the frame each frame-counter animation of the course draws after every
     *                             tick (ASSET_DIR/courseN-actors.dna, include/clock_actors.h), keyed by reference
     *                             frame (tests/test_clock_lockstep.py);
     *   --sound NEW_FILE          every sound command of the race ticks (phases 20..24) in practice.c's
     *                             order, keyed by reference frame (tools/compare_race_sound.py);
     *   --wheel-phase N           both wheel phases of the player (+5a, +250) at the start, the
     *                             original's start state: the new scene keeps what entity 501220
     *                             (the attract's car 0) had when START was pressed. Visual only.
     *   --start-lap N             the leading lap 5014ba the race's first sort pass reads (1224c,
     *                             12458 its only writer): the previous scene's last value, never
     *                             reset at the race entry; -1 in the original's runs recorded from
     *                             the attract (START at frame 1290). Cold start (default): 0, the
     *                             value after boot. A start-state input like carry mode's words.
     *   --course N                course slot 501460 of the reference (0 Beginner, 1 Expert, 2 Advanced):
     *                             loads courseN.dnc, courseN.dnr, courseN-route0..7.dnp and the visibility
     *                             pack (scenery.dnv for 0, courseN.dnv otherwise; include/course_assets.h).
     *                             The race-start header must hold the course (--courses).
     *   --counter-base N|auto     the frame counter 5010b0 on the phase20 tick (default 2368, the
     *                             value of every course-0 reference: NM_SLOT_MACHINE_COUNTER_BASE). It
     *                             depends on how long the menus took; its low bits are the collision
     *                             phase 503692 (5818..583c). A start-state input like --start-lap;
     *                             auto: the scene field of reference row FIRST_ROW.
     *   --poke-speed ROW:BITS     before the tick fed by reference row ROW, set the player's speed
     *                             to the float with these hex bits; the reference trace must come
     *                             from m2segment with M2_POKE_ROW=ROW M2_POKE_SPEED=BITS (the same
     *                             write at the player's block entry d834, trace_race.hpp). A test
     *                             input for states live play reaches rarely (a negative speed:
     *                             tests/test_negative_speed_lockstep.py).
     *   --frontend TRACE          the start state from the native front end fed by the original's front-end
     *                             trace (m2segment --trace-frontend of the same run): the carried pool
     *                             words of every car, the leading lap and the frame counter base, and
     *                             the transmission the select chose. Replaces carry mode's reference
     *                             words and --start-lap with what the native attract and select compute.
     *   --enter STATE             with --frontend: the game after trace_attract_native's --leave (tests/game_chain.h):
     *                             the pool of the scenes before it, the select from the iteration after its credit.
     *                             The trace is that run's whole front-end trace; --course must be the course chosen.
     *   --leave NEW_STATE         after the run, through phase 29: what the game leaves (tests/game_chain.h), for
     *                             trace_attract_native --enter. Without --enter: the pool from power-on.
     *   --transmission manual|automatic  the transmission 501464 the race was selected with, as a
     *                             NATIVE_TRANSMISSION build supplies it: the grid's body variant
     *                             (e5ac), the player's gear rule (d89c) and the 1210c field get it,
     *                             and the player's shifter (53e120 bits 20-22, a cabinet input like
     *                             the wheel) is taken from the reference row with manual. Every row
     *                             must show that transmission. Default: production automatic,
     *                             reference/seeded the trace's own.
     *   --difficulty easy|normal|hard|hardest  the operator difficulty 53e59c (include/settings.h), as
     *                             code/practice.c takes it from the native settings at the race start:
     *                             the start time and every checkpoint award. Every row of the reference
     *                             must show it (m2segment --nvram with tools/settings_eeprom.py).
     *                             Default: production normal, reference/seeded the trace's own. */
    const char *script_header=NULL,*checkpoint_file=NULL,*hud_file=NULL,*parts_file=NULL,*effects_file=NULL,*sound_file=NULL,*hints_file=NULL;
    const char *clock_file=NULL;
    const char *frontend_file=NULL;
    long wheel_phase=-1,leading_lap=0,poke_row=-1;uint32_t poke_speed=0;int transmission_option=-1,difficulty_option=-1;
    long course=0;uint32_t counter_base=NM_SLOT_MACHINE_COUNTER_BASE;
    const char *enter_file=NULL,*leave_file=NULL;
    while(argc>=9 && argv[argc-2][0]=='-') {
        if(!strcmp(argv[argc-2],"--script-header")) script_header=argv[argc-1];
        else if(!strcmp(argv[argc-2],"--checkpoints")) checkpoint_file=argv[argc-1];
        else if(!strcmp(argv[argc-2],"--hud")) hud_file=argv[argc-1];
        else if(!strcmp(argv[argc-2],"--parts")) parts_file=argv[argc-1];
        else if(!strcmp(argv[argc-2],"--effects")) effects_file=argv[argc-1];
        else if(!strcmp(argv[argc-2],"--sound")) sound_file=argv[argc-1];
        else if(!strcmp(argv[argc-2],"--hints")) hints_file=argv[argc-1];
        else if(!strcmp(argv[argc-2],"--clock")) clock_file=argv[argc-1];
        else if(!strcmp(argv[argc-2],"--wheel-phase")) wheel_phase=strtol(argv[argc-1],NULL,0)&0xffff;
        else if(!strcmp(argv[argc-2],"--start-lap")) leading_lap=strtol(argv[argc-1],NULL,0);
        else if(!strcmp(argv[argc-2],"--frontend")) frontend_file=argv[argc-1];
        else if(!strcmp(argv[argc-2],"--enter")) enter_file=argv[argc-1];
        else if(!strcmp(argv[argc-2],"--leave")) leave_file=argv[argc-1];
        else if(!strcmp(argv[argc-2],"--counter-base"))
            counter_base=strcmp(argv[argc-1],"auto") ? (uint32_t)strtoul(argv[argc-1],NULL,10) : UINT32_MAX;
        else if(!strcmp(argv[argc-2],"--course")) {
            course=strtol(argv[argc-1],NULL,10);
            if(course<0 || course>2) { fputs("--course needs 0, 1 or 2\n",stderr);return 2; }
        }
        else if(!strcmp(argv[argc-2],"--transmission")) {
            transmission_option=!strcmp(argv[argc-1],"manual") ? 1 : !strcmp(argv[argc-1],"automatic") ? 0 : -2;
            if(transmission_option<0) { fputs("--transmission needs manual or automatic\n",stderr);return 2; }
        }
        else if(!strcmp(argv[argc-2],"--difficulty")) {
            difficulty_option=nm_settings_difficulty_parse(argv[argc-1]);
            if(difficulty_option<0) { fputs("--difficulty needs easy, normal, hard or hardest\n",stderr);return 2; }
        }
        else if(!strcmp(argv[argc-2],"--poke-speed")) {
            char *end;poke_row=strtol(argv[argc-1],&end,10);
            if(*end!=':' || poke_row<0) { fputs("--poke-speed needs ROW:BITS\n",stderr);return 2; }
            poke_speed=(uint32_t)strtoul(end+1,NULL,16);
        }
        else break;
        argc-=2;
    }
    if(argc!=7) {
        fputs("usage: trace_race_native ASSET_DIR REFERENCE_TRACE NEW_OUTPUT FIRST_ROW MAX_TICKS production|reference\n",stderr);
        return 2;
    }
    const long first_row=strtol(argv[4],NULL,10),max_ticks=strtol(argv[5],NULL,10);
    const int seeded=!strncmp(argv[6],"seeded",6);
    const int guided_now=!strcmp(argv[6],"seeded+guidednow");
    const int guided=guided_now || !strcmp(argv[6],"seeded+guided");
    assert(!seeded || guided || !strcmp(argv[6],"seeded"));
    const int reference_inputs=seeded || !strcmp(argv[6],"reference");
    /* idle: no reference trace (REFERENCE_TRACE and FIRST_ROW are ignored). Constant
     * zero decoded controls for MAX_TICKS, as a console run without input, and the
     * same end-of-run lines code/practice.c prints, for SH4-versus-host comparison. */
    /* gas: idle with the accelerator held, as tests/race_controls_wrap.c supplies. */
    const int gas=!strcmp(argv[6],"gas");
    const int idle=gas || !strcmp(argv[6],"idle");
    /* carry: production, plus the collision tracking words the original's grid
     * reset leaves from the PREVIOUS scene in the reused entity pool (saved XZ
     * 28c/290, travel 294/298, contact side 27d, relative 2a4), taken from the
     * reference row FIRST_ROW-1. Labelled secondary comparison only: a cold
     * native start has no previous scene and zeroes them. */
    const int carry=!strcmp(argv[6],"carry");
    assert(idle || carry || reference_inputs || !strcmp(argv[6],"production"));
    assert(first_row>=0 && max_ticks>0);

    /* Load the reference T rows from FIRST_ROW. */
    FILE *in=idle ? NULL : fopen(argv[2],"r");assert(idle || in);
    reference_row_t *rows=calloc((size_t)max_ticks,sizeof(*rows));assert(rows);
    long available=idle ? max_ticks : 0;
    for(long i=0;idle && i<max_ticks;++i) { rows[i].n=rows[i].frame=i+1;rows[i].accel=gas ? 255 : 0;rows[i].scene=UINT32_MAX; }
    static char line[4096];
    int start_profile[44],start_lap[44];
    uint32_t carried[44][6];int carried_valid[44]={0};
    for(unsigned i=0;i<44;++i) start_profile[i]=start_lap[i]=-1;
    while(!idle && available<max_ticks && fgets(line,sizeof(line),in)) {
        if(line[0]=='C' && line[1]==' ' && need(line,"n")==first_row-1) {
            const long long id=need(line,"id");
            if(id>=0 && id<44) { start_profile[id]=(int)need(line,"prof");start_lap[id]=(int)need(line,"lap"); }
            if(carry && id>=0 && id<44) {
                static const char *const keys[6]={"sx","sz","tx","tz","cside","crel"};
                for(unsigned k=0;k<6;++k) {
                    char pattern[16];snprintf(pattern,sizeof(pattern)," %s=",keys[k]);
                    const char *at=strstr(line,pattern);
                    if(!at) { fprintf(stderr,"carry mode: reference row lacks %s\n",keys[k]);return 2; }
                    carried[id][k]=(uint32_t)strtoul(at+strlen(pattern),NULL,k==4 ? 10 : 16);
                }
                carried_valid[id]=1;
            }
        }
        if(line[0]!='T' || line[1]!=' ') continue;
        const long n=(long)need(line,"n");
        static unsigned guided_previous;
        const unsigned guided_before=guided_previous;
        guided_previous=(unsigned)need(line,"guided");
        if(n<first_row) continue;
        reference_row_t *r=rows+available++;
        r->n=n;r->frame=(long)need(line,"frame");
        /* Controls observed at the player's own block entry when present. */
        const int entry=need(line,"entry")!=0;
        r->steer=(int32_t)need(line,entry ? "esteer" : "steer");
        r->accel=(unsigned)need(line,entry ? "eaccel" : "accel");
        r->brake=(unsigned)need(line,entry ? "ebrake" : "brake");
        r->gearsel=(unsigned)need(line,"gearsel");r->manual=(unsigned)need(line,"manual");
        r->difficulty=(unsigned)need(line,"difficulty");r->penalty=(unsigned)need(line,"penalty");
        r->suppress=(unsigned)need(line,"suppress");r->blocked=(unsigned)need(line,"blocked");
        r->variant=(unsigned)need(line,"variant");r->recover=(unsigned)need(line,"recover");
        r->gate=(unsigned)need(line,"gate");r->hold=(unsigned)need(line,"hold");
        r->guided=guided_previous;r->guided_before=guided_before;
        { long long sc; r->scene=value(line,"scene",&sc) ? (uint32_t)sc : UINT32_MAX; }
        { long long vr; r->held=!value(line,"vr",&vr) ? 0u : (uint32_t)((vr&1 ? NM_VIEW_HELD_START : 0u)|
            (vr&2 ? NM_VIEW_HELD_VR1 : 0u)|(vr&4 ? NM_VIEW_HELD_VR2 : 0u)|(vr&8 ? NM_VIEW_HELD_VR3 : 0u)|
            (vr&16 ? NM_VIEW_HELD_VR4 : 0u)); }
        { long long c; if(value(line,"course",&c) && c!=course) {
            fprintf(stderr,"reference row %ld races course %lld, not --course %ld\n",n,c,course);return 2; } }
        assert(r->accel<256 && r->brake<256 && r->steer>=-127 && r->steer<=127);
    }
    if(in) fclose(in);
    assert(available>0);
    for(long i=0;!idle && transmission_option>=0 && i<available;++i)
        if(rows[i].manual!=(unsigned)transmission_option) {
            fprintf(stderr,"reference row %ld has transmission %u, not the one given\n",rows[i].n,rows[i].manual);return 2;
        }
    for(long i=0;!idle && difficulty_option>=0 && i<available;++i)
        if(rows[i].difficulty!=(unsigned)difficulty_option) {
            fprintf(stderr,"reference row %ld has difficulty %u, not the one given\n",rows[i].n,rows[i].difficulty);return 2;
        }
    /* 501464: the grid's body variant, the gear rule, the 1210c field. */
    unsigned transmission=transmission_option>=0 ? (unsigned)transmission_option :
        reference_inputs ? rows[0].manual : 0;
    assert(transmission<2);
    if(counter_base==UINT32_MAX) {
        if(rows[0].scene==UINT32_MAX) { fputs("--counter-base auto needs the reference's scene field\n",stderr);return 2; }
        counter_base=rows[0].scene;
        printf("counter base %u (5010b0 of reference row %ld)\n",(unsigned)counter_base,rows[0].n);
    }

    char path[1024];
#define ASSET(kind,route) (assert(nm_course_asset(path,sizeof(path),argv[1],(unsigned)course,kind,route)),path)
    math=nm_drive_load(ASSET(NM_COURSE_DRIVE,0));assert(math);
    assert(nm_contact_load(&contact,ASSET(NM_COURSE_CONTACT,0)));
    assert(nm_road_load(&road,ASSET(NM_COURSE_ROAD,0)));
    for(unsigned i=0;i<8;++i) assert(nm_path_load(paths+i,ASSET(NM_COURSE_ROUTE,i)));
    assert(nm_scenery_load(&scenery,ASSET(NM_COURSE_VISIBILITY,0)));
#undef ASSET
#ifndef NM_RACE_START_COURSES
#error "race_start_data.h predates nm_race_course_starts (prepare_intro.py --race-start --courses)"
#endif
    /* The course's start camera, pit cameras and pit stop, as code/practice.c practice_reset_session. */
    const nm_race_course_start_t *tables=nm_race_course_starts+course;
    if(!tables->tracks) {
        fprintf(stderr,"race_start_data.h does not hold course %ld (prepare_intro.py --race-start --courses)\n",course);return 2;
    }
    cameras.tracks=tables->tracks;cameras.tunnel=tables->tunnel;
#ifdef NM_RACE_VIEW_DATA
    cameras.pit_views=tables->pit_views;
#endif
    if(road.slot!=(uint32_t)course || contact.slot!=(uint32_t)course || scenery.slot!=(uint32_t)course) {
        fprintf(stderr,"packs of course %ld carry another slot\n",course);return 2;
    }
    marks.count=road.profile_count-1;marks.values=calloc(road.profile_count,1);assert(marks.values);
    static nm_entity_pool_t frontend_pool;uint32_t frontend_counter=0;
    static nm_game_chain_t chain;
    nm_frontend_pool_reset(&frontend_pool);
    if(enter_file) {
        if(!frontend_file || !nm_chain_read(enter_file,&chain)) { fputs("--enter needs --frontend and a state file\n",stderr);return 2; }
        frontend_pool=chain.pool;record_notification=chain.notification;
    }
    if(frontend_file) {
        unsigned chosen_course=0,chosen=0;
        if(carry || seeded || !frontend_from_trace(frontend_file,&frontend_pool,&chosen_course,&chosen,&frontend_counter,
            enter_file!=NULL,chain.counter)) return 2;
        /* The attract runs on the loaded packs, course 0's; a front end into another course goes through --enter. */
        if(chosen_course!=(unsigned)course) {
            fprintf(stderr,"front end chose course %u, not --course %ld\n",chosen_course,course);return 2;
        }
        if(!enter_file && course!=0) { fputs("this harness runs the attract on course 0 only: use --enter\n",stderr);return 2; }
        if(transmission_option>=0 && (unsigned)transmission_option!=chosen) {
            fputs("front end chose another transmission than --transmission\n",stderr);return 2;
        }
        transmission=chosen;
        for(long i=0;!idle && i<available;++i) if(rows[i].manual!=transmission) {
            fprintf(stderr,"reference row %ld has transmission %u, not the selected one\n",rows[i].n,rows[i].manual);return 2;
        }
    }
    if(parts_file && (!parts_setup(argv[1]) || !(parts_out=fopen(parts_file,"wx")))) {
        fprintf(stderr,"cannot set up the part list output %s\n",parts_file);return 2;
    }

    race_sound=NM_RACE_SOUND_BOOT;
    if(hints_file && !(hints_out=fopen(hints_file,"wx"))) {
        fprintf(stderr,"cannot create new hints file %s\n",hints_file);return 2;
    }
    if(sound_file && !(sound_out=fopen(sound_file,"wx"))) {
        fprintf(stderr,"cannot create new sound file %s\n",sound_file);return 2;
    }
    if(clock_file && !clock_setup(argv[1],(unsigned)course,clock_file)) {
        fprintf(stderr,"cannot set up the clock output %s\n",clock_file);return 2;
    }
    if(effects_file && !(effects_out=fopen(effects_file,"wx"))) {
        fprintf(stderr,"cannot create new effects file %s\n",effects_file);return 2;
    }
    FILE *out=fopen(argv[3],"wx");
    if(!out) { fprintf(stderr,"cannot create new output %s\n",argv[3]);return 2; }
    fprintf(out,"# race-trace v1 side=native mode=%s first_reference_row=%ld\n",argv[6],first_row);

    /* practice_reset_session(): the settings' difficulty (factory NORMAL), variant = the transmission. */
    const unsigned difficulty=difficulty_option>=0 ? (unsigned)difficulty_option :
        reference_inputs ? rows[0].difficulty : NM_DIFFICULTY_NORMAL;
    if(!nm_race_scene_start_carried(&race,math,&contact,&road,paths,difficulty,transmission,&marks,road.profile_count,tables->pit,
            frontend_pool.raced ? &frontend_pool.player : NULL)) {
        fputs("native race start rejected\n",stderr);return 1;
    }
#ifdef NM_RACE_VIEW_DATA
    race.view_presets=nm_race_view_presets; /* VR1..VR4, as code/practice.c */
#endif
    assert(race.count==road.grid_count);
    nm_spin_smoke_reset(&spin_smoke);
    if(leading_lap<-128 || leading_lap>127) { fputs("--start-lap needs a signed byte\n",stderr);return 2; }
    race.order.lap=(int8_t)leading_lap;
    race.counter_base=counter_base; /* --counter-base (default the course-0 references' 2368) */
    if(frontend_file && !nm_frontend_race_entry(&race,&frontend_pool,frontend_counter)) return 1;

    if(wheel_phase>=0) race.cars[0].car.wheel_phase[0]=race.cars[0].car.wheel_phase[1]=nm_yaw_angle((uint32_t)wheel_phase);
    if(seeded) {
        /* Per-car part of nm_race_scene_start, re-entered with native routines only. */
        for(unsigned id=1;id<race.count;++id) {
            if(start_profile[id]<0) { fprintf(stderr,"seeded mode: reference row %ld lacks car %u\n",first_row-1,id);return 2; }
            const int plain=start_lap[id]==0; /* Original keeps grid-reset state, normal updater. */
            const int wanted=plain ? start_profile[id] : start_profile[id]-1;
            const nm_path_pack_t *route=paths+road.grid[id].route;
            int found=-1;
            for(unsigned hint=0;hint<1024 && found<0;++hint) {
                nm_race_car_t *c=race.cars+id;
                memset(c,0,sizeof(*c));c->remembered=255;c->car.boundary.damage.flags=0x80000000;
                c->car.boundary.ground.road=hint;
                if(!nm_race_car_grid_reset(c,math,&contact,&road,route,id,transmission)) continue;
                if((uint16_t)c->car.phase[0]==(unsigned)wanted) found=(int)hint;
            }
            if(found<0) { fprintf(stderr,"seeded mode: no road hint reproduces car %u profile %d\n",id,wanted);return 1; }
            nm_race_car_t *c=race.cars+id;
            if(!plain && !nm_car_rolling_entry_pose(road.profiles+(uint16_t)c->car.phase[0],id,
                    &c->car,&c->effects.height.reference_height)) return 1;
            c->next=(uint16_t)((id+1)%race.count);c->previous=(uint16_t)((id+race.count-1)%race.count);
            race.rolling[id]=(uint8_t)!plain;
            if(plain || found) printf("seeded start: car %u %s, prior road hint %d -> profile %u\n",id,
                plain ? "plain grid reset (no rolling entry, normal updater)" : "rolling entry",found,(uint16_t)c->car.phase[0]);
        }
    }
    if(carry) for(unsigned id=0;id<race.count;++id) {
        if(!carried_valid[id]) { fprintf(stderr,"carry mode: reference row %ld lacks car %u\n",first_row-1,id);return 2; }
        nm_race_car_t *c=race.cars+id;
        memcpy(c->collision_motion.previous,carried[id],8);memcpy(c->collision_motion.travel,carried[id]+2,8);
        c->collision_side=(uint8_t)carried[id][4];c->relative=carried[id][5];
    }
    for(unsigned i=0;i<race.count;++i) {
        presentation.detail_order[i]=(uint8_t)i;presentation.detail_depth[i]=INT16_MAX;
    }
    if(!race_detail()) { fputs("native initial detail rejected\n",stderr);return 1; }
    /* The first tick's opponents read the drawn-car count of the race entry's detail pass (frontend.h). */
    if(frontend_file && !nm_frontend_race_guided(&race,presentation.view,&frontend_pool)) { fputs("front end: race guided mode rejected\n",stderr);return 1; }
    nm_controls_t controls={0};
    phase_timer=(int32_t)race.start_timer;
    write_rows(out,0,-1,&controls); /* Row0: state before any tick. */

    FILE *checkpoints=NULL;
    if(checkpoint_file && !(checkpoints=fopen(checkpoint_file,"wx"))) {
        fprintf(stderr,"cannot create new checkpoint file %s\n",checkpoint_file);return 2;
    }
    /* Same calls as code/render/ui/hud.c: reset at scene entry, one tick after each race tick. */
    FILE *hud_out=NULL;nm_hud_t hud;nm_hud_race_t hud_race;nm_hud_cars_t hud_cars;
    if(hud_file && !(hud_out=fopen(hud_file,"wx"))) {
        fprintf(stderr,"cannot create new HUD file %s\n",hud_file);return 2;
    }
    memset(&hud_cars,0,sizeof(hud_cars));hud_cars.math=math; /* The CONDITION panel's TGP divide and circle test. */
    hud_cars.manual=(uint8_t)transmission; /* b0c0, the gear indicator */
    nm_hud_observe(&race,&hud_race,&hud_cars);nm_hud_reset(&hud,&hud_race,&hud_cars);hud.course=race.course;
    nm_controls_t *fed=script_header ? calloc((size_t)available,sizeof(*fed)) : NULL;
    uint8_t *fed_start=script_header ? calloc((size_t)available,1) : NULL;
    uint8_t *fed_selector=script_header ? calloc((size_t)available,1) : NULL;
    uint32_t *fed_views=script_header ? calloc((size_t)available,sizeof(*fed_views)) : NULL;
    assert(!script_header || (fed && fed_start && fed_selector && fed_views));
    uint8_t checkpoint_phase=0;unsigned checkpoint_rows=0;int podium_live=0;
#define CHECKPOINT(tick_,timer_) do { \
        if(nm_race_checkpoint_due((uint32_t)(tick_),race.phase,&checkpoint_phase)) { \
            const nm_player_t *cars_[44];uint32_t row_[NM_RACE_CHECKPOINT_WORDS]; \
            const unsigned count_=podium_live ? presentation.count : race.count; \
            for(unsigned i_=0;i_<count_ && i_<44;++i_) cars_[i_]=podium_live ? presentation.cars+i_ : &race.cars[i_].car; \
            nm_race_checkpoint_row(row_,(uint32_t)(tick_),&race,(int32_t)(timer_),cars_,count_); \
            if(idle) nm_race_checkpoint_print(stdout,row_); \
            if(checkpoints) nm_race_checkpoint_print(checkpoints,row_); \
            ++checkpoint_rows; \
        } } while(0)
    unsigned previous_accel=0,previous_brake=0;
    uint32_t commands=0,command_hash=UINT32_C(2166136261);
    uint32_t result_commands=0,result_hash=UINT32_C(2166136261); /* Event tail 19038 in the results phases. */
    unsigned counter_mismatch=0; /* Race ticks whose reference 5010b0 is not counter_base+tick. */
    long tick=0;int status=0;
    for(;tick<available;++tick) {
        const reference_row_t *r=rows+tick;
        controls=(nm_controls_t){.steer=r->steer,.accel=(uint8_t)r->accel,.brake=(uint8_t)r->brake};
        /* practice.c always decodes with previous pedals0: edge == current bit7. */
        controls.accel_high_edge=(uint8_t)(((previous_accel^r->accel)&r->accel)>>7);
        controls.brake_high_edge=(uint8_t)(((previous_brake^r->brake)&r->brake)>>7);
        previous_accel=r->accel;previous_brake=r->brake;
        nm_player_tick_input_t input={.active={.dynamics={.motion_scale=1}},.phase=22};
        input.active.controls=controls;input.award.difficulty=(uint8_t)difficulty;
        input.active.force_recovery=(uint8_t)r->recover; /* START edge, practice.c start_pending */
        uint32_t hold=0;uint16_t gate=0;
        input.active.dynamics.curve=(uint8_t)transmission;
        input.active.gear_selector=(uint8_t)(transmission ? r->gearsel : 0);
        input.active.variant=(uint8_t)((race.counter_base+race.ticks)&1u); /* 503690 = 5010b0 & 1, as code/practice.c */
        if(reference_inputs) {
            input.active.dynamics.curve=(uint8_t)r->manual;
            input.active.gear_selector=(uint8_t)r->gearsel;
            input.active.proximity_disabled=r->penalty!=0;
            input.active.variant=(uint8_t)r->variant;
            input.award.blocked=r->blocked;input.award.penalty=r->penalty;
            input.award.suppression=(uint8_t)r->suppress;input.award.difficulty=(uint8_t)r->difficulty;
            hold=r->hold;gate=(uint16_t)r->gate;
        }
        if(guided) race.shared.guided_mode=(uint8_t)(guided_now ? r->guided : r->guided_before);
        if(fed) { fed[tick]=controls;fed_start[tick]=input.active.force_recovery;fed_selector[tick]=transmission ? input.active.gear_selector : 0; }
        if(r->n==poke_row) memcpy(&race.cars[0].car.boundary.speed,&poke_speed,4);
        sound_frame=r->frame;sound_tick=tick+1;
        const uint32_t clock_before=race.shared.clock; /* The phase handler's clock step: the beep (364c). */
        const uint8_t crashes_before=race.cars[0].car.boundary.damage.crash_count;
        int16_t remaining_before[44];int8_t inhibit_before[44];
        for(unsigned i=1;i<race.count && i<44;++i) {
            remaining_before[i]=race.cars[i].car.boundary.damage.remaining;inhibit_before[i]=race.cars[i].car.input_inhibit;
        }
        const uint32_t countdown=race.phase>=20 && race.phase<=24 ?
            nm_race_sound_countdown(race.phase,UINT32_C(1)<<race.phase,race.start_stage,
                (int)road.profile_count-1-race.cars[0].car.phase[0],road.profile_count-1) : 0;
        const int result=nm_race_scene_step(&race,math,&nm_intro_speeds,&contact,&road,paths,&input,hold,gate,0,
            (race.counter_base+race.ticks)&3u,&marks,road.profile_count,nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count);
        if(race.active_phase==24 || race.phase==24) phase_timer=(int32_t)race.finish.timer;
        else phase_timer=(int32_t)race.start_timer; /* 5010a8: 320 from the phase19 entry, counted in phase20 off course 0 */
        if(result==2) { printf("native phase handoff: scene phase=%u before tick %ld\n",race.phase,tick+1);break; }
        if(result!=1) {
            printf("native scene step rejected tick %ld (%s, car %u)\n",tick+1,nm_race_stage_name(race.stage),
                (unsigned)race.shared.updating);status=1;break;
        }
        if(hints_out) { /* The streamed phrases' prefetch hints of this tick (code/audio/race_sound.c). */
            uint32_t keys[8];
            const unsigned n=nm_race_sound_hints(road.awards,race.award.counter,
                nm_race_announcement_rows+nm_race_announcement_offsets[race.course],
                nm_race_announcement_counts[race.course],nm_race_announcement_ranks,nm_race_intent_rows,nm_race_intent_alternate[race.course],
                race.announcement_cursor,race.cars[0].rank,race.award.remaining,race.shared.clock,keys);
            fprintf(hints_out,"frame=%ld",(long)r->frame);
            for(unsigned i=0;i<n;++i) fprintf(hints_out," %06lx",(unsigned long)keys[i]);
            fputc('\n',hints_out);
        }
        sound(countdown);
        if(race.active_phase==22 && race.phase==22 && race.shared.clock!=clock_before)
            sound(nm_race_sound_beep(&beep,race.shared.flags,race.award.remaining));
        { uint32_t cues[3];for(unsigned i=0,n=nm_race_sound_award(&race.award,race.shared.flags,cues);i<n;++i) sound(cues[i]); }
        sound(nm_race_sound_spin(race.shared.flags,race.cars[0].car.early_sound_request)); /* 158c8 */
        { uint32_t crash[8];for(unsigned i=0,n=nm_race_sound_crash(race.shared.flags,crashes_before,&race.cars[0].car,crash);i<n;++i) sound(crash[i]); }
        for(unsigned i=1;i<race.count && i<44;++i) { /* The opponents' crash and spin starts (code/audio/race_sound.c). */
            const nm_player_t *car=&race.cars[i].car;
            if(car->boundary.damage.remaining>remaining_before[i])
                sound(nm_race_sound_opponent_cue(&cooldowns.crash,nm_race_sound_opponent_crash,race.shared.flags,car,&race.cars[0].car));
            if(!inhibit_before[i] && car->input_inhibit)
                sound(nm_race_sound_opponent_cue(&cooldowns.spin,nm_race_sound_opponent_spin,race.shared.flags,car,&race.cars[0].car));
        }
        nm_race_sound_cooldowns_tick(&cooldowns);
        if(race.active_phase==23) { /* code/practice.c: the finish commands, the finish music, 44c8/44cc. */
            for(unsigned i=0;i<race.finish.audio_count;++i) sound(race.finish.audio_commands[i]);
            sound(nm_music_finish[0]);sound(NM_RACE_SOUND_RPM_OFF);sound(NM_RACE_SOUND_SLIP_OFF);
        }
        nm_race_intent_output_t events;
        if(!nm_race_scene_events(&race,nm_race_intent_rows,57,nm_race_intent_alternate[race.course],nm_race_intent_special[NM_RACE_INTENT_UNIT],
            nm_race_announcement_rows+nm_race_announcement_offsets[race.course],nm_race_announcement_counts[race.course],
            nm_race_announcement_ranks,input.award.penalty,&events)) {
            printf("native race events rejected tick %ld\n",tick+1);status=1;break;
        }
        commands+=events.count;
        for(unsigned i=0;i<events.count;++i) {
            command_hash=(command_hash^events.command[i])*UINT32_C(16777619);
            if(race.shared.flags&NM_RACE_SOUND_GATE) sound(events.command[i]); /* 19bdc */
        }
        { uint32_t out[2];for(unsigned i=0,n=nm_race_sound_ambient(ambient,race.shared.flags,race.ticks-1u,&race.cars[0].car,out);i<n;++i) sound(out[i]); } /* 19590 */
        nm_player_effect_timers(&race.cars[0].car);
        /* The player's draw 9088 (code/audio/race_sound.c nm_race_sound_hook_draw): 90f0, 9888, 9148's lane. */
        sound(nm_race_sound_front_smoke(&front_smoke,race.cars[0].car.force.trigger_count[0]));
        sound(nm_race_sound_skid(race.shared.flags,race.cars[0].car.force.trigger_count[1],race.cars[0].car.force.grip[1]));
        { const int16_t lane=nm_race_sound_wall_lane(&race.cars[0].car); if(lane) race.cars[0].car.boundary.damage.signal=lane; }
        (void)nm_player_contact_clear(&race.cars[0].car); /* 9148 (6c0c -> 9088), contact_clear.h */
        const nm_player_t *car=&race.cars[0].car;
        const nm_reflection_input_t reflection={.frame=race.ticks-1,
            .speed_index=(uint32_t)car->speed_index,.flags=car->path_flags,
            .previous_flags=car->previous_path_flags,.heading=reflection_heading(car),
            .course=race.course,.next_mode=race.phase,.active_mode=race.active_phase};
        nm_reflection_update_t update;
        if(!nm_reflection_step(&reflection,&presentation.reflection_phase,&update)) {
            printf("native reflection rejected tick %ld\n",tick+1);status=1;break;
        }
        /* code/practice.c: after the reflection a818 (HUD object, row 61), then the slot machine
         * (row 109), its command after the events. */
        if(!nm_race_scene_wrong_way(&race,&contact)) { printf("native wrong-way rule rejected tick %ld\n",tick+1);status=1;break; }
        sound(nm_race_sound_rpm(&race_sound,race.shared.flags,race.cars[0].car.filtered_rpm)); /* 19c58, 19d1c */
        sound(nm_race_sound_slip(&race_sound,race.shared.flags,race.cars[0].car.force.tire_angle[1]));
        /* The pit crew rows 68..73 (pit.h), before the slot machine (row 109, suspended with the world actors
         * while the crew works); the crew's sound cues into the command sink, in queue order. */
        uint32_t cues[NM_PIT_CREW];unsigned cue_count;
        if(!nm_race_scene_pit_crew(&race,cues,&cue_count)) { printf("native pit crew rejected tick %ld\n",tick+1);status=1;break; }
        for(unsigned i=0;i<cue_count;++i) {
            ++commands;command_hash=(command_hash^cues[i])*UINT32_C(16777619);
            if(race.shared.flags&NM_RACE_SOUND_GATE) sound(cues[i]); /* 1ad98: 19bdc */
        }
        /* The props rows 75..86 and Expert's actors (code/props_scene.c), as code/practice.c: before the slot machine. */
        clock_start_edges+=input.active.force_recovery;
        if(clock_out || sound_out) {
            const uint32_t cone=nm_props_race_step(&race,math,&contact);
            if(cone) { ++commands;command_hash=(command_hash^cone)*UINT32_C(16777619);if(race.shared.flags&NM_RACE_SOUND_GATE) sound(cone); } /* 22302c: 19bdc */
        }
        const uint32_t slot=nm_race_scene_slot(&race,input.active.force_recovery);
        if(slot) { ++commands;command_hash=(command_hash^slot)*UINT32_C(16777619);sound(slot); }
        if(r->scene!=UINT32_MAX && r->scene!=race.counter_base+race.ticks-1u) ++counter_mismatch;
        view_held=r->held;if(fed_views) fed_views[tick]=r->held;
        if(!race_detail()) { printf("native follow-view/scenery/detail rejected tick %ld\n",tick+1);status=1;break; }
        /* Task 9e2c (row 66, after the camera row 60), presentation: the spin smoke of the car
         * race.shared.inhibit_owner names, by the view 8814 has just chosen (code/practice.c). */
        if(!nm_spin_smoke_tick(math,&spin_smoke,race.active_phase,
                &race.cars[race.shared.inhibit_owner<race.count ? race.shared.inhibit_owner : 0].car,&race.cars[0].car,race.view)) {
            printf("native spin smoke rejected tick %ld\n",tick+1);status=1;break;
        }
        write_rows(out,tick+1,r->frame,&controls);
        if(clock_out) clock_row(tick+1,r->frame,0,0,race.shared.pit.suspended);
        if(parts_out && !parts_dump(tick+1,r->frame)) { printf("native player parts rejected tick %ld\n",tick+1);status=1;break; }
        if(effects_out && !effects_dump(tick+1,r->frame)) { printf("native car effects rejected tick %ld\n",tick+1);status=1;break; }
        if(hud_out) {
            nm_hud_observe(&race,&hud_race,&hud_cars);hud_race.held=r->held;nm_hud_tick(&hud,&hud_race,&hud_cars);
            nm_hud_dump(hud_out,r->frame,&hud);
        }
        /* Same rows as code/practice.c NATIVE_RACE_CHECKPOINTS. */
        CHECKPOINT(race.ticks,race.active_phase==24 || race.phase==24 ? (int32_t)race.finish.timer : 0);
    }
    /* After the handoff: the same sequence as code/practice.c, gameplay state
     * only. Podium (03b84/03f3c timer, shared cinematic scene with the podium
     * stage graph), then phase25 record publication and the results timer of
     * gameplay/results.c (042a4 / 042e8..04320) up to phase29. No rendering,
     * result actor or labels; the result timer's held-input shortcut needs
     * the cabinet buttons and is not driven here. */
    if(!idle && !status && (race.phase==25 || race.phase==27)) {
        static nm_race_record_t records[24];
        memset(records,255,sizeof(records));nm_race_records_reset(records,nm_race_record_seeds[race.course]); /* 23b544[course] */
        nm_podium_t podium={0};
        uint32_t result_timer=0,result_penalty=0,result_clock=0;unsigned selected=UINT32_MAX,result_mode=1;int recorded=0;long results_ticks=0;
        int16_t result_scroll=0;
        /* Without a podium the pool still holds the race field: 4110 parks the
         * opponents and 1210c keeps running (race_scene.h nm_race_scene_results_field). */
        const int field_live=race.phase==25;
        /* The reference rows of these ticks: not those of frames without a logic pass (the loads of the podium's 2D screen
         * and of the name entry: 5010b0 stands and no double pass catches it up, compare_race_trace.py stalled()). */
        long row=tick;
        for(;row<available && !status;++tick,row=next_logic_row(rows,row,available)) {
            const reference_row_t *r=rows+row;
            if(race.phase==27 && !podium.phase) podium.phase=27;
            const unsigned results_phase=race.phase;
            if(podium.phase) {
                int advanced=nm_podium_step(&podium);
                if(advanced && podium.spawn) {
                    /* The course's podium graph 2865a70[501460], as code/practice.c (podium_data.h nm_podium_courses). */
                    const nm_podium_course_t *graph=nm_podium_courses+race.course;
                    if(!(NM_PODIUM_COURSES>>race.course&1u) || !graph->stages) {
                        printf("podium_data.h does not hold course %u (prepare_intro.py --podium --courses)\n",race.course);
                        status=1;break;
                    }
                    advanced=nm_intro_scene_start_stages(&presentation,graph->stages,graph->count,1u<<28,graph->overrun,
                        math,&road,paths);
                    if(advanced) nm_frontend_podium_start(&presentation,&race);
                    presentation.object_clock=race.ticks+NM_PODIUM_CLOCK_OFFSET; /* code/practice.c */
                    presentation.phase_clock=race.counter_base+presentation.object_clock+1u; /* code/practice.c */
                    podium_cars=1;podium_live=1;left_podium=1;
                } else if(advanced && podium.cinematic && !podium.entered)
                    advanced=nm_intro_scene_step(&presentation,math,&contact,&road,paths);
                if(advanced && podium.cinematic && !podium.entered) {
                    presentation.start_edge=(uint8_t)r->recover;clock_start_edges+=r->recover; /* code/practice.c */
                    advanced=nm_intro_scene_detail(&presentation,math,&scenery);
                    if(advanced && clock_out) nm_props_intro_step(&presentation,math,&contact); /* code/practice.c */
                }
                if(advanced && podium.cleanup) {
                    podium_live=0; /* practice.c: attract_active=0, the race field is live again. */
                    printf("native podium cleanup at tick %ld: stage=%" PRIu32 " transitions=%" PRIu32 " scene-ticks=%" PRIu32 "\n",
                        tick+1,presentation.stage,presentation.transitions,presentation.ticks);
                }
                if(!advanced) { printf("native podium rejected timer %ld at tick %ld\n",(long)podium.timer,tick+1);status=1;break; }
                race.phase=podium.phase;race.active_phase=28;phase_timer=podium.timer;
                /* As code/practice.c draws it: nothing on the entry tick, then the cinematic (attract_active); after
                 * its cleanup the podium's 2D screen, no world. */
                if(clock_out && (podium_live || podium.entered))
                    clock_row(tick+1,r->frame,podium_live,podium.entered ? (uint32_t)(1600-podium.timer) : 0u,podium.entered);
                CHECKPOINT(tick+1,podium.timer);
                if(podium.phase==25) podium.phase=0;
            } else if(race.phase==25) {
                if(!recorded) {
                    if(!nm_race_scene_record(&race,records,0,&selected,&record_notification)) {
                        printf("native phase25 record rejected at tick %ld\n",tick+1);status=1;break;
                    }
                    recorded=1;
                    printf("native phase25 record at tick %ld: selected=%u notification=%" PRIx32 " eligible=%" PRIx32 " clock=%" PRIu32 " best-lap=%u\n",
                        tick+1,selected,record_notification,race.cars[0].car.checkpoints.finish_flags&2u,race.shared.clock,
                        race.cars[0].car.lap.best_ticks);
                }
                podium_cars=0;
                result_timer=nm_race_timeout_ticks(result_penalty);race.cars[0].car.speed_index=0;
                race.active_phase=25;race.phase=26;phase_timer=(int32_t)result_timer;
            } else if(race.phase==26) {
                race.active_phase=26;
                race.phase=nm_race_timeout_timer(&result_timer,&result_penalty);
                /* As code/gameplay/results.c: the result actor waits to 576 (b414), then START held skips to 192 (b680). */
                if(result_mode==3) {
                    int32_t countdown=(int32_t)result_timer;
                    nm_result_clock_step(&result_clock,&countdown,(r->held&NM_VIEW_HELD_START)!=0);
                    result_timer=(uint32_t)countdown;
                } else (void)nm_result_wait_step(&result_mode,(int32_t)result_timer,&result_scroll);
                phase_timer=(int32_t)result_timer;
            } else { printf("native stops at phase %u before tick %ld (name entry / attract owners)\n",race.phase,tick+1);break; }
            if(field_live && !podium.phase) {
                /* 501a80 through the results is result_penalty (4110 reads it, 430c clears it). */
                const uint32_t penalty=result_penalty;
                nm_race_intent_output_t events;
                if(!nm_race_scene_results_field(&race,math,&road,results_phase,penalty,(race.counter_base+(unsigned)tick)&3u,0,
                       reference_inputs ? (uint8_t)r->manual : (uint8_t)transmission) ||
                   !nm_race_scene_events(&race,nm_race_intent_rows,57,nm_race_intent_alternate[race.course],nm_race_intent_special[NM_RACE_INTENT_UNIT],
                       nm_race_announcement_rows+nm_race_announcement_offsets[race.course],nm_race_announcement_counts[race.course],
                       nm_race_announcement_ranks,penalty,&events)) {
                    printf("native results field rejected tick %ld\n",tick+1);status=1;break;
                }
                result_commands+=events.count;
                for(unsigned i=0;i<events.count;++i) result_hash=(result_hash^events.command[i])*UINT32_C(16777619);
                /* code/practice.c nm_practice_results_tick: the actors run on (results.ticks counts this tick). */
                clock_start_edges+=r->recover;
                if(clock_out) nm_props_results_step(&race,(uint32_t)results_ticks+1u,results_phase,math,&contact);
            }
            /* After the tick's field pass, as code/practice.c nm_practice_results_tick. */
            if(results_phase==25 || results_phase==26) CHECKPOINT(tick+1,result_timer);
            /* The results of a race that ends there draw the race's world (gameplay/results.c: results.ticks counts
             * this tick already). */
            if(clock_out && field_live && !podium.phase) clock_row(tick+1,r->frame,0,(uint32_t)++results_ticks,0);
            write_rows(out,tick+1,r->frame,&controls);
        }
    }
    if(checkpoints) {
        fprintf(checkpoints,"Native checkpoints: rows=%u ticks=%ld\n",checkpoint_rows,
            tick);
        if(fclose(checkpoints)) return 2;
    }
    if(script_header) {
        /* The race loop ran race.ticks ticks; later phases take no controls. */
        FILE *h=fopen(script_header,"wx");
        if(!h) { fprintf(stderr,"cannot create new script header %s\n",script_header);return 2; }
        fprintf(h,"/* Local scripted-race controls: do not distribute. Generated by\n"
            " * tests/trace_race_native.c --script-header, mode %s, %" PRIu32 " race ticks. */\n"
            "#ifndef DAYTONA_RACE_SCRIPT_DATA_H\n#define DAYTONA_RACE_SCRIPT_DATA_H\n#include <stdint.h>\n"
            "/* edges: bit0 accelerator, bit1 brake rising bit7 (nm_controls_t), bit2 START rising (force_recovery, 53e584 bit4). */\n"
            "typedef struct { int8_t steer; uint8_t accel, brake, edges; } nm_race_script_tick_t;\n"
            "enum { NM_RACE_SCRIPT_TICKS = %" PRIu32 " };\n",argv[6],race.ticks,race.ticks);
        /* Manual only (automatic headers stay as they were): the transmission and, in edges
         * bits 3-5, the shifter 53e120>>20&7 of every tick. */
        if(transmission)
            fputs("/* Manual transmission (501464 = 1); edges bits 3-5: the shifter, 53e120 bits 20-22. */\n"
                "#define NM_RACE_SCRIPT_MANUAL 1\n",h);
        fputs("static const nm_race_script_tick_t nm_race_script[NM_RACE_SCRIPT_TICKS] = {\n",h);
        for(uint32_t i=0;i<race.ticks;++i)
            fprintf(h,"{%d,%u,%u,%u},%s",(int)fed[i].steer,fed[i].accel,fed[i].brake,
                (unsigned)(fed[i].accel_high_edge|(fed[i].brake_high_edge<<1)|(fed_start[i]<<2)|(fed_selector[i]<<3)),
                (i&7)==7 ? "\n" : "");
        fputs("};\n",h);
        /* The view buttons (53e120 layout, include/race_view.h) from each listed tick on; only when a tick has some. */
        uint32_t view_events=0,last=0;
        for(uint32_t i=0;i<race.ticks;++i) if(fed_views[i]!=last) { ++view_events;last=fed_views[i]; }
        if(view_events) {
            fprintf(h,"typedef struct { uint32_t tick, held; } nm_race_script_view_t;\n"
                "#define NM_RACE_SCRIPT_VIEWS %" PRIu32 "u /* a macro: code/practice.c tests it with #ifdef */\n"
                "static const nm_race_script_view_t nm_race_script_views[NM_RACE_SCRIPT_VIEWS] = {\n",view_events);
            last=0;
            for(uint32_t i=0,n=0;i<race.ticks;++i) if(fed_views[i]!=last) {
                last=fed_views[i];fprintf(h,"{%" PRIu32 ",0x%" PRIx32 "},%s",i,last,(++n&7)==0 ? "\n" : "");
            }
            fputs("};\n",h);
        }
        fputs("#endif\n",h);
        if(fclose(h)) return 2;
    }
    if(leave_file) {
        /* The race's and the results' iterations, then phase 29's; a name entry runs no scene of this harness: its
         * iterations come from the trace, and with no record the count must be the trace's. */
        if(status || race.phase!=29) { printf("--leave: the run did not reach phase 29 (phase %u)\n",race.phase);return 1; }
        chain.magic=NM_CHAIN_MAGIC;chain.course=(uint32_t)course;chain.pool=frontend_pool;
        chain.notification=record_notification;
        chain.counter=race.counter_base+(uint32_t)tick-1u+1u;
        const uint32_t over=frontend_file ? frontend_game_over(frontend_file,chain.counter) : 0;
        if(over && !record_notification && over!=chain.counter) {
            printf("--leave: the game ends at 5010b0 %u here, %u in the trace\n",(unsigned)chain.counter,(unsigned)over);return 1;
        }
        if(over) chain.counter=over;
        nm_frontend_pool_from_game(&chain.pool,&race,&presentation.cells,left_podium ? &presentation : NULL);
        if(!nm_chain_write(leave_file,&chain)) { fputs("cannot write --leave (it must be new)\n",stderr);return 2; }
    }
    const int failed=ferror(out);
    if(hud_out && fclose(hud_out)) { fputs("cannot write native HUD rows\n",stderr);return 2; }
    if(parts_out && fclose(parts_out)) { fputs("cannot write native part lists\n",stderr);return 2; }
    if(sound_out && fclose(sound_out)) { fputs("cannot write native sound commands\n",stderr);return 2; }
    if(hints_out && fclose(hints_out)) { fputs("cannot write the hints\n",stderr);return 2; }
    if(effects_out && fclose(effects_out)) { fputs("cannot write native car effects\n",stderr);return 2; }
    if(clock_out && fclose(clock_out)) { fputs("cannot write the clock rows\n",stderr);return 2; }
    if(fclose(out) || failed) { fputs("cannot write native trace\n",stderr);return 2; }
    printf("native_race_trace mode=%s ticks=%ld phase=%u active=%u remaining=%" PRIu32 " clock=%" PRIu32
        " lap=%u rank=%u commands=%" PRIu32 " hash=%08" PRIx32 " status=%d coverage=native-only\n",
        argv[6],tick,race.phase,race.active_phase,race.award.remaining,race.shared.clock,
        race.cars[0].car.lap.lap,race.cars[0].rank,commands,command_hash,status);
    if(result_commands) printf("results field: commands=%" PRIu32 " hash=%08" PRIx32 "\n",result_commands,result_hash);
    if(counter_mismatch) printf("slot counter: %u race ticks where 5010b0 != %u+tick\n",counter_mismatch,(unsigned)race.counter_base);
    if(idle) {
        /* Same values and wording as the end-of-run lines of code/practice.c. */
        const nm_player_t *p=&race.cars[0].car;unsigned released=0;
        for(unsigned i=0;i<race.count;++i) released+=race.rolling[i]!=1;
        printf("Native race view ticks: %" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n",
            race.view_ticks[0],race.view_ticks[1],race.view_ticks[2],race.view_ticks[3]);
        printf("Native race silent events: commands=%" PRIu32 " hash=%08" PRIx32 " cursor=%" PRIu32 " repeats=%u\n",
            commands,command_hash,race.announcement_cursor,(unsigned)race.intents.repeats);
        printf("Native player end: profile=%u laps=%u/%u speed-index=%ld drag=%d finish=%08" PRIx32 " position-bits=%08" PRIx32 "/%08" PRIx32 "\n",
            (uint16_t)p->phase[0],p->lap.lap,p->lap.completed,(long)p->speed_index,(int)p->drag_state,
            p->checkpoints.finish_flags,bits(p->boundary.ground.position[0]),bits(p->boundary.ground.position[2]));
        printf("Native race preview: phase=%u ticks=%" PRIu32 " released=%u clock=%" PRIu32 " remaining=%" PRIu32
            " pairs=%" PRIu32 " responses=%" PRIu32 "\n",race.phase,race.ticks,released,race.shared.clock,
            race.award.remaining,race.collisions.pairs,race.collisions.responses);
    }
    return status;
}
