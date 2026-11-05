/* Host-only scripted driver. NOT a test and no gameplay math: it uses the native
 * race (the production per-tick sequence of tests/trace_race_native.c / code/practice.c)
 * as a fast simulator to search raw cabinet inputs that complete a race, and writes
 * them as a scripts/inputs file for the reference board (`m2segment --inputs`). The
 * search is a short-horizon rollout over a fixed set of wheel/pedal values; it knows
 * nothing about the original beyond the input decode below.
 *
 * usage: drive_race_native [--manual] [--course N] [--counter-base B] [--replay SCRIPT [--target X,Z,PROFILE,NEAR,SPEED]] ASSET_DIR NEW_SCRIPT FIRST_FRAME MAX_TICKS
 *                          SPEED_CAP [CARRY_TRACE [shadow]]
 *   --manual     select the manual transmission (steer right through the
 *                transmission select, frames 2010-2390) and search the shifter
 *                too: at each decision also one gear up or down (include/shifter.h,
 *                the pad's sequential mapping), not during a shift. Each shift is
 *                one frame of gearN=1; the shifter rests in neutral (53e120 bits
 *                20-22 = 7), which keeps the gear (selector table 234abc). Course 0 only.
 *   --course N   course slot 501460 (0 Beginner, 1 Expert, 2 Advanced): its packs
 *                (include/course_assets.h), start tables (race_start_data.h --courses)
 *                and the menu of recipes/inputs/course_expert.txt / course_advanced.txt
 *                (course 0: race_to_end.txt).
 *   --counter-base B  the frame counter 5010b0 on the phase20 tick (default 2368, course 0's
 *                menu): its low bits are the collision phase and the variant bit. The
 *                reference trace's scene= field of its first phase-20 row.
 *   CARRY_TRACE  optional reference trace (m2segment --trace-race) of the same
 *                menu sequence: its grid row (frame FIRST_FRAME-1) supplies the
 *                collision tracking words the original carries over from the
 *                previous scene (see tests/trace_race_native.c carry mode), so
 *                the search simulates the same race the board will run.
 *   shadow       with CARRY_TRACE: search on the COLD native race (zeroed words)
 *                and replay the same inputs on a second, carried copy; reports
 *                the first tick where the two fields differ. "none" means the
 *                script does not depend on the carried words.
 *   FIRST_FRAME  reference frame of native tick1 (2584 for the
 *                scripts/inputs/race_to_end.txt menu sequence, copied below).
 *   SPEED_CAP    0 = drive as fast as the search finds; otherwise lift above
 *                this boundary speed (a deliberately slow, lower-placed race).
 *   --replay SCRIPT   the race's raw inputs come from an input script of this menu (later
 *                lines win, as tools/common/input_script.h); no search.
 *   --target X,Z,PROFILE,NEAR,SPEED  with --replay: once the car is within NEAR road profiles before
 *                PROFILE the replay stops and a controller brings it to (X, Z) at speed index
 *                5012f0 +18 <= SPEED: along the road's nearer edge, the last 30 units straight at the
 *                point, then wheel centred and no pedals once it has been within 1 unit. For a prop's
 *                knock at a chosen speed.
 * Raw values follow the original decode 4e34..4fd8: the cabinet bytes are a
 * 3:1 blend of the previous and current frame's ADC sample, then dead zone /
 * clamp / scale (include/vehicle.h nm_controls_decode).
 * Build with the project float flags (-fno-fast-math -ffp-contract=off), with the include
 * directories of tests/trace_race_native.c. */
#undef NDEBUG
#include "race_scene.h"
#include "race_intent_data.h"
#include "race_start_data.h"
#include "intro_scene.h"
#include "visibility.h"
#include "pose.h"
#include "shifter.h"
#include "race_camera.h"
#include "race_sound.h"
#include "course_assets.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    nm_race_scene_t race;
    nm_intro_scene_t presentation;
    uint8_t marks[4100];
    uint32_t mark_timer;
    uint8_t raw[3];           /* Previous frame's ADC samples: steer, accel, brake. */
    uint8_t accel,brake;      /* Previous decoded pedals, for the edge bits. */
    int8_t shift_to;          /* Manual: the gear the last tick's shifter selected, -1 neutral. */
} state_t;
static int manual,pursuit;
/* --launch A[:T]: the accelerator byte A through the standing start's countdown (phases 20/21) from tick T: the
 * start signal revs the engine with it (c130), and full accelerator over-revs (0xa8 launches best off course 0);
 * full accelerator otherwise and whenever the search does not drive (finish coast). */
