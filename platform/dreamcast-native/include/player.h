#ifndef DAYTONA_NATIVE_PLAYER_H
#define DAYTONA_NATIVE_PLAYER_H
#include "nm_mem.h"
#include "forces.h"
#include "tires.h"
#include "motion.h"
#include "guided.h"
#include "boundary.h"
#include "vehicle.h"
#include "gears.h"
#include "checkpoint.h"

/* One authoritative copy of each persistent field consumed/published by the
 * ordered active-player update block. No guest addresses or captured frame state. */
typedef struct {
    nm_boundary_state_t boundary;
    nm_forces_state_t force;
    nm_axle_state_t axle;
    nm_drive_terms_t terms;
    float previous_lateral, delta[2], lookahead[2];
    int32_t rpm, compensation;
    uint32_t alignment_frames;
    int16_t body_yaw, yaw_delta, yaw_offset, tire_filter, condition;
    int8_t countdown[6];
    nm_road_lateral_t lateral;
    float desired_speed, adjustment_target, adjustment;
    int16_t guided_ticks, road_yaw;
    uint16_t path_offset, ahead_offset, scenery_cell;
    int16_t phase[2];
    uint16_t path_flags, previous_path_flags;
    int8_t surface_ticks[4]; /*91..94: paired rear-wheel surface effects. */
    uint8_t profile_updates;
    uint32_t gear, brake; /* Gear routines access all32 brake bits; input writes low16 only. */
    int32_t steer, filtered_rpm, downshift_rpm;
    float resistance;
    uint32_t camera_words[6], auxiliary_flags, control;
    int32_t camera_mode;
    int16_t camera_angles[3];
    uint16_t accel;
    int8_t shift_ticks, drag_state, input_inhibit;
    uint8_t camera_transition, pre_sound_requests, held;
    int32_t speed_index;
    int16_t wheel_phase[2];
    float correction[2], inhibit_scale;
    int16_t shake_angles[2], inhibit_angle;
    int8_t correction_ticks, inhibit_direction;
    uint8_t early_sound_request;
    int16_t spin_body_offset; /* Sourcec0, distinct from shared yaw_offset. */
    int32_t offcourse_ticks;
    nm_lap_t lap;
    nm_checkpoints_t checkpoints; /* Shared race state: round-trip between participating cars. */
    nm_lap_write_t lap_write;
    int16_t render_angles[3], filtered_pitch, render_yaw_offset, bank, render_roll_offset;
    int16_t tilt[2], tilt_phase; /* Shared501844/501848, body22c. Preserve high shared halfwords externally. */
    uint8_t surface_phase; /*215: signed lookup, then six-bit phase update. */
    int16_t rank_key; /*60: low16 lap/profile progression, not race rank6a. */
    uint16_t rank_key_high; /*62: preserved by16-bit progress writes; announcement deltas read all32. */
    int8_t response_ticks; /*98: collision-response gate, not spin ticksec. */
    uint8_t route_transition; /* Output intent for sourcedc18; never a guest function pointer. */
    float proximity_circle[3]; /* Shared circle output: two coordinates and radius, not car XYZ. */
} nm_player_t;

/* da58..da80: player fields at entry to the post-finish guided drive.
 * Caller owns the preceding phase gate, timer hold and mode transition.
 * This is entry preparation, NOT the subsequent da84 physics loop. */
static inline int nm_player_finish_entry(nm_player_t *s) {
    if (!s) return 0;
    s->path_offset=(uint16_t)(((int32_t)s->phase[0]+5)*28);
    s->resistance=0;
    s->spin_body_offset=0;
    s->steer=0;
    s->accel=255;
    s->brake &= UINT32_C(0xffff0000);
    return 1;
}

enum { NM_FINISH_GUIDED, NM_FINISH_FORWARD, NM_FINISH_REVERSE };

/* daf4..db20, BEFORE motion/contact changes the road heading. */
static inline unsigned nm_player_finish_branch(const nm_player_t *s) {
    const int difference=nm_yaw_angle((uint32_t)((int)s->boundary.ground.road_heading-s->boundary.heading));
    if (difference>16384 || difference< -16384) return NM_FINISH_REVERSE;
    return (s->path_flags&1u) ? NM_FINISH_GUIDED : NM_FINISH_FORWARD;
}

/* db3c/db68/dba4 heading tails, AFTER branch-specific motion/contact.
 * Keep the pre-contact branch: reselecting here changes original behavior. */
static inline int nm_player_finish_heading(nm_player_t *s, unsigned branch) {
    if (!s || branch>NM_FINISH_REVERSE) return 0;
    const int offset=branch==NM_FINISH_REVERSE ? 32768 : 0;
    const int difference=nm_yaw_angle((uint32_t)((int)s->boundary.ground.road_heading+offset-s->boundary.heading));
    const unsigned shift=branch==NM_FINISH_GUIDED ? 5 : branch==NM_FINISH_FORWARD ? 3 : 2;
    s->boundary.heading=nm_yaw_angle((uint32_t)((int)s->boundary.heading+
        nm_guided_shift((uint32_t)difference,shift)));
    s->boundary.ground.yaw=s->boundary.heading;
    if (branch==NM_FINISH_FORWARD) s->path_offset=(uint16_t)(((int32_t)s->phase[0]+3)*28);
    return 1;
}

/* e3f4..e590 mapped gameplay fields. Preserve position, wheel phase, gear,
 * path and independently owned shared state; original is NOT a whole-car clear.
 * Caller separately owns entity metadata, self-neighbor links and motion scale. */
static inline int nm_player_base_reset(nm_player_t *s, float desired_speed) {
    if (!s || !isfinite(desired_speed)) return 0;
    s->boundary.damage.flags &= UINT32_C(0x80000000);
    s->desired_speed=desired_speed; s->boundary.speed=0; s->speed_index=0;
    s->force.longitudinal_delta=0; s->render_roll_offset=0;
    s->lap.counter=s->lap.completed=s->lap.lap=0;
    s->lap.best_ticks=s->lap.last_ticks=s->lap.start_ticks=0;
    s->lateral.current=s->lateral.target=s->lateral.control=0;
    nm_memset(s->boundary.damage.angle,0,sizeof(s->boundary.damage.angle));
    s->rpm=s->filtered_rpm=0; s->boundary.damage.damage=0;
    nm_memset(s->force.tire_angle,0,sizeof(s->force.tire_angle));
    nm_memset(s->delta,0,sizeof(s->delta)); s->force.rear_bank_force=0;
    s->terms.drive=0; nm_memset(s->force.inertial,0,sizeof(s->force.inertial));
    nm_memset(s->terms.brake,0,sizeof(s->terms.brake)); nm_memset(s->force.grip,0,sizeof(s->force.grip));
    s->resistance=0; s->boundary.damage.overlay_model=0;
    s->adjustment=s->adjustment_target=0; s->correction_ticks=0;
    nm_memset(s->correction,0,sizeof(s->correction));
    s->input_inhibit=s->inhibit_direction=0; s->boundary.damage.spin_ticks=0;
    s->boundary.damage.inhibit_ticks=s->boundary.damage.remaining=0;
    s->guided_ticks=0; s->boundary.damage.override_count=s->boundary.damage.initial_inhibit=0;
    s->drag_state=0; s->filtered_pitch=s->render_yaw_offset=s->bank=0;
    s->force.steer_angle=s->inhibit_angle=s->boundary.rotation=0;
    s->tilt[0]=0; s->boundary.damage.crash_count=0;
    nm_memset(s->surface_ticks,0,sizeof(s->surface_ticks));
    nm_memset(s->force.trigger_count,0,sizeof(s->force.trigger_count));
    s->response_ticks=0; s->boundary.condition=0;
    return 1;
}

/* e348..e3f0 grid reset tail: center probe, road frame, wheel positions and
 * road/profile selection. Deliberately does NOT run four wheel ground probes. */
static inline int nm_player_grid_ground(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, const nm_road_pack_t *road,
        const nm_path_pack_t *path, nm_player_t *state) {
    if (!p || !contact || !road || !state || contact->slot != road->slot) return 0;
    nm_player_t s; NM_COPY(&s,state);
    nm_wheels_state_t *g = &s.boundary.ground;
    nm_contact_result_t center = {g->position[1],g->road,0,0};
    if (!nm_contact_query(contact,p,g->position[0],g->position[2],&center)) return 0;
    g->position[1]=center.height; g->road=center.road; g->combined_mask=(uint16_t)center.mask;
    if (!nm_wheels_road_frame(contact,p,g)) return 0;
    s.boundary.heading=g->road_heading;
    if (!nm_contact_scenery_cell(g->position[0],g->position[2],&s.scenery_cell) ||
        !nm_wheels_transform(p,g->position,g->yaw,g->roll,g->world)) return 0;
    nm_road_selection_t selected = {g->road,s.path_offset,(uint16_t)s.phase[0],
        (uint16_t)s.phase[1],s.path_flags,s.previous_path_flags};
    if (!nm_road_select(road,path,p,g->combined_mask,s.boundary.damage.flags,
                       g->position[0],g->position[2],&selected)) return 0;
    g->road=selected.road; s.path_offset=selected.offset;
    s.phase[0]=nm_yaw_angle(selected.phase); s.phase[1]=nm_yaw_angle(selected.previous_phase);
    s.path_flags=selected.flags; s.previous_path_flags=selected.previous_flags;
    NM_COPY(state,&s);
    return 1;
}

