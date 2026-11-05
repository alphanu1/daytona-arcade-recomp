#ifndef DAYTONA_NATIVE_RACE_SOUND_H
#define DAYTONA_NATIVE_RACE_SOUND_H
#include <math.h>
#include <stdint.h>
#include "checkpoint.h"
#include "player.h"
#include "race_intents.h"
#include "road.h"

/* The race's sound commands that no gameplay owner returns: the original computes them
 * from gameplay state it does not change, so the native game takes them from its own state
 * after the tick. Presentation only: nothing here writes gameplay state. Each returns the
 * three-byte command the original queues (0: none); include/audio/sound.h plays them.
 *
 * 19bdc, the gated enqueue, queues only while the latched flags 501090 & 0x500000 select
 * phase 20 or 22 (race_scene.h shared.flags). */
#define NM_RACE_SOUND_GATE UINT32_C(0x500000)

typedef struct {
    int16_t rpm;  /* 503006: the last engine value 19c58 sent */
    int16_t slip; /* 50300a: the last slip value 19d1c sent */
} nm_race_sound_t;
/* The sound start-up 18f30 sets both to -1 (with 503068/503069, the last be34/be35 values);
 * the race never resets the slip (19d00/19dac write 503004/503008), and only the object at 4ddc
 * sets the rpm to 0 (no effect on what is sent: the race's first value is at least 1). */
#define NM_RACE_SOUND_BOOT ((nm_race_sound_t){-1,-1})

/* 19c58 (HUD object a7c0, after a818..b1a4): engine rpm from the player's +154 (filtered
 * rpm), clamped to 1800..8500, (rpm-1800)*128/5300 (divi) clamped to 1..127; queued as
 * be17xx when it differs from the last value sent. */
static inline uint32_t nm_race_sound_rpm(nm_race_sound_t *s,uint32_t flags,int32_t filtered_rpm) {
    if(!(flags&NM_RACE_SOUND_GATE)) return 0;
    int32_t rpm=filtered_rpm<1800 ? 1800 : filtered_rpm>8500 ? 8500 : filtered_rpm;
    int32_t value=((rpm-1800)<<7)/0x14b4;
    value=value<1 ? 1 : value>127 ? 127 : value;
    if(value==s->rpm) return 0;
    s->rpm=(int16_t)value;
    return UINT32_C(0xbe1700)|(uint32_t)value;
}

/* 19d1c (after 19c58): tyre slip from the player's +18a (rear tire angle, ldis): a = angle
 * shri 1 (arithmetic), |a| - 0x16c (none below), /0xb6 (divo) at most 7, the sign of a;
 * queued as be19(8+v) when it differs from the last value sent. 8 is no slip. */
static inline uint32_t nm_race_sound_slip(nm_race_sound_t *s,uint32_t flags,int16_t tire_angle) {
    if(!(flags&NM_RACE_SOUND_GATE)) return 0;
    const int32_t half=(int32_t)tire_angle>>1;
    const int positive=half>0;
    int32_t value=(positive ? half : -half)-0x16c;
    if(value<0) value=0;
    else {
        value=(int32_t)((uint32_t)value/0xb6u);
        if(value>7) value=7;
        if(positive) value=-value;
    }
    if(value==s->slip) return 0;
    s->slip=(int16_t)value;
    return (uint32_t)(UINT32_C(0xbe1908)+(uint32_t)value);
}

/* 134f8/135e0, the player's lap award (checkpoint.h nm_checkpoint_award_step): its cues of
 * this tick (the lap voice, or ae207c + ae2108 for the final lap, then ae2010) through 19bdc.
 * The player tick clears cue_count first, so after nm_race_scene_step it holds this tick's. */
static inline unsigned nm_race_sound_award(const nm_checkpoint_award_t *award,uint32_t flags,uint32_t out[3]) {
    if(!(flags&NM_RACE_SOUND_GATE)) return 0;
    unsigned n=0;
    for(unsigned i=0;i<award->cue_count && i<3;++i) if(award->cues[i]) out[n++]=award->cues[i];
    return n;
}

/* 158c8 in the player's update (nm_player_early_mode): the spin's sound 9e067f through 19bdc when the
 * input inhibit starts (bit 16/15) or its timer reaches 0x50; player.h keeps it as early_sound_request. */
static inline uint32_t nm_race_sound_spin(uint32_t flags,uint8_t early_sound_request) {
    return (flags&NM_RACE_SOUND_GATE) && early_sound_request ? UINT32_C(0x9e067f) : 0;
}

