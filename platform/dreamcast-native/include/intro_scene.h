#ifndef DAYTONA_NATIVE_INTRO_SCENE_H
#define DAYTONA_NATIVE_INTRO_SCENE_H
#include "intro_drive_data.h" /* Generated locally from verified owned ROMs. */
#include "visibility.h"
#include "reflection.h"
#include "collision_response.h"
#include "car_detail.h"
#include "slot_machine.h"
#include "tunnel_light.h"
#include "nm_mem.h"

/* Native scene assembly, not a board scheduler or recorded pose stream.
 * Remaining special-actor/attachment callbacks are NOT approximated here.
 * This is a partial attract scene until those source callbacks are connected. */
typedef struct {
    nm_player_t cars[44];
    nm_intro_anchor_t anchors[44];
    float reference_height[44];
    int16_t correction_yaw[44];
    int16_t collision_other_yaw[44]; /*212; correction_yaw owns216. */
    nm_intro_cursor_t events, camera_cursor;
    nm_intro_camera_t camera;
    alignas(32) float view[12];
    uint32_t stage, pending_stage, count, ticks, transitions;
    int16_t heading;
    uint16_t reflection_phase,reflection_u,reflection_v,sky_scroll;
    int16_t detail_depth[44];
    uint8_t detail_order[44],detail_level[44];
    alignas(32) nm_traffic_order_body_t traffic[44];
    alignas(32) uint16_t rank_order[44],track_order[44];
    int8_t traffic_lap;
    nm_race_timers_t race_timers;
    nm_traffic_pass_t passing;
    uint16_t near_marker[44]; /*146; high half of source32-bit writes aliases gear148. */
    alignas(32) nm_collision_travel_t collision_motion[44];
    uint32_t collision_relative[44];
    uint16_t collision_adjusted[44],collision_candidates[44],collision_order[14];
    uint8_t collision_side[44];
    nm_collision_contact_t collision_contact;
    uint32_t collision_pairs,collision_responses;
    uint32_t crash_starts,crash_ticks;
    int8_t restored_camera_mode; /* Selected native view descriptor, not current camera mode. */
    uint32_t player_crash_ticks;
    nm_scenery_cells_t cells;
    /* The cells of the pass before: the world-object actors (task list, 2002c)
     * test bitmaps 501520/501540 before the scenery object 5890 rewrites them,
     * as the view pass does. Presentation only; no gameplay reads it. */
    nm_scenery_cells_t cells_previous;
    /* Scene selection: authored stage graph, source501090 phase flags and the
     * record a car spawned on the last profile reads once (cffc, no wrap). */
    const nm_intro_stage_data_t *stages;
    const nm_road_profile_t *overrun;
    uint32_t stage_count,flags;
    uint8_t course; /* 501460 of the scene's road pack: the attract's 0, the podium's the race's course */
    uint8_t crash_sequence[44]; /* Directory of each car's active rows (source278). */
    /* Animated world objects (presentation only; no gameplay reads it): per
     * scenery cell, the ticks of this scene on which the cell's bit was set
     * in the near bitmap 501540 the actors saw (the pass before). The die
     * 220be4, the symbol wheel 220b14 and the pit crews 220d44 step only on
     * such ticks (their near test returns before any update) and are
     * recreated at scene start, as this structure is cleared. */
    uint16_t object_near_ticks[256];
    /* The slot machine 2204fc of the cinematic scenes: its own actor, made
     * at scene start like the race's (nm_race_scene_t.slot_machine), with
     * gated sound: visual only (nothing reads 5010d0 after phase22).
     * object_clock + ticks is the original's 5010b0. start_edge: 53e584 bit 4
     * of the next nm_intro_scene_detail, which consumes it (the podium). */
    nm_slot_machine_t slot_machine;
    uint32_t object_clock;
    uint8_t start_edge;
    /* 5010b0 - ticks on a step, for the words 5818 derives from it: the variant bit 503690 (5010b0 & 1) and the
     * collision phase 503692 (& 3). The podium: the race's counter base + object_clock + 1 (its first step is
     * two frames after the race's last tick: the entry and the spawn); the attract: object_clock - 3 (0 after
     * power-on; attract trace, the fourth loop's collisions). */
    uint32_t phase_clock;
    /* The scene light (camera object +60..+68, tunnel_light.h) of the attract: its camera object is the race's
     * (6484 -> 6510), whose 7024 follows the player into the tunnel ranges of the course (Advanced). Presentation
     * only; NULL tunnel: the normal light throughout (the podium). nm_intro_scene_light sets it after the start. */
    const nm_tunnel_lights_t *tunnel;
    nm_tunnel_light_t scene_light;
    /* The windows' tunnel texture (6fe0/7008, from the view pass's 6f24, as in the race): the last TUNNEL or RESTORE
     * of reflection.h a tick asked for, until the presentation takes it (0: none). */
    uint8_t window_action,window_others;
} nm_intro_scene_t;

