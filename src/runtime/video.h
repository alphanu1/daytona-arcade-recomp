// license:BSD-3-Clause
// copyright-holders:Olivier Galibert, R. Belmont, ElSemi, Angelo Salese
//
// Model 2 screen output: the Sega System 24 tilemap chip (segaic24: four
// 64x64-tile layers, per-line scroll, 8-pixel window masks), the palette the
// tilemaps use, the CRTC offsets, and the composition of the 2D layers with
// the 3D layer (Raster), as MAME's model2_state::screen_update does.
// Transplanted from MAME at dddd73680656e355bb2b5beecab1167c9f07bf81
// (src/mame/sega/segaic24.cpp, model2_v.cpp, model2.cpp; BSD-3-Clause,
// notices above kept as the licence requires). MAME's generic tilemap engine
// is replaced by a direct renderer with the same pixel rules. See
// THIRD_PARTY.md.
#pragma once

#include "runtime/raster.h"
#include "runtime/video_profile.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace rt {

class Video {
public:
    // tile_ram: 0x01000000 (0x10000 bytes, u16 entries); char_ram: 0x01080000
    // (0x80000 bytes, u16 entries); both as the i960 wrote them.
    Video(const uint8_t *tile_ram, const uint8_t *char_ram);

    // Register writes (MAME handlers), fed by the bus as they happen.
    // palette_w after the bus has stored the write (palram holds the new value).
    void palette_w(uint32_t offset, const uint8_t *palram, const uint8_t *colorxlat);
    void colorxlat_w(uint32_t offset) { if ((offset & 0xff) == 0x80 / 2) palette_dirty_ = true; }
    void enable_write_tracking() { write_tracking_ = true; }
    void tile_memory_w() { tile_memory_touched_ = true; }
    void character_memory_w() { character_memory_touched_ = true; }
    void xhout_w(uint16_t data) { crtc_x_ = 84 + int16_t(data); render_x_ = crtc_x_; }
    void xvout_w(uint16_t data) { crtc_y_ = 130 + int16_t(data); render_y_ = crtc_y_; }

