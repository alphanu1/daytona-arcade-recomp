// Lockstep interrupt replay (design doc, M1 plan; UART option (a)): applies
// MAME's interrupt events at the same completed-instruction counts, and
// checks every interrupt the core takes against MAME's log. Shared by the
// interpreter harness and recompiled code, so both use one definition.
#pragma once

#include "runtime/i960_core.h"
#include "runtime/m2_replay_bus.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rt {

class Lockstep {
public:
    // Loads MAME's IRQ log (M2TRACE_IRQLOG) and hooks the core's take callback.
    Lockstep(I960Core &core, const std::string &irq_log_path);

    uint64_t count = 0;            // completed instructions so far
    uint64_t end_count = UINT64_MAX; // MAME's run ends here
    uint64_t next_count = UINT64_MAX; // count of the next event to apply

    // Call before each instruction. Returns true when the run is over or an
    // interrupt was taken (IP changed); the caller re-dispatches on m_IP.
    bool boundary() {
        if (count < next_count && count < end_count) return false;
        return apply();
    }
    bool finished() const { return count >= end_count; }
    int interrupts() const { return taken_; }

private:
    struct Event {
        enum Kind { Line, Imm, Pend, End } kind;
        uint64_t count;
        int a = 0, b = 0;
        uint32_t ip = 0;
    };
    bool apply();
    void refresh_next();
    static void on_take(void *ctx, int vector, uint32_t ip, bool pending);

    I960Core &core_;
    std::vector<Event> log_;
    size_t next_ = 0;
    int taken_ = 0;
};

} // namespace rt
