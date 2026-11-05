#ifndef DAYTONA_NATIVE_INTRO_MATH_H
#define DAYTONA_NATIVE_INTRO_MATH_H
#include "nm_mem.h"
#include "intro.h"
#include "pose.h"
#include <stdalign.h>

typedef struct {
    alignas(32) int32_t position_anchor;
    int32_t target_anchor; /* -1 world space; otherwise live car index. */
    float position[3], target[3], roll, focus;
} nm_intro_key_t;
typedef struct { alignas(32) float position[3]; int16_t angles[3]; } nm_intro_anchor_t;
typedef struct {
    alignas(32) float position[3];
    float target[3], focus;
    int16_t roll;
    uint16_t reserved;
} nm_intro_camera_t;

typedef struct {
    float desired_divisor, drag_offset, minimum_delta;
    float response[512];
} nm_intro_speed_params_t;

/* Offline2264f4 event: source speed product, signed actor/lane and resolved
 * stage index. Supported callbacks are the authored no-op2265c0, removed offline. */
typedef struct {
    uint32_t tick;
    float speed;
    int16_t actor, lateral;
    uint32_t next_stage;
} nm_intro_event_t;

typedef struct {
    uint32_t route;
    float speed;
    uint16_t profile;
    int16_t lateral;
    uint32_t kind; /*0 leader,1 opponent,2 special d05c/d118 actor; resolved offline. */
} nm_intro_spawn_t;

/*226468 plus cfd0..d058 (or identical d08c..d114) pose/effect reset.
 * Caller binds route, model resources and the kind-specific updater.
 * Preserve fields the source doesn't reset. */
static inline int nm_intro_spawn_car(const nm_intro_spawn_t *spawn,
        const nm_road_pack_t *road, const nm_path_pack_t *path,
        nm_player_t *state, float *reference_height) {
    if(!spawn || !road || !road->profiles || !path || !state || !reference_height ||
       spawn->kind>2 || spawn->route!=path->route || road->slot!=path->slot ||
       spawn->profile>=road->profile_count || !isfinite(spawn->speed)) return 0;
    nm_player_t next; NM_COPY(&next,state);
    next.phase[0]=next.phase[1]=nm_yaw_angle(spawn->profile);
    next.lap.furthest=spawn->profile;
    next.boundary.speed=next.desired_speed=spawn->speed;
    next.lateral.current=next.lateral.target=spawn->lateral;
    next.boundary.damage.flags|=UINT32_C(0x80000000);
    nm_road_pose_t pose;
    if(!nm_road_pose_step(road->profiles+spawn->profile,&next.lateral,&pose)) return 0;
    for(unsigned a=0;a<3;++a) next.boundary.ground.position[a]=pose.xyz[a];
    next.boundary.ground.pitch=nm_yaw_angle((uint32_t)pose.angles[0]);
    next.body_yaw=next.road_yaw=next.boundary.heading=nm_yaw_angle((uint32_t)pose.angles[1]);
    next.boundary.ground.roll=nm_yaw_angle((uint32_t)pose.angles[2]);
    next.render_angles[0]=next.boundary.ground.pitch;
    next.render_angles[1]=next.boundary.ground.yaw=next.body_yaw;
    next.render_angles[2]=nm_yaw_angle((uint32_t)((int)next.boundary.ground.roll+next.render_roll_offset));
    next.phase[0]=nm_yaw_angle((uint32_t)((int)next.phase[0]+1));
    next.path_offset=(uint16_t)((uint32_t)(int32_t)next.phase[0]*28u);
    next.boundary.damage.overlay_model=0;
    for(unsigned a=0;a<3;++a) next.boundary.damage.angle[a]=0;
    next.resistance=0;next.filtered_pitch=next.render_yaw_offset=next.bank=0;
    next.inhibit_angle=0;next.input_inhibit=next.inhibit_direction=0;
    for(unsigned a=0;a<4;++a) next.surface_ticks[a]=0;
    next.force.trigger_count[0]=next.force.trigger_count[1]=0;
    next.response_ticks=0;next.boundary.condition=0;
    NM_COPY(state,&next);*reference_height=pose.xyz[1];return 1;
}

/* Return1 ordinary tick,2 stage transition,0 invalid. Events fire strictly
 * AFTER their time. A transition leaves cursor unchanged (source early RET).
 * Validate the due batch first; no per-frame allocation or full-car copies. */
static inline int nm_intro_events_step(const nm_intro_event_t *events, unsigned count,
        nm_player_t *cars, unsigned car_count, nm_intro_cursor_t *cursor, uint32_t *next_stage) {
    if(!events || !count || count>256 || !cars || !car_count || car_count>44 ||
       !cursor || cursor->key>=count || !next_stage) return 0;
    unsigned end=cursor->key;int transition=0;
    while(end<count && cursor->tick>events[end].tick) {
        const nm_intro_event_t *event=events+end;
        if(event->actor<0) { transition=1;break; }
        if((unsigned)event->actor>=car_count || !isfinite(event->speed)) return 0;
        ++end;
    }
    if(end==count) return 0; /* Pack must supply a transition or terminal wait. */
    for(unsigned i=cursor->key;i<end;++i) {
        cars[events[i].actor].desired_speed=events[i].speed;
        cars[events[i].actor].lateral.target=events[i].lateral;
    }
    *next_stage=transition ? events[end].next_stage : UINT32_MAX;
    if(!transition) { cursor->key=end;++cursor->tick; }
    return transition ? 2 : 1;
}

/*11438..115e0 autonomous speed controller. No throttle/RPM/normal-player
 * force model. Parameters are prepared numeric data, not target ROM reads. */
