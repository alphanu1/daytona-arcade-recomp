// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese
// nm_drive_sine adapts the MAME table-port lookup through runtime/tgp.cpp;
// pinned source and changes are recorded in THIRD_PARTY.md.
#ifndef DAYTONA_NATIVE_DRIVE_H
#define DAYTONA_NATIVE_DRIVE_H
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "finite.h"
#include "nm_mem.h"

typedef struct {
    float bank[2], pitch, transfer, scale[2], minimum[2], maximum[2];
} nm_axle_params_t;
#define NM_CRASH_ROWS 257
/* Legacy name model_key: source row+4/entity5c is raw IEEE754 vertical offset,
 * NOT a model identity. Source7974..79b0 combines it with Y before translation.
 * Preserve bits here; decode only at the validated presentation boundary. */
typedef struct { uint32_t model_key; uint16_t marker; int16_t angle[3]; } nm_crash_row_t;
/* DND27 (DNDR) payload: immutable little-endian parameters/tables, no bus pointers.
 * curve[0] is selector zero; curve[1] is nonzero. Drag tables are base, >2
 * affected wheels, <=2 affected wheels. Sine is a shared quarter-wave table. */
typedef struct {
    float throttle_scale, throttle_offset, gravity, brake[2], gear[4];
    float curve[2][67], drag[3][512];
    float compensation_scale; /* Offline TGP reciprocal for the fixed divisor2000. */
    nm_axle_params_t axle;
    float acceleration_scale, slip_threshold, slip_gear[4];
    float rpm_scale;
    float turn_geometry[2], turn_scale, inertial_scale[2];
    float tire_response[2], tire_speed_scale; /* Front/rear, speed squared. */
    float gear_shift_scale[4];
    float resistance[5];
    float spin_scale[32], spin_minimum;
    float wheel_local[4][3];
    float guided_adjust_limit, guided_player_target[256];
    int32_t gear_down[4], gear_up[4], gear_rpm_delta[4][2];
    int8_t gear_selector[8];
    uint32_t sine[16384];
    uint32_t arctangent[16384], reciprocal[16384], inverse_sqrt[16384];
    uint32_t crash_model[2][16];
    int32_t crash_threshold[2][2][16];
    nm_crash_row_t crash_rows[NM_CRASH_ROWS];
    uint32_t camera_reset_words[6]; /*637c copies these bits, no arithmetic. */
    int16_t camera_reset_angles[3];
    uint16_t camera_reset_reserved;
    uint32_t pivot_divisor[4][256]; /* Numeric divisors for the FULL unsigned lap byte. */
    uint8_t pivot_sides[4];
    float speed_index_scale, inhibit_speed_drop; /* e5f4 scale; folded15ab0 constant subtraction. */
    int16_t shake_angles[2][256]; /* Indexed by byte bits; source uses signed indices. */
    int16_t inhibit_angles[2][256]; /* Indexed by OLD unsigned timer, source144-old folded offline. */
    uint8_t collision_ticks[2][5][2]; /*15238: correction/inhibit, selector, second/first body. */
    uint16_t collision_threshold[3][4]; /*14fcc low/mid/high, player/opponent x player mode. */
    uint32_t collision_flags[2][4]; /* Offline SETBIT masks, first/second x impact side. */
    float collision_speed_step; /* Raw28b573c, NOT reconstructed from the quarter-step. */
    uint8_t collision_corner[4]; /* Source235b08 byte offsets folded to world[] indices. */
    uint16_t collision_signal[4]; /*235b40, indexed in tested world-vertex order. */
    uint8_t collision_schedule[4][14]; /*13ec4 signed[-5,8] offsets folded to window indices. */
    int16_t tilt_tail[2], boost_tilt[4]; /*1550c: two values beyond shake_angles[1], plus boost cycle. */
    float guided_targets[127]; /*10dd8: centered source table, indices[-63,63]. */
    int8_t guided_bounds[256][2], guided_rank_bias[87]; /* Rank delta[-43,43]. */
    uint8_t guided_reserved;
    int16_t opponent_inhibit_angles[512]; /*23543c, wide signed d4+d5 in[-256,254]. */
    int32_t opponent_road_threshold[2][256]; /*235290 signed mode,235284 unsigned mode. */
    float opponent_height_coefficients[22][2]; /*cd30: three courses x seven pairs, fallbackcdd8. */
} nm_drive_params_t;
typedef struct {
    uint8_t throttle, brake, curve;
    int8_t drag_state;
    uint16_t rpm;
    int16_t pitch;
    uint32_t gear, speed_index;
    int32_t compensation;
    uint16_t contact[4];
} nm_drive_input_t;
typedef struct { float drive, brake[2], slope, drag; } nm_drive_terms_t;
typedef struct {
    float inertial[2], longitudinal_delta; /* Entity16c/170/3c, reference units. */
    int16_t pitch, bank, speed_index;
    uint16_t contact[4];
} nm_axle_input_t;
typedef struct { float load[2]; } nm_axle_state_t; /* Entity194/198. */

