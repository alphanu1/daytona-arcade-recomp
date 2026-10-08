#pragma once

#include "runtime/video.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace vita {

// System 24 tile textures, two forms:
//   * colour (upload_system24_layer, libvita2d path): each texel is the pen's
//     RGBA colour. A palette change recolours every tile: all 4096 tiles of
//     the 4 layers are rewritten (16384 tiles, 8 MB; ~67 ms on the Vita).
//   * index (upload_system24_layer_indices, vitaGL path): each texel is the
//     pen NUMBER; the fragment shader looks its colour up in a 128x64 palette
//     texture (upload_system24_palette, 8192 pens, 32 KB). A palette change
//     rewrites only that texture; tiles are rewritten only when their pixels
//     or categories change.
// Both write only tiles changed since the last presented source generation.
// The caller must finish any previous GPU readers before writing them. Kept
// independent of GXM so tests exercise the exact Vita upload conversion.

// Index texel (RGBA8, bytes r g b a) of 13-bit pen p, already in palette texture
// units: r = (p % 128) * 2 (column), g = (p / 128) * 4 (row), a = 255 when visible;
// 0 (alpha 0, discarded by the alpha test) when not. The shader reads the column
// with floor(r * 127.5 + 0.01) and the row with floor(g * 63.75 + 0.01): the same
// form as the Model 2 palette lookup the console's shader compiler already takes.
inline uint32_t system24_index_texel(uint16_t pen, bool visible) {
    const uint32_t p = uint32_t(pen) & 0x1fffu;
    return visible ? 0xff000000u | ((p & 127u) << 1) | ((p >> 7) << 10) : 0u;
}

inline uint32_t system24_argb_to_rgba(uint32_t argb) {
    return (argb & 0xff00ff00u) | ((argb & 0xffu) << 16u) | ((argb >> 16u) & 0xffu);
}

template <bool kIndices>
inline unsigned upload_system24_layer_impl(const rt::Video &video, int layer, uint64_t previous,
                                           void *background, size_t background_stride,
                                           void *foreground, size_t foreground_stride) {
    constexpr size_t row_bytes = 512u * sizeof(uint32_t);
    if (!background || !foreground || background_stride < row_bytes || foreground_stride < row_bytes ||
        (background_stride % alignof(uint32_t)) || (foreground_stride % alignof(uint32_t))) return 0;
    // Index form: colours are not in the tiles, a palette change rewrites nothing here.
    const bool full = previous == UINT64_MAX || (!kIndices && video.system24_palette_generation() > previous);
    const bool opaque_background = layer >= 2;
    const bool split_background = opaque_background && (video.system24_word(0x5006) & 0x6000);
    const uint16_t *pixels = video.system24_pixels(layer);
    const uint8_t *flags = video.system24_flags(layer);
    auto *back = static_cast<uint8_t *>(background);
    auto *front = static_cast<uint8_t *>(foreground);
    unsigned uploaded = 0;
    for (unsigned tile = 0; tile < 4096; ++tile) {
        if (!full && video.system24_tile_generation(layer, tile) <= previous) continue;
        const unsigned tx = (tile & 63u) * 8u, ty = (tile >> 6u) * 8u;
        for (unsigned y = ty; y < ty + 8u; ++y) {
            auto *back_row = reinterpret_cast<uint32_t *>(back + size_t(y) * background_stride);
            auto *front_row = reinterpret_cast<uint32_t *>(front + size_t(y) * foreground_stride);
            for (unsigned x = tx; x < tx + 8u; ++x) {
                const size_t i = size_t(y) * 512u + x;
                const bool opaque = (flags[i] & 0x10) != 0;
                const bool category1 = (flags[i] & 1) != 0;
                // Unlike normal-mode draw_rect, split-mode tilemap_draw
                // retains the category test even with DRAW_OPAQUE.
                const bool back_visible = opaque_background ? (!split_background || !category1) :
                                                              (opaque && !category1);
                const bool front_visible = opaque && category1;
                if constexpr (kIndices) {
                    back_row[x] = system24_index_texel(pixels[i], back_visible);
                    front_row[x] = system24_index_texel(pixels[i], front_visible);
                } else {
                    const uint32_t rgba = system24_argb_to_rgba(video.system24_pen(pixels[i]));
                    back_row[x] = back_visible ? rgba : 0u;
                    front_row[x] = front_visible ? rgba : 0u;
                }
            }
        }
        ++uploaded;
    }
    return uploaded;
}

