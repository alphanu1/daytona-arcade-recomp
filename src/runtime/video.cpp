// license:BSD-3-Clause
// copyright-holders:Olivier Galibert, R. Belmont, ElSemi, Angelo Salese
//
// Model 2 screen output, transplanted from MAME (see video.h): segaic24's
// tile_info, draw_rect (rgb32) and draw_common; model2_state::palette_w,
// colorxlat_w, horizontal/vertical_sync_w, render_polygons' frame logic and
// screen_update. MAME's tilemap engine is replaced by build_layer (the same
// pixmap and flags MAME's tilemap caches hold) and tilemap_draw (the same
// pixel rule as tilemap_t::draw with one scroll value).

#include "runtime/video.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

namespace rt {

namespace {
constexpr uint8_t PIXEL_LAYER0 = 0x10;  // TILEMAP_PIXEL_LAYER0
constexpr uint8_t CATEGORY_MASK = 0x0f; // TILEMAP_PIXEL_CATEGORY_MASK
constexpr int DRAW_OPAQUE = 0x80;       // TILEMAP_DRAW_OPAQUE
inline uint16_t le16(const uint8_t *b, uint32_t i) { return uint16_t(b[i * 2] | b[i * 2 + 1] << 8); }
inline uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return 0xff000000u | (r << 16) | (g << 8) | b; }
} // namespace

Video::Video(const uint8_t *tile_ram, const uint8_t *char_ram)
    : tile_ram_(tile_ram), char_ram_(char_ram),
#ifdef M2_DC_SPEED
      // (The Dreamcast composes 16-bit layers: the 32-bit ones only if a
      // frame is ever drawn without external 3D, allocated then.)
      screen16_(size_t(kLayerStride) * H), sys24_16_(size_t(kLayerStride) * (H + 4)),
#else
      screen_(size_t(W) * H), sys24_(size_t(W) * (H + 4)),
#endif
#ifndef M2_DC_MEMORY
      // (Not on the Dreamcast: its layers are screen_ and sys24_ themselves.)
      background_gpu_(size_t(W) * H), foreground_gpu_(size_t(W) * H),
#endif
      gpu_tile_words_(kGpuTileWords), gpu_pens_(kGpuPens) {
    static uint64_t instances = 0;
    instance_ = ++instances;
    for (auto &p : pens_) p = rgb(0, 0, 0); // palette_device starts black
#ifdef M2_DC_SPEED
    for (auto &p : pens565_) p = rgb565(rgb(0, 0, 0));
    for (auto &p : pens1555_) p = argb1555(rgb(0, 0, 0));
#endif
    for (int i = 0; i < 256; i++) gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
    for (int l = 0; l < 4; l++) pixmap_[l].assign(512 * 512, 0), flags_[l].assign(512 * 512, 0);
    system24_tile_generations_.resize(4 * 4096);
#ifndef M2_VITA_RENDER_OPT
    dec_chars_.resize(0x80000);
    dec_char_dirty_.resize(0x4000);
    dec_tiles_.resize(4 * 4096);
#endif
#ifdef M2_VITA_RENDER_OPT
    character_copy_.resize(0x80000);
    character_dirty_.resize(0x4000);
    tile_ram_copy_.resize(0x10000);
    tile_values_.resize(4 * 4096);
    background_.resize(size_t(W) * (H + 4));
#endif
}

void Video::palette_w(uint32_t offset, const uint8_t *palram, const uint8_t *colorxlat) {
    const uint16_t palcolor = le16(palram, offset);
    const uint8_t r = uint8_t(le16(colorxlat, (0x0080 >> 1) + (((palcolor >> 0) & 0x1f) << 8)));
    const uint8_t g = uint8_t(le16(colorxlat, (0x4080 >> 1) + (((palcolor >> 5) & 0x1f) << 8)));
    const uint8_t b = uint8_t(le16(colorxlat, (0x8080 >> 1) + (((palcolor >> 10) & 0x1f) << 8)));
    const uint32_t pen = rgb(gamma_[r], gamma_[g], gamma_[b]);
    if (pens_[offset & 0x1fff] != pen) {
#ifdef M2_VITA_RENDER_OPT
        background_dirty_ = foreground_dirty_ = true;
#endif
        system24_source_dirty_ = true;
        system24_palette_generation_ = system24_texture_generation_ + 1;
#ifdef M2_DC_SPEED
        back_dirty_ = front_dirty_ = true;
        back_full_ = front_full_ = true;
        scroll_all_dirty_ = true;
#endif
    }
    pens_[offset & 0x1fff] = pen;
#ifdef M2_DC_SPEED
    pens565_[offset & 0x1fff] = rgb565(pen);
    pens1555_[offset & 0x1fff] = argb1555(pen);
#endif
}

#if defined(M2_DC_SPEED) && !defined(M2_VITA_RENDER_OPT)
// 32 bytes (16 tile values, both 4-aligned) equal: memcmp's answer, in
// eight word loads a side instead of a library call per block of tiles.
static inline bool same_block(const uint8_t *a, const uint16_t *b16)
{
    const uint8_t *b = reinterpret_cast<const uint8_t *>(b16);
#ifdef __GNUC__
    // (Known 4-aligned: each 4-byte memcpy below is one load, not a call.)
    a = static_cast<const uint8_t *>(__builtin_assume_aligned(a, 4));
    b = static_cast<const uint8_t *>(__builtin_assume_aligned(b, 4));
#endif
    uint32_t diff = 0;
    for (int k = 0; k < 32; k += 4) {
        uint32_t x, y;
        std::memcpy(&x, a + k, 4);
        std::memcpy(&y, b + k, 4);
        diff |= x ^ y;
    }
    return diff == 0;
}
#endif

