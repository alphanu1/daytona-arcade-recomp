#ifndef DAYTONA_NATIVE_FORCES_H
#define DAYTONA_NATIVE_FORCES_H
#include "force_math.h"

typedef struct { float resistance; uint8_t override_count; } nm_resistance_state_t;
/*1132c: signed byte decrement, but select the table using the WIDE result
 * before its byte store wraps. In particular -128 becomes127, selects slot4. */
static inline int nm_resistance_step(const nm_drive_params_t *p, nm_resistance_state_t *state) {
    if (!p || !state) return 0;
    int count = state->override_count < 128 ? state->override_count : (int)state->override_count-256;
    unsigned slot = 0;
    if (count) { --count; slot = count >= 0 && count <= 2 ? (unsigned)count+1 : 4; }
    if (!isfinite(p->resistance[slot])) return 0;
    state->resistance = p->resistance[slot]; state->override_count = (uint8_t)count;
    return 1;
}

/*11304 opponent entry: an active road mark overrides resistance without
 * consuming the per-car override counter. Timer alone selects slot1.
 * Caller resolves the current profile's signed mark from bounded road data. */
static inline int nm_resistance_marked_step(const nm_drive_params_t *p,
        uint32_t timer, int8_t mark, nm_resistance_state_t *state) {
    if (!p || !state || mark<0 || mark>4) return 0;
    if (!timer && !mark) return nm_resistance_step(p,state);
    const unsigned slot=mark ? (unsigned)mark : 1u;
    if (!isfinite(p->resistance[slot])) return 0;
    state->resistance=p->resistance[slot];
    return 1;
}

typedef struct {
    float position[2], neighbor[3][2]; /* X/Z, in the source linked-neighbor order. */
    int16_t heading;
    uint8_t disabled;
} nm_proximity_input_t;
/*11370..11434: update the drag timer from at most three ordered neighbors.
 * No target entity pointers, TGP dispatcher, nearest-neighbor search or heap. */
static inline int nm_proximity_step(const nm_drive_params_t *p, const nm_proximity_input_t *in,
                                    int8_t *drag_count) {
    if (!p || !in || !drag_count) return 0;
    if (in->disabled) return 1;
    if (!isfinite(in->position[0]) || !isfinite(in->position[1])) return 0;
    int count = *drag_count;
    if (count > 0) --count;
    for (unsigned n = 0; n < 3; ++n) {
        if (!isfinite(in->neighbor[n][0]) || !isfinite(in->neighbor[n][1])) return 0;
        const float x = in->position[0]-in->neighbor[n][0];
        const float z = in->neighbor[n][1]-in->position[1];
        const float distance = nm_force_length(p,x,z);
        /* Reference compares signed FLOAT BITS, not a numerical float. */
        const uint32_t bits = nm_force_bits(distance);
        if (!(bits & 0x80000000u) && bits >= 0x41f00000u) continue;
        const int32_t angle = nm_force_angle(p,z,x);
        if (angle == INT32_MIN) return 0;
        const unsigned wrapped = ((uint32_t)angle-(uint32_t)(int32_t)in->heading)&65535;
        const int delta = wrapped < 32768 ? (int)wrapped : (int)wrapped-65536;
        if (delta > -910 && delta < 910) { count = 64; break; }
    }
    *drag_count = (int8_t)count;
    return 1;
}

typedef struct {
    nm_drive_terms_t terms;
    nm_axle_state_t axle;
    float resistance; /* Entity164, distinct from the drive drag term160. */
    int32_t speed_index, rpm;
    uint32_t gear;
    int16_t bank, steer;
    int8_t condition_99, condition_ec, condition_15c, condition_265;
} nm_forces_input_t;
typedef struct {
    float inertial[2], grip[2], rear_bank_force, slip_decay, slip_ratio, longitudinal_delta;
    int16_t steer_angle, tire_angle[2];
    int8_t trigger_count[2]; /* Entity95/96; signed countdown checks. */
    uint32_t rpm_boost;
    uint32_t trigger_pair; /* Output event: request the two auxiliary objects, once. */
} nm_forces_state_t;

