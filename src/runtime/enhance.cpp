#include "runtime/enhance.h"
#include "runtime/enhance_diagnostics.h"
#include "runtime/scenery.h"

#include <algorithm>
#include <cstdlib>

namespace rt {

// The draw list holds at most 63 cells (0x5016c1..0x5016ff; the game uses
// 0x501700), so the widest square is 7x7 (49).
// The game's polygon budget per frame: objects stop being drawn once their
// costs pass it; the original allowance is set once at boot. Costs come
// from object-list metadata before clipping,
// not visible polygons; a later list is rejected after prior lists exceed it.
constexpr uint32_t kBudgetAddr = 0x5010f4, kGameBudget = 5000;

void hook_draw_list(Cpu &c) {
    const int level = std::clamp(Enhance::draw_distance, Enhance::kDrawMin, Enhance::kDrawMax);
    auto *state = c.bus->scenery();
    static bool legacy_changed = false; // non-board callers of the old distance hook
    bool &changed = state ? state->budget_changed : legacy_changed;
    const uint32_t budget = state ? state->budget(level) : automatic_scenery_budget(496, level);
    const bool wide = state && state->width > 496 && !state->original_selection;
    if (budget != kGameBudget || (state && state->custom_budget) || level > 0 || changed) {
        c.bus->write_dword(kBudgetAddr, budget);
        changed = budget != kGameBudget;
    }
    if (state && state->diagnostics) {
        ++state->measured.list_updates;
        state->measured.original_cells = c.bus->read_byte(0x5016c0);
        state->measured.selected_cells = state->measured.original_cells;
        state->measured.exclusion_mask = c.m_r[13];
        state->measured.direction_mask = c.m_r[9];
    }
    if (level == 0 && !wide) return;
    const uint32_t car = c.m_r[8];
    if (car > 255) return; // not a cell: leave the game's list alone
    const int cx = int(car & 15), cy = int(car >> 4);
    uint8_t original[63];
    int original_count = 0;
    if (EnhanceDiagnostics::draw_order_only && level > 0) {
        original_count = std::min<int>(c.bus->read_byte(0x5016c0), 63);
        for (int i = 0; i < original_count; ++i)
            original[i] = c.bus->read_byte(0x5016c1 + uint32_t(i));
    }
    uint8_t list[63];
    int n = 0;
    if (level <= 0) {
        // Less: the game's own list, cut to the car's cell (-2) or one cell around it (-1).
        const int keep = level == -2 ? 0 : level == -1 ? 1 : 2;
        const int count = std::min<int>(c.bus->read_byte(0x5016c0), 63);
        for (int i = 0; i < count; i++) {
            const int cell = c.bus->read_byte(0x5016c1 + uint32_t(i));
            const int dx = (cell & 15) - cx, dy = (cell >> 4) - cy;
            if (std::max(std::abs(dx), std::abs(dy)) <= keep) list[n++] = uint8_t(cell);
        }
        if (wide) {
            // r13 excludes invalid grid cells and course-specific hidden areas;
            // r9 is the original directional mask. Keep the former and supply
            // all remaining candidates in range to the widened geometrizer.
            // This conservative superset works for every camera direction.
            // Preserve the original prefix/order; append missing cells by ring.
            for (int radius = 0; radius <= keep; ++radius)
                for (int dy = -radius; dy <= radius; ++dy)
                    for (int dx = -radius; dx <= radius; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                        const int x = cx + dx, y = cy + dy;
                        if (x < 0 || x > 15 || y < 0 || y > 15) continue;
                        const unsigned slot = unsigned((dy + 2) * 5 + dx + 2);
                        if (c.m_r[13] & (1u << slot)) continue;
                        const auto cell = uint8_t(x + 16 * y);
                        if (std::find(list, list + n, cell) == list + n && n < 63) list[n++] = cell;
                    }
        }
    } else {
        // More: every cell within 2 (+1: the whole 5x5 the game chooses from)
        // or 3 (+2: 7x7) of the car's, nearest ring first, inside the
        // 16x16 grid (cells are bytes; no wrapping into the next row).
        const int radius = 1 + level;
        for (int r = 0; r <= radius; r++)
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    const int x = cx + dx, y = cy + dy;
                    if (x < 0 || x > 15 || y < 0 || y > 15) continue;
                    list[n++] = uint8_t(x + 16 * y);
                }
    }
    if (EnhanceDiagnostics::draw_order_only && level > 0) {
        int kept = 0;
        for (int i = 0; i < n; ++i)
            if (std::find(original, original + original_count, list[i]) != original + original_count)
                list[kept++] = list[i];
        n = kept;
    }
    c.bus->write_byte(0x5016c0, uint8_t(n));
    for (int i = 0; i < n; i++) c.bus->write_byte(0x5016c1 + uint32_t(i), list[i]);
    if (state && state->diagnostics) state->measured.selected_cells = uint32_t(n);
}

void hook_scenery_cost(Cpu &c) {
    auto *state = c.bus->scenery();
    if (state && state->diagnostics)
        state->measured.cost_peak = std::max(state->measured.cost_peak, c.m_r[4]);
}

void hook_scenery_budget_check(Cpu &c) {
    auto *state = c.bus->scenery();
    if (!state || !state->diagnostics) return;
    ++state->measured.budget_checks;
    state->measured.cost_peak = std::max(state->measured.cost_peak, c.m_r[5]);
    if (c.m_r[5] > c.m_r[4]) ++state->measured.budget_rejections;
}

} // namespace rt
