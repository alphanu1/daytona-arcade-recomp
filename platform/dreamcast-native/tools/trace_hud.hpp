// Host-only trace of what the ORIGINAL in-race HUD displays: one row per board
// frame while the race phases are selected, with the tile words the HUD draws
// (decoded to the displayed numbers by tools/check_hud.py) and the source
// variables the native rules in include/hud.h read. Optional snapshots keep
// the complete 2D state and the polygon list of chosen frames for the offline
// art export (tools/prepare_hud.py). Read-only: never writes guest memory.
// Output is ROM-run-derived evidence; keep it under ignored build/ directories.
#pragma once
#include "runtime/game_loop.h"
#include "dreamcast_assets.h"

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <array>
#include <vector>

namespace daytona::native {

struct HudTrace {
    rt::M2Board *board = nullptr;
    std::filesystem::path directory;
    std::FILE *file = nullptr;
    uint64_t rows = 0, snapshot_every = 0;

    void open(rt::M2Board &b, const std::string &path, uint64_t every) {
        if (!std::filesystem::create_directory(path))
            throw std::runtime_error("HUD trace directory already exists; choose a new directory");
        directory = path; board = &b; snapshot_every = every;
        file = std::fopen((directory / "hud.trace").c_str(), "w");
        if (!file) throw std::runtime_error("cannot create HUD trace");
        std::fputs("# hud-trace v1 side=reference\n", file);
    }
    ~HudTrace() { if (file) std::fclose(file); }

    // Model draws of the frame before a snapshot: call site, target, model (g0), task (g13).
    bool log_calls = false;
    std::vector<std::array<uint32_t,4>> calls;
    uint32_t radar_task = 0; // the map object (task g13) that draws the CONDITION panel 288b578 (12d3c)
    bool leads = false;      // the player led on the row before: the map shows 288b5f0 (14 polygons), not 288b5dc (6)
    static void observe_call(void *context, const rt::Cpu &cpu, uint32_t target, int type) {
        auto &self = *static_cast<HudTrace *>(context);
        if (!type && target == 0x1786c && cpu.m_r[16] == 0x288b578) self.radar_task = cpu.m_r[29];
        if (type || !self.log_calls) return;
        const uint32_t model = cpu.m_r[16];
        if (target == 0x1786c || (model >= 0x2880000 && model < 0x2900000))
            self.calls.push_back({cpu.m_IP, target, model, cpu.m_r[29]});
    }

    static bool entity_address(uint32_t at) { return at >= 0x500000 && at <= 0x5ffd00 && !(at & 3); }

    void words(const char *name, uint32_t first, unsigned count, unsigned stride = 2) {
        std::fprintf(file, " %s=", name);
        for (unsigned i = 0; i < count; ++i)
            std::fprintf(file, "%s%x", i ? "." : "", board->read_word(first + i * stride));
    }

    void snapshot(uint64_t frame) {
        auto &b = *board;
        const rt::Video &video = b.video();
        const std::string tag = std::to_string(frame);
        const auto dump = [&](const char *name, uint32_t first, uint32_t bytes) {
            std::ofstream out(directory / (std::string(name) + "_" + tag + ".bin"), std::ios::binary);
            for (uint32_t a = 0; a < bytes; ++a) out.put(char(b.read_byte(first + a)));
        };
        dump("tiles", 0x01000000, 0x10000);
        dump("chars", 0x01080000, 0x80000);
        {
            std::ofstream out(directory / ("pens_" + tag + ".bin"), std::ios::binary);
            for (unsigned pen = 0; pen < 8192; ++pen) {
                const uint32_t c = video.system24_pen(pen);
                out.write(reinterpret_cast<const char *>(&c), 4);
            }
        }
        tools::dreamcast::write_tga(directory / ("frame_" + tag + ".tga"),
            {unsigned(video.width()), rt::Video::H, false, video.screen()});
        std::FILE *polys = std::fopen((directory / ("polys_" + tag + ".txt")).c_str(), "w");
        if (!polys) throw std::runtime_error("cannot create HUD polygon list");
        std::fputs("# index z tex0 tex1 tex2 tex3 luma window count then x,y,pz,pu,pv per vertex (screen pixels)\n", polys);
        unsigned index = 0, baked = 0;
        for (const rt::GeoPoly &poly : video.gpu_polys()) {
            std::fprintf(polys, "%u %u %04x %04x %04x %04x %u %u %u", index, poly.z, poly.texheader[0], poly.texheader[1],
                poly.texheader[2], poly.texheader[3], poly.luma, poly.window, poly.num_vertices);
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (int i = 0; i < poly.num_vertices; ++i) { // as model2_3d_project
                const rt::GeoVertex &v = poly.v[i];
                const float z = v.p[0] + std::numeric_limits<float>::min();
                const float x = float(video.crtc_x() + poly.center[0]) + v.x / z;
                const float y = float((384 - poly.center[1]) + video.crtc_y()) - v.y / z;
                x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
                std::fprintf(polys, " %.3f,%.3f,%.6g,%.6g,%.6g", x, y, v.p[0], v.p[1], v.p[2]);
            }
            std::fputc('\n', polys);
            // Overlay candidates only: small screen-space polygons in the HUD areas.
            const bool hud_area = (x0 >= 150 && x1 <= 350 && y0 >= 30 && y1 <= 100) || (x0 >= 370 && y0 >= 50 && y1 <= 300);
            if (hud_area && (poly.texheader[0] & 0x4000) && baked < 64) {
                char name[96];
                std::snprintf(name, sizeof name, "polytex_%s_%u.tga", tag.c_str(), index);
                tools::dreamcast::write_tga(directory / name, tools::dreamcast::bake_texture(poly, video.gpu_mem()));
                ++baked;
            }
            ++index;
        }
        std::fclose(polys);
        std::FILE *list = std::fopen((directory / ("calls_" + tag + ".txt")).c_str(), "w");
        if (!list) throw std::runtime_error("cannot create HUD call list");
        for (const auto &c : calls) std::fprintf(list, "%06x %06x %08x %08x\n", c[0], c[1], c[2], c[3]);
        std::fclose(list);
    }

