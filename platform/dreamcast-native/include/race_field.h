#ifndef DAYTONA_NATIVE_RACE_FIELD_H
#define DAYTONA_NATIVE_RACE_FIELD_H
#include "nm_mem.h"
#include "intro_math.h"
#include "collision_response.h"
#include "race_intents.h"
#include "pit.h"
#ifdef __DREAMCAST__
#include <sh4zam/shz_mem.h>
#endif

/* Persistent native car storage. No guest pointers; links are spawn indices.
 * Alignment rounds each array stride, not merely the start of the array. */
typedef struct {
    alignas(32) nm_player_t car;
    nm_opponent_finish_state_t effects;
    float grid_speed, guided_scale;
    uint16_t next, previous;
    int16_t other_yaw;
    int16_t near_marker;
    uint8_t route, mode, remembered, rank;
    uint32_t relative; /*2a4: full word; opponent height gate consumes only low byte. */
    nm_collision_travel_t collision_motion;
    uint16_t collision_adjusted, collision_candidates;
    uint8_t collision_side;
} nm_race_car_t;

typedef struct {
    uint32_t flags, clock;
    uint32_t timer_hold; /*501a84: the pit's stop count (pit.h; 640 at the entry dc70, 1 at the release, 0 from
                          * the next normal tick d828), 1 from the first enabled finish-player call. */
    uint16_t tick;
    int16_t event_timer;
    uint8_t guided_mode, profile_updates, crash_counter, inhibit_owner, event;
    float heading_circle[3];
    uint32_t visible_cells[16];
    nm_pit_t pit; /* 50112c, 501128, 501a98, crew rows 68..73, rows 74..138 suspended (pit.h). */
    const nm_pit_data_t *pit_data; /* The race header's nm_race_pit (bound by nm_race_scene_start); NULL: no pit. */
    /* Diagnostic only, no original owner: the car whose movement update ran last
     * (nm_race_movement_step), so a rejected tick names its car. */
    uint8_t updating;
} nm_race_shared_t;

typedef struct {
    uint16_t rank[44], track[44];
    int8_t lap;
} nm_race_order_t;

typedef struct {
    nm_collision_contact_t contact;
    uint32_t pairs, responses;
} nm_race_collisions_t;

/* Normal12148's1904c/191d8 consumer pair, AFTER car interactions even when
 * those interactions were inhibited. Attract bypass and later19320 creation
 * remain caller-owned. Player fields own501a02/501a04; pending values inside
 * the persistent timer state are only temporary borrowed views. */
static inline int nm_race_consume_intents(nm_race_car_t *player,nm_race_intents_t *state,
        const nm_race_intent_row_t *rows,unsigned count,unsigned alternate,
        const uint32_t special[2],int32_t warmup,nm_race_intent_output_t *output) {
    if(!player || !state || !output) return 0;
    nm_race_intents_t next=*state;
    next.lane[0].pending=player->car.boundary.pivot_signal;
    next.lane[1].pending=player->car.boundary.damage.signal;
    const unsigned completed=player->car.lap.completed;
    const int8_t lap=(int8_t)(completed<128 ? (int)completed : (int)completed-256);
    if(!nm_race_intents_step(rows,count,alternate,special,warmup,lap,&next,output)) return 0;
    player->car.boundary.pivot_signal=next.lane[0].pending;
    player->car.boundary.damage.signal=next.lane[1].pending;
    *state=next;return 1;
}

/* Caller supplies validated linked neighbors; this view borrows native owners.
 * Do not sign-extend rank_key: source neighbor predicates read the preserved high half. */
static inline nm_race_announcement_input_t nm_race_announcement_view(
        const nm_race_car_t *player,const nm_race_car_t *next,const nm_race_car_t *next_next,
        uint32_t flags,uint32_t gate,uint32_t inhibit,uint8_t count) {
    const nm_race_announcement_input_t view={
        flags,gate,inhibit,
        (uint32_t)(uint16_t)player->car.rank_key | (uint32_t)player->car.rank_key_high<<16,
        (uint32_t)(uint16_t)next->car.rank_key | (uint32_t)next->car.rank_key_high<<16,
        (uint32_t)(uint16_t)next_next->car.rank_key | (uint32_t)next_next->car.rank_key_high<<16,
        player->car.speed_index,player->car.rpm,player->car.force.trigger_count[1],
        player->car.drag_state,(int8_t)(player->rank<128 ? (int)player->rank : (int)player->rank-256),count};
    return view;
}

