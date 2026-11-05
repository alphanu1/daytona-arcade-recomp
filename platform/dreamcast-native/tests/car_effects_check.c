/* Host check of the player's car-effect rules (include/car_effects.h).
 *
 * usage: car_effects_check                 the rules on chosen states (exit 0 on success)
 *        car_effects_check DRIVE.dnd       then, per stdin line of hex words
 *                                          "+91 +92 +95 +96 +98 +99 +d6 +ed +34 +180 view"
 *                                          (bytes and halfword as the car holds them, +34/+180 as float bits, view
 *                                          preset 501710), one line: the draws nm_car_effects gives, each
 *                                          "call:kind:index:side:m0.m1...m11" (local matrix T [Rx Ry] S as float
 *                                          bits, nm_car_effect_matrix on the identity frame), separated by spaces;
 *                                          "-" for none.
 *        car_effects_check DRIVE.dnd --spin then, per stdin line of hex words, one step of the spin smoke
 *                                          (nm_spin_smoke_step) from the task state at 9f28 entry:
 *                                          "48 particle floats (bits), 8 lives, ring, 12 angles, +d4 +d6 +58 +2c,
 *                                          x y z +9c +a4 (bits), player x z (bits), view"; one line: the state after
 *                                          it, "48 particle floats, 8 lives, ring, 12 angles, drawn mask, counter".
 * tests/test_effects_render.py feeds it the player bytes of every 9088 call and the task of every 9f28 call of the
 * original's effects logs. */
#undef NDEBUG
#include "car_effects.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static float word(uint32_t bits) { float f;memcpy(&f,&bits,4);return f; }
static uint32_t bits(float value) { uint32_t out;memcpy(&out,&value,4);return out; }

static void rules(void) {
    nm_player_t car;nm_car_effect_t fx[NM_CAR_EFFECTS_MAX];
    memset(&car,0,sizeof(car));
    assert(nm_car_effects(&car,2,fx)==0);
    /* Dust: both rear wheels, table index t & 31, scale 0.5 + speed/4. */
    car.surface_ticks[0]=31;car.surface_ticks[1]=1;car.boundary.speed=2.0f;
    assert(nm_car_effects(&car,2,fx)==2);
    assert(fx[0].call==NM_FX_CALL_DUST_LEFT && fx[0].index==31 && fx[0].offset[0]==-0.7f && fx[0].scale[1]==1.0f);
    assert(fx[1].call==NM_FX_CALL_DUST_RIGHT && fx[1].index==1 && fx[1].offset[0]==0.7f && fx[1].offset[2]==-1.75f);
    assert(nm_fx_model(NM_FX_DUST,31)==0x8c6a14 && nm_fx_model(NM_FX_DUST,0)==0);
    /* Smoke: translucent, after the dust; its height from the rear grip. */
    car.force.trigger_count[1]=23;car.force.grip[1]=0.25f;
    assert(nm_car_effects(&car,2,fx)==3 && fx[2].call==NM_FX_CALL_SMOKE_SPIN && fx[2].translucent && fx[2].scale[1]==1.05f);
    assert(!fx[0].translucent && !fx[1].translucent);
    memset(&car,0,sizeof(car));
    /* Sparks: side 0 left, 3 right, view 2 T(-+1.3, 0.2, 3.4); r + 8 & 31; both timers: the +98 spark twice. */
    car.boundary.condition=16;car.boundary.damage.side=3;
    assert(nm_car_effects(&car,2,fx)==1 && fx[0].index==24 && fx[0].offset[0]==1.3f && fx[0].offset[2]==3.4f);
    car.response_ticks=1;car.boundary.damage.side=0;
    assert(nm_car_effects(&car,3,fx)==2 && fx[0].index==9 && fx[1].index==9 && fx[0].offset[0]==-1.0f);
    assert(nm_car_effects(&car,0,fx)==0);                           /* driver's view */
    assert(nm_car_effects(&car,1,fx)==2 && fx[0].rotated && fx[0].yaw==0x1555 && fx[0].scale[0]==0.5f);
    car.boundary.damage.side=1;assert(nm_car_effects(&car,2,fx)==0); /* sides 1, 2: no sparks */
    car.boundary.damage.side=0;car.inhibit_angle=0x1001;assert(nm_car_effects(&car,2,fx)==0); /* 9148's clear */
    car.inhibit_angle=-0x1000;assert(nm_car_effects(&car,2,fx)==2);
    /* Every index the rules can reach is exported. */
    for(int t=1;t<=31;++t) assert(nm_fx_model(NM_FX_DUST,(unsigned)t) && nm_fx_model(NM_FX_SMOKE,(unsigned)t));
    for(int r=1;r<=16;++r) assert(nm_fx_model(NM_FX_SPARK,(unsigned)((r+8)&31)));
}

