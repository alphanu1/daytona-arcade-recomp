// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels, Ville Linde, Aaron Giles
//
// Model 2 3D layer, CPU reference: MAME's model2_3d_project,
// render_polygons, model2_3d_render, model2rd.ipp and the poly.h triangle
// and polygon setup, transplanted (see raster.h). Changes: polygons are drawn
// immediately (MAME's work queue keeps the same per-pixel order); float to
// int conversions that can overflow or see a NaN are pinned to the x86 result
// MAME's reference build gives, so every host agrees.

#include "runtime/raster.h"
#ifdef M2_VITA_RENDER_OPT
#include "runtime/raster_texel.h"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#ifdef M2_RASTER_BANDS
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#endif
#include "compat/bit_cast.h"
namespace rt {

namespace {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using s32 = int32_t;

inline u32 f2u(float f) { return compat::bit_cast<u32>(f); }
inline u32 rgb(u32 r, u32 g, u32 b) { return 0xff000000u | (r << 16) | (g << 8) | b; } // rgb_t(r, g, b)

// x86 cvttss2si: NaN and out of range give INT32_MIN.
inline s32 to_s32(float f) {
    if (!(f >= -2147483648.0f && f < 2147483648.0f)) return std::numeric_limits<s32>::min();
    return s32(f);
}

// poly.h round_coordinate (float), with NaN pinned as above.
inline s32 round_coordinate(float value) {
    if (std::isnan(value)) return std::numeric_limits<s32>::min();
    if (value >= float(std::numeric_limits<s32>::max())) return std::numeric_limits<s32>::max();
    const float ipart = std::floor(value);
    if (ipart < float(std::numeric_limits<s32>::min())) return std::numeric_limits<s32>::min();
    const float fpart = value - ipart;
    return s32(ipart) + ((fpart > 0.5f) ? 1 : 0);
}

constexpr u32 LERP(u32 x, u32 y, unsigned a) { return (x + (((y - x) * a) >> 8)) & 0x00ff00ff; }

inline s32 fast_log2(float value) {
    if (value < 0.0F) return 0;
    u32 ival = f2u(value) >> 16;
    s32 exp = s32(ival >> 7) - 127;
    static u8 const s_log2_table[128] = {
        0,   2,   5,   8,   11,  14,  16,  19,  22,  25,  27,  30,  33,  35,  38,  40,  43,  46,  48,  51,  53,  56,
        58,  61,  63,  65,  68,  70,  73,  75,  77,  80,  82,  84,  87,  89,  91,  93,  96,  98,  100, 102, 104, 106,
        109, 111, 113, 115, 117, 119, 121, 123, 125, 127, 129, 132, 134, 136, 138, 140, 141, 143, 145, 147, 149, 151,
        153, 155, 157, 159, 161, 162, 164, 166, 168, 170, 172, 173, 175, 177, 179, 181, 182, 184, 186, 188, 189, 191,
        193, 194, 196, 198, 200, 201, 203, 205, 206, 208, 209, 211, 213, 214, 216, 218, 219, 221, 222, 224, 225, 227,
        229, 230, 232, 233, 235, 236, 238, 239, 241, 242, 244, 245, 247, 248, 250, 251, 253, 254};
    return (exp << 8) | s_log2_table[ival & 127];
}

#ifndef M2_VITA_RENDER_OPT
inline u16 get_texel(u32 base_x, u32 base_y, int x, int y, const u32 *sheet) {
    int x2 = int(base_x) + x;
    int y2 = int(base_y) + y;
    if (x2 >= 1024) {
        // texture sheets are mapped as 2048x1024 but stored in RAM as 1024x2048
        x2 -= 1024;
        y2 ^= 1024;
    }
    u32 offset = u32(((y2 / 2) * 512) + (x2 / 2));
    u32 texel = sheet[(offset >> 1) & 0x7ffff];
    if (offset & 1) texel >>= 16;
    if ((y & 1) == 0) texel >>= 8;
    if ((x & 1) == 0) texel >>= 4;
    return texel & 0x0f;
}

#endif

inline u16 le16(const uint8_t *base, u32 index) { return u16(base[index * 2] | base[index * 2 + 1] << 8); }

} // namespace

struct Raster::Extra {
    u8 checker = 0;
    u32 lumabase = 0, colorbase = 0;
    const u32 *texsheet[2] = {nullptr, nullptr};
    u32 texwidth = 0, texheight = 0, texx = 0, texy = 0;
    u8 texwrapx = 0, texwrapy = 0, texmirrorx = 0, texmirrory = 0, utex = 0, utexminlod = 0;
    u32 utexx = 0, utexy = 0;
    s32 texlod = 0;
    u8 luma = 0;
#ifdef M2_VITA_RENDER_OPT
    struct Level {
        u32 width = 0, height = 0, x = 0, y = 0;
        const u32 *sheet = nullptr;
    };
    Level levels[13]; // 0 = microtexture, 1..12 = mip levels 0..11
    s32 max_level = 0;
    mutable const u32 *shades = nullptr;
#endif
};

struct Raster::Prepared {
    GeoPoly poly; // projected; texture parameters divided by z
    Extra extra;
    int renderer = 0;
    int clip[4] = {0, 0, 0, 0}; // left, right, top, bottom (inclusive)
    int32_t miny = 0, maxy = 0;  // scanlines the polygon can touch: [miny, maxy)
};

#ifdef M2_RASTER_BANDS
// Persistent workers; the calling thread takes tasks too. run() returns when
// every task is done, so nothing outlives the job it was given.
struct Raster::Pool {
    explicit Pool(int workers) {
        for (int i = 0; i < workers; i++) threads.emplace_back([this] { work(); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lock(m);
            stop = true;
        }
        wake.notify_all();
        for (std::thread &t : threads) t.join();
    }
    // Hand `count` tasks to the workers and return; `fn` must live until
    // finish() returns.
    void start(int count, const std::function<void(int)> &fn) {
        {
            std::lock_guard<std::mutex> lock(m);
            job = &fn;
            tasks = count;
            next.store(0, std::memory_order_relaxed);
            active = int(threads.size());
            ++generation;
        }
        wake.notify_all();
    }
    // Take the tasks still waiting, then wait for every worker's.
    void finish() {
        if (!job) return;
        take(*job, tasks);
        std::unique_lock<std::mutex> lock(m);
        done.wait(lock, [this] { return active == 0; });
        job = nullptr;
    }

private:
    void take(const std::function<void(int)> &fn, int count) {
        for (int i; (i = next.fetch_add(1, std::memory_order_relaxed)) < count;) fn(i);
    }
    void work() {
        uint64_t seen = 0;
        for (;;) {
            const std::function<void(int)> *fn;
            int count;
            {
                std::unique_lock<std::mutex> lock(m);
                wake.wait(lock, [&] { return stop || generation != seen; });
                if (stop) return;
                seen = generation;
                fn = job;
                count = tasks;
            }
            take(*fn, count);
            std::lock_guard<std::mutex> lock(m);
            if (--active == 0) done.notify_one();
        }
    }
    std::vector<std::thread> threads;
    std::mutex m;
    std::condition_variable wake, done;
    const std::function<void(int)> *job = nullptr;
    int tasks = 0, active = 0;
    uint64_t generation = 0;
    bool stop = false;
    std::atomic<int> next{0};
};
#else
struct Raster::Pool {};
#endif

#ifdef M2_DC_MEMORY
// The Dreamcast: the CPU rasterizer's buffers (1.25 MB) only once it renders
// (the PVR draws the 3D there).
Raster::Raster() {
#else
Raster::Raster() : dest_(512 * 512), fill_(512 * 512) {
#endif
    // MAME video_start
    for (int i = 0; i < 256; i++) {
        double raw_value = std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0);
        gamma_[i] = u8(raw_value);
    }
}

Raster::~Raster() { finish(); } // no band may still be drawing into members being destroyed

int Raster::threads() const {
#ifdef M2_RASTER_BANDS
    if (threads_ > 0) return std::min(threads_, 64);
    if (const char *env = std::getenv("M2_RASTER_THREADS")) {
        const int n = std::atoi(env);
        if (n > 0) return std::min(n, 64);
    }
    const unsigned hw = std::thread::hardware_concurrency();
    return int(std::clamp(hw ? hw : 1u, 1u, 8u));
#else
    return 1;
#endif
}

void Raster::set_wide_margin(int margin) {
    margin_ = std::max(margin, 0);
    const int stride = std::max(512, 496 + 2 * margin_);
    if (stride != stride_) {
        stride_ = stride;
        dest_.assign(size_t(stride_) * 512, 0u);
        fill_.assign(size_t(stride_) * 512, u8(0));
    }
}

uint64_t Raster::hash(int minx, int maxx, int miny, int maxy) const {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            const u32 px = dest_[size_t(y) * size_t(stride_) + size_t(x)];
            for (int b = 0; b < 4; b++) {
                h ^= (px >> (8 * b)) & 0xff;
                h *= 0x100000001b3ULL;
            }
        }
    return h;
}

void Raster::render(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem, int crtc_x, int crtc_y,
                    int render_x, int render_y, int clip_minx, int clip_maxx, int clip_miny, int clip_maxy) {
    render_start(polys, windows, mem, crtc_x, crtc_y, render_x, render_y, clip_minx, clip_maxx, clip_miny, clip_maxy);
    finish();
}

void Raster::finish() {
#ifdef M2_RASTER_BANDS
    if (pool_) pool_->finish();
#endif
}

void Raster::render_start(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem, int crtc_x, int crtc_y,
                          int render_x, int render_y, int clip_minx, int clip_maxx, int clip_miny, int clip_maxy) {
    finish(); // a frame still drawing (never expected: callers finish first)
    mem_ = &mem;
#ifdef M2_DC_MEMORY
    if (dest_.empty()) {
        dest_.assign(size_t(stride_) * 512, 0u);
        fill_.assign(size_t(stride_) * 512, u8(0));
    }
#endif
#ifdef M2_VITA_RENDER_OPT
    for (auto &entry : shades_) entry.key = 0xffffffffu;
#endif
    std::fill(dest_.begin(), dest_.end(), 0u);
    std::fill(fill_.begin(), fill_.end(), u8(0));
    // MAME: for window = cur_window..0, for z = min_z..max_z, each bucket
    // newest first.
#ifdef M2_VITA_RENDER_OPT
    order_.resize(polys.size());
    auto &order = order_;
#else
    std::vector<size_t> order(polys.size());
#endif
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (polys[a].window != polys[b].window) return polys[a].window > polys[b].window;
        if (polys[a].z != polys[b].z) return polys[a].z < polys[b].z;
        return a > b;
    });
