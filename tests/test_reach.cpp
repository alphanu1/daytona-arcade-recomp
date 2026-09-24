// Reachability pass on a hand-assembled image (our own bytes, no ROM data).

#include "i960/reach.h"

#include <cstdio>
#include <map>

namespace {

int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)

std::map<uint32_t, uint32_t> mem;
uint32_t ctrl(uint8_t op, int32_t d) { return uint32_t(op) << 24 | (uint32_t(d) & 0x00fffffc); }
constexpr uint32_t kRet = 0x0a000000;
constexpr uint32_t kBxG14 = 0x84079000;  // bx (g14)
constexpr uint32_t kMov = 0x5c880610;    // mov g0,g1 (any executable REG op)
constexpr uint32_t kInvalid = 0x40000000;

i960::ReadWord reader() {
    return [](uint32_t a) -> std::optional<uint32_t> {
        if (a >= 0x10000) return std::nullopt;
        auto it = mem.find(a);
        return it == mem.end() ? 0u : it->second;
    };
}

} // namespace

int main() {
    // Boot record: SAT at 0x100, PRCB at 0x200, reset IP 0x1000.
    mem[0] = 0x100;
    mem[4] = 0x200;
    mem[12] = 0x1000;
    mem[0x200 + 20] = 0x300;                // interrupt table
    mem[0x300 + 36 + (0x12 - 8) * 4] = 0x3000; // vector 0x12 -> 0x3000
    mem[0x100 + 152] = 0x800;               // system procedure table (clear of the 992-byte interrupt table)
    mem[0x800 + 48] = 0x4002;               // procedure 0, type bits set -> 0x4000
    // procedure 1 left 0: unused slot, must not become a seed.

    // Reset path.
    mem[0x1000] = kMov;
    mem[0x1004] = ctrl(0x0b, 0x100);        // bal 0x1104 (leaf)
    mem[0x1008] = ctrl(0x12, 0x10);         // be 0x1018
    mem[0x100c] = ctrl(0x09, 0x0ff4);       // call 0x2000
    mem[0x1010] = 0x86000300;               // callx 0x300 (MEMA offset) -> indirect site
    mem[0x1014] = ctrl(0x08, 0);            // b . (idle loop)
    mem[0x1018] = kInvalid;                 // conditional path hits an invalid word
    mem[0x1104] = kMov;
    mem[0x1108] = kBxG14;                   // leaf return: indirect, stops
    mem[0x2000] = ctrl(0x08, 0x20000);      // b 0x22000: outside the image
    mem[0x3000] = kRet;
    mem[0x4000] = kRet;

    const i960::BootSeeds bs = i960::boot_seeds(reader());
    CHECK(bs.reset_ip == 0x1000);
    CHECK(bs.interrupt_handlers.size() == 1 && bs.interrupt_handlers[0] == 0x3000);
    CHECK(bs.system_procedures.size() == 1 && bs.system_procedures[0] == 0x4000);

    const i960::ReachResult r = i960::reach(bs.all(), reader());
    for (uint32_t a : {0x1000u, 0x1004u, 0x1008u, 0x100cu, 0x1010u, 0x1014u, 0x1104u, 0x1108u, 0x2000u, 0x3000u, 0x4000u})
        CHECK(r.insns.count(a) == 1);
    CHECK(r.insns.count(0x1018) == 0);
    CHECK(r.insns.size() == 11);
    CHECK(r.indirect_sites.size() == 2); // callx at 0x1010, bx (g14) at 0x1108
    CHECK(r.stops.size() == 1 && r.stops[0] == 0x1018);
    CHECK(r.unmapped_targets.size() == 1 && r.unmapped_targets[0] == 0x22000);

    std::printf("test_reach: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
