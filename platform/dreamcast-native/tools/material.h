// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels, Ville Linde, Aaron Giles
// Presentation material state, shared by offline preparation and native lookup.
// Shading/addressing follow the BSD-3-Clause reference in raster.cpp.
#pragma once
#include "runtime/raster.h"
#include "runtime/raster_texel.h"

namespace rt {
inline uint32_t material_rgb(unsigned color, unsigned luma, const VideoMem &mem) {
    // Same table as Raster's constructor, evaluated at compile time rather
    // than repeating three double divisions for every palette entry lookup.
    static constexpr auto gamma = [] {
        std::array<uint8_t, 256> table{};
        for (unsigned i = 0; i < table.size(); ++i)
            table[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
        return table;
    }();
    // Only the low byte of each 16-bit translation entry reaches gamma.
    const auto r = gamma[mem.colorxlat[2 * (((color & 31) << 8) + luma)]];
    const auto g = gamma[mem.colorxlat[2 * (0x2000 + (((color >> 5) & 31) << 8) + luma)]];
    const auto b = gamma[mem.colorxlat[2 * (0x4000 + (((color >> 10) & 31) << 8) + luma)]];
    return (r << 16) | (g << 8) | b;
}

inline uint32_t material_solid_color(const GeoPoly &poly, const VideoMem &mem) {
    if (!mem.palram || !mem.colorxlat) throw std::runtime_error("missing solid material memory");
    const unsigned at = 2 * (((poly.texheader[3] >> 6) & 0x3ff) + 0x1000);
    const unsigned color = mem.palram[at] | mem.palram[at + 1] << 8;
    return 0xff000000u | material_rgb(color, poly.luma >> 2, mem);
}

inline std::array<uint32_t, 16> material_colors(const GeoPoly &poly, const VideoMem &mem) {
    if (!mem.palram || !mem.colorxlat || !mem.lumaram)
        throw std::runtime_error("missing material colour memory");
    auto le16 = [](const uint8_t *p, unsigned i) { return unsigned(p[2 * i] | p[2 * i + 1] << 8); };
    const auto *h = poly.texheader;
    const unsigned color = le16(mem.palram, ((h[3] >> 6) & 0x3ff) + 0x1000);
    std::array<uint32_t, 16> colors{};
    for (unsigned index = 0; index < 16; ++index) {
        const unsigned lumabase = unsigned(h[1] & 255) << 7;
        const unsigned luma = std::min(63u, unsigned(mem.lumaram[(lumabase + index * 8) * 4]) * poly.luma / 256);
        colors[index] = ((((h[0] >> 13) & 3) == 3 && index == 15) ? 0u : 0xff000000u) | material_rgb(color, luma, mem);
    }
    return colors;
}

// Four LOCAL adjacent indices, low nibble first: top-left, top-right,
// bottom-left, bottom-right. This keeps the proof at exactly four bits/texel.
inline uint16_t material_cell(const GeoPoly &poly, const VideoMem &mem, unsigned x, unsigned y) {
    const unsigned h2 = poly.texheader[2];
    const uint32_t *sheet = h2 & 0x1000 ? mem.tex1 : mem.tex0;
    if (!sheet) throw std::runtime_error("missing material texture memory");
    const auto q = read_texel_quad((h2 & 63) * 32, ((h2 >> 6) & 31) * 32, x, x + 1, y, y + 1, sheet);
    return uint16_t((q.t00 >> 4) | q.t01 | (q.t10 << 4) | (q.t11 << 8));
}
} // namespace rt
