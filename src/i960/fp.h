// i960KB floating point for the operations Daytona executes.
//
// Measured (design doc, Floating point): Daytona's reachable i960 FP is only
// cvtri, cvtzri, cvtir, cmpr and scaler, all on single-precision values in
// g/l registers (never fp0-fp3 or FP literals), with AC rounding control 0
// (round to nearest) and every FP exception masked.
//
// Two implementations of each:
//   ref_*   the hardware model: operands widened to 80-bit extended, computed
//           and rounded with Berkeley SoftFloat 3e (rules.md: FP whose result
//           can reach memory or a compare goes through extF80), in any of the
//           four AC rounding modes, reporting IEEE exception flags;
//   fast_*  native host arithmetic for round-to-nearest only. Recompiled code
//           may use these ONLY because tests/test_fp proves them bit-identical
//           to ref_* (exhaustively where the input space allows). They assume
//           the host FPU is in its default round-to-nearest mode and that the
//           build does not use -ffast-math.
//
// Values are passed as raw register bit patterns (uint32_t), as the i960 holds
// them. Results that are not confirmed against the i960 manual or a PCB:
//   - an invalid conversion (NaN or out of int32 range) returns 0x80000000,
//     SoftFloat's Intel (8086-SSE) "integer indefinite";
//   - where the i960 records exception flags in AC, and whether Daytona reads
//     them (MAME never sets them; the traces show them clear).
#pragma once

#include <cstdint>

namespace i960::fp {

// AC rounding control, bits 31:30 (MAME's round_to_int uses the same order).
enum class Round : uint8_t { Nearest = 0, Down = 1, Up = 2, Zero = 3 };
inline Round round_from_ac(uint32_t ac) { return Round((ac >> 30) & 3); }

// IEEE exception flags (SoftFloat's bit values).
enum Flag : uint8_t { kInexact = 1, kUnderflow = 2, kOverflow = 4, kInfinite = 8, kInvalid = 16 };

struct Result {
    uint32_t value; // int32 bit pattern or single-precision bit pattern
    uint8_t flags;
};

// AC condition code for a compare: 4 less, 2 equal, 1 greater, 0 unordered.
constexpr uint32_t kLess = 4, kEqual = 2, kGreater = 1, kUnordered = 0;

// Reference (hardware model).
Result ref_cvtri(uint32_t x, Round rm);          // single -> int32, rounded per rm
Result ref_cvtzri(uint32_t x);                   // single -> int32, toward zero
Result ref_cvtir(uint32_t i, Round rm);          // int32 -> single, rounded per rm
Result ref_cmpr(uint32_t a, uint32_t b);         // value = condition code
Result ref_scaler(uint32_t n, uint32_t x, Round rm); // x * 2^n (n as int32) -> single

// Native fast paths, round to nearest only.
uint32_t fast_cvtri(uint32_t x);
uint32_t fast_cvtzri(uint32_t x);
uint32_t fast_cvtir(uint32_t i);
uint32_t fast_cmpr(uint32_t a, uint32_t b);
uint32_t fast_scaler(uint32_t n, uint32_t x);

} // namespace i960::fp
