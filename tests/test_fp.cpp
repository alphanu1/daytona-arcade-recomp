// Proves the native FP fast paths bit-identical to the SoftFloat extF80
// reference model (src/i960/fp.h), and checks the reference against hand-
// computed IEEE results.
//
//   test_fp                quick: known values, specials, boundaries, random
//   test_fp --exhaustive   every 2^32 input for cvtri, cvtzri, cvtir; every
//                          2^32 float for scaler at each boundary exponent
//
// Exit 1 on any difference.

#include "i960/fp.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace i960::fp;

namespace {

int g_failures = 0, g_checks = 0;
std::mutex g_mu;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)

uint32_t F(float f) { return std::bit_cast<uint32_t>(f); }

// Run f over [0, count) on all cores; returns the number of mismatches.
uint64_t sweep(uint64_t count, const std::function<bool(uint64_t)> &ok, const char *what) {
    const unsigned n = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<uint64_t> bad{0};
    std::atomic<int> shown{0};
    std::vector<std::thread> ts;
    const auto t0 = std::chrono::steady_clock::now();
    for (unsigned t = 0; t < n; ++t)
        ts.emplace_back([&, t] {
            for (uint64_t i = count * t / n; i < count * (t + 1) / n; ++i)
                if (!ok(i)) {
                    ++bad;
                    if (shown.fetch_add(1) < 5) {
                        std::lock_guard<std::mutex> l(g_mu);
                        std::printf("  %s mismatch at input %llx\n", what, (unsigned long long)i);
                    }
                }
        });
    for (auto &th : ts) th.join();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  %-34s %14llu inputs, %llu mismatches, %.1f s\n", what, (unsigned long long)count,
                (unsigned long long)bad.load(), s);
    return bad.load();
}

void known_values() {
    // cvtri: IEEE round to nearest, ties to even; each rounding mode.
    CHECK(ref_cvtri(F(2.5f), Round::Nearest).value == 2);
    CHECK(ref_cvtri(F(3.5f), Round::Nearest).value == 4);
    CHECK(ref_cvtri(F(-2.5f), Round::Nearest).value == uint32_t(-2));
    CHECK(ref_cvtri(F(2.5f), Round::Down).value == 2);
    CHECK(ref_cvtri(F(2.5f), Round::Up).value == 3);
    CHECK(ref_cvtri(F(-2.5f), Round::Down).value == uint32_t(-3));
    CHECK(ref_cvtri(F(-2.7f), Round::Zero).value == uint32_t(-2));
    CHECK(ref_cvtri(F(2.5f), Round::Nearest).flags == kInexact);
    CHECK(ref_cvtri(F(4.0f), Round::Nearest).flags == 0);
    // Invalid: NaN and out of range give the integer indefinite.
    CHECK(ref_cvtri(0x7fc00000, Round::Nearest).value == 0x80000000u);
    CHECK(ref_cvtri(F(3e9f), Round::Nearest).value == 0x80000000u);
    CHECK(ref_cvtri(F(3e9f), Round::Nearest).flags & kInvalid);
    CHECK(ref_cvtri(F(-2147483648.0f), Round::Nearest).value == 0x80000000u);
    CHECK(ref_cvtri(F(-2147483648.0f), Round::Nearest).flags == 0); // exactly INT32_MIN: valid
    // cvtzri truncates.
    CHECK(ref_cvtzri(F(-2.7f)).value == uint32_t(-2));
    CHECK(ref_cvtzri(F(2.99f)).value == 2);
    // cvtir: 2^24 + 1 is the first int32 a single cannot hold.
    CHECK(ref_cvtir(16777217, Round::Nearest).value == F(16777216.0f));
    CHECK(ref_cvtir(16777217, Round::Up).value == F(16777218.0f));
    CHECK(ref_cvtir(16777217, Round::Nearest).flags == kInexact);
    CHECK(ref_cvtir(uint32_t(-5), Round::Nearest).value == F(-5.0f));
    // cmpr.
    CHECK(ref_cmpr(F(1.0f), F(2.0f)).value == kLess);
    CHECK(ref_cmpr(F(0.0f), F(-0.0f)).value == kEqual);
    CHECK(ref_cmpr(0x7fc00000, F(1.0f)).value == kUnordered);
    CHECK(ref_cmpr(0x7fc00000, F(1.0f)).flags == 0);            // quiet NaN: no invalid
    CHECK(ref_cmpr(0x7f800001, F(1.0f)).flags & kInvalid);      // signalling NaN: invalid
    CHECK(ref_cmpr(F(INFINITY), F(3e38f)).value == kGreater);
    // scaler.
    CHECK(ref_scaler(3, F(1.0f), Round::Nearest).value == F(8.0f));
    CHECK(ref_scaler(uint32_t(-2), F(3.0f), Round::Nearest).value == F(0.75f));
    CHECK(ref_scaler(uint32_t(-150), F(1.0f), Round::Nearest).value == 0);       // tie at half min denormal -> even (0)
    CHECK(ref_scaler(uint32_t(-150), F(1.0f), Round::Up).value == 1);            // -> min denormal
    CHECK(ref_scaler(uint32_t(-149), F(1.0f), Round::Nearest).value == 1);
    CHECK(ref_scaler(128, F(1.0f), Round::Nearest).value == F(INFINITY));
    CHECK(ref_scaler(128, F(1.0f), Round::Zero).value == 0x7f7fffff);           // overflow toward zero: max finite
    CHECK(ref_scaler(0x7fffffff, F(-1.0f), Round::Nearest).value == F(-INFINITY));
    CHECK(ref_scaler(0x80000000, F(1.0f), Round::Nearest).value == 0);
    CHECK(ref_scaler(5, 0x7fc00001, Round::Nearest).value == 0x7fc00001);         // NaN payload kept
}

