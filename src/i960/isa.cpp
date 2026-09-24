#include "isa.h"

#include <array>

namespace i960 {
namespace {

constexpr Format C = Format::Ctrl;
constexpr Format B = Format::Cobr;
constexpr Format M = Format::Mem;
constexpr Format R = Format::Reg;
constexpr bool X = true;  // MAME executes it
constexpr bool _ = false; // disassembler only

// Non-REG opcodes, keyed by the major byte.
constexpr OpInfo kMajor[] = {
    {0x08, "b", C, 1, Flow::Branch, X},
    {0x09, "call", C, 1, Flow::Call, X},
    {0x0a, "ret", C, 0, Flow::Ret, X},
    {0x0b, "bal", C, 1, Flow::Bal, X},

    {0x10, "bno", C, 1, Flow::CondBranch, X},
    {0x11, "bg", C, 1, Flow::CondBranch, X},
    {0x12, "be", C, 1, Flow::CondBranch, X},
    {0x13, "bge", C, 1, Flow::CondBranch, X},
    {0x14, "bl", C, 1, Flow::CondBranch, X},
    {0x15, "bne", C, 1, Flow::CondBranch, X},
    {0x16, "ble", C, 1, Flow::CondBranch, X},
    {0x17, "bo", C, 1, Flow::CondBranch, X},
    {0x18, "faultno", C, 0, Flow::CondFault, X},
    {0x19, "faultg", C, 0, Flow::CondFault, X},
    {0x1a, "faulte", C, 0, Flow::CondFault, X},
    {0x1b, "faultge", C, 0, Flow::CondFault, X},
    {0x1c, "faultl", C, 0, Flow::CondFault, X},
    {0x1d, "faultne", C, 0, Flow::CondFault, X},
    {0x1e, "faultle", C, 0, Flow::CondFault, X},
    {0x1f, "faulto", C, 0, Flow::CondFault, X},

    {0x20, "testno", B, 1, Flow::None, X},
    {0x21, "testg", B, 1, Flow::None, X},
    {0x22, "teste", B, 1, Flow::None, X},
    {0x23, "testge", B, 1, Flow::None, X},
    {0x24, "testl", B, 1, Flow::None, X},
    {0x25, "testne", B, 1, Flow::None, X},
    {0x26, "testle", B, 1, Flow::None, X},
    {0x27, "testo", B, 1, Flow::None, X},

    {0x30, "bbc", B, 3, Flow::CondBranch, X},
    {0x31, "cmpobg", B, 3, Flow::CondBranch, X},
    {0x32, "cmpobe", B, 3, Flow::CondBranch, X},
    {0x33, "cmpobge", B, 3, Flow::CondBranch, X},
    {0x34, "cmpobl", B, 3, Flow::CondBranch, X},
    {0x35, "cmpobne", B, 3, Flow::CondBranch, X},
    {0x36, "cmpoble", B, 3, Flow::CondBranch, X},
    {0x37, "bbs", B, 3, Flow::CondBranch, X},
    {0x38, "cmpibno", B, 3, Flow::CondBranch, _},
    {0x39, "cmpibg", B, 3, Flow::CondBranch, X},
    {0x3a, "cmpibe", B, 3, Flow::CondBranch, X},
    {0x3b, "cmpibge", B, 3, Flow::CondBranch, X},
    {0x3c, "cmpibl", B, 3, Flow::CondBranch, X},
    {0x3d, "cmpibne", B, 3, Flow::CondBranch, X},
    {0x3e, "cmpible", B, 3, Flow::CondBranch, X},
    {0x3f, "cmpibo", B, 3, Flow::CondBranch, _},

    {0x80, "ldob", M, 2, Flow::None, X},
    {0x82, "stob", M, -2, Flow::None, X},
    {0x84, "bx", M, 1, Flow::BranchInd, X},
    {0x85, "balx", M, 2, Flow::BalInd, X},
    {0x86, "callx", M, 1, Flow::CallInd, X},
    {0x88, "ldos", M, 2, Flow::None, X},
    {0x8a, "stos", M, -2, Flow::None, X},
    {0x8c, "lda", M, 2, Flow::None, X},
    {0x90, "ld", M, 2, Flow::None, X},
    {0x92, "st", M, -2, Flow::None, X},
    {0x98, "ldl", M, 2, Flow::None, X},
    {0x9a, "stl", M, -2, Flow::None, X},
    {0xa0, "ldt", M, 2, Flow::None, X},
    {0xa2, "stt", M, -2, Flow::None, X},
    {0xad, "dcinva", M, 1, Flow::None, _},
    {0xb0, "ldq", M, 2, Flow::None, X},
    {0xb2, "stq", M, -2, Flow::None, X},
    {0xc0, "ldib", M, 2, Flow::None, X},
    {0xc2, "stib", M, -2, Flow::None, X},
    {0xc8, "ldis", M, 2, Flow::None, X},
    {0xca, "stis", M, -2, Flow::None, X},
};

// REG opcodes, keyed by the 12-bit code. MAME's table also lists `ldtime` at
// 0x671, which `ediv` shadows; it can never decode and is left out.
constexpr OpInfo kReg[] = {
    {0x580, "notbit", R, -3, Flow::None, X},
    {0x581, "and", R, -3, Flow::None, X},
    {0x582, "andnot", R, -3, Flow::None, X},
    {0x583, "setbit", R, -3, Flow::None, X},
    {0x584, "notand", R, -3, Flow::None, X},
    {0x586, "xor", R, -3, Flow::None, X},
    {0x587, "or", R, -3, Flow::None, X},
    {0x588, "nor", R, -3, Flow::None, X},
    {0x589, "xnor", R, -3, Flow::None, X},
    {0x58a, "not", R, -2, Flow::None, X},
    {0x58b, "ornot", R, -3, Flow::None, X},
    {0x58c, "clrbit", R, -3, Flow::None, X},
    {0x58d, "notor", R, -3, Flow::None, X},
    {0x58e, "nand", R, -3, Flow::None, X},
    {0x58f, "alterbit", R, -3, Flow::None, X},

    {0x590, "addo", R, -3, Flow::None, X},
    {0x591, "addi", R, -3, Flow::None, X},
    {0x592, "subo", R, -3, Flow::None, X},
    {0x593, "subi", R, -3, Flow::None, X},
    {0x594, "cmpob", R, 2, Flow::None, _},
    {0x595, "cmpib", R, 2, Flow::None, _},
    {0x596, "cmpos", R, 2, Flow::None, _},
    {0x597, "cmpis", R, 2, Flow::None, _},
    {0x598, "shro", R, -3, Flow::None, X},
    {0x59a, "shrdi", R, -3, Flow::None, X},
    {0x59b, "shri", R, -3, Flow::None, X},
    {0x59c, "shlo", R, -3, Flow::None, X},
    {0x59d, "rotate", R, -3, Flow::None, X},
    {0x59e, "shli", R, -3, Flow::None, X},

    {0x5a0, "cmpo", R, 2, Flow::None, X},
    {0x5a1, "cmpi", R, 2, Flow::None, X},
    {0x5a2, "concmpo", R, 2, Flow::None, X},
    {0x5a3, "concmpi", R, 2, Flow::None, X},
    {0x5a4, "cmpinco", R, -3, Flow::None, X},
    {0x5a5, "cmpinci", R, -3, Flow::None, X},
    {0x5a6, "cmpdeco", R, -3, Flow::None, X},
    {0x5a7, "cmpdeci", R, -3, Flow::None, X},
    {0x5ac, "scanbyte", R, 2, Flow::None, X},
    {0x5ad, "bswap", R, -2, Flow::None, _},
    {0x5ae, "chkbit", R, 2, Flow::None, X},

    {0x5b0, "addc", R, -3, Flow::None, X},
    {0x5b2, "subc", R, -3, Flow::None, X},
    {0x5b4, "intdis", R, 0, Flow::None, _},
    {0x5b5, "inten", R, 0, Flow::None, _},

    {0x5cc, "mov", R, -2, Flow::None, X},
    {0x5d8, "eshro", R, -3, Flow::None, _},
    {0x5dc, "movl", R, -2, Flow::None, X},
    {0x5ec, "movt", R, -2, Flow::None, X},
    {0x5fc, "movq", R, -2, Flow::None, X},

    {0x600, "synmov", R, 2, Flow::None, X},
    {0x601, "synmovl", R, 2, Flow::None, _},
    {0x602, "synmovq", R, 2, Flow::None, X},
    {0x603, "cmpstr", R, 3, Flow::None, _},
    {0x604, "movqstr", R, -3, Flow::None, _},
    {0x605, "movstr", R, -3, Flow::None, _},

    {0x610, "atmod", R, 33, Flow::None, _},
    {0x612, "atadd", R, 33, Flow::None, _},
    {0x613, "inspacc", R, -2, Flow::None, _},
    {0x614, "ldphy", R, -2, Flow::None, _},
    {0x615, "synld", R, -2, Flow::None, _},
    {0x617, "fill", R, 3, Flow::None, _},

    {0x630, "sdma", R, 3, Flow::None, _},
    {0x631, "udma", R, 0, Flow::None, _},

    {0x640, "spanbit", R, -2, Flow::None, X},
    {0x641, "scanbit", R, -2, Flow::None, X},
    {0x642, "daddc", R, -3, Flow::None, _},
    {0x643, "dsubc", R, -3, Flow::None, _},
    {0x644, "dmovt", R, -2, Flow::None, X},
    {0x645, "modac", R, 3, Flow::None, X},

    {0x650, "modify", R, 33, Flow::None, _},
    {0x651, "extract", R, 33, Flow::None, _},
    {0x654, "modtc", R, 33, Flow::None, _},
    {0x655, "modpc", R, 33, Flow::None, X},
    {0x656, "receive", R, -2, Flow::None, _},
    {0x658, "intctl", R, -2, Flow::None, _},
    {0x659, "sysctl", R, 33, Flow::None, _},
    {0x65b, "icctl", R, 33, Flow::None, _},
    {0x65c, "dcctl", R, 33, Flow::None, _},
    {0x65d, "halt", R, 0, Flow::None, _},

    {0x660, "calls", R, 1, Flow::CallSys, X},
    {0x662, "send", R, -3, Flow::None, _},
    {0x663, "sendserv", R, 1, Flow::None, _},
    {0x664, "resumprcs", R, 1, Flow::None, _},
    {0x665, "schedprcs", R, 1, Flow::None, _},
    {0x666, "saveprcs", R, 0, Flow::None, _},
    {0x668, "condwait", R, 1, Flow::None, _},
    {0x669, "wait", R, 1, Flow::None, _},
    {0x66a, "signal", R, 1, Flow::None, _},
    {0x66b, "mark", R, 0, Flow::None, _},
    {0x66c, "fmark", R, 0, Flow::None, _},
    {0x66d, "flushreg", R, 0, Flow::None, X},
    {0x66f, "syncf", R, 0, Flow::None, _},

    {0x670, "emul", R, -3, Flow::None, X},
    {0x671, "ediv", R, -3, Flow::None, X},
    {0x674, "cvtir", R, -20, Flow::None, X},
    {0x675, "cvtilr", R, -20, Flow::None, X},
    {0x676, "scalerl", R, -30, Flow::None, X},
    {0x677, "scaler", R, -30, Flow::None, X},

    {0x680, "atanr", R, -30, Flow::None, X},
    {0x681, "logepr", R, -30, Flow::None, X},
    {0x682, "logr", R, -30, Flow::None, X},
    {0x683, "remr", R, -30, Flow::None, X},
    {0x684, "cmpor", R, 20, Flow::None, _},
    {0x685, "cmpr", R, 20, Flow::None, X},
    {0x688, "sqrtr", R, -20, Flow::None, X},
    {0x689, "expr", R, -20, Flow::None, X},
    {0x68a, "logbnr", R, -20, Flow::None, X},
    {0x68b, "roundr", R, -20, Flow::None, X},
    {0x68c, "sinr", R, -20, Flow::None, X},
    {0x68d, "cosr", R, -20, Flow::None, X},
    {0x68e, "tanr", R, -20, Flow::None, X},
    {0x68f, "classr", R, 10, Flow::None, _},

    {0x690, "atanrl", R, -30, Flow::None, X},
    {0x691, "logeprl", R, -30, Flow::None, _},
    {0x692, "logrl", R, -30, Flow::None, X},
    {0x693, "remrl", R, -30, Flow::None, _},
    {0x694, "cmporl", R, 20, Flow::None, _},
    {0x695, "cmprl", R, 20, Flow::None, X},
    {0x698, "sqrtrl", R, -20, Flow::None, X},
    {0x699, "exprl", R, -20, Flow::None, X},
    {0x69a, "logbnrl", R, -20, Flow::None, X},
    {0x69b, "roundrl", R, -20, Flow::None, X},
    {0x69c, "sinrl", R, -20, Flow::None, X},
    {0x69d, "cosrl", R, -20, Flow::None, X},
    {0x69e, "tanrl", R, -20, Flow::None, X},
    {0x69f, "classrl", R, 10, Flow::None, _},

    {0x6c0, "cvtri", R, -20, Flow::None, X},
    {0x6c1, "cvtril", R, -20, Flow::None, X},
    {0x6c2, "cvtzri", R, -20, Flow::None, X},
    {0x6c3, "cvtzril", R, -20, Flow::None, X},
    {0x6c9, "movr", R, -20, Flow::None, X},
    {0x6d9, "movrl", R, -20, Flow::None, X},
    // 0x6e1 is undocumented; MAME executes it as movre (Dead or Alive uses it).
    {0x6e1, "movre", R, -20, Flow::None, X},
    {0x6e2, "cpysre", R, -30, Flow::None, X},
    {0x6e3, "cpyrsre", R, -30, Flow::None, _},
    {0x6e9, "movre", R, -20, Flow::None, X},

    {0x701, "mulo", R, -3, Flow::None, X},
    {0x708, "remo", R, -3, Flow::None, X},
    {0x70b, "divo", R, -3, Flow::None, X},
    {0x741, "muli", R, -3, Flow::None, X},
    {0x748, "remi", R, -3, Flow::None, X},
    {0x749, "modi", R, -3, Flow::None, X},
    {0x74b, "divi", R, -3, Flow::None, X},

    {0x780, "addono", R, -3, Flow::None, _},
    {0x781, "addino", R, -3, Flow::None, _},
    {0x782, "subono", R, -3, Flow::None, _},
    {0x783, "subino", R, -3, Flow::None, _},
    {0x784, "selno", R, -3, Flow::None, _},
    {0x78b, "divr", R, -30, Flow::None, X},
    {0x78c, "mulr", R, -30, Flow::None, X},
    {0x78d, "subr", R, -30, Flow::None, X},
    {0x78f, "addr", R, -30, Flow::None, X},

    {0x790, "addog", R, -3, Flow::None, _},
    {0x791, "addig", R, -3, Flow::None, _},
    {0x792, "subog", R, -3, Flow::None, _},
    {0x793, "subig", R, -3, Flow::None, _},
    {0x794, "selg", R, -3, Flow::None, _},
    {0x79b, "divrl", R, -30, Flow::None, X},
    {0x79c, "mulrl", R, -30, Flow::None, X},
    {0x79d, "subrl", R, -30, Flow::None, X},
    {0x79f, "addrl", R, -30, Flow::None, X},

    {0x7a0, "addoe", R, -3, Flow::None, _},
    {0x7a1, "addie", R, -3, Flow::None, _},
    {0x7a2, "suboe", R, -3, Flow::None, _},
    {0x7a3, "subie", R, -3, Flow::None, _},
    {0x7a4, "sele", R, -3, Flow::None, _},
    {0x7b0, "addoge", R, -3, Flow::None, _},
    {0x7b1, "addige", R, -3, Flow::None, _},
    {0x7b2, "suboge", R, -3, Flow::None, _},
    {0x7b3, "subige", R, -3, Flow::None, _},
    {0x7b4, "selge", R, -3, Flow::None, _},
    {0x7c0, "addol", R, -3, Flow::None, _},
    {0x7c1, "addil", R, -3, Flow::None, _},
    {0x7c2, "subol", R, -3, Flow::None, _},
    {0x7c3, "subil", R, -3, Flow::None, _},
    {0x7c4, "sell", R, -3, Flow::None, _},
    {0x7d0, "addone", R, -3, Flow::None, _},
    {0x7d1, "addine", R, -3, Flow::None, _},
    {0x7d2, "subone", R, -3, Flow::None, _},
    {0x7d3, "subine", R, -3, Flow::None, _},
    {0x7d4, "selne", R, -3, Flow::None, _},
    {0x7e0, "addole", R, -3, Flow::None, _},
    {0x7e1, "addile", R, -3, Flow::None, _},
    {0x7e2, "subole", R, -3, Flow::None, _},
    {0x7e3, "subile", R, -3, Flow::None, _},
    {0x7e4, "selle", R, -3, Flow::None, _},
    {0x7f0, "addoo", R, -3, Flow::None, _},
    {0x7f1, "addio", R, -3, Flow::None, _},
    {0x7f2, "suboo", R, -3, Flow::None, _},
    {0x7f3, "subio", R, -3, Flow::None, _},
    {0x7f4, "selo", R, -3, Flow::None, _},
};

struct Tables {
    std::array<const OpInfo *, 256> major{};
    std::array<const OpInfo *, 4096> reg{};
    Tables() {
        for (const OpInfo &op : kMajor)
            major[op.code] = &op;
        for (const OpInfo &op : kReg)
            reg[op.code] = &op;
    }
};

const Tables &tables() {
    static const Tables t;
    return t;
}

constexpr const char *kRegNames[32] = {
    "pfp", "sp", "rip", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    "g0", "g1", "g2", "g3", "g4", "g5", "g6", "g7",
    "g8", "g9", "g10", "g11", "g12", "g13", "g14", "fp",
};

constexpr const char *kFpNames[32] = {
    "fp0", "fp1", "fp2", "fp3", "?", "?", "?", "?",
    "?", "?", "?", "?", "?", "?", "?", "?",
    "+0.0", "?", "?", "?", "?", "?", "+1.0", "?",
    "?", "?", "?", "?", "?", "?", "?", "?",
};

} // namespace

const OpInfo *lookup_major(uint8_t major) { return tables().major[major]; }
const OpInfo *lookup_reg(uint16_t code12) { return tables().reg[code12 & 0xfff]; }
const char *reg_name(unsigned r) { return kRegNames[r & 31]; }
const char *fp_name(unsigned r) { return kFpNames[r & 31]; }

} // namespace i960