    // The geometrizer started a new frame (MAME render_frame_start).
    void frame_start() { render_done_ = false; }
    // MAME screen_update at the end of vblank: 2D back layers, the 3D layer
    // (drawn from `polys` once per geometrizer frame, then reused), 2D front
    // layers. Output: 496x384, 0xAARRGGBB.
    void screen_update(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem);
    const std::vector<uint32_t> &screen() const { return screen_; } // width() x H
    // Widescreen (enhancement, 0 = off): the screen grows by `margin` pixels on
    // each side. The 3D layer fills it; the tilemap layers (HUD, text) stay
    // 496 wide in the centre. Not available with external 3D (the Vita path).
    void set_wide_margin(int margin);
    bool hud_at_edges_active() const { return hud_on_; }
    int wide_margin() const { return margin_; }
    int width() const { return W + 2 * margin_; }
    // With widescreen, behind 3D: stretch the tile backdrop across the whole
    // width (on) or fill the margins with the sky's plain colour (off, default).
    void set_stretch_backdrop(bool on) { stretch_backdrop_ = on; }
    bool stretch_backdrop() const { return stretch_backdrop_; }
    // With widescreen: the race HUD's side groups (lap times; position,
    // condition panel, course map) at the screen edges instead of 4:3 centred.
    void set_hud_edges(bool on) {
        if (on == hud_edges_) return;
        hud_edges_ = on;
        if (!on && hud_on_) { hud_on_ = false; set_raster_hud_moves(); render_done_ = false; }
    }
    // Vita GPU-fast path: keep the exact CPU tile layers, but let the host
    // draw the 3D polygons. The normal desktop/CPU path remains the default.
    // desktop: the SDL_GPU renderer, which draws the tilemap layers
    // itself from the decoded pixmaps (system24_pixels, system24_flags, the
    // tile generations) and this frame's snapshot (gpu_tile_words,
    // gpu_pens). Vita uses its separate GXM tile path, also with widescreen.
    void set_external_3d(bool enabled, bool desktop = false) {
        if (enabled == external_3d_ && desktop == desktop_) return;
        external_3d_ = enabled;
        desktop_ = desktop;
#ifndef M2_VITA_RENDER_OPT
        if (enabled && !desktop && margin_) set_wide_margin(0);
#endif
        gpu_front_margin_ = -1;
        render_done_ = false;
    }
    // External 3D with widescreen and the HUD at the edges: how far the
    // condition panel's overlay polygons move (0 = not at all), and which.
    int gpu_hud_shift() const { return external_3d_ && hud_on_ ? margin_ : 0; }
    // Desktop hardware renderer, as of the last screen_update: tile RAM words
    // kGpuTileFirst.. (line scroll tables, scroll registers, window masks) and
    // the tilemaps' pens; the widescreen margin and how to fill it; whether the
    // front layers come from the CPU (foreground_layer: the HUD moved to the
    // edges) instead of the pixmaps.
    static constexpr uint32_t kGpuTileFirst = 0x4000, kGpuTileWords = 0x3000, kGpuPens = 4096;
    const uint16_t *gpu_tile_words() const { return gpu_tile_words_.data(); }
    const uint32_t *gpu_pens() const { return gpu_pens_.data(); }
    int margin() const { return margin_; }
    size_t poly_count() const { return poly_count_; }
    int window_count() const { return gpu_windows_; }
    bool has_3d() const { return has_3d_; } // this frame drew 3D polygons (race, attract), not a 2D menu
    enum class Backdrop { Edges, Sky, Stretch }; // fill_margins' three cases
    Backdrop backdrop() const { return !scene() ? Backdrop::Edges : stretch_backdrop_ ? Backdrop::Stretch : Backdrop::Sky; }
    bool cpu_front() const { return hud_on_; }
    uint64_t instance() const { return instance_; } // tells a new Video from an old one at the same address
    bool external_3d() const { return external_3d_; }
    void set_gpu_background(bool enabled) { gpu_background_ = enabled; }
    bool gpu_background() const { return external_3d_ && gpu_background_ && margin_; }
    // Like desktop main: only relocated per-item HUDs need the CPU front layer.
    bool gpu_foreground() const { return gpu_background() && !hud_on_; }
#ifdef M2_DC_MEMORY
    // The Dreamcast, external 3D: the layers as composed, not copies (1.5 MB):
    // the background is the screen, the front tile layers sys24 (W x (H + 4)).
    const std::vector<uint32_t> &background_layer() const { return screen_; }
    const std::vector<uint32_t> &foreground_layer() const { return sys24_; }
#ifdef M2_DC_SPEED
    // Rows of the 16-bit layers: the PVR texture's width, so a layer goes to
    // video RAM in one DMA (columns W and up are composed but not shown).
    static constexpr int kLayerStride = 512;
    // With M2_DC_SPEED the external-3D layers are composed straight into the
    // PVR's 16-bit formats (kLayerStride x H, and screen_/sys24_ stay empty): the
    // background RGB565, the front layers ARGB1555 with 0 see-through. The
    // same pixels as converting the 32-bit layers (rgb565/argb1555 below).
    const std::vector<uint16_t> &background16() const { return screen16_; }
    const std::vector<uint16_t> &foreground16() const { return sys24_16_; }
    // Each moves when that layer is composed again: a layer whose inputs
    // (its category's tiles, the pens, scroll, line tables, window masks)
    // have not changed since is not composed, and keeps its pixels.
    uint64_t background16_generation() const { return back16_generation_; }
    uint64_t foreground16_generation() const { return front16_generation_; }
    // Since the start, per layer buffer (0 back, 1 front): composes, those
    // of every line, and the lines composed.
    uint64_t composes(int cat) const { return composes_[cat]; }
    uint64_t full_composes(int cat) const { return full_composes_[cat]; }
    uint64_t composed_lines(int cat) const { return composed_lines_[cat]; }
    static uint16_t rgb565(uint32_t c) { return uint16_t(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f)); }
    static uint16_t argb1555(uint32_t c) {
        return uint16_t((c ? 0x8000 : 0) | ((c >> 9) & 0x7c00) | ((c >> 6) & 0x03e0) | ((c >> 3) & 0x001f));
    }
    // The scrolled layer (the race's scenery): when the back layers' opaque
    // passes draw only pixmap layer 2, on every line with one scroll (split
    // mode 1 with the split off the screen, no line scroll), the frontend
    // draws it and background16() holds only layers 1 and 0, ARGB1555 with
    // 0 see-through. Screen (x, y) shows pixmap layer 2's
    // ((x + scroll_x) & 511, (y + scroll_y) & 511): scroll_pixel there, the
    // pen of a category 0 tile, otherwise pen 0's colour.
    bool back_scrolled() const { return back_scroll_; }
    void set_scroll_layer(bool allowed) { scroll_allowed_ = allowed; } // (tools/dcmemcheck compares both)
    int scroll_x() const { return scroll_x_; }
    int scroll_y() const { return scroll_y_; }
    uint16_t scroll_pixel(uint32_t x, uint32_t y) const {
        const uint32_t i = (y & 511) * 512 + (x & 511);
        return pens565_[tile_class_[2][((y & 511) >> 3) * 64 + ((x & 511) >> 3)] & 1 ? 0 : pixmap_[2][i]];
    }
    // Layer 2's tiles (64 x 64, row by row) whose scroll pixels changed
    // since scroll_taken(); all of them after a pen changed.
    bool scroll_all_changed() const { return scroll_all_dirty_; }
    const uint8_t *scroll_tiles_changed() const { return scroll_dirty_; }
    void scroll_taken() {
        std::fill(scroll_dirty_, scroll_dirty_ + 4096, uint8_t(0));
        scroll_all_dirty_ = false;
    }