#ifdef M2_RASTER_BANDS
    const int nthreads = threads();
    if (nthreads > 1 && clip_maxy >= clip_miny) {
        // Prepare every polygon once, in drawing order; then bands of rows.
        prepared_.resize(order.size());
        size_t count = 0;
        for (size_t i : order)
            if (polys[i].window <= windows &&
                prepare_one(polys[i], crtc_x, crtc_y, render_x, render_y, clip_minx, clip_maxx, clip_miny, clip_maxy,
                            prepared_[count]))
                ++count;
        if (!pool_ || pool_threads_ != nthreads) {
            pool_.reset();
            pool_ = std::make_unique<Pool>(nthreads - 1);
            pool_threads_ = nthreads;
        }
        // Several bands per thread, so a band full of scenery does not hold
        // up the rest; each band costs a pass over the prepared list.
        // Captured by value: the job runs after render_start returns (until
        // finish). It reads only prepared_, mem_'s memories and gamma_.
        const int rows = clip_maxy - clip_miny + 1;
        const int bands = std::min(rows, nthreads * 3);
        band_job_ = [this, rows, bands, count, clip_miny](int b) {
            const int lo = clip_miny + rows * b / bands;
            const int hi = clip_miny + rows * (b + 1) / bands - 1; // inclusive
            for (size_t k = 0; k < count; ++k) {
                const Prepared &p = prepared_[k];
                if (p.maxy <= lo || p.miny > hi) continue;
                draw_prepared(p, lo, hi);
            }
        };
        pool_->start(bands, band_job_);
        return;
    }
