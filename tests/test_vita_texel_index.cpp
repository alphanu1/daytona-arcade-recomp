// vita::texel_index (vitaGL index textures) against rt::read_texel_quad, the
// CPU rasterizer's texel fetch, on random and edge positions (x >= 1024 fold).
#include "../platform/vita/texel_index.h"
#include "runtime/raster_texel.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

int main() {
    std::mt19937 rng(0x7e1);
    std::vector<uint32_t> sheet(0x80000);
    for (auto &w : sheet) w = rng();
    unsigned checked = 0;
    auto check = [&](uint32_t bx, uint32_t by, uint32_t u, uint32_t v) {
        const uint32_t expected = rt::read_texel_quad(bx, by, u, u, v, v, sheet.data()).t00 >> 4;
        if (vita::texel_index(bx, by, u, v, sheet.data()) != expected) {
            std::fprintf(stderr, "texel_index mismatch at bx=%u by=%u u=%u v=%u\n", bx, by, u, v);
            std::exit(1);
        }
        ++checked;
    };
    for (unsigned i = 0; i < 2000000; ++i)
        check(32u * (rng() & 63u), 32u * (rng() & 31u), rng() & 2047u, rng() & 2047u);
    for (uint32_t bx : {0u, 992u, 1024u - 32u, 2016u})
        for (uint32_t u = 0; u < 64; ++u)
            for (uint32_t v = 0; v < 8; ++v) check(bx, 992u, 1000u + u, v);
    std::printf("vita::texel_index equals rt::read_texel_quad on %u reads\n", checked);
}
