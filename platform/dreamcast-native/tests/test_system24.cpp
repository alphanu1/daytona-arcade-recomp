#include "../tools/system24.hpp"
#undef NDEBUG
#include <cassert>
#include <cstdio>

int main() {
    std::vector<uint8_t> tile(0x10000), chars(0x80000), palette(0x4000), xlat(0xc000), luma(0x20000);
    uint32_t seed = 0x9134672;
    const auto random = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    const auto word = [](auto &bytes, unsigned at, unsigned value) {
        bytes[at * 2] = uint8_t(value); bytes[at * 2 + 1] = uint8_t(value >> 8);
    };
    for (auto &b : tile) b = uint8_t(random());
    for (auto &b : chars) b = uint8_t(random());
    for (auto &b : palette) b = uint8_t(random());
    for (auto &b : xlat) b = uint8_t(random());
    rt::Video video(tile.data(), chars.data());
    const rt::VideoMem mem{palette.data(), xlat.data(), luma.data(), nullptr, nullptr};
    for (unsigned i = 0; i < 4096; ++i) video.palette_w(i, palette.data(), xlat.data());
    const std::vector<rt::GeoPoly> empty;
    rt::GeoPoly rectangle{};
    rectangle.num_vertices = 4; rectangle.luma = 255;
    rectangle.center[1] = 384; rectangle.viewport[2] = 495; rectangle.viewport[3] = 384;
    rectangle.v[0] = {40, -40, {1, 0, 0}}; rectangle.v[1] = {440, -40, {1, 0, 0}};
    rectangle.v[2] = {440, -344, {1, 0, 0}}; rectangle.v[3] = {40, -344, {1, 0, 0}};
    const std::vector<rt::GeoPoly> polygons{rectangle};
    for (unsigned frame = 0; frame < 80; ++frame) {
        for (unsigned layer = 0; layer < 4; ++layer) {
            word(tile, 0x5000 + layer, (random() & 1023) | ((frame & (4u << (layer & 1))) ? 0x8000 : 0));
            word(tile, 0x5004 + layer, (random() & 1023) | ((frame & 3) << 13) |
                (frame % (11 + layer) == 0 ? 0x8000 : 0));
        }
        for (unsigned i = 0x4000; i < 0x4800; ++i) word(tile, i, random() & 1023);
        if (frame >= 40) {
            constexpr unsigned edges[] = {0, 1, 7, 8, 63, 64, 383, 495, 496, 511};
            for (unsigned layer = 0; layer < 4; ++layer) {
                const unsigned edge = edges[(frame / 4 + layer) % 10];
                word(tile, 0x5000 + layer, edge | ((frame & 1) ? 512 : 0));
                word(tile, 0x5004 + layer, ((0u - edge) & 511) |
                    ((frame & 2) ? 512 : 0) | ((frame & 3) << 13));
            }
        }
        for (unsigned i = 0x6000; i < 0x7000; ++i)
            word(tile, i, frame % 5 == 0 ? 0 : frame % 5 == 1 ? 65535 : random());
        word(tile, random() % 0x4000, random());
        chars[random() % chars.size()] ^= 255;
        video.frame_start(); video.screen_update(frame & 1 ? polygons : empty, 0, mem);
        std::vector<uint32_t> rectangles(rt::Video::W * rt::Video::H, video.system24_pen(0));
        const auto draw_rect = [&](const daytona::System24Rect &rect) {
            assert(rect.width && rect.height && rect.layer < 4);
            assert(rect.x + rect.width <= rt::Video::W && rect.y + rect.height <= rt::Video::H);
            assert(rect.sx + rect.width <= 512 && rect.sy + rect.height <= 512);
            for (unsigned y = 0; y < rect.height; ++y) for (unsigned x = 0; x < rect.width; ++x) {
                const unsigned pen = video.system24_pixel(int(rect.layer), rect.sx + x, rect.sy + y);
                if (rect.opaque || (pen & 15))
                    rectangles[(rect.y + y) * rt::Video::W + rect.x + x] = video.system24_pen(pen);
            }
        };
        daytona::system24_rectangles(video, false, draw_rect);
        if (frame & 1) for (unsigned y = 0; y < rt::Video::H; ++y) for (unsigned x = 0; x < rt::Video::W; ++x) {
            const auto pixel = video.raster().pixels()[y * video.raster().stride() + x];
            if (pixel) rectangles[y * rt::Video::W + x] = pixel;
        }
        daytona::system24_rectangles(video, true, draw_rect);
        assert(rectangles == video.screen());
        for (bool front : {false, true}) for (int y = 0; y < rt::Video::H; ++y)
            for (int first = 0; first < rt::Video::W; first += 64) {
                std::array<int16_t, 64> pens;
                const int count = std::min(64, rt::Video::W - first);
                daytona::system24_row(video, front, first, y, count, pens.data());
                for (int i = 0; i < count; ++i)
                    assert(pens[i] == daytona::system24_pass_pen(video, front, first + i, y));
            }
        for (int y = 0; y < rt::Video::H; ++y) for (int x = 0; x < rt::Video::W; ++x) {
            const auto pens = daytona::system24_pens(video, x, y);
            uint32_t color = video.system24_pen(unsigned(pens.back));
            const uint32_t geometry = video.raster().pixels()[size_t(y) * video.raster().stride() + size_t(x)];
            if ((frame & 1) && geometry) color = geometry;
            if (pens.front >= 0 && video.system24_pen(unsigned(pens.front))) color = video.system24_pen(unsigned(pens.front));
            if (color != video.screen()[size_t(y) * rt::Video::W + size_t(x)]) {
                std::fprintf(stderr, "System24 mismatch frame=%u x=%d y=%d back=%d front=%d\n", frame, x, y, pens.back, pens.front);
                return 1;
            }
        }
    }
    for (unsigned bank = 0; bank < 3; ++bank) for (unsigned color = 0; color < 32; ++color)
        word(xlat, bank * 0x2000 + color * 256 + 64, 64 + color * 6);
    std::puts("PASS: 80 exact System24 frames");
}