// segaic24 tile_info + MAME tilemap pixmap: 64x64 tiles (TILEMAP_SCAN_ROWS)
// of 8x8, 4bpp chars (char_layout, bit order swapped within 16-bit words),
// pen = color * 16 + pixel, pen 0 transparent, category = tile bit 15.
void Video::build_layer(int layer) {
    const uint32_t base = uint32_t(layer) * 0x1000; // tile_info_0s/0w/1s/1w
    uint16_t *pm = pixmap_[layer].data();
    uint8_t *fm = flags_[layer].data();
    for (uint32_t t = 0; t < 64 * 64; t++) {
#if defined(M2_DC_SPEED) && !defined(M2_VITA_RENDER_OPT)
        // No character changed: a block of 16 tiles whose values are all
        // as last decoded needs nothing (tile RAM and the copy are both
        // little-endian words).
        if (dec_valid_ && !dec_dirty_any_ && (t & 15) == 0 &&
            same_block(tile_ram_ + size_t(t | base) * 2, &dec_tiles_[base + t])) {
            t += 15;
            continue;
        }
#endif
        const uint16_t val = tile(t | base);
        const uint32_t code = val & 0x3fff;
#ifdef M2_VITA_RENDER_OPT
        uint16_t &previous = tile_values_[base + t];
        if (tiles_valid_ && previous == val && !character_dirty_[code]) continue;
        if (!tiles_valid_) background_dirty_ = foreground_dirty_ = true;
        else {
            (previous & 0x8000 ? foreground_dirty_ : background_dirty_) = true;
            (val & 0x8000 ? foreground_dirty_ : background_dirty_) = true;
        }
        previous = val;
#else
        // Desktop: only tiles whose value or character changed (decode_layers).
        uint16_t &previous = dec_tiles_[base + t];
        if (dec_valid_ && previous == val && !dec_char_dirty_[code]) continue;
        previous = val;
#endif
        ++profile_.tiles_rebuilt;
        system24_source_dirty_ = true;
        system24_tile_generations_[base + t] = system24_texture_generation_ + 1;
        const uint32_t color = (val >> 7) & 0xff;
        const uint8_t category = (val & 0x8000) ? 1 : 0;
        const uint32_t tx = (t & 63) * 8, ty = (t >> 6) * 8;
#ifdef M2_DC_SPEED
        bool opaque = false, solid = true;
#endif
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 8; x++) {
                const uint32_t b = code * 32 + y * 4 + (x >> 1);
                const uint8_t byte = char_ram_[b ^ 1];
                const uint8_t pix = (x & 1) ? (byte & 0x0f) : (byte >> 4);
                const size_t i = size_t(ty + y) * 512 + (tx + x);
                pm[i] = uint16_t(color * 16 + pix);
                fm[i] = uint8_t(category | (pix ? PIXEL_LAYER0 : 0));
#ifdef M2_DC_SPEED
                opaque |= pix != 0;
                solid &= pix != 0;
#endif
            }
#ifdef M2_DC_SPEED
        uint8_t &cls = tile_class_[layer][t];
        const uint32_t row = t >> 6;
        // The layer buffers of the old and the new category change.
        if (cls & 0x80) {
            (cls & 1 ? front_dirty_ : back_dirty_) = true;
            dirty_rows_[cls & 1][layer][row] = 1;
        }
        (category ? front_dirty_ : back_dirty_) = true;
        dirty_rows_[category][layer][row] = 1;
        if (layer == 2) scroll_dirty_[t] = 1;
        if (cls & 0x80) {
            --row_tiles_[layer][row][cls & 1];
            if (cls & 2) --row_opaque_[layer][row][cls & 1];
        }
        cls = uint8_t(0x80 | (solid ? 4 : 0) | (opaque ? 2 : 0) | category);
        ++row_tiles_[layer][row][category];
        if (opaque) ++row_opaque_[layer][row][category];
#endif
    }
}

#ifndef M2_VITA_RENDER_OPT
// The four layers' pixmaps, re-decoding only tiles whose tile value or
// character changed since the last frame: the same pixmaps as decoding all
// 16,384 tiles every frame, at a fraction of the cost (in a race only the
// HUD's digits change). Characters are compared (256-byte pages, then 32-byte
// characters) only on frames the game wrote character RAM.
void Video::decode_layers() {
#ifdef M2_DC_SPEED
    // Neither tile RAM nor character RAM written since the last decode (the
    // board reports both): no tile can need rebuilding, so skip the 16,384
    // tile compares.
    if (dec_valid_ && write_tracking_ && !tile_memory_touched_ && !character_memory_touched_) return;
#endif
    if (dec_valid_ && (character_memory_touched_ || !write_tracking_)) {
#ifdef M2_DC_SPEED
        if (dec_dirty_any_) std::fill(dec_char_dirty_.begin(), dec_char_dirty_.end(), uint8_t(0));
        dec_dirty_any_ = false;
#else
        std::fill(dec_char_dirty_.begin(), dec_char_dirty_.end(), uint8_t(0));
#endif
        constexpr size_t kPage = 256;
        for (size_t page = 0; page < dec_chars_.size(); page += kPage) {
            if (std::memcmp(char_ram_ + page, dec_chars_.data() + page, kPage) == 0) continue;
            for (size_t c = page / 32; c < (page + kPage) / 32; ++c)
                if (std::memcmp(char_ram_ + c * 32, dec_chars_.data() + c * 32, 32) != 0) {
                    dec_char_dirty_[c] = 1;
                    ++profile_.characters_changed;
#ifdef M2_DC_SPEED
                    dec_dirty_any_ = true;
#endif
                }
            std::memcpy(dec_chars_.data() + page, char_ram_ + page, kPage);
        }
    } else if (dec_valid_) {
#ifdef M2_DC_SPEED
        if (dec_dirty_any_) std::fill(dec_char_dirty_.begin(), dec_char_dirty_.end(), uint8_t(0));
        dec_dirty_any_ = false;
#else
        std::fill(dec_char_dirty_.begin(), dec_char_dirty_.end(), uint8_t(0));
#endif
    } else {
        std::memcpy(dec_chars_.data(), char_ram_, dec_chars_.size());
    }
    for (int l = 0; l < 4; l++) build_layer(l);
    dec_valid_ = true;
    character_memory_touched_ = tile_memory_touched_ = false;
}
#endif

#ifdef M2_VITA_RENDER_OPT
void Video::update_tile_cache() {
    const bool chars_changed = !tiles_valid_ || (write_tracking_ ? character_memory_touched_ :
        std::memcmp(char_ram_, character_copy_.data(), character_copy_.size()) != 0);
    const bool ram_changed = !tiles_valid_ || (write_tracking_ ? tile_memory_touched_ :
        std::memcmp(tile_ram_, tile_ram_copy_.data(), tile_ram_copy_.size()) != 0);
    if (!chars_changed && !ram_changed) return;
    std::fill(character_dirty_.begin(), character_dirty_.end(), uint8_t(0));
    if (chars_changed) {
        constexpr size_t page_bytes = 256;
        constexpr size_t chars_per_page = page_bytes / 32;
        for (size_t page = 0; page < character_copy_.size(); page += page_bytes) {
            if (tiles_valid_ && std::memcmp(char_ram_ + page, character_copy_.data() + page, page_bytes) == 0)
                continue;
            const size_t first = page / 32;
            for (size_t local = 0; local < chars_per_page; ++local) {
                const size_t code = first + local;
                const size_t offset = code * 32;
                if (!tiles_valid_ || std::memcmp(char_ram_ + offset, character_copy_.data() + offset, 32) != 0) {
                    std::memcpy(character_copy_.data() + offset, char_ram_ + offset, 32);
                    character_dirty_[code] = 1;
                    ++profile_.characters_changed;
                }
            }
        }
    }
    bool draw_state_changed = !tiles_valid_;
    if (tiles_valid_ && ram_changed) {
        auto changed = [&](size_t offset, size_t bytes) {
            return std::memcmp(tile_ram_ + offset, tile_ram_copy_.data() + offset, bytes) != 0;
        };
        // Line-scroll tables, layer control/scroll registers and window masks.
        draw_state_changed = changed(0x8000, 0x1000) || changed(0xa000, 0x10) || changed(0xc000, 0x2000);
    }
    for (int layer = 0; layer < 4; ++layer) build_layer(layer);
    // Normal-mode opaque backgrounds ignore category, whereas split modes
    // still filter category 0. Changing modes changes uploaded alpha even
    // when tile and character RAM are unchanged.
    if (tiles_valid_ && ram_changed &&
        bool(tile(0x5006) & 0x6000) != bool(le16(tile_ram_copy_.data(), 0x5006) & 0x6000)) {
        std::fill(system24_tile_generations_.begin() + 2 * 4096,
                  system24_tile_generations_.end(), system24_texture_generation_ + 1);
        system24_source_dirty_ = true;
    }
    // The remaining tile RAM contains scrolling, window masks and line tables.
    // Any change there invalidates composition even when no glyph was rebuilt.
    if (ram_changed) std::memcpy(tile_ram_copy_.data(), tile_ram_, tile_ram_copy_.size());
    if (draw_state_changed) background_dirty_ = foreground_dirty_ = true;
    tiles_valid_ = true;
    tile_memory_touched_ = character_memory_touched_ = false;
}
#endif