static int launch_raw=-1;static long launch_from;
static uint8_t launch_accel(const state_t *s,long tick) {
    return launch_raw>=0 && tick>=launch_from && s->race.active_phase<22 ? (uint8_t)launch_raw : 0xe0;
}
static unsigned course;

static nm_drive_params_t *math;
static nm_contact_pack_t contact;
static nm_road_pack_t road;
static nm_path_pack_t paths[8];
static nm_scenery_pack_t scenery;
static state_t live,trial,shadow;
static nm_race_cameras_t cameras={NULL,
#ifdef NM_RACE_VIEW_DATA
    nm_race_pit_view_tracks,
#else
    NULL,
#endif
    nm_race_crash_keys,nm_race_crash_times,NM_RACE_CRASH_KEYS};

/* practice_race_detail() of code/practice.c: the shared camera pass (race_camera.h), no view buttons. */
static int race_detail(state_t *s) { return !nm_race_camera_pass(&s->race,&s->presentation,math,&scenery,&cameras,0); }

/* The camera heading 5014a8 the reflection 6f24 reads, as tests/trace_race_native.c. */
static int16_t reflection_heading(const nm_player_t *car) {
    int16_t angles[3],heading;
    if(car->camera_mode==2 && nm_race_view_angles(car,angles,&heading)) return heading;
    return nm_follow_heading(car);
}

/* One tick with this frame's ADC samples and shift request (manual: +1 up, -1 down), the gameplay
 * part of tests/trace_race_native.c's production tick. Returns the scene step result. */
static int advance(state_t *s,const uint8_t raw[3],int shift) {
    uint8_t blended[3];
    for(unsigned i=0;i<3;++i) blended[i]=(uint8_t)((3u*s->raw[i]+raw[i])>>2);
    memcpy(s->raw,raw,3);
    const nm_controls_t controls=nm_controls_decode(blended[0],blended[1],blended[2],s->accel,s->brake);
    s->accel=controls.accel;s->brake=controls.brake;
    nm_race_scene_t *race=&s->race;
    nm_player_tick_input_t input={.active={.dynamics={.motion_scale=1}},.phase=22};
    input.active.controls=controls;input.award.difficulty=1;
    input.active.variant=(uint8_t)(race->ticks&1u); /* 503690, as code/practice.c */
    s->shift_to=-1;
    if(manual) {
        const uint32_t gear=race->cars[0].car.gear;
        input.active.dynamics.curve=1;input.active.gear_selector=nm_shifter_sequential(gear,shift);
        if(input.active.gear_selector!=NM_SHIFTER_NEUTRAL) s->shift_to=(int8_t)(gear+(shift>0 ? 1 : -1));
    }
    nm_road_marks_t marks={s->marks,road.profile_count-1,s->mark_timer};
    const int result=nm_race_scene_step(race,math,&nm_intro_speeds,&contact,&road,paths,&input,0,0,0,
        (race->counter_base+race->ticks)&3u,&marks,road.profile_count,nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count);
    s->mark_timer=marks.timer;
    if(result!=1) return result;
    nm_race_intent_output_t events;
    if(!nm_race_scene_events(race,nm_race_intent_rows,57,nm_race_intent_alternate[race->course],nm_race_intent_special[NM_RACE_INTENT_UNIT],
        nm_race_announcement_rows+nm_race_announcement_offsets[race->course],nm_race_announcement_counts[race->course],
        nm_race_announcement_ranks,0,&events)) return 0;
    nm_player_effect_timers(&race->cars[0].car);
    { const int16_t lane=nm_race_sound_wall_lane(&race->cars[0].car); if(lane) race->cars[0].car.boundary.damage.signal=lane; }
    (void)nm_player_contact_clear(&race->cars[0].car); /* 9148 (6c0c -> 9088), contact_clear.h */
    const nm_player_t *car=&race->cars[0].car;
    const nm_reflection_input_t reflection={.frame=race->ticks-1,.speed_index=(uint32_t)car->speed_index,
        .flags=car->path_flags,.previous_flags=car->previous_path_flags,.heading=reflection_heading(car),
        .course=race->course,.next_mode=race->phase,.active_mode=race->active_phase};
    nm_reflection_update_t update;
    if(!nm_reflection_step(&reflection,&s->presentation.reflection_phase,&update)) return 0;
    if(!nm_race_scene_wrong_way(race,&contact)) return 0; /* a818, HUD object row 61. */
    uint32_t cues[NM_PIT_CREW];unsigned cue_count;
    if(!nm_race_scene_pit_crew(race,cues,&cue_count)) return 0; /* Pit crew rows 68..73; no command sink. */
    (void)nm_race_scene_slot(race,0); /* 2204fc: the search never presses START; no command sink. */
    return race_detail(s) ? 1 : 0;
}

