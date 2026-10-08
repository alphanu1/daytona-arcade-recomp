// Scenery enhancement policy and opt-in measurements. No ROM-derived data.
#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace rt {

inline constexpr uint32_t kSceneryBudget = 5000;
// A supported setting bound, not a promise of renderer capacity. The game
// uses unsigned 32-bit costs and comparisons; keep ample overflow headroom.
inline constexpr uint32_t kMaxSceneryBudget = 1000000;

inline bool parse_scenery_budget(std::string_view text, uint32_t &value) {
    if (text.empty()) return false;
    uint32_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed > kMaxSceneryBudget)
        return false;
    value = parsed;
    return true;
}

inline uint32_t automatic_scenery_budget(int width, int distance) {
    const auto scaled = (uint64_t(kSceneryBudget) * uint64_t(std::max(width, 496)) + 495) / 496;
    const auto distance_budget = kSceneryBudget * uint32_t(1 + std::clamp(distance, 0, 2));
    return uint32_t(std::min<uint64_t>(kMaxSceneryBudget, std::max<uint64_t>(scaled, distance_budget)));
}

struct SceneryMeasurements {
    uint32_t list_updates = 0, original_cells = 0, selected_cells = 0;
    uint32_t exclusion_mask = 0, direction_mask = 0;
    uint32_t cost_peak = 0, budget_checks = 0, budget_rejections = 0;
};

// Owned by a board: changing one cabinet's aspect/budget must not affect another.
struct SceneryState {
    int width = 496;
    uint32_t custom_budget = 0; // zero = Automatic
    bool budget_changed = false;
    bool diagnostics = false;
    bool original_selection = false; // headless comparison control only
    SceneryMeasurements measured;

    uint32_t budget(int distance) const {
        return custom_budget ? custom_budget : automatic_scenery_budget(width, distance);
    }
};

} // namespace rt
