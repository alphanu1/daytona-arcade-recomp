// traceinfo: summarise an M2TR trace (docs/trace-format.md).
//
//   traceinfo trace.m2tr
//
// Prints the header, epoch and frame counts (epochs per frame shows whether
// the sample trigger fires once per frame), event counts per tapped range,
// and the first epoch each range is touched. Counts and addresses only.

#include "trace/trace.h"

#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Range {
    uint32_t lo, hi;
    const char *name;
    uint64_t writes = 0, reads = 0;
    int64_t first_epoch = -1;
};

std::vector<Range> ranges() {
    return {
        {0x00800000, 0x00803fff, "geo registers"},
        {0x00804000, 0x00807fff, "geo program upload"},
        {0x00880000, 0x00883fff, "TGP function port"},
        {0x00884000, 0x00887fff, "TGP FIFO"},
        {0x00980000, 0x00980003, "copro control"},
        {0x00980004, 0x0098000b, "FIFO status / geo ctl"},
        {0x0098000c, 0x0098000f, "video control"},
        {0x00e80000, 0x00e80003, "IRQ request/ack"},
        {0x00e80004, 0x00e80007, "IRQ enable"},
        {0x00f00000, 0x00f0000f, "timers"},
        {0x01c00000, 0x01c00fff, "I/O dual-port RAM"},
        {0x01c80000, 0x01c80003, "sound UART"},
    };
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: traceinfo trace.m2tr\n");
        return 2;
    }
    trace::Reader r(argv[1]);
    if (!r.error().empty()) {
        std::fprintf(stderr, "traceinfo: %s\n", r.error().c_str());
        return 2;
    }
    std::printf("game %s, producer %s, trigger %s\n", r.header().game.c_str(), r.header().producer.c_str(),
                r.header().trigger.c_str());

    std::vector<Range> rs = ranges();
    uint64_t epochs = 0, events = 0, other = 0, open_events = 0;
    uint64_t first_frame = 0, last_frame = 0, max_events = 0;
    uint32_t irq_enable_last = 0;
    bool have_frame = false, irq_enable_seen = false;
    trace::Epoch e;
    for (uint64_t ei = 0; r.next(e); ++ei) {
        for (const std::string &n : e.notes) std::printf("note @%" PRIu64 ": %s\n", ei, n.c_str());
        for (const trace::Access &a : e.events) {
            bool hit = false;
            for (Range &g : rs) {
                if (a.addr < g.lo || a.addr > g.hi) continue;
                (a.write ? g.writes : g.reads)++;
                if (g.first_epoch < 0) g.first_epoch = int64_t(ei);
                hit = true;
            }
            if (!hit) ++other;
            if (a.write && a.addr == 0x00e80004) {
                irq_enable_last = (irq_enable_last & ~a.mask) | (a.data & a.mask);
                irq_enable_seen = true;
            }
        }
        events += e.events.size();
        if (e.events.size() > max_events) max_events = e.events.size();
        if (!e.sample) {
            open_events = e.events.size();
            break;
        }
        ++epochs;
        if (!have_frame) first_frame = e.sample->frame;
        last_frame = e.sample->frame;
        have_frame = true;
    }
    if (!r.error().empty()) std::printf("warning: %s (summary covers what was read)\n", r.error().c_str());

    const uint64_t frames = have_frame ? last_frame - first_frame + 1 : 0;
    std::printf("epochs %" PRIu64 " over frames %" PRIu64 "-%" PRIu64 " (%" PRIu64 " frames): %.3f epochs/frame\n",
                epochs, first_frame, last_frame, frames, frames ? double(epochs) / double(frames) : 0.0);
    std::printf("events %" PRIu64 " (max %" PRIu64 " in one epoch, %" PRIu64 " after the last sample)\n", events,
                max_events, open_events);
    std::printf("%-24s %12s %12s  %s\n", "range", "writes", "reads", "first epoch");
    for (const Range &g : rs)
        std::printf("%-24s %12" PRIu64 " %12" PRIu64 "  %s\n", g.name, g.writes, g.reads,
                    g.first_epoch < 0 ? "never" : std::to_string(g.first_epoch).c_str());
    if (other) std::printf("%-24s %12" PRIu64 "\n", "(outside named ranges)", other);
    if (irq_enable_seen) std::printf("IRQ enable register at end: %08x\n", irq_enable_last);
    return 0;
}