/* Table quadrant behavior follows the existing MAME-derived TGP reference,
 * including the clamped quarter-turn endpoint and signed zero. */
static inline float nm_drive_sine(const nm_drive_params_t *p, uint16_t angle) {
    unsigned index = angle & 16383;
    if (angle & 16384) index = index ? 16384-index : 16383;
    const uint32_t bits = p->sine[index] ^ ((uint32_t)(angle & 32768) << 16);
    float result; nm_memcpy(&result,&bits,4); return result;
}

/* Host IEEE single operations for NaN operands (the reasons: force_math.h). */
static inline uint32_t nm_nan_word(float value) { uint32_t bits; nm_memcpy(&bits,&value,4); return bits; }
static inline float nm_nan_float(uint32_t bits) { float value; nm_memcpy(&value,&bits,4); return value; }
#define NM_DEFAULT_NAN UINT32_C(0x7fc00000)
static inline int nm_nan_bits(uint32_t bits) { return (bits&0x7fffffffu) > 0x7f800000u; }
static inline int nm_is_nan(float value) { return nm_nan_bits(nm_nan_word(value)); }
static inline int nm_is_inf(float value) { return (nm_nan_word(value)&0x7fffffffu) == 0x7f800000u; }
static inline float nm_nan_operands(uint32_t a, uint32_t b) {
    const uint32_t quiet = UINT32_C(0x00400000);
    if (nm_nan_bits(a) && !(a&quiet)) return nm_nan_float(a|quiet);
    if (nm_nan_bits(b) && !(b&quiet)) return nm_nan_float(b|quiet);
    return nm_nan_float(nm_nan_bits(a) ? a : b);
}
static inline float nm_nan_result(float value) {
    return nm_is_nan(value) ? nm_nan_float(NM_DEFAULT_NAN) : value;
}
static inline float nm_nan_mul(float a, float b) {
    const uint32_t x = nm_nan_word(a), y = nm_nan_word(b);
    return nm_nan_bits(x) || nm_nan_bits(y) ? nm_nan_operands(x,y) : nm_nan_result(a*b);
}
static inline float nm_nan_add(float a, float b) {
    const uint32_t x = nm_nan_word(a), y = nm_nan_word(b);
    return nm_nan_bits(x) || nm_nan_bits(y) ? nm_nan_operands(x,y) : nm_nan_result(a+b);
}
static inline float nm_nan_sub(float a, float b) {
    const uint32_t x = nm_nan_word(a), y = nm_nan_word(b);
    return nm_nan_bits(x) || nm_nan_bits(y) ? nm_nan_operands(x,y) : nm_nan_result(a-b);
}
/* Lowest negative speed index whose drag words the pack holds (below). */
static inline int nm_drive_nan_word(uint32_t bits) { return (bits&0x7fffffffu) > 0x7f800000u; }
#define NM_DRIVE_DRAG_INDEX_MIN (-131)
/* The drag words ef04..ef08 and 10410 read for a speed index (entity+18). A
 * negative index is live play: a side contact with an opponent that touched the
 * grass (collision impulse, the opponent at player speed - 50 steps) or a
 * spinning contact leaves the player moving backwards, and the next tick's e5f4
 * rounds that to a negative index before 10338 clamps the speed to zero (seen in
 * the original: index -20 on reference row 6188 of a fuzz race, speed reset to 0
 * that tick, no NaN anywhere after). The original indexes ROM as it stands:
 *  - base, `ld 0x28b5d20[g0*4]` with the signed word: the words before the table.
 *    0x28b5b14..0x28b5d0c (indices -131..-5) are the 10dd8 table the pack already
 *    holds as guided_targets (centre 0x28b5b10+(0x200>>1); prepare_drive.py checks
 *    the alignment), 0x28b5d10 (index -4) is that table's size word 0x200 (a
 *    denormal), 0x28b5d14..1c (-3..-1) are 0xffffffff.
 *  - surface, `ldos 0x18(g13)`: the index is the unsigned low half, so a negative
 *    index reads 0x28b6520/0x28b6d20 + 4*(65536+index), erased ROM (0xffffffff,
 *    checked by prepare_drive.py for every index down to the minimum).
 * 0xffffffff is a NaN: the drag term is then that NaN (nm_forces_step carries it
 * on). Below the minimum (backwards faster than 0.57 a tick) the pack has no
 * words: still a rejection, out of live play's reach (a contact leaves at most
 * about -0.22). */