/* 6850 -> 7024 every tick (a stage's entry tick too): the camera object's scene light from its car (the player),
 * after the view. */
static inline int nm_intro_scene_light_step(nm_intro_scene_t *s) {
    const nm_player_t *p=s->cars;
    return !s->tunnel || nm_tunnel_light_step(&s->scene_light,s->tunnel,p->boundary.ground.position,(uint16_t)p->phase[0],
        p->boundary.damage.flags,p->path_flags,(uint32_t)p->speed_index);
}
/* 6484 at the attract's setup (phase 2): the course's tunnel ranges, the normal light, range index 0. */
static inline void nm_intro_scene_light(nm_intro_scene_t *s,const nm_tunnel_lights_t *tunnel) {
    s->tunnel=tunnel && tunnel->normal && tunnel->rates ? tunnel : NULL;
    if(s->tunnel) nm_tunnel_light_reset(&s->scene_light,s->tunnel->normal);
}

/* The podium's 5010b0 - 1 against the race's final tick count and the
 * podium scene's ticks: the entry tick and the spawn tick come between them. */
#ifndef NM_PODIUM_CLOCK_OFFSET
#define NM_PODIUM_CLOCK_OFFSET 1u
#endif

/* One tick of the world-object actors: after cells_previous was set to the
 * cells they test (the race's and the cinematic scenes' detail passes). */
static inline int nm_world_objects_tick(uint16_t near_ticks[256],const nm_scenery_cells_t *seen) {
    uint32_t bitmaps[16];
    if(!near_ticks || !seen || !nm_scenery_bitmaps(seen,bitmaps)) return 0;
    for(unsigned w=0;w<8;++w)
        for(uint32_t bits=bitmaps[8+w];bits;bits&=bits-1u) ++near_ticks[32u*w+(unsigned)__builtin_ctz(bits)];
    return 1;
}

/*1210c: movement has completed for the entire field before
 *1224c. Keep these lists separate from render-depth ordering. The observed
 * cinematic flags=8 leave race ranks
 * disabled; track ordering and its neighbor ring still run.
 * Standalone boot starts shared lap at zero; stage entry must preserve it. */
static inline int nm_intro_scene_traffic(nm_intro_scene_t *s) {
    if(!s || s->count<3 || s->count>44) return 0;
    for(unsigned i=0;i<s->count;++i) {
        s->traffic[i].rank_key=s->cars[i].rank_key;
        s->traffic[i].profile=(uint16_t)s->cars[i].phase[0];
        const unsigned lap=s->cars[i].lap.lap;
        s->traffic[i].lap=(int8_t)(lap<128 ? (int)lap : (int)lap-256);
    }
    return nm_traffic_order(s->traffic,s->count,s->rank_order,s->track_order,
        s->count,s->count,s->flags,&s->traffic_lap);
}

/*12464 car effects: source reset pool starts at opponent1, not player0.
 * Rotate the temporary view so the existing helper's reset prefix matches.
 *139d8 operates on the non-car scheduler object; no fake car is introduced. */
