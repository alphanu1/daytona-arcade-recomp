/* Host robustness check: random live play must never make a race tick reject.
 * NOT a lockstep test and no gameplay math: it runs nm_race_scene_start and then
 * the production per-tick sequence of code/practice.c (scene step, events,
 * effect timers, the 9148 contact clear, reflection, wrong way, slot machine,
 * follow-view/scenery/detail) as tests/trace_race_native.c production mode does,
 * with pad inputs drawn from a seeded generator through the console's own
 * decode (code/practice.c: nm_controls_decode of the stick/trigger ranges).
 * The inputs are deliberately abusive: long grass drives, full-lock spins,
 * wall grinding, braking to a stop and turning round (wrong way), random START
 * presses (slot machine, 10a14 recovery), pedal chatter; races end at the
 * time out or, one in four (a longer timer), at the finish. One race in three
 * aims the road follower at the pit lane's trigger row (152, x -62.7..-56.6,
 * z -72..-52; spec-pitstop.md section 1) at each lap's end: the pit stop
 * (pit.h) with abuse around it, the pit taken again on later laps. The arcade never
 * rejects a tick, so every reject is a port bug; the program names the step
 * and the player's state and fails.
 *
 * usage: fuzz_race_native ASSET_DIR FIRST_SEED RACES [MAX_TICKS]
 *   Runs RACES races with seeds FIRST_SEED.. to the scene handoff (finish or
 *   time out) or MAX_TICKS (default 20000). Exit 0 when no tick rejected.
 * Build with the project float flags (-fno-fast-math -ffp-contract=off). */
#undef NDEBUG
#include "race_scene.h"
#include "race_sound.h"
#include "race_intent_data.h"
#include "race_start_data.h"
#include "intro_scene.h"
#include "visibility.h"
#include "pose.h"
#include "vehicle.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static nm_race_scene_t race;
static nm_intro_scene_t presentation;
static nm_drive_params_t *math;
static nm_contact_pack_t contact;
static nm_road_pack_t road;
static nm_path_pack_t paths[8];
static nm_scenery_pack_t scenery;
static nm_road_marks_t marks;

/* practice_race_detail() of code/practice.c; names the failing call. */
static const char *race_detail(void) {
    const nm_player_t *players[44];
    for(unsigned i=0;i<race.count;++i) players[i]=&race.cars[i].car;
    float origin[2];uint16_t cell;nm_scenery_cells_t next;
    if(race.camera_restart) { presentation.camera_cursor=(nm_intro_cursor_t){0};race.camera_restart=0; }
    if(players[0]->camera_mode==1 && !players[0]->camera_transition) {
        if(!nm_race_crash_camera(players[0],math,&scenery,nm_race_crash_keys,nm_race_crash_times,
            NM_RACE_CRASH_KEYS,&presentation.camera_cursor,&presentation.camera,presentation.view,
            &presentation.heading,&presentation.sky_scroll,&next)) return "crash camera";
    } else if(players[0]->camera_mode==1) {
        if(!nm_race_start_camera(race.cars,race.count,math,&scenery,nm_race_keyed_tracks[race.shared.pit.camera].keys,nm_race_keyed_tracks[race.shared.pit.camera].times,
            nm_race_keyed_tracks[race.shared.pit.camera].count,presentation.anchors,&presentation.camera_cursor,&presentation.camera,
            presentation.view,&presentation.heading,&presentation.sky_scroll,&next)) return "start camera";
    } else {
        if(!nm_follow_view(math,players[0],presentation.view,&presentation.heading)) return "follow view";
        if(!nm_follow_scenery_origin(math,players[0],presentation.heading,origin,&cell)) return "scenery origin";
        if(!nm_scenery_select(&scenery,math,cell,(uint16_t)players[0]->phase[0],
            origin[0],origin[1],presentation.heading,&next)) return "scenery select";
        if(!nm_follow_sky_scroll(math,players[0],&presentation.sky_scroll)) return "sky scroll";
    }
    if(!nm_race_detail_step(presentation.view,&presentation.cells,players,race.count,
        presentation.detail_depth,presentation.detail_order,presentation.detail_level)) return "detail step";
    if(!nm_car_detail_guided_mode(presentation.detail_level,race.count,race.shared.flags,
            race.shared.timer_hold,race.course,&race.shared.guided_mode)) return "guided mode";
    presentation.cells_previous=presentation.cells;
    presentation.cells=next;
    if(!race.shared.pit.suspended && !nm_world_objects_tick(presentation.object_near_ticks,&presentation.cells_previous)) return "world objects";
    if(!nm_scenery_bitmaps(&presentation.cells,race.shared.visible_cells)) return "scenery bitmaps";
    return NULL;
}

static uint64_t rng;
static uint32_t next_random(void) {
    rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (uint32_t)(rng>>16);
}
static unsigned below(unsigned n) { return next_random()%n; }