static inline int nm_drive_drag_bits(const nm_drive_params_t *p, int32_t index, unsigned table, uint32_t *bits) {
    if (index >= 512 || index < NM_DRIVE_DRAG_INDEX_MIN || table > 2) return 0;
    if (index >= 0) { nm_memcpy(bits,&p->drag[table][index],4); return 1; }
    if (table || index > -4) { *bits = UINT32_C(0xffffffff); return 1; }
    if (index == -4) { *bits = UINT32_C(0x00000200); return 1; }
    nm_memcpy(bits,&p->guided_targets[index-NM_DRIVE_DRAG_INDEX_MIN],4);
    return 1;
}

/* Complete 0xecc8..0xef7c plus its contact-count helper0x10410. This produces
 * force terms, not velocity/position integration. Params must be validated at
 * load; keep strict float operations and contraction disabled. The speed index
 * is the signed entity word (nm_drive_drag_bits). */
static inline int nm_drive_terms(const nm_drive_params_t *p, const nm_drive_input_t *in,
                                 nm_drive_terms_t *out) {
    if (!p || !in || !out || in->gear >= 4) return 0;
    unsigned contacts = 0;
    for (unsigned i = 0; i < 4; ++i) contacts += (in->contact[i] & 12) != 0;
    uint32_t drag_bits, surface_bits = 0;
    if (!nm_drive_drag_bits(p,(int32_t)in->speed_index,0,&drag_bits) ||
        (contacts && !nm_drive_drag_bits(p,(int32_t)in->speed_index,contacts > 2 ? 1 : 2,&surface_bits))) return 0;
    const unsigned rpm_index = (in->rpm > 8500 ? 8500 : in->rpm) >> 7;
    float throttle = (float)in->throttle / 255.0f;
    throttle = p->throttle_scale * throttle;
    throttle = throttle - p->throttle_offset;
    float drive = p->curve[in->curve != 0][rpm_index] * p->gear[in->gear];
    drive = drive * throttle;
    const float compensation = (float)in->compensation * p->compensation_scale;
    const float twice_gravity = p->gravity * 2.0f;
    const float adjustment = compensation * twice_gravity;
    nm_drive_terms_t result;
    result.drive = drive - adjustment;
    float brake = (float)in->brake / 255.0f;
    brake = brake * brake;
    result.brake[0] = brake * p->brake[0];
    result.brake[1] = brake * p->brake[1];
    const int pitch = in->pitch > 0 ? in->pitch/2 : in->pitch;
    const float slope = nm_drive_sine(p,(uint16_t)pitch) * p->gravity;
    result.slope = 0.5f * slope;
    /* NaN words propagate as the host's TGP model does (force_math.h); the
     * operation order is the plain one: 0.45*drag, surface+drag, 0.5*sum. */
    const unsigned nan_words = (nm_drive_nan_word(drag_bits) ? 1u : 0u) |
        (contacts && nm_drive_nan_word(surface_bits) ? 2u : 0u);
    float drag; nm_memcpy(&drag,&drag_bits,4);
    float surface = 0.0f;
    if (contacts) nm_memcpy(&surface,&surface_bits,4);
    if (nan_words) {
        /* Both NaN words are the quiet 0xffffffff: any sum or product with one is it. */
        const uint32_t nan = UINT32_C(0xffffffff);
        nm_memcpy(&result.drag,&nan,4);
    } else {
        if (in->drag_state > 0) drag = 0.45f * drag;
        result.drag = 0.5f * (surface + drag);
    }
    if (!isfinite(result.drive) || !isfinite(result.brake[0]) || !isfinite(result.brake[1]) ||
        !isfinite(result.slope) || (!nan_words && !isfinite(result.drag))) return 0;
    *out = result;
    return 1;
}