// M2_DC_SPEED: draw, draw_rect and tilemap_draw are templates on the pixel
// type with the pens to use (the Dreamcast's 16-bit layers); otherwise they
// are the 32-bit functions with pens_, exactly as before.
#ifdef M2_DC_SPEED
#define M2_PIXEL_TEMPLATE template <typename Pixel>
#define M2_PIXEL Pixel
#define M2_PENS , const Pixel *pens
#define M2_PENS_ARG , pens
#define M2_PEN pens
#else
#define M2_PIXEL_TEMPLATE
#define M2_PIXEL uint32_t
#define M2_PENS
#define M2_PENS_ARG
#define M2_PEN pens_
#endif

// segaic24 draw_rect, rgb32 version (model 1/2): copy a rectangle of the
// layer's pixmap to the bitmap through the 8-pixel window mask.
M2_PIXEL_TEMPLATE void Video::draw_rect(std::vector<M2_PIXEL> &dm, const uint16_t *mask, uint16_t tpri, int flags, int win, int L, int sx,
                      int sy, int xx1, int yy1, int xx2, int yy2 M2_PENS) {
    const uint16_t *source = &pixmap_[L][size_t(sy) * 512 + size_t(sx)];
    const uint8_t *trans = &flags_[L][size_t(sy) * 512 + size_t(sx)];
    M2_PIXEL *dest = &dm[size_t(yy1) * size_t(dw_) + size_t(xx1)];
    tpri |= PIXEL_LAYER0;
    mask += yy1 * 4;
    yy2 -= yy1;
    while (xx1 >= 128) {
        xx1 -= 128;
        xx2 -= 128;
        mask++;
    }
    for (int y = 0; y < yy2; y++) {
#ifdef M2_DC_SPEED
        // A row where no tile of this category has an opaque pixel draws
        // nothing (unless drawing opaque).
        // (Or a line not being composed again: compose16.)
        if ((line_filter_ && !line_filter_[yy1 + y]) ||
            (!(flags & DRAW_OPAQUE) && row_empty(L, uint32_t(sy + y), tpri & 1, false))) {
            source += 512;
            trans += 512;
            dest += dw_;
            mask += 4;
            continue;
        }
#endif
        const uint16_t *src = source;
        const uint8_t *srct = trans;
        M2_PIXEL *dst = dest;
        const uint16_t *mask1 = mask;
        int llx = xx2;
        int cur_x = xx1;
        while (llx > 0) {
            uint16_t m = *mask1++;
            if (win) m = uint16_t(~m);
            if (!cur_x && llx >= 128) {
                if (!m) {
                    for (int x = 0; x < 128; x++) {
                        if (*srct++ == tpri || (flags & DRAW_OPAQUE)) *dst = M2_PEN[*src];
                        src++;
                        dst++;
                    }
                } else if (m == 0xffff) {
                    src += 128;
                    srct += 128;
                    dst += 128;
                } else {
                    for (int x = 0; x < 128; x += 8) {
                        if (!(m & 0x8000))
                            for (int xx = 0; xx < 8; xx++)
                                if (srct[xx] == tpri || (flags & DRAW_OPAQUE)) dst[xx] = M2_PEN[src[xx]];
                        src += 8;
                        srct += 8;
                        dst += 8;
                        m = uint16_t(m << 1);
                    }
                }
            } else {
                const int llx1 = llx >= 128 ? 128 : llx;
                if (!m) {
                    for (int x = cur_x; x < llx1; x++) {
                        if (*srct++ == tpri || (flags & DRAW_OPAQUE)) *dst = M2_PEN[*src];
                        src++;
                        dst++;
                    }
                } else if (m == 0xffff) {
                    src += 128 - cur_x;
                    srct += 128 - cur_x;
                    dst += 128 - cur_x;
                } else {
                    for (int x = cur_x; x < llx1; x++) {
                        if ((*srct++ == tpri || (flags & DRAW_OPAQUE)) && !(m & (0x8000 >> (x >> 3)))) *dst = M2_PEN[*src];
                        src++;
                        dst++;
                    }
                }
            }
            llx -= 128;
            cur_x = 0;
        }
        source += 512;
        trans += 512;
        dest += dw_;
        mask += 4;
    }
}

