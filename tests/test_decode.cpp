// Unit tests for the i960 decoder's executor-side meaning: branch targets,
// effective-address modes, lengths, quirk flags and the executable set. Text
// syntax is covered by the MAME differential test; these cover what text
// cannot show. Encodings are built by hand from the i960 formats.

#include "i960/decode.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)

#define CHECK_TEXT(insn, expected)                                                                 \
    do {                                                                                           \
        ++g_checks;                                                                                \
        const std::string got_ = i960::format_mame(insn);                                          \
        if (got_ != (expected)) {                                                                  \
            ++g_failures;                                                                          \
            std::printf("%s:%d: text [%s], expected [%s]\n", __FILE__, __LINE__, got_.c_str(),     \
                        std::string(expected).c_str());                                            \
        }                                                                                          \
    } while (0)

// Register numbers as encoded: r0-r15 = 0-15, g0-g15 = 16-31.
constexpr uint32_t r(unsigned n) { return n; }
constexpr uint32_t g(unsigned n) { return 16 + n; }

uint32_t ctrl(uint8_t op, int32_t disp) { return uint32_t(op) << 24 | (uint32_t(disp) & 0x00fffffc); }

uint32_t cobr(uint8_t op, uint32_t src1, bool lit1, uint32_t src2, int32_t disp) {
    return uint32_t(op) << 24 | src1 << 19 | src2 << 14 | uint32_t(lit1) << 13 | (uint32_t(disp) & 0x1ffc);
}

uint32_t mema(uint8_t op, uint32_t srcdst, bool use_abase, uint32_t abase, uint32_t offset) {
    return uint32_t(op) << 24 | srcdst << 19 | abase << 14 | uint32_t(use_abase) << 13 | (offset & 0xfff);
}

uint32_t memb(uint8_t op, uint32_t srcdst, uint32_t abase, uint32_t mode, uint32_t scale, uint32_t index) {
    return uint32_t(op) << 24 | srcdst << 19 | abase << 14 | mode << 10 | scale << 7 | index;
}

uint32_t reg(uint16_t code, uint32_t dst, uint32_t src2, uint32_t src1, bool m1 = false, bool m2 = false,
             bool m3 = false) {
    return uint32_t(code >> 4) << 24 | dst << 19 | src2 << 14 | uint32_t(m3) << 13 | uint32_t(m2) << 12 |
           uint32_t(m1) << 11 | uint32_t(code & 0xf) << 7 | src1;
}

void test_ctrl() {
    auto b = i960::decode(0x1000, ctrl(0x08, -8), 0);
    CHECK(b.fmt == i960::Format::Ctrl);
    CHECK(b.flow() == i960::Flow::Branch);
    CHECK(b.has_target && b.target == 0x0ff8);
    CHECK(b.length == 4 && b.quirks == 0 && b.executable());
    CHECK_TEXT(b, "b       0x00000ff8");

    auto call = i960::decode(0x00fffff0, ctrl(0x09, 0x20), 0);
    CHECK(call.flow() == i960::Flow::Call && call.target == 0x01000010);

    // Largest forward and backward displacements.
    CHECK(i960::decode(0x40000000, ctrl(0x08, 0x7ffffc), 0).target == 0x407ffffc);
    CHECK(i960::decode(0x40000000, ctrl(0x08, -0x800000), 0).target == 0x3f800000);

    auto ret = i960::decode(0, 0x0a000000, 0);
    CHECK(ret.flow() == i960::Flow::Ret && !ret.has_target);
    CHECK_TEXT(ret, "ret");

    // Bits 1:0 set: the executor adds them, the disassembler masks them.
    auto odd = i960::decode(0x1000, 0x08fffffa, 0);
    CHECK(odd.quirks & i960::kQuirkLowDispBits);
    CHECK(odd.target == 0x0ffa);
    CHECK_TEXT(odd, "b       0x00000ff8");
    CHECK_TEXT(i960::decode(0x1000, 0x08fffff9, 0), "? 08fffff9"); // bit 0: disassembler rejects

    auto fault = i960::decode(0, 0x1a000000, 0);
    CHECK(fault.flow() == i960::Flow::CondFault && !fault.has_target);

    CHECK(!i960::decode(0, 0x0c000000, 0).valid());
}

