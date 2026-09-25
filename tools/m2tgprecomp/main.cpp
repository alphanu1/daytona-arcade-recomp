// m2tgprecomp: static recompiler for the TGP (Fujitsu MB86234) program.
//
//   m2tgprecomp TGP_PROGRAM.bin OUT.cpp
//
// The TGP has no ROM of its own: at boot the i960 uploads a program (2,024
// words for Daytona, stored in the game's data ROM; scripts/m2import.py
// extracts it). This tool turns that program into native C++: one label per
// word, each instruction's semantics inlined from src/runtime/tgp.h with its
// operand fields as template constants, direct gotos for constant branch
// targets and a dispatch switch for computed ones (brul/bsul/rtif).
//
// Every word gets a label, so a computed branch always lands on native code.
// Words that are not instructions MAME implements (tables, padding, forms it
// logs as unimplemented) compile to a hard error if reached. A reachability
// pass from the reset vector and every return address then checks that no
// such word is reachable by fall-through or a constant branch; if one is, the
// recompile fails. Nothing is interpreted at run time.
//
// The output is derived from the game: write it under build/ (git-ignored),
// never commit it.

#include "runtime/tgp.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string hx(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%xu", v);
    return b;
}

uint32_t crc32(const std::vector<uint8_t> &d) {
    uint32_t c = 0xffffffffu;
    for (uint8_t x : d) {
        c ^= x;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

struct Insn {
    bool known = false;         // an instruction MAME implements
    std::string why;            // if not
    std::string body;           // semantics; may `return` on a FIFO stall
    bool is_rep = false;
    bool falls = true;          // execution can continue at pc+1
    int const_target = -1;      // taken branch to a constant address
    bool computed = false;      // taken branch to a computed address (t.pc)
    bool cond_branch = false;   // branch is conditional (else: always taken)
    bool pushes = false;        // bsif/bsul: pc+1 is a return address
};

std::string stall_check(uint32_t reg, uint32_t pc) {
    if ((reg & 0x3f) != 0x21) return "";
    return " if (t.stall) { t.stall = false; t.pc = " + hx(pc) + "; return; }";
}

Insn decode(uint32_t pc, uint32_t o) {
    Insn in;
    const uint32_t type = (o >> 26) & 0x3f;
    auto bad = [&](const std::string &w) { in.known = false; in.why = w; return in; };
    in.known = true;

    if (type == 0x00 || type == 0x07) {
        const uint32_t r1 = o & 0x1ff, r2 = (o >> 9) & 0x1ff, alu = (o >> 21) & 0x1f, op = (o >> 18) & 7;
        if (!rt::Tgp::alu_known(alu)) return bad("alu op " + hx(alu) + " unimplemented in MAME");
        const std::string A = hx(alu), R1 = hx(r1), R2 = hx(r2);
        std::string b = "t.alu_pre<" + A + ">();";
        if (type == 0x00) { // lab
            std::string v1, v2;
            if (op == 0 || op == 1) {
                v1 = "t.data_r(t.ea_pre_0<" + R1 + ">())";
                v2 = "t.io_r(t.ea_pre_1<" + R2 + ">())";
            } else if (op == 3) {
                v1 = "t.data_r(t.ea_pre_0<" + R1 + ">())";
                v2 = "t.data_r(uint16_t(t.ea_pre_1<" + R2 + ">() + 0x200))";
            } else if (op == 4) {
                v1 = "t.data_r(uint16_t(t.ea_pre_0<" + R1 + ">() + 0x200))";
                v2 = "t.data_r(t.ea_pre_1<" + R2 + ">())";
            } else return bad("lab subop " + hx(op) + " unimplemented in MAME");
            b += " { const uint32_t v1 = " + v1 + "; const uint32_t v2 = " + v2 + "; t.ea_post_0<" + R1 +
                 ">(); t.ea_post_1<" + R2 + ">(); t.a = v1; t.b = v2; } t.alu_post_1<" + A + ">(); t.alu_post_2<" + A + ">();";
            in.body = b;
            return in;
        }
        // ld / mov
        const std::string P1 = " t.alu_post_1<" + A + ">();", P2 = " t.alu_post_2<" + A + ">();";
        std::string m;
        switch (op) {
        case 0: case 1:
            m = " { const uint32_t v = t.data_r(t.ea_pre_0<" + R1 + ">()); t.ea_post_0<" + R1 + ">();" + P1 +
                " t.write_mem_io_1<" + R2 + ">(v); }";
            break;
        case 2:
            m = " { const uint32_t v = t.io_r(t.ea_pre_0<" + R1 + ">()); t.ea_post_0<" + R1 + ">();" + P1 +
                " t.write_mem_internal_1<" + R2 + ">(v, false); }";
            break;
        case 3:
            m = " { const uint32_t v = t.data_r(t.ea_pre_0<" + R1 + ">()); t.ea_post_0<" + R1 + ">();" + P1 +
                " t.write_mem_internal_1<" + R2 + ">(v, true); }";
            break;
        case 4:
            m = " { const uint32_t v = t.data_r(uint16_t(t.ea_pre_0<" + R1 + ">() + 0x200)); t.ea_post_0<" + R1 + ">();" + P1 +
                " t.write_mem_internal_1<" + R2 + ">(v, false); }";
            break;
        case 5:
            m = " { const uint32_t v = t.prog_r(t.ea_pre_0<" + R1 + ">()); t.ea_post_0<" + R1 + ">();" + P1 +
                " t.write_mem_internal_1<" + R2 + ">(v, false); }";
            break;
        case 7:
            switch (r2 >> 6) {
            case 0:
                m = " { const uint32_t v = t.read_reg<" + R2 + ">();" + stall_check(r2, pc) + P1 +
                    " t.write_mem_internal_1<" + R1 + ">(v, false); }";
                break;
            case 1:
                m = " { const uint32_t v = t.read_reg<" + R2 + ">();" + stall_check(r2, pc) + P1 +
                    " t.write_mem_io_1<" + R1 + ">(v); }";
                break;
            case 2:
                m = " { const uint32_t v = t.data_r(uint16_t(t.ea_pre_1<" + R1 + ">() + 0x200)); t.ea_post_1<" + R1 + ">();" +
                    P1 + " t.write_reg<" + R2 + ">(v); }";
                break;
            case 3:
                m = " { const uint32_t v = t.data_r(t.ea_pre_1<" + R1 + ">()); t.ea_post_1<" + R1 + ">();" + P1 +
                    " t.write_reg<" + R2 + ">(v); }";
                break;
            case 4:
                m = " { const uint32_t v = t.io_r(t.ea_pre_1<" + R1 + ">()); t.ea_post_1<" + R1 + ">();" + P1 +
                    " t.write_reg<" + R2 + ">(v); }";
                break;
            case 5:
                m = " { const uint32_t v = t.prog_r(t.ea_pre_0<" + R1 + ">()); t.ea_post_0<" + R1 + ">();" + P1 +
                    " t.write_reg<" + R2 + ">(v); }";
                break;
            case 6:
                m = " { const uint32_t v = t.read_reg<" + R1 + ">();" + stall_check(r1, pc) + P1 +
                    " t.write_reg<" + R2 + ">(v); }";
                break;
            default: return bad("ld/mov subop 7/" + hx(r2 >> 6) + " unimplemented in MAME");
            }
            break;
        default: return bad("ld/mov subop " + hx(op) + " unimplemented in MAME");
        }
        in.body = b + m + P2;
        return in;
    }
    if (type == 0x0d) {
        if (((o >> 17) & 7) != 5) return bad("0d subop unimplemented in MAME");
        in.body = "t.m = " + hx(o & 0xffff) + ";"; // stmh (m is 16 bits)
        return in;
    }
    if (type == 0x0e) {
        const uint32_t s = rt::Tgp::sext24(o);
        switch ((o >> 24) & 3) {
        case 0: in.body = "t.p = (t.p & 0xff000000u) | " + hx(o & 0xffffff) + ";"; break; // lipl
        case 1: in.body = "t.a = " + hx(s) + ";"; break;
        case 2: in.body = "t.b = " + hx(s) + ";"; break;
        case 3: in.body = "t.d = " + hx(s) + ";"; break;
        }
        return in;
    }
    if (type == 0x0f) {
        const uint32_t alu = (o >> 20) & 0x1f, sub2 = (o >> 17) & 7;
        if (!rt::Tgp::alu_known(alu)) return bad("alu op " + hx(alu) + " unimplemented in MAME");
        const std::string A = hx(alu);
        std::string b = "t.alu_pre<" + A + ">();";
        switch (sub2) {
        case 0: // clr0
            if (o & 0x0004) b += " t.a = 0;";
            if (o & 0x0008) b += " t.b = 0;";
            if (o & 0x0010) b += " t.d = 0;";
            break;
        case 1: break; // clr1: MAME does nothing (flag mapping unknown)
        case 2: // rep: MAME skips the ALU post and the repeat check
            in.is_rep = true;
            if (o & 0x8000)
                in.body = b + " { const uint8_t n = uint8_t(t.read_reg<" + hx(o & 0x3f) + ">());" + stall_check(o, pc) + " t.r = n; }";
            else
                in.body = b + " t.r = " + hx(o & 0xff) + ";";
            return in;
        case 3: break; // set: MAME does nothing (flag mapping unknown)
        default: return bad("0f subop " + hx(sub2) + " unimplemented in MAME");
        }
        in.body = b + " t.alu_post_1<" + A + ">();";
        return in;
    }
    if (type >= 0x10 && type <= 0x1f) { // ldi
        in.body = "t.write_reg<" + hx((o >> 24) & 0x3f) + ">(" + hx(rt::Tgp::sext24(o)) + ");";
        return in;
    }
    if (type == 0x2f || type == 0x3f) {
        const uint32_t cond = (o >> 20) & 0x1f, sub = (o >> 17) & 7, data = o & 0xffff;
        const bool invert = o & 0x40000000;
        if (!rt::Tgp::cond_known(cond)) return bad("condition " + hx(cond) + " unimplemented in MAME");
        const std::string C = hx(cond);
        const std::string ea = hx(o & 0x1ff);
        in.cond_branch = !(cond == 0x16 && !invert);
        std::string b = "const bool c = " + std::string(invert ? "!" : "") + "t.cond<" + C + ">(); if (c) {";
        switch (sub) {
        case 0: b += " }"; in.const_target = int(data); break;
        case 2: b += " t.pc = " + hx(pc + 1) + "; t.pcs_push(); }"; in.const_target = int(data); in.pushes = true; break;
        case 1: case 3: {
            if (sub == 3) in.pushes = true;
            std::string push = sub == 3 ? " t.pc = " + hx(pc + 1) + "; t.pcs_push();" : "";
            if (o & 0x4000)
                b += " const uint32_t v = t.read_reg<" + hx(o & 0x3f) + ">();" + stall_check(o, pc) + push + " t.pc = uint16_t(v); }";
            else
                b += " const uint32_t v = t.data_r(t.ea_pre_0<" + ea + ">()); t.ea_post_0<" + ea + ">();" + push +
                     " t.pc = uint16_t(v); }";
            in.computed = true;
            break;
        }
        case 5: b += " t.pcs_pop(); }"; in.computed = true; break; // rtif
        case 6: // ldif: a conditional load, no transfer
            b += " const uint32_t v = t.data_r(t.ea_pre_0<" + ea + ">()); t.ea_post_0<" + ea + ">(); t.write_reg<" +
                 hx((o >> 9) & 0x3f) + ">(v); }";
            break;
        default: return bad("branch subtype " + hx(sub) + " unimplemented in MAME");
        }
        if (sub < 2) b += " t.count_down<" + C + ">();";
        in.body = b;
        in.falls = in.cond_branch || sub == 6;
        return in;
    }
    return bad("opcode type " + hx(type) + " unimplemented in MAME");
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: m2tgprecomp TGP_PROGRAM.bin OUT.cpp\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    const std::vector<uint8_t> raw{std::istreambuf_iterator<char>(f), {}};
    if (raw.empty() || raw.size() % 4 || raw.size() > 0x4000) {
        std::fprintf(stderr, "m2tgprecomp: %s: bad program image\n", argv[1]);
        return 2;
    }
    const uint32_t n = uint32_t(raw.size() / 4);
    std::vector<uint32_t> w(n);
    for (uint32_t i = 0; i < n; i++)
        w[i] = raw[i * 4] | raw[i * 4 + 1] << 8 | raw[i * 4 + 2] << 16 | uint32_t(raw[i * 4 + 3]) << 24;

    std::vector<Insn> ins(n);
    for (uint32_t pc = 0; pc < n; pc++) ins[pc] = decode(pc, w[pc]);

    // Reachability by fall-through and constant branches from the reset
    // vector and every return address. Computed targets (brul/bsul through
    // registers or data RAM) are not followed; they land on a label anyway.
    std::set<uint32_t> seen;
    std::vector<uint32_t> work{0};
    for (uint32_t pc = 0; pc < n; pc++)
        if (ins[pc].known && ins[pc].pushes) work.push_back(pc + 1);
    std::vector<std::string> errors;
    while (!work.empty()) {
        const uint32_t pc = work.back();
        work.pop_back();
        if (!seen.insert(pc).second) continue;
        if (pc >= n) { errors.push_back("control reaches " + hx(pc) + ", past the program"); continue; }
        const Insn &in = ins[pc];
        if (!in.known) {
            char b[160];
            std::snprintf(b, sizeof b, "%03x: %08x: %s", pc, w[pc], in.why.c_str());
            errors.push_back(b);
            continue;
        }
        if (in.falls || in.is_rep) work.push_back(pc + 1);
        if (in.const_target >= 0) work.push_back(uint32_t(in.const_target));
    }
    if (!errors.empty()) {
        std::fprintf(stderr, "m2tgprecomp: %zu reachable word(s) without a native template:\n", errors.size());
        for (auto &e : errors) std::fprintf(stderr, "  %s\n", e.c_str());
        return 1;
    }

    std::ostringstream o;
    o << "// Generated by m2tgprecomp from the TGP program the i960 uploads.\n"
         "// Derived from the game: never commit.\n"
         "#include \"runtime/tgp.h\"\n\n"
         "// Test builds (M2TGP_WITH_HOOK) call Tgp::hook after every instruction.\n"
         "#ifdef M2TGP_WITH_HOOK\n#define M2TGP_HOOK(pc) if (t.hook) t.hook(t, pc)\n#else\n#define M2TGP_HOOK(pc)\n#endif\n\n"
         "namespace rt::tgpgen {\n\n"
      << "const uint32_t program_crc32 = " << hx(crc32(raw)) << ";\n"
      << "const uint32_t program_words = " << n << ";\n\n"
      << "void run(Tgp &t, uint64_t budget) {\n"
         "dispatch:\n    if (t.count >= budget) return;\n    switch (t.pc) {\n";
    for (uint32_t pc = 0; pc < n; pc++) o << "    case " << hx(pc) << ": goto L_" << std::hex << pc << std::dec << ";\n";
    o << "    default: t.fatal(t.pc, \"control outside the recompiled program\");\n    }\n";
    auto go = [&](uint32_t tgt) {
        std::ostringstream s;
        s << "{ if (t.count >= budget) { t.pc = " << hx(tgt) << "; return; } goto L_" << std::hex << tgt << std::dec << "; }";
        return s.str();
    };
    for (uint32_t pc = 0; pc < n; pc++) {
        const Insn &in = ins[pc];
        char head[64];
        std::snprintf(head, sizeof head, "L_%x: // %03x: %08x", pc, pc, w[pc]);
        o << head << (in.known ? "" : (" (" + in.why + ")")) << "\n";
        if (!in.known) {
            o << "    t.fatal(" << hx(pc) << ", \"word is not an instruction MAME implements\");\n";
            continue;
        }
        const bool after_rep = pc > 0 && ins[pc - 1].known && ins[pc - 1].is_rep;
        o << "    { " << in.body << "\n      ++t.count; M2TGP_HOOK(" << hx(pc) << ");\n";
        if (after_rep) o << "      if (t.r != 1) { t.r--; goto L_" << std::hex << pc << std::dec << "; }\n";
        if (in.const_target >= 0) {
            if (in.cond_branch) o << "      if (c) " << go(uint32_t(in.const_target)) << "\n";
            else o << "      " << go(uint32_t(in.const_target)) << "\n";
        } else if (in.computed) {
            if (in.cond_branch) o << "      if (c) goto dispatch;\n";
            else o << "      goto dispatch;\n";
        }
        o << "    }\n";
        if (pc + 1 == n && (in.falls || in.is_rep)) o << "    t.pc = " << hx(n) << "; goto dispatch;\n";
    }
    o << "}\n\n} // namespace rt::tgpgen\n";

    std::ofstream out(argv[2]);
    out << o.str();
    size_t known = 0;
    for (auto &i : ins) known += i.known;
    std::printf("m2tgprecomp: %u words, %zu instructions MAME implements, %zu reachable by constant flow; all native\n", n,
                known, seen.size());
    return 0;
}
