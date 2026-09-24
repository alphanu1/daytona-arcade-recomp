// tracediff: compare two M2TR traces epoch by epoch and stop at the first
// divergence (docs/trace-format.md).
//
//   tracediff [options] A.m2tr B.m2tr
//
// Options:
//   --compare-ip       also compare ip (post-increment in MAME samples)
//   --compare-frame    also compare MAME screen frame numbers
//   --skip-reg NAME    ignore one register (repeatable), e.g. --skip-reg tc
//   --context N        events of context to print before an event divergence (default 8)
//
// Exit status: 0 identical, 1 divergent, 2 unreadable input.

#include "trace/trace.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void usage() {
    std::fprintf(stderr, "usage: tracediff [--compare-ip] [--compare-frame] [--skip-reg NAME]... "
                         "[--context N] A.m2tr B.m2tr\n");
    std::exit(2);
}

void print_access(const char *side, size_t i, const trace::Access &a) {
    std::printf("  %s[%zu] %s %08x = %08x (mask %08x)\n", side, i, a.write ? "W" : "R", a.addr, a.data, a.mask);
}

} // namespace

int main(int argc, char **argv) {
    trace::CompareOptions opt;
    size_t context = 8;
    const char *paths[2] = {nullptr, nullptr};
    int np = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--compare-ip") opt.compare_ip = true;
        else if (a == "--compare-frame") opt.compare_frame = true;
        else if (a == "--skip-reg" && i + 1 < argc) {
            const std::string name = argv[++i];
            int found = -1;
            for (int r = 0; r < trace::kNumRegs; ++r)
                if (name == trace::reg_name(r)) found = r;
            if (found < 0) {
                std::fprintf(stderr, "tracediff: unknown register %s\n", name.c_str());
                return 2;
            }
            opt.skip_regs |= uint64_t(1) << found;
        } else if (a == "--context" && i + 1 < argc) context = std::strtoul(argv[++i], nullptr, 0);
        else if (a[0] != '-' && np < 2) paths[np++] = argv[i];
        else usage();
    }
    if (np != 2) usage();

    trace::Reader ra(paths[0]), rb(paths[1]);
    for (const trace::Reader *r : {&ra, &rb}) {
        if (!r->error().empty()) {
            std::fprintf(stderr, "tracediff: %s\n", r->error().c_str());
            return 2;
        }
    }
    std::printf("A: %s (%s, trigger %s)\nB: %s (%s, trigger %s)\n", paths[0], ra.header().producer.c_str(),
                ra.header().trigger.c_str(), paths[1], rb.header().producer.c_str(), rb.header().trigger.c_str());
    if (ra.header().game != rb.header().game)
        std::printf("warning: game differs: %s vs %s\n", ra.header().game.c_str(), rb.header().game.c_str());
    if (ra.header().trigger != rb.header().trigger)
        std::printf("warning: sample triggers differ; epochs will not line up\n");

    uint64_t events = 0;
    for (uint64_t ei = 0;; ++ei) {
        trace::Epoch ea, eb;
        const bool ha = ra.next(ea), hb = rb.next(eb);
        for (const trace::Reader *r : {&ra, &rb}) {
            if (!r->error().empty()) {
                std::fprintf(stderr, "tracediff: epoch %llu: %s\n", (unsigned long long)ei, r->error().c_str());
                return 2;
            }
        }
        if (!ha && !hb) {
            std::printf("identical: %llu epochs, %llu events\n", (unsigned long long)ei, (unsigned long long)events);
            return 0;
        }
        for (const auto &n : ea.notes) std::printf("A note @%llu: %s\n", (unsigned long long)ei, n.c_str());
        for (const auto &n : eb.notes) std::printf("B note @%llu: %s\n", (unsigned long long)ei, n.c_str());

        if (ha != hb) {
            std::printf("DIVERGED at epoch %llu: trace %s ends here\n", (unsigned long long)ei, ha ? "B" : "A");
            return 1;
        }
        const trace::Divergence d = trace::compare(ea, eb, ei, opt);
        if (d.kind != trace::Divergence::None) {
            std::printf("DIVERGED at epoch %llu (frame A %llu): %s\n", (unsigned long long)ei,
                        (unsigned long long)(ea.sample ? ea.sample->frame : 0), d.detail.c_str());
            if (d.kind == trace::Divergence::Event || d.kind == trace::Divergence::EventCount) {
                const size_t from = d.index > context ? d.index - context : 0;
                std::printf("context (A):\n");
                for (size_t i = from; i < d.index && i < ea.events.size(); ++i) print_access("A", i, ea.events[i]);
            }
            return 1;
        }
        events += ea.events.size();
    }
}