/* One behaviour held for a while: raw stick (-128..127) and triggers (0..255). */
typedef struct { int stick; unsigned gas, brake, ticks, wobble, start_rate; } behaviour_t;
static unsigned abuse; /* One behaviour in `abuse` is abusive, the rest follow the road (2..4 by seed). */
static behaviour_t pick(void) {
    behaviour_t b={0,255,0,30+below(600),0,below(4)==0 ? 20+below(200) : 0};
    if(below(abuse)) { b.wobble=3;b.ticks=100+below(900);return b; } /* Follow the road. */
    switch(below(9)) {
    case 0: break;                                                       /* Flat out. */
    case 1: b.stick=below(2) ? 127 : -128;break;                          /* Full lock: spins, grass. */
    case 2: b.stick=(int)below(256)-128;b.gas=below(256);break;           /* Anything. */
    case 3: b.stick=below(2) ? 60+below(68) : -60-(int)below(69);b.wobble=1;break; /* Weave into walls. */
    case 4: b.gas=0;b.brake=255;b.ticks=30+below(200);break;              /* Stop. */
    case 5: b.stick=below(2) ? 127 : -128;b.gas=0;b.brake=255;break;      /* Locked brake turn: turn round. */
    case 6: b.stick=below(2) ? 20+below(40) : -20-(int)below(40);break;   /* Drift off the road. */
    case 7: b.stick=(int)below(256)-128;b.gas=below(256);b.brake=below(256);b.wobble=2;break; /* Chatter. */
    default: b.gas=below(2) ? 255 : 0;b.start_rate=5+below(30);break;     /* START mashing. */
    }
    return b;
}

static unsigned finished,pits_taken;
static int run(uint64_t seed,long max_ticks,unsigned long *total_ticks) {
    rng=seed*UINT64_C(0x9e3779b97f4a7c15)+1;abuse=2+(unsigned)(seed%3);
    for(unsigned i=0;i<4;++i) next_random();
    memset(&race,0,sizeof(race));memset(&presentation,0,sizeof(presentation));
    if(!nm_race_scene_start(&race,math,&contact,&road,paths,1,0,&marks,road.profile_count,&nm_race_pit)) {
        printf("seed %" PRIu64 ": race start rejected\n",seed);return 0;
    }
    /* One race in four starts with plenty of time (the timer word only) so the road follower completes all
     * eight laps: the finish phases 23/24 under abuse too. */
    if(seed%4==3) race.award.remaining+=30000;
    for(unsigned i=0;i<race.count;++i) { presentation.detail_order[i]=(uint8_t)i;presentation.detail_depth[i]=INT16_MAX; }
    const char *why=race_detail();
    if(why) { printf("seed %" PRIu64 ": initial %s rejected\n",seed,why);return 0; }
    nm_controls_t last={0};behaviour_t b=pick();unsigned held=0,starts=0,pits=0;
    const int pit_seeker=seed%3==1;uint8_t previous_mode=race.rolling[0];
    long tick=0;
    for(;tick<max_ticks;++tick) {
        if(++held>b.ticks) { b=pick();held=0; }
        int stick=b.stick;unsigned gas=b.gas,brake=b.brake;
        if(b.wobble==1 && (held/40)&1) stick=-stick-1;
        if(b.wobble==3) {
            /* Crude road follower: heading error against the road plus the lane offset. */
            const nm_player_t *p=&race.cars[0].car;
            const int error=(int16_t)((uint16_t)p->boundary.ground.yaw-(uint16_t)p->boundary.ground.road_heading);
            const int lateral=p->lateral.current-900;
            stick=error/48+lateral/12;
            const unsigned profile=(uint16_t)p->phase[0];
            if(pit_seeker && profile>=178 && profile<=194) {
                /* Heading toward the trigger row (-60, -62): -heading = atan2(dx, dz), the e194 convention. */
                const double dx=-60.0-p->boundary.ground.position[0],dz=-62.0-p->boundary.ground.position[2];
                const int wanted=(int)(-atan2(dx,dz)*32768.0/3.14159265358979323846);
                stick=(int16_t)((uint16_t)p->boundary.ground.yaw-(uint16_t)wanted)/48;
            }
        }
        if(b.wobble==2) { stick=(int)below(256)-128;gas=below(2)*255;brake=below(3)==0 ? 255 : 0; }
        if(stick<-128) stick=-128;
        if(stick>127) stick=127;
        /* code/practice.c: the pad's full ranges into the arcade ADC decode. */
        const int axis=stick<-127 ? -127 : stick;
        const nm_controls_t controls=nm_controls_decode((uint8_t)(128+axis*80/127),(uint8_t)(56+gas*128/255),
            (uint8_t)(56+brake*96/255),last.accel,last.brake);
        last=controls;
        nm_player_tick_input_t input={.active={.dynamics={.motion_scale=1}},.phase=22};
        input.active.controls=controls;input.award.difficulty=1;
        input.active.variant=(uint8_t)(race.ticks&1u); /* 503690 = 5010b0 & 1, as code/practice.c */
        input.active.force_recovery=(uint8_t)(b.start_rate && below(b.start_rate)==0);
        starts+=input.active.force_recovery;
        const int result=nm_race_scene_step(&race,math,&nm_intro_speeds,&contact,&road,paths,&input,0,0,0,
            race.ticks&3,&marks,road.profile_count,nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count);
        if(result==2) break;
        why=NULL;
        if(result!=1) why="race scene step";
        nm_race_intent_output_t events;
        if(!why && !nm_race_scene_events(&race,nm_race_intent_rows,57,nm_race_intent_alternate[0],nm_race_intent_special[NM_RACE_INTENT_UNIT],
            nm_race_announcement_rows+nm_race_announcement_offsets[0],nm_race_announcement_counts[0],
            nm_race_announcement_ranks,input.award.penalty,&events)) why="race events";
        if(!why) {
            nm_player_effect_timers(&race.cars[0].car);
            { /* 9148's intent lane write (race_sound.h; code/audio/race_sound.c nm_race_sound_hook_draw). */
                const int16_t lane=nm_race_sound_wall_lane(&race.cars[0].car);
                if(lane) race.cars[0].car.boundary.damage.signal=lane;
            }
            (void)nm_player_contact_clear(&race.cars[0].car);
            const nm_player_t *car=&race.cars[0].car;
            const nm_reflection_input_t reflection={.frame=race.ticks-1,
                .speed_index=(uint32_t)car->speed_index,.flags=car->path_flags,
                .previous_flags=car->previous_path_flags,.heading=nm_follow_heading(car),
                .course=0,.next_mode=race.phase,.active_mode=race.active_phase};
            nm_reflection_update_t update;
            if(!nm_reflection_step(&reflection,&presentation.reflection_phase,&update)) why="reflection";
        }
        if(!why && !nm_race_scene_wrong_way(&race,&contact)) why="wrong way";
        if(!why) {
            uint32_t cues[NM_PIT_CREW];unsigned cue_count;
            if(!nm_race_scene_pit_crew(&race,cues,&cue_count)) why="pit crew";
        }
        if(!why) (void)nm_race_scene_slot(&race,input.active.force_recovery);
        if(!why) why=race_detail();
        if(!why && race.rolling[0]==4 && previous_mode!=4) ++pits;
        previous_mode=race.rolling[0];
        if(why) {
            const nm_player_t *p=&race.cars[0].car;
            uint32_t speed;memcpy(&speed,&p->boundary.speed,4);
            printf("seed %" PRIu64 ": %s rejected tick %" PRIu32 " (stage %s, car %u): phase=%u/%u lap=%u/%u road=%" PRIu32 "/%u"
                " profile=%u/%u speed=%08" PRIx32 " speed-index=%" PRId32 " damage=%08" PRIx32 " flags=%08" PRIx32
                " camera=%" PRId32 " remaining=%" PRIu32 " starts=%u\n",seed,why,race.ticks,nm_race_stage_name(race.stage),
                race.shared.updating,race.active_phase,race.phase,p->lap.completed,p->checkpoints.total_laps,
                p->boundary.ground.road,contact.quad_count,(uint16_t)p->phase[0],road.profile_count,speed,p->speed_index,
                p->boundary.damage.damage,p->boundary.damage.flags,p->camera_mode,race.award.remaining,starts);
            *total_ticks+=(unsigned long)tick;
            return 0;
        }
    }
    *total_ticks+=(unsigned long)tick;
    const nm_player_t *p=&race.cars[0].car;
    printf("seed %" PRIu64 ": ticks=%ld phase=%u laps=%u starts=%u pits=%u damage=%08" PRIx32 " remaining=%" PRIu32 "\n",
        seed,tick,race.phase,p->lap.completed,starts,pits,p->boundary.damage.damage,race.award.remaining);
    pits_taken+=pits;
    finished+=p->lap.completed>=p->checkpoints.total_laps;
    return 1;
}

