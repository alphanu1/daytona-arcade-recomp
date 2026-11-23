// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels, Ville Linde, Aaron Giles
//
// CPU reference rasterizer for the Model 2 3D layer: projects, orders and
// draws a frame's display list (Geo::polys) into a 512x512 RGB32 layer.
// Transplanted from MAME at dddd73680656e355bb2b5beecab1167c9f07bf81
// (BSD-3-Clause; notices above kept as the licence requires):
// src/mame/sega/model2_v.cpp (model2_3d_project, render_polygons,
// model2_renderer::model2_3d_render), src/mame/sega/model2rd.ipp (scanline
// shaders, bilinear texel fetch) and the parts of src/devices/video/poly.h
// they use (render_triangle, render_polygon, round_coordinate), drawn
// immediately in order instead of through MAME's work queue. This is the
// ground truth for the GPU renderer, checked against MAME frame by frame.
// See THIRD_PARTY.md.
#pragma once

#include "runtime/geo.h"

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <array>
#include <memory>
#include <vector>

// Banded drawing: polygons are prepared once (projection and setup, in MAME's
// order), then horizontal bands of scanlines are drawn on several threads,
// each band drawing every polygon in that same order clipped to its rows.
// Every scanline's start values come from the polygon setup alone, and depth
// fill and colour are per pixel, so each pixel sees exactly the polygons and
// arithmetic of the serial path: the output is identical. (MAME's poly.h also
// splits work by scanlines.) Not on the Vita (its shade cache is shared
// across polygons) or the Dreamcast (one core).
#if !defined(M2_VITA_RENDER_OPT) && !defined(M2_DC_MEMORY) && !defined(M2_DC_SPEED)
#define M2_RASTER_BANDS 1
#endif

namespace rt {

// The video memories the rasterizer reads, as the i960 sees them.
struct VideoMem {
    const uint8_t *palram = nullptr;    // 0x01800000, 0x4000 bytes (u16 entries)
    const uint8_t *colorxlat = nullptr; // 0x01810000, 0xc000 bytes (u16 entries)
    const uint8_t *lumaram = nullptr;   // 0x12800000, 0x20000 bytes (byte lane 0 of each dword)
    const uint32_t *tex0 = nullptr;     // texture RAM 0, packed as MAME stores it (0x80000 dwords)
    const uint32_t *tex1 = nullptr;     // texture RAM 1
    uint64_t tex_generation = 0;        // counts writes to texture RAM (changes when either sheet does)
};

class Raster {
public:
    Raster();
    ~Raster();
    Raster(const Raster &) = delete;
    Raster &operator=(const Raster &) = delete;

    // Threads drawing bands (M2_RASTER_BANDS): 0 = automatic (the
    // M2_RASTER_THREADS environment variable if set, else the host's cores, at
    // most 8); 1 = the serial path. The picture is the same either way.
    void set_threads(int threads) { threads_ = threads; }
    int threads() const;

    // MAME render_polygons: clear, then draw windows from the last down to
    // 0, each in z-bucket order (low to high z; newest first within a
    // bucket). crtc_x/crtc_y: MAME's m_crtc_xoffset/m_crtc_yoffset (the
    // projection); render_x/render_y: the renderer's offsets (the viewport;
    // equal to the CRTC's once the game has set them). clip: the visible
    // area, inclusive.
    void render(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem, int crtc_x, int crtc_y,
                int clip_minx, int clip_maxx, int clip_miny, int clip_maxy) {
        render(polys, windows, mem, crtc_x, crtc_y, crtc_x, crtc_y, clip_minx, clip_maxx, clip_miny, clip_maxy);
    }
    void render(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem, int crtc_x, int crtc_y, int render_x,
                int render_y, int clip_minx, int clip_maxx, int clip_miny, int clip_maxy);

