// Host only: the source art of the course and transmission select screens and the "GENTLEMEN START YOUR
// ENGINES" screen (build/dc-native-frontend.PLAN.md), read from a run that goes through them. Output is
// ROM-derived; keep it under ignored build/ directories (tools/prepare_select.py turns it into PAL4
// textures and native_select.inc for code/render/ui/select_render.c).
//
// Captured on frames the select object 501218 (504380) shows them, detected from its state +62:
//   course/        state 1, 80+ iterations in (the maps revealed), 5010b0 & 7 in 1..4 (no panel blinks)
//   transmission/  state 9, 60+ iterations in, cursor 0, the same blink rule
//   gentlemen/     state 16 with 5010a8 in 2..20 (the photo and the text faded in)
// Each holds the unscrolled System24 layers as the screen composes them (layer_2.tga opaque, layer_0.tga
// with pen 0 transparent) with the tiles the select rewrites every iteration cleared from layer 0, and
// sprites.txt: `role x y width height file` for those dynamic pieces, rendered from their authored
// blocks (18e2c: int16 tile base, int16 format, u32 rows, u32 width, then the tiles) with the frame's
// character RAM and palette:
//   panels      23add4 / 23ae88 rows (44 bytes: +c column, +10 row, +14/+18 blocks A/B per language
//               53e59a, 8 bytes apart), 225fa8; role 16+2*slot+variant (course), 24+2*slot+variant
//   digits      18b44: tiles 8500 + 12*digit .. +11, 3x4, two of them right-aligned at tile 10001ec;
//               role 32+digit, 42 the blank tile 8280
//   CAR SELECT  23af9c[cursor] at 10007ae; role 48+cursor
// The 3D pieces are taken from the screen where it differs from the 2D composite: the chosen course's
// map (maps.txt `slot lit x y width height file`, the run's cursor slot lit, the others dim) and the
// frame ring around the chosen panel (frame.txt `x y width height centre_x centre_y file`, relative to
// the select's frame centre +34/+38 of that frame), and the transmission screen's small map's box
// (small_map.txt: the chosen course's map at 0.644 of the course screen's 0.9).
#pragma once
#include "dreamcast_assets.h"
#include "runtime/game_loop.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace daytona::native {

struct SelectExport {
    std::filesystem::path directory;
    unsigned previous_state = 255, in_state = 0, previous_cursor = 255, same_cursor = 0;
    bool course = false, transmission = false, gentlemen = false, cursor_maps[3] = {};

    static constexpr uint32_t SELECT = 0x504380;

    explicit SelectExport(const std::filesystem::path &dir) : directory(dir) {
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("select export directory already exists; choose a new directory");
    }

    struct Block { int16_t base, format; uint32_t rows, width, data; };
    static Block block(rt::M2Board &b, uint32_t at) {
        Block k{int16_t(b.read_word(at)), int16_t(b.read_word(at + 2)),
                uint32_t(b.read_word(at + 4)) | uint32_t(b.read_word(at + 6)) << 16,
                uint32_t(b.read_word(at + 8)) | uint32_t(b.read_word(at + 10)) << 16, at + 12};
        if (!k.rows || k.rows > 64 || !k.width || k.width > 64) throw std::runtime_error("unexpected select block");
        return k;
    }
    static std::vector<uint16_t> block_tiles(rt::M2Board &b, const Block &k) {
        std::vector<uint16_t> tiles;
        uint32_t at = k.data;
        for (uint32_t i = 0; i < k.rows * k.width; ++i) {
            if (k.format) { tiles.push_back(uint16_t(b.read_word(at) + k.base)); at += 2; }
            else { tiles.push_back(uint16_t(b.read_byte(at) + k.base)); ++at; }
        }
        return tiles;
    }
    // Tiles in rows of `width`, pen 0 transparent, as System24 draws them.
    static tools::dreamcast::TextureImage tiles_image(const rt::Video &v, const std::vector<uint16_t> &tiles, unsigned width) {
        const unsigned rows = unsigned(tiles.size()) / width;
        tools::dreamcast::TextureImage image{width * 8, rows * 8, true, std::vector<uint32_t>(width * rows * 64)};
        for (unsigned r = 0; r < rows; ++r) for (unsigned c = 0; c < width; ++c) {
            const unsigned t = tiles[r * width + c];
            for (unsigned y = 0; y < 8; ++y) {
                const uint32_t row = v.system24_character_row(t, y);
                for (unsigned x = 0; x < 8; ++x) {
                    const unsigned pixel = (row >> (((x & 7) ^ 3) * 4)) & 15;
                    image.pixels[(r * 8 + y) * width * 8 + c * 8 + x] =
                        pixel ? (v.system24_pen(((t >> 7) & 255) * 16 + pixel) | 0xff000000u) : 0;
                }
            }
        }
        return image;
    }