/* e304..e3f0: authored grid pose followed by center-only contact/selection.
 * Grid record selection and speed/default initialization precede this block. */
static inline int nm_player_grid_pose(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, const nm_road_pack_t *road,
        const nm_path_pack_t *path, nm_player_t *state, float *reference_height) {
    if (!road || !road->profiles || !state || !reference_height ||
        (uint16_t)state->phase[0] >= road->profile_count) return 0;
    nm_player_t s; NM_COPY(&s,state);
    nm_road_pose_t pose;
    if (!nm_road_pose_step(road->profiles+(uint16_t)s.phase[0],&s.lateral,&pose)) return 0;
    nm_wheels_state_t *g=&s.boundary.ground;
    nm_memcpy(g->position,pose.xyz,sizeof(pose.xyz));
    g->pitch=nm_yaw_angle((uint32_t)pose.angles[0]);
    g->yaw=nm_yaw_angle((uint32_t)pose.angles[1]);
    g->roll=nm_yaw_angle((uint32_t)pose.angles[2]);
    s.body_yaw=s.road_yaw=s.boundary.heading=g->yaw;
    s.render_angles[0]=g->pitch; s.render_angles[1]=g->yaw; s.render_angles[2]=g->roll;
    if (!nm_player_grid_ground(p,contact,road,path,&s)) return 0;
    *reference_height=pose.xyz[1];
    NM_COPY(state,&s);
    return 1;
}

/* Prepared grid -> mapped base reset -> authored pose -> road selection.
 * grid_speed is the offline result of e298, not the raw grid speed_parameter.
 * Source-only route/style/speed38/guided-scale metadata remains caller-owned. */
static inline int nm_player_grid_reset(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, const nm_road_pack_t *road,
        const nm_path_pack_t *path, unsigned id, float grid_speed, uint32_t body_model,
        nm_player_t *state, float *reference_height) {
    if (!road || !road->grid || id>=road->grid_count || !path || !state || !reference_height) return 0;
    const nm_road_grid_t *grid=road->grid+id;
    if (grid->route!=path->route || path->slot!=road->slot ||
        road->profile_count<2 || grid->profile>=road->profile_count-1 || grid->previous>65535 ||
        grid->path_offset!=((grid->profile*28u)&65535u) ||
        grid->lateral!=((id&1) ? 384u : 1408u) || (grid->lap!=-1 && grid->lap!=0)) return 0;
    nm_player_t s; NM_COPY(&s,state);
    if (!nm_player_base_reset(&s,grid_speed)) return 0;
    s.boundary.damage.body_model=body_model;
    s.phase[0]=nm_yaw_angle(grid->profile);s.phase[1]=nm_yaw_angle(grid->previous);
    s.lap.furthest=(uint16_t)grid->previous;s.lap.lap=(uint8_t)grid->lap;
    s.path_offset=(uint16_t)grid->path_offset;
    s.lateral.current=s.lateral.target=(int16_t)grid->lateral;
    if (!nm_player_grid_pose(p,contact,road,path,&s,reference_height)) return 0;
    NM_COPY(state,&s);
    return 1;
}

/*9014..9084, called by the outer car/effect update at6c18, NOT by physics.
 * All nonzero byte patterns decrement modulo256, including negative values.
 * Keep separate until that caller's scheduling is integrated. */
static inline void nm_player_effect_timers(nm_player_t *s) {
    int8_t *ticks[7] = { &s->surface_ticks[0], &s->surface_ticks[1],
        &s->surface_ticks[2], &s->surface_ticks[3], &s->force.trigger_count[0],
        &s->force.trigger_count[1], &s->response_ticks };
    for (unsigned i=0;i<7;++i)
        if (*ticks[i]) *ticks[i] = *ticks[i] == -128 ? 127 : (int8_t)(*ticks[i]-1);
}

/* d3f4, rolling-start pedals (profiles 0x89..0x8b on every course), before its physics pipeline. This is NOT
 * the enDjinn input reader. Preserve brake high16 and brake-branch steer/hold. */
static inline void nm_player_rolling_controls(nm_player_t *s) {
    const unsigned profile=(uint16_t)s->phase[0];
    if (profile >= 137 && profile <= 139) {
        s->accel=0; s->brake=(s->brake&0xffff0000u)|255u;
    } else {
        s->accel=255; s->brake&=0xffff0000u;
        s->boundary.damage.override_count=0; s->steer=0;
    }
}

typedef struct {
    uint32_t speed_flags;
    int32_t speed_index;
    float motion_scale, guided_scale;
    uint8_t curve, course_slot, fixed_offset, guided_mode, lap, entity_id;
} nm_player_dynamics_input_t;

/* One view preset of the table 1d00220 (backup RAM, 5248..5300 copies program 2320b6 there: 16 entries of 20
 * bytes, the race selects 0..3 by 501710 = VR1..VR4). words: the targets of camera words 501724 (eye height),
 * 501728 (eye forward), 501730 (window centre offset) and 501734 (focus), as raw bits; pitch: the target of
 * 501738; adjust (+12): enables the camera adjustment 8cb4 (0 in every race preset); mode (+13): 5014a4. */
typedef struct {
    uint32_t words[4];
    int16_t pitch;
    uint8_t adjust, mode;
} nm_race_view_preset_t;

/* What the frame of a tick draws with (presentation): the mode 5014a4 and the eye word 501728 the player's draw
 * 6c0c read (before 8814: the view's own words), the view 501710 the contact effects 9088 read (also before),
 * and the focus 501734 and centre offset 501730 that 17fb0/17ffc put into the window records after 8814. */
typedef struct {
    int32_t mode;
    uint32_t eye;
    uint8_t view, valid;
    float focus, centre;
} nm_race_view_frame_t;

typedef struct {
    nm_player_dynamics_input_t dynamics;
    /* active_step derives dynamics.speed_index from speed; lower-level helpers use the supplied value. */
    nm_controls_t controls; /* Already decoded; enDjinn mapping/calibration is separate. */
    int32_t input_counter;
    float neighbor[3][2]; /* Ordered live positions, not a recorded traffic trajectory. */
    int8_t restored_camera_mode;
    uint8_t gear_selector, proximity_disabled, variant;
    uint8_t force_recovery; /* Decoded source control bit4, not a board/I/O dependency. */
} nm_player_active_input_t;
typedef struct {
    nm_player_active_input_t active;
    nm_checkpoint_award_input_t award;
    uint8_t phase;
} nm_player_tick_input_t;

/*11aa4..11bd0 body orientation. Reuse existing rounding/width semantics and
 * DND inertial scale; do not reassociate the two TGP multiplies. Crash angles
 * ca/cc/ce are read but discarded by this source routine, so are not inputs.
 * All conversions precede publication, including masked NaNs/overflow guards. */
static inline int nm_player_body_angles_step(const nm_drive_params_t *p,
        int16_t tilt_offset, nm_player_t *s) {
    if (!p || !s) return 0;
    int32_t bank, pitch;
    if (!nm_drive_round_i32(s->force.rear_bank_force,&bank)) return 0;
    bank = nm_gears_signed(0u-(uint32_t)bank);
    bank = bank >= 0 ? bank/8 : -1-(int32_t)((~(uint32_t)bank)/8);
    if (bank < -1280) bank = -1280;
    if (bank > 1280) bank = 1280;
    const float scaled = s->force.longitudinal_delta*p->inertial_scale[0];
    if (!nm_drive_round_i32(140.0f*scaled,&pitch)) return 0;
    pitch = nm_gears_signed((uint32_t)pitch-(uint32_t)(int32_t)s->filtered_pitch);
    if (pitch < -64) pitch = -64;
    if (pitch > 64) pitch = 64;
    pitch += s->filtered_pitch;
    if (pitch < -256) pitch = -256;
    if (pitch > 256) pitch = 256;
    const int16_t filtered = nm_yaw_angle((uint32_t)(pitch+tilt_offset));
    s->bank = (int16_t)bank; s->filtered_pitch = filtered;
    s->render_angles[0] = nm_yaw_angle((uint32_t)((int)filtered+s->boundary.ground.pitch));
    s->render_angles[1] = nm_yaw_angle((uint32_t)((int)s->boundary.ground.yaw+s->render_yaw_offset));
    s->render_angles[2] = nm_yaw_angle((uint32_t)(bank+s->boundary.ground.roll+s->render_roll_offset));
    return 1;
}

/* Existing shake table's native signed index domain is[-128,127]. The tilt
 * wave reaches129; DND23 appends only those two missing halfwords. */
static inline int16_t nm_player_tilt_sample(const nm_drive_params_t *p, int index) {
    return index < 128 ? p->shake_angles[1][(uint8_t)index] : p->tilt_tail[index-128];
}

/*1550c..15674. One source update, AFTER rendering angles consumed the previous
 * tilt. Integer-only; speed sign, untouched output2 and zero-speed writes are
 * significant. The discarded5010b0/r7 test has no observable state effect. */