    const uint32_t *pixels() const { return dest_.data(); } // stride() x 512, 0x00RRGGBB
    int stride() const { return stride_; }                   // 512, wider for widescreen
    // Widescreen (enhancement, 0 = off): callers shift x by `margin`; a polygon
    // whose viewport spans the 496-pixel screen may then draw `margin` pixels
    // beyond either side. The layer grows to hold the wider screen.
    void set_wide_margin(int margin);
    // Widescreen, HUD at the edges. The race HUD's condition panel is drawn
    // with overlay polygons: its box, one checker-shaded polygon (texheader
    // 0x8000) at a fixed near sort z (0x0600; scenery there is above 18000),
    // about 77x82 pixels at x 385..462, y 67..149 (496-wide coordinates), and
    // the car icon inside it at the same z. find_race_hud looks for the box
    // and remembers its outline and z; with a shift set, only polygons at
    // exactly that z lying inside that outline move: the overlay's own quads,
    // never the 3D scene around them. No box: the race HUD is not on screen.
    static constexpr uint16_t kHudOverlayZ = 0x0fff;
    bool find_race_hud(const std::vector<GeoPoly> &polys, int crtc_x, int crtc_y);
    void set_hud_shift(int dx) { hud_dx_ = dx; }
    // The box find_race_hud found (x0, x1, y0, y1 in 496-wide coordinates)
    // and its sort z, for the hardware renderer's copy of the move.
    const float *hud_box() const { return hud_box_; }
    uint16_t hud_z() const { return hud_z_; }
    int hud_polygon_offset(const GeoPoly &projected) const;
    // Widescreen with the hardware renderer, which leaves no CPU 3D layer to
    // count: roughly how much of the original 496x384 screen the polygons
    // cover, in percent, from a coarse rasterization (8x8-pixel cells).
    int coverage_estimate(const std::vector<GeoPoly> &polys, int windows, int crtc_x, int crtc_y) const;
    uint64_t hash(int minx, int maxx, int miny, int maxy) const; // as the MAME log computes it

    struct Extra; // per-polygon shading state (MAME m2_poly_extra_data)

private:
    int stride_ = 512, margin_ = 0;
    int hud_dx_ = 0;                           // HUD overlay move (0 = none)
    float hud_box_[4] = {0, 0, 0, 0};          // the condition box: x0, x1, y0, y1 (496-wide)
    uint16_t hud_z_ = 0;
    std::vector<uint32_t> dest_;
    std::vector<uint8_t> fill_;
    uint8_t gamma_[256];
    const VideoMem *mem_ = nullptr;
#ifdef M2_VITA_RENDER_OPT
    struct ShadeEntry {
        uint32_t key = 0xffffffffu;
        std::array<uint32_t, 128> colors{};
    };
    // Cleared logically every render: palette/luma/translation RAM may change
    // between frames. A pointer is used only while its polygon is being drawn.
    std::array<ShadeEntry, 64> shades_;
    const uint32_t *shade_table(const Extra &o);
    std::vector<std::size_t> order_;
#endif

    // A polygon projected and set up for drawing (render_one's first half).
    struct Prepared;
    struct Pool;
    int threads_ = 0;
#ifdef M2_RASTER_BANDS
    int pool_threads_ = 0;
    std::unique_ptr<Pool> pool_;
    std::vector<Prepared> prepared_;
#endif
    bool prepare_one(const GeoPoly &source, int crtc_x, int crtc_y, int render_x, int render_y, int clip_minx, int clip_maxx,
                     int clip_miny, int clip_maxy, Prepared &out) const;
    void draw_prepared(const Prepared &p, int band_miny, int band_maxy);
    void render_one(const GeoPoly &poly, int crtc_x, int crtc_y, int render_x, int render_y, int clip_minx, int clip_maxx,
                    int clip_miny, int clip_maxy);
    template <bool Translucent> void draw_scanline_solid(int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o);
    template <bool Translucent> void draw_scanline_tex(int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o);
    template <bool Translucent, bool Cached> void draw_tex_span(int32_t y, int32_t x0, int32_t x1,
        const float *start, const float *dpdx, const Extra &o);
    void scanline(int renderer, int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o);
    void render_triangle(const int *clip, int renderer, const Extra &o, const GeoVertex &v1, const GeoVertex &v2, const GeoVertex &v3);
    template <int NumVerts> void render_polygon(const int *clip, int renderer, const Extra &o, const GeoVertex *v);
    template <bool Translucent> uint32_t fetch_bilinear_texel(const Extra &o, int32_t miplevel, int32_t u, int32_t v) const;
};

} // namespace rt