/*19320 follows consumption. The pending word aliases BOTH owner signals;
 *repeats belongs to the consumer state, not a second persistent copy. */
static inline int nm_race_create_intent(nm_race_car_t *player,nm_race_intents_t *intents,
        uint32_t *cursor,const nm_race_announcement_row_t *rows,unsigned count,
        const uint32_t ranks[9],const nm_race_announcement_input_t *input) {
    if(!player || !intents || !cursor) return 0;
    nm_race_announcement_state_t next={*cursor,intents->repeats,
        (uint32_t)(uint16_t)player->car.boundary.pivot_signal |
        (uint32_t)(uint16_t)player->car.boundary.damage.signal<<16};
    if(!nm_race_announcement_step(rows,count,ranks,input,&next)) return 0;
    player->car.boundary.pivot_signal=nm_yaw_angle(next.pending);
    player->car.boundary.damage.signal=nm_yaw_angle(next.pending>>16);
    intents->repeats=next.repeats;*cursor=next.cursor;
    return 1;
}

/* Silent normal12148 tail AFTER interactions, including inhibited frames.
 * Attract bypasses all event state.19590's positional sound calculation stays
 * omitted. As with the preceding collision pass, failure stops the scene;
 * consumer effects are not rolled back if subsequent creation rejects input. */
static inline int nm_race_event_tail(nm_race_car_t *player,nm_race_intents_t *intents,
        uint32_t *cursor,const nm_race_intent_row_t *rows,unsigned count,unsigned alternate,
        const uint32_t special[2],int32_t warmup,const nm_race_announcement_row_t *announcements,
        unsigned announcement_count,const uint32_t ranks[9],const nm_race_announcement_input_t *input,
        nm_race_intent_output_t *output) {
    if(!player || !intents || !cursor || !input || !output) return 0;
    if(input->flags&UINT32_C(0x10000000)) { output->count=0;return 1; }
    return nm_race_consume_intents(player,intents,rows,count,alternate,special,warmup,output) &&
        nm_race_create_intent(player,intents,cursor,announcements,announcement_count,ranks,input);
}

/*13f08 live pass AFTER preparation. Compact probes are staged once; only the
 *two changed cars refresh them after each response. Failures stop gameplay,
 *not an atomic rollback of responses already completed in this pass. */
static inline int nm_race_collide(nm_race_car_t *cars,unsigned count,
        nm_race_shared_t *shared,const nm_drive_params_t *math,const nm_road_pack_t *road,
        const uint16_t order[14],unsigned phase,uint32_t record_flags,
        const nm_collision_damage_input_t *input,nm_race_collisions_t *state) {
    if(!cars || !shared || !math || !road || !order || !input || !state || count<3 || count>44 || phase>=4) return 0;
    alignas(32) nm_collision_walk_body_t bodies[44];
    for(unsigned i=0;i<count;++i) {
        const nm_race_car_t *c=cars+i;
        if(c->next>=count) return 0;
        bodies[i]=(nm_collision_walk_body_t){
            {{c->car.boundary.ground.position[0],c->car.boundary.ground.position[2]},
                c->car.boundary.damage.overlay_model},c->next,c->collision_candidates};
    }
    for(unsigned i=0;i<14;++i) if(order[i]>=count) return 0;
    nm_collision_walk_t walk={.centre={shared->heading_circle[0],shared->heading_circle[1]},
        .circle_radius=shared->heading_circle[2],.budget=1000};
    unsigned cost=0;
    for(;;) {
        uint16_t pair[2];
        const int found=nm_collision_walk_next(math,bodies,count,order,0,phase,cost,&walk,pair);
        if(found<0) return 0;
        shared->heading_circle[0]=walk.centre[0];shared->heading_circle[1]=walk.centre[1];
        shared->heading_circle[2]=walk.circle_radius;
        if(!found) return 1;
        nm_race_car_t *a=cars+pair[0],*b=cars+pair[1];
        const nm_collision_car_view_t view[2]={
            {&a->car,&a->collision_side,&a->effects.correction_yaw,&a->other_yaw},
            {&b->car,&b->collision_side,&b->effects.correction_yaw,&b->other_yaw}};
        const nm_collision_travel_t *travel[2]={&a->collision_motion,&b->collision_motion};
        const uint8_t ids[2]={(uint8_t)pair[0],(uint8_t)pair[1]};
        nm_collision_route_t route;
        if(!nm_collision_pair(math,road,&cars[0].car,&shared->crash_counter,
            view,travel,ids,&state->contact,record_flags,input,&route)) return 0;
        for(unsigned i=0;i<2;++i) {
            const nm_player_t *c=&cars[pair[i]].car;
            bodies[pair[i]].probe=(nm_collision_probe_t){
                {c->boundary.ground.position[0],c->boundary.ground.position[2]},c->boundary.damage.overlay_model};
        }
        cost=route.caller_result;++state->pairs;state->responses+=route.branch!=NM_COLLISION_NONE;
    }
}