static inline int nm_player_tilt_step(const nm_drive_params_t *p, uint32_t flags, nm_player_t *s) {
    if (!p || !s) return 0;
    const int32_t speed = s->speed_index;
    if ((flags&0x1e00000) && !(s->boundary.ground.combined_mask&2)) {
        if (!(s->boundary.ground.combined_mask&12)) return 1;
        const int phase = s->surface_phase < 128 ? (int)s->surface_phase : (int)s->surface_phase-256;
        const int half = phase < 0 ? -((-phase+1)/2) : phase/2;
        s->tilt[0] = nm_player_tilt_sample(p,34+half);
        s->tilt[1] = nm_player_tilt_sample(p,66+half);
        if (speed) {
            int32_t step = speed >= 0 ? speed/32 : -1-(int32_t)((~(uint32_t)speed)/32);
            if (step > 4) step = 4;
            s->surface_phase = (uint8_t)((uint32_t)(phase+step)&63);
            s->boundary.damage.override_count = (uint8_t)((uint32_t)speed>>7);
        }
    } else if (s->force.rpm_boost) {
        s->tilt_phase = (int16_t)(((uint32_t)(int32_t)s->tilt_phase+1u)&3);
        s->tilt[0] = p->boost_tilt[s->tilt_phase];
    } else if (speed) {
        const int32_t step = speed >= 150 ? (speed-50)/32 :
            (speed >= 0 ? speed/64 : -1-(int32_t)((~(uint32_t)speed)/64))+1;
        s->tilt_phase = (int16_t)(((uint32_t)(int32_t)s->tilt_phase+(uint32_t)step)&31);
        int value = nm_player_tilt_sample(p,98+s->tilt_phase);
        if (s->boundary.damage.damage&0x130) value *= 2;
        s->tilt[0] = nm_yaw_angle((uint32_t)value);
    }
    return 1;
}

/* Shared longitudinal chain through10338, without state refresh. Normal
 * d8c8 adds traction/yaw/tires; rolling d374 skips those three operations.
 * Input/gear/resistance and movement remain caller responsibilities. Atomic. */
static inline int nm_player_longitudinal_step(const nm_drive_params_t *p,
        const nm_player_dynamics_input_t *in, nm_player_t *state, int rolling) {
    if (!p || !in || !state) return 0;
    nm_player_t s; NM_COPY(&s,state);
    nm_drive_input_t drive; nm_memset(&drive,0,sizeof(drive));
    drive.throttle = (uint8_t)s.accel; drive.brake = (uint8_t)(uint16_t)s.brake;
    drive.curve = in->curve; drive.drag_state = s.drag_state;
    drive.rpm = (uint16_t)s.rpm; drive.pitch = s.boundary.ground.pitch;
    drive.gear = s.gear; drive.speed_index = (uint32_t)in->speed_index;
    drive.compensation = s.compensation;
    nm_memcpy(drive.contact,s.boundary.ground.mask,sizeof(drive.contact));
    if (!nm_drive_terms(p,&drive,&s.terms)) return 0;

    nm_axle_input_t axle; nm_memset(&axle,0,sizeof(axle));
    nm_memcpy(axle.inertial,s.force.inertial,sizeof(axle.inertial));
    axle.longitudinal_delta = s.force.longitudinal_delta;
    axle.pitch = s.boundary.ground.pitch; axle.bank = s.boundary.ground.roll;
    axle.speed_index = nm_yaw_angle((uint32_t)in->speed_index);
    nm_memcpy(axle.contact,s.boundary.ground.mask,sizeof(axle.contact));
    if (!nm_axle_load_step(p,&axle,&s.axle)) return 0;

    /* d374..d380 skips10568, f9d0 andfdf8 during rolling start. */
    if (!rolling) {
    nm_traction_input_t traction; nm_memset(&traction,0,sizeof(traction));
    traction.lateral = s.boundary.ground.road_frame[2];
    nm_memcpy(traction.inertial,s.force.inertial,sizeof(traction.inertial));
    traction.gear = s.gear; traction.steer_angle = s.force.steer_angle;
    traction.tire_angle = s.force.tire_angle[1];
    nm_memcpy(traction.phase,s.phase,sizeof(traction.phase));
    traction.speed_index = axle.speed_index; traction.steer = nm_yaw_angle((uint32_t)s.steer);
    traction.pitch = s.boundary.ground.pitch; traction.surface = s.boundary.ground.combined_mask;
    nm_memcpy(traction.contact,s.boundary.ground.mask,sizeof(traction.contact));
    traction.accel = s.accel; traction.brake = (uint16_t)s.brake;
    traction.course_slot = in->course_slot; traction.override_count = s.boundary.damage.override_count;
    nm_traction_state_t ts; nm_memset(&ts,0,sizeof(ts));
    ts.previous_lateral = s.previous_lateral; ts.drive = s.terms.drive;
    ts.rear_load = s.axle.load[1]; ts.slope = s.terms.slope; ts.condition_26c = s.condition;
    nm_memcpy(ts.countdown,s.countdown,sizeof(ts.countdown));
    if (!nm_traction_step(p,&traction,&ts)) return 0;
    s.previous_lateral = ts.previous_lateral; s.terms.drive = ts.drive;
    s.axle.load[1] = ts.rear_load; s.terms.slope = ts.slope; s.condition = ts.condition_26c;
    nm_memcpy(s.countdown,ts.countdown,sizeof(s.countdown));
    }

    nm_forces_input_t force; nm_memset(&force,0,sizeof(force));
    force.terms = s.terms; force.axle = s.axle; force.resistance = s.resistance;
    force.speed_index = in->speed_index; force.rpm = s.rpm; force.gear = s.gear;
    force.bank = s.boundary.ground.roll; force.steer = nm_yaw_angle((uint32_t)s.steer); force.condition_99 = s.boundary.condition;
    force.condition_ec = s.boundary.damage.spin_ticks < 128 ? (int8_t)s.boundary.damage.spin_ticks :
        (int8_t)((int)s.boundary.damage.spin_ticks-256);
    force.condition_15c = s.shift_ticks; force.condition_265 = s.countdown[1];
    if (!nm_forces_step(p,&force,&s.force)) return 0;

    if (!rolling) {
    nm_yaw_input_t yaw; nm_memset(&yaw,0,sizeof(yaw));
    yaw.speed = s.boundary.speed; yaw.steer_angle = s.force.steer_angle; yaw.steer = nm_yaw_angle((uint32_t)s.steer);
    nm_memcpy(yaw.tire_angle,s.force.tire_angle,sizeof(yaw.tire_angle));
    yaw.speed_index = axle.speed_index; yaw.road_heading = s.boundary.ground.road_heading;
    yaw.course_slot = in->course_slot; yaw.fixed_offset = in->fixed_offset;
    nm_yaw_state_t ys; nm_memset(&ys,0,sizeof(ys));
    nm_memcpy(ys.inertial,s.force.inertial,sizeof(ys.inertial));
    ys.alignment_frames = s.alignment_frames; ys.yaw = s.boundary.ground.yaw;
    ys.heading = s.boundary.heading; ys.body_yaw = s.body_yaw; ys.yaw_delta = s.yaw_delta; ys.offset = s.yaw_offset;
    if (!nm_yaw_step(p,&yaw,&ys)) return 0;
    nm_memcpy(s.force.inertial,ys.inertial,sizeof(ys.inertial));
    s.alignment_frames = ys.alignment_frames; s.boundary.ground.yaw = ys.yaw;
    s.boundary.heading = ys.heading; s.body_yaw = ys.body_yaw; s.yaw_delta = ys.yaw_delta; s.yaw_offset = ys.offset;

    nm_tires_input_t tires; nm_memset(&tires,0,sizeof(tires));
    tires.drive = s.terms.drive; tires.speed = s.boundary.speed;
    nm_memcpy(tires.load,s.axle.load,sizeof(tires.load));
    tires.front_grip = s.force.grip[0]; tires.yaw_delta = s.yaw_delta;
    tires.speed_index = axle.speed_index; tires.steer_angle = s.force.steer_angle;
    nm_tires_state_t tire; nm_memset(&tire,0,sizeof(tire));
    tire.rear_grip = s.force.grip[1]; tire.flags = s.boundary.damage.flags;
    nm_memcpy(tire.angle,s.force.tire_angle,sizeof(tire.angle));
    tire.filter = s.tire_filter; tire.condition = s.condition;
    tire.trigger_count = s.force.trigger_count[1]; tire.override_count = s.boundary.damage.override_count;
    if (!nm_tires_step(p,&tires,&tire)) return 0;
    s.force.grip[1] = tire.rear_grip; s.boundary.damage.flags = tire.flags;
    nm_memcpy(s.force.tire_angle,tire.angle,sizeof(tire.angle));
    s.tire_filter = tire.filter; s.condition = tire.condition;
    s.force.trigger_count[1] = tire.trigger_count; s.boundary.damage.override_count = tire.override_count;
    s.force.trigger_pair |= tire.trigger_pair;
    }

    nm_speed_state_t speed = {s.boundary.speed,s.force.longitudinal_delta,s.rpm,s.compensation};
    if (!nm_speed_step(p,in->speed_flags,s.gear,&speed)) return 0;
    s.boundary.speed = speed.speed; s.force.longitudinal_delta = speed.delta;
    s.rpm = speed.rpm; s.compensation = speed.compensation;
    NM_COPY(state,&s);
    return 1;
}