#endif
#else
    const std::vector<uint32_t> &background_layer() const { return background_gpu_; }
    const std::vector<uint32_t> &foreground_layer() const { return foreground_gpu_; }
#endif
    uint64_t background_generation() const { return background_generation_; }
    uint64_t foreground_generation() const { return foreground_generation_; }
    const uint16_t *system24_pixels(int layer) const { return pixmap_[layer & 3].data(); }
    const uint8_t *system24_flags(int layer) const { return flags_[layer & 3].data(); }
    uint32_t system24_pen(uint32_t index) const { return pens_[index & 0x1fff]; }
    uint16_t system24_word(uint32_t index) const {
#ifdef M2_VITA_RENDER_OPT
        // GXM must use the same tile-register snapshot as screen_update. The
        // guest can write the live registers again before the host presents.
        const uint8_t *ram = tiles_valid_ ? tile_ram_copy_.data() : tile_ram_;
        return uint16_t(ram[index * 2] | ram[index * 2 + 1] << 8);
#else
        return tile(index);
#endif
    }
    uint64_t system24_texture_generation() const { return system24_texture_generation_; }
    uint64_t system24_palette_generation() const { return system24_palette_generation_; }
    uint64_t system24_tile_generation(int layer, unsigned tile_index) const {
        return system24_tile_generations_[unsigned(layer & 3) * 4096u + (tile_index & 4095u)];
    }
    bool system24_gpu_compatible() const;
    const std::vector<GeoPoly> &gpu_polys() const;
    int gpu_windows() const { return gpu_windows_; }
    const VideoMem &gpu_mem() const { return gpu_mem_; }
    int crtc_x() const { return crtc_x_; }
    int crtc_y() const { return crtc_y_; }
    int render_x() const { return render_x_; }
    int render_y() const { return render_y_; }
    uint64_t screen_hash() const;
    const Raster &raster() const { return raster_; }
    bool rendered_now() const { return rendered_now_; } // the last update drew the 3D layer afresh
    uint64_t raster_hash() const { return raster_.hash(0, 495, 0, 383); }

    using ProfileClock = uint64_t (*)();
    void set_profile_clock(ProfileClock clock) { profile_clock_ = clock; }
    const VideoProfile &last_profile() const { return profile_; }
    static constexpr int W = 496, H = 384;

private:
    uint16_t tile(uint32_t i) const { return uint16_t(tile_ram_[i * 2] | tile_ram_[i * 2 + 1] << 8); }
    void build_layer(int layer); // pixmap_/flags_ for one tilemap