    // Call after run_frame. `consumed` is the frame number passed to the input script.
    void sample(uint64_t consumed) {
        auto &b = *board;
        const unsigned phase = b.read_byte(0x5010a4), active = b.read_byte(0x5010a0);
        const uint32_t player = b.read_dword(0x501220);
        const bool racing = (phase >= 19 && phase <= 26) || (active >= 19 && active <= 26);
        if (!racing || !entity_address(player)) { calls.clear(); return; }
        std::fprintf(file, "H n=%" PRIu64 " frame=%" PRIu64 " phase=%u active=%u flags=%x remaining=%d clock=%u",
            rows, consumed, phase, active, b.read_dword(0x501090), std::bit_cast<int32_t>(b.read_dword(0x5010d0)),
            b.read_dword(0x5010cc));
        // Source variables read by the HUD owners (see docs in include/hud.h).
        std::fprintf(file, " lap=%u done=%u rank=%u si=%d spd=%08x gear=%u rpm=%d prof=%u key=%u count=%u laps=%u view=%d"
            " x=%08x z=%08x yaw=%u rpm2=%d unit=%u best=%u lapstart=%u hold=%d",
            b.read_byte(player + 0x6e), b.read_byte(player + 0x6d), b.read_byte(player + 0x6a),
            std::bit_cast<int32_t>(b.read_dword(player + 0x18)), b.read_dword(player + 0x34), b.read_dword(player + 0x148),
            std::bit_cast<int32_t>(b.read_dword(player + 0x150)), b.read_word(player + 0x128), b.read_word(player + 0x60),
            b.read_dword(0x501080), b.read_byte(0x5010ec), std::bit_cast<int8_t>(b.read_byte(0x501710)),
            b.read_dword(player + 0x1c), b.read_dword(player + 0x24), b.read_word(player + 0x58),
            std::bit_cast<int32_t>(b.read_dword(player + 0x154)), b.read_word(0x53e59a), b.read_word(player + 0x64),
            b.read_word(player + 0x68), std::bit_cast<int32_t>(b.read_dword(0x501a84)));
        {   // 12b34..12be0: the opponent whose rank byte is 0, or 1 when the player leads.
            const unsigned wanted = b.read_byte(player + 0x6a) == 0;
            uint32_t at = b.read_dword(0x501224);
            unsigned found = 0;
            for (uint32_t left = b.read_dword(0x501084); left && entity_address(at); --left, at += b.read_dword(at + 8))
                if (b.read_byte(at + 0x6a) == wanted) { found = 1; break; }
            if (found) std::fprintf(file, " leader_id=%u lx=%08x lz=%08x lyaw=%u", b.read_byte(at + 4), b.read_dword(at + 0x1c),
                b.read_dword(at + 0x24), b.read_word(at + 0x58));
            else std::fputs(" leader_id=255 lx=0 lz=0 lyaw=0", file);
        }
        std::fprintf(file, " start=%x scroll=%x.%x", b.read_dword(0x501854), b.read_word(0x0100a000), b.read_word(0x0100a008));
        // a818 wrong way: frame counter 5010b0 (bit 3 blinks the message), the HUD object's flags
        // (object 501268, bit 28 set by a818) and the player's flags (bit 28).
        std::fprintf(file, " counter=%u hudflags=%x pflags=%x", b.read_dword(0x5010b0), b.read_dword(b.read_dword(0x501268)),
            b.read_dword(player));
        std::fputs(" laptimes=", file);
        for (unsigned i = 0; i < 8; ++i) std::fprintf(file, "%s%u", i ? "." : "", b.read_dword(0x5015a0 + 4 * i));
        // Tile words under each numeric HUD element (layer 0, 64 words per row).
        words("timer", 0x010000b8, 6);                    // row 1, columns 28..33 (top row of the 3x4 digits)
        for (unsigned row = 0; row < 20; ++row) {         // lap block: rows 0..19, columns 0..15
            char name[8]; std::snprintf(name, sizeof name, "L%u", row);
            words(name, 0x01000000 + row * 0x80, 16);
        }
        for (unsigned row = 0; row < 6; ++row) {          // position block: rows 0..5, columns 44..61
            char name[8]; std::snprintf(name, sizeof name, "P%u", row);
            words(name, 0x01000000 + row * 0x80 + 44 * 2, 18);
        }
        for (unsigned row = 8; row < 12; ++row) {         // speed block: rows 8..11, columns 22..36
            char name[8]; std::snprintf(name, sizeof name, "S%u", row);
            words(name, 0x01000000 + row * 0x80 + 22 * 2, 15);
        }
        words("M25", 0x01000000 + 25 * 0x80 + 25 * 2, 16); // a818 message (rows 21..26, columns 25..40): its row 25
        // ae98 "TIME EXTENSION!!" (block 2872c42, 24x2 tiles at column 19 of row 12 or 16): the first row of
        // both places, its counter 5010d4 and the lap message counter 5010dc that chooses the place.
        words("X12", 0x01000000 + 12 * 0x80 + 19 * 2, 24);
        words("X16", 0x01000000 + 16 * 0x80 + 19 * 2, 24);
        std::fprintf(file, " ext=%d lapmsg=%d", std::bit_cast<int32_t>(b.read_dword(0x5010d4)),
            std::bit_cast<int32_t>(b.read_dword(0x5010dc)));
        // ad34, the lap message: the label's row 15 from column 24 (its time is in X16) and the lap's ticks +66;
        // flags8 (5010d8 below): bit1 hides it.
        words("M15", 0x01000000 + 15 * 0x80 + 24 * 2, 14);
        std::fprintf(file, " lastlap=%u", b.read_word(player + 0x66));
        // The rolling start's overlay: the hint (ab78, d438) on row 19 from column 19, the digits and GO! on row 18
        // from column 21, the banner and its car (d448..d46c) on row 24, their line scroll (d56c..d5a4: lines
        // 184..239, first word), the accelerator 501054 the hint reads and the start stage 501850.
        words("R19", 0x01000000 + 19 * 0x80 + 19 * 2, 25);
        words("R18", 0x01000000 + 18 * 0x80 + 21 * 2, 20);
        words("R24", 0x01000000 + 24 * 0x80, 64);
        std::fprintf(file, " lscroll=%x accel=%u stage=%d flags8=%x", b.read_word(0x01008170), b.read_word(0x501054),
            int16_t(b.read_word(0x501850)), b.read_dword(0x5010d8));
        // CONDITION panel 12c98..13208: the player's yaw offset +d6, the map task's zoom +34 and its index +53,
        // and every opponent of the list 501224 as id,x,z,+58,+d6 (positions as float bits).
        std::fprintf(file, " pd6=%d", int16_t(b.read_word(player + 0xd6)));
        if (radar_task) std::fprintf(file, " zoom=%08x zix=%u", b.read_dword(radar_task + 0x34), b.read_byte(radar_task + 0x53));
        // The same task's map size: index +52 (129d0..12a04, START + VR1 and VR2/3/4) and the scale +38; the held inputs.
        if (radar_task) std::fprintf(file, " mix=%u mscale=%08x", b.read_byte(radar_task + 0x52), b.read_dword(radar_task + 0x38));
        std::fprintf(file, " held=%x", b.read_dword(0x53e120));
        {
            std::fputs(" cars=", file);
            uint32_t at = b.read_dword(0x501224);
            unsigned n = 0;
            for (uint32_t left = b.read_dword(0x501084); left && entity_address(at); --left, at += b.read_dword(at + 8))
                std::fprintf(file, "%s%u,%08x,%08x,%d,%d", n++ ? ";" : "", b.read_byte(at + 4), b.read_dword(at + 0x1c),
                    b.read_dword(at + 0x24), int16_t(b.read_word(at + 0x58)),
                    int16_t(b.read_word(at + 0xd6)));
        }
        // Overlay polygons: the tachometer needle (afc0, model 288b6a4) and the first polygon
        // of each course-map marker (12b34..12c70; the map's polygons come first).
        const rt::Video &video = b.video();
        const auto project = [&](const rt::GeoPoly &poly, const char *name) {
            std::fprintf(file, " %s=", name);
            for (int i = 0; i < poly.num_vertices; ++i) {
                const rt::GeoVertex &v = poly.v[i];
                const float z = v.p[0] + std::numeric_limits<float>::min();
                std::fprintf(file, "%s%.4f,%.4f", i ? ";" : "", float(video.crtc_x() + poly.center[0]) + v.x / z,
                    float((384 - poly.center[1]) + video.crtc_y()) - v.y / z);
            }
        };
        // The map's polygons come first (24 on course 0, 98 on Advanced), then the leader's 6 (the "2" while the player
        // leads: 14, 12b34 with the rank of the tick the polygons come from) and the player's 13: the markers' first
        // polygons are counted from the end (on course 0 with every polygon drawn, 24 and 30).
        const unsigned leader_end = leads ? 27 : 19;
        unsigned overlay = 0, overlays = 0; bool needle = false;
        for (const rt::GeoPoly &poly : video.gpu_polys())
            if (!poly.texheader[0] && poly.z == 0 && poly.v[0].p[0] < 1e-3f) ++overlays;
        for (const rt::GeoPoly &poly : video.gpu_polys()) {
            if (poly.texheader[0]) continue;
            if (poly.z == 0 && poly.v[0].p[0] < 1e-3f) {
                if (overlays >= leader_end && overlay == overlays - leader_end) project(poly, "leader");
                if (overlays >= 19 && overlay == overlays - 13) project(poly, "player");
                ++overlay;
            } else if (!needle && poly.texheader[3] == 0x00c0 && poly.v[0].p[0] > 0.29f && poly.v[0].p[0] < 0.31f) {
                project(poly, "needle"); needle = true;
            }
        }
        std::fprintf(file, " overlay=%u", overlay);
        // CONDITION panel (window 1, depth 1.5): the panel polygon (texheader 8000, a stipple), then six
        // untextured polygons of full luma an icon, all within 12 pixels of the panel (the scene can put a
        // polygon of its own at that depth): the player's first, then one icon an opponent inside the circle.
        // Logged: the panel, the first polygon of the player's icon and of each opponent's ('|' between).
        {
            const auto radar = [&](const rt::GeoPoly &poly) {
                if (poly.window != 1 || poly.num_vertices < 3 || !(poly.v[0].p[0] > 1.45f && poly.v[0].p[0] < 1.55f)) return false;
                if (poly.texheader[0] == 0x8000) return true;
                if (poly.luma != 255) return false;
                for (int i = 0; i < poly.num_vertices; ++i) {
                    const rt::GeoVertex &v = poly.v[i];
                    const float z = v.p[0] + std::numeric_limits<float>::min();
                    const float x = float(video.crtc_x() + poly.center[0]) + v.x / z;
                    const float y = float((384 - poly.center[1]) + video.crtc_y()) - v.y / z;
                    if (x < 372.0f || x > 475.0f || y < 55.0f || y > 162.0f) return false;
                }
                return true;
            };
            unsigned icons = 0; bool panel = false;
            const auto vertices = [&](const rt::GeoPoly &poly) {
                for (int i = 0; i < poly.num_vertices; ++i) {
                    const rt::GeoVertex &v = poly.v[i];
                    const float z = v.p[0] + std::numeric_limits<float>::min();
                    std::fprintf(file, "%s%.4f,%.4f", i ? ";" : "", float(video.crtc_x() + poly.center[0]) + v.x / z,
                        float((384 - poly.center[1]) + video.crtc_y()) - v.y / z);
                }
            };
            for (const rt::GeoPoly &poly : video.gpu_polys()) {
                if (!radar(poly)) continue;
                if (poly.texheader[0] == 0x8000) {
                    if (!panel) { std::fputs(" rpanel=", file); vertices(poly); panel = true; }
                    continue;
                }
                if (!panel || poly.texheader[0]) continue;
                if (icons % 6 == 0) {
                    std::fputs(icons ? "|" : " ricons=", file);
                    vertices(poly);
                }
                ++icons;
            }
            if (panel) std::fprintf(file, " radar=%u", icons);
        }
        std::fputc('\n', file);
        leads = b.read_byte(player + 0x6a) == 0;
        if (snapshot_every && rows % snapshot_every == 0) snapshot(consumed);
        ++rows;
        calls.clear();
        log_calls = snapshot_every && rows % snapshot_every == 0;
    }
    void close() {
        if (!file) return;
        const bool failed = std::ferror(file) != 0;
        if (std::fclose(file) || failed) { file = nullptr; throw std::runtime_error("cannot write HUD trace"); }
        file = nullptr;
        std::printf("hud_trace rows=%" PRIu64 " coverage=reference-only\n", rows);
    }
};

} // namespace daytona::native