/* One selected pair. The shared crash counter is authoritative, even when
 * neither participant is player0. Detection/traversal remain caller-owned. */
static inline int nm_race_collision_response(nm_race_car_t *cars,unsigned count,
        nm_race_shared_t *shared,const nm_drive_params_t *math,const nm_road_pack_t *road,
        unsigned first,unsigned second,const nm_collision_contact_t *contact,
        unsigned branch,uint32_t record_flags,const nm_collision_damage_input_t *input) {
    if(!cars || !shared || count>44 || first>=count || second>=count) return 0;
    const nm_collision_car_view_t view[2]={
        {&cars[first].car,&cars[first].collision_side,&cars[first].effects.correction_yaw,&cars[first].other_yaw},
        {&cars[second].car,&cars[second].collision_side,&cars[second].effects.correction_yaw,&cars[second].other_yaw}};
    const uint8_t ids[2]={(uint8_t)first,(uint8_t)second};
    return nm_collision_response(math,road,&cars[0].car,&shared->crash_counter,
        view,ids,contact,branch,record_flags,input);
}

/*13f08 preparation: preserve the original14-car window and prior travel.
 *The source reset pool excludes player0; stage a rotated compact view. */
static inline int nm_race_collision_prepare(nm_race_car_t *cars, unsigned count,
        uint32_t profile_wrap, uint16_t order[NM_COLLISION_WINDOW]) {
    if(!cars || !order || count<3 || count>44 || !profile_wrap || profile_wrap>4096) return 0;
    alignas(32) nm_collision_rank_t bodies[44];
    uint16_t selected[NM_COLLISION_WINDOW];
    for(unsigned i=0;i<count;++i) {
        const nm_race_car_t *c=cars+i;
        if(c->next>=count || c->previous>=count) return 0;
        bodies[i ? i-1 : count-1]=(nm_collision_rank_t){
            .flags=c->car.boundary.damage.flags,.relative=c->relative,
            .position={c->car.boundary.ground.position[0],c->car.boundary.ground.position[2]},
            .motion=c->collision_motion,
            .next=(uint16_t)(c->next ? c->next-1 : count-1),
            .previous=(uint16_t)(c->previous ? c->previous-1 : count-1),
            .profile=(uint16_t)c->car.phase[0],.adjusted=c->collision_adjusted,
            .candidates=c->collision_candidates,.contact_side=c->collision_side};
    }
    if(!nm_collision_prepare(bodies,count,count-1,count-1,profile_wrap,selected)) return 0;
    for(unsigned i=0;i<count;++i) {
        nm_race_car_t *c=cars+i;
        const nm_collision_rank_t *b=bodies+(i ? i-1 : count-1);
        c->car.boundary.damage.flags=b->flags;c->relative=b->relative;
        c->collision_motion=b->motion;c->collision_adjusted=b->adjusted;
        c->collision_candidates=b->candidates;c->collision_side=b->contact_side;
    }
    for(unsigned i=0;i<NM_COLLISION_WINDOW;++i)
        order[i]=(uint16_t)(selected[i]+1==count ? 0 : selected[i]+1);
    return 1;
}

/*12150 then125b0, after native traffic. Control, short timer and501a04 have
 *one authoritative owner; timer/passing helpers receive temporary views.
 *Signals remain numeric intents. No sound device or board is involved. */