// tilemap_t::draw with one scroll value: dest (x, y) takes pixmap
// ((x + sx) & 511, (y + sy) & 511) where (flags & mask) == value; mask is the
// category, plus layer 0 (opacity) unless drawing opaque.
M2_PIXEL_TEMPLATE void Video::tilemap_draw(std::vector<M2_PIXEL> &dm, int L, int sx, int sy, int minx, int maxx, int miny, int maxy,
                                           int flags M2_PENS) {
    const uint8_t cat = uint8_t(flags & CATEGORY_MASK);
    const uint8_t mask = (flags & DRAW_OPAQUE) ? CATEGORY_MASK : uint8_t(CATEGORY_MASK | PIXEL_LAYER0);
    const uint8_t value = (flags & DRAW_OPAQUE) ? cat : uint8_t(cat | PIXEL_LAYER0);
#ifdef M2_DC_SPEED
    // The same pixels, a row at a time in runs that do not wrap at 512
    // (the per-pixel index and vector lookups cost the SH-4 most of it).
    const uint16_t *const pixmap = pixmap_[L].data();
    const uint8_t *const fl = flags_[L].data();
    const int x0 = std::max(minx, 0), x1 = std::min(maxx, dw_ - 1);
    for (int y = std::max(miny, 0); y <= std::min(maxy, H - 1); y++) {
        if (line_filter_ && !line_filter_[y]) continue;                         // not composed again (compose16)
        if (row_empty(L, uint32_t(y + sy), cat, flags & DRAW_OPAQUE)) continue; // no pixel of this row can match
        const size_t row = size_t((y + sy) & 511) * 512;
        M2_PIXEL *const out = dm.data() + size_t(y) * size_t(dw_);
        for (int x = x0; x <= x1;) {
            const int from = (x + sx) & 511, n = std::min(x1 - x + 1, 512 - from);
            const uint16_t *p = pixmap + row + size_t(from);
            const uint8_t *f = fl + row + size_t(from);
            M2_PIXEL *o = out + x;
            // A tile (8 pixels of this row) at a time, by what build_layer
            // recorded: one of another category, or with no opaque pixel on
            // a pass that needs one, draws nothing; one whose pixels all
            // match (the opaque pass, or every pixel opaque) needs no test.
            const uint8_t *const classes = tile_class_[L] + (row >> 12) * 64;
            for (int k = 0; k < n;) {
                const int col = from + k, end = std::min(n, k + 8 - (col & 7));
                const uint8_t cls = classes[col >> 3];
                if ((cls & 1) == cat) {
                    const bool all = flags & DRAW_OPAQUE || cls & 4;
                    if (end - k == 8) { // a whole tile: fixed length, unrolled
                        if (all)
                            for (int j = 0; j < 8; j++) o[k + j] = M2_PEN[p[k + j]];
                        else if (cls & 2)
                            for (int j = 0; j < 8; j++)
                                if ((f[k + j] & mask) == value) o[k + j] = M2_PEN[p[k + j]];
                    } else if (all) {
                        for (int j = k; j < end; j++) o[j] = M2_PEN[p[j]];
                    } else if (cls & 2) {
                        for (int j = k; j < end; j++)
                            if ((f[j] & mask) == value) o[j] = M2_PEN[p[j]];
                    }
                }
                k = end;
            }
            x += n;
        }
    }
#else
    for (int y = std::max(miny, 0); y <= std::min(maxy, H - 1); y++)
        for (int x = std::max(minx, 0); x <= std::min(maxx, dw_ - 1); x++) {
            const size_t i = size_t((y + sy) & 511) * 512 + size_t((x + sx) & 511);
            if ((flags_[L][i] & mask) == value) dm[size_t(y) * size_t(dw_) + size_t(x)] = M2_PEN[pixmap_[L][i]];
        }
#endif
}

// segaic24 draw_common for the rgb32 bitmap, cliprect = the whole screen.
M2_PIXEL_TEMPLATE void Video::draw(std::vector<M2_PIXEL> &bitmap, int layer, int flags M2_PENS) {
    uint16_t hscr = tile(0x5000 + uint32_t(layer >> 1));
    uint16_t vscr = tile(0x5004 + uint32_t(layer >> 1));
    const uint16_t ctrl = tile(0x5004 + uint32_t((layer >> 1) & 2));
#ifdef M2_DC_SPEED
    const uint32_t mask_base = layer & 4 ? 0x6800 : 0x6000; // (the mask is read below, where it is used)
#else
    uint16_t mask[0x800];
    for (uint32_t i = 0; i < 0x800; i++) mask[i] = tile((layer & 4 ? 0x6800 : 0x6000) + i);
#endif
    const uint16_t tpri = uint16_t(layer & 1);
    layer >>= 1;
    const int fl = tpri | flags;

    if (vscr & 0x8000) return; // layer disable

    if (ctrl & 0x6000) { // special window/scroll modes
        if (layer & 1) return;
        const int sy = vscr & 0x1ff;
        if (hscr & 0x8000) {
            const uint32_t hscrtb = 0x4000 + 0x200 * uint32_t(layer);
            switch ((ctrl & 0x6000) >> 13) {
            case 1: {
                const uint16_t v = uint16_t((-vscr) & 0x1ff);
                if (!((-vscr) & 0x200)) layer ^= 1;
                for (int y = 0; y < H; y++) {
                    const int l1 = y >= v ? layer ^ 1 : layer;
                    const uint16_t h = tile(hscrtb + uint32_t(y)) & 0x1ff;
                    tilemap_draw(bitmap, l1, -h, sy, 0, dw_ - 1, y, y, fl M2_PENS_ARG);
                }
                break;
            }
            case 2:
            case 3:
                for (int y = 0; y < H; y++) {
                    hscr = tile(hscrtb + uint32_t(y));
                    const int h = hscr & 0x1ff;
                    int l1 = layer;
                    if (!(hscr & 0x200)) l1 ^= 1;
                    tilemap_draw(bitmap, l1, -h, sy, 0, std::min(dw_ - 1, h - 1), y, y, fl M2_PENS_ARG);
                    tilemap_draw(bitmap, l1 ^ 1, -h, sy, std::max(0, h), dw_ - 1, y, y, fl M2_PENS_ARG);
                }
                break;
            }
        } else {
            const int sx = -(hscr & 0x1ff);
            switch ((ctrl & 0x6000) >> 13) {
            case 1: {
                const int v = (-vscr) & 0x1ff;
                if (!((-vscr) & 0x200)) layer ^= 1;
                tilemap_draw(bitmap, layer, sx, sy, 0, dw_ - 1, 0, std::min(H - 1, v - 1), fl M2_PENS_ARG);
                tilemap_draw(bitmap, layer ^ 1, sx, sy, 0, dw_ - 1, std::max(0, v), H - 1, fl M2_PENS_ARG);
                break;
            }
            case 2:
            case 3: {
                const int h = hscr & 0x1ff;
                if (!(hscr & 0x200)) layer ^= 1;
                tilemap_draw(bitmap, layer, sx, sy, 0, std::min(dw_ - 1, h - 1), 0, H - 1, fl M2_PENS_ARG);
                tilemap_draw(bitmap, layer ^ 1, sx, sy, std::max(0, h), dw_ - 1, 0, H - 1, fl M2_PENS_ARG);
                break;
            }
            }
        }
        return;
    }

    const int win = layer & 1;
#ifdef M2_DC_SPEED
    // The window mask, only for this path (disabled layers and the special
    // modes above never read it).
    uint16_t mask[0x800];
    for (uint32_t i = 0; i < 0x800; i++) mask[i] = tile(mask_base + i);
#endif
    if (hscr & 0x8000) {
        const uint32_t hscrtb = 0x4000 + 0x200 * uint32_t(layer);
        vscr &= 0x1ff;
        for (int y = 0; y < 384; y++) {
            hscr = uint16_t((-tile(hscrtb + uint32_t(y))) & 0x1ff);
            if (hscr + dw_ <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, y, dw_, y + 1 M2_PENS_ARG);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, y, 512 - hscr, y + 1 M2_PENS_ARG);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, y, dw_, y + 1 M2_PENS_ARG);
            }
            vscr = (vscr + 1) & 0x1ff;
        }
    } else {
        hscr = uint16_t((-hscr) & 0x1ff);
        vscr = uint16_t((+vscr) & 0x1ff);
        if (hscr + dw_ <= 512) {
            if (vscr + 384 <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, dw_, 384 M2_PENS_ARG);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, dw_, 512 - vscr M2_PENS_ARG);
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, 0, 0, 512 - vscr, dw_, 384 M2_PENS_ARG);
            }
        } else {
            if (vscr + 384 <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, 512 - hscr, 384 M2_PENS_ARG);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, 0, dw_, 384 M2_PENS_ARG);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, 512 - hscr, 512 - vscr M2_PENS_ARG);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, 0, dw_, 512 - vscr M2_PENS_ARG);
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, 0, 0, 512 - vscr, 512 - hscr, 384 M2_PENS_ARG);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, 0, 512 - hscr, 512 - vscr, dw_, 384 M2_PENS_ARG);
            }
        }
    }
}