static inline int nm_intro_speed_step(const nm_drive_params_t *p,
        const nm_intro_speed_params_t *params, uint32_t flags, nm_player_t *state) {
    if(!p || !params || !state) return 0;
    if(!(flags&UINT32_C(0x15400088))) return 1;
    if(state->speed_index<0 || state->speed_index>=512 ||
       !isfinite(state->desired_speed) || !isfinite(state->boundary.speed) ||
       !isfinite(state->resistance)) return 0;
#if defined(__DREAMCAST__) && defined(NM_GAMEPLAY_SH4ZAM)
    float limit=shz_divf(state->desired_speed,params->desired_divisor);
#else
    float limit=nm_force_divide(p,state->desired_speed,params->desired_divisor);
#endif
    limit=limit*params->response[state->speed_index];
    limit=limit-p->drag[0][state->speed_index];
    limit=limit-state->resistance;
    limit=limit-params->drag_offset;
    limit=limit*p->acceleration_scale;
    limit=limit*0.5f;
    float delta=state->desired_speed-state->boundary.speed;
    if(delta>limit) delta=limit;
    else if(delta<=params->minimum_delta) delta=params->minimum_delta;
    float speed=delta+state->boundary.speed;
    if(nm_force_bits(speed)&UINT32_C(0x80000000)) speed=0;
    int32_t index;
    if(!isfinite(limit) || !isfinite(delta) || !isfinite(speed) ||
       !nm_drive_round_i32(speed*p->speed_index_scale,&index)) return 0;
    state->boundary.speed=speed;state->force.longitudinal_delta=delta;state->speed_index=index;
    return 1;
}

/*cde0..cdec: ordered road resistance, speed control and normal guidance.
 *Refresh the lane snapshot AFTER acceleration; callers supply neighbors as
 *they exist at this car's turn, not a frame-wide snapshot of the whole field. */
/* In-place core of nm_opponent_motion_step: same arithmetic and order, but *state may be
 * left partly updated when it returns 0. Only for callers that already hold
 * a private copy (or abort the scene) on failure. */
static inline int nm_opponent_motion_step_inplace(const nm_drive_params_t *p,
        const nm_intro_speed_params_t *params, const nm_path_pack_t *path,
        const nm_road_profile_t *profile, const nm_player_dynamics_input_t *in,
        const nm_guided_race_input_t *race, uint32_t mark_timer, int8_t mark,
        uint8_t *remembered, nm_player_t *state) {
    if(!state || !remembered || !in || !race || !race->car) return 0;
    nm_player_t *const next=state;
    uint8_t selected=*remembered;
    nm_resistance_state_t resistance={next->resistance,next->boundary.damage.override_count};
    if(!nm_resistance_marked_step(p,mark_timer,mark,&resistance)) return 0;
    next->resistance=resistance.resistance;next->boundary.damage.override_count=resistance.override_count;
    if(!nm_intro_speed_step(p,params,in->speed_flags,next)) return 0;
    nm_player_dynamics_input_t dynamics=*in;dynamics.speed_index=next->speed_index;
    nm_guided_lane_car_t car=*race->car;car.speed=next->speed_index;
    nm_guided_race_input_t context=*race;context.car=&car;
    if(!nm_car_guided_motion(p,path,profile,&dynamics,&context,&selected,next)) return 0;
    *remembered=selected;return 1;
}
static inline int nm_opponent_motion_step(const nm_drive_params_t *p,
        const nm_intro_speed_params_t *params, const nm_path_pack_t *path,
        const nm_road_profile_t *profile, const nm_player_dynamics_input_t *in,
        const nm_guided_race_input_t *race, uint32_t mark_timer, int8_t mark,
        uint8_t *remembered, nm_player_t *state) {
    if(!state) return 0;
    nm_player_t copy; NM_COPY(&copy,state);
    const int result=nm_opponent_motion_step_inplace(p,params,path,profile,in,race,mark_timer,mark,remembered,&copy);
    if(!result) return 0;
    NM_COPY(state,&copy);return result;
}

/* d290..d29c (or d264..d270): speed, distinct wheel phase, shared11150.
 * Remaining attract callbacks, road bookkeeping and rendering are not folded
 * into this prefix. A late path failure must not publish speed/wheel changes. */
/* In-place core of nm_intro_motion_prefix: same arithmetic and order, but *state may be
 * left partly updated when it returns 0. Only for callers that already hold
 * a private copy (or abort the scene) on failure. */
static inline int nm_intro_motion_prefix_inplace(const nm_drive_params_t *p,
        const nm_intro_speed_params_t *params, const nm_path_pack_t *path,
        const nm_road_profile_t *profile, uint32_t flags, int player_wheels,
        nm_player_t *state) {
    if(!state || (player_wheels!=0 && player_wheels!=1)) return 0;
    nm_player_t *const next=state;
    if(!nm_intro_speed_step(p,params,flags,next)) return 0;
    int32_t phase=nm_gears_signed((uint32_t)next->speed_index<<(player_wheels ? 6 : 5));
    if(phase<0) phase=0;
    else if(phase>0xaaa) phase=0xaaa;
    next->wheel_phase[0]=nm_yaw_angle((uint32_t)(int32_t)next->wheel_phase[0]-(uint32_t)phase);
    if(player_wheels) next->wheel_phase[1]=next->wheel_phase[0];
    if(!nm_player_rolling_path_step(p,path,profile,next)) return 0;
    return 1;
}
static inline int nm_intro_motion_prefix(const nm_drive_params_t *p,
        const nm_intro_speed_params_t *params, const nm_path_pack_t *path,
        const nm_road_profile_t *profile, uint32_t flags, int player_wheels,
        nm_player_t *state) {
    if(!state) return 0;
    nm_player_t copy; NM_COPY(&copy,state);
    const int result=nm_intro_motion_prefix_inplace(p,params,path,profile,flags,player_wheels,&copy);
    if(!result) return 0;
    NM_COPY(state,&copy);return result;
}

/*1365c: attract cars use map progression, not the player's checkpoint awards.
 * Reuse the offline-folded flags. Clock is shared, lap bookkeeping is per car. */
