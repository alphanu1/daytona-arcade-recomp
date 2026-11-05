#ifndef DAYTONA_NATIVE_TUNNEL_LIGHT_H
#define DAYTONA_NATIVE_TUNNEL_LIGHT_H
#include <math.h>
#include <stdint.h>
#ifdef __DREAMCAST__
#include <sh4zam/shz_scalar.h>
#endif

/* The scene light of the race (7024..72fc, called from the camera object's updater 6510 at 6850, after the view):
 * the world direction at camera object +60..+68 that the original lights cars and scenery with (5021bc is its
 * copy in view space). Without path flag 256 it is the normal-race light 235db8 (+c..+14, lighting.h) at once
 * (72c4); with it, outside [first, end) the light glides back to the normal one, inside it is the light of the car's
 * tunnel range: a point light (mask bit set) gives the direction from the light to the car, normalised, else
 * none (0, 0, 0). The light glides there: every component moves by at most rate[speed index >> 6] a tick.
 * Tables by course: 232352[course] ranges, 23235a + course * 232356 (enter and exit profiles of each range,
 * the tunnel's first and end profile, the point-light mask, the light positions); rates 2323aa. Courses 0, 2
 * and 3 share one table (20 ranges, profiles 175..232), Expert has none. TGP ops: 0 add, 1 subtract (first
 * minus second), 12 normalise; float here (presentation: no gameplay value reads the light). */
typedef struct {
    const uint16_t *enter, *exit;   /* +70, +74: by range index, count + 1 profiles each */
    uint16_t first, end;            /* +78: the tunnel's profiles [first, end) */
    uint32_t mask;                  /* +80 -> word: bit k, range k has a point light */
    const float (*lights)[3];       /* +7c: by range index */
    uint8_t count;                  /* 232352[course]: +85 */
    const float *rates;             /* 2323aa: 8 negative steps by speed index >> 6 */
    const float *normal;            /* 235db8 + c */
} nm_tunnel_lights_t;

typedef struct {
    float light[3];   /* camera object +60..+68 */
    uint8_t index;    /* +84 */
} nm_tunnel_light_t;

/* 72c4 at the race entry: the normal light, range index 0 (64b8). */
static inline void nm_tunnel_light_reset(nm_tunnel_light_t *s, const float normal[3]) {
    s->light[0]=normal[0];s->light[1]=normal[1];s->light[2]=normal[2];s->index=0;
}

/* One tick: 7030 (the range index from the car's profile, one step a tick; backwards while the car faces the
 * wrong way, flags bit 28) and 709c (the target and the glide). position: the car's x, y, z; profile: +128;
 * flags: word 0; path_flags: +138; speed_index: +18. 0: invalid. */
static inline int nm_tunnel_light_step(nm_tunnel_light_t *s, const nm_tunnel_lights_t *t, const float position[3],
        uint16_t profile, uint32_t flags, uint16_t path_flags, uint32_t speed_index) {
    if(!s || !t || !t->rates || !t->normal || (t->count && (!t->enter || !t->exit || !t->lights)) || t->count>32 ||
       (speed_index>>6)>=8) return 0;
    unsigned index=s->index;
    if(index>t->count) return 0;
    if(flags&UINT32_C(0x10000000)) {
        /* 7040, 7080: backwards: profile 0 takes the last range; else one range back while its exit profile is
         * not below the car's. */
        if(!profile) index=t->count;
        else if(index && t->exit[index-1u]>=profile) --index;
    } else {
        /* 7050, 7060: profile 0 takes range 0; else one range on while the next one's enter profile is not above
         * the car's (the tables hold count + 1 profiles). */
        if(!profile) index=0;
        else if(index!=t->count && t->enter[index+1u]<=profile) ++index;
    }
    s->index=(uint8_t)index;
    /* 70a0: without path flag 256, 72c4: the normal light at once (no glide). */
    if(!(path_flags&256u)) {
        s->light[0]=t->normal[0];s->light[1]=t->normal[1];s->light[2]=t->normal[2];
        return 1;
    }
    float target[3];
    if(profile<t->first || profile>=t->end) {
        target[0]=t->normal[0];target[1]=t->normal[1];target[2]=t->normal[2];
    } else if(index<32 && (t->mask>>index&1u)) {
        if(index>=t->count) return 0;
        float d[3]={position[0]-t->lights[index][0],position[1]-t->lights[index][1],position[2]-t->lights[index][2]};
        const float squared=d[0]*d[0]+d[1]*d[1]+d[2]*d[2];
        if(!(squared>0.0f)) return 0;
#ifdef __DREAMCAST__
        const float inverse=shz_inv_sqrtf_fsrra(squared); /* FSRRA: a light direction, presentation only (the host keeps sqrtf) */
#else
        const float inverse=1.0f/sqrtf(squared);
#endif
        target[0]=d[0]*inverse;target[1]=d[1]*inverse;target[2]=d[2]*inverse;
    } else target[0]=target[1]=target[2]=0.0f;
    const float low=t->rates[speed_index>>6],high=-low;
    for(unsigned k=0;k<3;++k) {
        float delta=target[k]-s->light[k];
        if(delta<low) delta=low;
        else if(delta>high) delta=high;
        s->light[k]=delta+s->light[k];
    }
    return 1;
}
#endif
