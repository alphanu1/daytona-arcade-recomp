// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese
// Table indexing/exponent/quadrant semantics adapted from runtime/tgp.cpp.
// See THIRD_PARTY.md. Pure bounded functions: no TGP state, program or bus.
#ifndef DAYTONA_NATIVE_FORCE_MATH_H
#define DAYTONA_NATIVE_FORCE_MATH_H
#include "drive.h"
#include "nm_mem.h"

static inline uint32_t nm_force_bits(float value) {
    uint32_t bits; nm_memcpy(&bits,&value,4); return bits;
}
static inline float nm_force_float(uint32_t bits) {
    float value; nm_memcpy(&value,&bits,4); return value;
}
static inline float nm_force_inverse_entry(const nm_drive_params_t *p, uint32_t input, unsigned second) {
    const unsigned index = ((input>>9)&0x3ffe) | second;
    uint32_t bits = p->reciprocal[index];
    const uint8_t exponent = (uint8_t)((bits>>23) + 127 - ((input>>23)&255));
    bits = (bits&0x7fffff) | ((uint32_t)exponent<<23);
    if (second) bits |= input&0x80000000u;
    return nm_force_float(bits);
}
/* Command3: preserve table-refined reciprocal and separate float operations.
 * Zero/subnormal inputs follow the reference exponent wrap, not IEEE divide. */
static inline float nm_force_divide(const nm_drive_params_t *p, float numerator, float denominator) {
    const uint32_t bits = nm_force_bits(denominator);
    const float normalized = nm_force_float((bits&0x807fffffu)|0x3f800000u);
    const float product = normalized * nm_force_inverse_entry(p,bits,0);
    const float inverse = nm_force_inverse_entry(p,bits,1) - product;
    return numerator * inverse;
}
/* Command44: output order is sin(angle)*magnitude, cos(angle)*magnitude. */
static inline void nm_force_cartesian(const nm_drive_params_t *p, uint16_t angle, float magnitude, float out[2]) {
    out[0] = nm_drive_sine(p,angle) * magnitude;
    out[1] = nm_drive_sine(p,(uint16_t)(angle+16384u)) * magnitude;
}
static inline float nm_force_isqrt_entry(const nm_drive_params_t *p, uint32_t input, unsigned second) {
    const unsigned index = 0x2000 ^ (((input>>10)&0x3ffe) | second);
    uint32_t bits = p->inverse_sqrt[index];
    const uint8_t exponent = (uint8_t)((bits>>23) + 63 - ((input>>24)&127));
    bits = (bits&0x807fffffu) | ((uint32_t)exponent<<23);
    if (!second) bits &= 0x7fffffffu;
    return nm_force_float(bits);
}
static inline float nm_force_length(const nm_drive_params_t *p, float cosine_component, float sine_component) {
    const float a = cosine_component*cosine_component, b = sine_component*sine_component;
    const float square = a+b;
    const uint32_t bits = nm_force_bits(square);
    const float normalized = nm_force_float((bits&0x807fffffu)|0x3f800000u);
    const float product = normalized * nm_force_isqrt_entry(p,bits,0);
    const float inverse = nm_force_isqrt_entry(p,bits,1) - product;
    return square * inverse;
}
static inline int32_t nm_force_angle(const nm_drive_params_t *p, float cosine_component, float sine_component) {
    const uint32_t x = nm_force_bits(cosine_component), y = nm_force_bits(sine_component);
    const unsigned sx = x>>31, sy = y>>31;
    const unsigned swap = (x&0x7fffffffu) <= (y&0x7fffffffu);
    float ratio = swap ? nm_force_divide(p,cosine_component,sine_component) :
                         nm_force_divide(p,sine_component,cosine_component);
    if (fabsf(ratio) <= nm_force_float(0x37ffffffu)) ratio = 0;
    const uint32_t bits = nm_force_bits(ratio);
    const uint8_t shift = (uint8_t)(0x88-(bits>>23));
    unsigned index = shift <= 23 ? ((bits&0x7fffff)|0x800000)>>shift : 0;
    if (index == 16384) index = 16383;
    /* Finite ratios selected as min/max stay within the quarter table. */
    if (index >= 16384) return INT32_MIN;
    uint32_t angle = p->arctangent[index];
    if (sx ^ sy ^ swap) angle >>= 16;
    if (swap) angle += 16384;
    if ((sx && !swap) || (sy && swap)) angle += 32768;
    angle &= 65535;
    return angle < 32768 ? (int32_t)angle : (int32_t)angle-65536;
}
/* The player's NaN drag (include/drive.h nm_drive_drag_bits: a negative speed
 * index reads the erased ROM word 0xffffffff) runs through the force and tire
 * chain for one tick. The reference (MAME's TGP model on the host) computes it
 * with host IEEE single arithmetic, and the original's results depend on the NaN
 * bits (sign tests, table indices from bits); checked against the reference on
 * both NaN cases (base word index -1, surface word index -7 on grass): every
 * player field equal on every row after it. The host here is arm64: a NaN operand
 * propagates quietened, the first operand first, and an invalid operation of
 * non-NaN operands gives the default NaN 0x7fc00000. The SH4 FPU does neither
 * (0xffffffff is a signalling NaN to it, and its default NaN is 0x7fbfffff), so
 * the NaN-capable chain uses these operations; with non-NaN operands and a
 * non-NaN result they are the plain FPU operation. */