static inline int nm_intro_scene_near(nm_intro_scene_t *s,const nm_drive_params_t *math) {
    if(!s || !math || s->count<3 || s->count>44) return 0;
    alignas(32) nm_traffic_near_body_t bodies[44];
    for(unsigned i=0;i<s->count;++i) {
        const unsigned next=s->traffic[i].next,previous=s->traffic[i].previous;
        if(next>=s->count || previous>=s->count) return 0;
        bodies[i ? i-1 : s->count-1]=(nm_traffic_near_body_t){
            .flags=s->cars[i].boundary.damage.flags,
            .marker146=s->near_marker[i]|((s->cars[i].gear&65535u)<<16),
            .position={s->cars[i].boundary.ground.position[0],s->cars[i].boundary.ground.position[2]},
            .next=(uint16_t)(next ? next-1 : s->count-1),
            .previous=(uint16_t)(previous ? previous-1 : s->count-1)};
    }
    if(!nm_traffic_near_neighbors(math,bodies,s->count,s->count-1,s->count-1,
            &s->cars[0].profile_updates)) return 0;
    for(unsigned i=0;i<s->count;++i) {
        const nm_traffic_near_body_t *b=bodies+(i ? i-1 : s->count-1);
        s->cars[i].boundary.damage.flags=b->flags;
        s->near_marker[i]=(uint16_t)b->marker146;
        s->cars[i].gear=(s->cars[i].gear&UINT32_C(0xffff0000))|(b->marker146>>16);
    }
    return 1;
}

/*12150 then125b0. Player control/damage.signal own501124/501a04;
 * timer/passing views borrow them only for this call. Passing publishes a signal,
 * not audio. Existing source-table length preserves the exact15-unit gate. */
static inline int nm_intro_scene_signals(nm_intro_scene_t *s,const nm_drive_params_t *math) {
    if(!s || !math || s->count<3 || s->count>44) return 0;
    s->race_timers.control=nm_gears_signed(s->cars[0].control);
    if(!nm_race_timers_step(&s->race_timers)) return 0;
    s->cars[0].control=(uint32_t)s->race_timers.control;
    alignas(32) nm_traffic_pass_body_t bodies[44];
    for(unsigned i=0;i<s->count;++i) {
        bodies[i]=(nm_traffic_pass_body_t){
            .progress=(uint32_t)s->cars[i].speed_index,
            .position={s->cars[i].boundary.ground.position[0],s->cars[i].boundary.ground.position[2]},
            .lateral=s->cars[i].lateral.current,
            .next=s->traffic[i].next,.previous=s->traffic[i].previous};
    }
    /*501a04 is also written by collision impulse/player damage. Borrow its
     *current value, not the stale output of the previous passing call. */
    s->passing.signal=(uint16_t)s->cars[0].boundary.damage.signal;
    if(!nm_traffic_pass(math,bodies,s->count,0,&s->passing)) return 0;
    s->cars[0].boundary.damage.signal=nm_yaw_angle(s->passing.signal);
    return 1;
}

/*13f08 preparation only, before the live pair/response walk. Keep history in
 * spawn-index order across stage changes; the transient reset-pool view puts
 * player0 last so its corner flags are not reset along with the opponents. */
static inline int nm_intro_scene_collision_prepare(nm_intro_scene_t *s,const nm_road_pack_t *road) {
    if(!s || !road || s->count<3 || s->count>44 || road->profile_count<2) return 0;
    alignas(32) nm_collision_rank_t bodies[44];uint16_t order[14];
    for(unsigned i=0;i<s->count;++i) {
        const unsigned next=s->traffic[i].next,previous=s->traffic[i].previous;
        if(next>=s->count || previous>=s->count) return 0;
        bodies[i ? i-1 : s->count-1]=(nm_collision_rank_t){
            .flags=s->cars[i].boundary.damage.flags,.relative=s->collision_relative[i],
            .position={s->cars[i].boundary.ground.position[0],s->cars[i].boundary.ground.position[2]},
            .motion=s->collision_motion[i],.profile=(uint16_t)s->cars[i].phase[0],
            .next=(uint16_t)(next ? next-1 : s->count-1),
            .previous=(uint16_t)(previous ? previous-1 : s->count-1),
            .adjusted=s->collision_adjusted[i],.candidates=s->collision_candidates[i],
            .contact_side=s->collision_side[i]};
    }
    if(!nm_collision_prepare(bodies,s->count,s->count-1,s->count-1,road->profile_count-1,order)) return 0;
    for(unsigned i=0;i<s->count;++i) {
        const nm_collision_rank_t *b=bodies+(i ? i-1 : s->count-1);
        s->cars[i].boundary.damage.flags=b->flags;s->collision_relative[i]=b->relative;
        s->collision_motion[i]=b->motion;s->collision_adjusted[i]=b->adjusted;
        s->collision_candidates[i]=b->candidates;s->collision_side[i]=b->contact_side;
    }
    for(unsigned i=0;i<14;++i) s->collision_order[i]=order[i]+1==s->count ? 0 : (uint16_t)(order[i]+1);
    return 1;
}

