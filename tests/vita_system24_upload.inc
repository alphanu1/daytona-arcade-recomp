// Tests the exact CPU upload function used by the Vita GXM renderer, with
// padded GPU-like strides and skipped presentations. No Vita SDK is required.
void system24_upload_tests() {
    Memory mem;
    std::fill(mem.tile.begin(), mem.tile.end(), 0);
    std::fill(mem.chars.begin(), mem.chars.end(), 0x11);
    std::fill(mem.pal.begin(), mem.pal.end(), 0);
    std::fill(mem.xlat.begin(), mem.xlat.end(), 0xff);
    auto video = std::make_unique<rt::Video>(mem.tile.data(), mem.chars.data());
    video->set_external_3d(true);
    video->enable_write_tracking();
    for (unsigned i = 0; i < 0x2000; ++i) video->palette_w(i, mem.pal.data(), mem.xlat.data());
    constexpr size_t stride = 520;
    constexpr uint32_t sentinel = 0x19a55a91u;
    std::array<std::vector<uint32_t>, 8> current, fresh;
    for (auto &texture : current) texture.assign(stride * 512, sentinel);
    for (auto &texture : fresh) texture.assign(stride * 512, sentinel);
    uint64_t previous = UINT64_MAX;
    auto frame = [&] {
        video->tile_memory_w();
        video->character_memory_w();
        video->screen_update({}, 0, mem.view());
    };
    auto upload = [&] {
        unsigned count = 0;
        for (int layer = 0; layer < 4; ++layer) {
            count += vita::upload_system24_layer(*video, layer, previous,
                current[size_t(layer)].data(), stride * sizeof(uint32_t),
                current[size_t(layer + 4)].data(), stride * sizeof(uint32_t));
            vita::upload_system24_layer(*video, layer, UINT64_MAX,
                fresh[size_t(layer)].data(), stride * sizeof(uint32_t),
                fresh[size_t(layer + 4)].data(), stride * sizeof(uint32_t));
        }
        previous = video->system24_texture_generation();
        for (size_t plane = 0; plane < current.size(); ++plane) {
            compare(current[plane].data(), fresh[plane].data(), current[plane].size(),
                    int(plane), "incremental GXM tile upload");
            for (size_t y = 0; y < 512; ++y)
                for (size_t x = 512; x < stride; ++x)
                    require(current[plane][y * stride + x] == sentinel, "GPU stride padding unchanged");
        }
        return count;
    };
    frame();
    require(upload() == 16384, "initial GXM upload initializes every tile");
    require(current[0][0] == 0xffffffff && current[4][0] == 0,
            "category 0 tile goes behind 3D");
    frame();
    require(upload() == 0, "unchanged GXM texture has no writes");
    for (unsigned i = 0; i < 17; ++i) mem.word(i * 13u, 1);
    frame();
    require(upload() == 17, "17 changes upload only 17 tiles, not eight full textures");
    mem.chars[32] ^= 0x11;
    frame();
    require(upload() == 17, "character changes upload every referencing tile");
    mem.word(0x1000 + 8, 1);
    frame(); // The host skips presenting this emulated frame.
    mem.word(0x3000 + 12, 1);
    frame();
    require(upload() == 2, "dirty tiles survive skipped presentation");
    mem.word(0x5000, 43);
    mem.word(0x6000, 0xa55a);
    frame();
    require(upload() == 0, "scroll and masks do not recopy texture pixels");
    mem.word(0x2000, 0x8000);
    frame();
    require(upload() == 1, "category change uploads one tile");
    require(current[2][0] == 0xffffffff && current[6][0] == 0xffffffff,
            "normal opaque background ignores category");
    mem.word(0x5006, 0x2000);
    frame();
    require(upload() == 8192, "normal-to-split updates background pair alpha");
    require(current[2][0] == 0 && current[6][0] == 0xffffffff,
            "split opaque background filters category 1");
    mem.word(0x5006, 0x6000);
    frame();
    require(upload() == 0, "split-to-split does not rewrite unchanged alpha");
    mem.word(0x5006, 0);
    frame();
    require(upload() == 8192, "split-to-normal restores category 1 background");
    require(current[2][0] == 0xffffffff, "normal category restored");
    std::fill(mem.xlat.begin(), mem.xlat.end(), 0);
    video->palette_w(1, mem.pal.data(), mem.xlat.data());
    frame();
    require(upload() == 16384, "palette update recolors all cached tile planes");
    require(current[2][0] == 0xff000000, "updated palette is visible");
    video->palette_w(1, mem.pal.data(), mem.xlat.data());
    frame();
    require(upload() == 0, "same palette value does not invalidate uploads");
    require(vita::upload_system24_layer(*video, 0, UINT64_MAX, current[0].data(), 128,
                                        current[4].data(), stride * sizeof(uint32_t)) == 0,
            "invalid texture stride rejected");
    std::puts("PASS: exact Vita tile uploader: sparse writes, glyphs, skipped frames, palettes, split categories, padded strides");
}

