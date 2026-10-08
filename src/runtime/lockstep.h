// Lockstep interrupt replay (design doc, M1 plan; UART option (a)): applies
// MAME's interrupt events at the same completed-instruction counts, and
// checks every interrupt the core takes against MAME's log. Shared by the
// interpreter harness and recompiled code, so both use one definition.
#pragma once

#include "runtime/cpu.h"
#include "runtime/m2_replay_bus.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rt {

class Lockstep {
public:
    // Loads MAME's IRQ log (M2TRACE_IRQLOG) and hooks the core's take callback.
    Lockstep(Cpu &core, const std::string &irq_log_path);
    // Free run (the game on its own, no MAME): no log; the board schedules
    // its events with add_callback, and interrupts the board raises are taken
    // at the next instruction boundary (poke()).
    explicit Lockstep(Cpu &core);

    uint64_t count = 0;            // completed instructions so far
    uint64_t end_count = UINT64_MAX; // MAME's run ends here
    uint64_t next_count = UINT64_MAX; // count of the next event to apply

    // Call before each instruction. Returns true when the run is over or an
    // interrupt was taken (IP changed); the caller re-dispatches on m_IP.
    bool boundary() {
#ifdef M2_FAST_GEN
        // next_count is kept at or below end_count (refresh_next, set_end):
        // one compare (this is called before every instruction).
        if (count < next_count) return false;
#else
        if (count < next_count && count < end_count) return false;
#endif
        return apply();
    }
#ifdef M2_FAST_GEN
    // Set end_count (the game loop's frame end), keeping next_count at or
    // below it. A poke not yet taken is taken at the next boundary, as it
    // would have been: its next_count was at or below the count.
    void set_end(uint64_t end) {
        end_count = end;
        refresh_next();
        if (poked_) next_count = std::min(next_count, count);
    }
#endif
    bool finished() const { return count >= end_count; }
    // Run fn when `at` instructions have completed, before any interrupt
    // event at the same count (MAME's vblank handler parses the display list
    // before it raises the vblank line). Lockstep: call before the run
    // starts. Free run: callbacks may add further callbacks.
    void add_callback(uint64_t at, std::function<void()> fn);
    // Free run: an interrupt line changed; take it at the next boundary.
#ifdef M2_FAST_GEN
    // The rewritten generated code keeps the instructions it has run since
    // it last added them to count in a register; before calling the runtime
    // it stores them here (one store, not a 64-bit add), and adds them to
    // count itself at its rechecks and dispatches. What the runtime reads
    // during an instruction is now(), not count.
    uint32_t pending = 0;
    uint64_t now() const { return count + pending; }
    void poke() { next_count = std::min(next_count, now() + 1); poked_ = true; ++epoch; }
    // The Dreamcast's rewritten generated code (platform/dreamcast/scripts/
    // fast_gen.py) counts down the instructions it may run before the next
    // event instead of calling boundary() before each one. check() is its
    // full boundary at `ip`: 0 to re-dispatch, else how many instructions,
    // this one included, until the next event. epoch moves whenever
    // next_count or count is changed other than by one instruction (an
    // instruction that saw it move checks in full at the next).
    uint32_t check(uint32_t ip);
#else
    void poke() { next_count = std::min(next_count, count + 1); poked_ = true; }
#endif
    int interrupts() const { return taken_; }

private:
    struct Event {
        enum Kind { Call, Line, Imm, Pend, End } kind;
        uint64_t count;
        int a = 0, b = 0;
        uint32_t ip = 0;
        size_t fn = 0; // Call: index into calls_
    };
    std::vector<std::function<void()>> calls_;
    bool apply();
    void refresh_next();
    static void on_take(void *ctx, int vector, uint32_t ip, bool pending);

    Cpu &core_;
    std::vector<Event> log_;
    size_t next_ = 0;
    bool free_run_ = false, poked_ = false;
    int taken_ = 0;
#if defined(M2_DC_MEMORY) || defined(M2_FAST_GEN)
    // Slots of calls_ already called, for reuse: free run adds a callback
    // every 1024 instructions and calls_ never shrinks (16 bytes each on the
    // SH-4: 1 MB by frame 220 of the attract mode). Last, so the members the
    // generated code's inline boundary() and poke() use keep their offsets.
    std::vector<size_t> free_calls_;
#endif
#ifdef M2_FAST_GEN
public:
    uint32_t epoch = 0; // (last, like free_calls_)
#endif
};

} // namespace rt