static inline int nm_intro_road_step(const nm_road_pack_t *road, nm_player_t *state) {
    if(!road || !state || road->profile_count<2 || road->profile_count>4097) return 0;
    nm_road_selection_t selected={state->boundary.ground.road,state->path_offset,
        (uint16_t)state->phase[0],(uint16_t)state->phase[1],state->path_flags,state->previous_path_flags};
    if(!nm_road_select(road,NULL,NULL,0,0,0,0,&selected)) return 0;
    const int delta=(int)selected.previous_phase-selected.phase;
    if(delta>0 && (uint32_t)delta>road->profile_count-1u-7u) {
        state->lap.lap=(uint8_t)(state->lap.lap+1u);
        state->lap.completed=state->lap.lap;
        state->lap.last_ticks=(uint16_t)(state->checkpoints.clock-state->lap.start_ticks);
        state->lap.start_ticks=(uint16_t)state->checkpoints.clock;
    }
    state->phase[0]=nm_yaw_angle(selected.phase);state->phase[1]=nm_yaw_angle(selected.previous_phase);
    state->path_flags=selected.flags;state->previous_path_flags=selected.previous_flags;
    return 1;
}

/*15678: original SHLO16 then SHLI16 yields ZERO (not a signed16 right
 *shift). Preserve its road-heading stores and always-bit16 final request;
 *do not invent the unreachable crash/heading-reflection branch. */
/* In-place core of nm_opponent_road_trigger: same arithmetic and order, but *state may be
 * left partly updated when it returns 0. Only for callers that already hold
 * a private copy (or abort the scene) on failure. */
static inline int nm_opponent_road_trigger_inplace(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, uint8_t mode, float player_speed, nm_player_t *state) {
    if(!p || !contact || !state) return 0;
    nm_player_t *const next=state;
    const int first=next->speed_index>p->opponent_road_threshold[0][mode] && (next->boundary.ground.combined_mask&32);
    const int second=next->speed_index>p->opponent_road_threshold[1][mode];
    if(first || second) {
        if(!contact->quads || next->boundary.ground.road>=contact->quad_count) return 0;
        next->boundary.ground.road_heading=nm_yaw_angle(contact->quads[next->boundary.ground.road].heading);
    }
    if(second) {
        if(!isfinite(player_speed)) return 0;
        if(nm_gears_signed(nm_force_bits(player_speed))>=nm_gears_signed(nm_force_bits(p->collision_speed_step))) {
            const float drop=p->collision_speed_step*50.0f;
            next->boundary.speed=player_speed-drop;
            if(!isfinite(next->boundary.speed)) return 0;
        }
        next->boundary.damage.flags|=UINT32_C(0x10000);
    }
    return 1;
}
static inline int nm_opponent_road_trigger(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, uint8_t mode, float player_speed, nm_player_t *state) {
    if(!state) return 0;
    nm_player_t copy; NM_COPY(&copy,state);
    const int result=nm_opponent_road_trigger_inplace(p,contact,mode,player_speed,&copy);
    if(!result) return 0;
    NM_COPY(state,&copy);return result;
}

typedef struct {
    uint16_t sampled_tick;
    float sampled_height, sampled_velocity, reference_height, velocity;
} nm_opponent_height_t;

/*cb60: contact-height damping, distinct from the four-wheel suspension.
 *Keep source operation order: a fused lerp would change its two force terms. */
/* In-place core of nm_opponent_height_step: same arithmetic and order, but *state may be
 * left partly updated when it returns 0. Only for callers that already hold
 * a private copy (or abort the scene) on failure. */
static inline int nm_opponent_height_step_inplace(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, uint16_t tick,
        nm_opponent_height_t *height, nm_player_t *state) {
    if(!p || !contact || !height || !state) return 0;
    nm_opponent_height_t h=*height;
    nm_player_t *const next=state;
    nm_wheels_state_t *g=&next->boundary.ground;
    nm_contact_result_t hit={g->position[1],g->road,0,0};
    if(!nm_contact_query(contact,p,g->position[0],g->position[2],&hit)) return 0;
    g->combined_mask=(uint16_t)hit.mask; /*cb60 does NOT publish the returned road. */
    const uint32_t elapsed=(uint32_t)tick-h.sampled_tick;
    h.sampled_tick=tick;
    if(elapsed>=5) {
        h.sampled_height=h.reference_height=hit.height;
        h.sampled_velocity=h.velocity;
    } else {
        float velocity=hit.height-h.sampled_height;
        h.sampled_height=hit.height;
        if(elapsed!=1) {
#if defined(__DREAMCAST__) && defined(NM_GAMEPLAY_SH4ZAM)
            /* TGP command3 defines zero by exponent wrapping, not IEEE /
             *zero. Preserve that exceptional case; ordinary gaps use SH4ZAM. */
            velocity=elapsed ? shz_divf(velocity,(float)elapsed) : nm_force_divide(p,velocity,0.0f);
#else
            velocity=nm_force_divide(p,velocity,(float)elapsed);
#endif
        }
        h.sampled_velocity=velocity;
    }
    const uint16_t correction=(uint8_t)next->correction_ticks | ((uint16_t)next->surface_phase<<8);
    if(correction<=24 && contact->slot>=3) return 0;
    const unsigned index=correction<=24 ? contact->slot*7+(correction>>2) : 21;
    const float *coeff=p->opponent_height_coefficients[index];
    const float force=coeff[0]*(h.sampled_velocity-h.velocity);
    const float displacement=hit.height-h.reference_height;
    const float error=displacement-h.velocity;
    if(!isfinite(force) || !isfinite(error)) return 0;
    if((nm_force_bits(error)&0x7fffffffu)>0x3f800000u) {
        h.velocity=h.sampled_velocity;
        h.reference_height=hit.height;
    } else {
        const float correction_force=coeff[1]*error;
        const float acceleration=correction_force+force;
        h.velocity=acceleration+h.velocity;
        h.reference_height=h.velocity+h.reference_height;
    }
    if(!isfinite(h.velocity) || !isfinite(h.reference_height)) return 0;
    g->position[1]=h.reference_height;
    *height=h;return 1;
}
static inline int nm_opponent_height_step(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, uint16_t tick,
        nm_opponent_height_t *height, nm_player_t *state) {
    if(!state) return 0;
    nm_player_t copy; NM_COPY(&copy,state);
    const int result=nm_opponent_height_step_inplace(p,contact,tick,height,&copy);
    if(!result) return 0;
    NM_COPY(state,&copy);return result;
}

/*157e8: per-car contact age plus shared-event gate. Return2 requests event4
 *and shared timer320; caller publishes those only after a successful update. */