/* 364c..36e0, the phase 22 handler after its clock step (35e0) while 0 < remaining < 384: the byte
 * 501880 counts down; below zero it reloads from 231e48 by remaining/64 and 501884 flips, choosing
 * 231e60's 9e0e7f / 9e0d7f, queued through 19bdc (gated). Both bytes are zero from power-on and written
 * nowhere else. Call once per tick whose phase handler stepped the clock in phase 22. */
typedef struct { int8_t count; uint8_t toggle; } nm_race_sound_beep_t;
static inline uint32_t nm_race_sound_beep(nm_race_sound_beep_t *b,uint32_t flags,uint32_t remaining) {
    static const int8_t reload[8]={4,4,5,6,8,12,16,20};
    const int32_t left=(int32_t)remaining;
    if(left<=0 || left>=384) return 0;
    const int32_t count=(int32_t)b->count-1;
    b->count=(int8_t)count;
    if(count>=0) return 0;
    b->count=reload[(uint32_t)left>>6];
    b->toggle=(uint8_t)((b->toggle+1u)&1u);
    if(!(flags&NM_RACE_SOUND_GATE)) return 0;
    return b->toggle ? UINT32_C(0x9e0d7f) : UINT32_C(0x9e0e7f);
}

/* The player's crash and impact sounds of a tick, in the original's order (row 12, then row 56), all
 * through 19bdc. damage.h and boundary.h compute them as per-call intents; these are the ones a tick
 * leaves in the player's state:
 *  - 15f78/15f88, the crash sequence's advance in the pre-step (d888 -> 15fc4): a row marker over 256
 *    40..79 ticks after the last one 9e087f, 80 or more 9e1b7f (pre_sound_requests);
 *  - 16788, the wall response 166f4 without a running +99: 9e077f (boundary.pivot_sound);
 *  - 15d3c, each crash start 15cc8 while no phase from 23 is latched: 9e0a7f (crash count +c7 stepped).
 * crashes_before: the crash count before the tick. Returns how many of out[] it filled. */
static inline unsigned nm_race_sound_crash(uint32_t flags,uint8_t crashes_before,const nm_player_t *p,uint32_t out[8]) {
    unsigned n=0;
    if(!(flags&NM_RACE_SOUND_GATE)) return 0;
    if(p->pre_sound_requests&NM_CRASH_SOUND_SMALL) out[n++]=UINT32_C(0x9e087f);
    if(p->pre_sound_requests&NM_CRASH_SOUND_LARGE) out[n++]=UINT32_C(0x9e1b7f);
    if(p->boundary.pivot_sound) out[n++]=UINT32_C(0x9e077f);
    for(uint8_t c=crashes_before;c!=p->boundary.damage.crash_count && n<8;++c) out[n++]=UINT32_C(0x9e0a7f);
    return n;
}

/* 15b90 / 15c0c, an opponent's spin start (15ae4, intro_math.h nm_opponent_inhibit_step returning 2) and
 * crash start (15e10, nm_intro_crash_step): a cue by the car's distance to the player (TGP 43 on x/z, its
 * sign cleared, rounded, /8) from 235a38 (9e06) or 235a58 (9e0a), none from 64 on; then the cue's
 * cooldown 501a9c / 501aa4 := 8 (also when 19bdc's gate drops the command). The interaction object's tail
 * (1219c/121b8, row 56) counts both down every tick. Zero from power-on. */
typedef struct { int32_t spin, crash; } nm_race_sound_cooldowns_t;
static const uint32_t nm_race_sound_opponent_spin[8]={
    0x9e067f,0x9e066f,0x9e065f,0x9e064f,0x9e063f,0x9e062f,0x9e061f,0x9e060f};
static const uint32_t nm_race_sound_opponent_crash[8]={
    0x9e0a4f,0x9e0a4f,0x9e0a3f,0x9e0a3f,0x9e0a2f,0x9e0a2f,0x9e0a1f,0x9e0a0f};