void test_cobr() {
    auto c = i960::decode(0x2000, cobr(0x32, 5, true, g(0), 0x10), 0);
    CHECK(c.fmt == i960::Format::Cobr && c.flow() == i960::Flow::CondBranch);
    CHECK(c.m1 && c.src1 == 5 && c.src2 == g(0));
    CHECK(c.target == 0x2010);
    CHECK_TEXT(c, "cmpobe  5,g0,0x2010");

    auto back = i960::decode(0x2000, cobr(0x37, r(4), false, g(1), -0x1000), 0);
    CHECK(back.target == 0x1000);
    CHECK_TEXT(back, "bbs     r4,g1,0x1000");

    auto test = i960::decode(0, cobr(0x22, g(3), false, 0, 0), 0);
    CHECK(test.flow() == i960::Flow::None && !test.has_target);
    CHECK_TEXT(test, "teste   g3");

    // MAME implements neither; they decode but are not executable.
    CHECK(!i960::decode(0, cobr(0x38, 0, false, 0, 0), 0).executable());
    CHECK(!i960::decode(0, cobr(0x3f, 0, false, 0, 0), 0).executable());
    CHECK(i960::decode(0, cobr(0x39, 0, false, 0, 0), 0).executable());

    auto odd = i960::decode(0x2000, cobr(0x32, 0, false, 0, 8) | 2, 0);
    CHECK((odd.quirks & i960::kQuirkLowDispBits) && odd.target == 0x200a);
}

void test_mem() {
    auto a = i960::decode(0, mema(0x90, g(2), true, g(1), 0x123), 0);
    CHECK(a.ea == i960::EaMode::AbaseOffset && a.offset == 0x123 && a.length == 4);
    CHECK_TEXT(a, "ld      0x123(g1),g2");

    auto st = i960::decode(0, mema(0x92, r(5), false, 0, 0xfff), 0);
    CHECK(st.ea == i960::EaMode::Offset && st.offset == 0xfff);
    CHECK_TEXT(st, "st      r5,0xfff");

    auto full = i960::decode(0, memb(0x90, g(4), g(1), 0xf, 2, g(3)), 0x12345678);
    CHECK(full.ea == i960::EaMode::AbaseIndexDisp && full.length == 8);
    CHECK(full.disp == 0x12345678 && full.scale == 2 && full.index == g(3));
    CHECK_TEXT(full, "ld      0x12345678(g1)[g3*4],g4");

    auto ip = i960::decode(0x3000, memb(0x8c, g(0), 0, 0x5, 0, 0), 0x100);
    CHECK(ip.ea == i960::EaMode::IpDisp && ip.length == 8);
    CHECK_TEXT(ip, "lda     0x3108,g0");

    auto ab = i960::decode(0, memb(0x84, 0, g(14), 0x4, 0, 0), 0xdeadbeef);
    CHECK(ab.ea == i960::EaMode::Abase && ab.length == 4 && ab.flow() == i960::Flow::BranchInd);
    CHECK_TEXT(ab, "bx      (g14)");

    auto ix = i960::decode(0, memb(0x80, r(3), r(4), 0x7, 0, r(5)), 0);
    CHECK(ix.ea == i960::EaMode::AbaseIndex && ix.length == 4);
    CHECK_TEXT(ix, "ldob    (r4)[r5],r3");

    auto res = i960::decode(0, memb(0x90, 0, 0, 0x6, 0, 0), 0);
    CHECK(res.ea == i960::EaMode::Reserved && !res.executable());

    auto bits = i960::decode(0, memb(0x90, 0, 0, 0xc, 0, 0) | 0x20, 0x40);
    CHECK((bits.quirks & i960::kQuirkMembBits56) && bits.length == 8 && bits.executable());
    unsigned mame_len = 0;
    CHECK(i960::format_mame(bits, &mame_len) == "? 90003020" && mame_len == 4);

    auto sc = i960::decode(0, memb(0x90, 0, 0, 0xe, 5, 0), 0);
    CHECK((sc.quirks & i960::kQuirkMembScale) && sc.scale == 5);

    CHECK(i960::decode(0, memb(0x85, g(14), 0, 0xc, 0, 0), 0).flow() == i960::Flow::BalInd);
    CHECK(i960::decode(0, memb(0x86, 0, 0, 0xc, 0, 0), 0).flow() == i960::Flow::CallInd);
    CHECK(!i960::decode(0, memb(0xad, 0, 0, 0xc, 0, 0), 0).executable()); // dcinva: not KB
}