static inline int nm_race_signals_step(nm_race_car_t *cars, unsigned count,
        nm_race_shared_t *shared, int32_t event_cooldowns[2],
        nm_traffic_pass_t *passing, const nm_drive_params_t *math) {
    if(!cars || !shared || !event_cooldowns || !passing || !math || count<3 || count>44) return 0;
    nm_race_timers_t timers={nm_gears_signed(cars[0].car.control),
        {event_cooldowns[0],event_cooldowns[1]},shared->event_timer};
    nm_traffic_pass_t next=*passing;
    next.signal=(uint16_t)cars[0].car.boundary.damage.signal;
    alignas(32) nm_traffic_pass_body_t bodies[44];
    for(unsigned i=0;i<count;++i) {
        const nm_race_car_t *c=cars+i;
        bodies[i]=(nm_traffic_pass_body_t){(uint32_t)c->car.speed_index,
            {c->car.boundary.ground.position[0],c->car.boundary.ground.position[2]},
            c->car.lateral.current,c->next,c->previous};
    }
    if(!nm_race_timers_step(&timers) || !nm_traffic_pass(math,bodies,count,0,&next)) return 0;
    cars[0].car.control=(uint32_t)timers.control;shared->event_timer=timers.short_ticks;
    cars[0].car.boundary.damage.signal=nm_yaw_angle(next.signal);
    event_cooldowns[0]=timers.event[0];event_cooldowns[1]=timers.event[1];
    *passing=next;return 1;
}

/*1224c car effects after field movement: one sorting pass, rebuild the ring,
 *then proximity flags/markers. The139d8 scheduler-object hold is not a car.
 *Stage only compact views, not a second copy of the entire car array. */
static inline int nm_race_traffic_step(nm_race_car_t *cars, unsigned count,
        nm_race_order_t *order, nm_race_shared_t *shared, const nm_drive_params_t *math) {
    if(!cars || !order || !shared || !math || count<3 || count>44) return 0;
    nm_race_order_t sorted=*order;
    alignas(32) nm_traffic_order_body_t bodies[44];
    alignas(32) nm_traffic_near_body_t near[44];
    for(unsigned i=0;i<count;++i) {
        const nm_race_car_t *c=cars+i;
        const unsigned lap=c->car.lap.lap;
        bodies[i]=(nm_traffic_order_body_t){c->car.rank_key,(uint16_t)c->car.phase[0],
            c->next,c->previous,(int8_t)(lap<128 ? (int)lap : (int)lap-256),c->rank};
    }
    if(!nm_traffic_order(bodies,count,sorted.rank,sorted.track,count,count,shared->flags,&sorted.lap)) return 0;
    for(unsigned i=0;i<count;++i) {
        const nm_race_car_t *c=cars+i;
        const unsigned next=bodies[i].next,previous=bodies[i].previous;
        if(next>=count || previous>=count) return 0;
        near[i ? i-1 : count-1]=(nm_traffic_near_body_t){
            .flags=c->car.boundary.damage.flags,
            .marker146=(uint16_t)c->near_marker|((c->car.gear&65535u)<<16),
            .position={c->car.boundary.ground.position[0],c->car.boundary.ground.position[2]},
            .next=(uint16_t)(next ? next-1 : count-1),
            .previous=(uint16_t)(previous ? previous-1 : count-1)};
    }
    uint8_t updates=shared->profile_updates;
    if(!nm_traffic_near_neighbors(math,near,count,count-1,count-1,&updates)) return 0;
    for(unsigned i=0;i<count;++i) {
        nm_race_car_t *c=cars+i;
        const nm_traffic_near_body_t *n=near+(i ? i-1 : count-1);
        c->next=bodies[i].next;c->previous=bodies[i].previous;c->rank=bodies[i].rank;
        c->car.boundary.damage.flags=n->flags;
        c->near_marker=nm_yaw_angle(n->marker146);
        c->car.gear=(c->car.gear&UINT32_C(0xffff0000))|(n->marker146>>16);
    }
    *order=sorted;shared->profile_updates=updates;return 1;
}

/*1210c car interactions AFTER movement. Bit28 selects the attract branch,
 *which ignores501a80 and skips timers/passing. Normal inhibited frames skip
 *all car interactions. The15310 call is RET in daytona93: do NOT apply the
 *per-car15314/153c4 displacement a second time here. Audio-intent consumer
 *19038 remains caller-owned (normal branch only), even when output is silent.
 *As with collide, a failure stops the scene; earlier phases are not rolled back. */
