// Minimal stand-in for MAME's emu.h, enough to compile MAME's i960dis.cpp
// unmodified as a test oracle. Only what that file uses is provided.
#pragma once

#include <cstdint>
#include <cstdio>
#include <ostream>
#include <string>
#include <type_traits>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using offs_t = uint32_t;

namespace util {

template <typename T>
constexpr std::make_signed_t<T> sext(T value, unsigned bits) {
    using S = std::make_signed_t<T>;
    const unsigned shift = sizeof(T) * 8 - bits;
    return S(value << shift) >> shift;
}

namespace detail {

// MAME's string_format is type-safe printf: the argument's real type decides
// how it prints, whatever length modifier the spec carries.
struct Arg {
    enum { Signed, Unsigned, Str } kind;
    long long s = 0;
    unsigned long long u = 0;
    std::string str;
};

template <typename T> Arg make_arg(const T &v) {
    Arg a;
    if constexpr (std::is_same_v<T, std::string>) {
        a.kind = Arg::Str;
        a.str = v;
    } else if constexpr (std::is_convertible_v<T, const char *>) {
        a.kind = Arg::Str;
        a.str = v;
    } else if constexpr (std::is_signed_v<T>) {
        a.kind = Arg::Signed;
        a.s = v;
    } else {
        a.kind = Arg::Unsigned;
        a.u = v;
    }
    return a;
}

inline std::string format(const char *f, const std::vector<Arg> &args) {
    std::string out;
    size_t ai = 0;
    for (const char *p = f; *p; ++p) {
        if (*p != '%') { out += *p; continue; }
        if (p[1] == '%') { out += '%'; ++p; continue; }
        std::string spec = "%";
        ++p;
        while (*p && std::string("-+ #0123456789.").find(*p) != std::string::npos) spec += *p++;
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 'j') ++p; // length from the type
        const char conv = *p;
        const Arg &a = args.at(ai++);
        char buf[256];
        if (conv == 's') {
            snprintf(buf, sizeof buf, (spec + "s").c_str(), a.str.c_str());
        } else {
            // MAME's type-safe printf prints the value in the argument's own
            // width: a negative int under %x shows 32 bits, not 64.
            const unsigned long long bits = a.kind == Arg::Signed ? (unsigned long long)(unsigned)(int)a.s : a.u;
            if (conv == 'd' || conv == 'i')
                snprintf(buf, sizeof buf, (spec + "lld").c_str(), a.kind == Arg::Signed ? a.s : (long long)a.u);
            else
                snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), bits);
        }
        out += buf;
    }
    return out;
}

} // namespace detail

template <typename... A> std::string string_format(const char *f, const A &...a) {
    return detail::format(f, {detail::make_arg(a)...});
}

template <typename... A> void stream_format(std::ostream &os, const char *f, const A &...a) {
    os << string_format(f, a...);
}

class disasm_interface {
public:
    enum : offs_t {
        FLAGS_MASK = 0xf0000000,
        LENGTHMASK = 0x0000ffff,
        STEP_OUT = 0x80000000,
        STEP_OVER = 0x40000000,
        STEP_COND = 0x20000000,
        SUPPORTED = 0x10000000,
    };

    class data_buffer {
    public:
        data_buffer(offs_t base, const std::vector<u32> &words) : m_base(base), m_words(words) {}
        u32 r32(offs_t pc) const { return m_words.at((pc - m_base) / 4); }

    private:
        offs_t m_base;
        const std::vector<u32> &m_words;
    };

    virtual ~disasm_interface() = default;
    virtual u32 opcode_alignment() const = 0;
    virtual offs_t disassemble(std::ostream &stream, offs_t pc, const data_buffer &opcodes,
                               const data_buffer &params) = 0;
};

} // namespace util

using util::disasm_interface;
