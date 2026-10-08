// Headless investigation controls, separate from the launcher's enhancements.
#pragma once


namespace rt {
struct EnhanceDiagnostics {
    // With positive draw distance: use its ordering but retain only cells
    // in the game's original list, to distinguish membership from ordering.
    static inline bool draw_order_only = false;
};
} // namespace rt
