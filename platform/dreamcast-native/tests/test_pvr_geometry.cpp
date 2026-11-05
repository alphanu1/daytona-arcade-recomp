#include "pvr_geometry.hpp"
#undef NDEBUG
#include <cassert>
#include <limits>
#include <numeric>
#include <cstdio>

int main() {
    rt::GeoPoly poly{};
    poly.num_vertices = 4; poly.texheader[0] = 0x4000;
    poly.center[1] = 384;
    poly.viewport[0] = 20; poly.viewport[2] = 79;
    poly.viewport[1] = 305; poly.viewport[3] = 364; // [20,80) on both axes
    poly.v[0] = {-10, 20, {1, 0, 0}};
    poly.v[1] = {220, 40, {2, 256, 0}};
    poly.v[2] = {220, -200, {2, 256, 256}};
    poly.v[3] = {-10, -100, {1, 0, 256}};
    const auto original = poly;
    // Fully inside (including boundary vertices) must preserve every point.
    poly.viewport[0] = 0; poly.viewport[2] = 109;
    poly.viewport[1] = 284; poly.viewport[3] = 384;
    poly.v[0].x = 0; poly.v[3].x = 0;
    poly.v[0].y = 0; poly.v[1].y = 0;
    auto inside = daytona::project_and_clip(poly, 0, 0, 0, 0);
    assert(inside.count == 4);
    for (unsigned i = 0; i < inside.count; ++i) {
        assert(inside.points[i].x == poly.v[i].x / poly.v[i].p[0]);
        assert(inside.points[i].y == -poly.v[i].y / poly.v[i].p[0]);
    }
    poly = original;
    auto out = daytona::project_and_clip(poly, 0, 0, 0, 0);
    assert(out.count == 4);
    for (unsigned i = 0; i < out.count; ++i) {
        const auto &v = out.points[i];
        assert(v.x >= 20 && v.x <= 80 && v.y >= 20 && v.y <= 80);
        const float t = (v.x + 10) / 120;
        assert(std::abs(v.q - (1 - 0.5f * t)) < 1e-6f);
        assert(std::abs(v.uq - 0.5f * t) < 1e-6f);
        assert(std::abs(v.uq / v.q - t) > 0.05f); // perspective is not affine
    }
    assert(poly.v[1].x == original.v[1].x && poly.v[1].p[1] == original.v[1].p[1]);
    for (float z : {0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        poly.v[0].p[0] = z; assert(!daytona::project_and_clip(poly, 0, 0, 0, 0).count);
    }
    poly = original;
    poly.viewport[0] = 400; poly.viewport[2] = 495;
    assert(!daytona::project_and_clip(poly, 0, 0, 0, 0).count);
    poly = original;
    // CRTC projection and raster viewport offsets are distinct.
    out = daytona::project_and_clip(poly, 10, 10, 10, 10);
    assert(out.count == 4);
    for (unsigned i = 0; i < out.count; ++i)
        assert(out.points[i].x >= 30 && out.points[i].x <= 90 && out.points[i].y >= 30 && out.points[i].y <= 90);
    for (int offset = -1000; offset <= 1000; ++offset) {
        out = daytona::project_and_clip(poly, offset, -offset, 0, 0);
        assert(out.count <= 12);
        for (unsigned i = 0; i < out.count; ++i)
            assert(daytona::finite(out.points[i]) && out.points[i].x >= 20 && out.points[i].x <= 80 &&
                   out.points[i].y >= 20 && out.points[i].y <= 80);
    }
    poly.num_vertices = 8;
    for (unsigned i = 0; i < 8; ++i) {
        const float angle = float(i) * 0.7853981634f;
        const float x = 248 + 230 * std::cos(angle), y = 192 + 190 * std::sin(angle);
        poly.v[i] = {x, -y, {1, x * 8, y * 8}};
    }
    for (int inset = 0; inset < 190; ++inset) {
        poly.viewport[0] = int16_t(inset); poly.viewport[2] = int16_t(495 - inset);
        poly.viewport[1] = int16_t(inset); poly.viewport[3] = int16_t(384 - inset);
        out = daytona::project_and_clip(poly, 0, 0, 0, 0);
        assert(out.count >= 3 && out.count <= 12);
        for (unsigned i = 0; i < out.count; ++i) {
            const auto &v = out.points[i];
            assert(v.x >= inset && v.x <= 496 - inset && v.y >= inset && v.y <= 384 - inset + 1);
            assert(std::abs(v.uq / v.q - v.x / 32) < 1e-5f);
        }
    }
    std::array<rt::GeoPoly, 4> polys{};
    polys[0].z = 1; polys[1].z = 3; polys[2].z = 3; polys[3].window = 1;
    std::array<size_t, 4> order{0, 1, 2, 3};
    std::sort(order.begin(), order.end(), [&](auto a, auto b) { return daytona::painter_before(polys.data(), a, b); });
    assert((order == std::array<size_t, 4>{1, 2, 0, 3}));
    std::puts("PASS: perspective interpolation, viewport clipping, finite bounds and reverse reference ordering");
}