    struct Capture {
        std::filesystem::path dir;
        std::ofstream sprites;
        std::vector<std::array<unsigned, 4>> cleared; // tile column, row, width, height in layer 0
        unsigned count = 0;
    };
    void sprite(Capture &c, const rt::Video &v, unsigned role, unsigned x, unsigned y,
                const std::vector<uint16_t> &tiles, unsigned width) {
        const auto image = tiles_image(v, tiles, width);
        const std::string name = "sprite_" + std::to_string(c.count++) + ".tga";
        tools::dreamcast::write_tga(c.dir / name, image);
        c.sprites << role << ' ' << x << ' ' << y << ' ' << image.width << ' ' << image.height << ' ' << name << '\n';
    }
    void block_sprite(Capture &c, rt::M2Board &b, unsigned role, uint32_t address, uint32_t tile_address, bool clear) {
        const Block k = block(b, address);
        const unsigned offset = (tile_address - 0x1000000u) / 2, column = offset % 64, row = offset / 64;
        sprite(c, b.video(), role, column * 8, row * 8, block_tiles(b, k), k.width);
        if (clear) c.cleared.push_back({column, row, k.width, k.rows});
    }
    void digits(Capture &c, rt::M2Board &b) {
        for (unsigned d = 0; d <= 10; ++d) {
            std::vector<uint16_t> tiles(12);
            for (unsigned k = 0; k < 12; ++k) tiles[k] = uint16_t(d < 10 ? 0x8500 + d * 12 + k : 0x8280);
            sprite(c, b.video(), 32 + d, 0, 0, tiles, 3);
        }
        // Two digits, the units at 10001ec (column 54+3, row 3).
        c.cleared.push_back({54, 3, 6, 4});
    }

