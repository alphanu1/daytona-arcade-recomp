#ifndef DAYTONA_NATIVE_COLLISION_RESPONSE_H
#define DAYTONA_NATIVE_COLLISION_RESPONSE_H
#include "player.h"
#include "collision.h"
#include <stdalign.h>
#include "nm_mem.h"
#ifdef __DREAMCAST__
#include <sh4zam/shz_mem.h>
#endif

/* Borrow the two existing car layouts without copying a whole scene/field. */
typedef struct {
    nm_player_t *car;
    uint8_t *side;
    int16_t *own_yaw, *other_yaw;
} nm_collision_car_view_t;

/*148dc/148f0: separation, reaction, damage, impulse in that order. Caller
 * supplies actual entity identities and record/global flags, never infers
 * them from drawing order. Shared damage state belongs to player0. This
 * executes ONE selected response; detection and live pair traversal are separate.
 * Stage all writes so rejected arithmetic cannot publish half a response. */
static inline int nm_collision_response(const nm_drive_params_t *math,const nm_road_pack_t *road,
        nm_player_t *player,uint8_t *crash_counter,const nm_collision_car_view_t view[2],
        const uint8_t entity_id[2],
        const nm_collision_contact_t *contact,unsigned branch,uint32_t record_flags,
        const nm_collision_damage_input_t *input) {
    if(!math || !road || !player || !crash_counter || !view || !entity_id || !contact || !input || branch>1) return 0;
    for(unsigned i=0;i<2;++i)
        if(!view[i].car || !view[i].side || !view[i].own_yaw || !view[i].other_yaw) return 0;
    const unsigned other=view[0].car==view[1].car ? 0 : 1;
    if(!other && (entity_id[0]!=entity_id[1] || view[0].side!=view[1].side ||
        view[0].own_yaw!=view[1].own_yaw || view[0].other_yaw!=view[1].other_yaw)) return 0;
    alignas(32) nm_player_t car[2];
    for(unsigned i=0;i<2;++i) {
#ifdef __DREAMCAST__
        shz_memcpy4(car+i,view[i].car,sizeof(car[i]));
#else
        car[i]=*view[i].car;
#endif
    }
    alignas(32) nm_collision_body_t position[2];
    for(unsigned i=0;i<2;++i) position[i]=(nm_collision_body_t){
        {car[i].boundary.ground.position[0],car[i].boundary.ground.position[2]},*view[i].side};
    if(!nm_collision_separate(math,contact->angle[branch],contact->depth[branch],contact->side[branch],
        position+(branch ? other : 0),position+(branch ? 0 : other))) return 0;
    alignas(32) nm_collision_reaction_t reaction[2];
    for(unsigned i=0;i<=other;++i) reaction[i]=(nm_collision_reaction_t){
        {position[i].position[0],position[i].position[1]},car[i].boundary.heading,
        car[i].lateral.current,(uint16_t)car[i].phase[0],car[i].correction_ticks,
        car[i].boundary.damage.initial_inhibit};
    int16_t relative;
    if(!nm_collision_react(math,road,reaction,reaction+other,&relative)) return 0;
    alignas(32) nm_collision_damage_body_t damage[2];
    for(unsigned i=0;i<=other;++i) {
        car[i].boundary.damage.initial_inhibit=reaction[i].initial_inhibit;
        damage[i]=(nm_collision_damage_body_t){car[i].boundary.damage,
            {car[i].delta[0],car[i].delta[1]},car[i].input_inhibit,entity_id[i],position[i].side};
        damage[i].damage.shake_ticks=player->boundary.damage.shake_ticks;
        damage[i].damage.sequence_counter=*crash_counter;
    }
    int32_t severity;
    if(!nm_collision_damage(math,input,damage,damage+other,&severity)) return 0;
    alignas(32) nm_collision_impulse_body_t impulse[2];
    for(unsigned i=0;i<=other;++i) impulse[i]=(nm_collision_impulse_body_t){
        car[i].boundary.speed,{car[i].delta[0],car[i].delta[1]},
        {car[i].correction[0],car[i].correction[1]},car[i].boundary.ground.yaw,
        *view[i].own_yaw,*view[i].other_yaw,damage[i].damage.inhibit_ticks,
        car[i].input_inhibit,car[i].response_ticks,entity_id[i],position[i].side};
    nm_collision_impulse_shared_t shared={(uint16_t)player->boundary.damage.signal,damage[other].damage.shake_ticks};
    const nm_collision_impulse_input_t force={record_flags,contact->flags,severity};
    if(!nm_collision_impulse(math,&force,impulse,impulse+other,&shared)) return 0;
    const uint8_t sequence=damage[other].damage.sequence_counter;
    const uint8_t auxiliary=damage[other].damage.auxiliary_request;
    for(unsigned i=0;i<=other;++i) {
        car[i].boundary.ground.position[0]=position[i].position[0];
        car[i].boundary.ground.position[2]=position[i].position[1];
        car[i].lateral.current=reaction[i].lateral;car[i].correction_ticks=reaction[i].correction_ticks;
        /* Publish entity damage fields; shared fields are owned by player0. */
        damage[i].damage.shake_ticks=car[i].boundary.damage.shake_ticks;
        damage[i].damage.sequence_counter=car[i].boundary.damage.sequence_counter;
        car[i].boundary.damage=damage[i].damage;car[i].boundary.speed=impulse[i].speed;
        car[i].correction[0]=impulse[i].correction[0];car[i].correction[1]=impulse[i].correction[1];
        car[i].response_ticks=impulse[i].response_ticks;
    }
    for(unsigned i=0;i<=other;++i) {
#ifdef __DREAMCAST__
        shz_memcpy4(view[i].car,car+i,sizeof(car[i]));
#else
        *view[i].car=car[i];
#endif
        *view[i].side=position[i].side;
        *view[i].own_yaw=impulse[i].own_yaw;*view[i].other_yaw=impulse[i].other_yaw;
    }
    player->boundary.damage.shake_ticks=shared.shake_ticks;
    player->boundary.damage.signal=nm_yaw_angle(shared.signal);
    *crash_counter=sequence;
    if(auxiliary) player->auxiliary_flags|=UINT32_C(0x80000000);
    return 1;
}

