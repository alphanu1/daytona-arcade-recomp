// Local-ROM parity and timing fixture. The reference draws every scheduled
// picture; the candidate uses the desktop's catch-up selection. No GPU fences,
// saved images, or changes to cabinet files. Timings exclude comparisons.
#include "app/frame_batch.h"
#include "app/video_settings.h"
#include "../tools/common/input_script.h"
#include "../tools/common/nvram.h"
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
struct Batch { uint64_t frame; unsigned updates, polygons; };
std::vector<Batch> capture(const char *path) {
    std::ifstream file(path);
    require(bool(file), "cannot open capture");
    std::vector<Batch> result;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line.starts_with("frame,")) continue;
        std::istringstream in(line);
        std::vector<std::string> fields;
        for (std::string field; std::getline(in, field, ',');) fields.push_back(field);
        require(fields.size() == 33, "unexpected capture columns");
        require(fields[22] == "1366" && fields[23] == "0" && fields[26] == "1" &&
                fields[27] == "0" && fields[28] == "0" && fields[29] == "0" && fields[31] == "0",
                "capture needs software 32:9, HUD edges, Default distance, Automatic budget, every frame, reference audio");
        result.push_back({std::stoull(fields[0]), unsigned(std::stoul(fields[2])), unsigned(std::stoul(fields[32]))});
        require(result.back().updates <= 4, "capture batch exceeds desktop pacing limit");
    }
    require(!result.empty(), "empty capture");
    return result;
}
void compare_ram(rt::M2Board &a, rt::M2Board &b) {
    // Main/work RAM and the RAM-backed video inputs; no reads of device FIFOs.
    constexpr uint32_t ranges[][2] = {{0x200000, 0x20000}, {0x500000, 0x100000},
        {0x1000000, 0x10000}, {0x1080000, 0x80000}, {0x1800000, 0x4000}, {0x1810000, 0xc000}, {0x12800000, 0x20000}};
    for (const auto &range : ranges) for (uint32_t i = 0; i < range[1]; i += 4)
        require(a.read_dword(range[0] + i) == b.read_dword(range[0] + i), "guest RAM changed");
}
}

