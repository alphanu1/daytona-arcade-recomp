// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels, Ville Linde, Aaron Giles
// Local-only PVR asset preparation. Texture addressing and shading follow
// runtime/raster.cpp's BSD-3-Clause MAME reference (see THIRD_PARTY.md).
#pragma once

#include "runtime/video.h"
#include "material.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

namespace tools::dreamcast {

struct TextureImage {
    unsigned width, height;
    bool alpha;
    std::vector<uint32_t> pixels; // AARRGGBB, top-left origin
};

inline TextureImage bake_texture(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
    const auto *h = poly.texheader;
    const unsigned renderer = (h[0] >> 13) & 3;
    if (!(renderer & 2)) throw std::runtime_error("cannot bake a solid material as a texture");
    const unsigned width = 32u << (h[0] & 7), height = 32u << ((h[0] >> 3) & 7);
    if (width > 1024 || height > 1024)
        throw std::runtime_error("texture exceeds PVR dimensions; offline splitting is required");
    const uint32_t *sheet = (h[2] & 0x1000) ? mem.tex1 : mem.tex0;
    if (!sheet || !mem.palram || !mem.colorxlat || !mem.lumaram)
        throw std::runtime_error("missing texture/palette memory for asset preparation");
    const auto colors = rt::material_colors(poly, mem);
    TextureImage image{width, height, renderer == 3, std::vector<uint32_t>(size_t(width) * height)};
    const unsigned bx = (h[2] & 63) * 32, by = ((h[2] >> 6) & 31) * 32;
    for (unsigned y = 0; y < height; y += 2)
        for (unsigned x = 0; x < width; x += 2) {
            const auto q = rt::read_texel_quad(bx, by, x, x + 1, y, y + 1, sheet);
            image.pixels[size_t(y) * width + x] = colors[q.t00 >> 4];
            image.pixels[size_t(y) * width + x + 1] = colors[q.t01 >> 4];
            image.pixels[size_t(y + 1) * width + x] = colors[q.t10 >> 4];
            image.pixels[size_t(y + 1) * width + x + 1] = colors[q.t11 >> 4];
        }
    return image;
}

// Exact pre-compression state. It proves a live material still matches its
// prepared image; keys or a captured frame number alone cannot prove that.
inline std::vector<uint8_t> material_state(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
    const unsigned width = 32u << (poly.texheader[0] & 7), height = 32u << ((poly.texheader[0] >> 3) & 7);
    if (width > 1024 || height > 1024 || !(poly.texheader[0] & 0x4000))
        throw std::runtime_error("unsupported prepared material");
    std::vector<uint8_t> bytes;
    bytes.reserve(64 + width * height / 2);
    for (uint32_t color : rt::material_colors(poly, mem))
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(uint8_t(color >> shift));
    for (unsigned y = 0; y < height; y += 2) for (unsigned x = 0; x < width; x += 2) {
        const uint16_t cell = rt::material_cell(poly, mem, x, y);
        bytes.push_back(uint8_t(cell)); bytes.push_back(uint8_t(cell >> 8));
    }
    return bytes;
}

inline void write_tga(const std::filesystem::path &path, const TextureImage &image) {
    std::ofstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.open(path, std::ios::binary);
    const uint8_t header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        uint8_t(image.width), uint8_t(image.width >> 8), uint8_t(image.height), uint8_t(image.height >> 8), 32, 0x28};
    out.write(reinterpret_cast<const char *>(header), sizeof header);
    for (uint32_t color : image.pixels) {
        const uint8_t bgra[] = {uint8_t(color), uint8_t(color >> 8), uint8_t(color >> 16), uint8_t(color >> 24)};
        out.write(reinterpret_cast<const char *>(bgra), sizeof bgra);
    }
    out.close();
}