/* The spin smoke (task 9e2c) on chosen states: emission pairs every 8 ticks of the timer beyond +-10, lives 26/13,
 * growth, sinking and drift, the counter rule of the player's circle, the angles. */
static void spin_rules(const nm_drive_params_t *p) {
    nm_spin_smoke_t s;nm_spin_smoke_reset(&s);
    assert(s.angles[0]==0x200 && s.angles[1]==0xac00 && s.angles[2]==0x4200 && s.angles[10]==0xc200 && !s.ring);
    nm_spin_smoke_source_t c;memset(&c,0,sizeof(c));
    c.timer=-112;c.delta[0]=0.5f;c.delta[1]=-1.0f;c.position[0]=100.0f;c.position[2]=50.0f;
    assert(nm_spin_smoke_step(p,&s,&c,0.0f,0.0f,2));
    assert(s.life[0]==25 && s.life[1]==12 && s.ring==2 && s.drawn==3 && s.emitted==2 && s.angles[0]==1024);
    /* behind (z -2.7) and in front (+2.7) at height 0.33, moved by half the displacement once, sunk 0.08 */
    assert(fabsf(s.particle[0][0]-100.25f)<1e-4f && fabsf(s.particle[0][2]-(50.0f-2.7f-0.5f))<1e-4f);
    assert(fabsf(s.particle[1][2]-(50.0f+2.7f-0.5f))<1e-4f && fabsf(s.particle[0][1]-(0.33f-0.08f))<1e-6f);
    assert(s.particle[0][3]==0.25f && s.particle[0][4]==-0.5f && s.particle[0][5]==0.4f+0.2f);
    c.timer=-111;assert(nm_spin_smoke_step(p,&s,&c,0.0f,0.0f,2) && s.ring==2 && s.life[0]==24); /* no emission */
    c.timer=-8;assert(nm_spin_smoke_step(p,&s,&c,0.0f,0.0f,2) && s.ring==2);                  /* within +-10 */
    c.timer=16;assert(nm_spin_smoke_step(p,&s,&c,0.0f,0.0f,2) && s.ring==4 && s.life[2]==25 && s.drawn==15);
    /* Inside the circle of radius 6.5 around the player: the first two in slot order hidden, later ones drawn. */
    /* (the timer is still 16: a third pair, slots 4 and 5; six particles inside) */
    assert(nm_spin_smoke_step(p,&s,&c,100.0f,50.0f,2) && s.ring==6 && s.drawn==0x3c && s.counter==UINT16_MAX-4u);
    /* Lives run out: 13 ticks for the front one, 26 for the rear one; life 0 is drawn once. */
    c.timer=1;
    for(unsigned k=0;k<30;++k) assert(nm_spin_smoke_step(p,&s,&c,0.0f,0.0f,2));
    assert(!s.drawn && !s.life[0] && !s.life[1] && !s.life[2] && !s.life[3]);
    /* Phases: the task runs in 20..24 only and draws nothing outside them. */
    nm_player_t car;memset(&car,0,sizeof(car));car.input_inhibit=-112;
    nm_spin_smoke_reset(&s);
    assert(nm_spin_smoke_tick(p,&s,19,&car,&car,2) && !s.ticks);
    assert(nm_spin_smoke_tick(p,&s,22,&car,&car,2) && s.ticks==1 && s.life[0]==25);
    assert(nm_spin_smoke_tick(p,&s,25,&car,&car,2) && s.ticks==1 && !s.drawn && s.life[0]==25);
    float m[12];
    static const float identity[12]={1,0,0, 0,1,0, 0,0,1, 0,0,0};
    assert(nm_spin_smoke_matrix(p,identity,&s,0,m) && fabsf(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]-0.6f*0.6f)<1e-5f &&
           m[9]==s.particle[0][0] && m[11]==s.particle[0][2]);
}