/* nm_force_divide/length/angle/cartesian/combine with the operations above,
 * same operation order; equal to them whenever no NaN occurs. */
static inline float nm_force_divide_nan(const nm_drive_params_t *p, float numerator, float denominator) {
    const uint32_t bits = nm_force_bits(denominator);
    const float normalized = nm_force_float((bits&0x807fffffu)|0x3f800000u);
    const float product = nm_nan_mul(normalized,nm_force_inverse_entry(p,bits,0));
    const float inverse = nm_nan_sub(nm_force_inverse_entry(p,bits,1),product);
    return nm_nan_mul(numerator,inverse);
}
static inline float nm_force_length_nan(const nm_drive_params_t *p, float cosine_component, float sine_component) {
    const float a = nm_nan_mul(cosine_component,cosine_component), b = nm_nan_mul(sine_component,sine_component);
    const float square = nm_nan_add(a,b);
    const uint32_t bits = nm_force_bits(square);
    const float normalized = nm_force_float((bits&0x807fffffu)|0x3f800000u);
    const float product = nm_nan_mul(normalized,nm_force_isqrt_entry(p,bits,0));
    const float inverse = nm_nan_sub(nm_force_isqrt_entry(p,bits,1),product);
    return nm_nan_mul(square,inverse);
}
static inline int32_t nm_force_angle_nan(const nm_drive_params_t *p, float cosine_component, float sine_component) {
    const uint32_t x = nm_force_bits(cosine_component), y = nm_force_bits(sine_component);
    const unsigned sx = x>>31, sy = y>>31;
    const unsigned swap = (x&0x7fffffffu) <= (y&0x7fffffffu);
    float ratio = swap ? nm_force_divide_nan(p,cosine_component,sine_component) :
                         nm_force_divide_nan(p,sine_component,cosine_component);
    if ((nm_force_bits(ratio)&0x7fffffffu) <= 0x37ffffffu) ratio = 0; /* fabsf(ratio) <= 0x37ffffff, false for NaN */
    const uint32_t bits = nm_force_bits(ratio);
    const uint8_t shift = (uint8_t)(0x88-(bits>>23));
    unsigned index = shift <= 23 ? ((bits&0x7fffff)|0x800000)>>shift : 0;
    if (index == 16384) index = 16383;
    if (index >= 16384) return INT32_MIN;
    uint32_t angle = p->arctangent[index];
    if (sx ^ sy ^ swap) angle >>= 16;
    if (swap) angle += 16384;
    if ((sx && !swap) || (sy && swap)) angle += 32768;
    angle &= 65535;
    return angle < 32768 ? (int32_t)angle : (int32_t)angle-65536;
}
static inline void nm_force_cartesian_nan(const nm_drive_params_t *p, uint16_t angle, float magnitude, float out[2]) {
    out[0] = nm_nan_mul(nm_drive_sine(p,angle),magnitude);
    out[1] = nm_nan_mul(nm_drive_sine(p,(uint16_t)(angle+16384u)),magnitude);
}
typedef struct { int32_t angle; float magnitude; } nm_force_polar_t;
/* Command46: add two live polar forces. This is not a captured force stream. */
static inline int nm_force_combine(const nm_drive_params_t *p, const nm_force_polar_t *a,
                                    const nm_force_polar_t *b, nm_force_polar_t *out) {
    if (!p || !a || !b || !out || !isfinite(a->magnitude) || !isfinite(b->magnitude)) return 0;
    float ca[2], cb[2];
    nm_force_cartesian(p,(uint16_t)a->angle,a->magnitude,ca);
    nm_force_cartesian(p,(uint16_t)b->angle,b->magnitude,cb);
    const float cosine = cb[1]+ca[1], sine = cb[0]+ca[0];
    if (!isfinite(cosine) || !isfinite(sine)) return 0;
    nm_force_polar_t result;
    result.angle = nm_force_angle(p,cosine,sine);
    result.magnitude = nm_force_length(p,cosine,sine);
    if (result.angle == INT32_MIN || !isfinite(result.magnitude)) return 0;
    *out = result;
    return 1;
}
/* nm_force_combine, NaN magnitudes allowed (infinities still rejected). */
static inline int nm_force_combine_nan(const nm_drive_params_t *p, const nm_force_polar_t *a,
                                       const nm_force_polar_t *b, nm_force_polar_t *out) {
    if (!p || !a || !b || !out || nm_is_inf(a->magnitude) || nm_is_inf(b->magnitude)) return 0;
    float ca[2], cb[2];
    nm_force_cartesian_nan(p,(uint16_t)a->angle,a->magnitude,ca);
    nm_force_cartesian_nan(p,(uint16_t)b->angle,b->magnitude,cb);
    const float cosine = nm_nan_add(cb[1],ca[1]), sine = nm_nan_add(cb[0],ca[0]);
    if (nm_is_inf(cosine) || nm_is_inf(sine)) return 0;
    nm_force_polar_t result;
    result.angle = nm_force_angle_nan(p,cosine,sine);
    result.magnitude = nm_force_length_nan(p,cosine,sine);
    if (result.angle == INT32_MIN || nm_is_inf(result.magnitude)) return 0;
    *out = result;
    return 1;
}
#endif
