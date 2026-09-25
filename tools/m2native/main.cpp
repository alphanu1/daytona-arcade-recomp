// m2native: the M1 lockstep harness for recompiled code. Same checks as
// m2replay (every device access, every sample's RAM hashes and registers,
// every interrupt against MAME), but the i960 code is the native C++ that
// m2recomp generated. There is no fallback: control reaching an address with
// no recompiled code is a hard error naming it.
//
//   m2native IMAGES_DIR TRACE.m2tr IRQ.log

#include "runtime/gen_support.h"
#include "runtime/lockstep.h"
#include "runtime/m2_replay_bus.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: m2native IMAGES_DIR TRACE.m2tr IRQ.log\n");
        return 2;
    }
    const std::string dir = argv[1];
    uint64_t done = 0;
    try {
        rt::M2ReplayBus bus(load(dir + "/program.bin"), load(dir + "/main_data.bin"), argv[2]);
        rt::Cpu core(&bus);
        bus.attach(&core);
        rt::Lockstep ls(core, argv[3]);
        core.reset();
        gen::Env env{core, ls};

        const auto t0 = std::chrono::steady_clock::now();
        while (!ls.finished() && !bus.trace_done()) {
            if (!gen::has_code(core.m_IP)) {
                char b[128];
                std::snprintf(b, sizeof b, "no recompiled code at %08x (instruction %" PRIu64 "): add it to the seeds",
                              core.m_IP, ls.count);
                throw rt::Fatal(b);
            }
            gen::run(env);
            done = ls.count;
        }
        done = ls.count;
        if (!bus.all_events_consumed())
            throw rt::Divergence("MAME's run ended but the trace still has events we did not make; " + bus.where());
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("m2native: MATCH. %" PRIu64 " instructions, all native (%" PRIu64 " recompiled), %" PRIu64
                    " epochs, %" PRIu64 " device events, %d interrupts, identical to MAME; %.2f s (%.1f M instr/s)\n",
                    ls.count, gen::native_instructions(), bus.epochs_matched(), bus.events_matched(), ls.interrupts(), s,
                    double(ls.count) / s / 1e6);
        return 0;
    } catch (const rt::Divergence &d) {
        std::printf("m2native: DIVERGED after %" PRIu64 " instructions: %s\n", done, d.what());
        return 1;
    } catch (const rt::Fatal &f) {
        std::printf("m2native: FATAL after %" PRIu64 " instructions: %s\n", done, f.what());
        return 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m2native: %s\n", e.what());
        return 2;
    }
}