#endif
    for (size_t i : order)
        if (polys[i].window <= windows) render_one(polys[i], crtc_x, crtc_y, render_x, render_y, clip_minx, clip_maxx, clip_miny, clip_maxy);
}

int Raster::coverage_estimate(const std::vector<GeoPoly> &polys, int windows, int crtc_x, int crtc_y) const {
    constexpr int kCell = 8, CW = 496 / kCell, CH = 384 / kCell;
    std::array<uint8_t, size_t(CW) * CH> cells{};
    for (const GeoPoly &poly : polys) {
        if (poly.window > windows || poly.num_vertices < 3) continue;
        float px[8], py[8];
        for (int i = 0; i < poly.num_vertices; i++) { // as model2_3d_project, in 496-wide coordinates
            const GeoVertex &v = poly.v[i];
            const float z = v.p[0] + std::numeric_limits<float>::min();
            px[i] = float(crtc_x + poly.center[0]) + v.x / z - float(margin_);
            py[i] = float((384 - poly.center[1]) + crtc_y) - v.y / z;
        }
        for (int t = 1; t + 1 < poly.num_vertices; t++) { // fan; a cell counts when its centre is inside
            const float ax = px[0], ay = py[0], bx = px[t], by = py[t], cx = px[t + 1], cy = py[t + 1];
            const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            if (!(std::fabs(area) > 0.0f)) continue;
            const int x0 = std::max(0, int(std::floor(std::min({ax, bx, cx}) / kCell)));
            const int x1 = std::min(CW - 1, int(std::floor(std::max({ax, bx, cx}) / kCell)));
            const int y0 = std::max(0, int(std::floor(std::min({ay, by, cy}) / kCell)));
            const int y1 = std::min(CH - 1, int(std::floor(std::max({ay, by, cy}) / kCell)));
            for (int gy = y0; gy <= y1; gy++)
                for (int gx = x0; gx <= x1; gx++) {
                    const float qx = (float(gx) + 0.5f) * kCell, qy = (float(gy) + 0.5f) * kCell;
                    const float w0 = (bx - ax) * (qy - ay) - (by - ay) * (qx - ax);
                    const float w1 = (cx - bx) * (qy - by) - (cy - by) * (qx - bx);
                    const float w2 = (ax - cx) * (qy - cy) - (ay - cy) * (qx - cx);
                    if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
                        cells[size_t(gy) * CW + size_t(gx)] = 1;
                }
        }
    }
    int covered = 0;
    for (uint8_t c : cells) covered += c;
    return covered * 100 / (CW * CH);
}

bool Raster::find_race_hud(const std::vector<GeoPoly> &polys, int crtc_x, int crtc_y) {
    for (const GeoPoly &poly : polys) {
        if (poly.z > kHudOverlayZ || poly.texheader[0] != 0x8000 || poly.num_vertices < 3) continue;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int i = 0; i < poly.num_vertices; i++) { // as model2_3d_project
            const GeoVertex &v = poly.v[i];
            const float z = v.p[0] + std::numeric_limits<float>::min();
            const float x = float(crtc_x + poly.center[0]) + v.x / z - float(margin_);
            const float y = float((384 - poly.center[1]) + crtc_y) - v.y / z;
            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
        }
        if (x0 >= 375 && x1 <= 472 && y0 >= 57 && y1 <= 159 && x1 - x0 >= 60 && y1 - y0 >= 60) {
            hud_box_[0] = x0, hud_box_[1] = x1, hud_box_[2] = y0, hud_box_[3] = y1;
            hud_z_ = poly.z;
            return true;
        }
    }
    return false;
}

int Raster::hud_polygon_offset(const GeoPoly& poly) const {
    if (poly.num_vertices && hud_dx_ && poly.z == hud_z_) {
        float x0 = poly.v[0].x, x1 = x0, y0 = poly.v[0].y, y1 = y0;
        for (int i = 1; i < poly.num_vertices; i++) {
            x0 = std::min(x0, poly.v[i].x), x1 = std::max(x1, poly.v[i].x);
            y0 = std::min(y0, poly.v[i].y), y1 = std::max(y1, poly.v[i].y);
        }
        constexpr float tolerance = 1.5f;
        if (x0 >= hud_box_[0] + margin_ - tolerance && x1 <= hud_box_[1] + margin_ + tolerance &&
            y0 >= hud_box_[2] - tolerance && y1 <= hud_box_[3] + tolerance) return hud_dx_;
    }
    return 0;
}

