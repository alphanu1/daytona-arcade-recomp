#ifndef DAYTONA_NATIVE_YAW_H
#define DAYTONA_NATIVE_YAW_H
#include "force_math.h"

typedef struct {
    float speed;
    int16_t steer_angle, tire_angle[2], steer, speed_index, road_heading;
    uint8_t course_slot, fixed_offset;
} nm_yaw_input_t;
typedef struct {
    float inertial[2];
    uint32_t alignment_frames;
    int16_t yaw, heading, body_yaw, yaw_delta, offset;
} nm_yaw_state_t;

static inline int16_t nm_yaw_angle(uint32_t value) {
    const unsigned low = value & 65535;
    return (int16_t)(low < 32768 ? (int)low : (int)low-65536);
}

/* Persistent effects of0xf9d0..fdf4, one reference tick. Angles wrap only where the source
 * stores16 bits; intermediate sums/comparisons stay wide. offset is the
 * explicit native equivalent of shared5018a4, fixed_offset of5018a8.
 * Params are validated at load. The rotation's cvtri (fbd0) is masked: NaN
 * (straight driving at rest) gives INT32_MIN, and +-inf (equal front and rear
 * wheel angles in motion: the turn radius divide wraps) saturates
 * (nm_drive_round_i32); nonfinite published state still rejects.
 * Does not integrate XYZ, solve wheel contact or consume captured poses. */
static inline int nm_yaw_step(const nm_drive_params_t *p, const nm_yaw_input_t *in,
                              nm_yaw_state_t *state) {
    if (!p || !in || !state || !isfinite(in->speed) || !isfinite(p->turn_scale) ||
        !isfinite(state->inertial[0]) || !isfinite(state->inertial[1])) return 0;
    nm_yaw_state_t result = *state;
    const int front = (int)in->steer_angle+in->tire_angle[0];
    const int quarter = front >= in->tire_angle[1] ? 16384 : -16384;
    const int front_angle = front+quarter, rear_angle = (int)in->tire_angle[1]+quarter;
    const int separation = front_angle-rear_angle;
    const float rear_sine = nm_drive_sine(p,(uint16_t)rear_angle)*p->turn_geometry[0];
    const float separation_sine = nm_drive_sine(p,(uint16_t)separation);
    const float radius = fabsf(nm_force_divide(p,rear_sine,separation_sine));
    /* The source's other separation/cosine sum (g4) is discarded. */
    const float side = nm_drive_sine(p,(uint16_t)front_angle)*radius;
    const float forward = nm_drive_sine(p,(uint16_t)(front_angle+16384))*radius;
    const float center = p->turn_geometry[1]+forward;
    const int32_t direction = nm_force_angle(p,center,side);
    if (direction == INT32_MIN) return 0;
    const float direction_sine = nm_drive_sine(p,(uint16_t)direction);
    float turn_radius = fabsf(nm_force_divide(p,side,direction_sine));
    if (quarter < 0) turn_radius = -turn_radius;
    float rotation = nm_force_divide(p,in->speed,turn_radius);
    rotation = rotation*p->turn_scale;
    int32_t delta;
    if (!nm_drive_round_i32(rotation,&delta)) return 0;
    result.yaw = nm_yaw_angle((uint32_t)(int32_t)state->yaw+(uint32_t)delta);
    result.yaw_delta = nm_yaw_angle((uint32_t)delta);
    int offset = (front_angle == rear_angle ? front_angle : direction)-quarter;
    if (in->fixed_offset == 1) offset = state->offset;
    else result.offset = nm_yaw_angle((uint32_t)offset);
    const int heading = (int)result.yaw+offset;
    const int heading_change = heading-state->heading;
    result.heading = nm_yaw_angle((uint32_t)heading);
    float target = nm_drive_sine(p,(uint16_t)heading_change)*in->speed;
    target = p->inertial_scale[0]*target;
    /* The intervening delta*scale result is overwritten by the source. */
    target = p->inertial_scale[1]*target;
    for (unsigned a = 0; a < 2; ++a) {
        const float change = target-state->inertial[a];
        const float scaled = change*0.0625f;
        result.inertial[a] = state->inertial[a]+scaled;
        if (!isfinite(result.inertial[a])) return 0;
    }
    const int difference = nm_yaw_angle((uint32_t)((int)result.yaw-result.heading));
    const int blend = difference < 0 ? -((-difference+3)/4) : difference/4;
    const int steer = in->steer < 0 ? -((-(int)in->steer+31)/32) : in->steer/32;
    result.body_yaw = nm_yaw_angle((uint32_t)((int)result.heading+blend+steer));
    if (in->course_slot != 3 && in->speed_index > 0) {
        const int error = (int)result.yaw-in->road_heading;
        if (error < -910 || error > 910) result.alignment_frames = 0;
        else {
            ++result.alignment_frames;
            if (result.alignment_frames >= 4) {
                int adjustment = (int)in->road_heading-result.heading;
                adjustment = adjustment < -16 ? -16 : adjustment > 16 ? 16 : adjustment;
                result.yaw = nm_yaw_angle((uint32_t)((int)result.yaw+adjustment));
            }
        }
    }
    *state = result;
    return 1;
}
#endif
