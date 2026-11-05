#ifndef DAYTONA_NATIVE_INTRO_H
#define DAYTONA_NATIVE_INTRO_H
#include <stdint.h>

typedef struct { uint32_t tick, key; } nm_intro_cursor_t;
typedef struct { uint32_t key, elapsed, duration; } nm_intro_interval_t;

/* Exhaustive source-budget bound: at most44 bodies, plus wheel/window parts
 * for levels3/4 under strict2000 cost, plus one authored special extra part.
 * Plus shadows and detail1/2 crash attachments. Exhaustive source quota
 * check gives98+1: low crash attachments, normal high shadows, <=3 crash shadows.
 * Generated stages reject more than one special actor. */
enum { NM_INTRO_MAX_PARTS=99 };

/* 7acc/7da4/8060: normal shadows are unlimited; crash shadows share three
 * slots per frame in source draw order. Raw -0 also takes the crash branch. */
static inline int nm_intro_shadow_select(unsigned level,uint32_t raw_height,unsigned *crashes) {
    if(!crashes || level<2 || level>4) return 0;
    if(!raw_height) return 1;
    if(*crashes>=3) return 0;
    ++*crashes;return 1;
}

/* 7ab4/7d8c: authored combined-wheel mesh phase. Lower body detail levels
 * have no separate animated wheel submission. Integer bit selection only. */
static inline int nm_intro_wheel_phase(unsigned level,uint16_t phase) {
    return level==4 ? (int)((phase>>11)&7) : level==3 ? 8+(int)((phase>>11)&7) : -1;
}

/* 8590..85f4 after source CVTRI: nonnegative rounded abs(X/Y), signed Z.
 * Cell exclusion and floating-point transformation/conversion are upstream. */
static inline unsigned nm_intro_detail_level(int32_t abs_x,int32_t abs_y,int32_t z) {
    if(abs_x<0 || abs_y<0 || z<0 || z>256 || z<=abs_x-5 || z<=abs_y-5) return 0;
    return 1u+(z<=100)+(z<=35)+(z<=20);
}

/* 85fc..862c: one backward adjacent-ordering pass, not a full sort.
 * Depths are indexed by car, retained when its scenery cell is excluded.
 * Caller checks new transformed depths fit the source's signed16 storage. */
static inline int nm_intro_detail_order(uint8_t *order,const int16_t *depth,unsigned count) {
    if(!order || !depth || count>44) return 0;
    uint32_t seen[2]={0};
    for(unsigned i=0;i<count;++i) {
        const unsigned id=order[i];
        if(id>=count || (seen[id>>5]&(1u<<(id&31)))) return 0;
        seen[id>>5]|=1u<<(id&31);
    }
    for(unsigned end=count;end>1;--end) {
        const unsigned i=end-2;
        if(depth[order[i]]>depth[order[i+1]]) {
            const uint8_t old=order[i];order[i]=order[i+1];order[i+1]=old;
        }
    }
    return 1;
}

/* 836c..847c: four body-detail quotas, low to high. Source cost must be
 * STRICTLY below 2000. Demotions join the next pass, not this pass.
 * Pure integer bookkeeping: no SH4ZAM primitive applies. */
static inline int nm_intro_detail_budget(uint8_t quotas[4]) {
    if(!quotas) return 0;
    unsigned count=0;int q[4],cost=-2000;
    const int weights[4]={16,161,253,341};
    for(unsigned i=0;i<4;++i) {
        q[i]=quotas[i];count+=quotas[i];cost+=q[i]*weights[i];
    }
    if(count>44) return 0;
    while(cost>=0) {
        int demoted[3]={0};
        for(;;) {
            int moved=0;
            for(int level=3;level>0 && cost>=0;--level) if(q[level]) {
                --q[level];++demoted[level-1];moved=1;
                cost-=weights[level]-weights[level-1];
            }
            if(cost<0 || !moved) break;
        }
        for(unsigned i=0;i<3;++i) q[i]+=demoted[i];
    }
    for(unsigned i=0;i<4;++i) quotas[i]=(uint8_t)q[i];
    return 1;
}

/* 7668..77ac selection in caller-supplied source order. Zero is invisible;
 * levels1..4 map to body choices5..2. Sorting/visibility are separate. */
static inline int nm_intro_detail_assign(const uint8_t *requested,unsigned count,uint8_t *selected) {
    if(!requested || !selected || count>44) return 0;
    uint8_t quotas[4]={0};
    for(unsigned i=0;i<count;++i) {
        if(requested[i]>4) return 0;
        if(requested[i]) ++quotas[requested[i]-1];
    }
    if(!nm_intro_detail_budget(quotas)) return 0;
    for(unsigned i=0;i<count;++i) {
        unsigned level=requested[i];
        while(level>1 && !quotas[level-1]) --level;
        selected[i]=(uint8_t)level;
        if(level) --quotas[level-1];
    }
    return 1;
}

/* 5cb8..5d50 timeline for a zero-origin, self-looping camera sequence without
 * event callbacks. Internal zero-time keys start a new shot immediately.
 * Authored key times, not recorded poses.
 * The final key supplies the last interval's endpoint; reaching it wraps to
 * time0 in the SAME tick. Interpolation/anchor resolution are separate. */
static inline int nm_intro_camera_step(const uint32_t *times, uint32_t count,
        nm_intro_cursor_t *cursor, nm_intro_interval_t *out) {
    if (!times || !cursor || !out || count<2 || count>2048 || times[0] ||
        cursor->key>=count-1 || cursor->tick<times[cursor->key]) return 0;
    nm_intro_cursor_t next=*cursor;
    for (unsigned advances=0;advances<count*2;++advances) {
        const uint32_t begin=times[next.key], end=times[next.key+1];
        if (!end) {
            if(next.key+1==count-1) return 0;
            ++next.key;next.tick=0;continue;
        }
        if (end<=begin || end>INT32_MAX) return 0;
        if (next.tick<end) {
            const nm_intro_interval_t interval={next.key,next.tick-begin,end-begin};
            ++next.tick;
            *cursor=next; *out=interval;
            return 1;
        }
        if (++next.key==count-1) next=(nm_intro_cursor_t){0,0};
    }
    return 0;
}
#endif