static inline int nm_race_interactions_step(nm_race_car_t *cars,unsigned count,
        nm_race_shared_t *shared,nm_race_order_t *order,int32_t event_cooldowns[2],
        nm_traffic_pass_t *passing,nm_race_collisions_t *collisions,
        const nm_drive_params_t *math,const nm_road_pack_t *road,
        uint32_t inhibit,unsigned phase,uint32_t record_flags,uint8_t curve) {
    if(!cars || !shared || !order || !event_cooldowns || !passing || !collisions ||
       !math || !road || count<3 || count>44 || phase>=4 || road->profile_count<2 ||
       road->profile_count>4097) return 0;
    const int attract=(shared->flags&UINT32_C(0x10000000))!=0;
    if(!attract && inhibit) return 1;
    uint16_t window[NM_COLLISION_WINDOW];
    if(!nm_race_traffic_step(cars,count,order,shared,math) ||
       (!attract && !nm_race_signals_step(cars,count,shared,event_cooldowns,passing,math)) ||
       !nm_race_collision_prepare(cars,count,road->profile_count-1,window)) return 0;
    const nm_collision_damage_input_t input={shared->flags,cars[0].car.control,
        cars[0].car.lap.completed,curve};
    return nm_race_collide(cars,count,shared,math,road,window,phase,record_flags,&input,collisions);
}

static inline nm_guided_lane_car_t nm_race_lane_car(const nm_race_car_t *car, unsigned id) {
    return (nm_guided_lane_car_t){
        {car->car.boundary.ground.position[0],car->car.boundary.ground.position[2]},
        car->car.speed_index,car->car.boundary.damage.flags,car->car.lateral.current,
        car->near_marker,(uint8_t)id,(int8_t)(car->mode<128 ? car->mode : (int)car->mode-256)};
}

/*Normal player turn, d834..e0a4. Read the current native ring, not a frozen
 *frame-wide neighbor snapshot. Shared owners override stale copies carried
 *in player/input views. Rolling/finish and outer enable gates are separate.
 *The underlying tick publishes marks only after its final fallible operation. */
static inline int nm_race_player_step(nm_race_car_t *cars,unsigned count,
        nm_race_shared_t *shared,const nm_drive_params_t *math,
        const nm_contact_pack_t *contact,const nm_road_pack_t *road,const nm_path_pack_t *path,
        const nm_player_tick_input_t *input,nm_checkpoint_award_t *award,nm_road_marks_t *marks) {
    if(!cars || !shared || !math || !contact || !road || !input || !award || !marks ||
       !count || count>44 || (path && (cars[0].route!=path->route || path->slot!=road->slot))) return 0;
    nm_player_t next;
#ifdef __DREAMCAST__
    shz_memcpy4(&next,&cars[0].car,sizeof(next));
#else
    next=cars[0].car;
#endif
    next.checkpoints.clock=shared->clock;next.profile_updates=shared->profile_updates;
    next.boundary.damage.sequence_counter=shared->crash_counter;
    nm_player_tick_input_t tick; NM_COPY(&tick,input);
    tick.active.dynamics.speed_flags=shared->flags;
    tick.active.dynamics.guided_mode=shared->guided_mode;
    tick.active.dynamics.guided_scale=cars[0].guided_scale;
    tick.active.dynamics.course_slot=road->slot;tick.active.dynamics.entity_id=0;
    tick.active.dynamics.lap=next.lap.completed;
    if(!tick.active.proximity_disabled && next.boundary.recovery_ticks<=0) {
        unsigned id=0;
        for(unsigned i=0;i<3;++i) {
            id=cars[id].next;
            if(id>=count) return 0;
            tick.active.neighbor[i][0]=cars[id].car.boundary.ground.position[0];
            tick.active.neighbor[i][1]=cars[id].car.boundary.ground.position[2];
        }
    }
    if(!nm_player_tick_step(math,contact,road,path,&tick,&next,award,marks)) return 0;
    if(shared->flags&NM_PIT_FLAGS) shared->timer_hold=0; /* d824..d828 */
    shared->clock=next.checkpoints.clock;shared->profile_updates=next.profile_updates;
    shared->crash_counter=next.boundary.damage.sequence_counter;
#ifdef __DREAMCAST__
    shz_memcpy4(shared->heading_circle,next.proximity_circle,sizeof(shared->heading_circle));
    shz_memcpy4(&cars[0].car,&next,sizeof(next));
#else
    nm_memcpy(shared->heading_circle,next.proximity_circle,sizeof(shared->heading_circle));
    cars[0].car=next;
#endif
    return 1;
}

/*One normal opponent's turn, using current native neighbors. Publish shared
 *writes immediately for the NEXT car; do not freeze a frame-wide snapshot.
 *Player/rolling dispatch, traffic sorting and collision phases remain caller-owned. */