/* ea04 planar/spin movement, shared by normal driving and rolling dbd0. */
static inline int nm_player_planar_motion(const nm_drive_params_t *p,
        const nm_player_dynamics_input_t *in, nm_player_t *s) {
    nm_motion_input_t mi; nm_motion_state_t ms;
    nm_memset(&mi,0,sizeof(mi)); nm_memset(&ms,0,sizeof(ms));
    mi.scale = in->motion_scale; mi.flags = s->boundary.damage.flags; mi.speed_index = in->speed_index;
    mi.spin_ticks = s->boundary.damage.spin_ticks; mi.spin_heading = s->boundary.damage.spin_heading;
    mi.spin_rotation = s->boundary.damage.spin_rotation;
    nm_memcpy(ms.position,s->boundary.ground.position,sizeof(ms.position)); nm_memcpy(ms.delta,s->delta,sizeof(ms.delta));
    nm_memcpy(ms.lookahead,s->lookahead,sizeof(ms.lookahead));
    ms.speed = s->boundary.speed; ms.heading = s->boundary.heading; ms.yaw = s->boundary.ground.yaw;
    if (!nm_motion_step(p,&mi,&ms)) return 0;
    nm_memcpy(s->boundary.ground.position,ms.position,sizeof(ms.position)); nm_memcpy(s->delta,ms.delta,sizeof(ms.delta));
    nm_memcpy(s->lookahead,ms.lookahead,sizeof(ms.lookahead));
    s->boundary.speed = ms.speed; s->boundary.heading = ms.heading; s->boundary.ground.yaw = ms.yaw;
    return 1;
}

/*10ccc state adapter shared by normal guidance and post-finish movement.
 * Caller owns guided_ticks, longitudinal forces and subsequent ground work. */
static inline int nm_car_guided_motion(const nm_drive_params_t *p,
        const nm_path_pack_t *path,
        const nm_road_profile_t *profile, const nm_player_dynamics_input_t *in,
        const nm_guided_race_input_t *race, uint8_t *remembered, nm_player_t *s) {
    if (!p || !in || !s || (race && !remembered)) return 0;
    nm_guided_player_input_t gi; nm_guided_player_state_t gs;
    nm_memset(&gi,0,sizeof(gi)); nm_memset(&gs,0,sizeof(gs));
    gi.speed = s->boundary.speed; gi.scale = in->guided_scale; gi.speed_index = in->speed_index;
    gi.flags = s->boundary.damage.flags; gi.path_flags = s->path_flags; gi.surface = s->boundary.ground.combined_mask;
    gi.condition = s->boundary.damage.initial_inhibit < 128 ? (int8_t)s->boundary.damage.initial_inhibit :
        (int8_t)((int)s->boundary.damage.initial_inhibit-256);
    gi.mode = in->guided_mode;
    nm_memcpy(gs.position,s->boundary.ground.position,sizeof(gs.position)); nm_memcpy(gs.delta,s->delta,sizeof(gs.delta));
    gs.desired_speed = s->desired_speed; gs.adjustment_target = s->adjustment_target; gs.adjustment = s->adjustment;
    gs.cursor.offset = s->path_offset; gs.cursor.road = s->boundary.ground.road; gs.lateral = s->lateral;
    gs.heading = s->boundary.heading; gs.body_yaw = s->body_yaw; gs.pitch = s->boundary.ground.pitch;
    gs.roll = s->boundary.ground.roll; gs.road_yaw = s->road_yaw; gs.tire_angle = s->force.tire_angle[1];
    gs.ahead_offset = s->ahead_offset; gs.profile_updates = s->profile_updates;
    gs.remembered=remembered ? *remembered : 255;
    if (!nm_guided_step(path,p,profile,&gi,race,&gs)) return 0;
    nm_memcpy(s->boundary.ground.position,gs.position,sizeof(gs.position)); nm_memcpy(s->delta,gs.delta,sizeof(gs.delta));
    s->desired_speed = gs.desired_speed; s->adjustment_target = gs.adjustment_target; s->adjustment = gs.adjustment;
    s->path_offset = gs.cursor.offset; s->boundary.ground.road = gs.cursor.road; s->lateral = gs.lateral;
    s->boundary.heading = gs.heading; s->body_yaw = gs.body_yaw; s->boundary.ground.pitch = gs.pitch;
    s->boundary.ground.roll = gs.roll; s->road_yaw = gs.road_yaw; s->force.tire_angle[1] = gs.tire_angle;
    s->ahead_offset = gs.ahead_offset; s->profile_updates = gs.profile_updates;
    if(remembered) *remembered=gs.remembered;
    return 1;
}
static inline int nm_player_guided_motion(const nm_drive_params_t *p, const nm_path_pack_t *path,
        const nm_road_profile_t *profile, const nm_player_dynamics_input_t *in, nm_player_t *s) {
    return nm_car_guided_motion(p,path,profile,in,NULL,NULL,s);
}

/* Normal d8c8..d8f0 adds movement/ground/scenery to the shared physics chain.
 * force.trigger_pair publishes the auxiliary request. One atomic native tick. */
static inline int nm_player_dynamics_step(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, const nm_path_pack_t *path,
        const nm_road_profile_t *profile, const nm_player_dynamics_input_t *in,
        nm_player_t *state) {
    if (!p || !contact || !in || !state || in->course_slot != contact->slot) return 0;
    nm_player_t s; NM_COPY(&s,state);
    if (!nm_player_longitudinal_step(p,in,&s,0)) return 0;
    if (!s.boundary.damage.spin_ticks && s.guided_ticks > 0) {
        if (!nm_player_guided_motion(p,path,profile,in,&s)) return 0;
        --s.guided_ticks;
    } else {
        if (!nm_player_planar_motion(p,in,&s)) return 0;
    }
    if (!nm_wheels_ground(contact,p,&s.boundary.ground)) return 0;
    if (!nm_contact_scenery_cell(s.boundary.ground.position[0],s.boundary.ground.position[2],&s.scenery_cell)) return 0;
    NM_COPY(state,&s);
    return 1;
}

/*16da4: signed pair SUM, not two independent zero tests. Bit2 wins over bit3.
 * Only the rear masks' low bits are consumed by the original unaligned loads. */
static inline void nm_player_surface_timers(int32_t speed_index, const uint16_t masks[4], int8_t ticks[4]) {
    if (!speed_index) return;
    for (unsigned v = 0; v < 2; ++v) if ((int)ticks[v]+ticks[v+2] == 0) {
        if (masks[v+2]&4) ticks[v] = 32;
        else if (masks[v+2]&8) ticks[v+2] = 32;
    }
}

/* Shared10980 ->16060 ->16da4 after either movement path. */
static inline int nm_player_road_contact_step(const nm_drive_params_t *p, const nm_contact_pack_t *contact,
        const nm_road_pack_t *road, const nm_path_pack_t *path,
        const nm_player_dynamics_input_t *in, nm_player_t *state) {
    if (!p || !contact || !road || !in || !state || road->slot != contact->slot) return 0;
    nm_player_t s; NM_COPY(&s,state);
    nm_road_selection_t selected = {s.boundary.ground.road,s.path_offset,
        (uint16_t)s.phase[0],(uint16_t)s.phase[1],s.path_flags,s.previous_path_flags};
    if (!nm_road_select(road,path,p,s.boundary.ground.combined_mask,s.boundary.damage.flags,
        s.boundary.ground.position[0],s.boundary.ground.position[2],&selected)) return 0;
    s.boundary.ground.road = selected.road; s.path_offset = selected.offset;
    s.phase[0] = nm_yaw_angle(selected.phase); s.phase[1] = nm_yaw_angle(selected.previous_phase);
    s.path_flags = selected.flags; s.previous_path_flags = selected.previous_flags;
    const nm_boundary_input_t boundary = {in->speed_index,in->speed_flags,in->lap,in->curve,in->entity_id};
    if (!nm_boundary_step(contact,p,&boundary,&s.boundary)) return 0;
    nm_player_surface_timers(in->speed_index,s.boundary.ground.mask,s.surface_ticks);
    NM_COPY(state,&s);
    return 1;
}

/* Contiguous active-player d8c8..d8fc: dynamics/ground/scenery -> road/profile
 * selection -> complete boundary response -> rear surface timers. Each field
 * has one authoritative home, including flags/speed/heading and spin/override
 * state shared by dynamics and collision. No state reload between operations.
 * Inputs/gears and later race/caller updates are still caller responsibilities.
 * Shared globals inside boundary/ground must be published between entities.
 * One reference tick; enDjinn owns the outer loop. All-or-nothing publication. */
static inline int nm_player_step(const nm_drive_params_t *p, const nm_contact_pack_t *contact,
        const nm_road_pack_t *road, const nm_path_pack_t *path,
        const nm_player_dynamics_input_t *in, nm_player_t *state) {
    if (!p || !contact || !road || !in || !state || road->slot != contact->slot ||
        !road->profiles || !road->profile_count) return 0;
    nm_player_t s; NM_COPY(&s,state);
    const nm_road_profile_t *profile = NULL;
    if (!s.boundary.damage.spin_ticks && s.guided_ticks > 0) {
        const unsigned index = (uint16_t)s.phase[0];
        if (!path || path->slot != road->slot) return 0;
        // The guided helper rejects a missing profile only if its branch uses it.
        if (index < road->profile_count) profile = road->profiles+index;
    }
    if (!nm_player_dynamics_step(p,contact,path,profile,in,&s)) return 0;
    if (!nm_player_road_contact_step(p,contact,road,path,in,&s)) return 0;
    NM_COPY(state,&s);
    return 1;
}
/* Shared field boundary for active-player and intro-leader damage prefixes.
 * Intro omits15fc4; all camera/control fields remain explicit native state. */
