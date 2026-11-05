#ifndef DAYTONA_NATIVE_VEHICLE_H
#define DAYTONA_NATIVE_VEHICLE_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t steer;
    uint8_t accel, brake, accel_high_edge, brake_high_edge; /* Rising bit7, not first nonzero pressure. */
} nm_controls_t;
typedef struct {
    int32_t steer; /* Filtered reference value, not degrees or wheel angle. */
    uint16_t accel, brake;
    uint8_t override_count;
} nm_vehicle_input_t;

/* daytona93 0x4e5c..0x4fe4. ADC bytes presented to that routine; upstream I/O
 * and Dreamcast controller calibration/mapping are separate. Previous pedal
 * values are already decoded. */
static inline nm_controls_t nm_controls_decode(uint8_t steer, uint8_t accel, uint8_t brake,
                                               uint8_t previous_accel, uint8_t previous_brake) {
    int wheel = (int)steer - 128;
    int magnitude = wheel < 0 ? -wheel : wheel;
    magnitude = magnitude <= 5 ? 0 : magnitude > 80 ? 75 : magnitude-5;
    nm_controls_t result;
    result.steer = (wheel < 0 ? -magnitude : magnitude)*255/150;
    const unsigned a = accel < 56 ? 56 : accel > 184 ? 184 : accel;
    const unsigned b = brake < 56 ? 56 : brake > 152 ? 152 : brake;
    result.accel = (uint8_t)((a-56)*255/128);
    result.brake = (uint8_t)((b-56)*255/96);
    result.accel_high_edge = (uint8_t)(((previous_accel ^ result.accel) & result.accel) >> 7);
    result.brake_high_edge = (uint8_t)(((previous_brake ^ result.brake) & result.brake) >> 7);
    return result;
}

/* daytona93 0xe93c..0xea00. inhibited means signed entity[d4]+entity[c8]!=0;
 * counter is global[5010d0], speed_index is entity[18]. Physical units and the
 * inhibit causes are deliberately not guessed. One reference simulation tick.
 * Unsigned sum/explicit sign extension preserve wrapping i960 arithmetic. */
static inline int nm_vehicle_input_step(nm_vehicle_input_t *state, const nm_controls_t *input,
                                        int inhibited, int32_t counter, int32_t speed_index) {
    if (!state || !input || input->steer < -127 || input->steer > 127) return 0;
    const uint32_t sum = ((uint32_t)input->steer << 8) + (uint32_t)state->steer;
    const int32_t filtered = sum & 0x80000000u ? -1 - (int32_t)(~sum >> 1) : (int32_t)(sum >> 1);
    state->steer = filtered;
    if (inhibited) {
        state->accel = state->brake = 0;
        state->steer = filtered < -1820 ? filtered+1820 : filtered > 1820 ? filtered-1820 : 0;
    } else if (counter <= 64) {
        state->accel = 0; state->brake = 255;
        if (speed_index >= 64) ++state->override_count;
    } else {
        state->accel = input->accel; state->brake = input->brake;
    }
    return 1;
}
#endif