bool Video::system24_gpu_compatible() const {
    if (margin_) return false; // Wide HUD uses the shared per-item compositor.
    // The Vita GXM compositor supports normal windowing plus all three
    // System24 split-layer modes. Keep this query for the CPU fallback API.
    return true;
}

const std::vector<GeoPoly> &Video::gpu_polys() const {
    static const std::vector<GeoPoly> empty;
    return gpu_polys_ ? *gpu_polys_ : empty;
}

#ifdef M2_DC_SPEED
// Whether the back layers' opaque passes (layers 3 and 2, draw() with
// DRAW_OPAQUE) draw pixmap layer 2 alone, on every line with one scroll:
// layer 2 enabled, its mode word (0x5006, also layer 2's vertical scroll)
// split mode 1 with every line on layer 2 (the split line off the screen),
// no line scroll. Then layer 3's draw() returns at once (split mode, odd
// layer) and layer 2's is tilemap_draw(2, sx, sy) for every line, which is
// what back_scrolled() describes.
bool Video::scroll_mode(int &sx, int &sy) const {
    const uint16_t hscr = tile(0x5002), vscr = tile(0x5006);
    if (!scroll_allowed_) return false;
    if (vscr & 0x8000) return false;                  // layer 2 disabled
    if (((vscr & 0x6000) >> 13) != 1) return false;   // not split mode 1
    if (hscr & 0x8000) return false;                  // line scroll
    const int v = (-vscr) & 0x1ff;
    const int first = (-vscr) & 0x200 ? 2 : 3;        // the layer above the split line
    const bool all_layer2 = (v >= H && first == 2) || (v == 0 && first == 3);
    if (!all_layer2) return false;
    sx = -(hscr & 0x1ff);
    sy = vscr & 0x1ff;
    return true;
}

// One of the Dreamcast's 16-bit layer buffers (cat 0: the back layers over
// pen 0; 1: the front layers over see-through), kLayerStride wide. Every
// line when its full flag is set; otherwise only the lines that show a row
// of tiles rebuilt since (screen line y shows pixmap row (y + vscroll) & 511
// of the layer, or of its pair in the split modes: draw()'s every path),
// and those in extra_lines (line-scroll entries changed; may be null),
// cleared and drawn again: the same pixels as composing all of it.
void Video::compose16(int cat, const bool uses[4], const uint8_t *extra_lines) {
    bool &dirty = cat ? front_dirty_ : back_dirty_;
    bool &full = cat ? front_full_ : back_full_;
    std::vector<uint16_t> &buffer = cat ? sys24_16_ : screen16_;
    // (The back layers with the scrolled layer drawn by the frontend: only
    // layers 1 and 0, over see-through.)
    const bool scrolled = !cat && back_scroll_;
    const uint16_t clear = cat || scrolled ? uint16_t(0) : pens565_[0];
    const int layers = scrolled ? 2 : 4; // pixmap layers drawn: 0 to layers - 1
    if (dirty || full || extra_lines) {
        uint8_t lines[H];
        int count = H;
        if (!full) {
            uint32_t vscroll[4];
            for (uint32_t l = 0; l < 4; ++l) vscroll[l] = tile(0x5004 + l);
            count = 0;
            for (int y = 0; y < H; ++y) {
                uint8_t d = extra_lines ? extra_lines[y] : 0;
                for (int l = 0; l < layers && !d; ++l) {
                    const uint32_t r = ((uint32_t(y) + vscroll[l]) & 511) >> 3;
                    d = dirty_rows_[cat][l][r] | dirty_rows_[cat][l ^ 1][r];
                }
                lines[y] = d;
                count += d;
            }
        }
        if (count) {
            if (full) {
                std::fill(buffer.begin(), buffer.end(), clear);
            } else {
                for (int y = 0; y < H; ++y)
                    if (lines[y]) std::fill_n(buffer.data() + size_t(y) * kLayerStride, kLayerStride, clear);
                line_filter_ = lines;
            }
            const int dw = dw_;
            dw_ = kLayerStride; // composed kLayerStride wide: the rows are the texture's
            if (cat) {
                for (int layer = 3; layer >= 0; --layer) draw(buffer, (layer << 1) | 1, 0, pens1555_);
                ++front16_generation_;
            } else if (scrolled) {
                for (int layer = 1; layer >= 0; --layer) draw(buffer, layer << 1, 0, pens1555_);
                ++back16_generation_;
            } else {
                for (int layer = 3; layer >= 2; --layer) draw(buffer, layer << 1, DRAW_OPAQUE, pens565_);
                for (int layer = 1; layer >= 0; --layer) draw(buffer, layer << 1, 0, pens565_);
                ++back16_generation_;
            }
            dw_ = dw;
            line_filter_ = nullptr;
            ++composes_[cat];
            if (full) ++full_composes_[cat];
            composed_lines_[cat] += uint64_t(count);
        }
    }
    dirty = full = false;
    std::memset(dirty_rows_[cat], 0, sizeof dirty_rows_[cat]);
    for (int l = 0; l < 4; ++l) composed_uses_[cat][l] = uses[l];
}
#endif