static inline int nm_player_damage_prefix(const nm_drive_params_t *p,
        const nm_damage_pre_input_t *in,nm_player_t *state,int intro) {
    if (!p || !in || !state) return 0;
    nm_player_t s; NM_COPY(&s,state);
    nm_damage_pre_state_t pre; nm_memset(&pre,0,sizeof(pre));
    pre.damage = s.boundary.damage; pre.heading = s.boundary.heading;
    pre.decay = s.boundary.rotation; pre.path_offset = s.path_offset;
    pre.pivot_signal = (uint16_t)s.boundary.pivot_signal; pre.countdown = (uint8_t)s.boundary.condition;
    nm_memcpy(pre.camera_words,s.camera_words,sizeof(pre.camera_words));
    nm_memcpy(pre.camera_angles,s.camera_angles,sizeof(pre.camera_angles));
    pre.auxiliary_flags = s.auxiliary_flags; pre.control = s.control;
    pre.camera_mode = s.camera_mode; pre.camera_transition = s.camera_transition;
    if (!(intro ? nm_damage_intro_step(p,in,&pre) : nm_damage_pre_step(p,in,&pre))) return 0;
    s.boundary.damage = pre.damage; s.boundary.heading = pre.heading;
    s.boundary.rotation = pre.decay; s.path_offset = pre.path_offset;
    s.boundary.pivot_signal = nm_yaw_angle(pre.pivot_signal);
    s.boundary.condition = pre.countdown < 128 ? (int8_t)pre.countdown : (int8_t)((int)pre.countdown-256);
    nm_memcpy(s.camera_words,pre.camera_words,sizeof(s.camera_words));
    nm_memcpy(s.camera_angles,pre.camera_angles,sizeof(s.camera_angles));
    s.auxiliary_flags = pre.auxiliary_flags; s.control = pre.control;
    s.camera_mode = pre.camera_mode; s.camera_transition = pre.camera_transition;
    s.pre_sound_requests = pre.damage.sound_requests;
    s.boundary.damage.auxiliary_request = s.boundary.damage.sound_requests = 0;
    NM_COPY(state,&s);return 1;
}

/* d888..d8c8: lifecycle -> input -> gears -> recovery -> resistance/proximity.
 * Rolling d360..d370 keeps pedals, forces automatic gears, omits normal tail. */
static inline int nm_player_prepare_mode(const nm_drive_params_t *p,
        const nm_player_active_input_t *in, nm_player_t *state, int rolling) {
    if (!p || !in || !state) return 0;
    nm_player_t s; NM_COPY(&s,state);
    s.force.trigger_pair = s.boundary.pivot_sound = s.pre_sound_requests = 0;
    s.boundary.action = NM_BOUNDARY_NONE;
    const nm_damage_pre_input_t pi = {in->dynamics.speed_flags,
        (uint32_t)s.path_flags | ((uint32_t)s.previous_path_flags<<16),s.phase[0],s.boundary.ground.yaw,
        in->restored_camera_mode,in->variant,in->dynamics.entity_id};
    if(!nm_player_damage_prefix(p,&pi,&s,0)) return 0;

    if (!rolling) {
        nm_vehicle_input_t controls = {s.steer,s.accel,(uint16_t)s.brake,s.boundary.damage.override_count};
        if (!nm_vehicle_input_step(&controls,&in->controls,
            (int)s.input_inhibit+s.boundary.damage.inhibit_ticks != 0,in->input_counter,in->dynamics.speed_index)) return 0;
        s.steer = controls.steer; s.accel = controls.accel;
        s.brake = (s.brake&0xffff0000u) | controls.brake; s.boundary.damage.override_count = controls.override_count;
    }
    const nm_gears_input_t gi = {in->dynamics.speed_index,nm_gears_signed(s.force.rpm_boost),
        !rolling && in->dynamics.curve != 0,(uint8_t)(rolling ? 0 : in->gear_selector)};
    nm_gears_state_t gear = {s.gear,s.brake,s.rpm,s.filtered_rpm,s.downshift_rpm,s.shift_ticks};
    if (!nm_gears_step(p,&gi,&gear)) return 0;
    s.gear = gear.gear; s.brake = gear.brake; s.rpm = gear.rpm;
    s.filtered_rpm = gear.filtered_rpm; s.downshift_rpm = gear.downshift_rpm; s.shift_ticks = gear.shift_ticks;
    if (rolling) { NM_COPY(state,&s); return 1; } // d36c..d370: no input, recovery, resistance or proximity.
    s.held = s.boundary.recovery_ticks > 0;
    if (s.held) {
        if (!--s.boundary.recovery_ticks) s.force.tire_angle[1] = 0;
    } else {
        nm_resistance_state_t resistance = {s.resistance,s.boundary.damage.override_count};
        if (!nm_resistance_step(p,&resistance)) return 0;
        s.resistance = resistance.resistance; s.boundary.damage.override_count = resistance.override_count;
        nm_proximity_input_t proximity; nm_memset(&proximity,0,sizeof(proximity));
        proximity.disabled = in->proximity_disabled; proximity.heading = s.boundary.heading;
        proximity.position[0] = s.boundary.ground.position[0]; proximity.position[1] = s.boundary.ground.position[2];
        nm_memcpy(proximity.neighbor,in->neighbor,sizeof(proximity.neighbor));
        if (!nm_proximity_step(p,&proximity,&s.drag_state)) return 0;
    }
    NM_COPY(state,&s);
    return 1;
}
static inline int nm_player_prepare_step(const nm_drive_params_t *p,
        const nm_player_active_input_t *in, nm_player_t *state) {
    return nm_player_prepare_mode(p,in,state,0);
}

/*154d0: normal driving calls this before inhibit response; rolling calls it
 *after longitudinal physics. Caller owns validation and atomic publication. */
static inline void nm_player_shake_step(const nm_drive_params_t *p, nm_player_t *s) {
    if (s->boundary.damage.shake_ticks) {
        const unsigned timer = (uint8_t)s->boundary.damage.shake_ticks;
        s->shake_angles[0] = p->shake_angles[0][timer]; s->shake_angles[1] = p->shake_angles[1][timer];
        s->boundary.damage.shake_ticks = timer == 128 ? 127 : (int8_t)((int)s->boundary.damage.shake_ticks-1);
    }
}

/* e5f4 ->15314 ->154d0 ->158c8: speed index/wheel phases, existing impact
 * displacement, shared shake and authored input-inhibit response. Static
 * table addressing is folded into DND16. No invented collision/spawn behavior.
 * Early sound is an intent (9e067f), never playback. Publish atomically.
 * Rolling d354..d35c defers shake until after longitudinal physics (d384). */
/* e5f4 alone: speed index +18 from the speed, wheel phases +5a/+250. The pit
 * tick dd14 calls it without 15314/154d0/158c8 (df54). Caller publishes. */
static inline int nm_player_speed_index_step(const nm_drive_params_t *p, nm_player_t *s) {
    if (!nm_drive_round_i32(s->boundary.speed*p->speed_index_scale,&s->speed_index)) return 0;
    int32_t phase = nm_gears_signed((uint32_t)s->speed_index<<6);
    // Preserve source branch order: the upper clamp bypasses the lower cap.
    if (phase < 0) phase = 0;
    else if (phase > 0x2aa8) phase = 0x2aa8;
    else if (phase > 0xaaa) phase = 0xaaa;
    s->wheel_phase[0] = nm_yaw_angle((uint32_t)(int32_t)s->wheel_phase[0]-(uint32_t)phase);
    if (s->force.rpm_boost) phase = 0x2aa8;
    s->wheel_phase[1] = nm_yaw_angle((uint32_t)(int32_t)s->wheel_phase[1]-(uint32_t)phase);
    return 1;
}
static inline int nm_player_early_mode(const nm_drive_params_t *p, nm_player_t *state, int rolling) {
    if (!p || !state) return 0;
    nm_player_t s; NM_COPY(&s,state);
    s.early_sound_request = 0;
    if (!nm_player_speed_index_step(p,&s)) return 0;
    if (s.correction_ticks) {
        const int ticks = (int)s.correction_ticks-1;
        s.correction_ticks = ticks < -128 ? 127 : (int8_t)ticks;
        if (ticks == 1) s.correction[0] = s.correction[1] = 0;
        for (unsigned a = 0; a < 2; ++a) {
            const float position = s.correction[a]+s.boundary.ground.position[2*a];
            const float correction = 0.75f*s.correction[a];
            if (!isfinite(position) || !isfinite(correction)) return 0;
            s.boundary.ground.position[2*a] = position; s.correction[a] = correction;
        }
    }
    if (!rolling) nm_player_shake_step(p,&s);
    if (s.input_inhibit) {
        s.control = 224;
        const unsigned timer = (uint8_t)s.input_inhibit;
        s.input_inhibit = timer > 128 ? (int8_t)((int)timer-257) : (int8_t)(timer-1);
        s.early_sound_request = timer == 81;
        int angle = p->inhibit_angles[(s.boundary.damage.flags>>1)&1][timer];
        if (s.inhibit_direction >= 0) angle = -angle;
        s.inhibit_angle = nm_yaw_angle((uint32_t)angle);
        angle = s.inhibit_angle < 0 ? -(int)s.inhibit_angle : s.inhibit_angle;
        const float product = nm_force_float(0x384cccaa)*(float)angle;
        s.inhibit_scale = product-nm_force_float(0x3f59999a);
        const float speed = s.boundary.speed-p->inhibit_speed_drop;
        if (!isfinite(s.inhibit_scale) || !isfinite(speed)) return 0;
        if (!(nm_force_bits(speed)&0x80000000u)) s.boundary.speed = speed;
        s.boundary.damage.flags &= ~0x18000u;
    } else if (s.speed_index < 20 || (uint8_t)s.boundary.damage.inhibit_ticks) {
        s.boundary.damage.flags &= ~0x18000u;
    } else if (s.boundary.damage.flags&0x18000u) {
        s.inhibit_direction = s.boundary.damage.flags&0x10000u ? -1 : 1;
        s.input_inhibit = -112; // Original16-bit stores ff90/0190: byte timer144 plus direction.
        s.boundary.damage.flags &= ~0x18000u;
        s.early_sound_request = 1;
    }
    NM_COPY(state,&s);
    return 1;
}
static inline int nm_player_early_step(const nm_drive_params_t *p, nm_player_t *state) {
    return nm_player_early_mode(p,state,0);
}