/* Intro and race share the same four-operation response and atomic writes. */
static inline int nm_intro_scene_collision_response(nm_intro_scene_t *s,
        const nm_drive_params_t *math,const nm_road_pack_t *road,
        unsigned first,unsigned second,const uint8_t entity_id[2],
        const nm_collision_contact_t *contact,unsigned branch,uint32_t record_flags,
        const nm_collision_damage_input_t *input) {
    if(!s || first>=s->count || second>=s->count || s->count>44) return 0;
    const nm_collision_car_view_t view[2]={
        {s->cars+first,s->collision_side+first,s->correction_yaw+first,s->collision_other_yaw+first},
        {s->cars+second,s->collision_side+second,s->correction_yaw+second,s->collision_other_yaw+second}};
    return nm_collision_response(math,road,s->cars,&s->cars[0].boundary.damage.sequence_counter,
        view,entity_id,contact,branch,record_flags,input);
}

/* One13f08 pass after preparation. Publish each response before asking for
 * the next pair; its position/5c changes affect the live broad phase. Keep
 * transformed corners cached mid-pass exactly as the source flag5 requests. */
static inline int nm_intro_scene_collide(nm_intro_scene_t *s,const nm_drive_params_t *math,
        const nm_road_pack_t *road,unsigned phase,uint32_t record_flags,
        const nm_collision_damage_input_t *input) {
    if(!s || !math || !road || !input || s->count<3 || s->count>44 || phase>=4) return 0;
    alignas(32) nm_collision_walk_body_t bodies[44];
    for(unsigned i=0;i<s->count;++i) bodies[i]=(nm_collision_walk_body_t){
        {{s->cars[i].boundary.ground.position[0],s->cars[i].boundary.ground.position[2]},
            s->cars[i].boundary.damage.overlay_model},s->traffic[i].next,s->collision_candidates[i]};
    nm_collision_walk_t walk={.budget=1000};unsigned cost=0;
    for(;;) {
        uint16_t pair[2];
        const int found=nm_collision_walk_next(math,bodies,s->count,s->collision_order,0,phase,cost,&walk,pair);
        if(found<=0) return found==0;
        const unsigned other=pair[0]==pair[1] ? 0 : 1;
        const nm_collision_car_view_t view[2]={
            {s->cars+pair[0],s->collision_side+pair[0],s->correction_yaw+pair[0],s->collision_other_yaw+pair[0]},
            {s->cars+pair[1],s->collision_side+pair[1],s->correction_yaw+pair[1],s->collision_other_yaw+pair[1]}};
        const nm_collision_travel_t *travel[2]={s->collision_motion+pair[0],s->collision_motion+pair[1]};
        const uint8_t ids[2]={(uint8_t)pair[0],(uint8_t)pair[1]};
        nm_collision_route_t route;
        const uint8_t crashes[2]={s->cars[pair[0]].boundary.damage.crash_count,
            s->cars[pair[1]].boundary.damage.crash_count};
        if(!nm_collision_pair(math,road,s->cars,&s->cars[0].boundary.damage.sequence_counter,
            view,travel,ids,&s->collision_contact,record_flags,input,&route)) return 0;
        /*15cc8/15cd4 collision crashes always start the shared variant0 rows. */
        for(unsigned i=0;i<2;++i) if(s->cars[pair[i]].boundary.damage.crash_count!=crashes[i])
            s->crash_sequence[pair[i]]=0;
        s->collision_responses+=route.branch!=NM_COLLISION_NONE;
        for(unsigned i=0;i<=other;++i) {
            const nm_player_t *car=s->cars+pair[i];
            bodies[pair[i]].probe=(nm_collision_probe_t){
                {car->boundary.ground.position[0],car->boundary.ground.position[2]},car->boundary.damage.overlay_model};
        }
        cost=route.caller_result;++s->collision_pairs;
    }
}