void Video::screen_update(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem) {
    gpu_polys_ = &polys;
    has_3d_ = !polys.empty();
    poly_count_ = polys.size();
    gpu_windows_ = windows;
    gpu_mem_ = mem;
    profile_ = {};
    uint64_t before = ticks();
    // Retain the reference's sticky palette-dirty behavior. palette_w marks
    // cached composition dirty only if the resulting RGB value really changed.
    if (palette_dirty_) {
        for (uint32_t i = 0; i < 0x1000; i++) palette_w(i, mem.palram, mem.colorxlat);
        palette_dirty_ = false;
    }
#ifdef M2_VITA_RENDER_OPT
    update_tile_cache();
#else
    decode_layers();
#endif
    if (system24_source_dirty_) {
        ++system24_texture_generation_;
        system24_source_dirty_ = false;
    }
    profile_.tile_cache = ticks() - before;
#ifndef M2_VITA_RENDER_OPT
    if (external_3d_ && desktop_) {
        // Desktop hardware renderer: it draws the tilemap layers from the
        // pixmaps, with this frame's registers and pens (the game may write
        // them again before the frame is drawn). The CPU decides what the
        // composition needs: how to fill the widescreen margins (the 3D
        // coverage, estimated from the polygons: no CPU 3D layer here) and
        // whether the HUD moves to the edges, which it does itself (the
        // front layers drawn here; the condition panel's polygons move on
        // the GPU, gpu_hud_shift).
        rendered_now_ = false;
        for (uint32_t i = 0; i < kGpuTileWords; ++i) gpu_tile_words_[i] = tile(kGpuTileFirst + i);
        std::copy_n(pens_, kGpuPens, gpu_pens_.data());
        if (margin_) coverage_ = polys.empty() ? 0 : raster_.coverage_estimate(polys, windows, crtc_x_ + margin_, crtc_y_);
        hud_on_ = margin_ && hud_edges_ && raster_.find_race_hud(polys, crtc_x_ + margin_, crtc_y_);
        if (hud_on_) {
            before = ticks();
            std::fill(sys24_.begin(), sys24_.end(), 0u);
            for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
            foreground_gpu_.assign(size_t(width()) * H, 0u);
            copy_front_hud_to_edges(foreground_gpu_);
            ++foreground_generation_;
            profile_.tile_draw += ticks() - before;
        }
        return;
    }
#endif
#ifndef M2_DC_MEMORY
    // (Not the Dreamcast: its PVR renderer uploads the two layers this
    // function composes below, the screen and sys24.)
    if (external_3d_ && !desktop_ && system24_gpu_compatible()) {
        // GXM composes the cached System-24 tile textures around the 3D
        // layer. Do not spend ~35 ms rebuilding CPU bitmaps for scrolling.
        rendered_now_ = false;
        return;
    }
#endif
#ifdef M2_DC_SPEED
    if (external_3d_ && !desktop_) {
        // The Dreamcast: the same two layers as below (back layers over pen
        // 0, front layers over 0 = see-through), composed in the PVR's
        // 16-bit formats.
        before = ticks();
        // Only a layer whose inputs changed since it was last composed (the
        // same inputs give the same pixels): scroll, line tables and window
        // masks live in tile RAM 0x8000-0xdfff, compared with a copy.
        // Per pixmap layer: draw() reads its scroll words (0x5000 + L,
        // 0x5004 + L), the mode word (0x5004 + (L & 2)), its window mask
        // (0x6000 or 0x6800) and, with line scroll on (0x5000 + L bit 15,
        // when the rest of that word is not read), its line-scroll table
        // (0x4000 + 0x200 L, a word for each line). A change to any of them
        // composes every line, except table entries in line-scroll mode:
        // only those lines (scroll_lines).
        constexpr size_t kRegs = 0x8000, kRegsSize = 0x6000;
        bool state_changed[4] = {}, lines_changed[4] = {};
        uint8_t scroll_lines[4][H];
        if (regs_copy_.empty()) {
            regs_copy_.assign(tile_ram_ + kRegs, tile_ram_ + kRegs + kRegsSize);
            for (bool &c : state_changed) c = true;
        } else if (std::memcmp(tile_ram_ + kRegs, regs_copy_.data(), kRegsSize) != 0) {
            auto old_word = [&](uint32_t word) { return le16(regs_copy_.data(), word - kRegs / 2); };
            auto differs = [&](uint32_t word, uint32_t words) {
                const size_t at = size_t(word) * 2;
                return std::memcmp(tile_ram_ + at, regs_copy_.data() + (at - kRegs), size_t(words) * 2) != 0;
            };
            for (uint32_t l = 0; l < 4; ++l) {
                const uint16_t h = tile(0x5000 + l), was = old_word(0x5000 + l);
                state_changed[l] = differs(0x5004 + l, 1) || differs(0x5004 + (l & 2), 1) ||
                                   differs(l & 2 ? 0x6800 : 0x6000, 0x800) || ((h ^ was) & 0x8000) ||
                                   (!(h & 0x8000) && h != was);
                if (!state_changed[l] && (h & 0x8000) && differs(0x4000 + 0x200 * l, H)) {
                    lines_changed[l] = true;
                    for (int y = 0; y < H; ++y)
                        scroll_lines[l][y] = tile(0x4000 + 0x200 * l + uint32_t(y)) != old_word(0x4000 + 0x200 * l + uint32_t(y));
                }
            }
            regs_copy_.assign(tile_ram_ + kRegs, tile_ram_ + kRegs + kRegsSize);
        }
        // The scrolled layer on or off (back_scrolled): every back line again
        // when that changes (the buffer's contents and format change).
        int sx = 0, sy = 0;
        const bool scroll = scroll_mode(sx, sy);
        if (scroll != back_scroll_) back_full_ = true;
        back_scroll_ = scroll;
        scroll_x_ = sx;
        scroll_y_ = sy;
        for (int cat = 0; cat < 2; ++cat) {
            // A layer draws its own tiles and, in the split modes (mode word
            // bits 13-14), its pair's. (Layers 2 and 3 are not in the back
            // buffer while the frontend draws the scrolled layer.)
            bool uses[4];
            for (int l = 0; l < 4; ++l)
                uses[l] = !(scroll && !cat && l >= 2) &&
                          (layer_has(l, cat) || ((tile(0x5004 + uint32_t(l & 2)) & 0x6000) && layer_has(l ^ 1, cat)));
            uint8_t extra[H] = {};
            bool any_extra = false;
            for (int l = 0; l < 4; ++l) {
                if (!(uses[l] || composed_uses_[cat][l])) continue;
                if (state_changed[l]) (cat ? front_full_ : back_full_) = true;
                if (lines_changed[l]) {
                    any_extra = true;
                    for (int y = 0; y < H; ++y) extra[y] |= scroll_lines[l][y];
                }
            }
            compose16(cat, uses, any_extra ? extra : nullptr);
        }
        profile_.tile_draw += ticks() - before;
        profile_.layers_rebuilt = true;
        rendered_now_ = false;
        return;
    }
    if (screen_.empty()) { // a frame without external 3D (not the Dreamcast frontend's)
        screen_.assign(size_t(width()) * H, 0u);
        sys24_.assign(size_t(W) * (H + 4), 0u);
    }
#endif
    // Non-zero pixels of a `width`-wide source onto the screen at column `at`.
    const size_t out_w = size_t(width());
    auto copy_trans = [&](const uint32_t *source, size_t stride, int width = W, int at = 0) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < width; ++x)
                if (const uint32_t pixel = source[size_t(y) * stride + size_t(x)])
                    screen_[size_t(y) * out_w + size_t(at + x)] = pixel;
    };