/* Road profiles made good from BEFORE to S, across the line too: off course 0 the grid stands behind
 * the line (profiles near the last), so the first crossing wraps without completing a lap. */
static double progress(const state_t *s,unsigned before) {
    const int span=(int)road.profile_count-1;
    int d=(int)(uint16_t)s->race.cars[0].car.phase[0]-(int)before;
    if(d<-span/2) d+=span; else if(d>span/2) d-=span;
    return d;
}

/* Rollout value: distance made good, with penalties for a crash, for leaving
 * the lane band and for ending the horizon pointing across the road. */
static double rollout(const uint8_t raw[3],int shift,unsigned horizon,float speed_cap) {
    memcpy(&trial,&live,sizeof(trial));
    const unsigned before=(uint16_t)trial.race.cars[0].car.phase[0];
    double value=0;
    for(unsigned t=0;t<horizon;++t) {
        uint8_t now[3]={raw[0],raw[1],raw[2]};
        const nm_player_t *p=&trial.race.cars[0].car;
        if(speed_cap>0 && p->boundary.speed>speed_cap) now[1]=0x20;
        const int result=advance(&trial,now,t ? 0 : shift);
        if(result!=1) break;
        p=&trial.race.cars[0].car;
        value+=p->boundary.speed;
        if(p->boundary.damage.remaining || p->boundary.damage.spin_ticks) value-=3.0;
        if(p->boundary.ground.combined_mask!=2) value-=0.5;
        const int lateral=p->lateral.current;
        if(lateral<250) value-=(250-lateral)*0.004;
        if(lateral>1550) value-=(lateral-1550)*0.004;
    }
    const nm_player_t *p=&trial.race.cars[0].car;
    int error=(int16_t)((uint16_t)p->boundary.ground.yaw-(uint16_t)p->boundary.ground.road_heading);
    if(error<0) error=-error;
    value-=error*0.004;
    value+=progress(&trial,before)*4.0;
    return value;
}

/* --pursuit: a pure-pursuit policy (the car's yaw, 5012f0 +58, is atan2(-dx, dz) in 1/65536 turns; a wheel
 * byte below 0x80 turns the yaw up) and a search over its parameters: every candidate drives the whole horizon,
 * so a rollout sees the braking a corner needs. The target is the point LANE of the way across the authored
 * lanes of the first profile at least LOOKAHEAD ahead. */