    Capture begin(const char *name) {
        Capture c;
        c.dir = directory / name;
        std::filesystem::create_directory(c.dir);
        c.sprites.exceptions(std::ios::badbit | std::ios::failbit);
        c.sprites.open(c.dir / "sprites.txt");
        c.sprites << "# role x y width height file\n";
        return c;
    }
    // The layers, layer 0 without the dynamic tiles, the 2D composite and the screen; the 3D difference.
    void layers(Capture &c, rt::M2Board &b, bool maps, bool frame) {
        const rt::Video &v = b.video();
        {
            // The select's pose of this frame, for the native renderer's check (tests/test_select_render.py).
            std::ofstream pose(c.dir / "pose.txt");
            pose << "# state slot timer frame_x frame_y counter course\n" << unsigned(b.read_byte(SELECT + 0x62)) << ' '
                 << b.read_word(0x501468) << ' ' << b.read_word(SELECT + 0x56) << ' ' << int(b.read_dword(SELECT + 0x34)) << ' '
                 << int(b.read_dword(SELECT + 0x38)) << ' ' << b.read_dword(0x5010b0) << ' ' << unsigned(b.read_byte(0x501460)) << '\n';
        }
        for (unsigned l = 0; l < 4; ++l) if (v.system24_word(0x5000 + l) & 0x1ff || v.system24_word(0x5004 + l) & 0x1ff)
            throw std::runtime_error("select capture frame has scrolled layers");
        auto background = tools::dreamcast::bake_system24(v, 2, true);
        auto foreground = tools::dreamcast::bake_system24(v, 0, false);
        tools::dreamcast::TextureImage composite{rt::Video::W, rt::Video::H, false, std::vector<uint32_t>(rt::Video::W * rt::Video::H)};
        for (unsigned y = 0; y < rt::Video::H; ++y) for (unsigned x = 0; x < rt::Video::W; ++x) {
            const uint32_t f = foreground.pixels[y * 512 + x];
            composite.pixels[y * rt::Video::W + x] = (f >> 24) ? f : background.pixels[y * 512 + x];
        }
        for (const auto &r : c.cleared)
            for (unsigned y = r[1] * 8; y < (r[1] + r[3]) * 8; ++y) for (unsigned x = r[0] * 8; x < (r[0] + r[2]) * 8; ++x)
                foreground.pixels[y * 512 + x] = 0;
        tools::dreamcast::write_tga(c.dir / "layer_2.tga", background);
        tools::dreamcast::write_tga(c.dir / "layer_0.tga", foreground);
        tools::dreamcast::TextureImage screen{rt::Video::W, rt::Video::H, false, v.screen()};
        for (auto &p : screen.pixels) p |= 0xff000000u;
        tools::dreamcast::write_tga(c.dir / "screen.tga", screen);
        tools::dreamcast::write_tga(c.dir / "composite.tga", composite);
        // 3D pixels: the screen where it is not the 2D composite.
        std::vector<uint8_t> drawn(rt::Video::W * rt::Video::H);
        for (size_t i = 0; i < drawn.size(); ++i) drawn[i] = (screen.pixels[i] & 0xffffff) != (composite.pixels[i] & 0xffffff);
        const unsigned cursor = b.read_word(0x501468);
        auto box = [&](unsigned x0, unsigned x1, unsigned y0, unsigned y1, unsigned out[4]) {
            unsigned l = x1, t = y1, r = x0, d = y0;
            for (unsigned y = y0; y < y1; ++y) for (unsigned x = x0; x < x1; ++x) if (drawn[y * rt::Video::W + x]) {
                l = std::min(l, x); t = std::min(t, y); r = std::max(r, x + 1); d = std::max(d, y + 1);
            }
            if (l >= r) return false;
            out[0] = l; out[1] = t; out[2] = r - l; out[3] = d - t; return true;
        };
        auto cut = [&](const unsigned r[4], bool only_drawn, const std::string &name) {
            tools::dreamcast::TextureImage image{r[2], r[3], only_drawn, std::vector<uint32_t>(r[2] * r[3])};
            for (unsigned y = 0; y < r[3]; ++y) for (unsigned x = 0; x < r[2]; ++x) {
                const size_t i = (r[1] + y) * rt::Video::W + r[0] + x;
                image.pixels[y * r[2] + x] = (!only_drawn || drawn[i]) ? screen.pixels[i] : 0;
            }
            tools::dreamcast::write_tga(c.dir / name, image);
        };
        if (maps) {
            // The maps: below the panels and the ring (y 167..300), one band per slot.
            std::ofstream out(c.dir / "maps.txt");
            out << "# slot lit x y width height file\n";
            for (unsigned slot = 0; slot < 3; ++slot) {
                unsigned r[4];
                if (!box(slot * 165 + 4, slot * 165 + 162, 167, 300, r)) throw std::runtime_error("select map not drawn");
                const std::string name = "map_" + std::to_string(slot) + ".tga";
                cut(r, false, name);
                out << slot << ' ' << (slot == cursor) << ' ' << r[0] << ' ' << r[1] << ' ' << r[2] << ' ' << r[3] << ' ' << name << '\n';
            }
        }
        if (!maps && frame) {
            // The transmission screen's small map of the chosen course (3D, between the cars): its box.
            // The rows and columns the map fills (60+ drawn pixels; the cars only reach in with a corner).
            unsigned r[4] = {0, 0, 0, 0}, top = 0, bottom = 0, left = 0, right = 0;
            for (unsigned y = 240; y < 350; ++y) {
                unsigned n = 0;
                for (unsigned x = 200; x < 310; ++x) n += drawn[y * rt::Video::W + x];
                if (n >= 60) { if (!bottom) top = y; bottom = y + 1; }
            }
            for (unsigned x = 200; x < 310 && bottom; ++x) {
                unsigned n = 0;
                for (unsigned y = top; y < bottom; ++y) n += drawn[y * rt::Video::W + x];
                if (n >= 60) { if (!right) left = x; right = x + 1; }
            }
            if (!bottom || !right) throw std::runtime_error("select small map not drawn");
            r[0] = left; r[1] = top; r[2] = right - left; r[3] = bottom - top;
            std::ofstream out(c.dir / "small_map.txt");
            out << "# x y width height course\n" << r[0] << ' ' << r[1] << ' ' << r[2] << ' ' << r[3] << ' '
                << unsigned(b.read_byte(0x501460)) << '\n';
        }
        if (frame) {
            // The frame ring around the chosen panel (course y 56..167, transmission 40..200).
            const int cx = int(b.read_dword(SELECT + 0x34)), cy = int(b.read_dword(SELECT + 0x38));
            unsigned r[4];
            // Course panels are 152 apart from x 24 (144 wide), the ring reaches about 12 past them.
            const unsigned left = maps ? 152 * cursor : 248 * cursor, width = maps ? 188 : 248;
            const unsigned right = std::min(left + width, unsigned(rt::Video::W)), top = maps ? 56 : 40, bottom = maps ? 167 : 200;
            // The ring is one flat colour (pen 10d3): keep the commonest drawn colour only (the panel the
            // screen shows may be the other blink variant than the layers hold: one frame of display lag).
            std::map<uint32_t, unsigned> colours;
            for (unsigned y = top; y < bottom; ++y) for (unsigned x = left; x < right; ++x)
                if (drawn[y * rt::Video::W + x]) ++colours[screen.pixels[y * rt::Video::W + x]];
            uint32_t ring = 0; unsigned most = 0;
            for (const auto &[colour, n] : colours) if (n > most) { most = n; ring = colour; }
            for (unsigned y = 0; y < rt::Video::H; ++y) for (unsigned x = 0; x < rt::Video::W; ++x)
                if (screen.pixels[y * rt::Video::W + x] != ring) drawn[y * rt::Video::W + x] = 0;
            if (!box(left, right, top, bottom, r)) throw std::runtime_error("select frame not drawn");
            cut(r, true, "frame.tga");
            std::ofstream out(c.dir / "frame.txt");
            out << "# x y width height centre_x centre_y file\n" << r[0] << ' ' << r[1] << ' ' << r[2] << ' ' << r[3]
                << ' ' << cx << ' ' << cy << " frame.tga\n";
        }
    }