void Raster::render_one(const GeoPoly &poly, int crtc_x, int crtc_y, int render_x, int render_y, int clip_minx,
                        int clip_maxx, int clip_miny, int clip_maxy) {
    Prepared p;
    if (prepare_one(poly, crtc_x, crtc_y, render_x, render_y, clip_minx, clip_maxx, clip_miny, clip_maxy, p))
        draw_prepared(p, p.clip[2], p.clip[3]);
}

// Draw a prepared polygon's scanlines in [band_miny, band_maxy] (inclusive).
// Clipping to a band only narrows the scanline loop: each scanline's values
// are computed from the polygon setup, never from the scanline before.
void Raster::draw_prepared(const Prepared &p, int band_miny, int band_maxy) {
    int clip[4] = {p.clip[0], p.clip[1], std::max(p.clip[2], band_miny), std::min(p.clip[3], band_maxy)};
    if (clip[3] < clip[2]) return;
    const GeoPoly &poly = p.poly;
    switch (poly.num_vertices) {
    case 3: render_triangle(clip, p.renderer, p.extra, poly.v[0], poly.v[1], poly.v[2]); break;
    case 4: render_polygon<4>(clip, p.renderer, p.extra, poly.v); break;
    case 5: render_polygon<5>(clip, p.renderer, p.extra, poly.v); break;
    case 6: render_polygon<6>(clip, p.renderer, p.extra, poly.v); break;
    case 7: render_polygon<7>(clip, p.renderer, p.extra, poly.v); break;
    case 8: render_polygon<8>(clip, p.renderer, p.extra, poly.v); break;
    default: break;
    }
}

bool Raster::prepare_one(const GeoPoly &source, int crtc_x, int crtc_y, int render_x, int render_y, int clip_minx,
                         int clip_maxx, int clip_miny, int clip_maxy, Prepared &out) const {
    // Built in place in `out` (one copy of the polygon, as the serial path
    // always made: render_one took it by value).
    out.poly = source;
    GeoPoly &poly = out.poly;
    // Widescreen: a viewport spanning the screen extends into the side margins.
    const int wide = margin_ && poly.viewport[0] <= 0 && poly.viewport[2] >= 495 ? margin_ : 0;
    // model2_3d_project
    for (int i = 0; i < poly.num_vertices; i++) {
        GeoVertex &v = poly.v[i];
        v.x = float(crtc_x + poly.center[0]) + (v.x / (v.p[0] + std::numeric_limits<float>::min()));
        v.y = float((384 - poly.center[1]) + crtc_y) - (v.y / (v.p[0] + std::numeric_limits<float>::min()));
    }

    // Widescreen, HUD at the edges: the condition panel's own overlay quads
    // (its z, inside its box) move with the HUD; nothing else does.
    if (hud_dx_ && poly.z == hud_z_) {
        float x0 = poly.v[0].x, x1 = x0, y0 = poly.v[0].y, y1 = y0;
        for (int i = 1; i < poly.num_vertices; i++) {
            x0 = std::min(x0, poly.v[i].x), x1 = std::max(x1, poly.v[i].x);
            y0 = std::min(y0, poly.v[i].y), y1 = std::max(y1, poly.v[i].y);
        }
        const float sx = float(margin_); // projected x includes the margin
        constexpr float kTol = 1.5f;
        if (x0 - sx >= hud_box_[0] - kTol && x1 - sx <= hud_box_[1] + kTol && y0 >= hud_box_[2] - kTol &&
            y1 <= hud_box_[3] + kTol) {
            for (int i = 0; i < poly.num_vertices; i++) poly.v[i].x += float(hud_dx_);
            render_x += hud_dx_;
        }
    }
    // model2_3d_render
    out.extra = Extra{};
    Extra &extra = out.extra;
    const int renderer = (poly.texheader[0] >> 13) & 3;
    // rectangle(minx, maxx, miny, maxy) &= cliprect, in the renderer's offsets
    int clip[4] = {std::max(poly.viewport[0] - wide + render_x, clip_minx),
                   std::min(poly.viewport[2] + wide + render_x, clip_maxx),
                   std::max((384 - poly.viewport[3]) + render_y, clip_miny),
                   std::min((384 - poly.viewport[1]) + render_y, clip_maxy)};

    extra.checker = (poly.texheader[0] >> 15) & 1;
    extra.lumabase = u32(poly.texheader[1] & 0xff) << 7;
    extra.colorbase = (poly.texheader[3] >> 6) & 0x3ff;
    extra.luma = poly.luma;
    extra.texlod = poly.texlod;

    if (renderer & 2) {
        extra.texmirrorx = (poly.texheader[0] >> 8) & 1;
        extra.texmirrory = (poly.texheader[0] >> 9) & 1;
        extra.texwrapx = (poly.texheader[0] >> 6) & 1 & ~extra.texmirrorx;
        extra.texwrapy = (poly.texheader[0] >> 7) & 1 & ~extra.texmirrory;
        extra.texsheet[0] = (poly.texheader[2] & 0x1000) ? mem_->tex1 : mem_->tex0;
        extra.texsheet[1] = (poly.texheader[2] & 0x1000) ? mem_->tex0 : mem_->tex1;
        extra.texwidth = 32u << ((poly.texheader[0] >> 0) & 0x7);
        extra.texheight = 32u << ((poly.texheader[0] >> 3) & 0x7);
        extra.texx = 32u * ((poly.texheader[2] >> 0) & 0x3f);
        extra.texy = 32u * ((poly.texheader[2] >> 6) & 0x1f);
        extra.utex = (poly.texheader[0] >> 12) & 1;
        extra.utexminlod = (poly.texheader[0] >> 10) & 3;
        extra.utexx = ((poly.texheader[2] >> 13) & 1) * 128;
        extra.utexy = ((poly.texheader[2] >> 14) & 3) * 128;
#ifdef M2_VITA_RENDER_OPT
        extra.max_level = 30 - std::countl_zero(std::min(extra.texwidth, extra.texheight));
        extra.levels[0] = {128, 128, extra.utexx, extra.utexy, extra.texsheet[1]};
        for (int level = 0; level <= extra.max_level; ++level)
            extra.levels[level + 1] = {extra.texwidth >> level, extra.texheight >> level,
                ((extra.texx - 2048) >> level) & 2047, ((extra.texy - 1024) >> level) & 1023,
                extra.texsheet[level & 1]};
#endif
        for (int i = 0; i < poly.num_vertices; i++) {
            GeoVertex &v = poly.v[i];
            v.p[0] = 1.0f / (v.p[0] + std::numeric_limits<float>::min());
            v.p[1] = v.p[1] * v.p[0] * (1.0f / 8.0f);
            v.p[2] = v.p[2] * v.p[0] * (1.0f / 8.0f);
        }
    }

    if (poly.num_vertices < 3 || poly.num_vertices > 8) return false;
    // Rows the polygon can touch, for skipping it in other bands (the draw
    // itself clips exactly; round_coordinate is monotonic, NaN -> INT32_MIN).
    int32_t miny = std::numeric_limits<int32_t>::max(), maxy = std::numeric_limits<int32_t>::min();
    bool nan = false;
    for (int i = 0; i < poly.num_vertices; i++) {
        if (std::isnan(poly.v[i].y)) nan = true;
        const int32_t r = round_coordinate(poly.v[i].y);
        miny = std::min(miny, r), maxy = std::max(maxy, r);
    }
    if (nan) miny = clip[2], maxy = clip[3] + 1; // let the draw decide, as the serial path does
    out.miny = std::max(miny, clip[2]);
    out.maxy = std::min(maxy, clip[3] + 1);
    if (out.maxy - out.miny <= 0) return false;
    out.renderer = renderer;
    std::copy_n(clip, 4, out.clip);
    return true;
}

