#include "host_translucent_sort.h"

#include <algorithm>
#include <cmath>

namespace enj_host_translucent_sort {
namespace {

struct Bounds {
  float min_x;
  float min_y;
  float max_x;
  float max_y;
};

float cross_2d(float ax, float ay, float bx, float by) {
  return ax * by - ay * bx;
}

bool triangle_depth_at(const enj_host_pvr::QueuedPrimitive &primitive,
                       uint32_t ia, uint32_t ib, uint32_t ic,
                       float x, float y, float &depth) {
  const float ax = primitive.x[ia];
  const float ay = primitive.y[ia];
  const float bx = primitive.x[ib];
  const float by = primitive.y[ib];
  const float cx = primitive.x[ic];
  const float cy = primitive.y[ic];
  const float denominator = cross_2d(bx - ax, by - ay, cx - ax, cy - ay);
  if (std::abs(denominator) < 0.000001f) {
    return false;
  }

  const float wa = cross_2d(bx - x, by - y, cx - x, cy - y) / denominator;
  const float wb = cross_2d(cx - x, cy - y, ax - x, ay - y) / denominator;
  const float wc = 1.0f - wa - wb;
  constexpr float edge_epsilon = 0.00001f;
  if (wa < -edge_epsilon || wb < -edge_epsilon || wc < -edge_epsilon) {
    return false;
  }
  depth = wa * primitive.z[ia] + wb * primitive.z[ib] +
          wc * primitive.z[ic];
  return true;
}

bool primitive_depth_at(const enj_host_pvr::QueuedPrimitive &primitive,
                        float x, float y, float &depth) {
  if (primitive.count == 3u) {
    return triangle_depth_at(primitive, 0u, 1u, 2u, x, y, depth);
  }
  if (primitive.count == 4u) {
    return triangle_depth_at(primitive, 0u, 1u, 2u, x, y, depth) ||
           triangle_depth_at(primitive, 0u, 2u, 3u, x, y, depth);
  }
  return false;
}

bool segment_intersection(float ax, float ay, float bx, float by,
                          float cx, float cy, float dx, float dy,
                          float &x, float &y) {
  const float rx = bx - ax;
  const float ry = by - ay;
  const float sx = dx - cx;
  const float sy = dy - cy;
  const float denominator = cross_2d(rx, ry, sx, sy);
  if (std::abs(denominator) < 0.000001f) {
    return false;
  }
  const float qx = cx - ax;
  const float qy = cy - ay;
  const float t = cross_2d(qx, qy, sx, sy) / denominator;
  const float u = cross_2d(qx, qy, rx, ry) / denominator;
  constexpr float edge_epsilon = 0.00001f;
  if (t < -edge_epsilon || t > 1.0f + edge_epsilon ||
      u < -edge_epsilon || u > 1.0f + edge_epsilon) {
    return false;
  }
  x = ax + t * rx;
  y = ay + t * ry;
  return true;
}

}  // namespace

float primitive_average_z(const enj_host_pvr::QueuedPrimitive &primitive) {
  float z = 0.0f;
  for (uint32_t i = 0u; i < primitive.count; i++) {
    z += primitive.z[i];
  }
  return primitive.count > 0u ? z / static_cast<float>(primitive.count) : 0.0f;
}

int overlapping_depth_order(const enj_host_pvr::QueuedPrimitive &a,
                            const enj_host_pvr::QueuedPrimitive &b) {
  int relation = 0;
  const auto compare_at = [&](float x, float y) {
    float a_depth;
    float b_depth;
    if (!primitive_depth_at(a, x, y, a_depth) ||
        !primitive_depth_at(b, x, y, b_depth)) {
      return true;
    }
    const float difference = a_depth - b_depth;
    constexpr float depth_epsilon = 0.000001f;
    if (std::abs(difference) <= depth_epsilon) {
      return true;
    }
    const int sample_relation = difference < 0.0f ? -1 : 1;
    if (relation != 0 && relation != sample_relation) {
      return false;
    }
    relation = sample_relation;
    return true;
  };

  for (uint32_t i = 0u; i < a.count; i++) {
    if (!compare_at(a.x[i], a.y[i])) {
      return 0;
    }
  }
  for (uint32_t i = 0u; i < b.count; i++) {
    if (!compare_at(b.x[i], b.y[i])) {
      return 0;
    }
  }
  for (uint32_t ai = 0u; ai < a.count; ai++) {
    const uint32_t an = (ai + 1u) % a.count;
    for (uint32_t bi = 0u; bi < b.count; bi++) {
      const uint32_t bn = (bi + 1u) % b.count;
      float x;
      float y;
      if (segment_intersection(a.x[ai], a.y[ai], a.x[an], a.y[an],
                               b.x[bi], b.y[bi], b.x[bn], b.y[bn], x, y) &&
          !compare_at(x, y)) {
        return 0;
      }
    }
  }
  return relation;
}

namespace detail {

std::vector<size_t> sort_dependency_graph(
    const std::vector<float> &average_depth,
    const std::vector<std::vector<size_t>> &outgoing,
    Diagnostics &diagnostics) {
  const size_t count = average_depth.size();
  std::vector<size_t> incoming(count, 0u);
  for (const std::vector<size_t> &edges : outgoing) {
    for (size_t next : edges) {
      if (next < count) {
        incoming[next]++;
      }
    }
  }

  std::vector<size_t> order;
  order.reserve(count);
  std::vector<bool> emitted(count, false);
  for (size_t output = 0u; output < count; output++) {
    size_t selected = count;
    for (size_t i = 0u; i < count; i++) {
      if (emitted[i] || incoming[i] != 0u) {
        continue;
      }
      if (selected == count || average_depth[i] < average_depth[selected] ||
          (average_depth[i] == average_depth[selected] && i < selected)) {
        selected = i;
      }
    }
    if (selected == count) {
      diagnostics.cycle_breaks++;
      for (size_t i = 0u; i < count; i++) {
        if (!emitted[i] &&
            (selected == count || average_depth[i] < average_depth[selected] ||
             (average_depth[i] == average_depth[selected] && i < selected))) {
          selected = i;
        }
      }
    }
    order.push_back(selected);
    emitted[selected] = true;
    for (size_t next : outgoing[selected]) {
      if (next < count && !emitted[next] && incoming[next] > 0u) {
        incoming[next]--;
      }
    }
  }
  return order;
}

}  // namespace detail

Diagnostics sort(
    std::vector<const enj_host_pvr::QueuedPrimitive *> &primitives) {
  Diagnostics diagnostics{};
  const size_t count = primitives.size();
  diagnostics.primitive_count = count;
  if (count < 2u) {
    return diagnostics;
  }

  constexpr size_t dependency_sort_limit = 2048u;
  if (count > dependency_sort_limit) {
    std::stable_sort(primitives.begin(), primitives.end(),
        [](const enj_host_pvr::QueuedPrimitive *a,
           const enj_host_pvr::QueuedPrimitive *b) {
          return primitive_average_z(*a) < primitive_average_z(*b);
        });
    diagnostics.average_depth_fallback = true;
    return diagnostics;
  }

  std::vector<float> average_depth(count);
  std::vector<Bounds> bounds(count);
  for (size_t i = 0u; i < count; i++) {
    average_depth[i] = primitive_average_z(*primitives[i]);
    Bounds &box = bounds[i];
    box.min_x = box.max_x = primitives[i]->x[0];
    box.min_y = box.max_y = primitives[i]->y[0];
    for (uint32_t vertex = 1u; vertex < primitives[i]->count; vertex++) {
      box.min_x = std::min(box.min_x, primitives[i]->x[vertex]);
      box.min_y = std::min(box.min_y, primitives[i]->y[vertex]);
      box.max_x = std::max(box.max_x, primitives[i]->x[vertex]);
      box.max_y = std::max(box.max_y, primitives[i]->y[vertex]);
    }
  }

  std::vector<std::vector<size_t>> outgoing(count);
  for (size_t i = 0u; i < count; i++) {
    for (size_t j = i + 1u; j < count; j++) {
      if (bounds[i].max_x < bounds[j].min_x ||
          bounds[j].max_x < bounds[i].min_x ||
          bounds[i].max_y < bounds[j].min_y ||
          bounds[j].max_y < bounds[i].min_y) {
        continue;
      }
      diagnostics.candidate_pairs++;
      const int relation = overlapping_depth_order(*primitives[i], *primitives[j]);
      if (relation < 0) {
        outgoing[i].push_back(j);
        diagnostics.dependency_edges++;
      } else if (relation > 0) {
        outgoing[j].push_back(i);
        diagnostics.dependency_edges++;
      } else {
        diagnostics.unordered_pairs++;
      }
    }
  }

  const std::vector<const enj_host_pvr::QueuedPrimitive *> submitted = primitives;
  const std::vector<size_t> order =
      detail::sort_dependency_graph(average_depth, outgoing, diagnostics);
  for (size_t i = 0u; i < count; i++) {
    primitives[i] = submitted[order[i]];
  }
  return diagnostics;
}

}  // namespace enj_host_translucent_sort
