// m2recomp: the i960 static recompiler (design doc, i960 static recompiler).
//
//   m2recomp PROGRAM.bin OUTDIR [--seeds FILE]... [--chunk N]
//
// Decodes every instruction reachable from the boot record plus the seed
// addresses (src/i960/reach) and emits native C++: each instruction's
// semantics inline, with registers, literals and effective-address forms
// resolved here, at recompile time. Known branch targets are direct gotos; an
// indirect transfer (bx, callx, ret, an interrupt) re-dispatches on the
// address. The semantics are MAME's (src/runtime/i960_core.cpp), expression
// for expression, so the lockstep harness can hold the output to MAME.
//
// There is no fallback: an instruction without a native template fails the
// recompile, listed by address and mnemonic. At run time, control reaching an
// address that was not recompiled is a hard error naming it (add it to the
// seeds). Nothing is ever interpreted. The output is portable C++20 (no
// inline assembly, no compiler builtins) for x86-64 and ARM64 alike.
//
// The output is derived from the game: write it under build/ (git-ignored),
// never commit it (rules.md rule 6).
//
// Seeds files: one hex address per line, or the MAME harvest format
// ("kind from target count"; the target column is used).

#include "i960/decode.h"
#include "i960/reach.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using i960::Insn;

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08xu", v);
    return b;
}
std::string lbl(uint32_t a) {
    char b[16];
    std::snprintf(b, sizeof b, "L_%08x", a);
    return b;
}
std::string R(unsigned n) { return "R[" + std::to_string(n) + "]"; }
std::string lit(unsigned n) { return std::to_string(n) + "u"; }

// Operand forms, exactly as MAME's get_1_ri / get_2_ri / get_1_ci / get_2_ci.
std::string s1(const Insn &in) { return in.m1 ? lit(in.src1) : R(in.src1); }
std::string s2(const Insn &in) { return in.m2 ? lit(in.src2) : R(in.src2); }
std::string c1(const Insn &in) { return in.m1 ? lit(in.src1) : R(in.src1); }
std::string c2(const Insn &in) { return R(in.src2); }

// MAME get_ea, with the displacement word and IP-relative base known now.
std::string ea(const Insn &in) {
    const std::string ab = R(in.src2), ix = R(in.index);
    const std::string scaled = "(" + ix + " << " + std::to_string(in.scale) + ")";
    switch (in.ea) {
    case i960::EaMode::Offset: return hex(in.offset);
    case i960::EaMode::AbaseOffset: return "(" + ab + " + " + hex(in.offset) + ")";
    case i960::EaMode::Abase: return ab;
    case i960::EaMode::IpDisp: return hex(in.addr + 8 + in.disp); // disp + address after the displacement word
    case i960::EaMode::AbaseIndex: return "(" + ab + " + " + scaled + ")";
    case i960::EaMode::Disp: return hex(in.disp);
    case i960::EaMode::AbaseDisp: return "(" + hex(in.disp) + " + " + ab + ")";
    case i960::EaMode::IndexDisp: return "(" + hex(in.disp) + " + " + scaled + ")";
    case i960::EaMode::AbaseIndexDisp: return "(" + hex(in.disp) + " + " + ab + " + " + scaled + ")";
    default: return "";
    }
}

struct Emitter {
    const std::set<uint32_t> *chunk_addrs; // instructions emitted in this chunk
    std::ostringstream o;
    uint64_t native = 0;
    std::vector<std::string> *missing; // instructions without a native template

    // Transfer to a known address: a goto when it is in this chunk.
    std::string jump(uint32_t t) const {
        if (chunk_addrs->count(t)) return "goto " + lbl(t) + ";";
        return "{ c.m_IP = " + hex(t) + "; goto dispatch; }";
    }

    void line(const std::string &s) { o << "    " << s << "\n"; }

