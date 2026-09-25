// m2replay: the M1 lockstep harness (design doc, M1 plan), interpreter
// engine. Runs the i960 runtime over the game's own images, answers every
// device read from a MAME trace, replays MAME's interrupt delivery from its
// IRQ log (runtime/lockstep), and stops at the first difference: a device
// access, a written value, a region hash or a register at a sample, or an
// interrupt taken at a different point.
//
//   m2replay IMAGES_DIR TRACE.m2tr IRQ.log
//
// IMAGES_DIR holds program.bin and main_data.bin (scripts/m2import.py).
// Debugging (environment):
//   M2REPLAY_PCLOG=file, M2REPLAY_PCLOG_MAX=n  per-instruction log, same
//       format as the MAME patch's M2TRACE_PCLOG (count, PIP, AC, reg hash)
//   M2REPLAY_REGS_AT=n   print the register file before instruction n
//   M2REPLAY_DUMP_DIR=d  write our copy of a region whose hash differs

#include "runtime/i960_core.h"
#include "runtime/lockstep.h"
#include "runtime/m2_replay_bus.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
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
        std::fprintf(stderr, "usage: m2replay IMAGES_DIR TRACE.m2tr IRQ.log\n");
        return 2;
    }
    const std::string dir = argv[1];
    uint64_t done = 0;
    try {
        rt::M2ReplayBus bus(load(dir + "/program.bin"), load(dir + "/main_data.bin"), argv[2]);
        rt::I960Core core(&bus);
        bus.attach(&core);
        rt::Lockstep ls(core, argv[3]);
        core.reset();

        FILE *pclog = nullptr;
        uint64_t pclog_max = 200000;
        if (const char *p = std::getenv("M2REPLAY_PCLOG")) pclog = std::fopen(p, "w");
        if (const char *m = std::getenv("M2REPLAY_PCLOG_MAX")) pclog_max = std::strtoull(m, nullptr, 0);
        const char *dump_at = std::getenv("M2REPLAY_REGS_AT");
        const uint64_t dump_at_n = dump_at ? std::strtoull(dump_at, nullptr, 0) : 0;

        const auto t0 = std::chrono::steady_clock::now();
        while (!bus.trace_done()) {
            ls.boundary();
            if (ls.finished()) break;
            if (dump_at && ls.count == dump_at_n) {
                for (int i = 0; i < 32; ++i) std::printf("%s%s=%08x", i % 8 ? " " : "\n  ", trace::reg_name(i), core.m_r[i]);
                std::printf("\n  before instruction %" PRIu64 " at %08x\n", ls.count, core.m_IP);
            }
            core.execute_one();
            if (pclog && ls.count < pclog_max) {
                uint64_t h = 0xcbf29ce484222325ull;
                for (int i = 0; i < 32; ++i) h = (h ^ core.m_r[i]) * 0x100000001b3ull;
                std::fprintf(pclog, "%" PRIu64 " %08x %08x %016" PRIx64 "\n", ls.count, core.m_PIP, core.m_AC, h);
            }
            ++ls.count;
            done = ls.count;
        }
        if (!bus.all_events_consumed())
            throw rt::Divergence("MAME's run ended but the trace still has events we did not make; " + bus.where());
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("m2replay: MATCH. %" PRIu64 " instructions, %" PRIu64 " epochs, %" PRIu64
                    " device events, %d interrupts, all identical to MAME; %.1f s (%.1f M instr/s)\n",
                    ls.count, bus.epochs_matched(), bus.events_matched(), ls.interrupts(), s,
                    double(ls.count) / s / 1e6);
        return 0;
    } catch (const rt::Divergence &d) {
        std::printf("m2replay: DIVERGED after %" PRIu64 " instructions: %s\n", done, d.what());
        return 1;
    } catch (const rt::Fatal &f) {
        std::printf("m2replay: FATAL after %" PRIu64 " instructions: %s\n", done, f.what());
        return 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m2replay: %s\n", e.what());
        return 2;
    }
}