/* In-place core of nm_race_opponent_step: same arithmetic and order on
 * cars[id] and *shared directly. When it returns 0 the car may be left partly
 * updated (*shared is written only on success). The lane snapshots of the
 * player, this car, the car ahead and the remembered car are taken by value
 * before any motion, and read no field written before them, so the car ahead
 * or remembered being this same car changes nothing. For nm_race_movement_step,
 * whose documented contract is that a failure stops the scene without rolling
 * back. */
static inline int nm_race_opponent_step_inplace(nm_race_car_t *cars, unsigned count, unsigned id,
        nm_race_shared_t *shared, const nm_drive_params_t *math,
        const nm_intro_speed_params_t *speed, const nm_contact_pack_t *contact,
        const nm_road_pack_t *road, const nm_path_pack_t *path,
        const nm_road_marks_t *marks, const nm_crash_row_t *rows, unsigned row_count) {
    if(!cars || !shared || !math || !speed || !contact || !road || !road->profiles || !path ||
       !marks || !marks->values || count<2 || count>44 || !id || id>=count ||
       cars[id].next>=count || (cars[id].remembered!=255 && cars[id].remembered>=44) ||
       (uint16_t)cars[id].car.phase[0]>=road->profile_count ||
       (uint16_t)cars[id].car.phase[0]>=marks->count || cars[id].car.scenery_cell>=512 ||
       cars[id].route!=path->route || contact->slot!=road->slot || path->slot!=road->slot) return 0;
    nm_race_car_t *const next=cars+id;
    nm_race_shared_t *const context=shared;
    const unsigned profile=(uint16_t)next->car.phase[0],cell=next->car.scenery_cell;
    next->car.profile_updates=context->profile_updates;next->car.checkpoints.clock=context->clock;
    next->car.boundary.damage.sequence_counter=context->crash_counter;
    const nm_guided_lane_car_t player=nm_race_lane_car(cars,0),car=nm_race_lane_car(next,id);
    const nm_guided_lane_car_t front=nm_race_lane_car(cars+next->next,next->next);
    const nm_guided_lane_car_t saved=next->remembered==255 ? car :
        nm_race_lane_car(cars+next->remembered,next->remembered);
    nm_guided_race_input_t race={&player,&car,&front,next->remembered==255 ? NULL : &saved,
        cars[0].car.rank_key,next->car.rank_key,cars[0].car.boundary.heading,cars[0].rank,next->rank,
        {context->heading_circle[0],context->heading_circle[1],context->heading_circle[2]}};
    nm_player_dynamics_input_t dynamics;nm_memset(&dynamics,0,sizeof(dynamics));
    dynamics.speed_flags=context->flags;dynamics.guided_scale=next->guided_scale;dynamics.guided_mode=context->guided_mode;
    const unsigned mark=marks->values[profile];
    /* The cores below update the car in place; see the contract above. */
    if(!nm_opponent_motion_step_inplace(math,speed,path,road->profiles+profile,&dynamics,&race,marks->timer,
        (int8_t)(mark<128 ? (int)mark : (int)mark-256),&next->remembered,&next->car)) return 0;
    const nm_opponent_finish_input_t in={&cars[0].car,rows,row_count,context->tick,context->event_timer,
        next->other_yaw,(uint8_t)id,next->mode,context->guided_mode,
        (uint8_t)((context->visible_cells[cell>>5]>>(cell&31))&1),
        (int8_t)((next->relative&255)<128 ? (int)(next->relative&255) : (int)(next->relative&255)-256)};
    if(!nm_opponent_finish_step_inplace(math,contact,road,&in,&next->effects,&next->car)) return 0;
    context->profile_updates=next->car.profile_updates;context->crash_counter=next->car.boundary.damage.sequence_counter;
    if(next->effects.requests&1) context->inhibit_owner=(uint8_t)id;
    if(next->effects.requests&2) { context->event_timer=320;context->event=4; }
    return 1;
}
static inline int nm_race_opponent_step(nm_race_car_t *cars, unsigned count, unsigned id,
        nm_race_shared_t *shared, const nm_drive_params_t *math,
        const nm_intro_speed_params_t *speed, const nm_contact_pack_t *contact,
        const nm_road_pack_t *road, const nm_path_pack_t *path,
        const nm_road_marks_t *marks, const nm_crash_row_t *rows, unsigned row_count) {
    if(!cars || !shared || count<2 || count>44 || !id || id>=count) return 0;
    /* Atomic form: restore the one car this step writes if the core fails. */
    nm_race_car_t saved;
#ifdef __DREAMCAST__
    shz_memcpy4(&saved,cars+id,sizeof(saved));
#else
    saved=cars[id];
#endif
    if(nm_race_opponent_step_inplace(cars,count,id,shared,math,speed,contact,road,path,marks,rows,row_count)) return 1;
#ifdef __DREAMCAST__
    shz_memcpy4(cars+id,&saved,sizeof(saved));
#else
    cars[id]=saved;
#endif
    return 0;
}