/* In-place core of nm_opponent_road_gate: same arithmetic and order, but *state may be
 * left partly updated when it returns 0. Only for callers that already hold
 * a private copy (or abort the scene) on failure. */
static inline int nm_opponent_road_gate_inplace(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, uint8_t mode, uint8_t guided_mode,
        uint8_t slot, int16_t event_timer, const nm_player_t *player,
        uint8_t *contact_age, nm_player_t *state) {
    if(!p || !contact || !player || !contact_age || !state) return 0;
    nm_player_t *const next=state;
    int result=1;
    if((next->boundary.ground.combined_mask&44) && !next->boundary.damage.inhibit_ticks && !next->input_inhibit) {
        if(!nm_opponent_road_trigger_inplace(p,contact,mode,player->boundary.speed,next)) return 0;
    } else {
        const int lap=next->lap.completed<128 ? next->lap.completed : (int)next->lap.completed-256;
        const int delta=(int)next->phase[0]-player->phase[0];
        if(event_timer<=0 && guided_mode<=2 && !mode && lap>(slot ? 1 : 3) &&
           !next->input_inhibit && !(next->boundary.ground.road&126) && delta>0 && delta<12) {
            next->boundary.damage.flags|=UINT32_C(0x8000);
            result=2;
        }
    }
    if(*contact_age!=127) ++*contact_age;
    return result;
}
static inline int nm_opponent_road_gate(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, uint8_t mode, uint8_t guided_mode,
        uint8_t slot, int16_t event_timer, const nm_player_t *player,
        uint8_t *contact_age, nm_player_t *state) {
    if(!state) return 0;
    nm_player_t copy; NM_COPY(&copy,state);
    const int result=nm_opponent_road_gate_inplace(p,contact,mode,guided_mode,slot,event_timer,player,contact_age,&copy);
    if(!result) return 0;
    NM_COPY(state,&copy);return result;
}

/*15a04 gameplay effects. Return2 when caller must publish the current actor
 *as shared inhibit owner (5014e4); audio intent15b90 remains external/silent.
 *Active table index uses the WIDE sum before signed-byte wrapping. */
/* In-place core of nm_opponent_inhibit_step: same arithmetic and order, but *state may be
 * left partly updated when it returns 0. Only for callers that already hold
 * a private copy (or abort the scene) on failure. */
static inline int nm_opponent_inhibit_step_inplace(const nm_drive_params_t *p, uint32_t clock, nm_player_t *state) {
    if(!p || !state) return 0;
    nm_player_t *const next=state;
    if(next->input_inhibit) {
        const int index=(int)next->input_inhibit+next->inhibit_direction;
        next->input_inhibit=(int8_t)(index < -128 ? index+256 : index>127 ? index-256 : index);
        next->inhibit_angle=p->opponent_inhibit_angles[index+256];
        const int angle=next->inhibit_angle<0 ? -(int)next->inhibit_angle : next->inhibit_angle;
        const float product=nm_force_float(0x384cccaa)*(float)angle;
        next->inhibit_scale=product-nm_force_float(0x3f59999a);
        const float speed=next->boundary.speed-p->inhibit_speed_drop;
        if(!isfinite(next->inhibit_scale) || !isfinite(speed)) return 0;
        if(!(nm_force_bits(speed)>>31)) next->boundary.speed=speed;
        next->boundary.damage.flags&=~UINT32_C(0x18000);
    } else if(nm_gears_signed(clock)<=1920 || (uint8_t)next->boundary.damage.inhibit_ticks) {
        next->boundary.damage.flags&=~UINT32_C(0x18000);
    } else {
        const uint32_t flags=next->boundary.damage.flags;
        next->boundary.damage.flags&=~UINT32_C(0x10000);
        if(flags&0x10000) { next->input_inhibit=96;next->inhibit_direction=-1; }
        else {
            next->boundary.damage.flags&=~UINT32_C(0x8000);
            if(flags&0x8000) { next->input_inhibit=-96;next->inhibit_direction=1; }
            else { return 1; }
        }
        next->boundary.damage.override_count=128;
        return 2;
    }
    return 1;
}
static inline int nm_opponent_inhibit_step(const nm_drive_params_t *p, uint32_t clock, nm_player_t *state) {
    if(!state) return 0;
    nm_player_t copy; NM_COPY(&copy,state);
    const int result=nm_opponent_inhibit_step_inplace(p,clock,&copy);
    if(!result) return 0;
    NM_COPY(state,&copy);return result;
}

/*ce74..cebc: signed recovery timer, optional lateral remeasure, direct body
 *angles, map/lap, scenery and rank. No player road marks/path-offset update. */
/* In-place core of nm_opponent_road_tail: same arithmetic and order, but *state may be
 * left partly updated when it returns 0. Only for callers that already hold
 * a private copy (or abort the scene) on failure. */
static inline int nm_opponent_road_tail_inplace(const nm_drive_params_t *p,
        const nm_road_pack_t *road, nm_player_t *state) {
    if(!p || !road || !road->profiles || !state || (uint16_t)state->phase[0]>=road->profile_count) return 0;
    nm_player_t *const next=state;
    const uint8_t old=next->boundary.damage.initial_inhibit;
    if(old) {
        next->boundary.damage.initial_inhibit=(uint8_t)(old-1u);
        if(old==1) {
            if(!nm_road_measure_lateral(p,road->profiles+(uint16_t)next->phase[0],
                next->boundary.ground.position[0],next->boundary.ground.position[2],&next->lateral.current)) return 0;
            next->lateral.control=448;
        }
    }
    next->render_angles[0]=next->boundary.ground.pitch;
    next->render_angles[1]=next->boundary.ground.yaw=next->body_yaw;
    next->render_angles[2]=next->boundary.ground.roll;
    if(!nm_intro_road_step(road,next) ||
       !nm_contact_scenery_cell(next->boundary.ground.position[0],next->boundary.ground.position[2],&next->scenery_cell)) return 0;
    const int lap=next->lap.lap<128 ? next->lap.lap : (int)next->lap.lap-256;
    next->rank_key=nm_yaw_angle((uint32_t)(lap*(int)(road->profile_count-1)+next->phase[0]));
    return 1;
}
static inline int nm_opponent_road_tail(const nm_drive_params_t *p,
        const nm_road_pack_t *road, nm_player_t *state) {
    if(!state) return 0;
    nm_player_t copy; NM_COPY(&copy,state);
    const int result=nm_opponent_road_tail_inplace(p,road,&copy);
    if(!result) return 0;
    NM_COPY(state,&copy);return result;
}

