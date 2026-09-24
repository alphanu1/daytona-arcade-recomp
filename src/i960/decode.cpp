#include "decode.h"

#include <cstdarg>
#include <cstdio>

namespace i960 {
namespace {

int32_t sext(uint32_t v, unsigned bits) {
    const uint32_t m = 1u << (bits - 1);
    v &= (m << 1) - 1;
    return int32_t((v ^ m) - m);
}

EaMode memb_mode(unsigned mode) {
    switch (mode) {
    case 0x4: return EaMode::Abase;
    case 0x5: return EaMode::IpDisp;
    case 0x7: return EaMode::AbaseIndex;
    case 0xc: return EaMode::Disp;
    case 0xd: return EaMode::AbaseDisp;
    case 0xe: return EaMode::IndexDisp;
    case 0xf: return EaMode::AbaseIndexDisp;
    default: return EaMode::Reserved; // 0x6; bit 12 set rules out 0-3 and 8-b
    }
}

bool ea_has_disp(EaMode ea) {
    return ea == EaMode::IpDisp || ea == EaMode::Disp || ea == EaMode::AbaseDisp ||
           ea == EaMode::IndexDisp || ea == EaMode::AbaseIndexDisp;
}

Format format_of(uint8_t major) {
    if (major < 0x20) return Format::Ctrl;
    if (major < 0x40) return Format::Cobr;
    if (major < 0x58) return Format::Invalid;
    if (major < 0x80) return Format::Reg;
    return Format::Mem;
}

} // namespace

Insn decode(uint32_t addr, uint32_t w, uint32_t w1) {
    Insn in;
    in.addr = addr;
    in.word = w;
    const uint8_t major = uint8_t(w >> 24);
    in.fmt = format_of(major);

    switch (in.fmt) {
    case Format::Invalid:
        break;

    case Format::Ctrl:
        in.op = lookup_major(major);
        // MAME executor: IP (already +4) += sext(opcode, 24) - 4.
        in.target = addr + uint32_t(sext(w, 24));
        in.has_target = in.op && in.op->pat == 1;
        if (w & 3) in.quirks |= kQuirkLowDispBits;
        break;

    case Format::Cobr:
        in.op = lookup_major(major);
        in.src1 = (w >> 19) & 0x1f;
        in.src2 = (w >> 14) & 0x1f;
        in.m1 = (w >> 13) & 1;
        in.s2 = w & 1;
        if (in.op && in.op->pat == 3) {
            in.target = addr + uint32_t(sext(w, 13));
            in.has_target = true;
            if (w & 3) in.quirks |= kQuirkLowDispBits;
            if (w & 1) in.quirks |= kQuirkSfr;
        }
        if (in.op && in.op->pat == 1 && (w & 0x7ffff)) in.quirks |= kQuirkTestFields;
        break;

    case Format::Mem:
        in.op = lookup_major(major);
        in.src1 = (w >> 19) & 0x1f;   // srcdst
        in.src2 = (w >> 14) & 0x1f;   // abase
        if (!(w & 0x1000)) {
            in.ea = (w & 0x2000) ? EaMode::AbaseOffset : EaMode::Offset;
            in.offset = w & 0xfff;
        } else {
            in.ea = memb_mode((w >> 10) & 0xf);
            in.scale = (w >> 7) & 7;
            in.index = w & 0x1f;
            if (w & 0x60) in.quirks |= kQuirkMembBits56;
            if (in.scale > 4) in.quirks |= kQuirkMembScale;
            if (ea_has_disp(in.ea)) {
                in.disp = w1;
                in.length = 8;
            }
        }
        break;

    case Format::Reg:
        in.op = lookup_reg(uint16_t(((w >> 20) & 0xff0) | ((w >> 7) & 0xf)));
        in.dst = (w >> 19) & 0x1f;
        in.src2 = (w >> 14) & 0x1f;
        in.m3 = (w >> 13) & 1;
        in.m2 = (w >> 12) & 1;
        in.m1 = (w >> 11) & 1;
        in.s2 = (w >> 6) & 1;
        in.s1 = (w >> 5) & 1;
        in.src1 = w & 0x1f;
        if (in.s1 || in.s2) in.quirks |= kQuirkSfr;
        if (const OpInfo *op = lookup_reg(uint16_t(((w >> 20) & 0xff0) | ((w >> 7) & 0xf)))) {
            // Operand types as MAME's executor reads them (get_1_ri vs
            // get_1_rif, set_ri vs set_rif), which the disassembler's
            // pattern table does not always reflect.
            const uint16_t c = op->code;
            const bool int_src1 = c == 0x674 || c == 0x675 || c == 0x676 || c == 0x677; // cvtir cvtilr scalerl scaler
            const bool int_dst = c >= 0x6c0 && c <= 0x6c3;                              // cvtri cvtril cvtzri cvtzril
            const bool writes_dst = c == 0x645 || op->pat == 33;                        // modac; src/dst forms
            const int pat = op->pat;
            const bool fp = pat == 10 || pat == 20 || pat == -20 || pat == -30;
            auto fp_lit_ok = [](unsigned r) { return r < 4 || r == 16 || r == 22; };
            if (fp) {
                if (in.m1 && !int_src1 && !fp_lit_ok(in.src1)) in.quirks |= kQuirkFpLiteral;
                if ((pat == 20 || pat == -30) && in.m2 && !fp_lit_ok(in.src2)) in.quirks |= kQuirkFpLiteral;
                if ((pat == -20 || pat == -30) && in.m3 && (int_dst || in.dst >= 4)) in.quirks |= kQuirkLiteralDst;
            } else if ((pat == -1 || pat == -2 || pat == -3 || writes_dst) && in.m3) {
                in.quirks |= kQuirkLiteralDst;
            }
        }
        break;
    }

    if (in.op && in.op->fmt != in.fmt) in.op = nullptr;
    return in;
}

// ---------------------------------------------------------------------------
// MAME-syntax formatter. Mirrors i960dis.cpp case for case, including what it
// rejects, so the two can be compared as strings.

namespace {

std::string fmt(const char *f, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string invalid(const Insn &in, unsigned *len) {
    if (len) *len = 4;
    return fmt("? %08x", in.word);
}

std::string efa_memb(const Insn &in) {
    const char *ab = reg_name(in.src2);
    const char *ix = reg_name(in.index);
    const unsigned sc = 1u << in.scale;
    switch (in.ea) {
    case EaMode::Abase: return fmt("(%s)", ab);
    case EaMode::IpDisp: return fmt("0x%x", in.addr + in.disp + 8);
    case EaMode::AbaseIndex:
        return in.scale ? fmt("(%s)[%s*%u]", ab, ix, sc) : fmt("(%s)[%s]", ab, ix);
    case EaMode::Disp: return fmt("0x%x", in.disp);
    case EaMode::AbaseDisp: return fmt("0x%x(%s)", in.disp, ab);
    case EaMode::IndexDisp:
        return in.scale ? fmt("0x%x[%s*%u]", in.disp, ix, sc) : fmt("0x%x[%s]", in.disp, ix);
    case EaMode::AbaseIndexDisp:
        return in.scale ? fmt("0x%x(%s)[%s*%u]", in.disp, ab, ix, sc)
                        : fmt("0x%x(%s)[%s]", in.disp, ab, ix);
    default: return {};
    }
}

} // namespace

std::string format_mame(const Insn &in, unsigned *len) {
    if (!in.op) return invalid(in, len);
    const char *mn = in.op->mnem;
    const int pat = in.op->pat;
    if (len) *len = 4;

    switch (in.fmt) {
    case Format::Ctrl:
        if (in.word & 1) return invalid(in, len);
        if (pat == 0) return mn;
        // The disassembler masks bits 1:0 out of the displacement.
        return fmt("%-8s0x%08x", mn, in.addr + uint32_t(sext(in.word & 0x00fffffc, 24)));

    case Format::Cobr:
        if (pat == 1) return fmt("%-8s%s", mn, reg_name(in.src1));
        {
            std::string o1 = in.m1 ? fmt("%d", in.src1) : std::string(reg_name(in.src1));
            std::string o2 = in.s2 ? fmt("sf%d", in.src2) : std::string(reg_name(in.src2));
            const uint32_t t = in.addr + uint32_t(sext(in.word & 0x1ffc, 13));
            return fmt("%-8s%s,%s,0x%x", mn, o1.c_str(), o2.c_str(), t);
        }

    case Format::Mem: {
        std::string efa;
        if (in.ea == EaMode::Offset) {
            efa = fmt("0x%x", in.offset);
        } else if (in.ea == EaMode::AbaseOffset) {
            efa = fmt("0x%x(%s)", in.offset, reg_name(in.src2));
        } else {
            if (in.quirks & (kQuirkMembBits56 | kQuirkMembScale)) return invalid(in, len);
            if (in.ea == EaMode::Reserved) return invalid(in, len);
            efa = efa_memb(in);
            if (len) *len = in.length;
        }
        const char *sd = reg_name(in.src1);
        switch (pat) {
        case 1: return fmt("%-8s%s", mn, efa.c_str());
        case 2: return fmt("%-8s%s,%s", mn, efa.c_str(), sd);
        case -2: return fmt("%-8s%s,%s", mn, sd, efa.c_str());
        default: return invalid(in, len);
        }
    }

    case Format::Reg: {
        if ((in.s1 && in.m1) || (in.s2 && in.m2)) return invalid(in, len);
        auto int_op = [](bool s, bool m, unsigned r) {
            return s ? fmt("sf%u", r) : m ? fmt("%u", r) : std::string(reg_name(r));
        };
        auto fp_op = [](bool m, unsigned r) {
            return std::string(m ? fp_name(r) : reg_name(r));
        };
        const std::string o1 = int_op(in.s1, in.m1, in.src1);
        const std::string o2 = int_op(in.s2, in.m2, in.src2);
        const std::string d = in.m3 ? fmt("sf%u", in.dst) : std::string(reg_name(in.dst));
        switch (pat) {
        case 0: return fmt("%-8s", mn);
        case 1: return fmt("%-8s%s", mn, o1.c_str());
        case -1: return fmt("%-8s%s", mn, d.c_str());
        case 2: return fmt("%-8s%s,%s", mn, o1.c_str(), o2.c_str());
        case -2: return fmt("%-8s%s,%s", mn, o1.c_str(), d.c_str());
        case 3: {
            std::string o3 = in.m3 ? fmt("%u", in.dst) : std::string(reg_name(in.dst));
            return fmt("%-8s%s,%s,%s", mn, o1.c_str(), o2.c_str(), o3.c_str());
        }
        case -3: return fmt("%-8s%s,%s,%s", mn, o1.c_str(), o2.c_str(), d.c_str());
        case 33:
            if (in.m3) return invalid(in, len);
            return fmt("%-8s%s,%s,%s", mn, o1.c_str(), o2.c_str(), reg_name(in.dst));
        case 10: return fmt("%-8s%s", mn, fp_op(in.m1, in.src1).c_str());
        case 20:
            return fmt("%-8s%s,%s", mn, fp_op(in.m1, in.src1).c_str(), fp_op(in.m2, in.src2).c_str());
        case -20:
            return fmt("%-8s%s,%s", mn, fp_op(in.m1, in.src1).c_str(), fp_op(in.m3, in.dst).c_str());
        case -30:
            return fmt("%-8s%s,%s,%s", mn, fp_op(in.m1, in.src1).c_str(),
                       fp_op(in.m2, in.src2).c_str(), fp_op(in.m3, in.dst).c_str());
        default: return invalid(in, len);
        }
    }

    case Format::Invalid:
        break;
    }
    return invalid(in, len);
}

} // namespace i960
