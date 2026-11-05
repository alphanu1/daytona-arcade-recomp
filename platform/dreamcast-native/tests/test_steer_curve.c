/* The pad's steering table (include/steer_curve.h) through the arcade decoder (include/vehicle.h): the build's
 * curve and dead zone, then a sweep of others. Monotonic, mirror-symmetric, centre in the dead zone and nowhere
 * else, no step at the dead-zone edge, exactly full lock at both ends of the stick. */
#undef NDEBUG
#include "steer_curve.h"
#include "vehicle.h"
#include <assert.h>
#include <stdio.h>

static int32_t steer_of(const uint8_t table[256], int joyx) {
    return nm_controls_decode(nm_steer_table_adc(table,(int8_t)joyx),56,56,0,0).steer;
}
static void check(double exponent, int deadzone) {
    uint8_t table[256];
    nm_steer_table_build(table,exponent,deadzone);
    /* Ends: the wheel's full lock on both sides, -128 like -127. */
    assert(table[0]==128-80 && table[1]==128-80 && table[255]==128+80);
    assert(steer_of(table,-128)==-127 && steer_of(table,-127)==-127 && steer_of(table,127)==127);
    /* Mirror about the centre, byte for byte, and in the decoded steer. */
    for(int x=0;x<=127;++x) {
        assert((int)table[128+x]-128==128-(int)table[128-x]);
        assert(steer_of(table,x)==-steer_of(table,-x));
    }
    /* Monotonic in the stick, both the ADC byte and the decoded steer. */
    for(int x=-128;x<127;++x) {
        assert(table[x+129]>=table[x+128]);
        assert(steer_of(table,x+1)>=steer_of(table,x));
    }
    /* The dead zone reads as the centre; the first count beyond it is the band's edge (wheel 0) or at most one
     * wheel count up: no step in the decoded steer at the dead-zone edge (a dead zone leaving under eight live
     * counts is degenerate and only has to stay monotonic). */
    for(int x=-deadzone;x<=deadzone;++x) assert(table[x+128]==128 && steer_of(table,x)==0);
    if(deadzone<=120) {
        assert(table[128+deadzone+1]>=128+5 && table[128+deadzone+1]<=128+6);
        assert(steer_of(table,deadzone+1)<=2);
    }
    /* Outside the dead zone the byte is never the centre: the decoder sees the band from its edge up. */
    for(int x=deadzone+1;x<=127;++x) assert(table[128+x]>=133 && table[128+x]<=208 && table[128-x]<=123);
    /* Steer values, an eye on the shape: the full ladder 0..127 is reached. */
    int seen[128]={0};
    for(int x=0;x<=127;++x) seen[steer_of(table,x)]=1;
    assert(seen[0] && seen[127]);
}
int main(void) {
    check(NM_STEER_CURVE,NM_STEER_DEADZONE);
    /* A linear table (exponent 1) spreads the wheel evenly over the live travel: the middle of the travel is
     * near half lock; a curve of 2 puts the middle at about a quarter. */
    uint8_t linear[256],square[256];
    nm_steer_table_build(linear,1.0,12);
    nm_steer_table_build(square,2.0,12);
    const int middle=12+(127-12)/2;
    assert(steer_of(linear,middle)>=60 && steer_of(linear,middle)<=68);
    assert(steer_of(square,middle)>=28 && steer_of(square,middle)<=36);
    assert(steer_of(square,30)<steer_of(linear,30)); /* finer near the centre */
    static const double exponents[]={1.0,1.5,2.0,2.5,3.0,4.0};
    static const int deadzones[]={0,1,5,12,15,20,64,126};
    for(unsigned e=0;e<sizeof exponents/sizeof *exponents;++e)
        for(unsigned d=0;d<sizeof deadzones/sizeof *deadzones;++d) check(exponents[e],deadzones[d]);
    /* Out-of-range arguments are clamped, never a table without full lock. */
    uint8_t odd[256];
    nm_steer_table_build(odd,0.0,-3); assert(odd[0]==48 && odd[255]==208 && odd[128]==128);
    nm_steer_table_build(odd,2.0,500); assert(odd[0]==48 && odd[255]==208 && odd[128]==128);
    printf("PASS: steering table: curve %g dead zone %d; monotonic, mirrored, full lock at both ends, "
           "no step at the dead-zone edge (steer at stick 32/64/96: %ld/%ld/%ld)\n",(double)NM_STEER_CURVE,
           (int)NM_STEER_DEADZONE,(long)steer_of(square,32),(long)steer_of(square,64),(long)steer_of(square,96));
    return 0;
}
