/* The contact-timer clear of 9148 (include/contact_clear.h) on hand-set
 * players: the closed -0x1000..0x1000 window of the signed spin angle +d6,
 * either timer (+98, +99) arming it, negative timers counting as set, and
 * nothing else touched. The race lockstep (test_damage_lockstep.py, run C)
 * covers real runs. */
#undef NDEBUG
#include "contact_clear.h"
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d: %s\n",__LINE__,#c);++failures; } } while(0)

/* One call on a copy; returns the result and checks that only +98/+99 changed. */
static int run(int8_t r98,int8_t r99,int16_t angle,int8_t *out98,int8_t *out99) {
    static nm_player_t s;memset(&s,0x5a,sizeof(s));
    s.response_ticks=r98;s.boundary.condition=r99;s.inhibit_angle=angle;
    nm_player_t before=s;
    const int cleared=nm_player_contact_clear(&s);
    *out98=s.response_ticks;*out99=s.boundary.condition;
    before.response_ticks=s.response_ticks;before.boundary.condition=s.boundary.condition;
    CHECK(!memcmp(&before,&s,sizeof(s)));
    return cleared;
}

int main(void) {
    int8_t a,b;
    /* 1. Window edges of +d6 (9164 cmpibl -0x1000, 916c cmpibg 0x1000): +-0x1000 keep the timers. */
    static const struct { int16_t angle; int clears; } edge[]={
        {0,0},{0x1000,0},{-0x1000,0},{0x1001,1},{-0x1001,1},{0x7fff,1},{(int16_t)0x8000,1},{-8977,1},{-18620,1},{4095,0}};
    for(unsigned i=0;i<sizeof(edge)/sizeof(edge[0]);++i) {
        const int c=run(16,16,edge[i].angle,&a,&b);
        CHECK(c==edge[i].clears);
        CHECK(edge[i].clears ? (!a && !b) : (a==16 && b==16));
    }
    /* 2. Either timer arms it (90d4 / 90e0, 9148..9154) and both are cleared. */
    CHECK(run(16,0,0x2000,&a,&b)==1 && !a && !b);   /* car contact (14ca8) */
    CHECK(run(0,16,-0x2000,&a,&b)==1 && !a && !b);  /* wall contact (16748) */
    CHECK(run(9,3,0x4000,&a,&b)==1 && !a && !b);
    /* 3. Signed bytes: any nonzero value is set, including the wrapped -1 / -128. */
    CHECK(run(-1,0,0x2000,&a,&b)==1 && !a && !b);
    CHECK(run(0,-128,0x2000,&a,&b)==1 && !a && !b);
    /* 4. Both clear: 9148 is not called (and would return at 9154); the angle does not matter. */
    CHECK(run(0,0,0x7fff,&a,&b)==0 && !a && !b);
    CHECK(run(0,0,0,&a,&b)==0 && !a && !b);
    /* 5. Inside the window the timers stay as they are. */
    CHECK(run(-5,7,-0x800,&a,&b)==0 && a==-5 && b==7);
    if(failures) { printf("FAIL: %u\n",failures); return 1; }
    puts("native_contact_clear PASS");
    return 0;
}
