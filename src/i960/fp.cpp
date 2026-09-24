#include "fp.h"

#include <bit>
#include <cmath>

extern "C" {
#include "softfloat.h"
}

namespace i960::fp {
namespace {

uint_fast8_t sf_mode(Round rm) {
    switch (rm) {
    case Round::Nearest: return softfloat_round_near_even;
    case Round::Down: return softfloat_round_min;
    case Round::Up: return softfloat_round_max;
    default: return softfloat_round_minMag;
    }
}

// Widen a single-precision register value to extended. Exact; a signalling
// NaN raises invalid and becomes quiet, as the hardware's load would.
extFloat80_t widen(uint32_t x) {
    float32_t f;
    f.v = x;
    return f32_to_extF80(f);
}

void begin(Round rm) {
    softfloat_roundingMode = sf_mode(rm);
    softfloat_exceptionFlags = 0;
}

Result done(uint32_t v) { return {v, uint8_t(softfloat_exceptionFlags)}; }

} // namespace

Result ref_cvtri(uint32_t x, Round rm) {
    begin(rm);
    const extFloat80_t e = widen(x);
    return done(uint32_t(extF80_to_i32(e, sf_mode(rm), true)));
}

Result ref_cvtzri(uint32_t x) {
    begin(Round::Zero);
    const extFloat80_t e = widen(x);
    return done(uint32_t(extF80_to_i32_r_minMag(e, true)));
}

Result ref_cvtir(uint32_t i, Round rm) {
    begin(rm);
    const extFloat80_t e = i32_to_extF80(int32_t(i)); // exact
    return done(extF80_to_f32(e).v);
}

Result ref_cmpr(uint32_t a, uint32_t b) {
    begin(Round::Nearest);
    const extFloat80_t ea = widen(a), eb = widen(b);
    uint32_t cc;
    if (extF80_lt_quiet(ea, eb)) cc = kLess;
    else if (extF80_eq(ea, eb)) cc = kEqual;
    else if (extF80_lt_quiet(eb, ea)) cc = kGreater;
    else cc = kUnordered;
    return done(cc);
}

Result ref_scaler(uint32_t n_bits, uint32_t x, Round rm) {
    begin(rm);
    // Clamp n: beyond +-400 any finite non-zero single already overflows or
    // lies below half the smallest single denormal, so the rounded result is
    // unchanged, and 2^n stays exact in extended. The product of a 24-bit
    // significand and a power of two is exact in extended, so the only
    // rounding is the final one to single, as on the hardware.
    int32_t n = int32_t(n_bits);
    if (n > 400) n = 400;
    if (n < -400) n = -400;
    extFloat80_t pow2;
    pow2.signExp = uint16_t(0x3fff + n);
    pow2.signif = uint64_t(1) << 63;
    const extFloat80_t e = extF80_mul(widen(x), pow2);
    return done(extF80_to_f32(e).v);
}

// ---------------------------------------------------------------------------
// Native paths. Every line here is justified only by tests/test_fp.

uint32_t fast_cvtri(uint32_t x) {
    const float f = std::bit_cast<float>(x);
    if (!(f >= -2147483648.0f && f < 2147483648.0f)) return 0x80000000u; // NaN, out of range
    return uint32_t(int32_t(std::nearbyint(f)));                          // ties to even
}

uint32_t fast_cvtzri(uint32_t x) {
    const float f = std::bit_cast<float>(x);
    if (!(f >= -2147483648.0f && f < 2147483648.0f)) return 0x80000000u;
    return uint32_t(int32_t(f)); // C++ conversion truncates toward zero
}

uint32_t fast_cvtir(uint32_t i) { return std::bit_cast<uint32_t>(float(int32_t(i))); }

uint32_t fast_cmpr(uint32_t a, uint32_t b) {
    const float fa = std::bit_cast<float>(a), fb = std::bit_cast<float>(b);
    if (fa < fb) return kLess;
    if (fa == fb) return kEqual;
    if (fa > fb) return kGreater;
    return kUnordered;
}

uint32_t fast_scaler(uint32_t n, uint32_t x) {
    int32_t k = int32_t(n);
    if (k > 400) k = 400;
    if (k < -400) k = -400;
    return std::bit_cast<uint32_t>(std::ldexp(std::bit_cast<float>(x), k));
}

} // namespace i960::fp