void test_reg() {
    auto addo = i960::decode(0, reg(0x590, g(0), r(4), 3, true), 0);
    CHECK(addo.fmt == i960::Format::Reg && addo.executable());
    CHECK(addo.m1 && !addo.m2 && !addo.m3 && addo.src1 == 3 && addo.src2 == r(4) && addo.dst == g(0));
    CHECK_TEXT(addo, "addo    3,r4,g0");

    CHECK_TEXT(i960::decode(0, reg(0x5cc, g(1), 0, g(0)), 0), "mov     g0,g1");
    CHECK_TEXT(i960::decode(0, reg(0x66d, 0, 0, 0), 0), "flushreg");
    CHECK(i960::decode(0, reg(0x660, 0, 0, 7, true), 0).flow() == i960::Flow::CallSys);

    // FP operands: m bits select fp0-fp3 / +0.0 / +1.0.
    CHECK_TEXT(i960::decode(0, reg(0x78c, 1, 22, 0, true, true, true), 0), "mulr    fp0,+1.0,fp1");
    CHECK_TEXT(i960::decode(0, reg(0x68c, g(2), 0, g(0)), 0), "sinr    g0,g2");

    // The undocumented movre encoding MAME executes.
    CHECK(i960::decode(0, reg(0x6e1, 0, 0, 0), 0).executable());
    // MAME disassembles but does not execute these.
    CHECK(!i960::decode(0, reg(0x5ad, 0, 0, 0), 0).executable()); // bswap
    CHECK(!i960::decode(0, reg(0x612, 0, 0, 0), 0).executable()); // atadd
    CHECK(!i960::decode(0, reg(0x780, 0, 0, 0), 0).executable()); // addono
    // ediv shadows the ldtime entry at the same code.
    CHECK_TEXT(i960::decode(0, reg(0x671, g(4), g(2), g(0)), 0), "ediv    g0,g2,g4");
    // No mnemonic at all.
    CHECK(!i960::decode(0, reg(0x585, 0, 0, 0), 0).valid());
    CHECK(!i960::decode(0, 0x62000000, 0).valid());
    CHECK(!i960::decode(0, 0x40000000, 0).valid());
}

// The executable set must be exactly what MAME's i960.cpp implements: 42
// CTRL/COBR opcodes, 20 MEM opcodes and 102 REG opcodes, counted by walking the
// executor's switch at the pinned commit.
void test_exec_counts() {
    int major = 0, regs = 0;
    for (unsigned op = 0; op < 256; ++op)
        if (const i960::OpInfo *o = i960::lookup_major(uint8_t(op)); o && o->mame_exec) ++major;
    for (unsigned c = 0; c < 4096; ++c)
        if (const i960::OpInfo *o = i960::lookup_reg(uint16_t(c)); o && o->mame_exec) ++regs;
    CHECK(major == 62);
    CHECK(regs == 102);
    if (major != 62 || regs != 102) std::printf("  counted major %d, reg %d\n", major, regs);
}

} // namespace

int main() {
    test_ctrl();
    test_cobr();
    test_mem();
    test_reg();
    test_exec_counts();
    std::printf("test_decode: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
