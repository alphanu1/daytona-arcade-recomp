/* The slot machine step (include/slot_machine.h, original 220534..2209e0) on
 * hand-built states: visibility gate, spin rates, wrap, press-tick rounding,
 * both payouts, the stop-at-64 quirk, the sound gate, the fourth press and
 * the u32 add. The race lockstep (test_slot_lockstep.py) covers real runs. */
#undef NDEBUG
#include "slot_machine.h"
#include <stdio.h>

static unsigned failures;
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d: %s\n",__LINE__,#c);++failures; } } while(0)
#define PHASE22 (1u<<22)

static nm_slot_machine_t make(int r0,int r1,int r2,int state) {
    return (nm_slot_machine_t){{(int16_t)r0,(int16_t)r1,(int16_t)r2},(int16_t)state,0};
}
static int reels(const nm_slot_machine_t *s,int r0,int r1,int r2) {
    return s->reel[0]==r0 && s->reel[1]==r1 && s->reel[2]==r2;
}

int main(void) {
    uint32_t remaining=3200;
    /* 1. 220550: an undrawn cell skips everything, the edge is lost. */
    nm_slot_machine_t s=make(0,0,0,0);
    CHECK(nm_slot_machine_tick(&s,0,1,1,PHASE22,&remaining)==0);
    CHECK(reels(&s,0,0,0) && s.state==0 && remaining==3200);
    /* 2. Spin from the actor's zero init: reel1 on odd counters only. */
    static const int16_t expect[4][3]={{0,1,2},{1,2,4},{1,3,6},{2,4,8}};
    for(uint32_t counter=0;counter<4;++counter) {
        CHECK(nm_slot_machine_tick(&s,1,counter,0,PHASE22,&remaining)==0);
        CHECK(reels(&s,expect[counter][0],expect[counter][1],expect[counter][2]));
    }
    /* 3. Wrap: 64 -> 0 -> +step; the spin never stores 0. */
    s=make(64,64,64,0);
    nm_slot_machine_tick(&s,1,0,0,PHASE22,&remaining);CHECK(reels(&s,64,1,2));
    nm_slot_machine_tick(&s,1,1,0,PHASE22,&remaining);CHECK(reels(&s,1,2,4));
    /* 4. Press tick: reel1 is rounded from its previous value, no spin step. */
    s=make(39,10,20,0);
    nm_slot_machine_tick(&s,1,1,1,PHASE22,&remaining);
    CHECK(s.state==1 && reels(&s,32,11,22));
    /* 5. "7 7 7": +448 (22097c) and ae2003, once per scene. */
    s=make(0,0,6,2);remaining=3200;
    CHECK(nm_slot_machine_tick(&s,1,0,1,PHASE22,&remaining)==0xae2003u);
    CHECK(s.state==3 && reels(&s,0,0,0) && remaining==3648 && s.awarded==1);
    CHECK(nm_slot_machine_tick(&s,1,1,0,PHASE22,&remaining)==0 && remaining==3648);
    /* 6. "BAR BAR BAR": +320 (2209b0) and ae2002. */
    s=make(32,32,38,2);remaining=3200;
    CHECK(nm_slot_machine_tick(&s,1,0,1,PHASE22,&remaining)==0xae2002u);
    CHECK(reels(&s,32,32,32) && remaining==3520);
    /* 7. 64 draws the "7" list but compares as 64: no pay (reference test C). */
    s=make(64,0,0,3);remaining=3200;
    CHECK(nm_slot_machine_tick(&s,1,0,0,PHASE22,&remaining)==0 && remaining==3200 && !s.awarded);
    /* 8. 19bdc gate: phase21 pays silently; phase20 queues the command. */
    s=make(0,0,0,3);remaining=0;
    CHECK(nm_slot_machine_tick(&s,1,0,0,1u<<21,&remaining)==0 && remaining==448 && s.awarded==1);
    s=make(0,0,0,3);
    CHECK(nm_slot_machine_tick(&s,1,0,0,1u<<20,&remaining)==0xae2003u && remaining==896);
    /* 9. Fourth press restarts from the rounded stops; no second payout. */
    s=make(0,0,0,3);s.awarded=1;remaining=100;
    nm_slot_machine_tick(&s,1,1,1,PHASE22,&remaining);
    CHECK(s.state==0 && reels(&s,1,1,2));
    s=make(0,0,6,2);s.awarded=1;
    CHECK(nm_slot_machine_tick(&s,1,0,1,PHASE22,&remaining)==0 && reels(&s,0,0,0) && remaining==100);
    /* 10. addi wraps as u32. */
    s=make(32,32,32,3);remaining=UINT32_C(0xFFFFFF00);
    nm_slot_machine_tick(&s,1,0,0,PHASE22,&remaining);
    CHECK(remaining==UINT32_C(0x40));
    /* Visibility bit: word4 bit23. */
    uint32_t bitmaps[16]={0};
    CHECK(!nm_slot_machine_visible(bitmaps));
    bitmaps[4]=1u<<23;CHECK(nm_slot_machine_visible(bitmaps));
    if(failures) { printf("FAIL: %u slot machine checks\n",failures);return 1; }
    puts("PASS: slot machine step (gate, spin, wrap, rounding, 7/BAR payouts, 64 quirk, sound gate, restart, u32)");
    return 0;
}
