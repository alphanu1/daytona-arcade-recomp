#ifndef DAYTONA_NATIVE_GUIDED_H
#define DAYTONA_NATIVE_GUIDED_H
#include "nm_mem.h"
#include "path.h"
#include "road.h"
#include "yaw.h"
#ifdef __DREAMCAST__
#include <sh4zam/shz_vector.h>
#endif
/* Car state uses the source-exact divide/length on every target. SH4ZAM's
 * reciprocal-square-root forms are approximate and moved the console away
 * from the original within one rolling start; NM_GAMEPLAY_SH4ZAM restores
 * them for timing comparisons only. */

typedef struct {
    float speed, scale;
    int32_t speed_index;
    uint32_t flags;
    uint16_t path_flags, surface;
    int8_t condition;
    uint8_t mode;
} nm_guided_player_input_t;
typedef struct {
    float position[3], delta[2], desired_speed, adjustment_target, adjustment;
    nm_path_cursor_t cursor;
    nm_road_lateral_t lateral;
    int16_t heading, body_yaw, pitch, roll, road_yaw, tire_angle;
    uint16_t ahead_offset;
    uint8_t profile_updates, remembered; /* Remembered native neighbor ID;255 none. */
} nm_guided_player_state_t;

/* Defined i960 arithmetic shift, including oversized/negative shift counts. */
static inline int32_t nm_guided_shift(uint32_t bits, uint32_t shift) {
    if (shift >= 32) return bits&0x80000000u ? -1 : 0;
    if (shift) bits = (bits>>shift) | (bits&0x80000000u ? (~0u << (32-shift)) : 0);
    int32_t result; nm_memcpy(&result,&bits,4); return result;
}

/*10f68..10fa4: directional quarter-step toward the player's heading.
 * The initial signed16 difference is WIDE, not wrapped around a full turn;
 * bit12 selects [-6144,0] instead of [0,6144]. Preserve arithmetic shift. */
static inline int16_t nm_guided_heading_bias(int16_t heading, int16_t player_heading, uint32_t flags) {
    const int delta=(int)player_heading-heading;
    if(flags&4096u ? (delta < -6144 || delta > 0) : (delta < 0 || delta > 6144)) return heading;
    return nm_yaw_angle((uint32_t)((int)heading+nm_guided_shift((uint32_t)delta,2)));
}

/*10dd8..10e68: target adjustment from live progress/rank, using offline tables. */
static inline int nm_guided_target(const nm_drive_params_t *p, unsigned mode,
        int16_t player_progress, int16_t progress, unsigned player_rank, unsigned rank, float *out) {
    if(!p || !out || mode>=256 || player_rank>=44 || rank>=44) return 0;
    int index=(int)player_progress-progress;
    if(index<p->guided_bounds[mode][0]) index=p->guided_bounds[mode][0];
    else if(index>p->guided_bounds[mode][1]) index=p->guided_bounds[mode][1];
    index+=p->guided_rank_bias[43+(int)rank-(int)player_rank];
    if(index < -63) index=-63;
    else if(index>63) index=63;
    const float target=p->guided_targets[index+63];
    if(!isfinite(target)) return 0;
    *out=target;return 1;
}

typedef struct {
    float position[2];
    int32_t speed;
    uint32_t flags;
    int16_t lateral, marker;
    uint8_t id;
    int8_t mode;
} nm_guided_lane_car_t;
typedef struct { int16_t target, control; uint8_t remembered; } nm_guided_lane_state_t;
static inline float nm_guided_lane_distance(const nm_drive_params_t *p,
        const nm_guided_lane_car_t *a, const nm_guided_lane_car_t *b) {
    const float dx=a->position[0]-b->position[0], dz=a->position[1]-b->position[1];
    if(!isfinite(dx) || !isfinite(dz)) return NAN;
#if defined(__DREAMCAST__) && defined(NM_GAMEPLAY_SH4ZAM)
    (void)p;
    return shz_sqrtf(shz_vec2_magnitude_sqr(shz_vec2_init(dx,dz)));
#else
    return nm_force_length(p,dx,dz);
#endif
}

/*136e8..139d4. remembered is a native car ID;255 means none. front/saved
 * are live linked-neighbor snapshots, not a nearest-neighbor approximation. */