    // Emits one instruction. Returns false when control never falls through
    // to the next address.
    bool emit(const Insn &in) {
        const uint32_t pc = in.addr, next = pc + in.length;
        const std::string mn = in.op->mnem;
        const uint16_t code = in.fmt == i960::Format::Reg ? in.op->code : uint16_t(in.word >> 24);
        char comment[96];
        std::snprintf(comment, sizeof comment, "// %08x: %s", pc, i960::format_mame(in).c_str());
        o << lbl(pc) << ": " << comment << "\n";
        // Lockstep: MAME's interrupt events happen before this instruction.
        line("c.m_IP = " + hex(pc) + ";");
        line("if (ls.boundary()) goto dispatch;");

        std::string body, exit;   // body runs before ++count; exit transfers control
        bool falls = true;
        const std::string d = R(in.dst);
        auto set = [&](const std::string &v) { body += d + " = " + v + ";"; };
        auto two = [&](const std::string &v) { // read both sources first, as MAME does
            body += "{ const uint32_t t1 = " + s1(in) + ", t2 = " + s2(in) + "; " + d + " = " + v + "; }";
        };
        const uint32_t tgt = in.target, tgt_masked = in.target & ~3u; // bxx/bxx_s mask the target, b/bal/call/bbc/bbs do not

        switch (in.fmt) {
        case i960::Format::Ctrl:
            switch (code) {
            case 0x08: exit = jump(tgt); falls = false; break;                                   // b
            case 0x09: body = "c.m_IP = " + hex(next) + "; c.do_call(" + hex(tgt) + ", 0, R[1]);"; // call
                       exit = jump(tgt); falls = false; break;
            case 0x0a: body = "c.do_ret();"; exit = "goto dispatch;"; falls = false; break;       // ret
            case 0x0b: body = "R[30] = " + hex(next) + ";"; exit = jump(tgt); falls = false; break; // bal
            case 0x10: exit = "if (!(AC & 7)) " + jump(tgt); break;                                // bno (unmasked, as MAME)
            case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
                exit = "if (AC & " + std::to_string(code & 7) + ") " + jump(tgt_masked); break;
            default: return unsupported(in);                                                     // faults
            }
            break;

        case i960::Format::Cobr:
            if (code >= 0x20 && code <= 0x27) { // test*
                const std::string cond = code == 0x20 ? "!(AC & 7)" : "(AC & " + std::to_string(code & 7) + ")";
                body = R(in.src1) + " = " + cond + " ? 1u : 0u;";
            } else if (code == 0x30 || code == 0x37) { // bbc / bbs
                body = "{ const uint32_t t1 = " + c1(in) + " & 0x1f, t2 = " + c2(in) + "; cond = " +
                       (code == 0x37 ? "(t2 & (1u << t1)) != 0" : "!(t2 & (1u << t1))") +
                       "; AC = cond ? ((AC & ~7u) | 2u) : (AC & ~7u); }";
                exit = "if (cond) " + jump(tgt);
            } else if (code >= 0x31 && code <= 0x36) { // cmpob*
                body = "AC = (AC & ~7u) | gen::cc_u(" + c1(in) + ", " + c2(in) + ");";
                exit = "if (AC & " + std::to_string(code & 7) + ") " + jump(tgt_masked);
            } else if (code >= 0x39 && code <= 0x3e) { // cmpib*
                body = "AC = (AC & ~7u) | gen::cc_s(int32_t(" + c1(in) + "), int32_t(" + c2(in) + "));";
                exit = "if (AC & " + std::to_string(code & 7) + ") " + jump(tgt_masked);
            } else return unsupported(in);
            break;

        case i960::Format::Mem: {
            const std::string e = ea(in), r = R(in.src1);
            switch (code) {
            case 0x80: body = r + " = c.bus->read_byte(" + e + ");"; break;                          // ldob
            case 0x82: body = "c.bus->write_byte(" + e + ", uint8_t(" + r + "));"; break;            // stob
            case 0x84: body = "c.m_IP = " + e + ";"; exit = "goto dispatch;"; falls = false; break; // bx
            case 0x85: body = "{ const uint32_t t = " + e + "; " + r + " = " + hex(next) + "; c.m_IP = t; }"; // balx
                       exit = "goto dispatch;"; falls = false; break;
            case 0x86: body = "{ const uint32_t t = " + e + "; c.m_IP = " + hex(next) + "; c.do_call(t, 0, R[1]); }"; // callx
                       exit = "goto dispatch;"; falls = false; break;
            case 0x88: body = r + " = c.i960_read_word_unaligned(" + e + ");"; break;                 // ldos
            case 0x8a: body = "c.i960_write_word_unaligned(" + e + ", uint16_t(" + r + "));"; break;  // stos
            case 0x8c: body = r + " = " + e + ";"; break;                                             // lda
            case 0x90: body = r + " = c.i960_read_dword_unaligned(" + e + ");"; break;                // ld
            case 0x92: body = "c.i960_write_dword_unaligned(" + e + ", " + r + ");"; break;           // st
            case 0x98: case 0xa0: case 0xb0: case 0x9a: case 0xa2: case 0xb2: {                      // ldl/ldt/ldq, stl/stt/stq
                const int n = (code & 0xf0) == 0x90 ? 2 : (code & 0xf0) == 0xa0 ? 3 : 4;
                const unsigned base = in.src1 & (n == 2 ? 0x1eu : 0x1cu);
                const bool store = code & 2;
                // MAME advances the address only on BURST-flagged regions.
                body = "{ uint32_t a = " + e + "; ";
                for (int i = 0; i < n; ++i) {
                    body += store ? "c.i960_write_dword_unaligned(a, " + R(base + i) + "); "
                                  : R(base + i) + " = c.i960_read_dword_unaligned(a); ";
                    if (i + 1 < n) body += "if (c.bus->flags(a) & rt::I960Core::BURST) a += 4; ";
                }
                body += "}";
                break;
            }
            case 0xc0: body = r + " = uint32_t(int32_t(int8_t(c.bus->read_byte(" + e + "))));"; break;     // ldib
            case 0xc2: body = "c.bus->write_byte(" + e + ", uint8_t(" + r + "));"; break;                   // stib
            case 0xc8: body = r + " = uint32_t(int32_t(int16_t(c.i960_read_word_unaligned(" + e + "))));"; break; // ldis
            case 0xca: body = "c.i960_write_word_unaligned(" + e + ", uint16_t(" + r + "));"; break;         // stis
            default: return unsupported(in);
            }
            break;
        }

        case i960::Format::Reg: {
            // An FP operand with its mode bit set names fp0-fp3 or an FP literal,
            // which needs extended-precision state; only the operands each op
            // actually uses count (unused fields may carry mode bits).
            switch (code) {
            case 0x580: two("t2 ^ (1u << (t1 & 31))"); break;             // notbit
            case 0x581: two("t2 & t1"); break;                             // and
            case 0x582: two("t2 & ~t1"); break;                            // andnot
            case 0x583: two("t2 | (1u << (t1 & 31))"); break;             // setbit
            case 0x584: two("(~t2) & t1"); break;                          // notand
            case 0x586: two("t2 ^ t1"); break;                             // xor
            case 0x587: two("t2 | t1"); break;                             // or
            case 0x588: two("(~t2) & (~t1)"); break;                       // nor
            case 0x589: two("~(t2 ^ t1)"); break;                          // xnor
            case 0x58a: set("~" + s1(in)); break;                          // not
            case 0x58b: two("t2 | ~t1"); break;                            // ornot
            case 0x58c: two("t2 & ~(1u << (t1 & 31))"); break;            // clrbit
            case 0x58d: two("(~t2) | t1"); break;                          // notor
            case 0x58e: two("~t2 | ~t1"); break;                           // nand
            case 0x58f: two("(AC & 2) ? (t2 | (1u << (t1 & 31))) : (t2 & ~(1u << (t1 & 31)))"); break; // alterbit
            case 0x590: case 0x591: two("t2 + t1"); break;                 // addo, addi (MAME: no overflow)
            case 0x592: case 0x593: two("t2 - t1"); break;                 // subo, subi
            case 0x598: two("t1 >= 32 ? 0u : t2 >> t1"); break;            // shro
            case 0x59a: two("t1 >= 32 ? 0u : (int32_t(t2) < 0 ? ((t2 & ((1u << t1) - 1)) ? uint32_t((int32_t(t2) >> t1) + 1) "
                            ": uint32_t(int32_t(t2) >> t1)) : t2 >> t1)"); break; // shrdi
            case 0x59b: two("t1 >= 32 ? (int32_t(t2) < 0 ? 0xffffffffu : 0u) : uint32_t(int32_t(t2) >> t1)"); break; // shri
            case 0x59c: case 0x59e: two("t1 >= 32 ? 0u : t2 << t1"); break; // shlo, shli
            case 0x59d: two("std::rotl(t2, int(t1 & 0x1f))"); break;       // rotate
            case 0x5a0: body = "AC = (AC & ~7u) | gen::cc_u(" + s1(in) + ", " + s2(in) + ");"; break; // cmpo
            case 0x5a1: body = "AC = (AC & ~7u) | gen::cc_s(int32_t(" + s1(in) + "), int32_t(" + s2(in) + "));"; break; // cmpi
            case 0x5a2: body = "if (!(AC & 4)) AC = (AC & ~7u) | (" + s1(in) + " <= " + s2(in) + " ? 2u : 1u);"; break; // concmpo
            case 0x5a3: body = "if (!(AC & 4)) AC = (AC & ~7u) | (int32_t(" + s1(in) + ") <= int32_t(" + s2(in) + ") ? 2u : 1u);"; break; // concmpi
            case 0x5a4: case 0x5a5: case 0x5a6: case 0x5a7: {              // cmpinco/i, cmpdeco/i
                const bool sgn = code & 1, inc = code < 0x5a6;
                body = "{ const uint32_t t1 = " + s1(in) + ", t2 = " + s2(in) + "; AC = (AC & ~7u) | " +
                       (sgn ? "gen::cc_s(int32_t(t1), int32_t(t2))" : "gen::cc_u(t1, t2)") + "; " + d + " = t2 " +
                       (inc ? "+" : "-") + " 1; }";
                break;
            }
            case 0x5ac: body = "{ const uint32_t t1 = " + s1(in) + ", t2 = " + s2(in) + "; AC &= ~7u; "
                               "if ((t1 & 0xff000000) == (t2 & 0xff000000) || (t1 & 0x00ff0000) == (t2 & 0x00ff0000) || "
                               "(t1 & 0x0000ff00) == (t2 & 0x0000ff00) || (t1 & 0x000000ff) == (t2 & 0x000000ff)) AC |= 2; }"; break; // scanbyte
            case 0x5ae: body = "{ const uint32_t t1 = " + s1(in) + " & 0x1f, t2 = " + s2(in) + "; AC = (t2 & (1u << t1)) ? ((AC & ~7u) | 2u) : (AC & ~7u); }"; break; // chkbit
            case 0x5cc: set(s1(in)); break;                                // mov
            case 0x5dc: case 0x5ec: case 0x5fc: {                          // movl/movt/movq
                const int n = code == 0x5dc ? 2 : code == 0x5ec ? 3 : 4;
                const unsigned dst = in.dst & (n == 2 ? 0x1eu : 0x1cu);
                if (in.m1) {
                    for (int i = 0; i < n; ++i) body += R(dst + i) + " = " + lit(in.src1) + "; ";
                } else { // copy through temporaries (MAME memcpy; glibc loads before storing for these sizes)
                    body += "{ uint32_t t[4]; ";
                    for (int i = 0; i < n; ++i) body += "t[" + std::to_string(i) + "] = " + R(in.src1 + i) + "; ";
                    for (int i = 0; i < n; ++i) body += R(dst + i) + " = t[" + std::to_string(i) + "]; ";
                    body += "}";
                }
                break;
            }
            case 0x600: body = "{ const uint32_t t1 = " + s1(in) + ", t2 = " + s2(in) + "; if (t1 == 0xff000004u) c.m_ICR = c.bus->read_dword(t2); "
                               "else c.bus->write_dword(t1, c.bus->read_dword(t2)); AC = (AC & ~7u) | 2u; }"; break; // synmov
            case 0x602: body = "{ const uint32_t t1 = " + s1(in) + ", t2 = " + s2(in) + "; if (t1 == 0xff000010u) { c.m_IP = " + hex(next) +
                               "; c.send_iac(t2); } else { for (uint32_t k = 0; k < 16; k += 4) c.bus->write_dword(t1 + k, c.bus->read_dword(t2 + k)); } "
                               "AC = (AC & ~7u) | 2u; }";
                        exit = "if (c.m_IP != " + hex(next) + " && c.m_IP != " + hex(pc) + ") goto dispatch;"; break; // synmovq (an IAC can reinit)
            case 0x645: body = "{ const uint32_t t1 = " + s1(in) + ", t2 = " + s2(in) + "; " + d + " = AC; AC = (AC & ~t1) | (t2 & t1); }"; break; // modac
            case 0x655: body = "{ const uint32_t t1 = c.m_PC, t2 = " + s2(in) + "; c.m_PC = (c.m_PC & ~t2) | (" + d + " & t2); " + d +
                               " = t1; c.m_IP = " + hex(next) + "; if ((t1 >> 16 & 0x1f) > (c.m_PC >> 16 & 0x1f)) c.check_pending_irqs(); }";
                        exit = "if (c.m_IP != " + hex(next) + ") goto dispatch;"; break; // modpc
            case 0x670: body = "{ const uint64_t p = uint64_t(" + s1(in) + ") * uint64_t(" + s2(in) + "); " + d + " = uint32_t(p); " +
                               R(in.dst + 1) + " = uint32_t(p >> 32); }"; break; // emul
            case 0x671: {                                                  // ediv
                const std::string src2 = in.m2 ? "uint64_t(" + lit(in.src2) + ")"
                                               : "(uint64_t(" + R(in.src2) + ") | (uint64_t(" + R(in.src2 + 1) + ") << 32))";
                body = "{ const uint64_t a = " + s1(in) + ", b = " + src2 + "; const uint32_t rem = uint32_t(b % a), quo = uint32_t(b / a); " +
                       d + " = rem; " + R(in.dst + 1) + " = quo; }";
                break;
            }
            case 0x674: if (in.m3) return unsupported(in);                  // cvtir: integer src1, real dst
                        set("gen::f2u(float(double(int32_t(" + s1(in) + "))))"); break;
            case 0x677: if (in.m2 || in.m3) return unsupported(in);         // scaler: integer src1, real src2/dst
                        set("gen::f2u(float(double(gen::u2f(" + R(in.src2) + ")) * std::pow(2.0, double(int32_t(" + s1(in) + ")))))"); break;
            case 0x685: if (in.m1 || in.m2) return unsupported(in);         // cmpr
                        body = "AC = (AC & ~7u) | gen::cc_d(double(gen::u2f(" + R(in.src1) + ")), double(gen::u2f(" + R(in.src2) + ")));"; break; // cmpr
            case 0x6c0: if (in.m1) return unsupported(in);                  // cvtri: real src1, integer dst
                        set("uint32_t(int32_t(gen::round_to_int(double(gen::u2f(" + R(in.src1) + ")), AC)))"); break; // cvtri
            case 0x6c2: if (in.m1) return unsupported(in);                  // cvtzri
                        set("uint32_t(int32_t(double(gen::u2f(" + R(in.src1) + "))))"); break; // cvtzri
            case 0x701: two("t2 * t1"); break;                             // mulo
            case 0x708: two("t2 % t1"); break;                             // remo (MAME: undefined on 0)
            case 0x70b: two("t1 == 0 ? 0u : t2 / t1"); break;              // divo (MAME: 0 on divide by zero)
            case 0x741: two("uint32_t(int32_t(t2) * int32_t(t1))"); break; // muli
            case 0x748: two("uint32_t(int32_t(t2) % int32_t(t1))"); break; // remi
            case 0x74b: two("uint32_t(int32_t(t2) / int32_t(t1))"); break; // divi
            default: return unsupported(in);
            }
            break;
        }
        default: return unsupported(in);
        }

        ++native;
        if (body.find("cond") != std::string::npos) line("{ bool cond;");
        if (!body.empty()) line(body);
        line("++ls.count;");
        if (!exit.empty()) line(exit);
        if (body.find("cond") != std::string::npos) line("}");
        return falls;
    }

