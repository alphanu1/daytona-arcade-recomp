// m1replay: the M1 lockstep harness (design doc, M1 plan). Runs the i960
// runtime over the game's own images, answers every device read from a MAME
// trace, replays MAME's interrupt delivery from its IRQ log (keyed by
// completed-instruction count), and stops at the first difference: a device
// access, a written value, a region hash or a register at a sample, or an
// interrupt taken at a different point.
//
//   m1replay IMAGES_DIR TRACE.m2tr IRQ.log
//
// IMAGES_DIR holds program.bin and main_data.bin (scripts/m2import.py).

#include "runtime/i960_core.h"
#include "runtime/m2_replay_bus.h"

#include <chrono>
#include <cstdlib>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

struct IrqEvent {
    enum Kind { Line, Imm, Pend, End } kind;
    uint64_t count;
    int a = 0, b = 0; // line: line, state; imm/pend: vector
    uint32_t ip = 0;
};

std::vector<IrqEvent> load_irq_log(const std::string &path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<IrqEvent> v;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream s(line);
        std::string k;
        IrqEvent e{};
        s >> k >> e.count;
        if (k == "line") e.kind = IrqEvent::Line, s >> e.a >> e.b;
        else if (k == "imm" || k == "pend") {
            e.kind = k == "imm" ? IrqEvent::Imm : IrqEvent::Pend;
            s >> e.a >> std::hex >> e.ip;
        } else if (k == "end") e.kind = IrqEvent::End;
        else continue;
        v.push_back(e);
    }
    return v;
}

struct TakeCheck {
    const std::vector<IrqEvent> *log;
    size_t *next;
    uint64_t *count;
    bool expecting_imm = false;
    int taken = 0;
};

void on_take(void *ctx, int vector, uint32_t ip, bool pending) {
    auto &t = *static_cast<TakeCheck *>(ctx);
    const auto &log = *t.log;
    size_t &n = *t.next;
    char buf[256];
    if (n >= log.size() || log[n].count != *t.count || log[n].a != vector || log[n].ip != ip ||
        (log[n].kind == IrqEvent::Pend) != pending || (log[n].kind == IrqEvent::Imm) != !pending) {
        std::snprintf(buf, sizeof buf,
                      "interrupt taken here that MAME did not take: %s vector %d at ip %08x, instruction %" PRIu64
                      "; MAME's next event: count %" PRIu64 " vector %d ip %08x",
                      pending ? "pending" : "immediate", vector, ip, *t.count, n < log.size() ? log[n].count : 0,
                      n < log.size() ? log[n].a : -1, n < log.size() ? log[n].ip : 0);
        throw rt::Divergence(buf);
    }
    ++n;
    ++t.taken;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: m1replay IMAGES_DIR TRACE.m2tr IRQ.log\n");
        return 2;
    }
    const std::string dir = argv[1];
    uint64_t count = 0;
    size_t next = 0;
    try {
        rt::M2ReplayBus bus(load(dir + "/program.bin"), load(dir + "/main_data.bin"), argv[2]);
        rt::I960Core core(&bus);
        bus.attach(&core);
        const std::vector<IrqEvent> log = load_irq_log(argv[3]);
        TakeCheck tc{&log, &next, &count};
        core.on_take = on_take;
        core.on_take_ctx = &tc;
        core.reset();
        // Same per-instruction log as the MAME patch's M2TRACE_PCLOG, for diffing.
        FILE *pclog = nullptr;
        uint64_t pclog_max = 200000;
        if (const char *p = std::getenv("M1REPLAY_PCLOG")) pclog = std::fopen(p, "w");
        if (const char *m = std::getenv("M1REPLAY_PCLOG_MAX")) pclog_max = std::strtoull(m, nullptr, 0);
        const char *dump_at = std::getenv("M1REPLAY_REGS_AT"); // print registers before this instruction
        const uint64_t dump_at_n = dump_at ? std::strtoull(dump_at, nullptr, 0) : 0;

        // MAME's run ends after this many completed instructions (IRQ log "end").
        uint64_t end_count = UINT64_MAX;
        for (const IrqEvent &e : log)
            if (e.kind == IrqEvent::End) end_count = e.count;

        const auto t0 = std::chrono::steady_clock::now();
        while (!bus.trace_done() && count < end_count) {
            // MAME's interrupt events that happened after `count` completed instructions.
            while (next < log.size() && log[next].count == count && log[next].kind != IrqEvent::Pend) {
                const IrqEvent &e = log[next];
                if (e.kind == IrqEvent::Line) {
                    ++next;
                    core.execute_set_input(e.a, e.b);
                } else if (e.kind == IrqEvent::Imm) {
                    const int before = tc.taken;
                    core.check_immediate_irqs(); // on_take consumes the event
                    if (tc.taken == before)
                        throw rt::Divergence("MAME took an immediate interrupt here (vector " + std::to_string(e.a) +
                                             ") but ours has none waiting, instruction " + std::to_string(count));
                } else { // End
                    ++next;
                    break;
                }
            }
            if (next < log.size() && log[next].count < count && log[next].kind == IrqEvent::Pend)
                throw rt::Divergence("MAME took a pending interrupt (vector " + std::to_string(log[next].a) +
                                     ") at instruction " + std::to_string(log[next].count) + " that ours did not");
            if (dump_at && count == dump_at_n) {
                for (int i = 0; i < 32; ++i) std::printf("%s%s=%08x", i % 8 ? " " : "\n  ", trace::reg_name(i), core.m_r[i]);
                std::printf("\n  before instruction %" PRIu64 " at %08x\n", count, core.m_IP);
            }
            core.execute_one();
            if (pclog && count < pclog_max) {
                uint64_t h = 0xcbf29ce484222325ull;
                for (int i = 0; i < 32; ++i) h = (h ^ core.m_r[i]) * 0x100000001b3ull;
                std::fprintf(pclog, "%" PRIu64 " %08x %08x %016" PRIx64 "\n", count, core.m_PIP, core.m_AC, h);
            }
            ++count;
        }
        if (!bus.all_events_consumed())
            throw rt::Divergence("MAME's run ended but the trace still has events we did not make; " + bus.where());
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("m1replay: MATCH. %" PRIu64 " instructions, %" PRIu64 " epochs, %" PRIu64
                    " device events, %d interrupts, all identical to MAME; %.1f s (%.1f M instr/s)\n",
                    count, bus.epochs_matched(), bus.events_matched(), tc.taken, s, double(count) / s / 1e6);
        return 0;
    } catch (const rt::Divergence &d) {
        std::printf("m1replay: DIVERGED after %" PRIu64 " instructions: %s\n", count, d.what());
        return 1;
    } catch (const rt::Fatal &f) {
        std::printf("m1replay: FATAL after %" PRIu64 " instructions: %s\n", count, f.what());
        return 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m1replay: %s\n", e.what());
        return 2;
    }
}
