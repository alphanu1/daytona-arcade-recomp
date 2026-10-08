// One 4-bit Model 2 texel index from packed texture RAM, for the vitaGL index
// textures: rt::read_texel_quad(bx, by, u, u, v, v, sheet).t00 >> 4 without the
// quad's three other reads (tests/test_vita_texel_index.cpp checks they agree).
#pragma once

#include <cstdint>

namespace vita {

inline uint32_t texel_index(uint32_t bx, uint32_t by, uint32_t u, uint32_t v, const uint32_t *sheet) {
    uint32_t x = bx + u;
    const uint32_t y = by + v;
    const uint32_t fold = x >= 1024 ? 512 : 0; // the x>=1024 fold flips y bit 10 (see raster_texel.h)
    if (fold) x -= 1024;
    const uint32_t off = (((y >> 1) ^ fold) << 9) + (x >> 1);
    const unsigned bit = unsigned(((off & 1) << 4) | ((1u ^ (v & 1u)) << 3) | ((1u ^ (u & 1u)) << 2));
    return (sheet[(off >> 1) & 0x7ffff] >> bit) & 15u;
}

} // namespace vita