#ifdef M2_VITA_RENDER_OPT
const uint32_t *Raster::shade_table(const Extra &o) {
    const u32 color = le16(mem_->palram, o.colorbase + 0x1000) & 0x7fff;
    const u32 key = color | (u32(o.luma) << 15) | ((o.lumabase >> 7) << 23);
    auto &entry = shades_[(key * 2654435761u) >> 26];
    if (entry.key != key) {
        const u32 cr = ((color >> 0) & 0x1f) << 8;
        const u32 cg = 0x4000 / 2 + (((color >> 5) & 0x1f) << 8);
        const u32 cb = 0x8000 / 2 + (((color >> 10) & 0x1f) << 8);
        for (u32 t = 0; t < 128; ++t) {
            u8 luma = u8(u32(mem_->lumaram[(o.lumabase + t) * 4]) * o.luma / 256);
            luma = std::min(luma, u8(0x3f));
            entry.colors[t] = rgb(gamma_[le16(mem_->colorxlat, cr + luma) & 0xff],
                gamma_[le16(mem_->colorxlat, cg + luma) & 0xff],
                gamma_[le16(mem_->colorxlat, cb + luma) & 0xff]);
        }
        entry.key = key;
    }
    return entry.colors.data();
}
#endif

void Raster::scanline(int renderer, int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o) {
#ifdef M2_VITA_RENDER_OPT
    if (x0 >= x1) return;
    // A completely filled span cannot contribute any pixel. Test four bytes
    // at a time with memcpy (valid for unaligned pointers); partial spans keep
    // the original per-pixel FP additions, including additions over occlusion.
    const u8 *filled = fill_.data() + size_t(y) * size_t(stride_) + size_t(x0);
    int remaining = x1 - x0;
    bool covered = true;
    while (remaining >= 4) {
        u32 word; std::memcpy(&word, filled, sizeof word);
        if (word != 0xffffffffu) { covered = false; break; }
        filled += 4; remaining -= 4;
    }
    if (covered) {
        while (remaining-- > 0) if (*filled++ != 0xff) { covered = false; break; }
        if (covered) return;
    }
#endif
    switch (renderer) {
    case 0: draw_scanline_solid<false>(y, x0, x1, start, dpdx, o); break;
    case 1: draw_scanline_solid<true>(y, x0, x1, start, dpdx, o); break;
    case 2: draw_scanline_tex<false>(y, x0, x1, start, dpdx, o); break;
    default: draw_scanline_tex<true>(y, x0, x1, start, dpdx, o); break;
    }
}

