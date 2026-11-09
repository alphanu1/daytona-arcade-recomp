#include "runtime/panorama.h"
#include "runtime/panorama_revision.h"
#include <cstdio>
#include <cstdlib>

int main() {
    auto check = [](bool value, const char *message) {
        if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
    };
    for (const char *romset : {"daytona", "daytona93"}) {
        const auto *revision = rt::panorama_revision(romset);
        check(revision && revision->phase && revision->selector, "supported revision needs observer addresses");
        for (unsigned course = 0; course < 3; ++course)
            check(revision->descriptor(course) != 0, "playable course needs a descriptor");
        check(!revision->descriptor(3) && !revision->descriptor(255), "unverified course must have no descriptor");
    }
    check(!rt::panorama_revision("unknown"), "unsupported ROM must not use another revision's addresses");
    // Synthetic ROM, not extracted game content: distinct sections and a
    // foreground-category tile verify decoding, ordering and wrap direction.
    std::vector<uint8_t> rom(0x80000);
    auto put = [&](unsigned offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) rom[offset + i] = uint8_t(value >> (8 * i));
    };
    put(0x74240, 0x2000100);
    put(0x100, 0x2000200);
    put(0x104, 0x1080000);
    put(0x200, 8);
    for (unsigned section = 0; section < 8; ++section) {
        std::fill_n(rom.data() + 0x204 + section * 32, 32, uint8_t((section + 1) * 17));
        const unsigned offset = 0x1000 + section * 0xc80;
        put(0x74248 + section * 4, 0x2000000 + offset);
        put(offset + 4, 49);
        put(offset + 8, 32);
        for (unsigned tile = 0; tile < 32 * 49; ++tile) {
            rom[offset + 12 + tile * 2] = uint8_t(section);
            rom[offset + 13 + tile * 2] = section == 7 ? 0x80 : 0;
        }
    }
    rt::Panorama original;
    check(original.load_original(rom), "valid source must decode");
    check(original.indices.size() * 2 == 1605632, "index cache only, no RGB texture");
    for (unsigned phase = 0; phase < 65536; phase += 32) {
        original.phase = uint16_t(phase);
        const auto before = original.original_pixel(-2, 80);
        original.phase = uint16_t(phase + 32);
        check(original.original_pixel(-1, 80) == before, "scroll step jumped at a wrap");
        check(original.original_pixel(-1 + 2048, 80) == before, "panorama sampling must be periodic");
    }
    original.phase = 0;
    std::vector<uint8_t> live_tiles(0x10000), live_chars(0x80000);
    std::copy_n(rom.data() + 0x204, 8 * 32, live_chars.data());
    check(!original.matches_live(live_tiles.data(), live_chars.data()), "unfinished map upload must fall back");
    // The initial map spans sections 0 and 1 in the native 496-pixel view.
    for (unsigned row = 6; row < 55; ++row) for (unsigned x = 0; x < 64; ++x)
        live_tiles[(0x2000 + row * 64 + x) * 2] = uint8_t(x / 32);
    check(original.matches_live(live_tiles.data(), live_chars.data()), "complete live source window accepted");
    live_chars[0] ^= 1;
    check(!original.matches_live(live_tiles.data(), live_chars.data()), "stale character bank must fall back");
    live_chars[0] ^= 1;
    live_tiles[(0x2000 + 6 * 64) * 2] ^= 1;
    check(!original.matches_live(live_tiles.data(), live_chars.data()), "stale streamed column must fall back");
    for (int section = 0; section < 8; ++section)
        check(original.original_pixel(section * 256, 48) == ((section == 7 ? 0x8000 : 0) | (section + 1)), "source section order/category");
    original.phase = 32;
    check(original.original_pixel(0, 439) == 0x8008, "original full-width wrap and last source row");
    const auto *cached = original.indices.data();
    check(original.load_original({}) && cached == original.indices.data(), "immutable source stays cached across palette changes");
    check(!rt::Panorama{}.load_original({}), "truncated ROM rejected");
    put(0x200, 0xffffffff);
    check(!rt::Panorama{}.load_original(rom), "overflowing character count rejected");
    put(0x200, 8);
    put(0x1008, 64);
    check(!rt::Panorama{}.load_original(rom), "unexpected map width rejected");
    put(0x1008, 32);
    put(0x104, 0x1080001);
    check(!rt::Panorama{}.load_original(rom), "unaligned character upload rejected");
    put(0x104, 0x1080000);
    // Different source heights share one cache. Switching back must decode
    // again, rather than retaining another course's indices or validation data.
    rom.resize(0x82000);
    for (unsigned course : {0u, 2u, 1u, 0u}) {
        const auto *source = rt::Panorama::source_for(course);
        const unsigned table = source->table - 0x2000000, rows = source->height / 8;
        put(table, 0x2000100);
        for (unsigned section = 0; section < 8; ++section) {
            const unsigned offset = 0x1000 + section * 0x1000;
            put(table + 8 + section * 4, 0x2000000 + offset);
            put(offset + 4, rows);
            put(offset + 8, 32);
            for (unsigned tile = 0; tile < 32 * rows; ++tile) {
                rom[offset + 12 + tile * 2] = uint8_t(section);
                rom[offset + 13 + tile * 2] = section == 7 ? 0x80 : 0;
            }
        }
        check(original.load_original(rom, course), "course switch must decode");
        check(original.cached_course == course && original.source_height() == source->height,
              "course switch must update source geometry");
        check(original.indices.size() == 2048 * source->height && original.characters.size() == 8,
              "course switch must replace cache and character records");
        check(original.original_pixel(0, 48 + int(source->height) - 1) == 0x8008,
              "course-specific bottom row and full-width wrap");
        const auto *same = original.indices.data();
        check(original.load_original({}, course) && same == original.indices.data(), "same course must reuse cache");
    }
    check(!original.load_original(rom, 3) && !original.load_original(rom, 255), "unverified course must fall back");
    check(original.cached_course == 0, "unsupported course must not corrupt the cache");
    rt::Panorama truncated;
    check(!truncated.load_original({}, 2) && !truncated.load_original(rom, 2), "failed immutable course is not retried");
    check(truncated.load_original(rom, 0), "one failed course must not block other courses");
    std::puts("Panorama periodicity, aspect anchoring, horizon and cache checks passed");
}