// Unscrolled source art, not a captured screen. Separate opaque/alpha images
// retain pen-zero RGB for opaque draws without relying on VQ's alpha handling.
inline TextureImage bake_system24(const rt::Video &video, unsigned layer, bool opaque) {
    if (layer >= 4) throw std::runtime_error("System24 layer must be 0..3");
    TextureImage image{512, 512, !opaque, std::vector<uint32_t>(512 * 512)};
    for (unsigned y = 0; y < 512; ++y) for (unsigned x = 0; x < 512; ++x) {
        const unsigned pen = video.system24_pixel(int(layer), x, y);
        image.pixels[y * 512 + x] = video.system24_pen(pen) &
            ((opaque || (pen & 15)) ? 0xffffffffu : 0x00ffffffu);
    }
    return image;
}

inline void export_system24(const std::filesystem::path &directory, const rt::Video &video) {
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("System24 output directory already exists; choose a new directory");
    std::ofstream manifest;
    manifest.exceptions(std::ios::badbit | std::ios::failbit);
    manifest.open(directory / "layers.txt");
    manifest << "# file format width height layer opaque; unscrolled sources, inspect before selecting VQ art\n";
    for (unsigned layer = 0; layer < 4; ++layer) for (bool opaque : {false, true}) {
        const std::string name = "layer_" + std::to_string(layer) + (opaque ? "_opaque.tga" : "_alpha.tga");
        write_tga(directory / name, bake_system24(video, layer, opaque));
        manifest << name << ' ' << (opaque ? "RGB565" : "ARGB1555") << " 512 512 " << layer << ' ' << opaque << '\n';
    }
    manifest.close();
    std::puts("prepared_system24_sources=8 (local ROM-derived candidates; not live bindings or full-stage coverage)");
}

// A selected frame discovers assets, not a prerecorded gameplay frame. Full
// headers/luma are retained; the coarse reporting key must never select assets.
// ponytail: base-level, requested material variants only. Add original mipmaps,
// microtextures and cross-stage coverage before treating this as a complete pack.
inline void export_texture_materials(const std::filesystem::path &directory,
    const std::vector<std::array<uint16_t, 5>> &keys, const rt::VideoMem &mem) {
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("asset output directory already exists; choose a new directory");
    std::set<std::array<uint16_t, 5>> materials;
    std::ofstream manifest;
    manifest.exceptions(std::ios::badbit | std::ios::failbit);
    manifest.open(directory / "materials.txt");
    manifest << "# file format width height h0 h1 h2 h3 luma; base-level requested variants only\n";
    for (const auto &key : keys) {
        if (!(key[0] & 0x4000)) continue;
        if (key[4] > 255) throw std::runtime_error("material luminance out of range");
        if (!materials.insert(key).second) continue;
        rt::GeoPoly poly{};
        std::copy_n(key.begin(), 4, poly.texheader);
        poly.luma = uint8_t(key[4]);
        const TextureImage image = bake_texture(poly, mem);
        const std::string name = "texture_" + std::to_string(materials.size()) + ".tga";
        write_tga(directory / name, image);
        const auto state = material_state(poly, mem);
        std::ofstream proof;
        proof.exceptions(std::ios::badbit | std::ios::failbit);
        proof.open(directory / ("texture_" + std::to_string(materials.size()) + ".m2t"), std::ios::binary);
        proof.write(reinterpret_cast<const char *>(state.data()), state.size());
        proof.close();
        manifest << name << ' ' << (image.alpha ? "ARGB1555" : "RGB565") << ' ' << image.width << ' ' << image.height;
        for (auto value : key) manifest << ' ' << value;
        manifest << '\n';
    }
    manifest.close();
    std::printf("prepared_texture_sources=%zu (local ROM-derived assets, do not distribute)\n", materials.size());
}

inline void export_textures(const std::filesystem::path &directory, const rt::Video &video) {
    std::vector<std::array<uint16_t, 5>> keys;
    for (const auto &poly : video.gpu_polys()) {
        if (poly.window > video.gpu_windows() || poly.num_vertices < 3 || poly.num_vertices > 8 ||
            !(poly.texheader[0] & 0x4000)) continue;
        keys.push_back({poly.texheader[0], poly.texheader[1], poly.texheader[2], poly.texheader[3], poly.luma});
    }
    export_texture_materials(directory, keys, video.gpu_mem());
}

} // namespace tools::dreamcast
