#ifndef DAYTONA_NATIVE_GEARS_H
#define DAYTONA_NATIVE_GEARS_H
#include "force_math.h"

typedef struct {
    int32_t speed_index, rpm_bias;
    uint8_t manual, selector; /* Explicit mode and three selector bits, not I/O RAM. */
} nm_gears_input_t;
typedef struct {
    uint32_t gear, brake;
    int32_t rpm, filtered_rpm, downshift_rpm;
    int8_t shift_ticks;
} nm_gears_state_t;

static inline int32_t nm_gears_signed(uint32_t bits) {
    return bits <= INT32_MAX ? (int32_t)bits : -1-(int32_t)(~bits);
}

/* Full automatic11d60..11e7c and manual11e80..12064 persistent effects.
 * Tables validated at load. No simulated I/O, timer, allocation or sound.
 * Invalid gear/selector or unrepresentable conversion never publishes state. */
static inline int nm_gears_step(const nm_drive_params_t *p, const nm_gears_input_t *in,
                                nm_gears_state_t *state) {
    if (!p || !in || !state || state->gear >= 4 || in->selector >= 8) return 0;
    nm_gears_state_t result = *state;
    if (result.shift_ticks) {
        const int down = result.shift_ticks < 0;
        result.shift_ticks += down ? 1 : -1;
        if (in->manual && down) {
            result.rpm = result.downshift_rpm;
            result.downshift_rpm = nm_gears_signed((uint32_t)result.downshift_rpm-250u);
        }
    }
    const unsigned old = result.gear;
    if (!in->manual) {
        if (in->speed_index && result.rpm > p->gear_up[old]) {
            if (old == 3 || !isfinite(p->gear_shift_scale[old])) return 0;
            ++result.gear; result.shift_ticks = 16;
            if (!nm_drive_round_i32(p->gear_shift_scale[old] * (float)result.rpm,&result.rpm)) return 0;
        } else if (result.rpm < p->gear_down[old]) {
            if (!old || !isfinite(p->gear_shift_scale[old-1])) return 0;
            --result.gear; result.shift_ticks = -16;
            if (!nm_drive_round_i32(nm_force_divide(p,(float)result.rpm,p->gear_shift_scale[old-1]),&result.rpm)) return 0;
        }
    } else {
        const int selected = p->gear_selector[in->selector];
        if (selected < -1 || selected > 3) return 0;
        if (selected >= 0 && (unsigned)selected != old) {
            if (!isfinite(p->gear[old]) || !isfinite(p->gear[selected])) return 0;
            const float product = p->gear[old] * (float)result.rpm;
            if (!nm_drive_round_i32(nm_force_divide(p,product,p->gear[selected]),&result.rpm)) return 0;
            result.gear = (unsigned)selected;
            result.shift_ticks = selected > (int)old ? 16 : -16;
            if (selected < (int)old) result.downshift_rpm = nm_gears_signed((uint32_t)result.rpm+4000u);
        }
        if (result.rpm > 8500) { result.rpm = 8500; result.brake = 255; }
    }
    int32_t delta = nm_gears_signed((uint32_t)result.rpm + (uint32_t)in->rpm_bias - (uint32_t)result.filtered_rpm);
    const int32_t low = p->gear_rpm_delta[result.gear][0], high = p->gear_rpm_delta[result.gear][1];
    if (delta < low) delta = low;
    else if (delta > high) delta = high;
    result.filtered_rpm = nm_gears_signed((uint32_t)result.filtered_rpm+(uint32_t)delta);
    if (in->manual) {
        if (result.filtered_rpm < 0) result.filtered_rpm = 0;
        else if (result.filtered_rpm > 8500) result.filtered_rpm = 8500;
    }
    *state = result;
    return 1;
}
#endif