typedef struct { float lookahead, lane; int lift, brake; } policy_t;
static float profile_spacing;
static void lane_point(unsigned profile,float lane,float *x,float *z) {
    const nm_road_profile_t *r=road.profiles+profile;
    const int a=r->first_lane,b=r->last_lane;
    if(a<0 || b<a || b>7) { *x=r->lanes[0].xyz[0];*z=r->lanes[0].xyz[2];return; }
    const float t=(float)a+(float)(b-a)*lane;
    int i=(int)t;if(i>=b) i=b>a ? b-1 : a;
    const float w=b>a ? t-(float)i : 0;const int j=b>a ? i+1 : i;
    *x=r->lanes[i].xyz[0]*(1-w)+r->lanes[j].xyz[0]*w;*z=r->lanes[i].xyz[2]*(1-w)+r->lanes[j].xyz[2]*w;
}
static void policy_input(const state_t *s,const policy_t *q,uint8_t raw[3]) {
    const nm_player_t *p=&s->race.cars[0].car;
    const float x=p->boundary.ground.position[0],z=p->boundary.ground.position[2];
    const unsigned span=road.profile_count-1,k=(uint16_t)p->phase[0];
    float tx=x,tz=z;
    for(unsigned step=1;step<96;++step) {
        lane_point((k+step)%span,q->lane,&tx,&tz);
        if(hypotf(tx-x,tz-z)>=q->lookahead) break;
    }
    const int desired=(int)lround(atan2(-(double)(tx-x),(double)(tz-z))*32768.0/M_PI);
    const int err=(int16_t)(uint16_t)(desired-(int)(uint16_t)p->boundary.ground.yaw);
    int w=-err*80/1400;if(w>80) w=80;if(w<-80) w=-80;
    const int a=err<0 ? -err : err;
    raw[0]=(uint8_t)(0x80+w);raw[1]=a>q->lift ? 0x20 : 0xe0;raw[2]=a>q->brake && p->boundary.speed>0.6f ? 0xa0 : 0x20;
}
static double policy_rollout(const policy_t *q,unsigned horizon,float speed_cap) {
    memcpy(&trial,&live,sizeof(trial));
    const unsigned before=(uint16_t)trial.race.cars[0].car.phase[0];
    double value=0;
    for(unsigned t=0;t<horizon;++t) {
        uint8_t now[3];policy_input(&trial,q,now);
        const nm_player_t *p=&trial.race.cars[0].car;
        if(speed_cap>0 && p->boundary.speed>speed_cap) now[1]=0x20;
        if(advance(&trial,now,0)!=1) break;
        p=&trial.race.cars[0].car;
        if(p->boundary.damage.remaining || p->boundary.damage.spin_ticks) value-=2.0;
        if(p->boundary.ground.combined_mask!=2) value-=0.3;
        const int lateral=p->lateral.current;
        if(lateral<120 || lateral>1680) value-=0.3;
    }
    return value+progress(&trial,before)*profile_spacing*2.0+trial.race.cars[0].car.boundary.speed*20.0;
}

/* --replay, --target. */
static uint8_t (*replay)[3];
static long replay_first,replay_ticks;
static float target[2];
static int target_profile,target_near,target_speed=-1,target_reached,target_engaged;
static void read_replay(const char *path,long first_frame,long ticks) {
    FILE *in=fopen(path,"r");assert(in);
    replay=calloc((size_t)ticks,3);assert(replay);
    replay_first=first_frame;replay_ticks=ticks;
    for(long i=0;i<ticks;++i) { replay[i][0]=0x80;replay[i][1]=0x20;replay[i][2]=0x20; }
    static char line[256];
    while(fgets(line,sizeof(line),in)) {
        long from,to;char name[32];unsigned value;
        if(line[0]=='#' || sscanf(line,"%ld-%ld %31[^=]=%i",&from,&to,name,&value)!=4) {
            if(line[0]=='#' || sscanf(line,"%ld %31[^=]=%i",&from,name,&value)!=3) continue;
            to=from;
        }
        const int channel=!strcmp(name,"steer") ? 0 : !strcmp(name,"accel") ? 1 : !strcmp(name,"brake") ? 2 : -1;
        if(channel<0) continue;
        for(long f=from;f<=to;++f) if(f>=first_frame && f<first_frame+ticks) replay[f-first_frame][channel]=(uint8_t)value;
    }
    fclose(in);
}
static void target_input(const state_t *s,uint8_t raw[3]) {
    const nm_player_t *p=&s->race.cars[0].car;
    const float x=p->boundary.ground.position[0],z=p->boundary.ground.position[2];
    const float distance=hypotf(target[0]-x,target[1]-z);
    if(distance<1.0f) target_reached=1;
    if(target_reached) { raw[0]=0x80;raw[1]=0x20;raw[2]=0x20;return; }
    float tx=target[0],tz=target[1];
    if(distance>30.0f) { /* the road's edge on the point's side, 24 units on */
        const unsigned span=road.profile_count-1,k=(uint16_t)p->phase[0];
        float ax,az,bx,bz;lane_point(k,0.0f,&ax,&az);lane_point(k,1.0f,&bx,&bz);
        const float lane=hypotf(target[0]-ax,target[1]-az)<hypotf(target[0]-bx,target[1]-bz) ? 0.15f : 0.85f;
        for(unsigned step=1;step<96;++step) {
            lane_point((k+step)%span,lane,&tx,&tz);
            if(hypotf(tx-x,tz-z)>=24.0f) break;
        }
    }
    const int desired=(int)lround(atan2(-(double)(tx-x),(double)(tz-z))*32768.0/M_PI);
    const int err=(int16_t)(uint16_t)(desired-(int)(uint16_t)p->boundary.ground.yaw);
    int w=-err*80/1400;if(w>80) w=80;if(w<-80) w=-80;
    raw[0]=(uint8_t)(0x80+w);
    const int over=(int)p->speed_index-target_speed;
    raw[1]=over<-10 ? 0xe0 : over<0 ? 0x60 : 0x20;raw[2]=over>0 ? 0xe0 : 0x20;
}