// poly.h render_triangle<3>, clip = {left, right, top, bottom} inclusive.
void Raster::render_triangle(const int *clip, int renderer, const Extra &o, const GeoVertex &_v1, const GeoVertex &_v2,
                             const GeoVertex &_v3) {
    const GeoVertex *v1 = &_v1, *v2 = &_v2, *v3 = &_v3;
    if (v2->y < v1->y) std::swap(v1, v2);
    if (v3->y < v2->y) {
        std::swap(v2, v3);
        if (v2->y < v1->y) std::swap(v1, v2);
    }
    const int32_t v1y = round_coordinate(v1->y);
    const int32_t v3y = round_coordinate(v3->y);
    const int32_t v1yclip = std::max(v1y, clip[2]);
    const int32_t v3yclip = std::min(v3y, clip[3] + 1);
    if (v3yclip - v1yclip <= 0) return;

    const float dxdy_v1v2 = (v2->y == v1->y) ? 0.0f : (v2->x - v1->x) / (v2->y - v1->y);
    const float dxdy_v1v3 = (v3->y == v1->y) ? 0.0f : (v3->x - v1->x) / (v3->y - v1->y);
    const float dxdy_v2v3 = (v3->y == v2->y) ? 0.0f : (v3->x - v2->x) / (v3->y - v2->y);

    std::array<float, 3> param_start, param_dpdx, param_dpdy;
    const float a00 = v2->y - v3->y, a01 = v3->x - v2->x, a02 = v2->x * v3->y - v3->x * v2->y;
    const float a10 = v3->y - v1->y, a11 = v1->x - v3->x, a12 = v3->x * v1->y - v1->x * v3->y;
    const float a20 = v1->y - v2->y, a21 = v2->x - v1->x, a22 = v1->x * v2->y - v2->x * v1->y;
    const float det = a02 + a12 + a22;
    if (std::abs(det) < 0.00001f) {
        for (int p = 0; p < 3; p++) param_dpdx[p] = 0.0f, param_dpdy[p] = 0.0f, param_start[p] = v1->p[p];
    } else {
        const float idet = 1.0f / det;
        for (int p = 0; p < 3; p++) {
            param_dpdx[p] = idet * (v1->p[p] * a00 + v2->p[p] * a10 + v3->p[p] * a20);
            param_dpdy[p] = idet * (v1->p[p] * a01 + v2->p[p] * a11 + v3->p[p] * a21);
            param_start[p] = idet * (v1->p[p] * a02 + v2->p[p] * a12 + v3->p[p] * a22);
        }
    }

    for (int32_t curscan = v1yclip; curscan < v3yclip; curscan++) {
        const float fully = float(curscan) + 0.5f;
        const float startx = v1->x + (fully - v1->y) * dxdy_v1v3;
        const float stopx = fully < v2->y ? v1->x + (fully - v1->y) * dxdy_v1v2 : v2->x + (fully - v2->y) * dxdy_v2v3;
        int32_t istartx = round_coordinate(startx), istopx = round_coordinate(stopx);
        if (istartx > istopx) std::swap(istartx, istopx);
        istartx = std::max(istartx, clip[0]);
        istopx = std::min(istopx, clip[1] + 1);
        if (istartx >= istopx) istartx = istopx = 0;
        const float fullstartx = float(istartx) + 0.5f;
        float start[3], dpdx[3];
        for (int p = 0; p < 3; p++) {
            start[p] = param_start[p] + fullstartx * param_dpdx[p] + fully * param_dpdy[p];
            dpdx[p] = param_dpdx[p];
        }
        scanline(renderer, curscan, istartx, istopx, start, dpdx, o);
    }
}

// poly.h render_polygon<NumVerts, 3>
template <int NumVerts>
void Raster::render_polygon(const int *clip, int renderer, const Extra &o, const GeoVertex *v) {
    int minv = 0, maxv = 0;
    for (int vertnum = 1; vertnum < NumVerts; vertnum++) {
        if (v[vertnum].y < v[minv].y) minv = vertnum;
        else if (v[vertnum].y > v[maxv].y) maxv = vertnum;
    }
    const int32_t miny = round_coordinate(v[minv].y);
    const int32_t maxy = round_coordinate(v[maxv].y);
    const int32_t minyclip = std::max(miny, clip[2]);
    const int32_t maxyclip = std::min(maxy, clip[3] + 1);
    if (maxyclip - minyclip <= 0) return;

    struct poly_edge {
        const GeoVertex *v1 = nullptr, *v2 = nullptr;
        float dxdy = 0;
        std::array<float, 3> dpdy{};
    };
    poly_edge fedgelist[NumVerts - 1], bedgelist[NumVerts - 1];
    poly_edge *edgeptr = &fedgelist[0];
    for (int curv = minv; curv != maxv; curv = (curv == NumVerts - 1) ? 0 : (curv + 1)) {
        edgeptr->v1 = &v[curv];
        edgeptr->v2 = &v[(curv == NumVerts - 1) ? 0 : (curv + 1)];
        if (edgeptr->v1->y == edgeptr->v2->y) continue;
        const float ooy = 1.0f / (edgeptr->v2->y - edgeptr->v1->y);
        edgeptr->dxdy = (edgeptr->v2->x - edgeptr->v1->x) * ooy;
        for (int p = 0; p < 3; p++) edgeptr->dpdy[p] = (edgeptr->v2->p[p] - edgeptr->v1->p[p]) * ooy;
        ++edgeptr;
    }
    edgeptr = &bedgelist[0];
    for (int curv = minv; curv != maxv; curv = (curv == 0) ? (NumVerts - 1) : (curv - 1)) {
        edgeptr->v1 = &v[curv];
        edgeptr->v2 = &v[(curv == 0) ? (NumVerts - 1) : (curv - 1)];
        if (edgeptr->v1->y == edgeptr->v2->y) continue;
        const float ooy = 1.0f / (edgeptr->v2->y - edgeptr->v1->y);
        edgeptr->dxdy = (edgeptr->v2->x - edgeptr->v1->x) * ooy;
        for (int p = 0; p < 3; p++) edgeptr->dpdy[p] = (edgeptr->v2->p[p] - edgeptr->v1->p[p]) * ooy;
        ++edgeptr;
    }

    const poly_edge *ledge, *redge;
    if ((fedgelist[0].v1 == bedgelist[0].v1 && fedgelist[0].dxdy < bedgelist[0].dxdy) ||
        (fedgelist[0].v1 != bedgelist[0].v1 && fedgelist[0].v1->x < bedgelist[0].v1->x)) {
        ledge = fedgelist;
        redge = bedgelist;
    } else {
        ledge = bedgelist;
        redge = fedgelist;
    }

    for (int32_t curscan = minyclip; curscan < maxyclip; curscan++) {
        const float fully = float(curscan) + 0.5f;
        while (fully > ledge->v2->y && fully < v[maxv].y) ++ledge;
        while (fully > redge->v2->y && fully < v[maxv].y) ++redge;
        const float startx = ledge->v1->x + (fully - ledge->v1->y) * ledge->dxdy;
        const float stopx = redge->v1->x + (fully - redge->v1->y) * redge->dxdy;
        int32_t istartx = round_coordinate(startx), istopx = round_coordinate(stopx);
        if (istartx > istopx) std::swap(istartx, istopx);
        istartx = std::max(istartx, clip[0]);
        istopx = std::min(istopx, clip[1] + 1);
        float start[3], dpdx[3];
        const float ldy = fully - ledge->v1->y;
        const float rdy = fully - redge->v1->y;
        const float oox = 1.0f / (stopx - startx);
        for (int p = 0; p < 3; p++) {
            const float lparam = ledge->v1->p[p] + ldy * ledge->dpdy[p];
            const float rparam = redge->v1->p[p] + rdy * redge->dpdy[p];
            const float d = (rparam - lparam) * oox;
            start[p] = lparam + (float(istartx) + 0.5f - startx) * d;
            dpdx[p] = d;
        }
        if (istartx >= istopx) istartx = istopx = 0;
        scanline(renderer, curscan, istartx, istopx, start, dpdx, o);
    }
}