int main(int argc,char **argv) {
    if(argc!=4 && argc!=5) { fputs("usage: fuzz_race_native ASSET_DIR FIRST_SEED RACES [MAX_TICKS]\n",stderr);return 2; }
    const uint64_t first=strtoull(argv[2],NULL,10);const long races=strtol(argv[3],NULL,10);
    const long max_ticks=argc==5 ? strtol(argv[4],NULL,10) : 20000;
    char path[1024];
#define ASSET(name) (assert(snprintf(path,sizeof(path),"%s/%s",argv[1],name)<(int)sizeof(path)),path)
    math=nm_drive_load(ASSET("drive.dnd"));assert(math);
    assert(nm_contact_load(&contact,ASSET("course0.dnc")));
    assert(nm_road_load(&road,ASSET("course0.dnr")));
    for(unsigned i=0;i<8;++i) {
        char name[32];snprintf(name,sizeof(name),"course0-route%u.dnp",i);
        assert(nm_path_load(paths+i,ASSET(name)));
    }
    assert(nm_scenery_load(&scenery,ASSET("scenery.dnv")));
#undef ASSET
    marks.count=road.profile_count-1;marks.values=calloc(road.profile_count,1);assert(marks.values);
    unsigned failed=0;unsigned long ticks=0;
    for(long i=0;i<races;++i) failed+=!run(first+(uint64_t)i,max_ticks,&ticks);
    printf("fuzz_race_native: races=%ld ticks=%lu finished=%u pit stops=%u rejected=%u\n",races,ticks,finished,pits_taken,failed);
    return failed ? 1 : 0;
}
