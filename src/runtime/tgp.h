// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
//
// Fujitsu MB86234 TGP (Model 2 geometry coprocessor): context and the inline
// operations that recompiled TGP code (tools/m2tgprecomp output) is built
// from. The semantics are MAME's src/devices/cpu/mb86233/mb86233.cpp at
// dddd73680656e355bb2b5beecab1167c9f07bf81 (BSD-3-Clause; notice above kept
// as the licence requires), split into templates on the instruction's
// constant fields so each generated instruction folds to straight-line code.
// The board side (table ports, banked memory) is model2.cpp's
// model2_tgp_state. There is no MB86233 interpreter: the TGP's program (the
// 2,024 words the i960 uploads) is statically recompiled. See THIRD_PARTY.md.
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace rt {

// What the TGP sees outside itself: its FIFOs and the banked external memory
// (buffer RAM, copro data ROM). The lockstep harness answers these from
// MAME's log; the game answers them from the board.
class TgpBus {
public:
    virtual ~TgpBus() = default;
    virtual bool fifo_pop(uint32_t &v) = 0;       // false: input FIFO empty (the TGP stalls)
    virtual void fifo_push(uint32_t v) = 0;
    virtual uint32_t mem_r(uint32_t adr) = 0;     // adr = (bank & 0xff0000) | io offset
    virtual void mem_w(uint32_t adr, uint32_t v) = 0;
    virtual void bank_w(uint32_t v) { (void)v; }  // observed only (the harness checks it)
};

struct TgpFatal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Tgp {
    static constexpr uint32_t F_ZRC = 0x00000001, F_ZRD = 0x00000002, F_SGD = 0x00000008, F_CPD = 0x00000020,
                              F_OVD = 0x00000080, F_DVZD = 0x00000800, F_ZX0 = 0x08000000, F_ZX1 = 0x10000000,
                              F_ZX2 = 0x20000000, F_ZC0 = 0x40000000, F_ZC1 = 0x80000000;

    TgpBus *bus = nullptr;
    const uint32_t *tables = nullptr; // copro_tgp_tables (opr-14742a/14743a), 0x10000 words

    // MAME's mb86233 state, same names without m_.
    uint32_t st = 0, a = 0, b = 0, d = 0, p = 0;
    uint32_t alu_stmask = 0, alu_stset = 0, alu_r1 = 0, alu_r2 = 0;
    uint16_t pc = 0, sp = 0, b0 = 0, b1 = 0, x0 = 0, x1 = 0, i0 = 0, i1 = 0, vsmr = 7, pcs[4]{}, mask = 0, m = 1;
    uint8_t r = 1, rpc = 1, c0 = 1, c1 = 1, sft = 0, vsm = 0;
    bool gpio0 = false;
    bool stall = false;
    uint64_t count = 0; // completed instructions
    void (*hook)(Tgp &, uint16_t ppc) = nullptr; // test builds only (M2TGP_HOOK)

    // Board state (model2_tgp_state).
    uint32_t bank_reg = 0, sincos_base = 0, inv_base = 0, isqrt_base = 0, atan_base[4]{};

    uint32_t data[0x400]{};   // data RAM: 0x000-0x0ff and 0x200-0x3ff are mapped
    uint32_t prog[0x1000]{};  // program RAM (what the i960 uploaded)

    void reset() { // MAME device_reset
        pc = 0; st = F_ZRC | F_ZRD | F_ZX0 | F_ZX1 | F_ZX2 | F_ZC0 | F_ZC1; sp = 0;
        a = b = d = p = 0; r = 1; rpc = 1; c0 = 1; c1 = 1;
        b0 = b1 = x0 = x1 = i0 = i1 = 0; sft = 0; vsm = 0; vsmr = 7; mask = 0; m = 1;
        alu_stmask = alu_stset = alu_r1 = alu_r2 = 0;
        std::fill(std::begin(pcs), std::end(pcs), 0);
        stall = false;
    }

    [[noreturn]] void fatal(uint16_t at, const char *what) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "TGP %03x: %s", at, what);
        throw TgpFatal(buf);
    }

    static float u2f(uint32_t v) { return std::bit_cast<float>(v); }
    static uint32_t f2u(float f) { return std::bit_cast<uint32_t>(f); }
    static uint32_t sext24(uint32_t v) { return uint32_t(int32_t(v << 8) >> 8); }
    static uint32_t set_exp(uint32_t val, uint32_t exp) { return (val & 0x807fffff) | ((exp & 0xff) << 23); }
    static uint32_t set_mant(uint32_t val, uint32_t mant) { return (val & 0x07f800000) | ((mant & 0x00800000) << 8) | (mant & 0x007fffff); }
    static uint32_t get_exp(uint32_t val) { return (val >> 23) & 0xff; }
    static uint32_t get_mant(uint32_t val) { return val & 0x80000000 ? val | 0x7f800000 : val & 0x807fffff; }

    // MAME's s32(float) on its x86-64 build (cvttss2si): NaN and out-of-range
    // give 0x80000000. Spelled out so ARM64 (which saturates) agrees.
    static uint32_t to_s32(float f) {
        if (!(f >= -2147483648.0f && f < 2147483648.0f)) return 0x80000000u;
        return uint32_t(int32_t(f));
    }

    void pcs_push() { for (unsigned i = 3; i; i--) pcs[i] = pcs[i - 1]; pcs[0] = pc; }
    void pcs_pop() { pc = pcs[0]; for (unsigned i = 0; i != 3; i++) pcs[i] = pcs[i + 1]; }
    void stset_int(uint32_t v) { alu_stset = v ? (v & 0x80000000 ? F_SGD : 0) : F_ZRD; }
    void stset_fp(uint32_t v) { alu_stset = (v & 0x7fffffff) ? (v & 0x80000000 ? F_SGD : 0) : F_ZRD; }
    void alu_update_st() { st = (st & ~alu_stmask) | alu_stset; }

    static constexpr uint32_t STM = F_ZRD | F_SGD | F_CPD | F_OVD | F_DVZD;

    // ALU codes MAME implements; anything else is refused by the recompiler.
    static constexpr bool alu_known(uint32_t alu) {
        return alu <= 0x11 || alu == 0x13 || alu == 0x14 || (alu >= 0x16 && alu <= 0x1b);
    }

    template <uint32_t ALU> void alu_pre() {
        if constexpr (ALU == 0x00) {
        } else if constexpr (ALU == 0x01) { alu_stmask = STM; alu_r1 = d & a; stset_int(alu_r1); }
        else if constexpr (ALU == 0x02) { alu_stmask = STM; alu_r1 = d | a; stset_int(alu_r1); }
        else if constexpr (ALU == 0x03) { alu_stmask = STM; alu_r1 = d ^ a; stset_int(alu_r1); }
        else if constexpr (ALU == 0x04) { alu_stmask = STM; alu_r1 = ~d; stset_int(alu_r1); }
        else if constexpr (ALU == 0x05) { alu_stmask = STM; stset_fp(f2u(u2f(d) - u2f(a))); }
        else if constexpr (ALU == 0x06) { alu_stmask = STM; alu_r1 = f2u(u2f(d) + u2f(a)); stset_fp(alu_r1); }
        else if constexpr (ALU == 0x07) { alu_stmask = STM; alu_r1 = f2u(u2f(d) - u2f(a)); stset_fp(alu_r1); }
        else if constexpr (ALU == 0x08) { alu_stmask = 0; alu_r1 = f2u(u2f(a) * u2f(b)); alu_stset = 0; }
        else if constexpr (ALU == 0x09) { alu_stmask = STM; alu_r1 = f2u(u2f(d) + u2f(p)); alu_r2 = f2u(u2f(a) * u2f(b)); stset_fp(alu_r1); }
        else if constexpr (ALU == 0x0a) { alu_stmask = STM; alu_r1 = f2u(u2f(d) - u2f(p)); alu_r2 = f2u(u2f(a) * u2f(b)); stset_fp(alu_r1); }
        else if constexpr (ALU == 0x0b) { alu_stmask = STM; alu_r1 = d & 0x7fffffff; stset_fp(alu_r1); }
        else if constexpr (ALU == 0x0c) { alu_stmask = STM; alu_r1 = f2u(u2f(d) + u2f(p)); stset_fp(alu_r1); }
        else if constexpr (ALU == 0x0d) { alu_stmask = STM; alu_r1 = p; alu_r2 = f2u(u2f(a) * u2f(b)); stset_fp(alu_r1); }
        else if constexpr (ALU == 0x0e) { alu_stmask = STM; alu_r1 = f2u(float(int32_t(d))); stset_int(alu_r1); }
        else if constexpr (ALU == 0x0f) {
            alu_stmask = STM;
            switch ((m >> 1) & 3) {
            case 0: alu_r1 = to_s32(::roundf(u2f(d))); break;
            case 1: alu_r1 = to_s32(::ceilf(u2f(d))); break;
            case 2: alu_r1 = to_s32(::floorf(u2f(d))); break;
            case 3: alu_r1 = to_s32(u2f(d)); break;
            }
            stset_int(alu_r1);
        }
        else if constexpr (ALU == 0x10) { alu_stmask = STM; alu_r1 = f2u(u2f(d) / u2f(a)); stset_fp(alu_r1); }
        else if constexpr (ALU == 0x11) { alu_stmask = STM; alu_r1 = d ? d ^ 0x80000000 : 0; stset_fp(alu_r1); }
        else if constexpr (ALU == 0x13) { alu_stmask = STM; alu_r1 = f2u(u2f(b) + u2f(a)); stset_fp(alu_r1); }
        else if constexpr (ALU == 0x14) { alu_stmask = STM; alu_r1 = f2u(u2f(b) - u2f(a)); stset_fp(alu_r1); }
        // Shift counts: MAME shifts by an 8-bit sft; on its x86-64 and ARM64
        // builds a 32-bit shift uses the count mod 32, made explicit here.
        else if constexpr (ALU == 0x16) { alu_stmask = STM; alu_r1 = d >> (sft & 31); stset_int(alu_r1); }
        else if constexpr (ALU == 0x17) { alu_stmask = STM; alu_r1 = d << (sft & 31); stset_int(alu_r1); }
        else if constexpr (ALU == 0x18) { alu_stmask = STM; alu_r1 = uint32_t(int32_t(d) >> (sft & 31)); stset_int(alu_r1); }
        else if constexpr (ALU == 0x19) { alu_stmask = STM; alu_r1 = uint32_t(int32_t(d) << (sft & 31)); stset_int(alu_r1); }
        else if constexpr (ALU == 0x1a) { alu_stmask = STM; alu_r1 = d + a; stset_int(alu_r1); }
        else if constexpr (ALU == 0x1b) { alu_stmask = STM; alu_r1 = d - a; stset_int(alu_r1); }
        else static_assert(alu_known(ALU), "ALU op MAME does not implement");
    }

    // Integer ALU results land before a transfer, floating-point ones after.
    template <uint32_t ALU> void alu_post_1() {
        if constexpr (ALU == 0x01 || ALU == 0x02 || ALU == 0x03 || ALU == 0x04 || ALU == 0x0e || ALU == 0x0f ||
                      ALU == 0x16 || ALU == 0x17 || ALU == 0x18 || ALU == 0x19 || ALU == 0x1a || ALU == 0x1b) {
            d = alu_r1; alu_update_st();
        }
    }
    template <uint32_t ALU> void alu_post_2() {
        if constexpr (ALU == 0x05) alu_update_st();
        else if constexpr (ALU == 0x06 || ALU == 0x07 || ALU == 0x0b || ALU == 0x0c || ALU == 0x10 || ALU == 0x11 ||
                           ALU == 0x13 || ALU == 0x14) { d = alu_r1; alu_update_st(); }
        else if constexpr (ALU == 0x08) p = alu_r1;
        else if constexpr (ALU == 0x09 || ALU == 0x0a || ALU == 0x0d) { d = alu_r1; p = alu_r2; alu_update_st(); }
    }

    template <uint32_t R> uint16_t ea_pre_0() const {
        if constexpr ((R & 0x180) == 0x000) return R & 0x7f;
        else if constexpr ((R & 0x180) == 0x080 || (R & 0x180) == 0x100) return uint16_t((R & 0x7f) + b0 + x0);
        else if constexpr ((R & 0x60) == 0x00) return uint16_t(b0 + x0);
        else if constexpr ((R & 0x60) == 0x20) return x0;
        else if constexpr ((R & 0x60) == 0x40) return uint16_t(b0 + (x0 & vsmr));
        else return uint16_t(x0 & vsmr);
    }
    template <uint32_t R> void ea_post_0() {
        if constexpr (!(R & 0x100)) return;
        else if constexpr (!(R & 0x080)) x0 += i0;
        else x0 += uint16_t(int32_t(R << 27) >> 27);
    }
    template <uint32_t R> uint16_t ea_pre_1() const {
        if constexpr ((R & 0x180) == 0x000) return R & 0x7f;
        else if constexpr ((R & 0x180) == 0x080 || (R & 0x180) == 0x100) return uint16_t((R & 0x7f) + b1 + x1);
        else if constexpr ((R & 0x60) == 0x00) return uint16_t(b1 + x1);
        else if constexpr ((R & 0x60) == 0x20) return x1;
        else if constexpr ((R & 0x60) == 0x40) return uint16_t(b1 + (x1 & vsmr));
        else return uint16_t(x1 & vsmr);
    }
    template <uint32_t R> void ea_post_1() {
        if constexpr (!(R & 0x100)) return;
        else if constexpr (!(R & 0x080)) x1 += i1;
        else x1 += uint16_t(int32_t(R << 27) >> 27);
    }

    // Address spaces (model2_tgp_state maps).
    uint32_t data_r(uint16_t ea) const { return (ea < 0x100 || (ea >= 0x200 && ea < 0x400)) ? data[ea] : 0; }
    void data_w(uint16_t ea, uint32_t v) { if (ea < 0x100 || (ea >= 0x200 && ea < 0x400)) data[ea] = v; }
    uint32_t prog_r(uint16_t ea) const { return ea < 0x1000 ? prog[ea] : 0; }
    uint32_t io_r(uint16_t ea);
    void io_w(uint16_t ea, uint32_t v);

    // Register file. rf 1 is the input FIFO: popping it empty sets stall and
    // the generated code returns to re-run the instruction later, as MAME does.
    uint32_t rf_r(uint32_t n) {
        if (n == 1) {
            uint32_t v = 0;
            if (!bus->fifo_pop(v)) stall = true;
            return v;
        }
        return 0;
    }
    void rf_w(uint32_t n, uint32_t v) {
        if (n == 2) bus->fifo_push(v);
        else if (n == 3) { bank_reg = v; bus->bank_w(v); }
    }

    template <uint32_t RR> uint32_t read_reg() {
        constexpr uint32_t R = RR & 0x3f;
        if constexpr (R >= 0x20 && R < 0x30) return rf_r(R & 0x1f);
        else if constexpr (R == 0x00) return b0;
        else if constexpr (R == 0x01) return b1;
        else if constexpr (R == 0x02) return x0;
        else if constexpr (R == 0x03) return x1;
        else if constexpr (R == 0x0c) return c0;
        else if constexpr (R == 0x0d) return c1;
        else if constexpr (R == 0x10) return a;
        else if constexpr (R == 0x11) return get_exp(a);
        else if constexpr (R == 0x12) return get_mant(a);
        else if constexpr (R == 0x13) return b;
        else if constexpr (R == 0x14) return get_exp(b);
        else if constexpr (R == 0x15) return get_mant(b);
        else if constexpr (R == 0x19) return d;
        else if constexpr (R == 0x1a) return get_exp(d);
        else if constexpr (R == 0x1b) return get_mant(d);
        else if constexpr (R == 0x1c) return p;
        else if constexpr (R == 0x1d) return get_exp(p);
        else if constexpr (R == 0x1e) return get_mant(p);
        else if constexpr (R == 0x1f) return sft;
        else if constexpr (R == 0x34) return rpc;
        else return 0; // MAME: unimplemented read_reg, 0
    }
    template <uint32_t RR> void write_reg(uint32_t v) {
        constexpr uint32_t R = RR & 0x3f;
        if constexpr (R >= 0x20 && R < 0x30) rf_w(R & 0x1f, v);
        else if constexpr (R == 0x00) b0 = uint16_t(v);
        else if constexpr (R == 0x01) b1 = uint16_t(v);
        else if constexpr (R == 0x02) x0 = uint16_t(v);
        else if constexpr (R == 0x03) x1 = uint16_t(v);
        else if constexpr (R == 0x05) i0 = uint16_t(v);
        else if constexpr (R == 0x06) i1 = uint16_t(v);
        else if constexpr (R == 0x08) sp = uint16_t(v);
        else if constexpr (R == 0x0a) { vsm = v & 7; vsmr = uint16_t((8 << vsm) - 1); }
        else if constexpr (R == 0x0c) { c0 = uint8_t(v); if (c0 == 1) st |= F_ZC0; else st &= ~F_ZC0; }
        else if constexpr (R == 0x0d) { c1 = uint8_t(v); if (c1 == 1) st |= F_ZC1; else st &= ~F_ZC1; }
        else if constexpr (R == 0x10) a = v;
        else if constexpr (R == 0x11) a = set_exp(a, v);
        else if constexpr (R == 0x12) a = set_mant(a, v);
        else if constexpr (R == 0x13) b = v;
        else if constexpr (R == 0x14) b = set_exp(b, v);
        else if constexpr (R == 0x15) b = set_mant(b, v);
        else if constexpr (R == 0x19) d = v;
        else if constexpr (R == 0x1a) d = set_exp(d, v);
        else if constexpr (R == 0x1b) d = set_mant(d, v);
        else if constexpr (R == 0x1c) p = v;
        else if constexpr (R == 0x1d) p = set_exp(p, v);
        else if constexpr (R == 0x1e) p = set_mant(p, v);
        else if constexpr (R == 0x1f) sft = uint8_t(v);
        else if constexpr (R == 0x34) rpc = uint8_t(v);
        else if constexpr (R == 0x3c) mask = uint16_t(v);
        // MAME: 0x0f and unimplemented registers ignore the write
    }

    template <uint32_t R> void write_mem_internal_1(uint32_t v, bool bank) {
        uint16_t ea = ea_pre_1<R>();
        if (bank) ea += 0x200;
        data_w(ea, v);
        ea_post_1<R>();
    }
    template <uint32_t R> void write_mem_io_1(uint32_t v) {
        io_w(ea_pre_1<R>(), v);
        ea_post_1<R>();
    }

    // Branch conditions MAME implements (others are refused by the recompiler).
    static constexpr bool cond_known(uint32_t c) {
        return c <= 2 || c == 0x0a || c == 0x0b || c == 0x0c || c == 0x10 || c == 0x11 || c == 0x12 || c == 0x16;
    }
    template <uint32_t C> bool cond() const {
        if constexpr (C == 0x00) return st & F_ZRD;
        else if constexpr (C == 0x01) return !(st & F_SGD);
        else if constexpr (C == 0x02) return st & (F_ZRD | F_SGD);
        else if constexpr (C == 0x0a) return gpio0;
        else if constexpr (C == 0x0b || C == 0x0c || C == 0x12) return false; // gpio1-3: never driven on Model 2
        else if constexpr (C == 0x10) return !(st & F_ZC0);
        else if constexpr (C == 0x11) return !(st & F_ZC1);
        else if constexpr (C == 0x16) return true;
        else static_assert(cond_known(C), "condition MAME does not implement");
    }
    template <uint32_t C> void count_down() {
        if constexpr (C == 0x10) { if (c0 != 1) { c0--; if (c0 == 1) st |= F_ZC0; } }
        else if constexpr (C == 0x11) { if (c1 != 1) { c1--; if (c1 == 1) st |= F_ZC1; } }
    }
};

// Recompiled TGP program (generated by m2tgprecomp).
namespace tgpgen {
// Runs from t.pc until the TGP stalls on an empty input FIFO (returns with
// t.pc at the stalled instruction) or `budget` instructions have completed.
void run(Tgp &t, uint64_t budget);
extern const uint32_t program_crc32; // of the words the code was generated from
extern const uint32_t program_words;
} // namespace tgpgen

} // namespace rt
