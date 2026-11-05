/* The race's sound commands as the game sends them: see include/audio/race_sound_hooks.h.
 * Compiled without LTO (Makefile), like audio/sfx.c. */
#include "audio/race_sound_hooks.h"
#include "audio/sound.h"
#include "race_sound.h"
#include "audio/sfx.h"
#include "race_intent_data.h"
#include <stdio.h>

static struct {
    nm_race_sound_t last; /* race_sound.h: the last engine/slip values sent */
    uint8_t front_smoke;  /* 501aa0: be3701 sent and not yet be3700 (from power-on) */
    nm_race_sound_beep_t beep; /* 501880/501884 (from power-on) */
    uint32_t clock;       /* 5010cc before the tick: the phase handler stepped it when it changed */
    uint8_t crashes;      /* the player's crash count +c7 before the tick */
    nm_race_sound_cooldowns_t cooldowns; /* 501a9c/501aa4 (from power-on) */
    uint8_t ambient[2];   /* 503068/503069: the last be35/be34 levels (0 at every race's phase 20, see reset) */
    int16_t remaining[44]; /* the opponents' crash rows left before the tick: a start raises it */
    int8_t inhibit[44];    /* their input inhibit before the tick: a spin start sets it from 0 */
    unsigned commands;    /* commands from race_sound.h this session (diagnostic) */
    uint32_t hash;
} race_sound;

/* Played and counted apart from the silent event sink's (practice.c race_commands), whose totals
 * stay comparable with earlier runs. */
static void play(uint32_t command) {
    if(!command) return;
    ++race_sound.commands;
    race_sound.hash=(race_sound.hash^command)*UINT32_C(16777619);
    nm_sound_command(command);
}

static void counted(uint32_t *commands,uint32_t *hash,uint32_t command) {
    if(!command) return;
    ++*commands;*hash=(*hash^command)*UINT32_C(16777619);
    nm_sound_command(command);
}

void nm_race_sound_hook_reset(int boot) {
    if(boot) { race_sound.last=NM_RACE_SOUND_BOOT;race_sound.front_smoke=0;race_sound.beep=(nm_race_sound_beep_t){0,0};
               race_sound.cooldowns=(nm_race_sound_cooldowns_t){0,0}; }
    /* 18f30 sets 503068/503069 to 0xff, but the attract and the race's entry run the same event tail before
     * phase 20 (gate shut, the car outside the source's stretch): every race starts from 0. */
    race_sound.ambient[0]=race_sound.ambient[1]=0;
    race_sound.commands=0;race_sound.hash=UINT32_C(2166136261);
}

uint32_t nm_race_sound_hook_before(const nm_race_scene_t *race,unsigned profile_count) {
    race_sound.clock=race->shared.clock;race_sound.crashes=race->cars[0].car.boundary.damage.crash_count;
    for(unsigned i=1;i<race->count && i<44;++i) {
        race_sound.remaining[i]=race->cars[i].car.boundary.damage.remaining;
        race_sound.inhibit[i]=race->cars[i].car.input_inhibit;
    }
    if(race->phase<20 || race->phase>24) return 0;
    return nm_race_sound_countdown(race->phase,UINT32_C(1)<<race->phase,race->start_stage,
        (int)profile_count-1-race->cars[0].car.phase[0],profile_count-1);
}

/* The streamed phrases' prefetch (race_sound.h nm_race_sound_hints, course 0's tables). */
static const nm_road_awards_t *course_awards; /* nm_race_sound_hook_course */
void nm_race_sound_hook_course(const nm_road_awards_t *awards) { course_awards=awards; }

static void hints(const nm_race_scene_t *race) {
    uint32_t keys[8];
    const unsigned n=nm_race_sound_hints(course_awards,race->award.counter,
        nm_race_announcement_rows+nm_race_announcement_offsets[0],
        nm_race_announcement_counts[0],nm_race_announcement_ranks,nm_race_intent_rows,nm_race_intent_alternate[0],
        race->announcement_cursor,race->cars[0].rank,race->award.remaining,race->shared.clock,keys);
    (void)nm_race_intent_special;
    nm_sfx_hint(keys,n);
}