inline unsigned upload_system24_layer(const rt::Video &video, int layer, uint64_t previous,
                                      void *background, size_t background_stride,
                                      void *foreground, size_t foreground_stride) {
    return upload_system24_layer_impl<false>(video, layer, previous, background, background_stride, foreground,
                                             foreground_stride);
}

namespace detail {

// One run of index texels (count is a multiple of 8): back/front rows of pen numbers.
// back visible  = (opaque | A) & (~category1 | B), front visible = opaque & category1, with
//   normal layer: A = B = 0 (opaque, not category 1); opaque background: A = ~0, and
//   B = ~0 (normal mode: always) or 0 (split mode: not category 1).
inline void system24_index_run(const uint16_t *pixels, const uint8_t *flags, unsigned count,
                               uint32_t *back, uint32_t *front, uint32_t a, uint32_t b) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(DAYTONA_TEST_NEON)
    const uint16x8_t a16 = vdupq_n_u16(uint16_t(a)), b16 = vdupq_n_u16(uint16_t(b));
    const uint16x8_t pen_mask = vdupq_n_u16(0x1fff), column_mask = vdupq_n_u16(127);
    const uint16x8_t opaque_bit = vdupq_n_u16(0x10), category_bit = vdupq_n_u16(1);
    const uint32x4_t alpha = vdupq_n_u32(0xff000000u);
    for (unsigned x = 0; x < count; x += 8) {
        const uint16x8_t p = vandq_u16(vld1q_u16(pixels + x), pen_mask);
        const uint16x8_t lo = vorrq_u16(vshlq_n_u16(vandq_u16(p, column_mask), 1), vshlq_n_u16(vshrq_n_u16(p, 7), 10));
        const uint16x8_t f = vmovl_u8(vld1_u8(flags + x));
        const uint16x8_t opaque = vtstq_u16(f, opaque_bit), category1 = vtstq_u16(f, category_bit);
        const uint16x8_t front_mask = vandq_u16(opaque, category1);
        const uint16x8_t back_mask = vandq_u16(vorrq_u16(opaque, a16), vorrq_u16(vmvnq_u16(category1), b16));
        const uint32x4_t lo0 = vorrq_u32(vmovl_u16(vget_low_u16(lo)), alpha);
        const uint32x4_t lo1 = vorrq_u32(vmovl_u16(vget_high_u16(lo)), alpha);
        // 16-bit all-ones / zero masks widened by sign extension.
        const uint32x4_t b0 = vreinterpretq_u32_s32(vmovl_s16(vreinterpret_s16_u16(vget_low_u16(back_mask))));
        const uint32x4_t b1 = vreinterpretq_u32_s32(vmovl_s16(vreinterpret_s16_u16(vget_high_u16(back_mask))));
        const uint32x4_t f0 = vreinterpretq_u32_s32(vmovl_s16(vreinterpret_s16_u16(vget_low_u16(front_mask))));
        const uint32x4_t f1 = vreinterpretq_u32_s32(vmovl_s16(vreinterpret_s16_u16(vget_high_u16(front_mask))));
        vst1q_u32(back + x, vandq_u32(lo0, b0));
        vst1q_u32(back + x + 4, vandq_u32(lo1, b1));
        vst1q_u32(front + x, vandq_u32(lo0, f0));
        vst1q_u32(front + x + 4, vandq_u32(lo1, f1));
    }
#else
    for (unsigned x = 0; x < count; ++x) {
        const uint32_t p = uint32_t(pixels[x]) & 0x1fffu;
        const uint32_t texel = 0xff000000u | ((p & 127u) << 1) | ((p >> 7) << 10);
        const uint32_t f = flags[x];
        const uint32_t opaque = 0u - ((f >> 4) & 1u), category1 = 0u - (f & 1u);
        back[x] = texel & (opaque | a) & (~category1 | b);
        front[x] = texel & opaque & category1;
    }
#endif
}

} // namespace detail