static inline uint32_t nm_race_sound_opponent_cue(int32_t *cooldown,const uint32_t table[8],uint32_t flags,
        const nm_player_t *car,const nm_player_t *player) {
    if(*cooldown) return 0;
    const float dx=car->boundary.ground.position[0]-player->boundary.ground.position[0];
    const float dz=car->boundary.ground.position[2]-player->boundary.ground.position[2];
    const float distance=sqrtf(dx*dx+dz*dz);
    if(!(distance<64.0f)) return 0;
    const int32_t index=(int32_t)lrintf(distance)>>3;
    if(index>7) return 0;
    *cooldown=8;
    return flags&NM_RACE_SOUND_GATE ? table[index] : 0;
}
static inline void nm_race_sound_cooldowns_tick(nm_race_sound_cooldowns_t *c) {
    c->spin=c->spin>0 ? c->spin-1 : 0;c->crash=c->crash>0 ? c->crash-1 : 0;
}

/* 19590, the event tail 19038's last call (row 56, after the announcements), on frames with 5010b0 & 3 == 0:
 * the course's ambient source, a level each for the right (be35) and left (be34) layers. Course 0
 * (197d8): while the player's profile +128 is 0x77..0x79, d = TGP 43 from the player (x, z) to
 * (65.13, 144.0); each level 127 - round(8 (18 - 0.8 |d - 100|)), clamped to 1..127; outside, 0. The
 * player's wrong way (flags bit 28) swaps the two. Each is queued (19bdc) when it differs from the last
 * value 503068 / 503069 (both 0xff from the sound start-up 18f30, also when the gate drops it). The other
 * courses' branches (195d4, 19764) are not native: they have no course. Returns how many of out[2]. */
static inline unsigned nm_race_sound_ambient(uint8_t last[2],uint32_t flags,uint32_t frame_counter,
        const nm_player_t *p,uint32_t out[2]) {
    if(frame_counter&3u) return 0;
    int32_t right=0,left=0;
    const uint32_t profile=(uint16_t)p->phase[0];
    if(profile>=0x77u && profile<=0x79u) {
        const float dx=p->boundary.ground.position[0]-65.13f,dz=p->boundary.ground.position[2]-144.0f;
        const float d=sqrtf(dx*dx+dz*dz);
        const float near=fabsf(0.8f*(d-100.0f));
        right=left=127-(int32_t)lrintf(8.0f*(18.0f-near));
        right=right<1 ? 1 : right>127 ? 127 : right;
        left=right;
    }
    if(p->boundary.damage.flags&(UINT32_C(1)<<28)) { const int32_t t=right;right=left;left=t; }
    unsigned n=0;
    if((uint8_t)right!=last[0]) { last[0]=(uint8_t)right; if(flags&NM_RACE_SOUND_GATE) out[n++]=UINT32_C(0xbe3500)|(uint32_t)right; }
    if((uint8_t)left!=last[1]) { last[1]=(uint8_t)left; if(flags&NM_RACE_SOUND_GATE) out[n++]=UINT32_C(0xbe3400)|(uint32_t)left; }
    return n;
}

/* The announcer's phrases a race is likely to request next, for the streamed phrases' prefetch (audio/sfx.h),
 * most useful first: the next lap award's voice and the phrase of its signal (134f8: cue[counter], signal[counter]
 * of the course's awards), the next two rows of the announcement creator 19320 from cursor (a rank row by the
 * current rank) as race_intents.h turns their signals into commands, the time call 35b0 is about to raise
 * (within 600 ticks), the race's first call (row 26, ac24 after GO) until 300 ticks into the race, then the most
 * frequent reactions: repeated wall hits (row 31, 166fc) and the wrong way (row 30). Tables: race_intent_data.h's,
 * awards: the road pack's (NULL: none). */
static inline unsigned nm_race_sound_hints(const nm_road_awards_t *awards,unsigned award_counter,
        const nm_race_announcement_row_t *rows,unsigned count,
        const uint32_t ranks[9],const nm_race_intent_row_t *intents,unsigned alternate,uint32_t cursor,
        unsigned rank,uint32_t remaining,uint32_t clock,uint32_t out[8]) {
    unsigned n=0;
    if(awards) {
        const unsigned i=award_counter&15u;
        if(awards->cue[i]==UINT32_C(0xae2108)) out[n++]=UINT32_C(0xae2108);
        else {
            out[n++]=awards->cue[i];
            if(awards->signal[i] && awards->signal[i]<57) out[n++]=intents[awards->signal[i]].command[alternate];
        }
    }
    for(unsigned k=0;k<2;++k) {
        if(cursor+k>=count) break;
        uint32_t signal=rows[cursor+k].signal;
        if(rows[cursor+k].predicate==8) signal=rank<=8 ? ranks[rank] : 0;
        if(signal && signal<57) out[n++]=intents[signal].command[alternate];
    }
    if(remaining>640u && remaining<=640u+600u) out[n++]=intents[28].command[alternate];
    else if(remaining>385u && remaining<=385u+600u) out[n++]=intents[29].command[alternate];
    if(clock<300u) out[n++]=intents[26].command[alternate];
    out[n++]=intents[31].command[alternate];
    out[n++]=intents[30].command[alternate];
    return n;
}