#ifdef M2_VITA_RENDER_OPT
    before = ticks();
    if (external_3d_ && margin_)
        hud_on_ = hud_edges_ && raster_.find_race_hud(polys, crtc_x_ + margin_, crtc_y_);
    const bool rebuild_background = background_dirty_ && !gpu_background();
    if (rebuild_background) {
        // All tile writes are replacements, not blends. Drawing the back
        // layers over pen 0 is identical to zero + transparent copy over pen 0.
        std::fill(background_.begin(), background_.end(), pens_[0]);
        for (int layer = 3; layer >= 2; --layer) draw(background_, layer << 1, DRAW_OPAQUE);
        for (int layer = 1; layer >= 0; --layer) draw(background_, layer << 1, 0);
        background_dirty_ = false;
        ++background_generation_;
        profile_.layers_rebuilt = true;
    }
    if (foreground_dirty_ && !gpu_foreground()) {
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
        foreground_dirty_ = false;
        ++foreground_generation_;
        profile_.layers_rebuilt = true;
    }
    profile_.tile_draw = ticks() - before;
    before = ticks();
    if (external_3d_ && margin_) {
        // Keep the backdrop native-sized: scaling and plain sky margins are
        // cheap 2D GPU draws, not a CPU widescreen bitmap per frame.
        if (!gpu_background() && (rebuild_background || background_gpu_.size() != size_t(W) * H))
            background_gpu_.assign(background_.data(), background_.data() + size_t(W) * H);
        set_raster_hud_moves();
        if (gpu_foreground()) {
            // Do not build, compare or upload a CPU front bitmap. Leave the
            // dirty flag set so a later HUD-edge frame reconstructs it.
            profile_.composite += ticks() - before;
            rendered_now_ = false;
            return;
        }
        if (gpu_front_margin_ != margin_ || gpu_front_hud_ != hud_on_ || gpu_front_source_ != sys24_) {
            std::fill(screen_.begin(), screen_.end(), 0u);
            if (hud_on_) copy_front_hud_to_edges(screen_);
            else copy_trans(sys24_.data(), W, W, margin_);
            foreground_gpu_ = screen_;
            gpu_front_source_ = sys24_;
            gpu_front_margin_ = margin_;
            gpu_front_hud_ = hud_on_;
            ++foreground_generation_;
        }
        profile_.composite += ticks() - before;
        rendered_now_ = false;
        return;
    }
    if (!margin_) std::copy_n(background_.data(), screen_.size(), screen_.data());
    else {
        std::fill(screen_.begin(), screen_.end(), background_[0]);
        copy_trans(background_.data(), W, W, margin_);
    }
    profile_.composite += ticks() - before;
#else
    before = ticks();
    std::fill(screen_.begin(), screen_.end(), pens_[0]);
#ifdef M2_DC_SPEED
    // As the Vita's path: the back layers drawn straight over pen 0, not
    // into a cleared sys24_ copied over it (no margins: the same stride).
    if (!margin_) {
        for (int layer = 3; layer >= 2; --layer) draw(screen_, layer << 1, DRAW_OPAQUE);
        for (int layer = 1; layer >= 0; --layer) draw(screen_, layer << 1, 0);
        profile_.tile_draw += ticks() - before;
        profile_.layers_rebuilt = true;
    } else
#endif
    {
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 2; --layer) draw(sys24_, layer << 1, DRAW_OPAQUE);
        for (int layer = 1; layer >= 0; --layer) draw(sys24_, layer << 1, 0);
        profile_.tile_draw += ticks() - before;
        profile_.layers_rebuilt = true;
        before = ticks();
        copy_trans(sys24_.data(), W, W, margin_);
        profile_.composite += ticks() - before;
    }
#endif
    rendered_now_ = false;
    if (external_3d_) {
        // Save the exact two System-24 layers separately. The Vita frontend
        // draws background -> GPU 3D -> foreground. No CPU polygon pixels are
        // produced in this mode, so raster_ms should remain zero.
#ifndef M2_DC_MEMORY
        // (The Dreamcast reads screen_ and sys24_ themselves: no copies.)
        std::copy_n(screen_.data(), screen_.size(), background_gpu_.data());
#endif
#ifndef M2_VITA_RENDER_OPT
        // Reference path has not drawn the post-3D tile pass yet.
        before = ticks();
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
        profile_.tile_draw += ticks() - before;
#endif
#ifndef M2_DC_MEMORY
        if (margin_) {
            hud_on_ = hud_edges_ && raster_.find_race_hud(polys, crtc_x_ + margin_, crtc_y_);
            set_raster_hud_moves();
            std::fill(screen_.begin(), screen_.end(), 0u);
            if (hud_on_) copy_front_hud_to_edges(screen_);
            else copy_trans(sys24_.data(), W, W, margin_);
            foreground_gpu_ = screen_;
            ++background_generation_; ++foreground_generation_;
        } else {
            std::fill(foreground_gpu_.begin(), foreground_gpu_.end(), 0u);
            std::copy_n(sys24_.data(), std::min(sys24_.size(), foreground_gpu_.size()), foreground_gpu_.data());
        }
#endif
        return;
    }
    // Widescreen, HUD at the edges: the front tilemaps are drawn first, to
    // decide which HUD groups move; 3D windows inside a moved group go with it.
    const bool hud_edges = margin_ && hud_edges_;
    if (hud_edges) {
        before = ticks();
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
        profile_.tile_draw += ticks() - before;
        // Only while the race HUD is on screen (its condition panel's box).
        const bool race_hud = raster_.find_race_hud(polys, crtc_x_ + margin_, crtc_y_);
        if (race_hud != hud_on_) { hud_on_ = race_hud; set_raster_hud_moves(); render_done_ = false; }
    }
    if (!render_done_ && !polys.empty()) {
        before = ticks();
        raster_.render(polys, windows, mem, crtc_x_ + margin_, crtc_y_, render_x_ + margin_, render_y_, 0,
                       width() - 1, 0, H - 1);
        profile_.raster = ticks() - before;
        if (margin_) { // widescreen: how much of the original screen the 3D layer covers
            size_t covered = 0;
            for (int y = 0; y < H; ++y) {
                const uint32_t *row = raster_.pixels() + size_t(y) * size_t(raster_.stride()) + size_t(margin_);
                for (int x = 0; x < W; ++x) covered += row[x] != 0;
            }
            coverage_ = int(covered * 100 / (size_t(W) * H));
        }
        render_done_ = true;
        rendered_now_ = true;
    }
    before = ticks();
    if (margin_) {
        if (!render_done_) coverage_ = 0; // no 3D this frame: a 2D screen
        fill_margins();
    }
    if (render_done_) copy_trans(raster_.pixels(), size_t(raster_.stride()), width());
    profile_.composite += ticks() - before;
#ifndef M2_VITA_RENDER_OPT
    if (!hud_edges) {
        before = ticks();
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
        profile_.tile_draw += ticks() - before;
    }
#endif
    before = ticks();
    if (hud_edges && hud_on_) {
        copy_front_hud_to_edges(screen_);
    } else {
        copy_trans(sys24_.data(), W, W, margin_);
    }
    profile_.composite += ticks() - before;
}