/* Contact, cached corners, route and optional response for ONE live pair.
 * Failure stops the pass: successful detection is not rolled back if a later
 * reaction rejects input. All callers stop their scene on failure.
 * One function in the program (config/hot_layout.tsv places it): under LTO the inliner would otherwise copy its
 * 10 KB into the race's and the intro's collision passes. */
static __attribute__((noinline)) int nm_collision_pair(const nm_drive_params_t *math,const nm_road_pack_t *road,
        nm_player_t *player,uint8_t *crash_counter,const nm_collision_car_view_t view[2],
        const nm_collision_travel_t *const travel[2],const uint8_t entity_id[2],
        nm_collision_contact_t *contact,uint32_t record_flags,
        const nm_collision_damage_input_t *input,nm_collision_route_t *route) {
    if(!math || !road || !player || !crash_counter || !view || !travel ||
       !entity_id || !contact || !input || !route) return 0;
    for(unsigned i=0;i<2;++i)
        if(!view[i].car || !view[i].side || !view[i].own_yaw || !view[i].other_yaw || !travel[i]) return 0;
    const unsigned other=view[0].car==view[1].car ? 0 : 1;
    alignas(32) nm_collision_shape_t shape[2];
    for(unsigned i=0;i<=other;++i) {
        const nm_player_t *car=view[i].car;
        shape[i]=(nm_collision_shape_t){.flags=car->boundary.damage.flags,
            .heading=car->boundary.ground.yaw,.roll=car->boundary.ground.roll,
            .inhibit_ticks=car->boundary.damage.inhibit_ticks,.side=car->boundary.damage.side};
#ifdef __DREAMCAST__
        shz_memcpy4(shape[i].position,car->boundary.ground.position,sizeof(shape[i].position));
        shz_memcpy4(shape[i].world,car->boundary.ground.world,sizeof(shape[i].world));
#else
        nm_memcpy(shape[i].position,car->boundary.ground.position,sizeof(shape[i].position));
        nm_memcpy(shape[i].world,car->boundary.ground.world,sizeof(shape[i].world));
#endif
    }
    /*Contact writes501a02, NOT passing/impulse's501a04. */
    uint16_t contact_signal=(uint16_t)player->boundary.pivot_signal;
    if(!nm_collision_detect(math,shape,shape+other,view[1].car==player,contact,&contact_signal)) return 0;
    player->boundary.pivot_signal=nm_yaw_angle(contact_signal);
    alignas(32) nm_collision_choice_body_t choice[2];
    for(unsigned i=0;i<=other;++i) {
        nm_player_t *car=view[i].car;car->boundary.damage.flags=shape[i].flags;
        /*Detection owns ED; separation owns the distinct27D contact side. */
        car->boundary.damage.side=shape[i].side;
#ifdef __DREAMCAST__
        shz_memcpy4(car->boundary.ground.world,shape[i].world,sizeof(shape[i].world));
        shz_memcpy4(choice[i].travel,travel[i]->travel,sizeof(choice[i].travel));
        shz_memcpy4(choice[i].world,shape[i].world,sizeof(choice[i].world));
#else
        nm_memcpy(car->boundary.ground.world,shape[i].world,sizeof(shape[i].world));
        nm_memcpy(choice[i].travel,travel[i]->travel,sizeof(choice[i].travel));
        nm_memcpy(choice[i].world,shape[i].world,sizeof(choice[i].world));
#endif
    }
    const nm_collision_contact_t *c=contact;
    const nm_collision_route_input_t route_input={
        {{c->depth[0],c->depth[1]},{c->corner[0],c->corner[1]},c->angle[0]},
        c->flags,{shape[0].heading,shape[other].heading}};
    if(!nm_collision_route(math,&route_input,choice,choice+other,route)) return 0;
    return route->branch==NM_COLLISION_NONE ||
        nm_collision_response(math,road,player,crash_counter,view,entity_id,
            contact,route->branch,record_flags,input);
}
#endif