static inline int nm_guided_lane_step(const nm_drive_params_t *p,
        const nm_guided_lane_car_t *player, const nm_guided_lane_car_t *car,
        const nm_guided_lane_car_t *front, const nm_guided_lane_car_t *saved,
        uint16_t row_lateral, uint8_t updates, nm_guided_lane_state_t *state) {
    if(!p || !player || !car || !state || car->id>=44 || player->id>=44) return 0;
    nm_guided_lane_state_t next=*state;
    if(player->id==car->id || (uint32_t)car->speed<=100) return 1;
    if((uint16_t)next.control<=224) next.target=nm_yaw_angle(row_lateral);
    if(updates>16 || !(car->flags&1024u)) { NM_COPY(state,&next);return 1; }
    int target;
    if(!car->mode) {
        if(!car->marker) { NM_COPY(state,&next);return 1; }
        target=player->lateral<car->lateral ? player->lateral+256*((int)car->marker+1) : player->lateral-256*(int)car->marker;
    } else if(car->flags&16384u) {
        if(player->speed>=car->speed) { next.target=car->lateral;next.control=224;NM_COPY(state,&next);return 1; }
        goto avoid;
    } else if(car->flags&8192u) {
        const unsigned lane=(uint16_t)player->lateral;
        const int pivot=(0xc3u>>((lane>>8)&31))&1u ? 896 : (uint16_t)car->lateral;
        target=(int)lane>pivot ? pivot-512 : (int)lane+512;
    } else {
        if(!front || front->id>=44) return 0;
        if(front->speed<car->speed) goto avoid;
        const float distance=nm_guided_lane_distance(p,car,front);
        if(!isfinite(distance)) return 0;
        if(distance<=10) { next.target=front->lateral;next.control=224; }
        NM_COPY(state,&next);return 1;
    }
    goto publish;
avoid:
    {
        const nm_guided_lane_car_t *other=front;
        int retained=0;
        if(saved) {
            if(saved->id>=44 || saved->id!=next.remembered) return 0;
            const float distance=nm_guided_lane_distance(p,car,saved);
            if(!isfinite(distance)) return 0;
            if(distance<6) { other=saved;retained=1; }
            else next.remembered=255;
        }
        if(!other || other->id>=44) return 0;
        if(!retained) {
            const float distance=nm_guided_lane_distance(p,car,other);
            if(!isfinite(distance)) return 0;
            if(distance>=20) { NM_COPY(state,&next);return 1; }
        }
        const int delta=(int)(uint16_t)other->lateral-(uint16_t)car->lateral;
        if(delta < -512 || delta>512) { NM_COPY(state,&next);return 1; }
        target=(uint16_t)other->lateral>896 ? 384 : (uint16_t)other->lateral+512;
        next.remembered=other->id;
    }
publish:
    if(target<0) target=0;
    else if(target>1792) target=1792;
    next.target=(int16_t)target;next.control=224;NM_COPY(state,&next);return 1;
}

typedef struct {
    const nm_guided_lane_car_t *player, *car, *front, *saved;
    int16_t player_progress, progress, player_heading;
    uint8_t player_rank, rank;
    float heading_circle[3]; /* Command40 snapshot: X,Z,radius, not current player position. */
} nm_guided_race_input_t;

/*10ccc shared movement; race supplies live opponents/linked neighbors.
 * A null race context retains the validated player/self specialization.
 * profile is the selected immutable road profile, required only on its branch.
 *136e8 returns on equal entity IDs;10f68's self-heading correction is zero.
 * Keep live speed and target/filter lag; this never plays back authored poses.
 * Out-of-range CVTZRI rejects atomically. Synthetic negative-zero speed reaches
 * this domain; actual-player reachability and exception behavior remain gates. */
