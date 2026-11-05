#ifndef DAYTONA_NATIVE_TIRES_H
#define DAYTONA_NATIVE_TIRES_H
#include "force_math.h"

typedef struct {
    float drive, speed, load[2], front_grip;
    int16_t yaw_delta, speed_index, steer_angle;
} nm_tires_input_t;
typedef struct {
    float rear_grip;
    uint32_t flags, trigger_pair; /* Same auxiliary event as nm_forces_step. */
    int16_t angle[2], filter, condition;
    int8_t trigger_count;
    uint8_t override_count;
} nm_tires_state_t;

/* Persistent effects of0xfdf8..10268 and10480, one reference tick. No wheel
 * collision or world integration. Parameters validated once at pack load;
 * preserve asymmetric front/rear division order and signed-zero decisions.
 * The front grip is NaN after a NaN drag term (nm_forces_step); its TGP work
 * runs on force_math.h's NaN-capable operations and ends in an angle. */
static inline int nm_tires_step(const nm_drive_params_t *p, const nm_tires_input_t *in,
                                nm_tires_state_t *state) {
    if (!p || !in || !state || !isfinite(in->drive) || !isfinite(in->speed) ||
        !isfinite(in->load[0]) || !isfinite(in->load[1]) || nm_is_inf(in->front_grip) ||
        !isfinite(state->rear_grip) || !isfinite(p->tire_response[0]) ||
        !isfinite(p->tire_response[1]) || !isfinite(p->tire_speed_scale) ||
        !isfinite(p->turn_geometry[0])) return 0;
    nm_tires_state_t result = *state;
    result.trigger_pair = 0;
    if (result.trigger_count <= 0 && in->speed_index >= 100 &&
        result.angle[1] >= -768 && result.angle[1] <= 768 &&
        (in->yaw_delta > 216 || in->yaw_delta < -216)) {
        int magnitude = in->yaw_delta < 0 ? -(int)in->yaw_delta : in->yaw_delta;
        magnitude -= 216;
        result.condition = (int16_t)(magnitude < 4 ? 4 : magnitude > 48 ? 48 : magnitude);
    }
    if (result.condition > 0) {
        result.condition -= (nm_force_bits(in->drive)>>31) ? 3 : 1;
        const float factor = 48.0f - (float)result.condition;
        result.rear_grip = nm_force_bits(factor)>>31 ? 0.0f :
            nm_force_divide(p,factor * result.rear_grip,48.0f);
        if (result.trigger_count <= 4) {
            result.trigger_count = result.trigger_count ? 24 : 32;
            result.trigger_pair = 1;
        }
    }
    int32_t yaw;
    if (!nm_drive_round_i32(nm_drive_sine(p,(uint16_t)((int)state->angle[1]+16384)) *
                            (float)in->yaw_delta,&yaw) || yaw == INT32_MIN) return 0;
    yaw = -yaw;
    float force = in->load[1] * result.rear_grip;
    force = p->tire_response[1] * force;
    const float square = in->speed * in->speed;
    const float denominator = square * p->tire_speed_scale;
    const float ratio = nm_force_divide(p,p->turn_geometry[0],denominator);
    int32_t response = nm_force_angle(p,1.0f,ratio * force);
    if (response == INT32_MIN) return 0;
    if (result.trigger_count > 0 && in->speed_index < 70 &&
        (in->steer_angle < 0) != (state->angle[1] < 0)) {
        const int half = in->steer_angle < 0 ? -((-(int)in->steer_angle+1)/2) : in->steer_angle/2;
        if (!nm_drive_round_i32(nm_drive_sine(p,(uint16_t)(half*9+16384)) *
                                (float)response,&response)) return 0;
        if (response < 0) response = 0;
    }
    int desired = -(int)state->angle[1];
    response *= 3;
    if (desired < 0) { response = -response; if (response < desired) response = desired; }
    else if (response > desired) response = desired;
    const int delta = yaw + response/3 - state->filter;
    const int filtered = state->filter + (delta < 0 ? -((-delta+3)/4) : delta/4);
    result.filter = (int16_t)(filtered < -256 ? -256 : filtered > 256 ? 256 : filtered);
    unsigned wrapped = (uint16_t)((int)state->angle[1]+result.filter);
    int rear = wrapped < 32768 ? (int)wrapped : (int)wrapped-65536;

    force = nm_nan_mul(in->load[0],in->front_grip);
    force = nm_nan_mul(p->tire_response[0],force);
    const float quotient = nm_force_divide_nan(p,denominator,force);
    response = nm_force_angle_nan(p,1.0f,nm_force_divide_nan(p,p->turn_geometry[0],quotient));
    if (response == INT32_MIN) return 0;
    response *= 4;
    desired = (int)in->steer_angle - rear;
    if ((in->steer_angle < 0) != (desired < 0)) {
        const int bound = in->steer_angle < 0 ? -4064 : 4064;
        /* MULI keeps low32 bits. Avoid signed C overflow on extreme angles. */
        const uint32_t product = (uint32_t)(bound-in->steer_angle) * (uint32_t)response;
        response = product <= INT32_MAX ? (int32_t)product : -1-(int32_t)(~product);
        response /= bound;
    }
    if (desired < 0) { response = -response; if (response < desired) response = desired; }
    else if (response > desired) response = desired;
    wrapped = (uint16_t)(response/4-desired);
    result.angle[0] = (int16_t)(wrapped < 32768 ? (int)wrapped : (int)wrapped-65536);
    if (in->speed_index > 65) {
        if (rear <= -13312) result.flags |= 0x10002;
        else if (rear >= 13312) result.flags |= 0x8002;
    }
    if (rear <= -13824) { rear = -13824; result.override_count = 1; }
    else if (rear > 13824) { rear = 13824; result.override_count = 1; }
    result.angle[1] = (int16_t)rear;
    if (!isfinite(result.rear_grip)) return 0;
    *state = result;
    return 1;
}
#endif