/*11150 adapter shared by rolling start and authored attract driving.
 * No powertrain, shake or ground-contact side effects; publish after success. */
static inline int nm_player_rolling_path_step(const nm_drive_params_t *p,
        const nm_path_pack_t *path, const nm_road_profile_t *profile,
        nm_player_t *state) {
    if (!p || !state) return 0;
    nm_guided_player_state_t movement; nm_memset(&movement,0,sizeof(movement));
    nm_memcpy(movement.position,state->boundary.ground.position,sizeof(movement.position));
    nm_memcpy(movement.delta,state->delta,sizeof(movement.delta));
    movement.cursor.offset = state->path_offset; movement.cursor.road = state->boundary.ground.road;
    movement.lateral = state->lateral; movement.heading = state->boundary.heading;
    movement.body_yaw = state->body_yaw; movement.road_yaw = state->road_yaw;
    movement.pitch = state->boundary.ground.pitch; movement.roll = state->boundary.ground.roll;
    if (!nm_rolling_player_step(path,p,profile,state->boundary.speed,&movement)) return 0;
    nm_memcpy(state->boundary.ground.position,movement.position,sizeof(movement.position));
    nm_memcpy(state->delta,movement.delta,sizeof(movement.delta));
    state->path_offset = movement.cursor.offset; state->boundary.ground.road = movement.cursor.road;
    state->lateral = movement.lateral; state->boundary.heading = movement.heading;
    state->body_yaw = movement.body_yaw; state->road_yaw = movement.road_yaw;
    state->boundary.ground.pitch = movement.pitch; state->boundary.ground.roll = movement.roll;
    return 1;
}

/* d374..d390: rolling powertrain -> delayed shake ->11150 or dbd0 spin movement.
 * Ground/collision follows. Publish only after the whole prefix succeeds. */
static inline int nm_player_rolling_motion_step(const nm_drive_params_t *p,
        const nm_path_pack_t *path, const nm_road_profile_t *profile,
        const nm_player_dynamics_input_t *in, nm_player_t *state) {
    if (!p || !in || !state) return 0;
    nm_player_t s; NM_COPY(&s,state);
    if (!nm_player_longitudinal_step(p,in,&s,1)) return 0;
    nm_player_shake_step(p,&s);
    if (s.boundary.damage.spin_ticks) {
        if (!nm_player_planar_motion(p,in,&s)) return 0;
    } else if (!nm_player_rolling_path_step(p,path,profile,&s)) return 0;
    NM_COPY(state,&s);
    return 1;
}

/* d394..d3c8: contact height/road frame, retaining11150's pitch/roll (the
 * caller saves local r3/r4 across116d0), then signed16 heading correction. */
static inline int nm_player_rolling_ground_step(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, nm_player_t *state) {
    if (!p || !contact || !state) return 0;
    nm_player_t s; NM_COPY(&s,state);
    const int16_t pitch = s.boundary.ground.pitch, roll = s.boundary.ground.roll;
    if (!nm_wheels_ground(contact,p,&s.boundary.ground)) return 0;
    s.boundary.ground.pitch = pitch; s.boundary.ground.roll = roll;
    const int16_t delta = nm_yaw_angle((uint32_t)((int)s.boundary.ground.road_heading-s.boundary.heading));
    s.boundary.heading = nm_yaw_angle((uint32_t)((int)s.boundary.heading+
        nm_guided_shift((uint32_t)(int32_t)delta,5)));
    s.boundary.ground.yaw = s.boundary.heading;
    NM_COPY(state,&s);
    return 1;
}

/*10a14/10a34: road-loss timer and64-tick recovery, distinct from16060's
 *21-tick impact recovery. DNC3 already holds both authored height and ordered
 *vertex-average center. Signed timer increments wrap before comparison. */
static inline int nm_player_offcourse_step(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, int force_recovery, nm_player_t *state) {
    if (!p || !contact || !state) return 0;
    nm_player_t s; NM_COPY(&s,state);
    if (force_recovery) s.offcourse_ticks = 0x18c0;
    if (s.boundary.ground.combined_mask&0x602) s.offcourse_ticks = 0;
    else {
        s.offcourse_ticks = nm_gears_signed((uint32_t)s.offcourse_ticks+1u);
        if (s.offcourse_ticks > 512) {
            const unsigned road = s.boundary.ground.road;
            if (!contact->quads || road >= contact->quad_count) return 0;
            const nm_contact_quad_t *quad = contact->quads+road;
            if (!isfinite(quad->recovery_height)) return 0;
            s.offcourse_ticks = 0; s.boundary.speed = 0;
            s.boundary.hold_ticks = s.boundary.recovery_ticks = 64;
            s.boundary.ground.position[1] = quad->recovery_height;
            for (unsigned v = 0; v < 4; ++v) s.boundary.ground.height[v] = quad->recovery_height;
            s.boundary.ground.road_heading = nm_yaw_angle(quad->heading);
            for (unsigned a = 0; a < 3; ++a) {
                if (!isfinite(quad->recovery_center[a]) || !isfinite(s.boundary.ground.position[a])) return 0;
                s.boundary.displacement[a] = nm_force_divide(p,
                    quad->recovery_center[a]-s.boundary.ground.position[a],64.0f);
                if (!isfinite(s.boundary.displacement[a])) return 0;
            }
            s.boundary.recovery_rotation = (int16_t)(((int)s.boundary.ground.road_heading-s.boundary.ground.yaw)/64);
        }
    }
    NM_COPY(state,&s);
    return 1;
}

/* d2c0..d324 player / d684..d6f4 opponent entry after the grid reset.
 * Opponents alternate lanes and also update source26e (road yaw).
 * The separate source288 height
 * is returned for the camera/other vehicle consumers, not aliased to live Y.
 * Caller owns UI counter reset, start flag and updater selection. */
static inline int nm_car_rolling_entry_pose(const nm_road_profile_t *profile,
        unsigned id, nm_player_t *state, float *reference_height) {
    if (!state || !reference_height || id>=44) return 0;
    nm_player_t s; NM_COPY(&s,state);
    s.lateral.current = s.lateral.target = id&1u ? 1408 : 384;
    nm_road_pose_t pose;
    if (!nm_road_pose_step(profile,&s.lateral,&pose)) return 0;
    s.lap.lap = 255;
    nm_memcpy(s.boundary.ground.position,pose.xyz,sizeof(pose.xyz));
    s.boundary.ground.pitch = nm_yaw_angle((uint32_t)pose.angles[0]);
    s.boundary.ground.roll = nm_yaw_angle((uint32_t)pose.angles[2]);
    s.body_yaw = s.boundary.heading = s.boundary.ground.yaw = nm_yaw_angle((uint32_t)pose.angles[1]);
    if (id) s.road_yaw = s.boundary.ground.yaw;
    s.render_angles[0] = s.boundary.ground.pitch;
    s.render_angles[1] = s.boundary.ground.yaw;
    s.render_angles[2] = nm_yaw_angle((uint32_t)((int)s.boundary.ground.roll+s.render_roll_offset));
    s.phase[0] = nm_yaw_angle((uint32_t)((int)s.phase[0]+1));
    s.path_offset = (uint16_t)((uint16_t)s.phase[0]*28u);
    *reference_height = pose.xyz[1];
    NM_COPY(state,&s);
    return 1;
}

static inline int nm_player_rolling_entry_pose(const nm_road_profile_t *profile,
        nm_player_t *state, float *reference_height) {
    return nm_car_rolling_entry_pose(profile,0,state,reference_height);
}

/* Player0 prepared reset -> rolling entry (road slot 0..2). Outer phase timing, UI and
 * shared camera/effect owners remain external. No ROM access or startup math. */
static inline int nm_player_rolling_start(const nm_drive_params_t *p,
        const nm_contact_pack_t *contact, const nm_road_pack_t *road,
        const nm_path_pack_t *path, unsigned variant, nm_player_t *state,
        float *reference_height, uint32_t *start_flags) {
    if (!road || road->slot>2 || !road->grid || !road->grid_count || !road->start || !road->grid_speeds || !road->grid_guided_scales ||
        !state || !reference_height || !start_flags || variant>=4 ||
        !isfinite(road->start->rolling_speed)) return 0;
    nm_player_t s; NM_COPY(&s,state);float height;
    if (!nm_player_grid_reset(p,contact,road,path,0,road->grid_speeds[0],
                             road->start->body_model[variant],&s,&height) ||
        (uint16_t)s.phase[0]>=road->profile_count) return 0;
    s.boundary.speed=road->start->rolling_speed;
    if (!nm_player_rolling_entry_pose(road->profiles+(uint16_t)s.phase[0],&s,&height)) return 0;
    NM_COPY(state,&s);*reference_height=height;*start_flags|=1u;
    return 1;
}