/* 0xf278: combine surface categories, not wheel count. Unknown remaining bits
 * together contribute one category. Integer division precedes float lookup. */
static inline float nm_axle_surface(uint16_t mask, int16_t speed_index) {
    if (speed_index <= 10) return 1.0f;
    const unsigned rough = ((mask>>1)&1) + ((mask>>10)&1);
    const unsigned loose = ((mask>>2)&1) + ((mask>>3)&1);
    const unsigned other = (mask & (uint16_t)~0x040e) != 0;
    const unsigned count = rough + loose + other;
    return count ? (float)((230*rough + 128*loose + 150*other)/count)/255.0f : 0.0f;
}

/* Source uses signed integer comparisons of FLOAT BIT PATTERNS. Preserve
 * that ordering (including negative zero); ordinary float clamp is different. */
static inline float nm_axle_clamp(float value, float low, float high) {
    int32_t bits, low_bits, high_bits;
    nm_memcpy(&bits,&value,4); nm_memcpy(&low_bits,&low,4); nm_memcpy(&high_bits,&high,4);
    return bits < low_bits ? low : bits > high_bits ? high : value;
}

/* Complete0xef80..0xf274, plus surface helper. One reference tick; no wheel
 * collision detection or velocity/position integration. Publish only success. */
static inline int nm_axle_load_step(const nm_drive_params_t *p, const nm_axle_input_t *in,
                                    nm_axle_state_t *state) {
    /* The previous tick's delta is NaN after a NaN drag term when 10338 did not
     * run (finish phases: flags outside 0x05400088). It reaches the clamp below,
     * whose integer compare of the bits settles it (host NaN operations). */
    if (!p || !in || !state || nm_is_inf(in->longitudinal_delta) ||
        (!nm_is_nan(in->longitudinal_delta) && !isfinite(in->longitudinal_delta))) return 0;
    const int nan_delta = nm_is_nan(in->longitudinal_delta);
    for (unsigned a = 0; a < 2; ++a)
        if (!isfinite(in->inertial[a]) || !isfinite(state->load[a])) return 0;
    const float bank_cos = nm_drive_sine(p,(uint16_t)((int)in->bank+16384));
    const float bank_sin = nm_drive_sine(p,(uint16_t)in->bank);
    const int half_pitch = in->pitch < 0 ? -((-(int)in->pitch+1)/2) : in->pitch/2;
    float transfer = nm_drive_sine(p,(uint16_t)half_pitch) * p->axle.pitch;
    transfer = nm_nan_add(in->longitudinal_delta,transfer);
    transfer = nm_nan_mul(p->axle.transfer,transfer);
    nm_axle_state_t result;
    for (unsigned a = 0; a < 2; ++a) {
        const float base = bank_cos * p->axle.bank[a];
        float value = -in->inertial[a] * bank_sin;
        value = value + base;
        value = a ? nm_nan_add(value,transfer) : nm_nan_sub(value,transfer);
        value = nm_nan_mul(value,nm_axle_surface(in->contact[2*a] | in->contact[2*a+1],in->speed_index));
        value = nm_nan_mul(value,p->axle.scale[a]);
        if (nm_is_inf(value) || (!nan_delta && !isfinite(value))) return 0;
        value = nm_axle_clamp(value,p->axle.minimum[a],p->axle.maximum[a]);
        value = value - state->load[a];
        value = value * 0.0625f;
        result.load[a] = value + state->load[a];
        if (!isfinite(result.load[a])) return 0;
    }
    *state = result;
    return 1;
}

typedef struct { float speed, delta; int32_t rpm, compensation; } nm_speed_state_t;
/* cvtri with the board's AC (3f001000: round to nearest, ties away as the PC
 * reference's round_to_int, every exception masked). Intel 80960KB PRM, both
 * pages visually checked: 12-23, a masked invalid operation (NaN source) stores
 * the maximum negative integer, INT32_MIN; 11-50, a result whose magnitude int32
 * cannot hold (infinities included) raises integer overflow, masked by AC bit 12,
 * and stores the maximum positive or negative value by the source's sign. Live
 * play reaches the overflow: f9d0's yaw step divides by a turn radius of about
 * 2^127 when the front and rear wheel angles are equal (no separation), the TGP
 * reciprocal of that wraps to infinity and fbd0 converts -inf (INT32_MIN; only
 * its low 16 bits reach the yaw, so the yaw holds). The conversion never fails;
 * the int return stays for the callers' && chains. No AC flags are modelled. */
