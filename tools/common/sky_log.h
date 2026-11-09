// Read-only background telemetry for original panoramas.
#pragma once
#include "runtime/game_loop.h"
#include "runtime/panorama_revision.h"
#include <fstream>

namespace tools {
class SkyLog {
public:
    explicit SkyLog(const std::string &path) {
        if (path.empty()) return;
        stream_.open(path);
        if (!stream_) throw std::runtime_error("cannot open sky log: " + path);
    }
    void write(rt::GameLoop &game) {
        if (!stream_.is_open()) return;
        auto &b = game.board();
        const auto &v = b.video();
        const auto *revision = rt::panorama_revision(M2_ROMSET);
        stream_ << "{\"frame\":" << b.frame() << ",\"course\":" << unsigned(b.read_byte(0x501460))
                << ",\"romset\":\"" << M2_ROMSET << "\""
                << ",\"windows\":" << v.gpu_windows() << ",\"backdrop\":" << int(v.backdrop())
                << ",\"sky_phase\":" << (revision ? b.read_word(revision->phase) : 0)
                << ",\"descriptor_slot\":" << (revision ? b.read_dword(revision->selector) : 0)
                << ",\"tile_generation\":" << v.system24_texture_generation()
                << ",\"panorama\":" << (v.panorama_active() ? "true" : "false")
                << ",\"panorama_phase\":" << v.panorama().phase
                << ",\"panorama_valid\":" << (v.panorama().phase_valid ? "true" : "false")
                << ",\"panorama_vertical\":" << v.panorama().vertical
                << ",\"panorama_original\":" << (v.panorama().original ? "true" : "false")
                << ",\"panorama_source_valid\":" << (v.panorama().source_valid ? "true" : "false")
                << ",\"panorama_course\":" << unsigned(v.panorama().cached_course)
                << ",\"panorama_height\":" << v.panorama().source_height()
                << ",\"palette_generation\":" << v.system24_palette_generation()
                << ",\"sky_colour\":" << v.system24_pen(v.system24_pixels(2)[0])
                << ",\"scroll\":[";
        for (unsigned i = 0; i < 8; ++i) {
            if (i) stream_ << ',';
            stream_ << v.system24_word(0x5000 + i);
        }
        stream_ << "]}\n";
        if (!stream_) throw std::runtime_error("cannot write sky log");
    }
private:
    std::ofstream stream_;
};
} // namespace tools