/*d6f8..d7f8 race opponent rolling update. Return2 on release to normal opponent logic,
 *1 while rolling,0 atomically on invalid input. Speed follows the player only
 *AFTER movement; wheels use the new speed and the OLD visibility decision. */
/* In-place core of nm_opponent_rolling_step: same arithmetic and order, but *state may be
 * left partly updated when it returns 0. Only for callers that already hold
 * a private copy (or abort the scene) on failure. */
static inline int nm_opponent_rolling_step_inplace(const nm_drive_params_t *p,
        const nm_road_pack_t *road, const nm_path_pack_t *path,
        unsigned id, float player_speed, uint32_t mark_timer, int8_t mark,
        int visible, nm_player_t *state) {
    if(!p || !state || !road || !road->profiles || !path || !id || id>=44 ||
       road->slot!=path->slot || (uint16_t)state->phase[0]>=road->profile_count ||
       !isfinite(player_speed) || (visible!=0 && visible!=1)) return 0;
    nm_player_t *const next=state;
    next->boundary.damage.flags&=~UINT32_C(0x18000);
    nm_resistance_state_t resistance={next->resistance,next->boundary.damage.override_count};
    if(!nm_resistance_marked_step(p,mark_timer,mark,&resistance) ||
       !nm_player_rolling_path_step(p,path,road->profiles+(uint16_t)next->phase[0],next)) return 0;
    next->resistance=resistance.resistance;next->boundary.damage.override_count=resistance.override_count;
    /* Original ROM28b575c, exact binary32 rather than a rounded decimal. */
    next->boundary.speed=id&1u ? player_speed-nm_force_float(UINT32_C(0x3cd55555)) : player_speed;
    if(!nm_drive_round_i32(next->boundary.speed*p->speed_index_scale,&next->speed_index)) return 0;
    next->render_angles[0]=next->boundary.ground.pitch;
    next->render_angles[1]=next->boundary.ground.yaw=next->body_yaw;
    next->render_angles[2]=next->boundary.ground.roll;
    if(visible) {
        int32_t phase=nm_gears_signed((uint32_t)next->speed_index<<5);
        if(phase<0) phase=0;
        else if(phase>0xaaa) phase=0xaaa;
        next->wheel_phase[0]=nm_yaw_angle((uint32_t)(int32_t)next->wheel_phase[0]-(uint32_t)phase);
    }
    next->boundary.damage.initial_inhibit=0;
    if(!nm_intro_road_step(road,next) ||
       !nm_contact_scenery_cell(next->boundary.ground.position[0],next->boundary.ground.position[2],&next->scenery_cell)) return 0;
    const int lap=next->lap.lap<128 ? next->lap.lap : (int)next->lap.lap-256;
    next->rank_key=nm_yaw_angle((uint32_t)(lap*(int)(road->profile_count-1)+next->phase[0]));
    const int released=next->phase[0]==0;
    if(released) { next->lap.completed=next->lap.lap=0;next->lateral.control=448; }
    return released ? 2 : 1;
}
static inline int nm_opponent_rolling_step(const nm_drive_params_t *p,
        const nm_road_pack_t *road, const nm_path_pack_t *path,
        unsigned id, float player_speed, uint32_t mark_timer, int8_t mark,
        int visible, nm_player_t *state) {
    if(!state) return 0;
    nm_player_t copy; NM_COPY(&copy,state);
    const int result=nm_opponent_rolling_step_inplace(p,road,path,id,player_speed,mark_timer,mark,visible,&copy);
    if(!result) return 0;
    NM_COPY(state,&copy);return result;
}

/*d290..d2a4 / d264..d278: ordered movement, road progression, scenery cell.
 * Profile is selected BEFORE movement, as in the original per-car update.
 * Collision correction/model callbacks still follow this block. */
/* In-place core of nm_intro_drive_step_profile: same arithmetic and order, but *state may be
 * left partly updated when it returns 0. Only for callers that already hold
 * a private copy (or abort the scene) on failure. */
static inline int nm_intro_drive_step_profile_inplace(const nm_drive_params_t *p,
        const nm_intro_speed_params_t *params, const nm_road_pack_t *road,
        const nm_path_pack_t *path, const nm_road_profile_t *profile,
        uint32_t flags, int player_wheels, nm_player_t *state) {
    if(!state || !road || !profile || !path || road->slot!=path->slot) return 0;
    nm_player_t *const next=state;
    const uint8_t lap=next->lap.lap;
    if(!nm_intro_motion_prefix_inplace(p,params,path,profile,flags,player_wheels,next) ||
       !nm_intro_road_step(road,next) ||
       !nm_contact_scenery_cell(next->boundary.ground.position[0],next->boundary.ground.position[2],&next->scenery_cell)) return 0;
    /* A cinematic car's +6d is the original's +6e after the line: its spawns (cf74/cfc0) write 255 where the
     * native's write 0, so the first crossing leaves 0 there (attract trace: cars 17 and 19 from 5010b0 509). */
    if(next->lap.lap!=lap) next->lap.completed=(uint8_t)(next->lap.lap-1u);
    return 1;
}
static inline int nm_intro_drive_step_profile(const nm_drive_params_t *p,
        const nm_intro_speed_params_t *params, const nm_road_pack_t *road,
        const nm_path_pack_t *path, const nm_road_profile_t *profile,
        uint32_t flags, int player_wheels, nm_player_t *state) {
    if(!state) return 0;
    nm_player_t copy; NM_COPY(&copy,state);
    const int result=nm_intro_drive_step_profile_inplace(p,params,road,path,profile,flags,player_wheels,&copy);
    if(!result) return 0;
    NM_COPY(state,&copy);return result;
}
static inline int nm_intro_drive_step(const nm_drive_params_t *p,
        const nm_intro_speed_params_t *params, const nm_road_pack_t *road,
        const nm_path_pack_t *path, uint32_t flags, int player_wheels, nm_player_t *state) {
    if(!state || !road || !road->profiles || (uint16_t)state->phase[0]>=road->profile_count) return 0;
    return nm_intro_drive_step_profile(p,params,road,path,road->profiles+(uint16_t)state->phase[0],
        flags,player_wheels,state);
}