/* Reference form, kept for the exhaustive equivalence test
 * (tests/test_round_i32.c): library round-half-away, then the saturation. */
static inline int nm_drive_round_i32_reference(float value, int32_t *out) {
    if (isnan(value)) { *out = INT32_MIN; return 1; }
    const float rounded = roundf(value);
    if (rounded < -2147483648.0f) { *out = INT32_MIN; return 1; }
    if (rounded >= 2147483648.0f) { *out = INT32_MAX; return 1; }
    *out = (int32_t)rounded;
    return 1;
}
/* Same result for every float without the roundf library call: truncate with
 * the FPU's float-to-int conversion, then step away from zero when the
 * remaining fraction is at least one half. Below 2^23 the truncated value and
 * the fraction are exact floats; from 2^23 up every float is an integer, so
 * the fraction is zero. NaN, infinities and values outside int32 behave as in
 * the reference: INT32_MIN for NaN, the signed endpoint for the rest. */
static inline int nm_drive_round_i32(float value, int32_t *out) {
    uint32_t bits; nm_memcpy(&bits,&value,sizeof(bits));
    const uint32_t magnitude = bits & UINT32_C(0x7fffffff);
    if (magnitude > UINT32_C(0x7f800000)) { *out = INT32_MIN; return 1; }
    /* |value| >= 2^31 (or infinite): saturate by sign (PRM 11-50); exactly
     * -2^31 is INT32_MIN itself. */
    if (magnitude >= UINT32_C(0x4f000000)) {
        *out = bits >> 31 ? INT32_MIN : INT32_MAX; return 1;
    }
    int32_t whole = (int32_t)value;
    const float fraction = value - (float)whole;
    if (fraction >= 0.5f) ++whole;
    else if (fraction <= -0.5f) --whole;
    *out = whole;
    return 1;
}
/* Full0x10338: integrate longitudinal speed, then derive RPM and over-rev
 * compensation. Reference cvtri uses mode0 ties AWAY, not nearest-even.
 * Native mode flags replace the original global word; no board state. */
static inline int nm_speed_step(const nm_drive_params_t *p, uint32_t flags, unsigned gear,
                                nm_speed_state_t *state) {
    if (!p || !state) return 0;
    if (!(flags & 0x05400088u)) return 1;
    uint32_t bits; nm_memcpy(&bits,&state->delta,4);
    const int nan_delta = nm_drive_nan_word(bits);
    if (gear >= 4 || !isfinite(state->speed) || (!nan_delta && !isfinite(state->delta)) ||
        !isfinite(p->rpm_scale) || !isfinite(p->gear[gear])) return 0;
    nm_speed_state_t result = *state;
    /* A NaN delta (nm_forces_step after a NaN drag term) makes the sum that NaN
     * on the reference's host; its sign bit decides, as for any sum. */
    if (nan_delta) bits |= UINT32_C(0x00400000);
    else { result.speed = state->delta + state->speed; nm_memcpy(&bits,&result.speed,4); }
    if (bits & 0x80000000u) result.speed = result.delta = 0;
    else if (nan_delta) return 0; /* A positive NaN speed: not reached (the drag NaN is 0xffffffff). */
    if (!isfinite(result.speed)) return 0;
    float rpm = result.speed * p->gear[gear];
    rpm = p->rpm_scale * rpm;
    int32_t raw;
    if (!nm_drive_round_i32(rpm,&raw)) return 0;
    result.rpm = raw < 0 ? 0 : raw > 8500 ? 8500 : raw;
    result.compensation = raw > result.rpm ? raw-result.rpm : 0;
    *state = result;
    return 1;
}

#ifdef __cplusplus
extern "C" {
#endif
int nm_drive_params_valid(const nm_drive_params_t *p);
/* One bounded allocation, caller frees it. NULL on malformed/truncated input. */
nm_drive_params_t *nm_drive_load(const char *path);
#ifdef __cplusplus
}
#endif
#endif