// Index form, written by rows of tiles: in each row of 64 tiles the changed tiles are
// grouped into runs, then each of the 8 texel lines is written run by run. A full rewrite
// (scene change) writes whole 512-texel lines: long sequential stores, which is what the
// GPU memory (write-combined, not cached) takes best, instead of 8-texel pieces 2 KB
// apart. The texels are those of upload_system24_layer_impl<true> (tests compare both).
inline unsigned upload_system24_layer_indices(const rt::Video &video, int layer, uint64_t previous,
                                              void *background, size_t background_stride,
                                              void *foreground, size_t foreground_stride) {
    constexpr size_t row_bytes = 512u * sizeof(uint32_t);
    if (!background || !foreground || background_stride < row_bytes || foreground_stride < row_bytes ||
        (background_stride % alignof(uint32_t)) || (foreground_stride % alignof(uint32_t))) return 0;
    const bool full = previous == UINT64_MAX; // colours are not in the tiles: palettes change nothing here
    const bool opaque_background = layer >= 2;
    const bool split_background = opaque_background && (video.system24_word(0x5006) & 0x6000);
    const uint32_t a = opaque_background ? ~0u : 0u;
    const uint32_t b = opaque_background && !split_background ? ~0u : 0u;
    const uint16_t *pixels = video.system24_pixels(layer);
    const uint8_t *flags = video.system24_flags(layer);
    auto *back = static_cast<uint8_t *>(background);
    auto *front = static_cast<uint8_t *>(foreground);
    unsigned uploaded = 0;
    for (unsigned ty = 0; ty < 64; ++ty) {
        uint64_t dirty = ~uint64_t(0);
        if (!full) {
            dirty = 0;
            for (unsigned tx = 0; tx < 64; ++tx)
                if (video.system24_tile_generation(layer, ty * 64u + tx) > previous) dirty |= uint64_t(1) << tx;
            if (!dirty) continue;
        }
        uploaded += unsigned(std::popcount(dirty));
        // Runs of consecutive changed tiles: [first, first + length) in tiles.
        std::array<uint8_t, 64> run_first{}, run_length{};
        unsigned runs = 0;
        for (uint64_t rest = dirty; rest;) {
            const unsigned first = unsigned(std::countr_zero(rest));
            const uint64_t from_first = rest >> first;
            const unsigned length = ~from_first ? unsigned(std::countr_zero(~from_first)) : 64u - first;
            run_first[runs] = uint8_t(first);
            run_length[runs] = uint8_t(length);
            ++runs;
            rest = first + length >= 64 ? 0 : rest & (~uint64_t(0) << (first + length));
        }
        for (unsigned y = ty * 8u; y < ty * 8u + 8u; ++y) {
            auto *back_row = reinterpret_cast<uint32_t *>(back + size_t(y) * background_stride);
            auto *front_row = reinterpret_cast<uint32_t *>(front + size_t(y) * foreground_stride);
            const size_t line = size_t(y) * 512u;
            for (unsigned r = 0; r < runs; ++r) {
                const unsigned x = run_first[r] * 8u;
                detail::system24_index_run(pixels + line + x, flags + line + x, run_length[r] * 8u,
                                           back_row + x, front_row + x, a, b);
            }
        }
    }
    return uploaded;
}

// The palette texture of the index form: pen p at texel (p % 128, p / 128),
// RGBA8, rows of row_pixels texels (>= 128).
constexpr unsigned kSystem24PaletteWidth = 128, kSystem24PaletteHeight = 64; // 8192 pens
inline bool upload_system24_palette(const rt::Video &video, uint32_t *texels, size_t row_pixels) {
    if (!texels || row_pixels < kSystem24PaletteWidth) return false;
    for (unsigned pen = 0; pen < kSystem24PaletteWidth * kSystem24PaletteHeight; ++pen)
        texels[size_t(pen / kSystem24PaletteWidth) * row_pixels + pen % kSystem24PaletteWidth] =
            system24_argb_to_rgba(video.system24_pen(pen));
    return true;
}

} // namespace vita
