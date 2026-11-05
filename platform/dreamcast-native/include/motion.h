#ifndef DAYTONA_NATIVE_MOTION_H
#define DAYTONA_NATIVE_MOTION_H
#include "yaw.h"

typedef struct {
    float position[3], delta[2], lookahead[2], speed;
    int16_t heading, yaw;
} nm_motion_state_t;
typedef struct {
    float scale, displacement[3];
    uint32_t flags;
    int32_t speed_index;
    int16_t guided_ticks, spin_heading, spin_rotation, hold_rotation;
    uint8_t spin_ticks, held;
} nm_motion_input_t;

/* ea04 normal/spin paths and ec44 held movement, one reference tick.
 * The guided ea04 ->10ccc road-following path is NOT implemented here:
 * reject it without mutation, never silently substitute planar driving.
 * Contact/Y for ordinary driving is supplied by the separate road update.
 * Returns1 only after all consumed arithmetic is valid; no allocation/bus. */
static inline int nm_motion_step(const nm_drive_params_t *p, const nm_motion_input_t *in,
                                 nm_motion_state_t *state) {
    if (!p || !in || !state) return 0;
    nm_motion_state_t result = *state;
    if (in->held) {
        result.yaw = nm_yaw_angle((uint32_t)((int)state->yaw+in->hold_rotation));
        result.heading = result.yaw;
        for (unsigned a = 0; a < 3; ++a) {
            if (!isfinite(in->displacement[a]) || !isfinite(state->position[a])) return 0;
            result.position[a] = in->displacement[a]+state->position[a];
            if (!isfinite(result.position[a])) return 0;
        }
    } else {
        if (in->spin_ticks > 32 || (!in->spin_ticks && in->guided_ticks > 0) ||
            !isfinite(state->speed) || !isfinite(state->position[0]) || !isfinite(state->position[2])) return 0;
        float distance;
        if (in->spin_ticks) {
            const int rotation = in->spin_rotation/48;
            result.yaw = nm_yaw_angle((uint32_t)((int)state->yaw+
                ((in->flags&0x00c00000u) ? rotation : -rotation)));
            result.heading = in->spin_heading;
            result.speed = p->spin_scale[in->spin_ticks-1]*state->speed;
            distance = in->speed_index > 30 ? result.speed : p->spin_minimum;
            if (!isfinite(result.speed)) return 0;
        } else {
            if (!isfinite(in->scale)) return 0;
            distance = in->scale*state->speed;
        }
        if (!isfinite(distance)) return 0;
        const uint16_t angle = (uint16_t)(-(int)result.heading);
        nm_force_cartesian(p,angle,distance,result.delta);
        for (unsigned a = 0; a < 2; ++a) {
            result.position[2*a] = result.delta[a]+state->position[2*a];
            if (!isfinite(result.delta[a]) || !isfinite(result.position[2*a])) return 0;
        }
        if (!in->spin_ticks) {
            const float ahead = 16.0f*distance;
            if (!isfinite(ahead)) return 0;
            nm_force_cartesian(p,angle,ahead,result.lookahead);
            for (unsigned a = 0; a < 2; ++a) {
                /* Source origin is the OLD position, not the updated one. */
                result.lookahead[a] = result.lookahead[a]+state->position[2*a];
                if (!isfinite(result.lookahead[a])) return 0;
            }
        }
    }
    *state = result;
    return 1;
}
#endif
