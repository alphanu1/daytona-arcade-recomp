#include "reach.h"

#include <algorithm>
#include <deque>
#include <set>

namespace i960 {

ReachResult reach(const std::vector<uint32_t> &seeds, const ReadWord &read) {
    ReachResult r;
    std::set<uint32_t> indirect, stops, unmapped;
    std::deque<uint32_t> work(seeds.begin(), seeds.end());

    while (!work.empty()) {
        uint32_t pc = work.front() & ~3u;
        work.pop_front();
        // Walk a straight line until control leaves it.
        while (!r.insns.count(pc)) {
            const auto w0 = read(pc);
            if (!w0) {
                unmapped.insert(pc);
                break;
            }
            const Insn in = decode(pc, *w0, read(pc + 4).value_or(0));
            if (!in.executable()) {
                stops.insert(pc);
                break;
            }
            r.insns.emplace(pc, in);
            const Flow f = in.flow();
            if (in.has_target) work.push_back(in.target);
            if (f == Flow::BranchInd || f == Flow::BalInd || f == Flow::CallInd || f == Flow::CallSys)
                indirect.insert(pc);
            // Paths that do not fall through.
            if (f == Flow::Branch || f == Flow::Ret || f == Flow::BranchInd) break;
            pc += in.length;
        }
    }
    r.indirect_sites.assign(indirect.begin(), indirect.end());
    r.stops.assign(stops.begin(), stops.end());
    r.unmapped_targets.assign(unmapped.begin(), unmapped.end());
    return r;
}

std::vector<uint32_t> BootSeeds::all() const {
    std::vector<uint32_t> v{reset_ip};
    v.insert(v.end(), interrupt_handlers.begin(), interrupt_handlers.end());
    v.insert(v.end(), system_procedures.begin(), system_procedures.end());
    return v;
}

BootSeeds boot_seeds(const ReadWord &read, unsigned max_sysprocs) {
    BootSeeds s;
    auto mapped = [&](uint32_t a) { return read(a & ~3u).has_value(); };
    s.reset_ip = read(12).value_or(0);

    std::set<uint32_t> irq, sys;
    if (const auto prcb = read(4)) {
        if (const auto tab = read(*prcb + 20)) {
            for (uint32_t v = 8; v < 256; ++v) {
                const auto h = read(*tab + 36 + (v - 8) * 4);
                if (h && *h && mapped(*h)) irq.insert(*h); // 0: unused slot
            }
        }
    }
    if (const auto sat = read(0)) {
        if (const auto tab = read(*sat + 152)) {
            for (uint32_t i = 0; i < max_sysprocs; ++i) {
                const auto p = read(*tab + 48 + i * 4);
                if (p && (*p & ~3u) && mapped(*p & ~3u)) sys.insert(*p & ~3u); // 0: unused slot
            }
        }
    }
    s.interrupt_handlers.assign(irq.begin(), irq.end());
    s.system_procedures.assign(sys.begin(), sys.end());
    return s;
}

} // namespace i960