/* d290/d118 call15de0 after pose; player d230 has a different prefix.
 * Cinematic global8 selects directory[frame&1], NOT the special-car kind.
 *503690 is frame&1 from05818; flag28 (podium) selects the third directory.
 * The choice is made only when15df0..15e00 permits a restart: an active
 * animation keeps its rows.
 * Share503680 via player0 between every opponent, retaining per-car storage. */
static inline int nm_intro_scene_crash(nm_intro_scene_t *s,unsigned car) {
    if(!s || !car || car>=s->count || s->count>44) return 0;
    nm_damage_state_t next=s->cars[car].boundary.damage;
    if((next.flags&(1u<<6)) && !s->cars[car].input_inhibit && !next.inhibit_ticks)
        s->crash_sequence[car]=(s->flags&UINT32_C(0x10000000)) ? 2 : (uint8_t)(s->ticks&1);
    const unsigned variant=s->crash_sequence[car];
    if(variant>=sizeof(nm_intro_crash_sequences)/sizeof(nm_intro_crash_sequences[0])) return 0;
    const uint8_t prior=next.sequence_counter,shared=s->cars[0].boundary.damage.sequence_counter;
    next.sequence_counter=shared;
    if(!nm_intro_crash_step(nm_intro_crash_sequences[variant].rows,nm_intro_crash_sequences[variant].count,
        (uint8_t)car,s->cars[car].input_inhibit,&next)) return 0;
    const uint8_t counter=next.sequence_counter;
    next.sequence_counter=prior;
    s->cars[car].boundary.damage=next;s->cars[0].boundary.damage.sequence_counter=counter;
    s->crash_starts+=counter!=shared;s->crash_ticks+=next.remaining!=0;
    return 1;
}

/* Course0 has no bit256 road flags (validated at scene entry), so neither
 * player nor neighbor can request tunnel-header changes there. A cinematic of
 * another course (its attract, the podium) runs on that course's road: a tunnel edge (bit 256) of car 0
 * is a window-header change (6fe0/7008, presentation) or nothing, and leaves
 * the UV offsets as they were. The shared phase
 * follows player cars[0], persists across stages, and advances on ticks only.
 * Standalone scene uses frame/phase zero, like practice; full boot alignment
 * belongs to the future title-to-attract transition, not sampled ROM poses. */
static inline int nm_intro_scene_reflection(nm_intro_scene_t *s) {
    if(!s->course && (s->cars[0].path_flags|s->cars[0].previous_path_flags)&256) return 0;
    const nm_reflection_input_t input={.frame=s->ticks,.speed_index=(uint32_t)s->cars[0].speed_index,
        .flags=s->cars[0].path_flags,.previous_flags=s->cars[0].previous_path_flags,
        .heading=s->heading,.course=s->course,.next_mode=0,.active_mode=0};
    nm_reflection_update_t update;
    if(!nm_reflection_step(&input,&s->reflection_phase,&update) ||
       (!s->course && update.action!=NM_REFLECTION_UV)) return 0;
    if(update.action==NM_REFLECTION_UV) { s->reflection_u=update.u_offset;s->reflection_v=update.v_offset; }
    else if(update.action) { s->window_action=update.action;s->window_others=update.others_only; } /* for the presentation */
    return 1;
}