/* d5a8 gate and d648..d678 gameplay effects. Return the event that switches
 * the native updater to normal driving; no guest function pointer or UI MMIO.
 * Other start-flag bits, checkpoint history and race clock are preserved. */
static inline int nm_player_rolling_release(nm_player_t *s, uint32_t *start_flags) {
    if (!s || !start_flags || s->phase[0]) return 0;
    s->checkpoints.last_lap = s->checkpoints.completed = 0;
    s->lap.completed = s->lap.lap = 0;
    s->lap.last_ticks = s->lap.best_ticks = 0;
    *start_flags &= ~UINT32_C(1);
    return 1;
}

/*10bac/13de0. Caller validates profile count before this infallible tail. */
static inline void nm_player_road_progress(const nm_road_pack_t *road, nm_player_t *s) {
    const int lap = s->lap.lap < 128 ? (int)s->lap.lap : (int)s->lap.lap-256;
    if (!(s->boundary.ground.combined_mask&0x50c)) s->path_offset = (uint16_t)((uint16_t)s->phase[0]*28u);
    s->rank_key = nm_yaw_angle((uint32_t)(lap*(int)(road->profile_count-1)+s->phase[0]));
}

/* dbec..dc10: body uses the OLD offset, then offset approaches half a turn. */
static inline void nm_player_spin_pose(nm_player_t *s) {
    s->body_yaw = nm_yaw_angle((uint32_t)((int)s->boundary.ground.yaw+s->spin_body_offset));
    s->spin_body_offset = nm_yaw_angle((uint32_t)((int)s->spin_body_offset+
        nm_guided_shift((uint32_t)(32768-(int)s->spin_body_offset),6)));
}

/* d350..d3e4 or dbd0..dc14. Return1 for rolling,2 for spin,0 for failure.
 * No normal checkpoint/road-mark/tilt-wave update. Spin skips road progression
 * and subsequent release/initial-inhibit reset even if boundary changes spin. */
static inline int nm_player_rolling_prefix(const nm_drive_params_t *p, const nm_contact_pack_t *contact,
        const nm_road_pack_t *road, const nm_path_pack_t *path,
        const nm_player_active_input_t *in, nm_player_t *state) {
    if (!p || !contact || !road || !path || !in || !state || !road->profiles ||
        road->profile_count < 2 || road->profile_count > 4097 || road->slot != contact->slot ||
        path->slot != road->slot || in->dynamics.course_slot != road->slot) return 0;
    nm_player_t s; NM_COPY(&s,state);
    nm_player_active_input_t tick; NM_COPY(&tick,in);
    nm_player_rolling_controls(&s);
    if (!nm_player_early_mode(p,&s,1)) return 0;
    tick.dynamics.speed_index = s.speed_index;
    if (!nm_player_prepare_mode(p,&tick,&s,1)) return 0;
    const int spinning = s.boundary.damage.spin_ticks != 0;
    const unsigned profile = (uint16_t)s.phase[0];
    if (profile >= road->profile_count ||
        !nm_player_rolling_motion_step(p,path,road->profiles+profile,&tick.dynamics,&s) ||
        !(spinning ? nm_wheels_ground(contact,p,&s.boundary.ground) : nm_player_rolling_ground_step(p,contact,&s)) ||
        !nm_contact_scenery_cell(s.boundary.ground.position[0],s.boundary.ground.position[2],&s.scenery_cell) ||
        !nm_player_road_contact_step(p,contact,road,path,&tick.dynamics,&s) ||
        !nm_player_body_angles_step(p,s.tilt[0],&s)) return 0;
    if (s.boundary.damage.auxiliary_request) s.auxiliary_flags |= 0x80000000u;
    if (spinning) nm_player_spin_pose(&s);
    else nm_player_road_progress(road,&s);
    NM_COPY(state,&s);
    return spinning ? 2 : 1;
}

/* daac..dc10 post-finish player update, excluding outer phase gate and e078
 * presentation tail. Unlike rolling, no d3f4 controls/154d0 shake/release or
 * normal checkpoint/mark updates. Caller owns effect timers and finish entry. */
static inline int nm_player_finish_prefix(const nm_drive_params_t *p, const nm_contact_pack_t *contact,
        const nm_road_pack_t *road, const nm_path_pack_t *path,
        const nm_player_active_input_t *in, nm_player_t *state) {
    if (!p || !contact || !road || !path || !in || !state || !road->profiles ||
        road->profile_count<2 || road->profile_count>4097 || road->slot!=contact->slot ||
        path->slot!=road->slot || in->dynamics.course_slot!=road->slot) return 0;
    nm_player_t s; NM_COPY(&s,state);
    nm_player_active_input_t tick; NM_COPY(&tick,in);
    if (s.boundary.damage.initial_inhibit) --s.boundary.damage.initial_inhibit;
    if (!nm_player_early_mode(p,&s,1)) return 0;
    tick.dynamics.speed_index=s.speed_index;
    if (!nm_player_prepare_mode(p,&tick,&s,1) ||
        !nm_player_longitudinal_step(p,&tick.dynamics,&s,1)) return 0;
    if (s.boundary.damage.spin_ticks) {
        if (!nm_player_planar_motion(p,&tick.dynamics,&s) ||
            !nm_wheels_ground(contact,p,&s.boundary.ground)) return 0;
    } else {
        const unsigned branch=nm_player_finish_branch(&s);
        if (branch==NM_FINISH_GUIDED) {
            const unsigned profile=(uint16_t)s.phase[0];
            if (profile>=road->profile_count ||
                !nm_player_guided_motion(p,path,road->profiles+profile,&tick.dynamics,&s)) return 0;
            const int16_t pitch=s.boundary.ground.pitch,roll=s.boundary.ground.roll;
            if (!nm_wheels_ground(contact,p,&s.boundary.ground)) return 0;
            s.boundary.ground.pitch=pitch;s.boundary.ground.roll=roll;
        } else if (!nm_player_planar_motion(p,&tick.dynamics,&s) ||
                   !nm_wheels_ground(contact,p,&s.boundary.ground)) return 0;
        if (!nm_player_finish_heading(&s,branch)) return 0;
    }
    if (!nm_contact_scenery_cell(s.boundary.ground.position[0],s.boundary.ground.position[2],&s.scenery_cell) ||
        !nm_player_road_contact_step(p,contact,road,path,&tick.dynamics,&s) ||
        !nm_player_body_angles_step(p,s.tilt[0],&s)) return 0;
    if (s.boundary.damage.auxiliary_request) s.auxiliary_flags|=0x80000000u;
    nm_player_spin_pose(&s); /* Source reaches dbec for every finish branch. */
    NM_COPY(state,&s);
    return 1;
}

/*13a14 ->10bac ->13de0 ->13e3c. Reuse the checked lateral operation and DNR2
 * profiles. All validation/conversion precedes writes to car or shared marks. */
static inline int nm_player_road_tail(const nm_drive_params_t *p, const nm_road_pack_t *road,
        nm_road_marks_t *marks, nm_player_t *s) {
    if (!p || !road || !road->profiles || road->profile_count < 2 || road->profile_count > 4097 ||
        !marks || !marks->values || marks->count != road->profile_count-1 || !s ||
        (uint16_t)s->phase[0] >= road->profile_count) return 0;
    int16_t lateral;
    if (!nm_road_measure_lateral(p,&road->profiles[(uint16_t)s->phase[0]],
            s->boundary.ground.position[0],s->boundary.ground.position[2],&lateral)) return 0;
    /* Marks are the final fallible operation; after its validated writes no
     * operation may reject without deferring publication of shared state. */
    if (!nm_road_marks_step(marks,(uint16_t)s->phase[0],s->speed_index,
            s->input_inhibit,s->boundary.damage.inhibit_ticks)) return 0;
    s->lateral.current = lateral;
    nm_player_road_progress(road,s);
    return 1;
}

/* 637c (preset NULL) and 62b4 (the selected view's preset): the auxiliary object 501270 stops (flags bit31),
 * camera mode 5014a4 := the preset's mode (restored_camera_mode, byte +13), transition 501713 := 0, and the
 * camera words 501720.. and angles 501738.. reset. 637c takes all of them from the reset record 28b5710..;
 * 62b4 takes the eye height and forward, the pitch, 501730 and the focus from the preset (x, yaw, roll and
 * 50172c from the reset record). Callers: the rolling start (race_start.h, ab78..ad30: 62b4), the pit release
 * def4..df24 / dfd4..e070 (pit.h: 637c below view 2, else 62b4) and the crash camera's end (damage.h: 637c). */
static inline void nm_player_view_reset(const nm_drive_params_t *p, nm_player_t *player, int8_t restored_camera_mode,
        const nm_race_view_preset_t *preset) {
    player->auxiliary_flags&=UINT32_C(0x7fffffff);
    player->camera_mode=preset ? preset->mode : restored_camera_mode;player->camera_transition=0;
    nm_memcpy(player->camera_words,p->camera_reset_words,sizeof(player->camera_words));
    nm_memcpy(player->camera_angles,p->camera_reset_angles,sizeof(player->camera_angles));
    if(preset) {
        player->camera_words[1]=preset->words[0];player->camera_words[2]=preset->words[1];
        player->camera_words[4]=preset->words[2];player->camera_words[5]=preset->words[3];
        player->camera_angles[0]=preset->pitch;
    }
}

