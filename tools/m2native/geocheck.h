// Geometrizer check for m2native: at each MAME vblank (same completed i960
// instruction count), run the native geometrizer on our buffer RAM and hold
// its output to MAME's M2TRACE_GEOLOG: every word it hands the rasterizer,
// and every polygon kept after culling and clipping (vertices bit for bit).
#pragma once

#include "runtime/geo.h"
#include "runtime/lockstep.h"
#include "runtime/m2_replay_bus.h"
#include "runtime/m2_tgp_board.h"
#include "runtime/raster.h"

#include <cstdlib>

#include <bit>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

class GeoCheck {
public:
    GeoCheck(const std::string &path, rt::Geo &geo, rt::TgpBoard &board, rt::M2ReplayBus &bus, rt::Lockstep &ls)
        : f_(path), geo_(geo), board_(board), bus_(bus) {
        if (!f_) throw std::runtime_error("cannot open " + path);
        std::string line;
        while (std::getline(f_, line)) {
            if (line.compare(0, 3, "vb ") != 0) continue;
            Frame fr{};
            unsigned long long count = 0;
            int parse = 0;
            std::sscanf(line.c_str(), "vb %u %llu %d", &fr.frame, &count, &parse);
            fr.count = count;
            fr.parse = parse != 0;
            fr.off = f_.tellg();
            frames_.push_back(fr);
        }
        for (size_t k = 0; k < frames_.size(); k++) ls.add_callback(frames_[k].count, [this, k] { at(k); });
    }

    uint64_t frames_checked = 0, words = 0, polys = 0;
    uint64_t fb_checked = 0, fb_mismatch = 0;
    std::string first_fb_mismatch;
    size_t frames_logged() const { return frames_.size(); }

private:
    struct Frame {
        unsigned frame = 0;
        uint64_t count = 0;
        bool parse = false;
        std::streamoff off = 0;
    };

    [[noreturn]] void diverge(const Frame &fr, const std::string &what) {
        char b[96];
        std::snprintf(b, sizeof b, "geometrizer, frame %u (i960 instruction %" PRIu64 "): ", fr.frame, fr.count);
        throw rt::Divergence(b + what);
    }

    void at(size_t k) {
        const Frame &fr = frames_[k];
        // MAME's screen_update at this vblank rendered the previous parse
        // (its "fb" line precedes this "vb"); render ours the same way.
        if (!pending_fb_.empty()) check_fb(fr, pending_fb_);
        pending_fb_.clear();
        if (!fr.parse) return;
        geo_.zclip_w(bus_.peek(0x0181c000));
        geo_.record_pushes = true;
        geo_.parse(board_.geo_read_start());
        f_.clear();
        f_.seekg(fr.off);
        std::string line;
        size_t pi = 0, qi = 0;
        while (std::getline(f_, line) && line.compare(0, 3, "vb ") != 0) {
            if (line.compare(0, 2, "p ") == 0) {
                const uint32_t want = uint32_t(std::stoul(line.substr(2), nullptr, 16));
                if (pi >= geo_.pushed.size()) diverge(fr, "MAME pushed more words to the rasterizer (" + line + ")");
                if (geo_.pushed[pi] != want) {
                    char b[96];
                    std::snprintf(b, sizeof b, "rasterizer word %zu: ours %08x, MAME %08x", pi, geo_.pushed[pi], want);
                    diverge(fr, b);
                }
                ++pi;
            } else if (line.compare(0, 3, "fb ") == 0) {
                pending_fb_ = line;
            } else if (line.compare(0, 5, "poly ") == 0) {
                if (qi >= geo_.polys.size()) diverge(fr, "MAME kept more polygons");
                check_poly(fr, qi, geo_.polys[qi], line);
                ++qi;
            }
        }
        if (pi != geo_.pushed.size()) diverge(fr, "we pushed more words to the rasterizer than MAME");
        if (qi != geo_.polys.size()) diverge(fr, "we kept more polygons than MAME");
        ++frames_checked;
        words += pi;
        polys += qi;
    }

    void check_poly(const Frame &fr, size_t qi, const rt::GeoPoly &p, const std::string &line) {
        std::istringstream s(line.substr(5));
        auto hx = [&] { std::string t; s >> t; return uint32_t(std::stoul(t, nullptr, 16)); };
        auto dc = [&] { long v; s >> v; return v; };
        bool ok = hx() == p.z && hx() == p.window;
        for (int i = 0; i < 4; i++) ok = ok && hx() == p.texheader[i];
        ok = ok && hx() == p.luma && hx() == uint32_t(p.texlod) && hx() == p.num_vertices;
        for (int i = 0; i < 4; i++) ok = ok && dc() == p.viewport[i];
        for (int i = 0; i < 2; i++) ok = ok && dc() == p.center[i];
        ok = ok && dc() == long(p.reverse);
        for (int i = 0; ok && i < p.num_vertices; i++) {
            const rt::GeoVertex &v = p.v[i];
            const float f[5] = {v.x, v.y, v.p[0], v.p[1], v.p[2]};
            for (float x : f) ok = ok && hx() == std::bit_cast<uint32_t>(x);
        }
        if (!ok) {
            char b[160];
            std::snprintf(b, sizeof b, "polygon %zu differs; ours z %x window %u luma %u verts %u, MAME \"%.80s\"", qi, p.z,
                          p.window, p.luma, p.num_vertices, line.c_str());
            diverge(fr, b);
        }
    }

    void check_fb(const Frame &fr, const std::string &line) {
        unsigned frame = 0;
        char what[32] = {};
        std::sscanf(line.c_str(), "fb %u %31s", &frame, what);
        if (std::string(what) == "same" || std::string(what) == "empty") return;
        unsigned long long want = std::strtoull(what, nullptr, 16);
        int cx, cy, x0, x1, y0, y1;
        if (std::sscanf(line.c_str(), "fb %*u %*s %d %d %d %d %d %d", &cx, &cy, &x0, &x1, &y0, &y1) != 6) return;
        raster_.render(geo_.polys, geo_.windows(), bus_.video_mem(), cx, cy, x0, x1, y0, y1);
        const uint64_t got = raster_.hash(x0, x1, y0, y1);
        ++fb_checked;
        if (got != want && !fb_mismatch++) {
            char b[96];
            std::snprintf(b, sizeof b, "frame %u (at vblank %u)", frame, fr.frame);
            first_fb_mismatch = b;
        }
        static const char *dir = std::getenv("M2NATIVE_FBDUMP_DIR");
        static const int every = std::getenv("M2NATIVE_FBDUMP_EVERY") ? std::atoi(std::getenv("M2NATIVE_FBDUMP_EVERY")) : 0;
        if (dir && every > 0 && frame % unsigned(every) == 0) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/ours_%05u.rgb", dir, frame);
            if (FILE *d = std::fopen(path, "wb")) {
                for (int y = y0; y <= y1; y++) std::fwrite(raster_.pixels() + y * 512 + x0, 4, size_t(x1 - x0 + 1), d);
                std::fclose(d);
            }
        }
    }

    rt::Raster raster_;
    std::string pending_fb_;
    std::ifstream f_;
    rt::Geo &geo_;
    rt::TgpBoard &board_;
    rt::M2ReplayBus &bus_;
    std::vector<Frame> frames_;
};