/* Once per simulation tick, including initial scene entry. Repeated render
 * calls must not advance the persistent source ordering pass. */
static inline int nm_intro_scene_detail(nm_intro_scene_t *s,const nm_drive_params_t *math,
        const nm_scenery_pack_t *scenery) {
    uint16_t cell;nm_scenery_cells_t next;
    if(!s || !math || !s->count || s->count>44) return 0;
    /* 5970..5ae8: the keyed camera's scenery anchor is the eye moved 10 units
     * along heading 5014a8, not the eye itself (same routine as the race start). */
    const float dx=nm_drive_sine(math,(uint16_t)s->heading)*10.0f;
    const float dz=nm_drive_sine(math,(uint16_t)(s->heading+16384))*10.0f;
    const float x=dx+s->camera.position[0],z=s->camera.position[2]-dz;
    if(!nm_contact_scenery_cell(x,z,&cell) ||
       !nm_scenery_select(scenery,math,cell,(uint16_t)s->cars[0].phase[0],x,z,s->heading,&next)) return 0;
    const nm_player_t *cars[44];
    for(unsigned i=0;i<s->count;++i) cars[i]=s->cars+i;
    /* View pass 6b58 (84e0 requests) runs before the scenery object 5890
     * rewrites bitmap 501520: this view with the PREVIOUS pass's cells.
     * Same list builder 8480..84dc as the race: the pool at 501224 only, so
     * the car in slot 501220 (car 0) is never listed (list count 19 with 20
     * cars in the attract) and is drawn with its own fixed parts. */
    if(!(s->count>=2 ? nm_race_detail_step : nm_car_detail_step)(s->view,&s->cells,cars,s->count,
        s->detail_depth,s->detail_order,s->detail_level)) return 0;
    s->cells_previous=s->cells;
    s->cells=next;
    uint32_t bitmaps[16],remaining=0;
    if(!nm_world_objects_tick(s->object_near_ticks,&s->cells_previous) ||
       !nm_scenery_bitmaps(&s->cells_previous,bitmaps)) return 0;
    if(!s->course) /* the slot machine is a course-0 actor */
        (void)nm_slot_machine_tick(&s->slot_machine,nm_slot_machine_visible(bitmaps),s->object_clock+s->ticks,s->start_edge,
            s->flags,&remaining); /* 220534 of the scene's actor; flags 1<<3 / 1<<28 gate the sound off */
    s->start_edge=0;
    return 1;
}

static inline int nm_intro_scene_camera(nm_intro_scene_t *s,const nm_drive_params_t *math) {
    const nm_intro_stage_data_t *stage=s->stages+s->stage;
    for(unsigned i=0;i<s->count;++i) {
        const nm_wheels_state_t *g=&s->cars[i].boundary.ground;
        for(unsigned a=0;a<3;++a) s->anchors[i].position[a]=g->position[a];
        s->anchors[i].angles[0]=g->pitch;s->anchors[i].angles[1]=s->cars[i].body_yaw;
        s->anchors[i].angles[2]=g->roll;
    }
    nm_intro_interval_t interval;
    if(!nm_intro_camera_step(stage->times,stage->key_count,&s->camera_cursor,&interval) ||
        !nm_intro_camera_sample(math,stage->keys+interval.key,stage->keys+interval.key+1,
            s->anchors,s->count,interval.elapsed,interval.duration,&s->camera)) return 0;
    if(!nm_intro_camera_view(math,&s->camera,s->view,&s->heading,&s->sky_scroll)) return 0;
    return 1;
}