/* Movement pass in car-ID order, NOT race-rank/render order. Lifecycle byte1
 * means rolling,0 normal; player alone supports2 finish entry,3 finish coast,
 * 4 pit entry (handler dc18, set by the entry test d920 of a normal tick:
 * route_transition), 5 pit (dd14; pit.h nm_player_pit_tick).
 * Release changes the NEXT tick's selection. Phase23 entry chooses2 before
 * this pass while latched flags remain23, even though next phase is24.
 * Caller owns initialization, phase timers, early diagnostic/enable gates,
 * then traffic/collisions/event tail. A late failure stops the scene, without
 * rolling back earlier cars or shared writes. No scheduler or recorded poses. */
static inline int nm_race_movement_step(nm_race_car_t *cars,unsigned count,
        nm_race_shared_t *shared,uint8_t *rolling,uint32_t *start_flags,
        const nm_drive_params_t *math,const nm_intro_speed_params_t *speed,
        const nm_contact_pack_t *contact,const nm_road_pack_t *road,const nm_path_pack_t paths[8],
        const nm_player_tick_input_t *input,nm_checkpoint_award_t *award,nm_road_marks_t *marks,
        unsigned mark_capacity,const nm_crash_row_t *rows,unsigned row_count) {
    if(!cars || !shared || !rolling || !start_flags || !math || !speed || !contact || !road ||
       !paths || !input || !award || !marks || !marks->values || count<3 || count>44 ||
       road->profile_count<2 || road->profile_count>4097 || marks->count!=road->profile_count-1 ||
       mark_capacity<road->profile_count) return 0;
    const int finishing=rolling[0]==2 || rolling[0]==3;
    if(finishing) {
        if((shared->flags!=(1u<<23) && shared->flags!=(1u<<24)) ||
           (input->phase!=24 && input->phase!=25 && input->phase!=27)) return 0;
    } else if((input->phase!=22 && ((input->phase!=23 && input->phase!=25) || shared->flags!=(1u<<22)) &&
            ((input->phase!=20 && input->phase!=21) || shared->flags!=(1u<<20))) ||
        /* Race entry: the phase20/21 object passes (handlers 3458/34a8) run the
         * same updaters with flags bit20 then bit21 before the phase22 handler;
         * phase20 lasts 321 ticks off course 0 (the 5010a8 countdown). */
        (shared->flags&UINT32_C(0x10000000)) || !(shared->flags&UINT32_C(0x00700000))) return 0;
    for(unsigned i=0;i<count;++i)
        if(rolling[i]>(i ? 1 : 5) || cars[i].route>=8 || paths[cars[i].route].route!=cars[i].route ||
           paths[cars[i].route].slot!=road->slot) return 0;
    shared->updating=0;
    if(rolling[0]) {
        nm_player_t next;
#ifdef __DREAMCAST__
        shz_memcpy4(&next,&cars[0].car,sizeof(next));
#else
        next=cars[0].car;
#endif
        next.checkpoints.clock=shared->clock;next.profile_updates=shared->profile_updates;
        next.boundary.damage.sequence_counter=shared->crash_counter;
        nm_player_active_input_t tick=input->active;
        tick.dynamics.speed_flags=shared->flags;tick.dynamics.guided_mode=shared->guided_mode;
        tick.dynamics.guided_scale=cars[0].guided_scale;tick.dynamics.course_slot=road->slot;
        tick.dynamics.entity_id=0;tick.dynamics.lap=next.lap.completed;
        uint8_t released=0;
        if(rolling[0]==4 || rolling[0]==5) {
            if(!nm_player_pit_tick(math,speed,contact,road,paths+cars[0].route,shared->pit_data,&tick,rolling[0]==4,
                   &next,&shared->pit,&shared->timer_hold,&released)) return 0;
            /* dc18 enabled: dd14 from the next tick; the release d7fc changes it to normal below. */
            if(tick.dynamics.speed_flags&NM_PIT_FLAGS) rolling[0]=5;
        } else if(finishing) {
            uint8_t entered=(uint8_t)(rolling[0]==3);
            if(!nm_player_finish_tick(math,contact,road,paths+cars[0].route,&tick,&next,&entered,&shared->timer_hold)) return 0;
            rolling[0]=(uint8_t)(2+entered);
        } else if(!nm_player_rolling_tick(math,contact,road,paths+cars[0].route,&tick,&next,start_flags,&released)) return 0;
        shared->clock=next.checkpoints.clock;shared->profile_updates=next.profile_updates;
        shared->crash_counter=next.boundary.damage.sequence_counter;
#ifdef __DREAMCAST__
        shz_memcpy4(shared->heading_circle,next.proximity_circle,sizeof(shared->heading_circle));
        shz_memcpy4(&cars[0].car,&next,sizeof(next));
#else
        nm_memcpy(shared->heading_circle,next.proximity_circle,sizeof(shared->heading_circle));cars[0].car=next;
#endif
        if(released) rolling[0]=0;
    } else {
        if(!nm_race_player_step(cars,count,shared,math,contact,road,paths+cars[0].route,input,award,marks)) return 0;
        if(cars[0].car.route_transition) rolling[0]=4; /* d9b0: +c := dc18 */
    }
    /* Player updates wrap at logical count; opponents also read the terminal
     * profile. This borrowed read-only view uses physical capacity, not wrap. */
    const nm_road_marks_t opponent_marks={marks->values,mark_capacity,marks->timer};
    for(unsigned id=1;id<count;++id) {
        nm_race_car_t *car=cars+id;
        shared->updating=(uint8_t)id;
        if(rolling[id]) {
            const unsigned profile=(uint16_t)car->car.phase[0],cell=car->car.scenery_cell;
            if(profile>=mark_capacity || cell>=512) return 0;
            const unsigned mark=marks->values[profile];
            /* In place, as for the normal step below: a failure stops the scene. */
            const int result=nm_opponent_rolling_step_inplace(math,road,paths+car->route,id,cars[0].car.boundary.speed,
                marks->timer,(int8_t)(mark<128 ? (int)mark : (int)mark-256),
                (shared->visible_cells[cell>>5]>>(cell&31))&1,&car->car);
            if(!result) return 0;
            if(result==2) rolling[id]=0;
        } else if(!nm_race_opponent_step_inplace(cars,count,id,shared,math,speed,contact,road,paths+car->route,
                    &opponent_marks,rows,row_count)) return 0;
    }
    return 1;
}

