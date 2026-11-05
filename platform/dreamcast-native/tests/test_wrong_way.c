/* The wrong-way rule (include/wrong_way.h, original a818..a968) on hand-built
 * quads: the closed 0x4000..0xc000 window of the 16-bit heading difference,
 * the sign-extended halfword heading, the HUD object's latch (a910: only a
 * bit the rule set is cleared), the other flag bits and the rejected road.
 * The race lockstep (test_damage_lockstep.py) covers real runs. */
#undef NDEBUG
#include "wrong_way.h"
#include <stdio.h>

static unsigned failures;
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d: %s\n",__LINE__,#c);++failures; } } while(0)

int main(void) {
    nm_contact_quad_t quads[2]={{.heading=0},{.heading=UINT32_C(0x12345000)}};
    const nm_contact_pack_t pack={.quad_count=2,.quads=quads};
    uint8_t shown=0;uint32_t flags=UINT32_C(0x80000020);
    /* 1. Window edges: d = quad - heading, low 16 bits; 0x4000 and 0xc000 are inside. */
    static const struct { int16_t heading; int inside; } edge[]={
        {0,0},{-0x3fff,0},{-0x4000,1},{(int16_t)0x8000,1},{0x4000,1},{0x3fff,0},{-0x4001,1},{0x4001,1}};
    for(unsigned i=0;i<sizeof(edge)/sizeof(edge[0]);++i) {
        shown=0;flags=UINT32_C(0x80000020);
        CHECK(nm_wrong_way_tick(&pack,0,edge[i].heading,&shown,&flags));
        CHECK(shown==edge[i].inside && flags==(UINT32_C(0x80000020)|(edge[i].inside ? NM_WRONG_WAY_FLAG : 0)));
    }
    /* 2. Only the low 16 bits of the quad heading count (and 0xffff after subi). */
    shown=0;flags=0;
    CHECK(nm_wrong_way_tick(&pack,1,(int16_t)0x5000,&shown,&flags) && !shown && !flags);  /* d = 0 */
    CHECK(nm_wrong_way_tick(&pack,1,(int16_t)0xd000,&shown,&flags) && shown && flags==NM_WRONG_WAY_FLAG); /* d = 0x8000 */
    /* 3. Leaving the window clears both bits, once; other bits stay. */
    flags|=UINT32_C(0x02000000);
    CHECK(nm_wrong_way_tick(&pack,0,0,&shown,&flags) && !shown && flags==UINT32_C(0x02000000));
    /* 4. a910: a bit 28 the rule did not set (latch clear) is left alone outside the window. */
    flags=NM_WRONG_WAY_FLAG;shown=0;
    CHECK(nm_wrong_way_tick(&pack,0,0,&shown,&flags) && !shown && flags==NM_WRONG_WAY_FLAG);
    /* 5. Inside the window again and again: idempotent. */
    for(int i=0;i<3;++i) CHECK(nm_wrong_way_tick(&pack,0,(int16_t)0x8000,&shown,&flags) && shown && flags==NM_WRONG_WAY_FLAG);
    /* 6. A road outside the pack is rejected and publishes nothing. */
    shown=0;flags=0;
    CHECK(!nm_wrong_way_tick(&pack,2,(int16_t)0x8000,&shown,&flags) && !shown && !flags);
    CHECK(!nm_wrong_way_tick(NULL,0,0,&shown,&flags));
    if(failures) { printf("FAIL: %u wrong-way checks\n",failures);return 1; }
    puts("PASS: wrong-way rule (window edges, 16-bit difference, latch, other bits, rejected road)");
    return 0;
}
