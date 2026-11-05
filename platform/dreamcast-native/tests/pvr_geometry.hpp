// Presentation-only projection/ordering. Model 2 conventions follow the
// BSD-3-Clause CPU reference in runtime/raster.cpp; no guest state is changed.
#pragma once
#include "runtime/geo.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace daytona {
struct PvrPoint {
    float x, y, q, uq, vq; // screen coordinates, 1/z, normalized u/z and v/z
};
struct PvrPolygon {
    std::array<PvrPoint, 12> points{}; // convex octagon + four clip planes
    unsigned count = 0;
};

// Reverse of Raster's first-covered-pixel ordering: paint far/old first,
// then near/new, including window priority. Geometric 1/z stays free for UVs.
inline bool painter_before(const rt::GeoPoly *polys, size_t a, size_t b) {
    if (polys[a].window != polys[b].window) return polys[a].window < polys[b].window;
    if (polys[a].z != polys[b].z) return polys[a].z > polys[b].z;
    return a < b;
}

inline bool finite(const PvrPoint &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.q) &&
           std::isfinite(v.uq) && std::isfinite(v.vq) && v.q > 0;
}

inline PvrPolygon project_and_clip(const rt::GeoPoly &poly, int crtc_x, int crtc_y, int render_x, int render_y) {
    PvrPolygon result;
    if (poly.num_vertices < 3 || poly.num_vertices > 8) return result;
    const float bounds[4] = {
        float(std::max(int(poly.viewport[0]) + render_x, 0)),
        float(std::min(int(poly.viewport[2]) + render_x + 1, 496)),
        float(std::max(384 - int(poly.viewport[3]) + render_y, 0)),
        float(std::min(384 - int(poly.viewport[1]) + render_y + 1, 384))};
    if (bounds[0] >= bounds[1] || bounds[2] >= bounds[3]) return result;
    const float w = float(32u << (poly.texheader[0] & 7)), h = float(32u << ((poly.texheader[0] >> 3) & 7));
    unsigned outside_any = 0, outside_all = 15;
    for (unsigned i = 0; i < poly.num_vertices; ++i) {
        const auto &v = poly.v[i];
        if (!(v.p[0] > 0) || !std::isfinite(v.p[0])) return {};
        const float q = 1.0f / v.p[0];
        PvrPoint p{float(crtc_x + poly.center[0]) + v.x / v.p[0],
                   float(384 - poly.center[1] + crtc_y) - v.y / v.p[0],
                   q, v.p[1] * q / (8.0f * w), v.p[2] * q / (8.0f * h)};
        if (!finite(p)) return {};
        const unsigned outside = unsigned(p.x < bounds[0]) | (unsigned(p.x > bounds[1]) << 1) |
                                 (unsigned(p.y < bounds[2]) << 2) | (unsigned(p.y > bounds[3]) << 3);
        outside_any |= outside; outside_all &= outside;
        result.points[result.count++] = p;
    }
    if (outside_all) return {}; // all vertices beyond the same plane
    // Clip in screen space, interpolating q, uq and vq rather than raw UVs.
    // Dividing uq/vq by q is deferred until PVR vertex submission.
    for (unsigned plane = 0; plane < 4 && result.count; ++plane) {
        // Clipping cannot push vertices outside a plane they were all inside.
        if (!(outside_any & (1u << plane))) continue;
        PvrPolygon clipped;
        const auto distance = [&](const PvrPoint &v) {
            const float coordinate = plane < 2 ? v.x : v.y;
            return plane & 1 ? bounds[plane] - coordinate : coordinate - bounds[plane];
        };
        PvrPoint a = result.points[result.count - 1];
        float da = distance(a);
        for (unsigned i = 0; i < result.count; ++i) {
            const PvrPoint b = result.points[i];
            const float db = distance(b);
            if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
                const float t = da / (da - db);
                PvrPoint p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                           a.q + (b.q - a.q) * t, a.uq + (b.uq - a.uq) * t,
                           a.vq + (b.vq - a.vq) * t};
                if (!finite(p) || clipped.count == clipped.points.size()) return {};
                if (plane < 2) p.x = bounds[plane]; else p.y = bounds[plane];
                clipped.points[clipped.count++] = p;
            }
            if (db >= 0) {
                if (clipped.count == clipped.points.size()) return {};
                clipped.points[clipped.count++] = b;
            }
            a = b; da = db;
        }
        result = clipped;
    }
    if (result.count < 3) result.count = 0;
    return result;
}
} // namespace daytona