/* e078: shared circle output really uses X twice, not X/Z. */
static inline void nm_player_circle_step(nm_player_t *s) {
    nm_memcpy(&s->proximity_circle[0],&s->boundary.ground.position[0],sizeof(float));
    nm_memcpy(&s->proximity_circle[1],&s->boundary.ground.position[0],sizeof(float));
    s->proximity_circle[2] = 8.0f;
}

/*d920/d9f8 through e0a4. Internal non-null state; no failure after shared road
 * marks have been published. Preserve wide heading subtraction, signed lap
 * bytes and the unusual comparison of previous path flags against LAP.
 * Circle setup really uses X twice, not X/Z; pass this explicit output to
 * later native consumers instead of keeping a target TGP/FIFO state machine. */
static inline void nm_player_end_step(nm_player_t *s) {
    s->route_transition = 0;
    if (s->held) {
        const int delta = nm_yaw_angle((uint32_t)((int)s->boundary.ground.yaw-s->body_yaw));
        const int step = delta >= 0 ? delta/4 : -((-delta+3)/4);
        s->body_yaw = nm_yaw_angle((uint32_t)((int)s->body_yaw+step));
    } else {
        const int lap = s->lap.lap < 128 ? (int)s->lap.lap : (int)s->lap.lap-256;
        const int total = s->checkpoints.total_laps < 128 ? (int)s->checkpoints.total_laps : (int)s->checkpoints.total_laps-256;
        const int angle = (int)s->boundary.heading-s->boundary.ground.road_heading+0x3fff;
        s->route_transition = (s->path_flags&128) && lap < total-1 &&
            (uint32_t)lap != s->previous_path_flags && angle >= 0xfff && angle <= 0x5fff &&
            !s->boundary.damage.inhibit_ticks && !s->boundary.condition &&
            !s->boundary.damage.spin_ticks && !s->input_inhibit && !s->response_ticks;
    }
    nm_player_circle_step(s);
}

/* d328 gameplay tick, excluding UI writes. The event changes the
 * caller's mode AFTER this tick. Phase-disabled calls execute only e078.
 * Spin follows dbd0 and bypasses the release/initial-inhibit reset. */
static inline int nm_player_rolling_tick(const nm_drive_params_t *p, const nm_contact_pack_t *contact,
        const nm_road_pack_t *road, const nm_path_pack_t *path,
        const nm_player_active_input_t *in, nm_player_t *state,
        uint32_t *start_flags, uint8_t *released) {
    if (!in || !state || !start_flags || !released) return 0;
    nm_player_t s; NM_COPY(&s,state);
    uint32_t flags = *start_flags;
    uint8_t event = 0;
    if (in->dynamics.speed_flags & UINT32_C(0x19f800fc)) {
        const int branch = nm_player_rolling_prefix(p,contact,road,path,in,&s);
        if (!branch) return 0;
        if (branch == 1) {
            event = (uint8_t)nm_player_rolling_release(&s,&flags);
            s.boundary.damage.initial_inhibit = 0;
        }
    }
    nm_player_circle_step(&s);
    NM_COPY(state,&s); *start_flags = flags; *released = event;
    return 1;
}

/* da18/da84 through e0a4. Entry and timer hold occur only on the first
 * ENABLED call; disabled phases still publish the shared X/X/radius circle.
 * No guest updater pointer: entered is the native da18->da84 transition.
 * Failure publishes neither partial physics nor the entry/hold transition. */
static inline int nm_player_finish_tick(const nm_drive_params_t *p,const nm_contact_pack_t *contact,
        const nm_road_pack_t *road,const nm_path_pack_t *path,
        const nm_player_active_input_t *in,nm_player_t *state,uint8_t *entered,uint32_t *hold) {
    if(!in || !state || !entered || *entered>1 || !hold) return 0;
    nm_player_t s; NM_COPY(&s,state);uint8_t started=*entered;uint32_t timer_hold=*hold;
    if(in->dynamics.speed_flags&UINT32_C(0x19f800fc)) {
        if(!started) {
            if(!nm_player_finish_entry(&s)) return 0;
            started=1;timer_hold=1;
        }
        if(!nm_player_finish_prefix(p,contact,road,path,in,&s)) return 0;
    }
    nm_player_circle_step(&s);
    NM_COPY(state,&s);*entered=started;*hold=timer_hold;return 1;
}

/* Active-player block frome5f4 through e0a4 (normal and held).
 * Earlier collision/race gates and other entity modes remain outside this
 * block. No board scheduler or snapshot refresh; one atomic native tick. */
static inline int nm_player_active_step(const nm_drive_params_t *p, const nm_contact_pack_t *contact,
        const nm_road_pack_t *road, const nm_path_pack_t *path,
        const nm_player_active_input_t *in, nm_player_t *state, nm_road_marks_t *marks) {
    if (!p || !contact || !in || !state || in->dynamics.course_slot != contact->slot) return 0;
    nm_player_t s; NM_COPY(&s,state);
    nm_player_active_input_t tick; NM_COPY(&tick,in);
    s.lap_write.written = 0;
    if (!nm_player_early_step(p,&s)) return 0;
    tick.dynamics.speed_index = s.speed_index;
    if (!nm_player_prepare_step(p,&tick,&s)) return 0;
    if (!s.held) {
        if (!nm_player_step(p,contact,road,path,&tick.dynamics,&s)) return 0;
        if (s.boundary.damage.auxiliary_request) s.auxiliary_flags |= 0x80000000u;
        if (!nm_player_offcourse_step(p,contact,tick.force_recovery,&s)) return 0;
        uint16_t previous = (uint16_t)s.phase[1];
        if (!nm_checkpoint_step(road,(uint16_t)s.phase[0],&previous,&s.boundary.damage.flags,
                &s.lap,&s.checkpoints,&s.lap_write)) return 0;
        s.phase[1] = nm_yaw_angle(previous);
    } else {
        nm_motion_input_t mi; nm_motion_state_t ms;
        nm_memset(&mi,0,sizeof(mi)); nm_memset(&ms,0,sizeof(ms));
        mi.held = 1; mi.hold_rotation = s.boundary.recovery_rotation;
        nm_memcpy(mi.displacement,s.boundary.displacement,sizeof(mi.displacement));
        nm_memcpy(ms.position,s.boundary.ground.position,sizeof(ms.position));
        nm_memcpy(ms.delta,s.delta,sizeof(ms.delta)); nm_memcpy(ms.lookahead,s.lookahead,sizeof(ms.lookahead));
        ms.speed = s.boundary.speed; ms.heading = s.boundary.heading; ms.yaw = s.boundary.ground.yaw;
        if (!nm_motion_step(p,&mi,&ms)) return 0;
        nm_memcpy(s.boundary.ground.position,ms.position,sizeof(ms.position));
        s.boundary.heading = ms.heading; s.boundary.ground.yaw = ms.yaw;
        if (!nm_wheels_ground(contact,p,&s.boundary.ground)) return 0;
        if (!nm_contact_scenery_cell(s.boundary.ground.position[0],s.boundary.ground.position[2],&s.scenery_cell)) return 0;
    }
    if (!nm_player_body_angles_step(p,s.tilt[0],&s)) return 0;
    if (!s.held && !nm_player_tilt_step(p,tick.dynamics.speed_flags,&s)) return 0;
    if (!s.held && !nm_player_road_tail(p,road,marks,&s)) return 0;
    nm_player_end_step(&s);
    NM_COPY(state,&s);
    return 1;
}
/*d834..e0a4: consume the previous tick's checkpoint event BEFORE controls/
 * physics. Award time feeds the very same tick's input counter; spin enable
 * is latched before lifecycle changes it. Earlier enable/diagnostic gates are
 * still the caller's responsibility. No publication on a later rejection. */
static inline int nm_player_tick_step(const nm_drive_params_t *p, const nm_contact_pack_t *contact,
        const nm_road_pack_t *road, const nm_path_pack_t *path, const nm_player_tick_input_t *in,
        nm_player_t *state, nm_checkpoint_award_t *award, nm_road_marks_t *marks) {
    if (!p || !contact || !road || !in || !state || !award) return 0;
    nm_player_t s; NM_COPY(&s,state); nm_checkpoint_award_t a = *award;
    nm_player_active_input_t tick = in->active;
    const int checkpoint = !!(s.boundary.damage.flags&(1u<<21));
    s.boundary.damage.flags &= ~(1u<<21); a.cue_count = 0;
    if (checkpoint && in->phase == 22 && !(in->award.blocked&1)) {
        nm_checkpoint_award_input_t input = in->award;
        input.player_flags = s.boundary.damage.flags; input.path_flags = s.path_flags;
        if (!nm_checkpoint_award_step(road->awards,&input,&a,&s.boundary.pivot_signal)) return 0;
    }
    tick.input_counter = nm_gears_signed(a.remaining);
    tick.dynamics.fixed_offset = s.boundary.damage.spin_ticks;
    if (!nm_player_active_step(p,contact,road,path,&tick,&s,marks)) return 0;
    *award = a; NM_COPY(state,&s);
    return 1;
}
#endif
