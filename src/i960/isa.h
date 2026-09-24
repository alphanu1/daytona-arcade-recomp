// i960 instruction set tables.
//
// The table covers every opcode MAME's i960 disassembler decodes (the whole
// family, so text can be diffed against it). `mame_exec` marks the subset
// MAME's i960KB executor implements; anything else raises fatalerror there,
// so the recompiler treats it as invalid and routes it to the fallback.
#pragma once

#include <cstdint>

namespace i960 {

enum class Format : uint8_t { Invalid, Ctrl, Cobr, Mem, Reg };

// Operand pattern, same encoding as MAME's i960dis.cpp `flags` so formatting
// can follow it exactly. Negative = last operand is a destination.
//   CTRL: 0 none, 1 target
//   COBR: 1 test (dst reg), 3 src1,src2,target
//   MEM:  1 efa (bx/callx/dcinva), 2 load efa,dst, -2 store src,efa
//   REG:  0 none, 1 src1, -1 dst, 2 src1,src2, -2 src1,dst, 3 src1,src2,src3,
//         -3 src1,src2,dst, 33 src1,src2,srcdst; FP: 10, 20, -20, -30
using Pattern = int8_t;

enum class Flow : uint8_t {
    None,       // falls through
    Branch,     // b: unconditional, direct
    CondBranch, // bxx, cmpobxx, cmpibxx, bbc, bbs
    Call,       // call: direct, saves locals
    Bal,        // bal: direct, link in g14, no local save
    Ret,        // ret
    BranchInd,  // bx: indirect via EA
    BalInd,     // balx: indirect, link in dst
    CallInd,    // callx: indirect, saves locals
    CallSys,    // calls: through the system procedure table
    CondFault,  // faultxx
};

struct OpInfo {
    uint16_t code;      // REG: 12-bit (major << 4 | minor); others: major byte
    const char *mnem;
    Format fmt;
    Pattern pat;
    Flow flow;
    bool mame_exec;     // implemented by MAME's i960KB executor
};

// Lookup; nullptr when the encoding has no mnemonic.
const OpInfo *lookup_major(uint8_t major);
const OpInfo *lookup_reg(uint16_t code12);

// Register name for a 5-bit g/l register number (pfp, sp, rip, r3.., g0.., fp).
const char *reg_name(unsigned r);
// Name for a 5-bit FP operand field with M set (fp0-fp3, +0.0, +1.0, "?").
const char *fp_name(unsigned r);

} // namespace i960