// Inputs chosen to hit every class and boundary.
std::vector<uint32_t> special_floats() {
    std::vector<uint32_t> v = {0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007fffff, 0x807fffff,
                               0x00800000, 0x80800000, 0x3f800000, 0xbf800000, 0x7f7fffff, 0xff7fffff,
                               0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7f800001, 0xff800001,
                               0x7fbfffff, 0x7fc00001, 0x4effffff, 0x4f000000, 0xcf000000, 0xcf000001,
                               0x3f000000, 0xbf000000, 0x3fc00000, 0x40200000, 0x4b000000, 0x4b7fffff};
    for (int e = 0; e < 256; ++e)
        for (uint32_t m : {0u, 1u, 0x400000u, 0x7fffffu})
            for (uint32_t s : {0u, 0x80000000u}) v.push_back(s | uint32_t(e) << 23 | m);
    return v;
}

} // namespace

int main(int argc, char **argv) {
    const bool exhaustive = argc > 1 && std::strcmp(argv[1], "--exhaustive") == 0;
    known_values();
    std::printf("test_fp: known values: %d checks, %d failures\n", g_checks, g_failures);

    uint64_t bad = 0;
    const auto spec = special_floats();
    const std::vector<int32_t> scale_ns = {0, 1, -1, 2, -2, 23, -23, 24, -24, 125, 126, 127, 128, 254, 255,
                                           -125, -126, -127, -148, -149, -150, -151, -252, -253, 300, -300,
                                           400, -400, 401, -401, 0x7fffffff, int32_t(0x80000000)};

    // Specials against each other and against scale factors.
    for (uint32_t a : spec) {
        bad += fast_cvtri(a) != ref_cvtri(a, Round::Nearest).value;
        bad += fast_cvtzri(a) != ref_cvtzri(a).value;
        for (uint32_t b : spec) bad += fast_cmpr(a, b) != ref_cmpr(a, b).value;
        for (int32_t n : scale_ns) bad += fast_scaler(uint32_t(n), a) != ref_scaler(uint32_t(n), a, Round::Nearest).value;
    }
    std::printf("  specials x specials / scale set: %llu mismatches\n", (unsigned long long)bad);

    std::mt19937_64 seed(960);
    const uint64_t s0 = seed();
    auto rnd = [s0](uint64_t i) { // splitmix64: stateless, thread-safe
        uint64_t z = s0 + i * 0x9e3779b97f4a7c15ull;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    };

    const uint64_t full = uint64_t(1) << 32;
    const uint64_t quick = uint64_t(1) << 26;
    if (exhaustive) {
        bad += sweep(full, [](uint64_t i) { return fast_cvtri(uint32_t(i)) == ref_cvtri(uint32_t(i), Round::Nearest).value; },
                     "cvtri, every single");
        bad += sweep(full, [](uint64_t i) { return fast_cvtzri(uint32_t(i)) == ref_cvtzri(uint32_t(i)).value; },
                     "cvtzri, every single");
        bad += sweep(full, [](uint64_t i) { return fast_cvtir(uint32_t(i)) == ref_cvtir(uint32_t(i), Round::Nearest).value; },
                     "cvtir, every int32");
        for (int32_t n : {0, 1, -1, 127, 128, -126, -127, -149, -150, 254, -300}) {
            char what[64];
            std::snprintf(what, sizeof what, "scaler n=%d, every single", n);
            bad += sweep(full, [n](uint64_t i) {
                return fast_scaler(uint32_t(n), uint32_t(i)) == ref_scaler(uint32_t(n), uint32_t(i), Round::Nearest).value;
            }, what);
        }
    } else {
        bad += sweep(quick, [&](uint64_t i) { uint32_t x = uint32_t(rnd(i)); return fast_cvtri(x) == ref_cvtri(x, Round::Nearest).value; },
                     "cvtri, random singles");
        bad += sweep(quick, [&](uint64_t i) { uint32_t x = uint32_t(rnd(i)); return fast_cvtzri(x) == ref_cvtzri(x).value; },
                     "cvtzri, random singles");
        bad += sweep(quick, [&](uint64_t i) { uint32_t x = uint32_t(rnd(i)); return fast_cvtir(x) == ref_cvtir(x, Round::Nearest).value; },
                     "cvtir, random int32");
    }
    // Always: random scale factors in [-420, 420] and random pairs for cmpr.
    bad += sweep(quick, [&](uint64_t i) {
        const uint64_t r = rnd(i);
        const uint32_t x = uint32_t(r), n = uint32_t(int32_t((r >> 32) % 841) - 420);
        return fast_scaler(n, x) == ref_scaler(n, x, Round::Nearest).value;
    }, "scaler, random n in [-420,420]");
    bad += sweep(quick, [&](uint64_t i) {
        const uint64_t r = rnd(i);
        const uint32_t a = uint32_t(r), b = (i & 1) ? uint32_t(r >> 32) : uint32_t(r) + uint32_t((r >> 32) % 5) - 2;
        return fast_cmpr(a, b) == ref_cmpr(a, b).value;
    }, "cmpr, random and adjacent pairs");

    std::printf("test_fp: %s; %llu fast/reference mismatches; %d known-value failures\n",
                exhaustive ? "exhaustive" : "quick", (unsigned long long)bad, g_failures);
    return (bad || g_failures) ? 1 : 0;
}
