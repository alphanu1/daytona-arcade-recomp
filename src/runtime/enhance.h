// Enhancements that act inside the recompiled game code, through hooks the
// recompiler places (m2recomp --hooks, seeds/daytona93_hooks.txt): just before
// the instruction at a hook's address, the generated code calls
// rt::hook_<name>(c). All off by default; while off a hook returns at once,
// so the game runs exactly as recompiled (rules.md: parity runs that way).
#pragma once

#include "runtime/cpu.h"

namespace rt {

struct Enhance {
    // Draw distance (see hook_draw_list): 0 = the game's own; -1 one cell
    // around the car, -2 the car's cell only; +1 the whole 5x5, +2 7x7.
    static constexpr int kDrawMin = -2, kDrawMax = 2;
    static inline int draw_distance = 0;
};

// 0x17078 (daytona93; 0x17508 in Revision A, the same code and RAM), after
// the game has built its draw list of course cells: the count
// at 0x5016c0 and the cell numbers from 0x5016c1, chosen from the 5x5 cells
// around the car's (r8) in a grid 16 cells wide (cell = x + 16 y).
void hook_draw_list(Cpu &c);
// Diagnostic observers at the game's cost updates and budget comparisons.
// Never alter registers, RAM, conditions or control flow.
void hook_scenery_cost(Cpu &c);
void hook_scenery_budget_check(Cpu &c);

} // namespace rt