/* 90f0, the player's draw (9088, between the front smoke 97f8 and the rear skid 9888): while the
 * front tyre smoke timer +95 (force.trigger_count[0]) runs, be3701 once, be3700 once it has stopped;
 * 501aa0 remembers which was sent (zero from power-on, never reset). Queued through 19bf4, the
 * enqueue without 19bdc's gate: also outside phases 20/22. */
static inline uint32_t nm_race_sound_front_smoke(uint8_t *sent,int8_t front_timer) {
    if(front_timer) { if(*sent==1) return 0; *sent=1; return UINT32_C(0xbe3701); }
    if(!*sent) return 0;
    *sent=0; return UINT32_C(0xbe3700);
}

/* 9148, the player's draw, once for +98 and once for +99: unless it clears both timers (|+d6| over
 * 0x1000, contact_clear.h), the spark's side +ed writes the intent lane 501a04 (boundary.damage.signal):
 * 2 on the left (918c), 6 on the right (91cc); race_intents.h turns them into the wall rub 9e0c/9e0b.
 * Gameplay state, as in the original: call before nm_player_contact_clear. 0: no write. */
static inline int16_t nm_race_sound_wall_lane(const nm_player_t *p) {
    if(!p->response_ticks && !p->boundary.condition) return 0;
    if(p->inhibit_angle<-0x1000 || p->inhibit_angle>0x1000) return 0;
    return p->boundary.damage.side==0 ? 2 : p->boundary.damage.side==3 ? 6 : 0;
}

/* 9888..9968, the player's draw (6c0c -> 9088, after 9014's timer decrement: nm_player_effect_timers):
 * while the rear tyre smoke timer +96 (force.trigger_count[1]) runs, its step 23 (the byte & 31) queues
 * the skid through 19bdc by the rear grip +180 (force.grip[1]): below 0.2 9e257f (TIRE SKID & REV high),
 * below 0.4 9e135f, else 9e247f (low). */
static inline uint32_t nm_race_sound_skid(uint32_t flags,int8_t rear_timer,float rear_grip) {
    if(!(flags&NM_RACE_SOUND_GATE) || !rear_timer || ((uint8_t)rear_timer&31u)!=23u) return 0;
    return rear_grip<0.2f ? UINT32_C(0x9e257f) : rear_grip<0.4f ? UINT32_C(0x9e135f) : UINT32_C(0x9e247f);
}

/* 19d00/19dac: engine and slip off (attract entry 1cd4/1cd8, finish 44c8/44cc after the
 * finish music, results entry b260/b264). Ungated; the last-sent values stay. */
#define NM_RACE_SOUND_RPM_OFF UINT32_C(0xbe1700)
#define NM_RACE_SOUND_SLIP_OFF UINT32_C(0xbe1900)

/* ab78..ad30, the phase22 handler's first call (race_start.h nm_race_start_sequence): the start
 * voice this tick queues through 19bdc. stage: 501850 before nm_race_scene_step; remaining:
 * wrap - the player's profile (+128, phase[0]) before it, wrap = profile count - 1, as the
 * sequence reads them. 29, 19, 9 profiles before the line: ae2109 THREE, ae210a TWO, ae210b ONE
 * (each only from its stage 0, 1, 2); from 0x5a profiles before the wrap, leaving stage 3:
 * ae210c GO. Stage 64 and above: the sequence is over. */
static inline uint32_t nm_race_sound_countdown(unsigned active_phase,uint32_t flags,int16_t stage,
        int remaining,unsigned wrap) {
    if(active_phase!=22 || !(flags&NM_RACE_SOUND_GATE) || stage>=64) return 0;
    if(remaining>=(int)wrap-0x5a) return stage==3 ? UINT32_C(0xae210c) : 0;
    if(remaining==29) return stage==0 ? UINT32_C(0xae2109) : 0;
    if(remaining==19) return stage==1 ? UINT32_C(0xae210a) : 0;
    if(remaining==9) return stage==2 ? UINT32_C(0xae210b) : 0;
    return 0;
}
#endif