/* Failures stop the scene; no claim of atomic rollback of an entire field. */
static inline int nm_intro_scene_enter(nm_intro_scene_t *s,unsigned index,
        const nm_drive_params_t *math,const nm_road_pack_t *road,const nm_path_pack_t paths[8]) {
    if(!s->stages || index>=s->stage_count) return 0;
    const nm_intro_stage_data_t *stage=s->stages+index;
    if(stage->cars<3 || stage->cars>44) return 0;
    for(unsigned i=0;i<stage->cars;++i) {
        const nm_intro_spawn_t *spawn=stage->spawn+i;
        if(spawn->route>=8 || !nm_intro_spawn_car(spawn,road,paths+spawn->route,
                s->cars+i,s->reference_height+i)) return 0;
    }
    s->stage=index;s->count=stage->cars;s->pending_stage=UINT32_MAX;
    if(!nm_traffic_order_init(s->rank_order,s->track_order,s->count)) return 0;
    s->passing=(nm_traffic_pass_t){s->traffic[0].next,s->traffic[0].previous,0};
    s->cars[0].boundary.damage.signal=0;
    s->cars[0].boundary.pivot_signal=0;
    for(unsigned i=0;i<s->count;++i) { s->detail_order[i]=(uint8_t)i;s->detail_depth[i]=INT16_MAX;s->detail_level[i]=0; }
    s->events=(nm_intro_cursor_t){0};s->camera_cursor=(nm_intro_cursor_t){0};
    /* 226378 re-arms the camera object (6484): its scene light's range index +84 starts at 0 again, the light
     * stays (attract trace, Advanced: 20 -> 0 at the stage's re-entry, 5010b0 4752). */
    s->scene_light.index=0;
    return nm_intro_scene_camera(s,math);
}

/* flags is the scene's source501090 word: 1<<3 attract, 1<<28 podium. overrun
 * may be NULL when no stage spawns a car on the last authored profile. */
static inline int nm_intro_scene_start_stages(nm_intro_scene_t *s,
        const nm_intro_stage_data_t *stages,unsigned stage_count,uint32_t flags,
        const nm_road_profile_t *overrun,const nm_drive_params_t *math,
        const nm_road_pack_t *road,const nm_path_pack_t paths[8]) {
    if(!s || !stages || !stage_count || stage_count>64 || !math || !road || !paths ||
       (flags!=(1u<<3) && flags!=(1u<<28)) ||
       ((flags>>28) && sizeof(nm_intro_crash_sequences)/sizeof(nm_intro_crash_sequences[0])<3)) return 0;
    /* The attract runs its loop's course (2865a60[501460]), the podium the race's (2865a70[501460]). */
    if(road->slot>2 || !road->mapping || !road->map_count) return 0;
    /* Active animation keeps its original sequence pointer. Both cinematic
     * parity directories deduplicate to the same owned-ROM sequence here. */
    if(nm_intro_crash_sequences[0].rows!=nm_intro_crash_sequences[1].rows ||
       nm_intro_crash_sequences[0].count!=nm_intro_crash_sequences[1].count) return 0;
    for(unsigned i=0;!road->slot && i<road->map_count;++i) if(road->mapping[i].flags&256) return 0;
    nm_memset(s,0,sizeof(*s));
    s->course=(uint8_t)road->slot;
    /* Fresh standalone view0: original initialized backup descriptor restores2.
     * A future view/settings change must update this selected descriptor value. */
    s->restored_camera_mode=2;
    s->stages=stages;s->stage_count=stage_count;s->flags=flags;s->overrun=overrun;
    for(unsigned i=0;i<44;++i) if(!nm_player_base_reset(s->cars+i,0)) return 0;
    return nm_intro_scene_enter(s,0,math,road,paths);
}
/* The attract's 5010b0 is its scene tick + 3 (reference display lists, as
 * the video screen); the podium sets object_clock itself after its start.
 * STAGES: the attract graph of the road's course (226360: 2865a60[501460]; podium_data.h nm_attract_courses),
 * course 0's the intro header's nm_intro_stages. */
static inline int nm_intro_scene_start_attract(nm_intro_scene_t *s,const nm_intro_stage_data_t *stages,
        unsigned stage_count,const nm_drive_params_t *math,const nm_road_pack_t *road,const nm_path_pack_t paths[8]) {
    if(!nm_intro_scene_start_stages(s,stages,stage_count,1u<<3,NULL,math,road,paths)) return 0;
    s->object_clock=3;return 1;
}
static inline int nm_intro_scene_start(nm_intro_scene_t *s,const nm_drive_params_t *math,
        const nm_road_pack_t *road,const nm_path_pack_t paths[8]) {
    return !road->slot && nm_intro_scene_start_attract(s,nm_intro_stages,
        sizeof(nm_intro_stages)/sizeof(nm_intro_stages[0]),math,road,paths);
}

