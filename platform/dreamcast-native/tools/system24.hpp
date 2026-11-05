// license:BSD-3-Clause
// copyright-holders:Olivier Galibert, R. Belmont, ElSemi, Angelo Salese
// Raw System24 composition for the native frontend. Pixel rules follow
// runtime/video.cpp and the existing desktop tile shader; no guest writes.
#pragma once
#include "runtime/video.h"

namespace daytona {
// Logical layers 0..7 interleave background/foreground categories. -1 is
// transparent. Coordinates are native visible pixels (496x384).
inline int system24_layer_pen(const rt::Video &video, unsigned layer, bool opaque, int x, int y) {
    const unsigned l = layer >> 1, category = layer & 1;
    const unsigned hscr = video.system24_word(0x5000 + l);
    const unsigned vscr = video.system24_word(0x5004 + l);
    const unsigned ctrl = video.system24_word(0x5004 + (l & 2));
    if (vscr & 0x8000) return -1;
    unsigned source = l;
    int sx;
    const int sy = int(vscr & 511);
    if (ctrl & 0x6000) {
        if (l & 1) return -1;
        const unsigned mode = (ctrl >> 13) & 3;
        const unsigned horizontal = hscr & 0x8000 ? video.system24_word(0x4000 + 0x200 * l + unsigned(y)) : hscr;
        const int h = int(horizontal & 511);
        sx = -h;
        if (mode == 1) {
            const unsigned above = ((0u - vscr) & 512) ? l : l ^ 1;
            source = y < int((0u - vscr) & 511) ? above : above ^ 1;
        } else {
            const unsigned left = horizontal & 512 ? l : l ^ 1;
            source = x < h ? left : left ^ 1;
        }
    } else {
        unsigned mask = video.system24_word((layer & 4 ? 0x6800 : 0x6000) + unsigned(y) * 4 + unsigned(x >> 7));
        if (l & 1) mask = ~mask;
        if (mask & (0x8000u >> ((x >> 3) & 15))) return -1;
        const unsigned horizontal = hscr & 0x8000 ? video.system24_word(0x4000 + 0x200 * l + unsigned(y)) : hscr;
        sx = int((0u - horizontal) & 511);
    }
    const unsigned tx = unsigned(x + sx) & 511, ty = unsigned(y + sy) & 511;
    const unsigned tile = video.system24_word(source * 4096 + (ty / 8) * 64 + tx / 8);
    const unsigned pen = video.system24_pixel(int(source), tx, ty);
    // Split modes filter category even on opaque backgrounds. Normal-mode
    // opaque backgrounds deliberately ignore both category and transparent pen.
    if (((ctrl & 0x6000) || !opaque) && (tile >> 15) != category) return -1;
    return !opaque && !(pen & 15) ? -1 : int(pen);
}

inline int system24_pass_pen(const rt::Video &video, bool foreground, int x, int y) {
    for (unsigned layer = foreground ? 1 : 0; layer < 8; layer += 2) {
        const int pen = system24_layer_pen(video, layer, !foreground && layer >= 4, x, y);
        if (pen >= 0) return pen;
    }
    return foreground ? -1 : 0;
}
struct System24Pens { int back, front; };
inline System24Pens system24_pens(const rt::Video &video, int x, int y) {
    return {system24_pass_pen(video, false, x, y), system24_pass_pen(video, true, x, y)};
}

// Registers once per layer/row; masks/category and character row per span.
// Horizontal splits occur at tx=0, already a tile-span boundary.
inline void system24_row(const rt::Video &video, bool front, int first, int y, int count, int16_t *pens) {
    std::fill_n(pens, count, int16_t(-1));
    for (unsigned layer = front ? 1 : 0; layer < 8; layer += 2) {
        const unsigned l = layer >> 1;
        const unsigned vscr = video.system24_word(0x5004 + l);
        if (vscr & 0x8000) continue;
        const bool opaque = !front && layer >= 4;
        const unsigned mode = (video.system24_word(0x5004 + (l & 2)) >> 13) & 3;
        if (mode && (l & 1)) continue;
        const unsigned hscr = video.system24_word(0x5000 + l);
        const unsigned horizontal = hscr & 0x8000 ? video.system24_word(0x4000 + 0x200 * l + unsigned(y)) : hscr;
        const unsigned ty = (unsigned(y) + vscr) & 511;
        for (int i = 0; i < count;) {
            const unsigned x = unsigned(first + i), tx = (x - horizontal) & 511;
            const int end = i + std::min({count - i, int(8 - (x & 7)), int(8 - (tx & 7))});
            unsigned mask = 0, source = l;
            if (mode == 1) {
                const unsigned above = ((0u - vscr) & 512) ? l : l ^ 1;
                source = unsigned(y) < ((0u - vscr) & 511) ? above : above ^ 1;
            } else if (mode) {
                const unsigned left = horizontal & 512 ? l : l ^ 1;
                source = x < (horizontal & 511) ? left : left ^ 1;
            } else {
                mask = video.system24_word((layer & 4 ? 0x6800 : 0x6000) + unsigned(y) * 4 + (x >> 7));
                if (l & 1) mask = ~mask;
            }
            if (!(mask & (0x8000u >> ((x >> 3) & 15)))) {
                const unsigned tile = video.system24_word(source * 4096 + (ty / 8) * 64 + tx / 8);
                if ((!mode && opaque) || (tile >> 15) == (layer & 1)) {
                    const uint32_t pixels = video.system24_character_row(tile, ty);
                    const unsigned bank = ((tile >> 7) & 255) * 16;
                    for (int at = i; at < end; ++at) if (pens[at] < 0) {
                        const unsigned pixel = (pixels >> ((((tx + unsigned(at - i)) & 7) ^ 3) * 4)) & 15;
                        if (opaque || pixel) pens[at] = int16_t(bank + pixel);
                    }
                }
            }
            i = end;
        }
    }
    if (!front) for (int i = 0; i < count; ++i) if (pens[i] < 0) pens[i] = 0;
}

// Source-layer rectangles, in painter order. Source coordinates never wrap
// inside a rectangle; texture alpha handles pen zero unless opaque is set.
struct System24Rect {
    unsigned layer, x, y, sx, sy, width, height;
    bool opaque;
};

template<class Emit>
void system24_rectangles(const rt::Video &video, bool front, Emit emit) {
    for (int logical = front ? 7 : 6; logical >= 0; logical -= 2) {
        const unsigned l = unsigned(logical) >> 1;
        const unsigned vscr = video.system24_word(0x5004 + l);
        const unsigned mode = (video.system24_word(0x5004 + (l & 2)) >> 13) & 3;
        if ((vscr & 0x8000) || (mode && (l & 1))) continue;
        const unsigned hscr = video.system24_word(0x5000 + l);
        const bool opaque = !front && logical >= 4;
        // At most two interleaved sets of 8-pixel boundaries across 496 pixels.
        std::array<System24Rect, 128> previous{}, current{};
        unsigned previous_count = 0;
        for (unsigned y = 0; y < rt::Video::H; ++y) {
            const unsigned horizontal = hscr & 0x8000 ? video.system24_word(0x4000 + 0x200 * l + y) : hscr;
            const unsigned sy = (y + vscr) & 511;
            unsigned count = 0;
            for (unsigned x = 0; x < rt::Video::W;) {
                const unsigned sx = (x - horizontal) & 511;
                const unsigned width = std::min({unsigned(rt::Video::W) - x, 8 - (x & 7), 8 - (sx & 7)});
                unsigned source = l, mask = 0;
                if (mode == 1) {
                    const unsigned above = ((0u - vscr) & 512) ? l : l ^ 1;
                    source = y < ((0u - vscr) & 511) ? above : above ^ 1;
                } else if (mode) {
                    const unsigned left = horizontal & 512 ? l : l ^ 1;
                    source = x < (horizontal & 511) ? left : left ^ 1;
                } else {
                    mask = video.system24_word((logical & 4 ? 0x6800 : 0x6000) + y * 4 + (x >> 7));
                    if (l & 1) mask = ~mask;
                }
                const unsigned tile = video.system24_word(source * 4096 + (sy / 8) * 64 + sx / 8);
                if (!(mask & (0x8000u >> ((x >> 3) & 15))) &&
                    ((!mode && opaque) || (tile >> 15) == (unsigned(logical) & 1))) {
                    if (count && current[count - 1].layer == source &&
                        current[count - 1].x + current[count - 1].width == x &&
                        current[count - 1].sx + current[count - 1].width == sx)
                        current[count - 1].width += width;
                    else current.at(count++) = {source, x, y, sx, sy, width, 1, opaque};
                }
                x += width;
            }
            unsigned old = 0;
            for (unsigned i = 0; i < count; ++i) {
                auto &rect = current[i];
                while (old < previous_count && previous[old].x < rect.x) emit(previous[old++]);
                if (old < previous_count && previous[old].x == rect.x) {
                    const auto &before = previous[old++];
                    if (before.layer == rect.layer && before.sx == rect.sx &&
                        before.sy + before.height == rect.sy && before.width == rect.width) {
                        rect.y = before.y;
                        rect.sy = before.sy;
                        rect.height = before.height + 1;
                    } else emit(before);
                }
            }
            while (old < previous_count) emit(previous[old++]);
            std::copy_n(current.begin(), count, previous.begin());
            previous_count = count;
        }
        for (unsigned i = 0; i < previous_count; ++i) emit(previous[i]);
    }
}
} // namespace daytona
