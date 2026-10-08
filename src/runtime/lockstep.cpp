#include "runtime/lockstep.h"

#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace rt {

Lockstep::Lockstep(Cpu &core, const std::string &path) : core_(core) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream s(line);
        std::string k;
        Event e{};
        s >> k >> e.count;
        if (k == "line") e.kind = Event::Line, s >> e.a >> e.b;
        else if (k == "imm" || k == "pend") {
            e.kind = k == "imm" ? Event::Imm : Event::Pend;
            s >> e.a >> std::hex >> e.ip;
        } else if (k == "end") {
            e.kind = Event::End;
            end_count = e.count;
        } else continue;
        log_.push_back(e);
    }
    core_.on_take = on_take;
    core_.on_take_ctx = this;
    refresh_next();
}

// The next count at which boundary() must act: an event to apply, or a
// pending-table take MAME made that ours must have made by then.
Lockstep::Lockstep(Cpu &core) : core_(core), free_run_(true) {
    core_.on_take = on_take;
    core_.on_take_ctx = this;
    refresh_next();
}

void Lockstep::refresh_next() {
    next_count = next_ < log_.size() ? log_[next_].count : UINT64_MAX;
    if (next_ < log_.size() && log_[next_].kind == Event::Pend) next_count += 1; // checked after it should happen
#ifdef M2_FAST_GEN
    next_count = std::min(next_count, end_count); // boundary() compares next_count only
    ++epoch;
#endif
}

#ifdef M2_FAST_GEN
uint32_t Lockstep::check(uint32_t ip) {
    core_.m_IP = ip;
    if (boundary()) return 0;
    if (next_count <= count) return 1; // check again at the next instruction
    const uint64_t left = next_count - count;
    return left > 0x40000000u ? 0x40000000u : uint32_t(left);
}
#endif

bool Lockstep::apply() {
    if (count >= end_count) return true;
    const uint32_t ip_before = core_.m_IP;
    while (next_ < log_.size() && log_[next_].count == count && log_[next_].kind != Event::Pend) {
        const Event &e = log_[next_];
        if (e.kind == Event::Call) {
            const size_t fn = e.fn; // the callback may add events (e moves)
            ++next_;
#if defined(M2_DC_MEMORY) || defined(M2_FAST_GEN)
            // Each callback runs once: take it out (it may add the next one,
            // which can reuse its slot) and free the slot.
            auto call = std::move(calls_[fn]);
            calls_[fn] = nullptr;
            free_calls_.push_back(fn);
            call();
#else
            calls_[fn]();
#endif
        } else if (e.kind == Event::Line) {
            ++next_;
            core_.execute_set_input(e.a, e.b);
        } else if (e.kind == Event::Imm) {
            const int before = taken_;
            core_.check_immediate_irqs(); // on_take consumes the event
            if (taken_ == before)
                throw Divergence("MAME took an immediate interrupt here (vector " + std::to_string(e.a) +
                                 ") but ours has none waiting, instruction " + std::to_string(count));
        } else {
            ++next_; // End
        }
    }
    if (free_run_) {
        poked_ = false;
        if (core_.m_immediate_irq) core_.check_immediate_irqs();
        if (next_ > 4096) { // drop consumed events
            log_.erase(log_.begin(), log_.begin() + long(next_));
            next_ = 0;
        }
        refresh_next();
        return core_.m_IP != ip_before;
    }
    if (next_ < log_.size() && log_[next_].kind == Event::Pend && log_[next_].count < count)
        throw Divergence("MAME took a pending interrupt (vector " + std::to_string(log_[next_].a) + ") at instruction " +
                         std::to_string(log_[next_].count) + " that ours did not");
    refresh_next();
    return core_.m_IP != ip_before;
}

void Lockstep::add_callback(uint64_t at, std::function<void()> fn) {
    Event e{};
    e.kind = Event::Call;
    e.count = at;
#if defined(M2_DC_MEMORY) || defined(M2_FAST_GEN)
    if (!free_calls_.empty()) {
        e.fn = free_calls_.back();
        free_calls_.pop_back();
        calls_[e.fn] = std::move(fn);
    } else {
        e.fn = calls_.size();
        calls_.push_back(std::move(fn));
    }
#else
    e.fn = calls_.size();
    calls_.push_back(std::move(fn));
#endif
    // Before the first event at the same count or later (the log is in count order).
    auto it = log_.begin() + long(next_);
    while (it != log_.end() && it->count < at) ++it;
    log_.insert(it, e);
    refresh_next();
}

void Lockstep::on_take(void *ctx, int vector, uint32_t ip, bool pending) {
    auto &t = *static_cast<Lockstep *>(ctx);
    if (t.free_run_) {
        ++t.taken_;
        return;
    }
    const auto &log = t.log_;
    const size_t n = t.next_;
    if (n >= log.size() || log[n].count != t.count || log[n].a != vector || log[n].ip != ip ||
        (log[n].kind == Event::Pend) != pending || (log[n].kind == Event::Imm) != !pending) {
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "interrupt taken here that MAME did not take: %s vector %d at ip %08x, instruction %" PRIu64
                      "; MAME's next event: count %" PRIu64 " vector %d ip %08x",
                      pending ? "pending" : "immediate", vector, ip, t.count, n < log.size() ? log[n].count : 0,
                      n < log.size() ? log[n].a : -1, n < log.size() ? log[n].ip : 0);
        throw Divergence(buf);
    }
    ++t.next_;
    ++t.taken_;
    t.refresh_next();
}

} // namespace rt
