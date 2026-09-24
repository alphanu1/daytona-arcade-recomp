// Measures where MAME's i960 FP (host double) disagrees with the extF80
// hardware model (src/i960/fp.h) for the five operations Daytona executes, in
// Daytona's rounding mode (AC 31:30 = 0, round to nearest).
//
// MAME's semantics are transcribed from src/devices/cpu/i960/i960.cpp at
// dddd7368 (BSD-3-Clause):
//   cvtri   (int32_t)round_to_int((double)u2f(x)); mode 0 -> round()
//   cvtzri  (int32_t)(double)u2f(x)
//   cvtir   f2u((float)(double)(int32_t)i)
//   cmpr    cmp_d((double)a, (double)b): < 4, == 2, > 1, else 0
//   scaler  f2u((float)((double)u2f(x) * pow(2.0, (double)(int32_t)n)))
// The (int32_t) casts of NaN or out-of-range doubles are undefined behaviour
// in C++; on x86-64 they produce 0x80000000 (cvttsd2si), on ARM64 they
// saturate (NaN -> 0). Both are modelled; the report says which host is which.
//
//   fp_vs_mame [--quick]
//
// Prints disagreement counts by operation and cause. Always exits 0: this is
// a measurement, not a pass/fail test.

#include "i960/fp.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace i960::fp;

namespace {

enum class Host { X86, Arm };

uint32_t to_i32(double r, Host h) {
    if (r >= -2147483648.0 && r < 2147483648.0) return uint32_t(int32_t(r));
    if (h == Host::X86) return 0x80000000u;
    if (std::isnan(r)) return 0;
    return r < 0 ? 0x80000000u : 0x7fffffffu;
}

uint32_t mame_cvtri(uint32_t x, Host h) { return to_i32(std::round(double(std::bit_cast<float>(x))), h); }
uint32_t mame_cvtzri(uint32_t x, Host h) { return to_i32(std::trunc(double(std::bit_cast<float>(x))), h); }
uint32_t mame_cvtir(uint32_t i) { return std::bit_cast<uint32_t>(float(double(int32_t(i)))); }
uint32_t mame_cmpr(uint32_t a, uint32_t b) {
    const double x = std::bit_cast<float>(a), y = std::bit_cast<float>(b);
    if (x < y) return 4;
    if (x == y) return 2;
    if (x > y) return 1;
    return 0;
}
uint32_t mame_scaler(uint32_t n, uint32_t x) {
    return std::bit_cast<uint32_t>(float(double(std::bit_cast<float>(x)) * std::pow(2.0, double(int32_t(n)))));
}

bool is_nan(uint32_t f) { return (f & 0x7fffffff) > 0x7f800000; }
bool is_inf(uint32_t f) { return (f & 0x7fffffff) == 0x7f800000; }
bool is_zero(uint32_t f) { return (f & 0x7fffffff) == 0; }

struct Tally {
    std::mutex mu;
    std::map<std::string, std::pair<uint64_t, uint32_t>> causes; // cause -> (count, first input)
    void add(const std::string &c, uint32_t in) {
        std::lock_guard<std::mutex> l(mu);
        auto &e = causes[c];
        if (e.first++ == 0) e.second = in;
    }
};

template <typename F> void over(uint64_t count, F f) {
    const unsigned n = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < n; ++t)
        ts.emplace_back([&, t] {
            for (uint64_t i = count * t / n; i < count * (t + 1) / n; ++i) f(i);
        });
    for (auto &th : ts) th.join();
}

void report(const char *op, uint64_t inputs, Tally &t) {
    uint64_t total = 0;
    for (auto &[c, e] : t.causes) total += e.first;
    std::printf("%-8s %12llu inputs, %llu disagree\n", op, (unsigned long long)inputs, (unsigned long long)total);
    for (auto &[c, e] : t.causes)
        std::printf("         %12llu  %-58s e.g. input %08x\n", (unsigned long long)e.first, c.c_str(), e.second);
}

} // namespace

int main(int argc, char **argv) {
    const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
    const uint64_t full = quick ? (uint64_t(1) << 24) : (uint64_t(1) << 32);
    const uint64_t step = quick ? 256 : 1; // quick mode samples every 256th input
    std::printf("MAME (host double) vs extF80 hardware model, round to nearest, %s\n\n",
                quick ? "every 256th input" : "every input");

    for (Host h : {Host::X86, Host::Arm}) {
        const char *hn = h == Host::X86 ? "x86-64" : "ARM64";
        Tally tri, tzri;
        over(full, [&](uint64_t i) {
            const uint32_t x = uint32_t(i * step);
            const uint32_t ref = ref_cvtri(x, Round::Nearest).value, m = mame_cvtri(x, h);
            if (ref != m) {
                const double v = std::bit_cast<float>(x);
                if (is_nan(x)) tri.add(std::string("NaN input (MAME on ") + hn + ")", x);
                else if (!(v >= -2147483648.5 && v < 2147483647.5)) tri.add(std::string("out of int32 range (MAME on ") + hn + ")", x);
                else if (v - std::floor(v) == 0.5) tri.add("tie x.5: MAME round() away from zero, IEEE to even", x);
                else tri.add("other", x);
            }
            const uint32_t rz = ref_cvtzri(x).value, mz = mame_cvtzri(x, h);
            if (rz != mz) tzri.add(std::string(is_nan(x) ? "NaN input" : "out of int32 range") + " (MAME on " + hn + ")", x);
        });
        std::printf("[MAME built for %s]\n", hn);
        report("cvtri", full, tri);
        report("cvtzri", full, tzri);
        std::printf("\n");
    }

    Tally tir;
    over(full, [&](uint64_t i) {
        const uint32_t x = uint32_t(i * step);
        if (ref_cvtir(x, Round::Nearest).value != mame_cvtir(x)) tir.add("any", x);
    });
    report("cvtir", full, tir);

    Tally tcmp;
    const uint64_t pairs = uint64_t(1) << (quick ? 22 : 28);
    over(pairs, [&](uint64_t i) {
        uint64_t z = i * 0x9e3779b97f4a7c15ull;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        z ^= z >> 31;
        const uint32_t a = uint32_t(z), b = (i & 1) ? uint32_t(z >> 32) : a + uint32_t((z >> 32) % 5) - 2;
        if (ref_cmpr(a, b).value != mame_cmpr(a, b)) tcmp.add("any", a);
    });
    report("cmpr", pairs, tcmp);

    Tally tsc;
    const std::vector<int32_t> ns = {0, 1, -1, 127, 128, -126, -149, -150, 254, -300, 1023, 1024, -1075, -1076,
                                     2000, -2000, 0x7fffffff, int32_t(0x80000000)};
    for (int32_t n : ns) {
        over(full, [&](uint64_t i) {
            const uint32_t x = uint32_t(i * step);
            const uint32_t r = ref_scaler(uint32_t(n), x, Round::Nearest).value, m = mame_scaler(uint32_t(n), x);
            if (r == m) return;
            char c[96];
            if (is_zero(x) && is_nan(m))
                std::snprintf(c, sizeof c, "0 * 2^n: MAME pow() -> inf, 0*inf = NaN (n=%d)", n);
            else if (is_inf(x) && is_nan(m))
                std::snprintf(c, sizeof c, "inf * 2^n: MAME pow() -> 0, inf*0 = NaN (n=%d)", n);
            else
                std::snprintf(c, sizeof c, "other (n=%d)", n);
            tsc.add(c, x);
        });
    }
    report("scaler", full * ns.size(), tsc);
    return 0;
}