// The vitaGL index form (pen numbers + palette texture) against the colour form above:
// decoded through the palette it gives the same texels, its incremental uploads match a
// full one (two texture sets alternating, as gpu_gl.cpp's slots), a palette change
// rewrites no tile, and the run-based writer equals the per-tile reference.
void system24_index_tests() {
    Memory mem; // random tile RAM, characters and palette
    auto video = std::make_unique<rt::Video>(mem.tile.data(), mem.chars.data());
    video->set_external_3d(true);
    video->enable_write_tracking();
    for (unsigned i = 0; i < 0x2000; ++i) video->palette_w(i, mem.pal.data(), mem.xlat.data());
    constexpr size_t stride = 512, bytes = stride * sizeof(uint32_t);
    auto frame = [&] {
        video->tile_memory_w();
        video->character_memory_w();
        video->screen_update({}, 0, mem.view());
    };
    auto planes = [] {
        std::array<std::vector<uint32_t>, 8> p;
        for (auto &t : p) t.assign(stride * 512, 0x5a5a5a5au);
        return p;
    };
    auto full = [&](bool indices) {
        auto p = planes();
        for (int layer = 0; layer < 4; ++layer) {
            if (indices)
                vita::upload_system24_layer_indices(*video, layer, UINT64_MAX, p[size_t(layer)].data(), bytes,
                                                    p[size_t(layer + 4)].data(), bytes);
            else
                vita::upload_system24_layer(*video, layer, UINT64_MAX, p[size_t(layer)].data(), bytes,
                                            p[size_t(layer + 4)].data(), bytes);
        }
        return p;
    };
    auto check_decoded = [&](const char *what) {
        const auto colour = full(false), index = full(true);
        std::vector<uint32_t> palette(vita::kSystem24PaletteWidth * vita::kSystem24PaletteHeight);
        require(vita::upload_system24_palette(*video, palette.data(), vita::kSystem24PaletteWidth), "palette upload");
        for (size_t plane = 0; plane < 8; ++plane)
            for (size_t i = 0; i < stride * 512; ++i) {
                const uint32_t t = index[plane][i];
                if (!(t >> 24)) { require(colour[plane][i] == 0, what); continue; }
                const uint32_t pen = ((t >> 10) & 63u) * 128u + ((t >> 1) & 127u);
                require(palette[pen] == colour[plane][i], what);
            }
    };
    frame();
    check_decoded("index form through the palette equals the colour form");

    // Two sets, each two frames behind, as gpu_gl.cpp's S24Slot pair.
    std::array<std::array<std::vector<uint32_t>, 8>, 2> sets{planes(), planes()};
    std::array<uint64_t, 2> generation{UINT64_MAX, UINT64_MAX};
    auto upload_set = [&](unsigned s) {
        unsigned count = 0;
        for (int layer = 0; layer < 4; ++layer)
            count += vita::upload_system24_layer_indices(*video, layer, generation[s], sets[s][size_t(layer)].data(),
                                                         bytes, sets[s][size_t(layer + 4)].data(), bytes);
        generation[s] = video->system24_texture_generation();
        const auto fresh = full(true);
        for (size_t plane = 0; plane < 8; ++plane)
            compare(sets[s][plane].data(), fresh[plane].data(), fresh[plane].size(), int(plane),
                    "incremental index upload");
        return count;
    };
    require(upload_set(0) == 16384 && upload_set(1) == 16384, "first index uploads write every tile");
    for (unsigned i = 0; i < 9; ++i) mem.word(i * 37u, uint16_t(i * 101u));
    frame();
    require(upload_set(0) == 9, "index set catches up the changed tiles");
    mem.word(0x1234, 7);
    frame();
    require(upload_set(1) == 10, "the other set catches up two frames of changes");
    require(upload_set(0) == 1, "set 0 catches up the last change");
    std::fill(mem.xlat.begin(), mem.xlat.end(), 0x3c);
    for (unsigned i = 0; i < 0x2000; ++i) video->palette_w(i, mem.pal.data(), mem.xlat.data());
    frame();
    require(upload_set(0) == 0 && upload_set(1) == 0, "a palette change rewrites no index tile");
    check_decoded("index form follows the new palette");
    mem.word(0x5006, 0x2000); // split mode: the opaque background filters category 1
    frame();
    upload_set(1);
    check_decoded("index form in split mode");

    // The run-based writer against the per-tile reference form.
    for (int layer = 0; layer < 4; ++layer) {
        auto a = planes(), b = planes();
        vita::upload_system24_layer_indices(*video, layer, UINT64_MAX, a[0].data(), bytes, a[4].data(), bytes);
        vita::upload_system24_layer_impl<true>(*video, layer, UINT64_MAX, b[0].data(), bytes, b[4].data(), bytes);
        compare(a[0].data(), b[0].data(), a[0].size(), layer, "run writer background");
        compare(a[4].data(), b[4].data(), a[4].size(), layer, "run writer foreground");
    }
    std::puts("PASS: vitaGL index tiles: palette decode = colour form, two alternating sets, palette changes, split mode");
}
