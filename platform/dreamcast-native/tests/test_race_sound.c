/* include/race_sound.h against hand-worked values of the original's routines (19c58, 19d1c,
 * ab78, 134f8 through 19bdc). The whole-race check is the recipe's step sound_check:
 * tests/trace_race_native.c --sound against the reference race (tools/compare_race_sound.py). */
#include "race_sound.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define EXPECT(cond) do { if(!(cond)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond);++failures; } } while(0)

int main(void) {
    const uint32_t race=UINT32_C(1)<<22, start=UINT32_C(1)<<20, finish=UINT32_C(1)<<24;
    nm_race_sound_t s=NM_RACE_SOUND_BOOT;
    /* Engine: 1800 and below give 1, the clamp; 8500 and above 127 (161 before the clamp). */
    EXPECT(nm_race_sound_rpm(&s,start,0)==0xbe1701);
    EXPECT(nm_race_sound_rpm(&s,race,1700)==0);             /* still 1: not sent again */
    EXPECT(nm_race_sound_rpm(&s,race,1800+5300/2)==0xbe1740); /* 2650*128/5300 = 64 */
    EXPECT(nm_race_sound_rpm(&s,race,1841)==0xbe1701);     /* 41*128/5300 = 0 -> 1 */
    EXPECT(nm_race_sound_rpm(&s,race,1842)==0);            /* 42*128/5300 = 1, unchanged */
    EXPECT(nm_race_sound_rpm(&s,race,1882)==0);            /* 82*128/5300 = 1 */
    EXPECT(nm_race_sound_rpm(&s,race,1883)==0xbe1702);     /* 83*128/5300 = 2 */
    EXPECT(nm_race_sound_rpm(&s,race,9000)==0xbe177f);
    EXPECT(nm_race_sound_rpm(&s,finish,1800)==0 && s.rpm==127); /* gated: nothing, nothing kept */
    /* Slip: half the angle, its magnitude less 364, /182, at most 7; negative angle -> 8+v. */
    s=NM_RACE_SOUND_BOOT;
    EXPECT(nm_race_sound_slip(&s,race,0)==0xbe1908);       /* -1 at boot: 0 is new */
    EXPECT(nm_race_sound_slip(&s,race,729)==0);            /* 364 - 364 = 0 */
    EXPECT(nm_race_sound_slip(&s,race,-1092)==0xbe1909);   /* -546: 546-364 = 182 -> 1, plus */
    EXPECT(nm_race_sound_slip(&s,race,1092)==0xbe1907);    /* +546 -> -1 */
    EXPECT(nm_race_sound_slip(&s,race,-32768)==0xbe190f);  /* 16384-364 = 16020 / 182 = 88 -> 7 */
    EXPECT(nm_race_sound_slip(&s,race,32767)==0xbe1901);   /* 16383 -> -7 */
    EXPECT(nm_race_sound_slip(&s,race,-3)==0xbe1908);      /* -3 shri 1 = -2 (rounds down), within */
    EXPECT(nm_race_sound_slip(&s,race,-3)==0);             /* unchanged */
    EXPECT(nm_race_sound_slip(&s,finish,-32768)==0);
    /* Countdown: wrap 1000; THREE 29 profiles out from stage 0 only, GO leaving stage 3 from 0x5a. */
    EXPECT(nm_race_sound_countdown(22,race,0,29,1000)==0xae2109);
    EXPECT(nm_race_sound_countdown(22,race,1,29,1000)==0);
    EXPECT(nm_race_sound_countdown(22,race,1,19,1000)==0xae210a);
    EXPECT(nm_race_sound_countdown(22,race,2,9,1000)==0xae210b);
    EXPECT(nm_race_sound_countdown(22,race,3,1000-0x5a,1000)==0xae210c);
    EXPECT(nm_race_sound_countdown(22,race,0,1000-0x5a,1000)==0); /* counting from stage 0: silent */
    EXPECT(nm_race_sound_countdown(22,race,4,1000,1000)==0);
    EXPECT(nm_race_sound_countdown(22,race,64,29,1000)==0);
    EXPECT(nm_race_sound_countdown(21,race,0,29,1000)==0);
    EXPECT(nm_race_sound_countdown(22,finish,0,29,1000)==0);
    /* Lap award cues as the award left them; gated. */
    nm_checkpoint_award_t a={0};
    a.cues[0]=0xae207c;a.cues[1]=0xae2108;a.cues[2]=0xae2010;a.cue_count=3;
    uint32_t out[3];
    EXPECT(nm_race_sound_award(&a,race,out)==3 && out[0]==0xae207c && out[2]==0xae2010);
    EXPECT(nm_race_sound_award(&a,finish,out)==0);
    a.cue_count=0;
    EXPECT(nm_race_sound_award(&a,race,out)==0);
    /* Skid: the rear smoke timer's step 23 (byte & 31), by rear grip; spin: the player's request. */
    EXPECT(nm_race_sound_skid(race,23,0.1f)==0x9e257f);
    EXPECT(nm_race_sound_skid(race,23,0.3f)==0x9e135f);
    EXPECT(nm_race_sound_skid(race,(int8_t)-9,0.5f)==0x9e247f); /* 0xf7 & 31 = 23 */
    EXPECT(nm_race_sound_skid(race,24,0.1f)==0 && nm_race_sound_skid(race,0,0.1f)==0 && nm_race_sound_skid(finish,23,0.1f)==0);
    EXPECT(nm_race_sound_spin(race,1)==0x9e067f && nm_race_sound_spin(race,0)==0 && nm_race_sound_spin(finish,1)==0);
    /* be37 (90f0): once when the front smoke starts, once when it stops; ungated. */
    uint8_t sent=0;
    EXPECT(nm_race_sound_front_smoke(&sent,5)==0xbe3701 && nm_race_sound_front_smoke(&sent,4)==0 && sent==1);
    EXPECT(nm_race_sound_front_smoke(&sent,0)==0xbe3700 && nm_race_sound_front_smoke(&sent,0)==0 && !sent);
    /* 9148's lane: a running contact timer, |+d6| <= 0x1000, side 0 -> 2, 3 -> 6, others none. */
    nm_player_t p; memset(&p,0,sizeof(p));
    EXPECT(nm_race_sound_wall_lane(&p)==0);
    p.boundary.condition=3; EXPECT(nm_race_sound_wall_lane(&p)==2);
    p.boundary.damage.side=3; p.inhibit_angle=0x1000; EXPECT(nm_race_sound_wall_lane(&p)==6);
    p.inhibit_angle=0x1001; EXPECT(nm_race_sound_wall_lane(&p)==0);
    p.inhibit_angle=0; p.boundary.damage.side=4; EXPECT(nm_race_sound_wall_lane(&p)==0);
    /* 364c: below 384 left, the counter reloads by remaining/64 and the two beeps alternate; 0 and 384 none. */
    nm_race_sound_beep_t beep={0,0};
    EXPECT(nm_race_sound_beep(&beep,race,384)==0 && nm_race_sound_beep(&beep,race,0)==0 && beep.count==0);
    EXPECT(nm_race_sound_beep(&beep,race,383)==0x9e0d7f && beep.count==12);   /* 383/64 = 5: 12 */
    for(int i=0;i<12;++i) EXPECT(nm_race_sound_beep(&beep,race,300)==0);
    EXPECT(nm_race_sound_beep(&beep,race,63)==0x9e0e7f && beep.count==4);
    EXPECT(nm_race_sound_beep(&beep,finish,63)==0 && beep.count==3);           /* gated: counts on */
    /* Crash family: the pre-step's 9e08 / 9e1b, the wall's 9e07, one 9e0a per crash count step. */
    uint32_t o8[8];
    memset(&p,0,sizeof(p));
    p.pre_sound_requests=NM_CRASH_SOUND_SMALL|NM_CRASH_SOUND_LARGE; p.boundary.pivot_sound=1;
    p.boundary.damage.crash_count=2;
    EXPECT(nm_race_sound_crash(race,0,&p,o8)==5 && o8[0]==0x9e087f && o8[1]==0x9e1b7f && o8[2]==0x9e077f &&
           o8[3]==0x9e0a7f && o8[4]==0x9e0a7f);
    EXPECT(nm_race_sound_crash(finish,0,&p,o8)==0);
    /* Opponents' cues: distance/8 into the table, cooldown 8 (also gated), none from 64 on. */
    nm_race_sound_cooldowns_t cool={0,0};
    nm_player_t car; memset(&car,0,sizeof(car)); memset(&p,0,sizeof(p));
    car.boundary.ground.position[0]=30.0f; car.boundary.ground.position[2]=40.0f;  /* 50 -> 6 */
    EXPECT(nm_race_sound_opponent_cue(&cool.spin,nm_race_sound_opponent_spin,race,&car,&p)==0x9e061f && cool.spin==8);
    EXPECT(nm_race_sound_opponent_cue(&cool.spin,nm_race_sound_opponent_spin,race,&car,&p)==0);
    EXPECT(nm_race_sound_opponent_cue(&cool.crash,nm_race_sound_opponent_crash,finish,&car,&p)==0 && cool.crash==8);
    for(int i=0;i<8;++i) nm_race_sound_cooldowns_tick(&cool);
    EXPECT(!cool.spin && !cool.crash);
    car.boundary.ground.position[0]=64.0f; car.boundary.ground.position[2]=0.0f;
    EXPECT(nm_race_sound_opponent_cue(&cool.crash,nm_race_sound_opponent_crash,race,&car,&p)==0 && !cool.crash);
    car.boundary.ground.position[0]=3.9f;                                          /* rounds to 4 -> 0 */
    EXPECT(nm_race_sound_opponent_cue(&cool.crash,nm_race_sound_opponent_crash,race,&car,&p)==0x9e0a4f);
    /* Ambient (course 0): every fourth frame, on profiles 0x77..0x79; 100 from the source is the quietest. */
    uint8_t last[2]={0,0};
    memset(&p,0,sizeof(p)); p.phase[0]=0x78;
    p.boundary.ground.position[0]=65.13f+100.0f; p.boundary.ground.position[2]=144.0f;
    EXPECT(nm_race_sound_ambient(last,race,1,&p,o8)==0);
    EXPECT(nm_race_sound_ambient(last,race,4,&p,o8)==2 && o8[0]==0xbe3501 && o8[1]==0xbe3401);
    p.boundary.ground.position[0]=65.13f+100.0f+22.5f;                             /* 0.8*22.5 = 18 -> 127 */
    EXPECT(nm_race_sound_ambient(last,race,8,&p,o8)==2 && o8[0]==0xbe357f && o8[1]==0xbe347f);
    p.phase[0]=0x7a; EXPECT(nm_race_sound_ambient(last,finish,12,&p,o8)==0 && last[0]==0 && last[1]==0);
    if(failures) return 1;
    puts("PASS: race sound commands (engine rpm, slip, countdown, lap award, skid, spin, front smoke, wall lane, time-out beeps, crashes, opponents' cues, ambient) as the original's routines");
    return 0;
}
