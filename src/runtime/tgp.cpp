// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese
//
// The TGP's I/O space on the Model 2 CPU board: the sin/cos, atan, 1/x and
// 1/sqrt table ports and the banked window onto buffer RAM and the copro data
// ROM. Transplanted from MAME's src/mame/sega/model2.cpp (model2_tgp_state)
// at dddd73680656e355bb2b5beecab1167c9f07bf81 (BSD-3-Clause; notice above
// kept as the licence requires). See THIRD_PARTY.md.

#include "runtime/tgp.h"

namespace rt {

// With the bank enabled (bank_reg & 0xc00000) the whole I/O space is the
// banked window, hiding the table ports (MAME's copro_tgp_bank view).
uint32_t Tgp::io_r(uint16_t ea) {
    if (bank_reg & 0xc00000) return bus->mem_r((bank_reg & 0xff0000) | ea);
    switch (ea) {
    case 0x20: case 0x21: case 0x22: case 0x23: { // copro_sincos_r
        const uint32_t ang = sincos_base + (ea - 0x20) * 0x4000;
        uint32_t index = ang & 0x3fff;
        if (ang & 0x4000) index = uint32_t(std::min(0x4000 - int(index), 0x3fff));
        uint32_t result = tables[index];
        if (ang & 0x8000) result ^= 0x80000000;
        return result;
    }
    case 0x24: case 0x25: case 0x26: case 0x27: { // copro_atan_r
        const uint8_t ie = uint8_t(0x88 - (atan_base[3] >> 23));
        const bool s0 = atan_base[0] & 0x80000000;
        const bool s1 = atan_base[1] & 0x80000000;
        const bool s2 = (atan_base[0] & 0x7fffffff) <= (atan_base[1] & 0x7fffffff);
        const uint32_t im = atan_base[3] & 0x7fffff;
        uint32_t index = ie <= 0x17 ? (im | 0x800000) >> ie : 0;
        if (index == 0x4000) index = 0x3fff;
        uint32_t result = tables[index | 0x4000];
        if (s0 ^ s1 ^ s2) result >>= 16;
        if (s2) result += 0x4000;
        if ((s0 && !s2) || (s1 && s2)) result += 0x8000;
        return result & 0xffff;
    }
    case 0x28: case 0x29: { // copro_inv_r
        const uint32_t offset = ea - 0x28;
        const uint32_t index = ((inv_base >> 9) & 0x3ffe) | (offset & 1);
        uint32_t result = tables[index | 0x8000];
        const uint8_t bexp = (inv_base >> 23) & 0xff;
        const uint8_t exp = uint8_t((result >> 23) + (0x7f - bexp));
        result = (result & 0x007fffff) | (uint32_t(exp) << 23);
        if (inv_base & 0x80000000 && offset) result |= 0x80000000;
        return result;
    }
    case 0x2a: case 0x2b: { // copro_isqrt_r
        const uint32_t offset = ea - 0x2a;
        const uint32_t index = 0x2000 ^ (((isqrt_base >> 10) & 0x3ffe) | (offset & 1));
        uint32_t result = tables[index | 0xc000];
        const uint8_t bexp = (isqrt_base >> 24) & 0x7f;
        const uint8_t exp = uint8_t((result >> 23) + (0x3f - bexp));
        result = (result & 0x807fffff) | (uint32_t(exp) << 23);
        if (!(offset & 1)) result &= 0x7fffffff;
        return result;
    }
    default: return 0; // unmapped
    }
}

void Tgp::io_w(uint16_t ea, uint32_t v) {
    if (bank_reg & 0xc00000) { bus->mem_w((bank_reg & 0xff0000) | ea, v); return; }
    switch (ea) {
    case 0x20: case 0x21: case 0x22: case 0x23: sincos_base = v; break;
    case 0x24: case 0x25: case 0x26: case 0x27:
        atan_base[ea - 0x24] = v;
        gpio0 = (atan_base[0] & 0x7fffffff) <= (atan_base[1] & 0x7fffffff);
        break;
    case 0x28: case 0x29: inv_base = v; break;
    case 0x2a: case 0x2b: isqrt_base = v; break;
    default: break; // unmapped
    }
}

} // namespace rt