#ifdef M2_DC_SPEED
    // Any pixel type, with its own pens (the 16-bit layers); the 32-bit
    // screen with pens_ as before.
    template <typename Pixel> void draw(std::vector<Pixel> &bitmap, int layer, int flags, const Pixel *pens);
    template <typename Pixel>
    void draw_rect(std::vector<Pixel> &dm, const uint16_t *mask, uint16_t tpri, int flags, int win, int L, int sx, int sy,
                   int xx1, int yy1, int xx2, int yy2, const Pixel *pens);
    template <typename Pixel>
    void tilemap_draw(std::vector<Pixel> &dm, int L, int sx, int sy, int minx, int maxx, int miny, int maxy, int flags,
                      const Pixel *pens);
    void draw(std::vector<uint32_t> &bitmap, int layer, int flags) { draw(bitmap, layer, flags, pens_); }
#else
    void draw(std::vector<uint32_t> &bitmap, int layer, int flags);
    void draw_rect(std::vector<uint32_t> &dm, const uint16_t *mask, uint16_t tpri, int flags, int win, int L, int sx,
                   int sy, int xx1, int yy1, int xx2, int yy2);
    void tilemap_draw(std::vector<uint32_t> &dm, int L, int sx, int sy, int minx, int maxx, int miny, int maxy, int flags);
#endif

    uint64_t ticks() const { return profile_clock_ ? profile_clock_() : 0; }
#ifndef M2_VITA_RENDER_OPT
    void decode_layers();                      // build_layer for changed tiles only
    std::vector<uint8_t> dec_chars_, dec_char_dirty_; // char RAM as last decoded; characters changed since
    std::vector<uint16_t> dec_tiles_;          // tile values as last decoded
    bool dec_valid_ = false;
#ifdef M2_DC_SPEED
    bool dec_dirty_any_ = true; // some dec_char_dirty_ flag set (clear only then; build_layer skips by blocks without)
#endif
#endif
    ProfileClock profile_clock_ = nullptr;
    VideoProfile profile_;
#ifdef M2_VITA_RENDER_OPT
    // Snapshot comparisons also see writes made through replay/raw RAM pointers.
    // No write-hook assumptions, hashes with collisions, or per-frame allocation.
    void update_tile_cache();
    std::vector<uint8_t> character_copy_, tile_ram_copy_, character_dirty_;
    std::vector<uint16_t> tile_values_;
    std::vector<uint32_t> background_;
    bool tiles_valid_ = false, background_dirty_ = true, foreground_dirty_ = true;
#endif
    const uint8_t *tile_ram_, *char_ram_;
    uint32_t pens_[8192];
    bool palette_dirty_ = false;
    int crtc_x_ = 0, crtc_y_ = 0, render_x_ = 90, render_y_ = -8;
    uint8_t gamma_[256];
    std::vector<uint16_t> pixmap_[4];
    std::vector<uint8_t> flags_[4];
#ifdef M2_DC_SPEED
    // Per layer and row of tiles (8 pixmap rows), how many of its 64 tiles
    // are of each category, and how many of those have an opaque pixel:
    // draw passes skip rows where nothing can match (build_layer keeps them).
    uint8_t row_tiles_[4][64][2] = {}, row_opaque_[4][64][2] = {};
    uint8_t tile_class_[4][4096] = {}; // 0x80 built | 4 every pixel opaque | 2 an opaque pixel | category
    bool row_empty(int L, uint32_t pixmap_row, int cat, bool opaque_pass) const {
        const uint32_t r = (pixmap_row & 511) >> 3;
        return !(opaque_pass ? row_tiles_[L][r][cat & 1] : row_opaque_[L][r][cat & 1]);
    }
#endif
    std::vector<uint32_t> screen_, sys24_;