void nm_race_sound_hook_tick(const nm_race_scene_t *race,uint32_t countdown) {
    hints(race);
    play(countdown);
    if(race->active_phase==22 && race->phase==22 && race->shared.clock!=race_sound.clock) /* 35e0..36e0 */
        play(nm_race_sound_beep(&race_sound.beep,race->shared.flags,race->award.remaining));
    uint32_t cues[3];
    for(unsigned i=0,n=nm_race_sound_award(&race->award,race->shared.flags,cues);i<n;++i) play(cues[i]);
    play(nm_race_sound_spin(race->shared.flags,race->cars[0].car.early_sound_request));
    uint32_t crash[8];
    for(unsigned i=0,n=nm_race_sound_crash(race->shared.flags,race_sound.crashes,&race->cars[0].car,crash);i<n;++i)
        play(crash[i]);
    /* The opponents (rows 13-55): each one's crash start, then its spin start; then row 56's countdown. */
    for(unsigned i=1;i<race->count && i<44;++i) {
        const nm_player_t *car=&race->cars[i].car;
        if(car->boundary.damage.remaining>race_sound.remaining[i])
            play(nm_race_sound_opponent_cue(&race_sound.cooldowns.crash,nm_race_sound_opponent_crash,race->shared.flags,
                car,&race->cars[0].car));
        if(!race_sound.inhibit[i] && car->input_inhibit)
            play(nm_race_sound_opponent_cue(&race_sound.cooldowns.spin,nm_race_sound_opponent_spin,race->shared.flags,
                car,&race->cars[0].car));
    }
    nm_race_sound_cooldowns_tick(&race_sound.cooldowns);
}

void nm_race_sound_hook_events(const nm_race_scene_t *race,uint32_t frame_counter) {
    uint32_t out[2];
    for(unsigned i=0,n=nm_race_sound_ambient(race_sound.ambient,race->shared.flags,frame_counter,&race->cars[0].car,out);
        i<n;++i) play(out[i]);
}

void nm_race_sound_hook_finish(void) {
    play(NM_RACE_SOUND_RPM_OFF);play(NM_RACE_SOUND_SLIP_OFF);
}

void nm_race_sound_hook_draw(nm_race_scene_t *race) {
    nm_player_t *p=&race->cars[0].car;
    play(nm_race_sound_front_smoke(&race_sound.front_smoke,p->force.trigger_count[0]));
    play(nm_race_sound_skid(race->shared.flags,p->force.trigger_count[1],p->force.grip[1]));
    const int16_t lane=nm_race_sound_wall_lane(p);
    if(lane) p->boundary.damage.signal=lane;
}

void nm_race_sound_hook_hud(const nm_race_scene_t *race) {
    const nm_player_t *p=&race->cars[0].car;
    play(nm_race_sound_rpm(&race_sound.last,race->shared.flags,p->filtered_rpm));
    play(nm_race_sound_slip(&race_sound.last,race->shared.flags,p->force.tire_angle[1]));
}

void nm_race_sound_hook_gated(const nm_race_scene_t *race,uint32_t command) {
    if(race->shared.flags&NM_RACE_SOUND_GATE) nm_sound_command(command);
}

void nm_race_sound_hook_results_off(uint32_t *commands,uint32_t *hash) {
    counted(commands,hash,NM_RACE_SOUND_RPM_OFF);
    counted(commands,hash,NM_RACE_SOUND_SLIP_OFF);
}

void nm_race_sound_hook_results_gated(const nm_race_scene_t *race,uint32_t *commands,uint32_t *hash,
                                      uint32_t command) {
    if(race->shared.flags&NM_RACE_SOUND_GATE) counted(commands,hash,command);
}

void nm_race_sound_hook_engine_off(void) {
    nm_sound_command(NM_RACE_SOUND_RPM_OFF);nm_sound_command(NM_RACE_SOUND_SLIP_OFF);
}

void nm_race_sound_hook_report(void) {
    printf("Native race sound (race_sound.h): commands=%u hash=%08lx\n",race_sound.commands,
        (unsigned long)race_sound.hash);
}