    void sample(rt::M2Board &b) {
        const uint32_t sel = b.read_dword(0x501218);
        if (sel != SELECT || !(b.read_dword(SELECT) >> 31)) { previous_state = 255; return; }
        const unsigned state = b.read_byte(SELECT + 0x62), b0 = b.read_dword(0x5010b0) & 7;
        in_state = state == previous_state ? in_state + 1 : 0;
        previous_state = state;
        const bool steady = b0 >= 1 && b0 <= 4;
        const unsigned language = b.read_word(0x53e59a) & 1, cursor = b.read_word(0x501468);
        same_cursor = cursor == previous_cursor ? same_cursor + 1 : 0;
        previous_cursor = cursor;
        // The chosen course's map lit: maps of every cursor position the run holds for 8+ iterations.
        if (course && state == 1 && in_state >= 80 && cursor < 3 && !cursor_maps[cursor] && same_cursor >= 8) {
            const std::string name = "course_cursor" + std::to_string(cursor);
            Capture c = begin(name.c_str());
            layers(c, b, true, true);
            cursor_maps[cursor] = true;
            std::printf("select_export %s b0=%u\n", name.c_str(), b.read_dword(0x5010b0));
        }
        if (!course && state == 1 && in_state >= 80 && steady) {
            Capture c = begin("course");
            for (unsigned slot = 0; slot < 3; ++slot) {
                const uint32_t row = 0x23add4 + 44 * slot;
                const uint32_t tile = 0x1000000u + 2 * b.read_dword(row + 0xc) + 128 * b.read_dword(row + 0x10);
                for (unsigned variant = 0; variant < 2; ++variant)
                    block_sprite(c, b, 16 + 2 * slot + variant, b.read_dword(row + 0x14 + 4 * variant + 8 * language), tile, true);
            }
            digits(c, b);
            layers(c, b, true, true);
            course = true;cursor_maps[cursor < 3 ? cursor : 0] = true;
            std::printf("select_export course frame b0=%u cursor=%u\n", b.read_dword(0x5010b0), b.read_word(0x501468));
        }
        if (!transmission && state == 9 && in_state >= 60 && steady && b.read_word(0x501468) == 0) {
            Capture c = begin("transmission");
            for (unsigned slot = 0; slot < 2; ++slot) {
                const uint32_t row = 0x23ae88 + 44 * slot;
                const uint32_t tile = 0x1000000u + 2 * b.read_dword(row + 0xc) + 128 * b.read_dword(row + 0x10);
                for (unsigned variant = 0; variant < 2; ++variant)
                    block_sprite(c, b, 24 + 2 * slot + variant, b.read_dword(row + 0x14 + 4 * variant + 8 * language), tile, true);
            }
            for (unsigned cursor = 0; cursor < 2; ++cursor)
                block_sprite(c, b, 48 + cursor, b.read_dword(0x23af9c + 4 * cursor), 0x10007ae, true);
            digits(c, b);
            layers(c, b, false, true);
            transmission = true;
            std::printf("select_export transmission frame b0=%u\n", b.read_dword(0x5010b0));
        }
        const int timer = int(b.read_dword(0x5010a8));
        if (!gentlemen && state == 16 && timer >= 2 && timer <= 20) {
            Capture c = begin("gentlemen");
            layers(c, b, false, false);
            gentlemen = true;
            std::printf("select_export gentlemen frame course=%u\n", b.read_byte(0x501460));
        }
    }
    void finish() const {
        if (!course) throw std::runtime_error("the run did not show the course select");
        std::printf("select_export course=%d transmission=%d gentlemen=%d\n", course, transmission, gentlemen);
    }
};

} // namespace daytona::native