template <bool Translucent>
void Raster::draw_scanline_solid(int32_t y, int32_t x0, int32_t x1, const float *, const float *, const Extra &o) {
    if (Translucent) return; // nothing to render
    u32 *const p = &dest_[size_t(y) * size_t(stride_)];
    u8 *const fill = &fill_[size_t(y) * size_t(stride_)];
    const u8 luma = o.luma >> 2;
    const u32 color = le16(mem_->palram, o.colorbase + 0x1000) & 0xffff;
    const u32 tr = gamma_[le16(mem_->colorxlat, 0x0000 / 2 + (((color >> 0) & 0x1f) << 8) + luma) & 0xff];
    const u32 tg = gamma_[le16(mem_->colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff];
    const u32 tb = gamma_[le16(mem_->colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff];
    const u32 c = rgb(tr, tg, tb);
    int x = x0;
    const int dx = o.checker ? 2 : 1;
    if (o.checker && !((x ^ y) & 1)) x++;
    for (; x < x1; x += dx)
        if (fill[x] == 0) p[x] = c, fill[x] = 0xff;
}

template <bool Translucent>
uint32_t Raster::fetch_bilinear_texel(const Extra &o, int32_t miplevel, int32_t u, int32_t v) const {
#ifdef M2_VITA_RENDER_OPT
    const auto &level = o.levels[miplevel + 1];
    const u32 tex_width = level.width, tex_height = level.height, tex_x = level.x, tex_y = level.y;
    const u32 *sheet = level.sheet;
    if (miplevel == -1) {
        u <<= 1 << o.utexminlod;
        v <<= 1 << o.utexminlod;
    } else {
        u >>= miplevel;
        v >>= miplevel;
    }
#else
    u32 tex_width, tex_height, tex_x, tex_y;
    const u32 *sheet;
    if (miplevel == -1) { // microtexture
        tex_width = 128;
        tex_height = 128;
        tex_x = o.utexx;
        tex_y = o.utexy;
        sheet = o.texsheet[1];
        u <<= 1 << o.utexminlod;
        v <<= 1 << o.utexminlod;
    } else {
        tex_width = o.texwidth >> miplevel;
        tex_height = o.texheight >> miplevel;
        tex_x = ((o.texx - 2048) >> miplevel) & 2047;
        tex_y = ((o.texy - 1024) >> miplevel) & 1023;
        sheet = o.texsheet[miplevel & 1];
        u >>= miplevel;
        v >>= miplevel;
    }
#endif
    if (o.texmirrorx && (u & s32(tex_width << 8))) u = ~u;
    if (o.texmirrory && (v & s32(tex_height << 8))) v = ~v;
    u -= 0x80;
    v -= 0x80;
    u32 ufrac = u32(u) & 0xff;
    u32 vfrac = u32(v) & 0xff;
    u32 u0 = u32(u >> 8) & (tex_width - 1);
    u32 u1 = (u0 + 1) & (tex_width - 1);
    u32 v0 = u32(v >> 8) & (tex_height - 1);
    u32 v1 = (v0 + 1) & (tex_height - 1);
    if (!o.texwrapx && u1 == 0) {
        if (ufrac >= 0x80) u0 = u1, u1++, ufrac = 0;
        else u1 = u0, u0--, ufrac = 0x100;
    }
    if (!o.texwrapy && v1 == 0) {
        if (vfrac >= 0x80) v0 = 0, v1++, vfrac = 0;
        else v1 = v0, v0--, vfrac = 0x100;
    }
#ifdef M2_VITA_RENDER_OPT
    const auto quad = read_texel_quad(tex_x, tex_y, u0, u1, v0, v1, sheet);
    u32 tex00 = quad.t00, tex01 = quad.t01, tex10 = quad.t10, tex11 = quad.t11;
#else
    u32 tex00 = u32(get_texel(tex_x, tex_y, int(u0), int(v0), sheet)) << 4;
    u32 tex01 = u32(get_texel(tex_x, tex_y, int(u1), int(v0), sheet)) << 4;
    u32 tex10 = u32(get_texel(tex_x, tex_y, int(u0), int(v1), sheet)) << 4;
    u32 tex11 = u32(get_texel(tex_x, tex_y, int(u1), int(v1), sheet)) << 4;
#endif
    if (Translucent) {
        if (tex00 != 0xf0) tex00 |= 0x00800000;
        if (tex01 != 0xf0) tex01 |= 0x00800000;
        if (tex10 != 0xf0) tex10 |= 0x00800000;
        if (tex11 != 0xf0) tex11 |= 0x00800000;
        if (tex00 == 0x000000f0) tex00 = tex01 & 0xff;
        if (tex01 == 0x000000f0) tex01 = tex00 & 0xff;
        if (tex10 == 0x000000f0) tex10 = tex11 & 0xff;
        if (tex11 == 0x000000f0) tex11 = tex10 & 0xff;
    }
    u32 tex0x = LERP(tex00, tex01, ufrac);
    u32 tex1x = LERP(tex10, tex11, ufrac);
    if (Translucent) {
        if (tex0x == 0x000000f0) tex0x = tex1x & 0xff;
        if (tex1x == 0x000000f0) tex1x = tex0x & 0xff;
    }
    return LERP(tex0x, tex1x, vfrac);
}

template <bool Translucent>
void Raster::draw_scanline_tex(int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o) {
#ifdef M2_VITA_RENDER_OPT
    // Building 128 shades for a four-pixel polygon is a regression. Short
    // spans retain the original lighting equations; wider spans amortize a
    // table and subsequent rows reuse it. This chooses work, not image quality.
    if (!o.shades && x1 - x0 >= 32) o.shades = shade_table(o);
    if (o.shades) { draw_tex_span<Translucent, true>(y, x0, x1, start, dpdx, o); return; }
#endif
    draw_tex_span<Translucent, false>(y, x0, x1, start, dpdx, o);
}

template <bool Translucent, bool Cached>
void Raster::draw_tex_span(int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o) {
    u32 *const p = &dest_[size_t(y) * size_t(stride_)];
    u8 *const fill = &fill_[size_t(y) * size_t(stride_)];
    float ooz = start[0], uoz = start[1], voz = start[2];
    float dooz = dpdx[0], duoz = dpdx[1], dvoz = dpdx[2];
#ifdef M2_VITA_RENDER_OPT
    const s32 max_level = o.max_level;
#else
    const s32 max_level = 30 - std::countl_zero(std::min(o.texwidth, o.texheight));
#endif
    u32 cr = 0, cg = 0, cb = 0;
    if constexpr (!Cached) {
        const u32 colorbase = le16(mem_->palram, o.colorbase + 0x1000) & 0x7fff;
        cr = (((colorbase >> 0) & 0x1f) << 8);
        cg = 0x4000 / 2 + (((colorbase >> 5) & 0x1f) << 8);
        cb = 0x8000 / 2 + (((colorbase >> 10) & 0x1f) << 8);
    }

    int x = x0;
    int dx = 1;
    if (o.checker) {
        if (!((x ^ y) & 1)) {
            x++;
            ooz += dooz;
            uoz += duoz;
            voz += dvoz;
        }
        dx = 2;
        dooz *= 2.0F;
        duoz *= 2.0F;
        dvoz *= 2.0F;
    }
    for (; x < x1; x += dx, ooz += dooz, uoz += duoz, voz += dvoz) {
        if (fill[x] > 0) continue;
        float const z = 1.0F / ooz;
        s32 const mml = -o.texlod + fast_log2(z);
        s32 const level = std::clamp(mml >> 7, 0, max_level);
        s32 const u = to_s32(uoz * z * 256.0F);
        s32 const v = to_s32(voz * z * 256.0F);
        u32 t = fetch_bilinear_texel<Translucent>(o, level, u, v);
        if (mml > 0 && level < max_level) {
            u32 const t2 = fetch_bilinear_texel<Translucent>(o, level + 1, u, v);
            s32 const frac = (mml & 127) << 1;
            t = LERP(t, t2, unsigned(frac));
        } else if (o.utex && mml < 0) {
            u32 const t2 = fetch_bilinear_texel<Translucent>(o, -1, u, v);
            s32 const frac = std::min(-mml >> o.utexminlod, 127);
            t = LERP(t, t2, unsigned(frac));
        }
        if (Translucent) {
            if (t < 0x00400000) continue;
            t &= 0xff;
        }
#ifdef M2_VITA_RENDER_OPT
        if constexpr (Cached) p[x] = o.shades[t >> 1];
        else
#endif
        {
            u8 luma = u8(u32(mem_->lumaram[(o.lumabase + (t >> 1)) * 4]) * o.luma / 256);
            luma = std::min(luma, u8(0x3f));
            const u32 tr = gamma_[le16(mem_->colorxlat, cr + luma) & 0xff];
            const u32 tg = gamma_[le16(mem_->colorxlat, cg + luma) & 0xff];
            const u32 tb = gamma_[le16(mem_->colorxlat, cb + luma) & 0xff];
            p[x] = rgb(tr, tg, tb);
        }
        fill[x] = 0xff;
    }
}

} // namespace rt