typedef struct {
    float lateral, inertial[2];
    uint32_t gear;
    int16_t steer_angle, tire_angle, phase[2], speed_index, steer, pitch;
    uint16_t contact[4], surface, accel, brake;
    uint8_t course_slot, override_count;
} nm_traction_input_t;
typedef struct {
    float previous_lateral, drive, rear_load, slope;
    int16_t condition_26c;
    int8_t countdown[6]; /* Entity264..269; signed checks, ordered updates. */
} nm_traction_state_t;

/* Full0x10568's persistent effects: modifies supplied drive/load/contact state,
 * does NOT find wheel/road collisions. Its discarded distance conversions
 * (1c4/1c8 -> g4) never reach memory or the only caller's next operation.
 * Keep ordered timer overrides; later timers can replace earlier results. */
static inline int nm_traction_step(const nm_drive_params_t *p, const nm_traction_input_t *in,
                                   nm_traction_state_t *state) {
    if (!p || !in || !state) return 0;
    if (in->course_slot == 3) return 1;
    if (!isfinite(in->lateral) || !isfinite(in->inertial[0]) || !isfinite(in->inertial[1]) ||
        !isfinite(state->previous_lateral) || !isfinite(state->drive) ||
        !isfinite(state->rear_load) || !isfinite(state->slope)) return 0;
    nm_traction_state_t result = *state;
    const int steer = in->steer_angle;
    const int aligned = (steer < 0) == (in->tire_angle < 0);
    const int magnitude = aligned ? (steer < 0 ? -steer : steer) : 0;
    const float change = in->lateral-state->previous_lateral;
    result.previous_lateral = in->lateral;
    float lateral = 0;
    if (!((nm_force_bits(change)^nm_force_bits(in->lateral))&0x80000000u) &&
        in->phase[0] == in->phase[1] &&
        ((nm_force_bits(change)^((uint32_t)(int32_t)in->tire_angle))&0x80000000u) &&
        (in->tire_angle < -2048 || in->tire_angle >= 2048)) lateral = change;
    if (in->speed_index >= 70 && ((nm_force_bits(in->inertial[0])^nm_force_bits(in->inertial[1]))&0x80000000u) &&
        (in->steer >= 4096 || in->steer < -4096)) result.countdown[1] = 32;
    if ((in->contact[0]&in->contact[1]&in->contact[2]&in->contact[3])&4) result.countdown[1] = 48;
    if (in->speed_index >= 150 && fabsf(lateral) >= 0.2f && (in->surface&2)) result.countdown[2] = 1;
    if (in->override_count) result.countdown[3] = 16;
    if (in->pitch >= 1536 && (((in->contact[2]&4) && steer < 0) || ((in->contact[3]&4) && steer > 0)))
        result.countdown[1] = 64;
    if (!in->gear && !in->speed_index) {
        if (in->accel >= 250) { result.countdown[4] = 16; result.countdown[5] = 0; }
        if (in->brake >= 250 && in->accel >= 200 && in->accel <= 220) {
            result.countdown[5] = 32; result.countdown[4] = 0;
        }
    }
    const int count = result.countdown[0] + (in->speed_index >= 100 && magnitude >= 3584);
    if (count > 0) {
        result.countdown[0] = (int8_t)(count-1);
        const float load_squared = result.rear_load*result.rear_load;
        const float lateral_squared = in->inertial[1]*in->inertial[1];
        const float drive_squared = result.drive*result.drive;
        const float sum = drive_squared+lateral_squared;
        const float ratio = nm_force_divide(p,load_squared,sum);
        float limit = result.drive*ratio;
        if (nm_force_bits(limit)&0x80000000u) limit = 0;
        if (result.drive < 0) result.drive = 0;
        else if (!(limit >= result.drive)) result.drive = limit;
    }
    if (result.countdown[1] > 0) { --result.countdown[1]; result.condition_26c = 0; }
    if (result.countdown[2] > 0) { --result.countdown[2]; result.rear_load = 1.03f*result.rear_load; }
    if (result.countdown[3] > 0) { --result.countdown[3]; result.rear_load = p->axle.maximum[1]; }
    if (result.countdown[4] > 0) { --result.countdown[4]; result.rear_load = 0.5f*result.rear_load; result.slope = 0; }
    if (result.countdown[5] > 0) {
        --result.countdown[5];
        if (!(nm_force_bits(result.drive)&0x80000000u)) result.slope = -result.drive;
    }
    /* Reject nonfinite published state, not an intermediate later clamped out.
     * Some synthetic zero-force/active-timer inputs make the reference NaN. */
    if (!isfinite(result.drive) || !isfinite(result.rear_load) || !isfinite(result.slope)) return 0;
    *state = result;
    return 1;
}

