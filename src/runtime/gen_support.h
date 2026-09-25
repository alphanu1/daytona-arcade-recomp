// Support for recompiled i960 code (tools/m2recomp output). Generated code is
// native C++: every instruction's semantics are emitted inline with operands
// resolved at recompile time. It calls into the runtime only for memory
// access, call/return frame management and interrupt entry, as a CPU would.
#pragma once

#include "runtime/i960_core.h"
#include "runtime/lockstep.h"

#include <bit>
#include <cmath>
#include <cstdint>

namespace gen {

struct Env {
    rt::I960Core &c;
    rt::Lockstep &ls;
};

// Condition codes, as MAME's cmp_u / cmp_s / concmp_u / concmp_s.
inline uint32_t cc_u(uint32_t a, uint32_t b) { return a < b ? 4u : a == b ? 2u : 1u; }
inline uint32_t cc_s(int32_t a, int32_t b) { return a < b ? 4u : a == b ? 2u : 1u; }
inline uint32_t cc_d(double a, double b) { return a < b ? 4u : a == b ? 2u : a > b ? 1u : 0u; }

// FP with MAME's host-double semantics (lockstep against MAME). The shipped
// path will use the proven native paths in i960/fp.h instead.
inline double round_to_int(double v, uint32_t ac) {
    switch ((ac >> 30) & 3) {
    case 0: return std::round(v);
    case 1: return std::floor(v);
    case 2: return std::ceil(v);
    default: return std::trunc(v);
    }
}
inline uint32_t f2u(float f) { return std::bit_cast<uint32_t>(f); }
inline float u2f(uint32_t u) { return std::bit_cast<float>(u); }

// Recompiled code entry points (generated): run from c.m_IP until it leaves
// recompiled code or the lockstep run ends.
bool has_code(uint32_t addr);   // generated
void run(Env &e);               // generated: dispatches into the chunk holding c.m_IP
uint64_t native_instructions(); // generated: instructions recompiled (all of them: there is no fallback)

} // namespace gen
