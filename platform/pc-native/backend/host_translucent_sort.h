#ifndef ENJ_HOST_TRANSLUCENT_SORT_H
#define ENJ_HOST_TRANSLUCENT_SORT_H

#include "host_pvr.h"

#include <cstddef>
#include <vector>

namespace enj_host_translucent_sort {

struct Diagnostics {
  size_t primitive_count{};
  size_t candidate_pairs{};
  size_t dependency_edges{};
  size_t unordered_pairs{};
  size_t cycle_breaks{};
  bool average_depth_fallback{};
};

float primitive_average_z(const enj_host_pvr::QueuedPrimitive &primitive);

/* -1 means a is farther, 1 means b is farther, and 0 means that the pair
 * does not overlap or cannot be represented by one global depth order. */
int overlapping_depth_order(const enj_host_pvr::QueuedPrimitive &a,
                            const enj_host_pvr::QueuedPrimitive &b);

Diagnostics sort(
    std::vector<const enj_host_pvr::QueuedPrimitive *> &primitives);

namespace detail {

/* Exposed for deterministic graph/cycle tests; production callers should use
 * sort(), which constructs this dependency graph from projected geometry. */
std::vector<size_t> sort_dependency_graph(
    const std::vector<float> &average_depth,
    const std::vector<std::vector<size_t>> &outgoing,
    Diagnostics &diagnostics);

}  // namespace detail
}  // namespace enj_host_translucent_sort

#endif