    // No native template: recorded, and the recompile fails.
    bool unsupported(const Insn &in) {
        char b[128];
        std::snprintf(b, sizeof b, "%08x: %s", in.addr, i960::format_mame(in).c_str());
        missing->push_back(b);
        return false;
    }
};

std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "m2recomp: cannot open %s\n", path.c_str());
        std::exit(2);
    }
    return {std::istreambuf_iterator<char>(f), {}};
}

std::vector<uint32_t> load_seeds(const std::string &path) {
    std::ifstream f(path);
    std::vector<uint32_t> v;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        std::string a, b, c;
        s >> a;
        if (s >> b >> c) { // harvest format: kind from target count
            if (a == "icr" || a == "irqat") continue;
            v.push_back(uint32_t(std::strtoul(c.c_str(), nullptr, 16)));
        } else {
            v.push_back(uint32_t(std::strtoul(a.c_str(), nullptr, 16)));
        }
    }
    return v;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2recomp PROGRAM.bin OUTDIR [--seeds FILE]... [--chunk N]\n");
        return 2;
    }
    const std::vector<uint8_t> img = load(argv[1]);
    const std::string out = argv[2];
    std::vector<uint32_t> seeds;
    size_t chunk = 1500;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc) {
            auto s = load_seeds(argv[++i]);
            seeds.insert(seeds.end(), s.begin(), s.end());
        } else if (!std::strcmp(argv[i], "--chunk") && i + 1 < argc) {
            chunk = std::strtoul(argv[++i], nullptr, 0);
        }
    }

    // model2o: program ROM at 0, its upper 128 KiB mirrored at 0x00220000.
    auto read = [&](uint32_t a) -> std::optional<uint32_t> {
        uint64_t off;
        if (a < img.size()) off = a;
        else if (a >= 0x220000 && a < 0x240000) off = a - 0x200000;
        else return std::nullopt;
        if (off + 4 > img.size()) return std::nullopt;
        uint32_t v;
        std::memcpy(&v, &img[off], 4);
        return v;
    };
    std::vector<uint32_t> all = i960::boot_seeds(read).all();
    all.insert(all.end(), seeds.begin(), seeds.end());
    const i960::ReachResult r = i960::reach(all, read);

    std::vector<const Insn *> insns;
    for (const auto &[a, in] : r.insns) insns.push_back(&in);

    uint64_t native = 0;
    std::vector<std::string> missing;
    std::vector<std::pair<uint32_t, uint32_t>> ranges; // per chunk: first, last address
    for (size_t ci = 0, start = 0; start < insns.size(); ++ci, start += chunk) {
        const size_t end = std::min(insns.size(), start + chunk);
        std::set<uint32_t> addrs;
        for (size_t k = start; k < end; ++k) addrs.insert(insns[k]->addr);
        Emitter em{&addrs, {}, 0, &missing};
        char name[32];
        std::snprintf(name, sizeof name, "chunk_%03zu", ci);
        em.o << "// Generated by m2recomp. Derived from the game: never commit.\n"
             << "#include \"runtime/gen_support.h\"\n\n"
             << "namespace gen {\n\nvoid " << name << "(Env &e) {\n"
             << "    rt::I960Core &c = e.c;\n    rt::Lockstep &ls = e.ls;\n"
             << "    uint32_t *const R = c.m_r;\n    uint32_t &AC = c.m_AC;\n"
             << "dispatch:\n    if (ls.finished()) return;\n    switch (c.m_IP) {\n";
        for (uint32_t a : addrs) em.o << "    case " << hex(a) << ": goto " << lbl(a) << ";\n";
        em.o << "    default: return;\n    }\n";
        for (size_t k = start; k < end; ++k) {
            const Insn &in = *insns[k];
            const bool falls = em.emit(in);
            const uint32_t next = in.addr + in.length;
            const bool next_is_following = k + 1 < end && insns[k + 1]->addr == next;
            if (falls && !next_is_following) em.line("{ c.m_IP = " + hex(next) + "; goto dispatch; }");
        }
        em.o << "}\n\n} // namespace gen\n";
        std::ofstream(out + "/" + name + ".cpp") << em.o.str();
        native += em.native;
        ranges.emplace_back(insns[start]->addr, insns[end - 1]->addr);
    }

    // Dispatch table and instruction index.
    std::ostringstream t;
    t << "// Generated by m2recomp. Derived from the game: never commit.\n#include \"runtime/gen_support.h\"\n\n"
      << "#include <algorithm>\n\nnamespace gen {\n\n";
    for (size_t i = 0; i < ranges.size(); ++i) t << "void chunk_" << (i < 10 ? "00" : i < 100 ? "0" : "") << i << "(Env &);\n";
    t << "\nnamespace {\nconst uint32_t kAddrs[] = {\n";
    for (const Insn *in : insns) t << "    " << hex(in->addr) << ",\n";
    t << "};\nstruct Range { uint32_t first, last; void (*fn)(Env &); };\nconst Range kChunks[] = {\n";
    for (size_t i = 0; i < ranges.size(); ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "chunk_%03zu", i);
        t << "    {" << hex(ranges[i].first) << ", " << hex(ranges[i].second) << ", " << name << "},\n";
    }
    t << "};\n} // namespace\n\n"
      << "bool has_code(uint32_t a) { return std::binary_search(std::begin(kAddrs), std::end(kAddrs), a); }\n\n"
      << "void run(Env &e) {\n    const uint32_t a = e.c.m_IP;\n"
      << "    for (const Range &r : kChunks)\n        if (a >= r.first && a <= r.last) { r.fn(e); return; }\n}\n\n"
      << "uint64_t native_instructions() { return " << native << "ull; }\n\n} // namespace gen\n";
    if (!missing.empty()) {
        std::fprintf(stderr, "m2recomp: FAILED: %zu reachable instructions have no native template:\n", missing.size());
        for (const std::string &m : missing) std::fprintf(stderr, "  %s\n", m.c_str());
        return 1;
    }
    std::ofstream(out + "/gen_table.cpp") << t.str();

    std::printf("m2recomp: %zu instructions from %zu seeds -> %zu chunks in %s, all native; %zu indirect sites\n",
                insns.size(), all.size(), ranges.size(), out.c_str(), r.indirect_sites.size());
    return 0;
}