/*e78c: temporary center contact after an impact. Source20d is read SIGNED;
 * zero and128..255 do nothing. Do not probe four wheels or overwrite angles. */
static inline int nm_intro_contact_step(const nm_contact_pack_t *contact,
        const nm_drive_params_t *p, nm_player_t *state) {
    if(!state) return 0;
    const unsigned ticks=state->boundary.damage.initial_inhibit;
    if(!ticks || ticks>=128) return 1;
    nm_contact_result_t result={state->boundary.ground.position[1],state->boundary.ground.road,0,0};
    if(!nm_contact_query(contact,p,state->boundary.ground.position[0],state->boundary.ground.position[2],&result)) return 0;
    state->boundary.damage.initial_inhibit=(uint8_t)(ticks-1);
    state->boundary.ground.position[1]=result.height;
    state->boundary.ground.road=result.road;state->boundary.ground.combined_mask=(uint16_t)result.mask;
    return 1;
}

/*153c4: attract collision displacement plus wrapped heading relaxation.
 * The impulse system owns other_yaw/own_yaw (source212/216). Unlike15314,
 * heading changes here; the unclamped step updates own_yaw before clamping. */
/* In-place core of nm_intro_correction_step: same arithmetic and order, but *state may be
 * left partly updated when it returns 0. Only for callers that already hold
 * a private copy (or abort the scene) on failure. */
static inline int nm_intro_correction_step_inplace(nm_player_t *state, int16_t other_yaw, int16_t *own_yaw) {
    if(!state || !own_yaw) return 0;
    if(!state->correction_ticks) return 1;
    nm_player_t *const next=state;
    const int ticks=(int)next->correction_ticks-1;
    next->correction_ticks=ticks < -128 ? 127 : (int8_t)ticks;
    if(ticks==1) next->correction[0]=next->correction[1]=0;
    const int16_t difference=nm_yaw_angle((uint32_t)((int)other_yaw-*own_yaw));
    int step=nm_guided_shift((uint32_t)(int32_t)difference,3);
    const int16_t yaw=nm_yaw_angle((uint32_t)((int)*own_yaw+step));
    if(step < -64) step=-64;
    else if(step > 64) step=64;
    next->boundary.heading=nm_yaw_angle((uint32_t)((int)next->boundary.heading+step));
#ifdef __DREAMCAST__
    const shz_vec2_t correction=shz_vec2_init(next->correction[0],next->correction[1]);
    const shz_vec2_t position=shz_vec2_add(correction,
        shz_vec2_init(next->boundary.ground.position[0],next->boundary.ground.position[2]));
    const shz_vec2_t decay=shz_vec2_scale(correction,0.75f);
    next->boundary.ground.position[0]=position.x;next->boundary.ground.position[2]=position.y;
    next->correction[0]=decay.x;next->correction[1]=decay.y;
#else
    for(unsigned a=0;a<2;++a) {
        next->boundary.ground.position[2*a]=next->correction[a]+next->boundary.ground.position[2*a];
        next->correction[a]=0.75f*next->correction[a];
    }
#endif
    for(unsigned a=0;a<2;++a)
        if(!isfinite(next->boundary.ground.position[2*a]) || !isfinite(next->correction[a])) return 0;
    next->boundary.damage.flags|=1u<<10;
    *own_yaw=yaw;return 1;
}
static inline int nm_intro_correction_step(nm_player_t *state, int16_t other_yaw, int16_t *own_yaw) {
    if(!state) return 0;
    nm_player_t copy; NM_COPY(&copy,state);
    const int result=nm_intro_correction_step_inplace(&copy,other_yaw,own_yaw);
    if(!result) return 0;
    NM_COPY(state,&copy);return result;
}

typedef struct {
    const nm_player_t *player;
    const nm_crash_row_t *crash_rows;
    unsigned crash_count;
    uint16_t tick;
    int16_t event_timer, other_yaw;
    uint8_t id, mode, guided_mode, visible;
    int8_t relative;
} nm_opponent_finish_input_t;
typedef struct {
    nm_opponent_height_t height;
    int16_t correction_yaw;
    uint8_t contact_age;
    uint8_t requests; /* Bit0: publish inhibit owner; bit1: event4/timer320. */
} nm_opponent_finish_state_t;

/*cdec..cebc. Visibility uses the cell from BEFORE motion. The caller shares
 *damage.sequence_counter and checkpoint clock between cars, not per frame. */
/* In-place core of nm_opponent_finish_step: same arithmetic and order, but *state may be
 * left partly updated when it returns 0. Only for callers that already hold
 * a private copy (or abort the scene) on failure. */
