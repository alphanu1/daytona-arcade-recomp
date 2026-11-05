#ifndef DAYTONA_NATIVE_CAR_DETAIL_H
#define DAYTONA_NATIVE_CAR_DETAIL_H
#include "intro_math.h"
#include "visibility.h"

/* Borrow individual native owners: intro players and race-car records have
 * different strides. Call once per simulation tick, not once per rendering.
 * Depth/order persist when cells disappear; no car state is copied or changed. */
static inline int nm_car_detail_step(const float view[12],const nm_scenery_cells_t *cells,
        const nm_player_t *const cars[],unsigned count,int16_t depth[44],
        uint8_t order[44],uint8_t level[44]) {
    if(!view || !cells || !cars || !depth || !order || !level || !count || count>44 ||
       cells->draw_count>25) return 0;
    for(unsigned i=0;i<count;++i) if(!cars[i]) return 0;
    uint8_t requested[44]={0},ordered[44],selected[44];
#ifdef __DREAMCAST__
    alignas(32) shz_mat3x4_t matrix;
    shz_memcpy4(matrix.elem,view,12*sizeof(*view));shz_xmtrx_load_3x4(&matrix);
#endif
    for(unsigned i=0;i<count;++i) {
        const float *p=cars[i]->boundary.ground.position;
        uint16_t car_cell;
        if(!nm_contact_scenery_cell(p[0],p[2],&car_cell)) return 0;
        unsigned visible=0;
        for(unsigned c=0;c<cells->draw_count;++c) visible|=cells->draw[c]==car_cell;
        if(!visible) continue; /* Source retains the previous depth key. */
        float point[3];
#ifdef __DREAMCAST__
        const shz_vec3_t v=shz_xmtrx_transform_point3(shz_vec3_init(p[0],p[1],p[2]));
        point[0]=shz_fabsf(v.x);point[1]=shz_fabsf(v.y);point[2]=v.z;
#else
        for(unsigned a=0;a<3;++a) point[a]=p[0]*view[a]+p[1]*view[3+a]+p[2]*view[6+a]+view[9+a];
        point[0]=fabsf(point[0]);point[1]=fabsf(point[1]);
#endif
        int32_t q[3];
        for(unsigned a=0;a<3;++a)
            if(!isfinite(point[a]) || !nm_drive_round_i32(point[a],q+a)) return 0;
        if(q[0]<0 || q[1]<0 || q[2]<INT16_MIN || q[2]>INT16_MAX) return 0;
        requested[i]=(uint8_t)nm_intro_detail_level(q[0],q[1],q[2]);
        depth[i]=(int16_t)q[2];
    }
    if(!nm_intro_detail_order(order,depth,count)) return 0;
    for(unsigned i=0;i<count;++i) ordered[i]=requested[order[i]];
    if(!nm_intro_detail_assign(ordered,count,selected)) return 0;
    for(unsigned i=0;i<count;++i) level[order[i]]=selected[i];
    return 1;
}

/* Race variant. 8480..84dc builds the detail list 503208 from the pool at
 * 501224 only (count 501084, entities with flag bit31): every car BUT the
 * player, whose entity 501220 is never listed, never budgeted and whose level
 * byte stays 0; the player draw 6c40/6dc0 submits its own fixed parts. Car 0
 * is that player: order[0]=0, level[0]=0 and depth[0] is left alone, while
 * order[1..]/depth[1..]/level[1..] carry the opponents' persistent state
 * (order values are car indices). Same arithmetic as nm_car_detail_step. */
static inline int nm_race_detail_step(const float view[12],const nm_scenery_cells_t *cells,
        const nm_player_t *const cars[],unsigned count,int16_t depth[44],
        uint8_t order[44],uint8_t level[44]) {
    if(!cars || !depth || !order || !level || count<2 || count>44 || order[0]) return 0;
    uint8_t listed[44];
    for(unsigned i=1;i<count;++i) {
        if(!order[i] || order[i]>=count) return 0;
        listed[i-1]=(uint8_t)(order[i]-1);
    }
    if(!nm_car_detail_step(view,cells,cars+1,count-1,depth+1,listed,level+1)) return 0;
    for(unsigned i=1;i<count;++i) order[i]=(uint8_t)(listed[i-1]+1);
    level[0]=0;
    return 1;
}

/* 7668..77cc tail of the same selection: shared byte5014f7, consumed by the NEXT
 * tick's opponent target (10de4) and road gate (15830). Under flags117800a8 with
 * hold501a84<=1 it is the number of listed cars (503208: every car but the
 * player) with a nonzero detail level; otherwise the constant32. 77b0..77c0
 * shifts it right by the byte 77d0[course 501460]: 2, 1, 2, 0 for slots 0..3. */
static inline int nm_car_detail_guided_mode(const uint8_t level[44],unsigned count,
        uint32_t flags,uint32_t hold,unsigned course,uint8_t *out) {
    static const uint8_t shift[4]={2,1,2,0};
    if(!level || !out || !count || count>44 || course>3) return 0;
    unsigned drawn=32;
    if((flags&UINT32_C(0x117800a8)) && (int32_t)hold<=1) {
        drawn=0;
        for(unsigned i=1;i<count;++i) drawn+=level[i]!=0;
    }
    *out=(uint8_t)(drawn>>shift[course]);
    return 1;
}
#endif