// Widescreen, HUD at the edges: the race HUD's side groups (lap and lap
// times at the top left; position, condition panel and course map on the
// right) move out by the margin; the centre (speed, speedometer) stays.
// The front tilemaps also carry banners that scroll through those areas
// ("ROLLING START"), so what moves is decided per item, not per area: the
// front layers' pixels are grouped into blobs (pixels within kHudJoin of
// each other join, so a banner's letters and backing are one blob), and a
// blob moves only if it lies wholly inside a group. A banner crossing a
// group's edge stays put and is never torn, and nothing flips frame to
// frame as its letters pass. Rectangles in 496-wide coordinates.
namespace {
struct HudGroup { int x0, x1, y0, y1, side; };
constexpr HudGroup kHudGroups[2] = {{0, 125, 0, 130, -1},   // lap, lap times
                                    {352, 496, 0, 300, 1}}; // position ("40TH" reaches x 367), condition, course map
constexpr int kHudJoin = 4;
} // namespace

void Video::set_raster_hud_moves() {
    // The condition panel's overlay quads go with the right-hand group.
    raster_.set_hud_shift(hud_on_ ? kHudGroups[1].side * margin_ : 0);
}

void Video::copy_front_hud_to_edges(std::vector<uint32_t> &out) {
    const size_t n = size_t(W) * H;
    // Pixels present, widened by kHudJoin in x then y (a square neighbourhood).
    hud_mask_.assign(n, 0);
    hud_tmp_.assign(n, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (sys24_[size_t(y) * W + size_t(x)])
                for (int d = std::max(0, x - kHudJoin); d <= std::min(W - 1, x + kHudJoin); ++d)
                    hud_tmp_[size_t(y) * W + size_t(d)] = 1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (hud_tmp_[size_t(y) * W + size_t(x)])
                for (int d = std::max(0, y - kHudJoin); d <= std::min(H - 1, y + kHudJoin); ++d)
                    hud_mask_[size_t(d) * W + size_t(x)] = 1;
    // Label the widened blobs; bound each by its real pixels.
    hud_label_.assign(n, -1);
    hud_box_.clear();
    std::vector<uint32_t> &stack = hud_stack_;
    for (size_t start = 0; start < n; ++start) {
        if (!hud_mask_[start] || hud_label_[start] >= 0) continue;
        const int label = int(hud_box_.size());
        hud_box_.push_back({W, 0, H, 0});
        stack.assign(1, uint32_t(start));
        hud_label_[start] = label;
        while (!stack.empty()) {
            const uint32_t i = stack.back();
            stack.pop_back();
            const int x = int(i % W), y = int(i / W);
            if (sys24_[i]) {
                auto &b = hud_box_[size_t(label)];
                b[0] = std::min(b[0], x), b[1] = std::max(b[1], x + 1);
                b[2] = std::min(b[2], y), b[3] = std::max(b[3], y + 1);
            }
            const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
            for (int k = 0; k < 4; ++k) {
                if (nx[k] < 0 || nx[k] >= W || ny[k] < 0 || ny[k] >= H) continue;
                const uint32_t j = uint32_t(ny[k]) * W + uint32_t(nx[k]);
                if (hud_mask_[j] && hud_label_[j] < 0) hud_label_[j] = label, stack.push_back(j);
            }
        }
    }
    // Each blob's move: its group's, if it lies wholly inside one.
    hud_move_.assign(hud_box_.size(), 0);
    for (size_t l = 0; l < hud_box_.size(); ++l)
        for (const HudGroup &G : kHudGroups) {
            const auto &b = hud_box_[l];
            if (b[0] >= G.x0 && b[1] <= G.x1 && b[2] >= G.y0 && b[3] <= G.y1) hud_move_[l] = G.side * margin_;
        }
    const size_t out_w = size_t(width());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + size_t(x);
            if (const uint32_t pixel = sys24_[i])
                out[size_t(y) * out_w + size_t(margin_ + x + hud_move_[size_t(hud_label_[i])])] = pixel;
        }
}

// Widescreen side margins, under the 3D layer. On a 2D screen (car and
// circuit select, titles: see scene()) each row carries its own edge colours
// out: the art covers only 496 columns. Behind a 3D scene (the race, the
// attract's camera shots) the margins are the sky's plain colour (the back layers'
// top-left pixel, open sky), or with "stretch tile background" the backdrop
// as drawn for the 496 columns is stretched across the whole width, never
// repeated: the race sky is one 512-pixel layer whose ends do not meet, so
// drawing it further (tried, also with split pairs) showed a seam.
void Video::fill_margins() {
    const bool scene = this->scene();
    const int out = width();
    const uint32_t sky = screen_[size_t(margin_)];
    if (scene && stretch_backdrop_) {
        stretch_row_.resize(size_t(W));
        for (int y = 0; y < H; ++y) {
            uint32_t *row = &screen_[size_t(y) * size_t(out)];
            std::copy_n(row + margin_, W, stretch_row_.data());
            for (int x = 0; x < out; ++x) {
                // out column x samples backdrop column (x + 0.5) * W / out - 0.5, blended
                const float u = std::clamp((float(x) + 0.5f) * float(W) / float(out) - 0.5f, 0.0f, float(W - 1));
                const int i = int(u), j = std::min(i + 1, W - 1);
                const float f = u - float(i);
                const uint32_t a = stretch_row_[size_t(i)], b = stretch_row_[size_t(j)];
                uint32_t p = 0;
                for (int k = 0; k < 24; k += 8) {
                    const float c = float((a >> k) & 0xff) * (1.0f - f) + float((b >> k) & 0xff) * f;
                    p |= uint32_t(c + 0.5f) << k;
                }
                row[x] = p | (a & 0xff000000u);
            }
        }
        return;
    }
    for (int y = 0; y < H; ++y) {
        uint32_t *row = &screen_[size_t(y) * size_t(out)];
        const uint32_t left = scene ? sky : row[margin_], right = scene ? sky : row[margin_ + W - 1];
        std::fill(row, row + margin_, left ? left : pens_[0]);
        std::fill(row + margin_ + W, row + out, right ? right : pens_[0]);
    }
}

void Video::set_wide_margin(int margin) {
#ifdef M2_VITA_RENDER_OPT
    margin = std::clamp(margin, 0, 200);
#else
    if (external_3d_ && !desktop_) margin = 0;
    margin = std::max(margin, 0);
#endif
    if (margin == margin_) return;
    margin_ = margin;
    set_raster_hud_moves();
    screen_.assign(size_t(width()) * H, 0u);
#ifndef M2_DC_MEMORY
    background_gpu_.assign(screen_.size(), 0u);
    foreground_gpu_.assign(screen_.size(), 0u);
#endif
    ++background_generation_; ++foreground_generation_;
    raster_.set_wide_margin(margin_);
    render_done_ = false; // redraw the 3D layer at the new width
}

uint64_t Video::screen_hash() const {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t px : screen_)
        for (int b = 0; b < 4; b++) {
            h ^= (px >> (8 * b)) & 0xff;
            h *= 0x100000001b3ULL;
        }
    return h;
}

} // namespace rt