/* The menu of each course slot up to the race (frames before FIRST_FRAME): scripts/inputs/race_to_end.txt,
 * course_expert.txt (wheel to Long through the confirm edge), course_advanced.txt (Medium). */
static void write_menu(FILE *out,long first_frame) {
    fputs("1200-1209 coin=1\n1240-1249 coin=1\n1280-1289 coin=1\n1400-1409 start=1\n",out);
    if(course==0) fprintf(out,"1600-1609 start=1\n1800-1809 vr1=1\n2000-2009 start=1\n2200-2209 vr1=1\n2400-2409 start=1\n"
        "1400-%ld accel=0xe0\n",first_frame-1);
    else if(course==1) fprintf(out,"1560-1719 steer=0xe0\n1700-1719 accel=0xe0\n1860-1879 accel=0xe0\n1990-%ld accel=0xe0\n",first_frame-1);
    else fprintf(out,"1560-1679 steer=0xa0\n1660-1679 accel=0xe0\n1820-1839 accel=0xe0\n1950-%ld accel=0xe0\n",first_frame-1);
}

int main(int argc,char **argv) {
    uint32_t counter_base=NM_SLOT_MACHINE_COUNTER_BASE;
    const char *replay_path=NULL;
    setvbuf(stdout,NULL,_IOLBF,0);
    for(;;) {
        if(argc>1 && !strcmp(argv[1],"--manual")) { manual=1;argv[1]=argv[0];++argv;--argc; }
        else if(argc>1 && !strcmp(argv[1],"--pursuit")) { pursuit=1;argv[1]=argv[0];++argv;--argc; }
        else if(argc>2 && !strcmp(argv[1],"--launch")) {
            char *end;launch_raw=(int)strtol(argv[2],&end,0);launch_from=*end==':' ? strtol(end+1,NULL,10) : 0;
            argv[2]=argv[0];argv+=2;argc-=2;
        }
        else if(argc>2 && !strcmp(argv[1],"--replay")) { replay_path=argv[2];argv[2]=argv[0];argv+=2;argc-=2; }
        else if(argc>2 && !strcmp(argv[1],"--target")) {
            if(sscanf(argv[2],"%f,%f,%d,%d,%d",target,target+1,&target_profile,&target_near,&target_speed)!=5) return 2;
            argv[2]=argv[0];argv+=2;argc-=2;
        }
        else if(argc>2 && !strcmp(argv[1],"--course")) { course=(unsigned)strtoul(argv[2],NULL,10);argv[2]=argv[0];argv+=2;argc-=2; }
        else if(argc>2 && !strcmp(argv[1],"--counter-base")) { counter_base=(uint32_t)strtoul(argv[2],NULL,10);argv[2]=argv[0];argv+=2;argc-=2; }
        else break;
    }
    const int shadowed=argc==8 && !strcmp(argv[7],"shadow");
    if((argc!=6 && argc!=7 && !shadowed) || course>2 || (manual && course)) {
        fputs("usage: drive_race_native [--manual] [--course N] [--counter-base B] [--replay SCRIPT [--target X,Z,PROFILE,NEAR,SPEED]] ASSET_DIR NEW_SCRIPT FIRST_FRAME MAX_TICKS SPEED_CAP [CARRY_TRACE [shadow]]\n",stderr);
        return 2;
    }
    const long first_frame=strtol(argv[3],NULL,10),max_ticks=strtol(argv[4],NULL,10);
    const float speed_cap=strtof(argv[5],NULL);
    assert(first_frame>2000 && max_ticks>0);
    if(replay_path) read_replay(replay_path,first_frame,max_ticks);
    char path[1024];
#define ASSET(kind,route) (assert(nm_course_asset(path,sizeof(path),argv[1],course,kind,route)),path)
    math=nm_drive_load(ASSET(NM_COURSE_DRIVE,0));assert(math);
    assert(nm_contact_load(&contact,ASSET(NM_COURSE_CONTACT,0)));
    assert(nm_road_load(&road,ASSET(NM_COURSE_ROAD,0)));
    for(unsigned i=0;i<8;++i) assert(nm_path_load(paths+i,ASSET(NM_COURSE_ROUTE,i)));
    assert(nm_scenery_load(&scenery,ASSET(NM_COURSE_VISIBILITY,0)));
#undef ASSET
    assert(road.slot==course && contact.slot==course && scenery.slot==course);
    { double sum=0;const unsigned span=road.profile_count-1;
      for(unsigned i=0;i<span;++i) { float ax,az,bx,bz;lane_point(i,0.5f,&ax,&az);lane_point((i+1)%span,0.5f,&bx,&bz);sum+=hypot(bx-ax,bz-az); }
      profile_spacing=(float)(sum/span);printf("driver: %u profiles, mean spacing %g\n",span,(double)profile_spacing); }
    const nm_race_course_start_t *tables=nm_race_course_starts+course;
    if(!tables->tracks) { fprintf(stderr,"race_start_data.h does not hold course %u\n",course);return 2; }
    cameras.tracks=tables->tracks;cameras.tunnel=tables->tunnel;
#ifdef NM_RACE_VIEW_DATA
    cameras.pit_views=tables->pit_views;
#endif
    assert(road.profile_count<=sizeof(live.marks));
    nm_road_marks_t marks={live.marks,road.profile_count-1,0};
    if(!nm_race_scene_start(&live.race,math,&contact,&road,paths,1,(unsigned)manual,&marks,road.profile_count,tables->pit)) return 1;
#ifdef NM_RACE_VIEW_DATA
    live.race.view_presets=nm_race_view_presets; /* VR1..VR4, as code/practice.c */
#endif
    live.race.counter_base=counter_base;
    for(unsigned i=0;i<live.race.count;++i) {
        live.presentation.detail_order[i]=(uint8_t)i;live.presentation.detail_depth[i]=INT16_MAX;
    }
    long shadow_split=-1;
    if(shadowed) memcpy(&shadow,&live,sizeof(shadow));
    state_t *const seeded_state=shadowed ? &shadow : &live;
    if(argc>=7) {
        FILE *in=fopen(argv[6],"r");assert(in);
        static char line[4096];char frame_key[32];long row=-1;unsigned seeded=0;
        snprintf(frame_key,sizeof(frame_key)," frame=%ld ",first_frame-1);
        while(fgets(line,sizeof(line),in)) {
            if(line[0]=='T' && strstr(line,frame_key)) row=strtol(strstr(line," n=")+3,NULL,10);
            else if(line[0]=='C' && row>=0) {
                if(strtol(strstr(line," n=")+3,NULL,10)!=row) break;
                const long id=strtol(strstr(line," id=")+4,NULL,10);
                static const char *const keys[6]={" sx="," sz="," tx="," tz="," cside="," crel="};
                uint32_t words[6];
                for(unsigned k=0;k<6;++k) {
                    const char *at=strstr(line,keys[k]);assert(at);
                    words[k]=(uint32_t)strtoul(at+strlen(keys[k]),NULL,k==4 ? 10 : 16);
                }
                assert(id>=0 && id<(long)live.race.count);
                nm_race_car_t *c=seeded_state->race.cars+id;
                memcpy(c->collision_motion.previous,words,8);memcpy(c->collision_motion.travel,words+2,8);
                c->collision_side=(uint8_t)words[4];c->relative=words[5];++seeded;
            }
        }
        fclose(in);
        if(seeded!=live.race.count) { fprintf(stderr,"carry trace lacks the grid row (%u cars)\n",seeded);return 2; }
    }
    /* The menu sequence holds the wheel centred, accelerator down, brake up. */
    live.raw[0]=0x80;live.raw[1]=0xe0;live.raw[2]=0x20;live.accel=255;
    if(!race_detail(&live)) return 1;
    if(shadowed) {
        shadow.raw[0]=0x80;shadow.raw[1]=0xe0;shadow.raw[2]=0x20;shadow.accel=255;
        if(!race_detail(&shadow)) return 1;
    }

    FILE *out=fopen(argv[2],"wx");
    if(!out) { fprintf(stderr,"cannot create new script %s\n",argv[2]);return 2; }
    static const char *const menus[3]={"scripts/inputs/race_to_end.txt","recipes/inputs/course_expert.txt","recipes/inputs/course_advanced.txt"};
    fprintf(out,"# Generated by platform/dreamcast-native/tests/drive_race_native.c (speed cap %g). Host evidence only.\n"
        "# Menu sequence of %s, then one searched input per frame.\nframes %ld\n",
        (double)speed_cap,menus[course],first_frame+max_ticks+2000);
    write_menu(out,first_frame);
    if(manual) fputs("# manual transmission: steer right through the transmission select\n2010-2390 steer=0xe0\n",out);

    static const int wheel[]={0,-7,7,-10,10,-14,14,-19,19,-26,26,-36,36,-50,50,-80,80};
    uint8_t held[3]={0x80,0xe0,0x20};int held_shift=0;
    /* One run a channel (steer, accel, brake): a line when a channel's value changes, not at every change of any. */
    static const char *const channel[3]={"steer","accel","brake"};static const uint8_t rest[3]={0x80,0x20,0x20};
    uint8_t run[3]={0x80,0xe0,0x20};long run_start[3]={first_frame,first_frame,first_frame};
    long tick=0;unsigned last_lap=0;int status=0;
    for(;tick<max_ticks;++tick) {
        const nm_player_t *p=&live.race.cars[0].car;
        const int driving=live.race.active_phase==22 && live.race.phase==22 && !(live.race.start_flags&1u) &&
            !live.race.rolling[0];
        held_shift=0;
        if(replay) {
            const int ahead=target_profile-(int)(uint16_t)p->phase[0];
            if(target_speed>=0 && !target_engaged && driving && ahead>=0 && ahead<=target_near) {
                target_engaged=1;printf("driver: tick %ld target engaged at profile %u speed index %d\n",tick+1,(uint16_t)p->phase[0],(int)p->speed_index);
            }
            if(target_engaged) target_input(&live,held);
            else memcpy(held,replay[tick],3);
        }
        else if(!driving) { held[0]=0x80;held[1]=launch_accel(&live,tick);held[2]=0x20; }
        else if(pursuit) {
            static policy_t chosen={0};
            if(tick%4==0) {
                static const float lanes[]={0.15f,0.3f,0.5f,0.7f,0.85f},looks[]={1.5f,3.0f,5.0f};
                static const int pedals[][2]={{100000,100000},{2500,100000},{1500,4000}};
                double best=-1e30;
                for(unsigned l=0;l<3;++l) for(unsigned f=0;f<5;++f) for(unsigned e=0;e<3;++e) {
                    const policy_t q={looks[l]*24.0f,lanes[f],pedals[e][0],pedals[e][1]};
                    double value=policy_rollout(&q,150,speed_cap);
                    if(!memcmp(&q,&chosen,sizeof(q))) value+=0.5;
                    if(value>best) { best=value;chosen=q; }
                }
            }
            policy_input(&live,&chosen,held);
        }
        else if(tick%4==0) {
            double best=-1e30;uint8_t choice[3]={0x80,0xe0,0x20};int choice_shift=0;
            const int shifts=manual && !p->shift_ticks ? 3 : 1;
            for(unsigned w=0;w<sizeof(wheel)/sizeof(wheel[0]);++w) for(unsigned pedal=0;pedal<3;++pedal)
            for(int k=0;k<shifts;++k) {
                if(pedal && w>8) continue; /* Lift/brake only with moderate wheel. */
                const int shift=k==1 ? 1 : k==2 ? -1 : 0;
                if(shift && nm_shifter_sequential(p->gear,shift)==NM_SHIFTER_NEUTRAL) continue;
                const uint8_t raw[3]={(uint8_t)(0x80+wheel[w]),(uint8_t)(pedal ? 0x20 : 0xe0),(uint8_t)(pedal==2 ? 0xa0 : 0x20)};
                double value=rollout(raw,shift,pedal ? 36 : 48,speed_cap);
                /* Prefer keeping the current input (and gear) when values tie closely. */
                if(!memcmp(raw,held,3)) value+=0.05;
                if(shift) value-=0.1;
                if(value>best) { best=value;memcpy(choice,raw,3);choice_shift=shift; }
            }
            memcpy(held,choice,3);held_shift=choice_shift;
        }
        uint8_t now[3]={held[0],held[1],held[2]};
        if(driving && speed_cap>0 && p->boundary.speed>speed_cap) now[1]=0x20;
        if(target_engaged && (tick%10)==0)
            printf("driver: tick %ld at (%.1f %.1f) distance %.1f speed index %d yaw %d lateral %d raw %02x %02x %02x\n",tick+1,
                (double)p->boundary.ground.position[0],(double)p->boundary.ground.position[2],
                (double)hypotf(target[0]-p->boundary.ground.position[0],target[1]-p->boundary.ground.position[2]),
                (int)p->speed_index,(uint16_t)p->boundary.ground.yaw,p->lateral.current,now[0],now[1],now[2]);
        for(unsigned c=0;c<3;++c) if(now[c]!=run[c]) {
            const long end=first_frame+tick-1;
            if(run[c]!=rest[c] && end>=run_start[c]) fprintf(out,"%ld-%ld %s=0x%02x\n",run_start[c],end,channel[c],run[c]);
            run[c]=now[c];run_start[c]=first_frame+tick;
        }
        const int result=advance(&live,now,held_shift);
        if(live.shift_to>=0) fprintf(out,"%ld gear%d=1\n",first_frame+tick,live.shift_to+1);
        if(shadowed && shadow_split<0) {
            int differ=advance(&shadow,now,held_shift)!=result;
            for(unsigned id=0;!differ && result==1 && id<live.race.count;++id) {
                const nm_player_t *a=&live.race.cars[id].car,*b=&shadow.race.cars[id].car;
                differ=memcmp(a->boundary.ground.position,b->boundary.ground.position,sizeof(a->boundary.ground.position)) ||
                    memcmp(&a->boundary.speed,&b->boundary.speed,sizeof(a->boundary.speed));
            }
            if(differ) shadow_split=tick+1;
        }
        if(result==2) { printf("driver: scene handoff phase=%u before tick %ld\n",live.race.phase,tick+1);break; }
        if(result!=1) { printf("driver: scene step rejected at tick %ld\n",tick+1);status=1;break; }
        p=&live.race.cars[0].car;
        if((tick+1)%500==0)
            printf("driver: tick %ld profile %u speed %g lateral %d rank %u damage %d\n",tick+1,(uint16_t)p->phase[0],
                (double)p->boundary.speed,p->lateral.current,live.race.cars[0].rank,(int)p->boundary.damage.remaining);
        if(p->lap.completed!=last_lap || live.race.phase!=live.race.active_phase) {
            printf("driver: tick %ld lap %u/%u rank %u phase %u->%u clock %" PRIu32 " remaining %" PRId32 " finish %" PRIx32 "\n",
                tick+1,p->lap.completed,p->checkpoints.total_laps,live.race.cars[0].rank,live.race.active_phase,
                live.race.phase,live.race.shared.clock,(int32_t)live.race.award.remaining,p->checkpoints.finish_flags);
            last_lap=p->lap.completed;
        }
    }
    const long end=first_frame+tick;
    for(unsigned c=0;c<3;++c)
        if(run[c]!=rest[c] && end>=run_start[c]) fprintf(out,"%ld-%ld %s=0x%02x\n",run_start[c],end,channel[c],run[c]);
    const int failed=ferror(out);
    if(fclose(out) || failed) return 2;
    if(shadowed) {
        if(shadow_split>0) printf("driver: cold and carried races first differ at tick %ld\n",shadow_split);
        else printf("driver: cold and carried races identical for all %ld ticks: none\n",tick);
    }
    printf("driver: ticks=%ld phase=%u active=%u rank=%u laps=%u clock=%" PRIu32 " remaining=%" PRId32 " profile=%u speed=%g status=%d\n",
        tick,live.race.phase,live.race.active_phase,live.race.cars[0].rank,live.race.cars[0].car.lap.completed,
        live.race.shared.clock,(int32_t)live.race.award.remaining,(uint16_t)live.race.cars[0].car.phase[0],
        (double)live.race.cars[0].car.boundary.speed,status);
    return status;
}