static inline int nm_opponent_finish_step_inplace(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, const nm_road_pack_t *road,
        const nm_opponent_finish_input_t *in, nm_opponent_finish_state_t *effects,
        nm_player_t *state) {
    if(!p || !contact || !road || !in || !effects || !state || !in->player ||
       !in->id || in->id>=44 || in->visible>1) return 0;
    nm_player_t *const next=state;
    nm_opponent_finish_state_t result=*effects;result.requests=0;
    if(in->visible) {
        int32_t phase=nm_gears_signed((uint32_t)next->speed_index<<5);
        if(phase<0) phase=0;
        else if(phase>0xaaa) phase=0xaaa;
        next->wheel_phase[0]=nm_yaw_angle((uint32_t)(int32_t)next->wheel_phase[0]-(uint32_t)phase);
        if(!nm_intro_crash_step(in->crash_rows,in->crash_count,in->id,next->input_inhibit,&next->boundary.damage)) return 0;
        const int inhibit=nm_opponent_inhibit_step_inplace(p,next->checkpoints.clock,next);
        if(!inhibit) return 0;
        result.requests|=inhibit==2 ? 1 : 0;
        const int gate=nm_opponent_road_gate_inplace(p,contact,in->mode,in->guided_mode,(uint8_t)road->slot,
            in->event_timer,in->player,&result.contact_age,next);
        if(!gate || !nm_intro_correction_step_inplace(next,in->other_yaw,&result.correction_yaw)) return 0;
        result.requests|=gate==2 ? 2 : 0;
        if((uint32_t)(int32_t)in->relative<=4 || (next->boundary.damage.flags&1024)) {
            if(!nm_opponent_height_step_inplace(p,contact,in->tick,&result.height,next)) return 0;
        } else {
            result.height.velocity=next->boundary.ground.position[1]-result.height.reference_height;
            result.height.reference_height=next->boundary.ground.position[1];
            if(!isfinite(result.height.velocity) || !isfinite(result.height.reference_height)) return 0;
        }
    } else next->correction_ticks=0;
    if(!nm_opponent_road_tail_inplace(p,road,next)) return 0;
    *effects=result;return 1;
}
static inline int nm_opponent_finish_step(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, const nm_road_pack_t *road,
        const nm_opponent_finish_input_t *in, nm_opponent_finish_state_t *effects,
        nm_player_t *state) {
    if(!state) return 0;
    nm_player_t copy; NM_COPY(&copy,state);
    const int result=nm_opponent_finish_step_inplace(p,contact,road,in,effects,&copy);
    if(!result) return 0;
    NM_COPY(state,&copy);return result;
}

/*e760: copy source body yaw, not the corrected steering heading. */
static inline void nm_intro_render_angles(nm_player_t *state) {
    state->render_angles[0]=state->boundary.ground.pitch;
    state->render_angles[1]=state->boundary.ground.yaw=state->body_yaw;
    state->render_angles[2]=nm_yaw_angle((uint32_t)((int)state->boundary.ground.roll+state->render_roll_offset));
}

/*d2a4..d2b0 / d278..d284: contact, impact correction, render angles.
 * Model/crash callbacks follow separately. Publish both outputs together. */
/* In-place core of nm_intro_pose_step: same arithmetic and order, but *state may be
 * left partly updated when it returns 0. Only for callers that already hold
 * a private copy (or abort the scene) on failure. */
static inline int nm_intro_pose_step_inplace(const nm_contact_pack_t *contact,
        const nm_drive_params_t *p, nm_player_t *state, int16_t other_yaw, int16_t *own_yaw) {
    if(!state || !own_yaw) return 0;
    nm_player_t *const next=state;int16_t yaw=*own_yaw;
    if(!nm_intro_contact_step(contact,p,next) || !nm_intro_correction_step_inplace(next,other_yaw,&yaw)) return 0;
    nm_intro_render_angles(next);
    *own_yaw=yaw;return 1;
}
static inline int nm_intro_pose_step(const nm_contact_pack_t *contact,
        const nm_drive_params_t *p, nm_player_t *state, int16_t other_yaw, int16_t *own_yaw) {
    if(!state) return 0;
    nm_player_t copy; NM_COPY(&copy,state);
    const int result=nm_intro_pose_step_inplace(contact,p,&copy,other_yaw,own_yaw);
    if(!result) return 0;
    NM_COPY(state,&copy);return result;
}

/*6200: current car translation/yaw/pitch/roll, not its cosmetic body angles.
 * XMTRX is caller-clobbered, as with existing presentation pose helpers. */
