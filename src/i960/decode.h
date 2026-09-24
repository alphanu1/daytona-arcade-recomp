// i960 instruction decoder.
//
// Decoding follows MAME's i960 *executor* (i960.cpp), which is the behavioural
// oracle: operand fields, effective-address modes, instruction length and
// branch targets are what MAME would execute. Where MAME's disassembler reads
// the same word differently, the difference is flagged, not resolved here.
#pragma once

#include "isa.h"

#include <cstdint>
#include <string>

namespace i960 {

// MEM effective-address modes (MEMA: bit 12 clear; MEMB: bits 13:10).
enum class EaMode : uint8_t {
    Offset,             // MEMA: offset
    AbaseOffset,        // MEMA: (abase) + offset
    Abase,              // MEMB 0x4: (abase)
    IpDisp,             // MEMB 0x5: address of next instruction + disp
    Reserved,           // MEMB 0x6: MAME executor fatalerror
    AbaseIndex,         // MEMB 0x7: (abase) + (index) << scale
    Disp,               // MEMB 0xc: disp
    AbaseDisp,          // MEMB 0xd: (abase) + disp
    IndexDisp,          // MEMB 0xe: (index) << scale + disp
    AbaseIndexDisp,     // MEMB 0xf: (abase) + (index) << scale + disp
};

// Encodings whose meaning is not settled: MAME's disassembler rejects them,
// or its executor treats them specially, or Ghidra's SLEIGH module (which also
// models later i960 parts) reads them differently. The hardware behaviour for
// these is unconfirmed; every one seen in Daytona's code is a finding.
enum Quirk : uint8_t {
    kQuirkNone = 0,
    kQuirkLowDispBits = 1 << 0, // CTRL/COBR bits 1:0 set: executor adds them to the target
    kQuirkMembBits56 = 1 << 1,  // MEMB bits 6:5 set: ignored by the executor
    kQuirkMembScale = 1 << 2,   // MEMB scale > 4: executor shifts by it anyway
    kQuirkSfr = 1 << 3,         // REG s1/s2 or COBR bit 0 set: special-function
                                // registers (Cx and later); the executor ignores the bit
    kQuirkLiteralDst = 1 << 4,  // destination field is a literal (integer m3, or an FP
                                // literal other than fp0-fp3): executor fatalerror
    kQuirkFpLiteral = 1 << 5,   // FP literal source other than fp0-fp3, +0.0 (16) or
                                // +1.0 (22): the executor reads it as 0.0
    kQuirkTestFields = 1 << 6,  // test*: fields other than the destination non-zero;
                                // the executor ignores them
};

struct Insn {
    uint32_t addr = 0;
    uint32_t word = 0;          // first instruction word
    uint32_t disp = 0;          // second word (MEMB modes with displacement)
    uint8_t length = 4;         // bytes the executor consumes: 4 or 8
    Format fmt = Format::Invalid;
    const OpInfo *op = nullptr; // nullptr: no mnemonic for this encoding

    // Raw fields; meaning depends on format.
    uint8_t src1 = 0;           // REG src1, COBR src1, MEM srcdst, CTRL unused
    uint8_t src2 = 0;           // REG/COBR src2, MEM abase
    uint8_t dst = 0;            // REG srcdst (bits 23:19)
    uint8_t index = 0;          // MEMB index
    uint8_t scale = 0;          // MEMB scale (shift count)
    bool m1 = false, m2 = false, m3 = false; // REG mode bits; COBR uses m1
    bool s1 = false, s2 = false;             // REG special-function bits; COBR s2 = bit 0

    EaMode ea = EaMode::Offset;
    uint32_t offset = 0;        // MEMA offset (12 bits)

    uint32_t target = 0;        // CTRL/COBR branch target, as MAME executes it
    bool has_target = false;

    uint8_t quirks = kQuirkNone;

    bool valid() const { return op != nullptr; }
    // MAME's executor would run this without fatalerror on decode alone.
    bool executable() const {
        return op && op->mame_exec && ea != EaMode::Reserved && !(quirks & kQuirkLiteralDst);
    }
    Flow flow() const { return op ? op->flow : Flow::None; }
};

// Decode the instruction at `addr`. `word1` is the following word; it is only
// read for MEMB modes that carry a displacement (length 8).
Insn decode(uint32_t addr, uint32_t word0, uint32_t word1);

// Text in the exact syntax of MAME's i960 disassembler, so output can be
// diffed against it. `mame_length` receives the length MAME's disassembler
// reports, which differs from Insn::length only for encodings it rejects.
std::string format_mame(const Insn &insn, unsigned *mame_length = nullptr);

} // namespace i960
