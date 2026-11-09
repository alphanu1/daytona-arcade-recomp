// Original course panoramas decoded from the user's ROM.
// Host-only caches never feed back into game RAM or polygon selection.
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace rt {
class Panorama {
public:
    static constexpr int Width = 2048;
    // The game derives layer-2 hscroll from phase >> 5, then masks to 511.
    // Retain the full 16-bit phase: one complete cycle spans 2048 texels.
    int scroll_x() const { return int(phase >> 5); }
    // Both revisions share eight original 32-column tile maps per course. Keep palette
    // indices/category, not RGB, so the normal per-frame pens supply fades.
    // ROM offsets are verified in docs/original-panorama.md; no ROM data
    // is embedded in the executable or loaded from extracted image files.
    static constexpr int SourceY = 48;
    struct Source { uint32_t table; unsigned height; };
    static const Source *source_for(unsigned course) {
        // Game IDs are Beginner / Expert / Advanced, not menu order.
        static constexpr Source sources[] = {{0x2074240, 392},
            {0x2074268, 432}, {0x2081180, 344}};
        return course < 3 ? &sources[course] : nullptr;
    }
    unsigned source_height() const {
        const auto *source = source_for(cached_course);
        return source ? source->height : 0;
    }
    bool load_original(const std::vector<uint8_t> &rom, unsigned requested_course = 0) {
        const auto *layout = source_for(requested_course);
        if (!layout) return false;
        if (cached_course == requested_course) return true;
        if (failed_courses & (1u << requested_course)) return false;
        failed_courses |= 1u << requested_course; // immutable ROM: cache failures too
        auto range = [&](uint32_t address, uint32_t size) {
            return address >= 0x2000000 && uint64_t(address - 0x2000000) + size <= rom.size();
        };
        auto word = [&](uint32_t address) {
            const auto *p = rom.data() + (address - 0x2000000);
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        };
        const uint32_t table = layout->table, rows = layout->height / 8;
        if (!range(table, 40)) return false;
        std::vector<uint8_t> chars(0x80000), present(0x4000);
        uint32_t upload = word(table);
        bool ended = false;
        for (unsigned block = 0; block < 256; ++block, upload += 8) {
            if (!range(upload, 8)) return false;
            const uint32_t source = word(upload), destination = word(upload + 4);
            if (!source) { ended = true; break; }
            if (!range(source, 4) || destination < 0x1080000) return false;
            const uint32_t count = word(source), offset = destination - 0x1080000;
            if (count > 0x4000 || offset % 32 || uint64_t(offset) + count * 32 > chars.size() ||
                !range(source, 4 + count * 32)) return false;
            std::copy_n(rom.data() + (source + 4 - 0x2000000), count * 32, chars.data() + offset);
            std::fill_n(present.data() + offset / 32, count, uint8_t(1));
        }
        if (!ended) return false;
        std::vector<uint16_t> decoded(Width * layout->height), maps(256 * rows);
        std::vector<Character> decoded_characters;
        std::array<bool, 0x4000> used{};
        for (unsigned section = 0; section < 8; ++section) {
            const uint32_t source = word(table + 8 + section * 4);
            if (!range(source, 12 + 32 * rows * 2) || word(source + 4) != rows || word(source + 8) != 32)
                return false;
            for (unsigned tile = 0; tile < 32 * rows; ++tile) {
                const auto *p = rom.data() + (source + 12 + tile * 2 - 0x2000000);
                const uint16_t value = uint16_t(p[0] | p[1] << 8);
                const unsigned code = value & 0x3fff, colour = (value >> 7) & 255;
                if (!present[code]) return false;
                maps[(tile / 32) * 256 + section * 32 + tile % 32] = value;
                used[code] = true;
                for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
                    const uint8_t byte = chars[(code * 32 + y * 4 + x / 2) ^ 1];
                    const unsigned pixel = (x & 1) ? byte & 15 : byte >> 4;
                    decoded[(tile / 32 * 8 + y) * Width + section * 256 + tile % 32 * 8 + x] =
                        uint16_t((value & 0x8000) | colour * 16 | pixel);
                }
            }
        }
        for (unsigned code = 0; code < used.size(); ++code) if (used[code]) {
            Character character{};
            character.offset = code * 32;
            std::copy_n(chars.data() + character.offset, 32, character.bytes.data());
            decoded_characters.push_back(character);
        }
        characters.swap(decoded_characters);
        source_tiles.swap(maps);
        indices.swap(decoded);
        cached_course = uint8_t(requested_course);
        failed_courses &= ~(1u << requested_course);
        return true;
    }
    // Course selection precedes the actual sky upload. Match the columns
    // visible in the original view and the characters they reference before
    // enabling the extension. This also rejects a stale streaming window.
    bool matches_live(const uint8_t *tiles, const uint8_t *chars) const {
        if (indices.empty()) return false;
        const unsigned start = unsigned(-int((sweep ? register_phase : phase) >> 5)) & (Width - 1);
        const unsigned columns = ((start & 7) + 496 + 7) / 8;
        for (unsigned row = 0; row < source_height() / 8; ++row) for (unsigned col = 0; col < columns; ++col) {
            const unsigned source = (start / 8 + col) & 255;
            const unsigned address = (0x2000 + (row + 6) * 64 + (source & 63)) * 2;
            const uint16_t live = uint16_t(tiles[address] | tiles[address + 1] << 8);
            if (live != source_tiles[row * 256 + source]) return false;
        }
        for (const auto &character : characters)
            if (std::memcmp(chars + character.offset, character.bytes.data(), 32)) return false;
        return true;
    }
    uint16_t original_pixel(int x, int source_y) const {
        return indices[size_t(source_y - SourceY) * Width + (unsigned(x - scroll_x()) & (Width - 1))];
    }
    bool original = false; // automatic with widescreen; explicit in capture tools
    bool source_valid = false;
    unsigned failed_courses = 0;
    uint8_t cached_course = 255;
    bool sweep = false; // capture-only sampling stress, never changes the game
    bool only = false; // capture-only isolation of the existing background pass
    uint16_t phase = 0;
    uint16_t pending_phase = 0, register_phase = 0;
    bool phase_valid = false, register_valid = false;
    uint16_t horizontal = 0, vertical = 0;
    uint8_t course = 255;
    std::vector<uint16_t> indices;
    std::vector<uint16_t> source_tiles;
    struct Character { unsigned offset; std::array<uint8_t, 32> bytes; };
    std::vector<Character> characters;
};
} // namespace rt