static inline int nm_intro_scene_step(nm_intro_scene_t *s,const nm_drive_params_t *math,
        const nm_contact_pack_t *contact,const nm_road_pack_t *road,const nm_path_pack_t paths[8]) {
    if(!s || !math || !contact || !road || !paths || !s->stages || s->stage>=s->stage_count) return 0;
    if(s->pending_stage!=UINT32_MAX) {
        if(!nm_intro_scene_enter(s,s->pending_stage,math,road,paths)) return 0;
        if(!nm_intro_scene_reflection(s) || !nm_intro_scene_light_step(s)) return 0;
        ++s->transitions;++s->ticks;return 1;
    }
    const nm_intro_stage_data_t *stage=s->stages+s->stage;
    if(s->count!=stage->cars || !nm_intro_events_step(stage->events,stage->event_count,
            s->cars,s->count,&s->events,&s->pending_stage)) return 0;
    for(unsigned i=0;i<s->count;++i) {
        const nm_intro_spawn_t *spawn=stage->spawn+i;
        if(spawn->kind==0) {
            const nm_player_t *car=s->cars+i;
            const nm_damage_pre_input_t in={s->flags,
                (uint32_t)car->path_flags|((uint32_t)car->previous_path_flags<<16),
                car->phase[0],car->boundary.ground.yaw,s->restored_camera_mode,(uint8_t)((s->phase_clock+s->ticks)&1),0};
            const int active=car->boundary.damage.remaining!=0;
            if(!nm_player_damage_prefix(math,&in,s->cars+i,1)) return 0;
            s->player_crash_ticks+=active;
        }
        const unsigned profile=(uint16_t)s->cars[i].phase[0];
        if(!road->profiles || profile>road->profile_count || (profile==road->profile_count && !s->overrun) ||
           /* In place: a failure anywhere in this loop already stops the
            * scene with earlier cars updated, so no per-car rollback copy. */
           !nm_intro_drive_step_profile_inplace(math,&nm_intro_speeds,road,paths+spawn->route,
               profile==road->profile_count ? s->overrun : road->profiles+profile,
               s->flags,spawn->kind==0,s->cars+i) ||
           !nm_intro_pose_step_inplace(contact,math,s->cars+i,s->collision_other_yaw[i],s->correction_yaw+i)) return 0;
        /* d288, player only; cf74 disables caf4's optional tilt update.
         * Standalone reset supplies zero shared tilt. Preserve that value
         * across stages; full title/race transitions must carry shared tilt. */
        if(spawn->kind==0 && !nm_player_body_angles_step(math,s->cars[i].tilt[0],s->cars+i)) return 0;
        if(spawn->kind!=0 && !nm_intro_scene_crash(s,i)) return 0;
    }
    if(!nm_intro_scene_traffic(s) || !nm_intro_scene_near(s,math) || !nm_intro_scene_signals(s,math) ||
       !nm_intro_scene_collision_prepare(s,road)) return 0;
    /* Source05818 derives phase from the frame counter&3. Standalone intro
     * starts at frame0 (as reflection does). Observed cinematic context:
     * global8, worker record80000000, course0 curve0; entity IDs are spawn indices. */
    /*Damage selector reads source6D (completed), not current lap6E. */
    const nm_collision_damage_input_t collision_input={.global_flags=s->flags,
        .control=s->cars[0].control,.player_mode=s->cars[0].lap.completed,.curve=0};
    if(!nm_intro_scene_collide(s,math,road,(s->phase_clock+s->ticks)&3,UINT32_C(0x80000000),&collision_input) ||
       !nm_intro_scene_camera(s,math) || !nm_intro_scene_reflection(s)) return 0;
    if(!nm_intro_scene_light_step(s)) return 0;
    ++s->ticks;return 1;
}
#endif
