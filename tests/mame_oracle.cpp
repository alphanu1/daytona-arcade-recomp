// Differential test: our decoder + MAME-syntax formatter against MAME's own
// i960 disassembler (extern/mame, compiled unmodified through tests/mame_shim).
//
//   mame_oracle                 random words plus a sweep of every opcode byte
//   mame_oracle --exhaustive    all 2^32 first words (second word randomised)
//
// Prints the first mismatches and a summary; exit status 1 on any mismatch.

#include "i960/decode.h"

#include "emu.h"
#include "cpu/i960/i960dis.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Result {
    uint64_t checked = 0;
    uint64_t mismatches = 0;
};

std::mutex g_print_mutex;
std::atomic<int> g_printed{0};

// Compare one instruction word. `addr` varies so targets and IP-relative
// modes exercise the address arithmetic, not just the fields.
bool check(i960_disassembler &mame, uint32_t addr, uint32_t w0, uint32_t w1) {
    const std::vector<u32> words{w0, w1};
    const util::disasm_interface::data_buffer buf(addr, words);
    std::ostringstream os;
    const offs_t r = mame.disassemble(os, addr, buf, buf);
    const unsigned mame_len = r & util::disasm_interface::LENGTHMASK;

    const i960::Insn in = i960::decode(addr, w0, w1);
    unsigned our_len = 0;
    const std::string ours = i960::format_mame(in, &our_len);

    if (ours == os.str() && our_len == mame_len) return true;
    if (g_printed.fetch_add(1) < 20) {
        std::lock_guard<std::mutex> lock(g_print_mutex);
        std::printf("MISMATCH @%08x %08x %08x\n  mame: [%s] len %u\n  ours: [%s] len %u\n",
                    addr, w0, w1, os.str().c_str(), mame_len, ours.c_str(), our_len);
    }
    return false;
}

Result run_range(uint64_t first, uint64_t last, uint32_t seed) {
    i960_disassembler mame;
    std::mt19937 rng(seed);
    Result res;
    for (uint64_t w = first; w < last; ++w) {
        const uint32_t addr = rng() & ~3u;
        if (!check(mame, addr, uint32_t(w), rng())) ++res.mismatches;
        ++res.checked;
    }
    return res;
}

Result run_random(uint64_t count, uint32_t seed) {
    i960_disassembler mame;
    std::mt19937 rng(seed);
    Result res;
    for (uint64_t i = 0; i < count; ++i) {
        const uint32_t addr = rng() & ~3u;
        const uint32_t w0 = rng();
        if (!check(mame, addr, w0, rng())) ++res.mismatches;
        ++res.checked;
    }
    // Every opcode byte, with every value of the low 24 bits' interesting
    // fields hit many times: 256 bytes x 65,536 random tails.
    for (uint32_t op = 0; op < 256; ++op) {
        for (uint32_t i = 0; i < 65536; ++i) {
            const uint32_t w0 = (op << 24) | (rng() & 0x00ffffff);
            if (!check(mame, rng() & ~3u, w0, rng())) ++res.mismatches;
            ++res.checked;
        }
    }
    return res;
}

} // namespace

int main(int argc, char **argv) {
    const bool exhaustive = argc > 1 && std::strcmp(argv[1], "--exhaustive") == 0;
    const unsigned nthreads = std::max(1u, std::thread::hardware_concurrency());
    const auto t0 = std::chrono::steady_clock::now();

    std::vector<Result> results(nthreads);
    std::vector<std::thread> threads;
    const uint64_t total = exhaustive ? (uint64_t(1) << 32) : 0;
    for (unsigned t = 0; t < nthreads; ++t) {
        threads.emplace_back([&, t] {
            if (exhaustive)
                results[t] = run_range(total * t / nthreads, total * (t + 1) / nthreads, 1234 + t);
            else
                results[t] = run_random(4'000'000 / nthreads, 1234 + t);
        });
    }
    for (auto &th : threads) th.join();

    Result sum;
    for (const Result &r : results) {
        sum.checked += r.checked;
        sum.mismatches += r.mismatches;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s: %llu instruction words compared against MAME i960dis, %llu mismatches, "
                "%.1f s on %u threads\n",
                exhaustive ? "exhaustive" : "random+sweep", (unsigned long long)sum.checked,
                (unsigned long long)sum.mismatches, secs, nthreads);
    return sum.mismatches ? 1 : 0;
}