static int spin_lines(const nm_drive_params_t *p) {
    char line[2048];unsigned rows=0;
    while(fgets(line,sizeof(line),stdin)) {
        uint32_t w[48+8+1+12+4+5+2+1];unsigned n=0;
        for(char *at=line,*end;n<sizeof(w)/sizeof(w[0]);at=end,++n) {
            w[n]=(uint32_t)strtoul(at,&end,16);
            if(end==at) break;
        }
        if(n!=sizeof(w)/sizeof(w[0])) { fputs("bad spin line\n",stderr);return 2; }
        nm_spin_smoke_t s;memset(&s,0,sizeof(s));
        for(unsigned i=0;i<48;++i) s.particle[i/6][i%6]=word(w[i]);
        for(unsigned i=0;i<8;++i) s.life[i]=(uint8_t)w[48+i];
        s.ring=(uint8_t)w[56];
        for(unsigned i=0;i<12;++i) s.angles[i]=(uint16_t)w[57+i];
        nm_spin_smoke_source_t c;
        c.timer=(int8_t)w[69];c.spin_angle=(int16_t)w[70];c.yaw=(int16_t)w[71];c.roll=(int16_t)w[72];
        for(unsigned i=0;i<3;++i) c.position[i]=word(w[73+i]);
        c.delta[0]=word(w[76]);c.delta[1]=word(w[77]);
        if(!nm_spin_smoke_step(p,&s,&c,word(w[78]),word(w[79]),w[80])) { fputs("spin step rejected\n",stderr);return 1; }
        for(unsigned i=0;i<48;++i) printf("%08" PRIx32 " ",bits(s.particle[i/6][i%6]));
        for(unsigned i=0;i<8;++i) printf("%x ",s.life[i]);
        printf("%x ",s.ring);
        for(unsigned i=0;i<12;++i) printf("%x ",s.angles[i]);
        printf("%x %x\n",s.drawn,s.counter);
        ++rows;
    }
    fprintf(stderr,"car_effects_check: %u spin ticks\n",rows);
    return 0;
}

int main(int argc,char **argv) {
    rules();
    if(argc==1) { puts("car_effects_check: rules PASS");return 0; }
    if(argc!=2 && !(argc==3 && !strcmp(argv[2],"--spin"))) { fputs("usage: car_effects_check [DRIVE.dnd [--spin]] < states\n",stderr);return 2; }
    const nm_drive_params_t *p=nm_drive_load(argv[1]);
    if(!p) { fputs("cannot load drive.dnd\n",stderr);return 2; }
    spin_rules(p);
    if(argc==3) return spin_lines(p);
    static const float identity[12]={1,0,0, 0,1,0, 0,0,1, 0,0,0};
    char line[512];unsigned rows=0;
    while(fgets(line,sizeof(line),stdin)) {
        unsigned v[8],view;uint32_t speed,grip;
        if(sscanf(line,"%x %x %x %x %x %x %x %x %" SCNx32 " %" SCNx32 " %x",v,v+1,v+2,v+3,v+4,v+5,v+6,v+7,
                  &speed,&grip,&view)!=11) { fputs("bad state line\n",stderr);return 2; }
        nm_player_t car;memset(&car,0,sizeof(car));
        car.surface_ticks[0]=(int8_t)v[0];car.surface_ticks[1]=(int8_t)v[1];
        car.force.trigger_count[0]=(int8_t)v[2];car.force.trigger_count[1]=(int8_t)v[3];
        car.response_ticks=(int8_t)v[4];car.boundary.condition=(int8_t)v[5];
        car.inhibit_angle=(int16_t)v[6];car.boundary.damage.side=(uint8_t)v[7];
        car.boundary.speed=word(speed);car.force.grip[1]=word(grip);
        nm_car_effect_t fx[NM_CAR_EFFECTS_MAX];
        const unsigned n=nm_car_effects(&car,view,fx);
        if(!n) fputs("-",stdout);
        for(unsigned i=0;i<n;++i) {
            float m[12];
            if(!nm_car_effect_matrix(p,identity,fx+i,m)) { fputs("effect matrix rejected\n",stderr);return 1; }
            printf("%s%u:%u:%u:%u:",i ? " " : "",fx[i].call,fx[i].kind,fx[i].index,fx[i].side);
            for(unsigned k=0;k<12;++k) printf("%s%08" PRIx32,k ? "." : "",bits(m[k]));
        }
        putchar('\n');
        ++rows;
    }
    fprintf(stderr,"car_effects_check: %u states\n",rows);
    return 0;
}