static inline int nm_guided_step(const nm_path_pack_t *path, const nm_drive_params_t *p,
                                        const nm_road_profile_t *profile,
                                        const nm_guided_player_input_t *in, const nm_guided_race_input_t *race,
                                        nm_guided_player_state_t *state) {
    if (!path || !p || !in || !state || !isfinite(in->speed) || !isfinite(in->scale) ||
        !isfinite(state->position[1]) || !isfinite(state->adjustment_target) || !isfinite(state->adjustment)) return 0;
    if(race && (!race->player || !race->car || race->player_rank>=44 || race->rank>=44)) return 0;
    nm_guided_player_state_t next; NM_COPY(&next,state);
    if (!nm_path_cursor_step(path,p,state->position[0],state->position[2],0,&next.cursor) ||
        next.cursor.matched+1u >= path->records) return 0; /* 10cd0 reads the row after: a guard record. */
    const nm_path_row_t *row = path->rows+next.cursor.matched+1;
    nm_road_pose_t pose;
    int use_profile = 0;
    const uint32_t player_flags=race ? race->player->flags : in->flags;
    if (!(player_flags&0x10000000u) && in->condition <= 0 && !(in->path_flags&1024)) {
        if(race) {
            nm_guided_lane_state_t lane={next.lateral.target,next.lateral.control,next.remembered};
            if(!nm_guided_lane_step(p,race->player,race->car,race->front,race->saved,
                (uint16_t)row->lateral,next.profile_updates,&lane)) return 0;
            next.lateral.target=lane.target;next.lateral.control=lane.control;next.remembered=lane.remembered;
        }
        use_profile = next.lateral.control > 0;
        next.lateral.control = nm_yaw_angle((uint32_t)((int)next.lateral.control-1));
    }
    if (use_profile) {
        if (!nm_road_pose_step(profile,&next.lateral,&pose)) return 0;
        ++next.profile_updates;
    } else {
        nm_memcpy(pose.xyz,row->xyz,12);
        pose.angles[0] = row->pitch; pose.angles[1] = (int)row->yaw*4; pose.angles[2] = row->roll;
        next.lateral.current = row->lateral; next.lateral.step = 0;
    }
    next.ahead_offset = (uint16_t)(next.cursor.offset+56);
    float adjust = state->adjustment_target-state->adjustment;
    if (nm_force_bits(adjust)) {
        const float low = nm_force_float(nm_force_bits(p->guided_adjust_limit)|0x80000000u);
        if (adjust < low) adjust = low;
        else if (adjust > p->guided_adjust_limit) adjust = p->guided_adjust_limit;
        adjust = adjust+state->adjustment;
        next.adjustment = adjust;
    } else if(race) {
        if(!nm_guided_target(p,in->mode,race->player_progress,race->progress,race->player_rank,race->rank,&next.adjustment_target)) return 0;
    } else next.adjustment_target = p->guided_player_target[in->mode];
    next.desired_speed = row->speed*in->scale+adjust;
    float divisor = 32767.0f;
    int32_t ticks = 32767;
    const float dx = state->position[0]-pose.xyz[0], dz = pose.xyz[2]-state->position[2];
    if (!isfinite(dx) || !isfinite(dz)) return 0;
    if (nm_force_bits(in->speed)) {
#if defined(__DREAMCAST__) && defined(NM_GAMEPLAY_SH4ZAM)
        divisor = shz_divf(shz_sqrtf(shz_vec2_magnitude_sqr(shz_vec2_init(dx,dz))),in->speed);
#else
        divisor = nm_force_divide(p,nm_force_length(p,dx,dz),in->speed);
#endif
        if (!isfinite(divisor) || divisor < -2147483648.0f || divisor >= 2147483648.0f) return 0;
        ticks = (int32_t)divisor; /* CVTZRI truncates, unlike CVTRI. */
        if (ticks <= 4) { ticks = 4; divisor = 4; }
    }
    if(race) {
        const float cx=race->heading_circle[0]-state->position[0], cz=race->heading_circle[1]-state->position[2];
        if(!isfinite(cx) || !isfinite(cz) || !isfinite(race->heading_circle[2])) return 0;
#if defined(__DREAMCAST__) && defined(NM_GAMEPLAY_SH4ZAM)
        const float clearance=shz_sqrtf(shz_vec2_magnitude_sqr(shz_vec2_init(cx,cz)))-race->heading_circle[2];
#else
        const float clearance=nm_force_length(p,cx,cz)-race->heading_circle[2];
#endif
        if(!isfinite(clearance)) return 0;
        if(nm_force_bits(clearance)>>31) next.heading=nm_guided_heading_bias(next.heading,race->player_heading,race->car->flags);
    }
    if ((in->surface&33) || !in->condition) {
        if (in->surface&33) ticks = 16; /* Height keeps the ORIGINAL float divisor. */
        const int32_t angle = nm_force_angle(p,dz,dx);
        if (angle == INT32_MIN) return 0;
        const int delta = nm_yaw_angle((uint32_t)(angle-next.heading));
        next.heading = nm_yaw_angle((uint32_t)(next.heading+delta/ticks));
    }
    const int32_t shift = nm_guided_shift(334u-(uint32_t)in->speed_index,6);
    const int32_t tire = nm_guided_shift((uint32_t)(int32_t)row->tire,(uint32_t)shift);
    const int16_t tire_delta = nm_yaw_angle((uint32_t)(tire-state->tire_angle));
    next.tire_angle = nm_yaw_angle((uint32_t)(state->tire_angle+nm_guided_shift((uint32_t)(int32_t)tire_delta,3)));
    next.body_yaw = nm_yaw_angle((uint32_t)((int)next.heading-next.tire_angle));
    const int road_delta = nm_yaw_angle((uint32_t)(pose.angles[1]-state->road_yaw));
    next.road_yaw = nm_yaw_angle((uint32_t)(state->road_yaw+road_delta/ticks));
    next.pitch = nm_yaw_angle((uint32_t)(state->pitch+(pose.angles[0]-state->pitch)/ticks));
    next.roll = nm_yaw_angle((uint32_t)(state->roll+(pose.angles[2]-state->roll)/ticks));
#if defined(__DREAMCAST__) && defined(NM_GAMEPLAY_SH4ZAM)
    next.position[1] = shz_divf(pose.xyz[1]-state->position[1],divisor)+state->position[1];
#else
    next.position[1] = nm_force_divide(p,pose.xyz[1]-state->position[1],divisor)+state->position[1];
#endif
    nm_force_cartesian(p,(uint16_t)(-(int)next.heading),in->speed,next.delta);
    for (unsigned a = 0; a < 2; ++a) next.position[2*a] = next.delta[a]+state->position[2*a];
    if (!isfinite(next.desired_speed) || !isfinite(next.adjustment)) return 0;
    for (unsigned a = 0; a < 3; ++a) if (!isfinite(next.position[a])) return 0;
    NM_COPY(state,&next);
    return 1;
}
static inline int nm_guided_player_step(const nm_path_pack_t *path, const nm_drive_params_t *p,
        const nm_road_profile_t *profile, const nm_guided_player_input_t *in, nm_guided_player_state_t *state) {
    return nm_guided_step(path,p,profile,in,NULL,state);
}
/* 11150 rolling-start movement, distinct from10ccc above. Reuse the same
 * native state, but leave guided-only speed/adjustment/tire fields untouched.
 * Host table arithmetic is the bit-exact oracle. Target division/length use
 * SH4ZAM; target numerical acceptance still requires a hardware comparison. */
