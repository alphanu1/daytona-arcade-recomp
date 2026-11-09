// Read-only snapshots of decoded System 24 layers for local asset inspection.
#pragma once
#include "runtime/game_loop.h"
#include "runtime/panorama_revision.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace tools {
inline void dump_tiles(rt::GameLoop &game, const std::string &directory) {
    if (directory.empty()) return;
    const auto &video = game.board().video();
    std::filesystem::create_directories(directory);
    std::ostringstream prefix;
    prefix << directory << "/frame_" << std::setfill('0') << std::setw(5) << game.frames();
    auto save = [](const std::string &path, const void *data, size_t bytes) {
        std::ofstream file(path, std::ios::binary);
        file.write(static_cast<const char *>(data), std::streamsize(bytes));
        if (!file) throw std::runtime_error("cannot write tile snapshot: " + path);
    };
    // m2gpushot uses desktop external-3D mode: these are the palette/register
    // snapshots from screen_update, not guest values from the following frame.
    save(prefix.str() + "_pens.bin", video.gpu_pens(), rt::Video::kGpuPens * sizeof(uint32_t));
    save(prefix.str() + "_registers.bin", video.gpu_tile_words(), rt::Video::kGpuTileWords * sizeof(uint16_t));
    // The original backdrop is streamed into layer 2 from a wider set of tile
    // maps. Character RAM and the selector let the offline extractor recover
    // the complete source, including portions absent from this frame's window.
    std::vector<uint8_t> characters(0x80000);
    for (uint32_t i = 0; i < characters.size(); ++i)
        characters[i] = game.board().read_byte(0x01080000 + i);
    save(prefix.str() + "_characters.bin", characters.data(), characters.size());
    std::ofstream meta(prefix.str() + "_source.json");
    const auto *revision = rt::panorama_revision(M2_ROMSET);
    meta << "{\"frame\":" << game.frames()
         << ",\"romset\":\"" << M2_ROMSET << "\""
         << ",\"course\":" << unsigned(game.board().read_byte(0x501460))
         << ",\"descriptor_slot\":" << (revision ? game.board().read_dword(revision->selector) : 0) << "}\n";
    if (!meta) throw std::runtime_error("cannot write tile source metadata");
    for (int layer = 0; layer < 4; ++layer) {
        const std::string base = prefix.str() + "_layer" + std::to_string(layer);
        save(base + "_indices.bin", video.system24_pixels(layer), 512 * 512 * sizeof(uint16_t));
        save(base + "_flags.bin", video.system24_flags(layer), 512 * 512);
    }
}
} // namespace tools