static inline int nm_intro_anchor_point(const nm_drive_params_t *p, int32_t index,
        const nm_intro_anchor_t *anchors, uint32_t count, const float point[3], float out[3]) {
    for(unsigned a=0;a<3;++a) if(!isfinite(point[a])) return 0;
    if(index==-1) { out[0]=point[0];out[1]=point[1];out[2]=point[2];return 1; }
    if(index<0 || (uint32_t)index>=count || !anchors) return 0;
#ifdef __DREAMCAST__
    alignas(32) shz_mat3x4_t matrix;
    if(!nm_body_pose(p,anchors[index].position,anchors[index].angles,matrix.elem)) return 0;
    shz_xmtrx_load_3x4(&matrix);
    const shz_vec3_t v=shz_xmtrx_transform_point3(shz_vec3_init(point[0],point[1],point[2]));
    out[0]=v.x;out[1]=v.y;out[2]=v.z;
#else
    float m[12];
    if(!nm_body_pose_reference(p,anchors[index].position,anchors[index].angles,m)) return 0;
    for(unsigned a=0;a<3;++a) out[a]=point[0]*m[a]+point[1]*m[3+a]+point[2]*m[6+a]+m[9+a];
#endif
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

/*60e8..61d4: source look-at yaw, then pitch in that yaw's vertical plane.
 * Keep quantized yaw for pitch; replacing it with horizontal length changes
 * the original arithmetic. SH4 approximations still need target validation. */
static inline int nm_intro_camera_angles(const nm_drive_params_t *p,
        const nm_intro_camera_t *camera, int16_t out[3]) {
    if(!p || !camera || !out) return 0;
    float delta[3];
#ifdef __DREAMCAST__
    const shz_vec3_t difference=shz_vec3_sub(
        shz_vec3_init(camera->target[0],camera->target[1],camera->target[2]),
        shz_vec3_init(camera->position[0],camera->position[1],camera->position[2]));
    delta[0]=difference.x;delta[1]=difference.y;delta[2]=difference.z;
#else
    for(unsigned i=0;i<3;++i) delta[i]=camera->target[i]-camera->position[i];
#endif
    for(unsigned i=0;i<3;++i) if(!isfinite(delta[i])) return 0;
    int32_t yaw,pitch;
#ifdef __DREAMCAST__
    /* SH4ZAM atan2 failed the source camera gate by 48 angle units.
     * Reuse the imported angle lookup; keep SH4ZAM vector/matrix work. */
    yaw=nm_force_angle(p,delta[2],delta[0]);
    const shz_sincos_t sc=shz_sincosu16((uint16_t)yaw);
    const float horizontal=shz_vec2_dot(shz_vec2_init(sc.sin,sc.cos),shz_vec2_init(delta[0],delta[2]));
    pitch=nm_force_angle(p,horizontal,delta[1]);
    if(yaw==INT32_MIN || pitch==INT32_MIN) return 0;
#else
    yaw=nm_force_angle(p,delta[2],delta[0]);
    const float horizontal=nm_drive_sine(p,(uint16_t)yaw)*delta[0]+
        nm_drive_sine(p,(uint16_t)(yaw+16384))*delta[2];
    pitch=nm_force_angle(p,horizontal,delta[1]);
    if(yaw==INT32_MIN || pitch==INT32_MIN) return 0;
#endif
    out[0]=nm_yaw_angle(0u-(uint32_t)pitch);
    out[1]=nm_yaw_angle((uint32_t)yaw);out[2]=camera->roll;
    return 1;
}

/* World-space cinematic mode1/501713=1: roll,pitch,yaw followed by -eye.
 * Not the car-relative mode1 variant or the normal follow camera. */
static inline int nm_intro_camera_view(const nm_drive_params_t *p,
        const nm_intro_camera_t *camera, float out[12], int16_t *heading,uint16_t *sky_scroll) {
    if(!out || !heading) return 0;
    int16_t angles[3];
    if(!nm_intro_camera_angles(p,camera,angles)) return 0;
    uint16_t sky=0;
    if(sky_scroll && !nm_camera_sky_scroll(p,camera->position[1],angles[0],camera->focus,
        nm_force_float(p->camera_reset_words[4]),&sky)) return 0;
    float result[12];
#ifdef __DREAMCAST__
    alignas(32) shz_mat3x4_t matrix;
    shz_xmtrx_init_rotation_yxz(nm_pose_radians((uint16_t)angles[1]),
        nm_pose_radians((uint16_t)angles[0]),nm_pose_radians((uint16_t)angles[2]));
    shz_xmtrx_transpose();
    const shz_vec3_t eye=shz_vec3_neg(shz_vec3_init(camera->position[0],camera->position[1],camera->position[2]));
    const shz_vec3_t translated=shz_xmtrx_transform_vec3(eye);
    shz_xmtrx_set_translation(translated.x,translated.y,translated.z);
    shz_xmtrx_store_3x4(&matrix);
    shz_memcpy4(result,matrix.elem,sizeof(result));
#else
    const float zero[3]={0};float rotation[12];int16_t inverse[3];
    for(unsigned i=0;i<3;++i) inverse[i]=nm_yaw_angle(0u-(uint32_t)(int32_t)angles[i]);
    if(!nm_body_pose_reference(p,zero,inverse,rotation)) return 0;
    for(unsigned r=0;r<3;++r) {
        for(unsigned c=0;c<3;++c) result[3*c+r]=rotation[3*r+c];
        result[9+r]=-(result[r]*camera->position[0]+result[3+r]*camera->position[1]+result[6+r]*camera->position[2]);
    }
#endif
    for(unsigned i=0;i<12;++i) if(!isfinite(result[i])) return 0;
#ifdef __DREAMCAST__
    shz_memcpy4(out,result,sizeof(result));
#else
    nm_memcpy(out,result,sizeof(result));
#endif
    *heading=nm_yaw_angle(0u-(uint32_t)(int32_t)angles[1]);
    if(sky_scroll) *sky_scroll=sky;
    return 1;
}
/*5d28..60e0: transform each endpoint using CURRENT anchor poses, then lerp.
 * Host path is the independent source-math oracle. SH4 presentation uses
 * library division/FMA interpolation; target tolerance validation is required
 * before connecting this helper to a shipping mode. Atomic output publication. */
static inline int nm_intro_camera_sample(const nm_drive_params_t *p,
        const nm_intro_key_t *a, const nm_intro_key_t *b,
        const nm_intro_anchor_t *anchors, uint32_t count,
        uint32_t elapsed, uint32_t duration, nm_intro_camera_t *out) {
    if(!p || !a || !b || !out || !duration || duration>16777216 || elapsed>=duration ||
       !isfinite(a->roll) || !isfinite(b->roll) || !isfinite(a->focus) || !isfinite(b->focus)) return 0;
    float pa[3],pb[3],ta[3],tb[3];
    if(!nm_intro_anchor_point(p,a->position_anchor,anchors,count,a->position,pa) ||
       !nm_intro_anchor_point(p,b->position_anchor,anchors,count,b->position,pb) ||
       !nm_intro_anchor_point(p,a->target_anchor,anchors,count,a->target,ta) ||
       !nm_intro_anchor_point(p,b->target_anchor,anchors,count,b->target,tb)) return 0;
    nm_intro_camera_t result={{0},{0},0,0,0};
    float roll;
#ifdef __DREAMCAST__
    const float t=shz_divf((float)elapsed,(float)duration);
    const shz_vec3_t position=shz_vec3_lerp(shz_vec3_init(pa[0],pa[1],pa[2]),shz_vec3_init(pb[0],pb[1],pb[2]),t);
    const shz_vec3_t target=shz_vec3_lerp(shz_vec3_init(ta[0],ta[1],ta[2]),shz_vec3_init(tb[0],tb[1],tb[2]),t);
    result.position[0]=position.x;result.position[1]=position.y;result.position[2]=position.z;
    result.target[0]=target.x;result.target[1]=target.y;result.target[2]=target.z;
    result.focus=shz_lerpf(a->focus,b->focus,t);roll=shz_lerpf(a->roll,b->roll,t);
#else
    const float t=nm_force_divide(p,(float)elapsed,(float)duration);
    for(unsigned i=0;i<3;++i) {
        result.position[i]=pa[i]+t*(pb[i]-pa[i]);
        result.target[i]=ta[i]+t*(tb[i]-ta[i]);
    }
    result.focus=a->focus+t*(b->focus-a->focus);roll=a->roll+t*(b->roll-a->roll);
#endif
    int32_t rounded;
    if(!nm_drive_round_i32(roll,&rounded) || !isfinite(result.focus)) return 0;
    result.roll=nm_yaw_angle((uint32_t)rounded);
    for(unsigned i=0;i<3;++i) if(!isfinite(result.position[i]) || !isfinite(result.target[i])) return 0;
    *out=result;return 1;
}
#endif