static inline int nm_rolling_player_step(const nm_path_pack_t *path, const nm_drive_params_t *p,
        const nm_road_profile_t *profile, float speed, nm_guided_player_state_t *state) {
    if (!state || !p || !isfinite(speed) || !isfinite(state->position[1])) return 0;
    nm_guided_player_state_t next; NM_COPY(&next,state);
    if (!nm_path_cursor_step(path,p,state->position[0],state->position[2],0,&next.cursor)) return 0;
    nm_road_pose_t pose;
    if (!nm_road_pose_step(profile,&next.lateral,&pose)) return 0;
    float divisor=32767.0f;
    int32_t ticks=32767;
    const float dx=state->position[0]-pose.xyz[0], dz=pose.xyz[2]-state->position[2];
    if (!isfinite(dx) || !isfinite(dz)) return 0;
    if (nm_force_bits(speed)) {
#if defined(__DREAMCAST__) && defined(NM_GAMEPLAY_SH4ZAM)
        const shz_vec2_t displacement=shz_vec2_init(dx,dz);
        divisor=shz_divf(shz_sqrtf(shz_vec2_magnitude_sqr(displacement)),speed);
#else
        divisor=nm_force_divide(p,nm_force_length(p,dx,dz),speed);
#endif
        if (!isfinite(divisor) || !nm_drive_round_i32(divisor,&ticks)) return 0;
        if (ticks<1) { ticks=1; divisor=1; }
    }
    const int32_t angle=nm_force_angle(p,dz,dx);
    if (angle==INT32_MIN) return 0;
    const int delta=nm_yaw_angle((uint32_t)(angle-state->heading));
    next.heading=nm_yaw_angle((uint32_t)(state->heading+delta/ticks));
    next.body_yaw=next.road_yaw=next.heading;
    next.pitch=nm_yaw_angle((uint32_t)(state->pitch+(pose.angles[0]-state->pitch)/ticks));
    next.roll=nm_yaw_angle((uint32_t)(state->roll+(pose.angles[2]-state->roll)/ticks));
#if defined(__DREAMCAST__) && defined(NM_GAMEPLAY_SH4ZAM)
    next.position[1]=shz_divf(pose.xyz[1]-state->position[1],divisor)+state->position[1];
#else
    next.position[1]=nm_force_divide(p,pose.xyz[1]-state->position[1],divisor)+state->position[1];
#endif
    nm_force_cartesian(p,(uint16_t)(-(int)next.heading),speed,next.delta);
    for(unsigned a=0;a<2;++a) next.position[2*a]=next.delta[a]+state->position[2*a];
    for(unsigned a=0;a<3;++a) if(!isfinite(next.position[a])) return 0;
    NM_COPY(state,&next);
    return 1;
}
#endif