#ifdef M2_DC_SPEED
    std::vector<uint16_t> screen16_, sys24_16_;
    bool back_dirty_ = true, front_dirty_ = true; // a tile of that category rebuilt, or a pen changed
    // Every line of that layer buffer to compose again: a pen or the
    // registers, line tables or window masks changed. Otherwise only the
    // lines that show a row of tiles build_layer rebuilt (dirty_rows_:
    // [category][pixmap layer][row of tiles]) are composed (line_filter_).
    bool back_full_ = true, front_full_ = true;
    uint8_t dirty_rows_[2][4][64] = {};
    const uint8_t *line_filter_ = nullptr; // compose only lines y with line_filter_[y] set (nullptr: all)
    bool composed_uses_[2][4] = {}; // [category][pixmap layer]: the layer (or its split pair) had such tiles when composed
    uint64_t composed_lines_[2] = {}, composes_[2] = {}, full_composes_[2] = {}; // counts (lines composed, and how)
    bool layer_has(int L, int cat) const {
        for (int r = 0; r < 64; ++r)
            if (row_tiles_[L][r][cat]) return true;
        return false;
    }
    void compose16(int cat, const bool uses[4], const uint8_t *extra_lines);
    std::vector<uint8_t> regs_copy_;              // tile RAM 0x8000-0xdfff (line tables, registers, masks) as composed
    uint64_t back16_generation_ = 0, front16_generation_ = 0;
    uint16_t pens565_[8192], pens1555_[8192]; // pens_ as rgb565 and argb1555 (palette_w keeps them)
    bool back_scroll_ = false, scroll_all_dirty_ = true, scroll_allowed_ = true; // (back_scrolled, scroll_all_changed)
    int scroll_x_ = 0, scroll_y_ = 0;
    uint8_t scroll_dirty_[4096] = {};
    bool scroll_mode(int &sx, int &sy) const;
#endif
    std::vector<uint32_t> background_gpu_, foreground_gpu_;
    uint64_t background_generation_ = 0, foreground_generation_ = 0, system24_texture_generation_ = 0;
    bool system24_source_dirty_ = true;
    std::vector<uint64_t> system24_tile_generations_;
    uint64_t system24_palette_generation_ = 0;
    const std::vector<GeoPoly> *gpu_polys_ = nullptr;
    VideoMem gpu_mem_{};
    int gpu_windows_ = 0;
    bool external_3d_ = false, desktop_ = false;
    std::vector<uint16_t> gpu_tile_words_;
    std::vector<uint32_t> gpu_pens_;
    uint64_t instance_;
    bool gpu_background_ = false;
    std::vector<uint32_t> gpu_front_source_;
    int gpu_front_margin_ = -1;
    bool gpu_front_hud_ = false;
    int margin_ = 0;
    int dw_ = W;                               // draw()'s output width
    std::vector<uint32_t> stretch_row_;        // widescreen: one backdrop row, for stretching
    bool has_3d_ = true;
    size_t poly_count_ = 0;
    int coverage_ = 100;                       // widescreen: % of the screen the last 3D render covered
    // Widescreen: is this frame a 3D scene (race, attract) rather than a 2D
    // screen (titles, car and circuit select)? Scenes draw in one window and
    // cover at least 15% of the screen; measured: scenes looking at a lot of
    // sky 36-49% (so not "half the screen", which made those frames smear
    // their edge colours and then snap to stretched), daytona93's select
    // screens put their 3D in 2-3 windows, Revision A's have none, titles none.
    bool scene() const { return gpu_windows_ <= 1 && coverage_ >= 15; }
    bool stretch_backdrop_ = false;
    void fill_margins();
    bool hud_edges_ = false;
    bool hud_on_ = false;                      // the rasterizer is moving the HUD overlay polygons
    void set_raster_hud_moves();
    // Scratch for copy_front_hud_to_edges (kept to avoid per-frame allocation).
    std::vector<uint8_t> hud_mask_, hud_tmp_;
    std::vector<int32_t> hud_label_, hud_move_;
    std::vector<std::array<int, 4>> hud_box_;
    std::vector<uint32_t> hud_stack_;
    void copy_front_hud_to_edges(std::vector<uint32_t> &out); // width() wide
    bool write_tracking_ = false, tile_memory_touched_ = false, character_memory_touched_ = false;
    Raster raster_;
    bool rendered_now_ = false, render_done_ = false;
};

} // namespace rt