int check_catchup(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr, "usage: m2catchupcheck IMAGES NVRAM CAPTURE.csv | IMAGES NVRAM --race INPUTS\n");
        return 2;
    }
    try {
        const bool race = argc == 4 && std::string(argv[2]) == "--race";
        require(argc == 3 || race, "unknown catch-up fixture mode");
        tools::Script script;
        std::vector<Batch> batches;
        if (race) {
            script.load(argv[3]);
            for (unsigned view = 1; view <= 4; ++view)
                script.lines.push_back({3200 + (view - 1) * 600, 3209 + (view - 1) * 600, "vr" + std::to_string(view), 1});
            constexpr unsigned sizes[] = {1, 3, 2, 4, 2, 1, 4, 3};
            for (uint64_t frame = 0, i = 0; frame < 5400; ++i) {
                const unsigned n = unsigned(std::min<uint64_t>(sizes[i % 8], 5400 - frame));
                frame += n; batches.push_back({frame, n, 0});
            }
        } else batches = capture(argv[2]);

        rt::GameLoop reference(argv[0]), candidate(argv[0]);
        app::Config cfg; cfg.aspect = "32:9"; cfg.renderer = "software"; cfg.hud_edges = true;
        const auto clock = +[]() -> uint64_t { return SDL_GetTicksNS(); };
        const auto apply_settings = [&](rt::GameLoop &game) {
            app::apply_video_settings(game, cfg, true);
            // Headless coverage includes 32:9 even before the launcher exposes it.
            if (cfg.aspect == "32:9") game.set_aspect(32.0 / 9.0);
        };
        for (auto *game : {&reference, &candidate}) {
            tools::load_nvram(*game, argv[1]);
            apply_settings(*game);
            game->set_profile_clock(clock); game->board().video().set_profile_clock(clock);
        }
        // Advance to the first captured update without changing its guest state.
        const uint64_t warmup = batches.front().frame - batches.front().updates;
        for (uint64_t frame = 0; frame < warmup; ++frame) for (auto *game : {&reference, &candidate}) {
            game->run_frame({}); game->sound()->take_fm(); game->sound()->take_pcm();
        }
        std::vector<double> before, after;
        unsigned suppressed = 0, comparisons = 0, multi = 0, matched_polygons = 0;
        for (const auto &batch : batches) {
            if (race) {
                const uint64_t frame = reference.frames();
                const unsigned section = frame < 2200 ? 0 : unsigned((frame - 2200) / 300);
                cfg.renderer = frame < 2200 || section == 9 ? "hardware" : "software";
                constexpr const char *aspects[] = {"32:9", "16:9", "original"};
                cfg.aspect = aspects[(section / 3) % 3];
                cfg.draw_mode = int(section % 3); cfg.hud_edges = section % 2 == 0;
                for (auto *game : {&reference, &candidate}) apply_settings(*game);
            }
            std::array<double, 2> time{};
            auto run = [&](rt::GameLoop &game, bool optimise) {
                const auto begin = clock();
                for (unsigned left = batch.updates; left; --left) {
                    const bool discard = optimise && !game.board().video().external_3d() &&
                        !app::keep_software_picture(game.board().frame(), left, cfg.draw_mode);
                    if (discard) { game.board().skip_next_screen_update(); ++suppressed; }
                    game.run_frame(script.at(game.board().frame()));
                    if (discard) {
                        const auto &p = game.board().video().last_profile();
                        require(!p.tile_cache && !p.tile_draw && !p.raster && !p.composite, "superseded picture retained video timings");
                    }
                }
                time[optimise] = double(clock() - begin) * 1e-6;
            };
            // Alternate order to reduce systematic cache/host-load bias.
            if (comparisons % 2) { run(candidate, true); run(reference, false); }
            else { run(reference, false); run(candidate, true); }
            require(reference.frames() == batch.frame && candidate.frames() == batch.frame, "game updates were lost");
            require(reference.instructions() == candidate.instructions() && reference.interrupts() == candidate.interrupts(), "CPU execution changed");
            require(reference.board().tgp().tgp_instructions() == candidate.board().tgp().tgp_instructions(), "TGP execution changed");
            require(reference.sound()->instructions() == candidate.sound()->instructions() &&
                    reference.sound()->bytes_received() == candidate.sound()->bytes_received(), "sound execution changed");
            require(reference.sound()->take_fm() == candidate.sound()->take_fm() &&
                    reference.sound()->take_pcm() == candidate.sound()->take_pcm(), "audio samples changed");
            require(reference.board().io().eeprom == candidate.board().io().eeprom &&
                    reference.board().backup_ram() == candidate.board().backup_ram(), "cabinet state changed");
            require(reference.board().io().drive_commands == candidate.board().io().drive_commands, "drive commands changed");
            reference.board().io().drive_commands.clear(); candidate.board().io().drive_commands.clear();
            const auto &rv = reference.board().video(), &cv = candidate.board().video();
            if (rv.external_3d()) {
                require(rv.cpu_front() == cv.cpu_front() && rv.foreground_layer() == cv.foreground_layer(), "hardware HUD preparation changed");
            } else if (reference.screen() != candidate.screen()) {
                std::fprintf(stderr, "frame %llu batch %u aspect %s draw_mode %d\n",
                    static_cast<unsigned long long>(batch.frame), batch.updates, cfg.aspect.c_str(), cfg.draw_mode);
                throw std::runtime_error("last presented pixels changed");
            }
            if (comparisons % 32 == 0) compare_ram(reference.board(), candidate.board());
            if (!race && rv.gpu_polys().size() == batch.polygons) ++matched_polygons;
            if (batch.updates > 1) {
                ++multi; before.push_back(time[0]); after.push_back(time[1]);
                if (!race && batch.frame >= 900 && batch.frame <= 965)
                    std::printf("frame %llu batch %u: %.3f -> %.3f ms CPU work\n", static_cast<unsigned long long>(batch.frame), batch.updates, time[0], time[1]);
            }
            ++comparisons;
        }
        compare_ram(reference.board(), candidate.board());
        require(suppressed > 0 && multi > 0, "fixture did not exercise catch-up");
        const double mean_before = std::accumulate(before.begin(), before.end(), 0.0) / before.size();
        const double mean_after = std::accumulate(after.begin(), after.end(), 0.0) / after.size();
        std::sort(before.begin(), before.end()); std::sort(after.begin(), after.end());
        std::printf("PASS %s %s: %u batches, %u multi-update, %u screen-update suppression requests; final pixels, CPU/TGP, RAM, audio and cabinet state match\n",
            M2_ROMSET, race ? argv[3] : argv[2], comparisons, multi, suppressed);
        std::printf("Multi-update CPU work mean %.3f -> %.3f ms; p95 %.3f -> %.3f; max %.3f -> %.3f\n",
            mean_before, mean_after, before[before.size() * 95 / 100], after[after.size() * 95 / 100], before.back(), after.back());
        if (!race) std::printf("Captured polygon counts reproduced: %u/%u rows; no vsync/audio device or pacing waits in this fixture\n", matched_polygons, comparisons);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "catch-up check: %s\n", error.what()); return 1;
    }
    return 0;
}
