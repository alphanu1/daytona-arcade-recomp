#include "../tools/dreamcast_assets.h"

#undef NDEBUG
#include <cassert>

int main() {
    std::vector<uint8_t> palette(0x4000), xlat(0xc000), luma(0x20000);
    std::vector<uint32_t> sheet(0x80000);
    for (size_t i = 0; i < sheet.size(); ++i) sheet[i] = uint32_t(i * 0x1020301u) ^ 0x12345678u;
    for (unsigned bank = 0; bank < 3; ++bank)
        for (unsigned level = 0; level < 64; ++level)
            xlat[bank * 0x4000 + level * 2] = uint8_t(64 + level * 3);
    for (unsigned index = 0; index < 16; ++index) luma[index * 8 * 4] = uint8_t(index * 4 + 1);
    const rt::VideoMem mem{palette.data(), xlat.data(), luma.data(), sheet.data(), sheet.data()};
    rt::GeoPoly poly{};
    poly.num_vertices = 4;
    poly.texheader[0] = 0x40c0; // wrap both axes, no mip/detail blending
    poly.luma = 255;
    poly.center[1] = 384;
    poly.viewport[1] = 352; poly.viewport[2] = 31; poly.viewport[3] = 384;
    poly.v[0] = {0, 0, {1, 0, 0}};
    poly.v[1] = {32, 0, {1, 256, 0}};
    poly.v[2] = {32, -32, {1, 256, 256}};
    poly.v[3] = {0, -32, {1, 0, 256}};
    rt::Raster reference;
    // Exhaust the byte-wide gamma input before changing its implementation.
    // Translation words' high bytes must not affect the shaded RGB result.
    const auto original_xlat = xlat;
    for (unsigned value = 0; value < 256; ++value) {
        const unsigned inputs[] = {value, 255 - value, value ^ 128};
        uint32_t expected = 0;
        for (unsigned channel = 0; channel < 3; ++channel) {
            xlat[channel * 0x4000] = uint8_t(inputs[channel]);
            xlat[channel * 0x4000 + 1] = 255;
            const auto gamma = uint32_t(std::max((double(inputs[channel]) - 64) * 255 / 191, 0.0));
            expected |= gamma << (16 - channel * 8);
        }
        assert(rt::material_rgb(0, 0, mem) == expected);
    }
    std::copy(original_xlat.begin(), original_xlat.end(), xlat.begin());
    for (unsigned header : {0u, 31u, 32u, 63u, 0x17ffu}) {
        poly.texheader[2] = uint16_t(header);
        const auto image = tools::dreamcast::bake_texture(poly, mem);
        reference.render({poly}, 0, mem, 0, 0, 0, 31, 0, 31);
        assert(image.width == 32 && image.height == 32 && !image.alpha);
        for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 32; ++x) {
            unsigned sx = (header & 63) * 32 + x, sy = ((header >> 6) & 31) * 32 + y;
            if (sx >= 1024) { sx -= 1024; sy ^= 1024; }
            const unsigned offset = (sy / 2) * 512 + sx / 2;
            const unsigned shift = (offset & 1) * 16 + (1 - (y & 1)) * 8 + (1 - (x & 1)) * 4;
            const unsigned nibble = (sheet[(offset >> 1) & 0x7ffff] >> shift) & 15;
            const unsigned gray = uint32_t(double(nibble * 12) * 255.0 / 191.0);
            assert(image.pixels[y * 32 + x] == (0xff000000u | gray * 0x10101u));
            assert(image.pixels[y * 32 + x] == reference.pixels()[y * reference.stride() + x]);
        }
    }
    // Distinct luminance within one reporting bin must remain distinct assets.
    poly.luma = 16;
    auto darker = tools::dreamcast::bake_texture(poly, mem);
    poly.luma = 17;
    assert(darker.pixels != tools::dreamcast::bake_texture(poly, mem).pixels);
    poly.luma = 255;
    poly.texheader[0] = 0x6000;
    auto transparent = tools::dreamcast::bake_texture(poly, mem);
    assert(transparent.alpha);
    unsigned clear = 0;
    for (auto pixel : transparent.pixels) if (!(pixel >> 24)) ++clear;
    assert(clear > 0 && clear < transparent.pixels.size());
    // Native untextured materials use the reference's direct luma, not the
    // texture luminance table. Keep both palette paths tied to the CPU oracle.
    poly.texheader[0] = 0;
    for (unsigned shade : {0u, 1u, 4u, 63u, 127u, 255u}) {
        poly.luma = uint8_t(shade);
        reference.render({poly}, 0, mem, 0, 0, 0, 31, 0, 31);
        const uint32_t color = rt::material_solid_color(poly, mem);
        for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 32; ++x)
            assert(reference.pixels()[y * reference.stride() + x] == color);
    }
    bool rejected = false;
    poly.texheader[0] = 0x4006; // 2048 wide: must not silently resize.
    try { tools::dreamcast::bake_texture(poly, mem); } catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    std::vector<uint8_t> tiles(0x10000), chars(0x80000);
    for (size_t i = 0; i < tiles.size(); ++i) tiles[i] = uint8_t(i * 13 + i / 11);
    for (size_t i = 0; i < chars.size(); ++i) chars[i] = uint8_t(i * 7 + i / 17);
    rt::Video video(tiles.data(), chars.data());
    for (unsigned i = 0; i < 4096; ++i) video.palette_w(i, palette.data(), xlat.data());
    video.screen_update({}, 0, mem);
    for (unsigned layer = 0; layer < 4; ++layer) {
        const auto opaque = tools::dreamcast::bake_system24(video, layer, true);
        const auto alpha = tools::dreamcast::bake_system24(video, layer, false);
        assert(opaque.width == 512 && opaque.height == 512 && !opaque.alpha && alpha.alpha);
        for (unsigned i = 0; i < 512 * 512; ++i) {
            const auto pen = video.system24_pixels(int(layer))[i];
            assert(opaque.pixels[i] == video.system24_pen(pen));
            assert(alpha.pixels[i] == (opaque.pixels[i] & ((pen & 15) ? 0xffffffffu : 0x00ffffffu)));
        }
    }
    rejected = false;
    try { tools::dreamcast::bake_system24(video, 4, true); } catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    std::puts("PASS: offline texel/shade/alpha layout, System24 source layers and PVR bounds");
}