/* Full0xf31c, including0xf830 and the state/event effects of10480/104ec.
 * One reference tick; no scheduler, board runtime, allocation or sound.
 * Auxiliary object scheduling is an explicit event, not guest code pointers.
 * Contact solving, yaw and position integration are separate operations.
 * The drag term may be NaN (a negative speed index, nm_drive_drag_bits): the
 * TGP work runs on the NaN-capable operations of force_math.h and the i960's own
 * float compares keep their unordered outcome (cmpr: no branch condition holds):
 * f4c4/f614 `ble` keep 1.0 only for an ordered magnitude <= load, so a NaN
 * magnitude takes the ratio; f650 `bg` skips the trigger only for an ordered
 * ratio > threshold; f968/f980 clamp a NaN grip to 0.1. The NaN longitudinal
 * delta reaches 10338, whose sign test (0xffffffff) zeroes speed and delta. */
static inline int nm_forces_step(const nm_drive_params_t *p, const nm_forces_input_t *in,
                                 nm_forces_state_t *state) {
    if (!p || !in || !state || in->gear >= 4 || !isfinite(in->resistance) ||
        !isfinite(in->terms.drive) || !isfinite(in->terms.slope) || nm_is_inf(in->terms.drag) ||
        !isfinite(state->slip_decay) || !isfinite(state->slip_ratio)) return 0;
    for (unsigned a = 0; a < 2; ++a)
        if (!isfinite(in->terms.brake[a]) || !isfinite(in->axle.load[a]) || !isfinite(state->inertial[a])) return 0;
    nm_forces_state_t result = *state;
    result.trigger_pair = 0;
    float cart[2][2];
    const uint16_t angle = (uint16_t)(-(int)in->bank);
    const float cosine = -nm_drive_sine(p,(uint16_t)(angle+16384u));
    const float sine = nm_drive_sine(p,angle);
    for (unsigned a = 0; a < 2; ++a) {
        const float base = sine * p->axle.bank[a];
        float lateral = state->inertial[a] * cosine;
        lateral = lateral + base;
        if (a) result.rear_bank_force = lateral;
        const float brake = a ? in->terms.brake[1]-in->terms.drive : in->terms.brake[0];
        nm_force_polar_t forward = {0,brake}, backward = {-32768,in->terms.drag};
        const nm_force_polar_t side = {16384,lateral};
        nm_force_polar_t combined;
        if (!nm_force_combine_nan(p,&forward,&backward,&combined) ||
            !nm_force_combine_nan(p,&side,&combined,&combined)) return 0;
        const float ratio = nm_force_divide_nan(p,in->axle.load[a],combined.magnitude);
        const int over = nm_is_nan(combined.magnitude) || combined.magnitude > in->axle.load[a];
        result.grip[a] = over ? ratio : 1.0f;
        if (a && over &&
            combined.angle > -8192 && combined.angle < 8192 &&
            (nm_is_nan(ratio) || ratio <= p->slip_threshold) && in->condition_15c <= 0 &&
            result.trigger_count[0] <= 4 && in->speed_index > 20) {
            result.trigger_count[0] = 24;
            result.trigger_pair = 1;
        }
        forward.magnitude = -brake;
        backward.magnitude = nm_nan_add(in->terms.slope,in->terms.drag);
        if (!nm_force_combine_nan(p,&forward,&backward,&combined) ||
            !nm_force_combine_nan(p,&side,&combined,&combined)) return 0;
        nm_force_cartesian_nan(p,(uint16_t)combined.angle,combined.magnitude,cart[a]);
    }
    const int steering = -(int)in->steer;
    result.steer_angle = (int16_t)(steering < 0 ? -((-steering+7)/8) : steering/8);
    if (in->condition_99) {
        const int negative = -(int)state->tire_angle[1];
        const int delta = negative < 0 ? -((-negative+3)/4) : negative/4;
        for (unsigned a = 0; a < 2; ++a) {
            const unsigned wrapped = (uint16_t)((int)state->tire_angle[a]+delta);
            result.tire_angle[a] = (int16_t)(wrapped < 32768 ? (int)wrapped : (int)wrapped-65536);
            result.inertial[a] = 0;
        }
    }
    if (in->condition_ec) {
        result.steer_angle = result.tire_angle[0] = result.tire_angle[1] = 0;
        result.inertial[0] = result.inertial[1] = 0;
        result.slip_decay = -1;
    }
    result.rpm_boost = 0;
    /* The helper tests the signed LOW16 speed, unlike104ec's full32 speed. */
    const unsigned low_speed = (uint16_t)in->speed_index;
    if (low_speed > 0 && low_speed < 32768) {
        const float rpm = (float)in->rpm;
        float slip = 8500.0f-rpm;
        slip = nm_force_divide(p,slip,8500.0f);
        slip = in->terms.drive * slip;
        const float load = result.grip[1] * in->axle.load[1];
        slip = nm_force_divide(p,slip,load);
        result.slip_ratio = nm_force_divide(p,p->slip_gear[in->gear],slip);
        if (slip >= p->slip_gear[in->gear]) {
            if (result.trigger_count[1] <= 4) {
                result.trigger_count[1] = result.trigger_count[1] ? 24 : 32;
                result.trigger_pair = 1;
            }
            if (in->condition_265 <= 0) {
                result.rpm_boost = (uint32_t)in->rpm << 1;
                result.slip_decay = nm_force_divide(p,rpm,9000.0f);
            }
        }
    }
    float grip = nm_nan_sub(result.grip[1],result.slip_decay);
    grip = nm_is_nan(grip) || grip < 0.1f ? 0.1f : grip > 1.0f ? 1.0f : grip;
    if (!(nm_force_bits(result.slip_decay)&0x80000000u)) result.slip_decay -= 0.01f;
    result.grip[1] = grip;
    const float front = nm_nan_mul(cart[0][1],result.grip[0]);
    float acceleration = nm_nan_mul(cart[1][1],result.grip[1]);
    acceleration = nm_nan_add(acceleration,front);
    acceleration = nm_nan_sub(acceleration,in->resistance);
    result.longitudinal_delta = nm_nan_mul(acceleration,p->acceleration_scale);
    /* NaN only from a NaN drag term (grip[0] and the delta, consumed this tick). */
    const int nan_drag = nm_is_nan(in->terms.drag);
#define NM_FORCES_BAD(x) (nm_is_inf(x) || (nm_is_nan(x) && !nan_drag))
    if (NM_FORCES_BAD(result.grip[0]) || !isfinite(result.grip[1]) ||
        !isfinite(result.rear_bank_force) || !isfinite(result.slip_decay) ||
        !isfinite(result.slip_ratio) || NM_FORCES_BAD(result.longitudinal_delta)) return 0;
#undef NM_FORCES_BAD
    *state = result;
    return 1;
}
#endif