/*e20c reset, including metadata previously outside the single-player view.
 *Preserve fields that the source does not reset, including height history
 *and the remembered neighbor. Caller initializes cold storage separately. */
static inline int nm_race_car_grid_reset(nm_race_car_t *state,
        const nm_drive_params_t *math, const nm_contact_pack_t *contact,
        const nm_road_pack_t *road, const nm_path_pack_t *path,
        unsigned id, unsigned model_variant) {
    if(!state || !math || !contact || !road || !road->grid || !road->grid_speeds ||
       !road->grid_guided_scales || !road->start || id>=road->grid_count || id>=44 ||
       model_variant>=4 || road->grid[id].route>=8 || road->grid[id].style>255 ||
       !isfinite(road->grid_guided_scales[id])) return 0;
    nm_race_car_t next;
#ifdef __DREAMCAST__
    shz_memcpy4(&next,state,sizeof(next));
#else
    next=*state;
#endif
    if(!nm_player_grid_reset(math,contact,road,path,id,road->grid_speeds[id],
        road->start->body_model[model_variant],&next.car,&next.effects.height.reference_height)) return 0;
    next.grid_speed=road->grid_speeds[id];next.guided_scale=road->grid_guided_scales[id];
    next.route=(uint8_t)road->grid[id].route;next.mode=(uint8_t)road->grid[id].style;
    next.next=next.previous=(uint16_t)id;
    next.effects.height.velocity=0; /*e4b8: a0 resets, unlike sampled280/284. */
    next.other_yaw=next.effects.correction_yaw=0;
    next.effects.contact_age=127;next.effects.requests=0;
#ifdef __DREAMCAST__
    shz_memcpy4(state,&next,sizeof(next));
#else
    *state=next;
#endif
    return 1;
}
#endif
