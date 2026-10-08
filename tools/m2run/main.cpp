// m2run: the recompiled game running on its own on the native board: no
// trace, no MAME, no emulated CPU and no instruction clock. Headless for now:
// frames go to raw dumps (scripts/rgb2png.py converts them).
//
//   m2run IMAGES_DIR FRAMES [--inputs scripts/inputs/X.txt] [--dump DIR --every N] [--wav FILE]
//         [--aspect W:H [--hud-edges] [--stretch-backdrop]] [--draw-distance N] [--frame-skip N]
//         [--dump-from FRAME] [--native-audio-check] [--nvram DIR] [--save-nvram DIR]
//         [--link-listen PORT --link-next HOST:PORT [--link-sync]]
//         [--draw-budget N] [--draw-order-only] [--original-selection] [--scenery-log FILE]
//
// --draw-budget is an override independent of --draw-distance:
// 0 uses Automatic; e.g. --draw-distance 1 --draw-budget 5000 widens
// cell selection while retaining the original budget. The launcher exposes
// the same Automatic/Custom policy.
// --draw-order-only retains the original cell membership but orders it as
// the positive --draw-distance would; combine with --draw-budget 5000.
// --original-selection disables only the new widescreen selection, for comparison.
// --scenery-log writes JSON lines for EVERY frame at/after --dump-from, even
// without image capture. Costs are game metadata, not visible polygon counts.
// --nvram DIR starts from the app's saved settings EEPROM and backup RAM
// (tools/common/nvram.h). --link-listen/--link-next: link play (the
// communication board, Revision A) over TCP, as the app does: listen for the
// cabinet before this one, connect to the next; --link-sync holds each frame
// to the master's. The link's state is printed at the end.
// --native-audio-check: no reference sound board; the game's sound commands
// go to the native audio engine as the app's would, and the first fault
// stops the run with its frame and that frame's command bytes.
// --aspect widens the screen (the widescreen enhancement, e.g. 16:9); dumps
// are then wider than 496 (the width is printed).
// --wav writes the sound board's output (YM3438 + both MultiPCMs, mixed at
// 48 kHz, 16-bit stereo).
//
// Frame pacing is the game's own: vblank starts when the game has finished
// its frame and waits in its idle loop (or after a cap, for frames that never
// idle), and ends when the vblank handler has returned. A windowed build
// waits for the display's vsync at that point; the frame rate is the only
// limit.

#include "runtime/game_loop.h"
#include "../common/input_script.h"
#include "runtime/native_sound_engine.h"
#include "runtime/enhance_diagnostics.h"
#include "../common/nvram.h"
#include "../common/scenery_log.h"
#include "app/link_socket.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <memory>
#include <utility>
#include <vector>

namespace {

// Linear resampling of interleaved stereo to `rate`, added into out.
void mix_into(std::vector<float> &out, const std::vector<float> &in, double in_rate, double rate) {
    const size_t frames_in = in.size() / 2;
    if (frames_in < 2) return;
    const size_t frames_out = size_t(double(frames_in - 1) * rate / in_rate);
    if (out.size() < frames_out * 2) out.resize(frames_out * 2, 0.0f);
    for (size_t i = 0; i < frames_out; ++i) {
        const double pos = double(i) * in_rate / rate;
        const size_t k = size_t(pos);
        const float f = float(pos - double(k));
        for (int ch = 0; ch < 2; ++ch) out[i * 2 + ch] += in[k * 2 + ch] * (1 - f) + in[(k + 1) * 2 + ch] * f;
    }
}

void write_wav(const std::string &path, const std::vector<float> &mix, uint32_t rate) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.put(char(v)); f.put(char(v >> 8)); f.put(char(v >> 16)); f.put(char(v >> 24)); };
    auto u16 = [&](uint16_t v) { f.put(char(v)); f.put(char(v >> 8)); };
    const uint32_t bytes = uint32_t(mix.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    f.write("data", 4); u32(bytes);
    for (float v : mix) u16(uint16_t(int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2run IMAGES_DIR FRAMES [--inputs FILE] [--dump DIR --every N] [--wav FILE] "
                             "[--aspect W:H]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t frames = std::strtoull(argv[2], nullptr, 10);
    std::string dump_dir, inputs_path, wav_path, nvram_dir, save_nvram_dir, link_next, scenery_log_path;
    int link_listen = 0;
    bool link_sync = false, native_check = false;
    uint64_t every = 0, dump_from = 0;
    double aspect = 0;
    int frame_skip = 0;
    bool hud_edges = false, stretch_backdrop = false, original_selection = false;
    uint32_t draw_budget = 0;
    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--draw-budget") && i + 1 == argc) {
            std::fprintf(stderr, "m2run: --draw-budget needs a value\n");
            return 2;
        }
        if (!std::strcmp(argv[i], "--hud-edges")) hud_edges = true;
        if (!std::strcmp(argv[i], "--stretch-backdrop")) stretch_backdrop = true;
        if (!std::strcmp(argv[i], "--link-sync")) link_sync = true;
        if (!std::strcmp(argv[i], "--native-audio-check")) native_check = true;
        if (!std::strcmp(argv[i], "--draw-order-only")) rt::EnhanceDiagnostics::draw_order_only = true;
        if (!std::strcmp(argv[i], "--original-selection")) original_selection = true;
    }
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--hud-edges") || !std::strcmp(argv[i], "--stretch-backdrop") ||
            !std::strcmp(argv[i], "--link-sync") || !std::strcmp(argv[i], "--native-audio-check") ||
            !std::strcmp(argv[i], "--draw-order-only") || !std::strcmp(argv[i], "--original-selection")) { i--; continue; }
        if (!std::strcmp(argv[i], "--inputs")) inputs_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--dump")) dump_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--every")) every = std::strtoull(argv[i + 1], nullptr, 10);
        else if (!std::strcmp(argv[i], "--dump-from")) dump_from = std::strtoull(argv[i + 1], nullptr, 10);
        else if (!std::strcmp(argv[i], "--wav")) wav_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--nvram")) nvram_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--save-nvram")) save_nvram_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--link-listen")) link_listen = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--link-next")) link_next = argv[i + 1];
        else if (!std::strcmp(argv[i], "--draw-distance")) rt::GameLoop::set_draw_distance(std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--draw-budget")) {
            if (!rt::parse_scenery_budget(argv[i + 1], draw_budget)) {
                std::fprintf(stderr, "m2run: --draw-budget needs 0 (Automatic) or 1..1000000\n");
                return 2;
            }
        }
        else if (!std::strcmp(argv[i], "--scenery-log")) scenery_log_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--frame-skip")) frame_skip = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--aspect")) {
            double w = 0, h = 0;
            if (std::sscanf(argv[i + 1], "%lf:%lf", &w, &h) == 2 && h > 0) aspect = w / h;
        }
    }

    try {
        if (rt::EnhanceDiagnostics::draw_order_only && rt::Enhance::draw_distance <= 0)
            throw std::runtime_error("--draw-order-only needs positive --draw-distance");
        tools::SceneryLog scenery_log(scenery_log_path);
        rt::GameLoop game(dir, !native_check);
        game.set_draw_budget(draw_budget);
        game.board().scenery()->original_selection = original_selection;
        scenery_log.attach(game);
        std::unique_ptr<snd::NativeSoundEngine> native;
        if (native_check) {
            auto file = [&](const char *name) {
                std::ifstream f(dir + "/" + name, std::ios::binary);
                return std::vector<uint8_t>{std::istreambuf_iterator<char>(f), {}};
            };
            native = std::make_unique<snd::NativeSoundEngine>(file("sound_program.bin"), file("pcm1.bin"), file("pcm2.bin"));
        }
        std::vector<float> native_out;
        uint64_t native_rendered = 0;
        if (!nvram_dir.empty()) tools::load_nvram(game, nvram_dir);
        std::unique_ptr<app::TcpLink> link;
        if (link_listen > 0 || !link_next.empty()) {
            link = std::make_unique<app::TcpLink>(uint16_t(link_listen), link_next);
            if (!link->error().empty()) throw std::runtime_error("link: " + link->error());
            game.board().set_link(link.get(), link_sync);
        }
        game.set_frame_skip(frame_skip);
        if (aspect > 0) {
            game.set_aspect(aspect);
            game.set_hud_edges(hud_edges);
            game.set_stretch_backdrop(stretch_backdrop);
            std::printf("m2run: screen %dx%d\n", game.screen_width(), rt::GameLoop::kHeight);
        }
        tools::Script script;
        if (!inputs_path.empty()) script.load(inputs_path);
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<float> fm, pcm;
        uint64_t drive_commands = 0, drive_kinds[16] = {}; // the force feedback drive board's commands, by type
        for (uint64_t f = 0; f < frames; f++) {
            game.run_frame(script.at(game.board().frame()));
            for (uint8_t c : std::exchange(game.board().io().drive_commands, {})) ++drive_commands, ++drive_kinds[c >> 4];
            if (native) { // the app's native audio path, frame by frame
                const auto bytes = game.board().take_sound_bytes();
                const auto before = native->stats();
                try {
                    native->send(bytes.data(), bytes.size());
                    const uint64_t due = uint64_t(double(game.frames()) * 48000.0 / rt::GameLoop::kFrameHz);
                    native_out.resize(size_t(due - native_rendered) * 2);
                    if (due > native_rendered) native->render(native_out.data(), size_t(due - native_rendered));
                    native_rendered = due;
                } catch (const std::exception &e) {
                    std::fprintf(stderr, "m2run: native audio fault at frame %" PRIu64 ": %s\n", game.frames(), e.what());
                }
                const auto after = native->stats();
                if (after.invalid != before.invalid || after.unsupported != before.unsupported || native->sequence_stats().invalid_data) {
                    std::fprintf(stderr, "m2run: native audio at frame %" PRIu64 ": invalid %" PRIu64 " -> %" PRIu64
                                 ", unsupported %" PRIu64 " -> %" PRIu64 "; bytes this frame:",
                                 game.frames(), before.invalid, after.invalid, before.unsupported, after.unsupported);
                    for (uint8_t b : bytes) std::fprintf(stderr, " %02x", b);
                    std::fprintf(stderr, "\n");
                    break;
                }
            }
            if (!wav_path.empty() && game.sound()) {
                const auto a = game.sound()->take_fm(), b = game.sound()->take_pcm();
                fm.insert(fm.end(), a.begin(), a.end());
                pcm.insert(pcm.end(), b.begin(), b.end());
            }
            if (game.board().frame() >= dump_from)
                scenery_log.write(game);
            if (!dump_dir.empty() && every && game.board().frame() >= dump_from && game.board().frame() % every == 0) {
                char path[512];
                std::snprintf(path, sizeof path, "%s/run_%05" PRIu64 ".rgb", dump_dir.c_str(), game.board().frame());
                if (FILE *d = std::fopen(path, "wb")) {
                    std::fwrite(game.screen().data(), 4, game.screen().size(), d);
                    std::fclose(d);
                }
            }
        }
        if (!save_nvram_dir.empty()) tools::save_nvram(game, save_nvram_dir);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("m2run: %" PRIu64 " frames, %" PRIu64 " i960 instructions (all native), %" PRIu64
                    " TGP instructions, %d interrupts, %" PRIu64 " bytes to the sound board; %.2f s (%.0f frames/s)\n",
                    game.frames(), game.instructions(), game.board().tgp().tgp_instructions(), game.interrupts(),
                    game.board().sound_bytes_total(), s, double(game.frames()) / s);
        std::printf("  last screen hash %016" PRIx64 "\n", game.board().video().screen_hash());
        if (const rt::CommBoard *cb = game.board().comm_board()) {
            static const char *states[] = {"off (the game never started the board)", "waiting for the other cabinets", "up", "lost"};
            std::printf("  link: %s", states[int(cb->link())]);
            if (cb->link() == rt::CommBoard::Link::Up) std::printf(", cabinet %d of %d", cb->id(), cb->count());
            std::printf("\n");
        }
        std::printf("  drive board: %" PRIu64 " commands (", drive_commands);
        for (int k = 0, first = 1; k < 16; k++)
            if (drive_kinds[k]) std::printf("%s%x-: %" PRIu64, first ? "" : ", ", k, drive_kinds[k]), first = 0;
        std::printf(")\n");
        if (const snd::SoundBoard *sb = game.sound())
            std::printf("  sound board: %" PRIu64 " 68000 instructions (all native), %zu command bytes received\n", sb->instructions(),
                        sb->bytes_received());
        if (!wav_path.empty() && game.sound()) {
            std::vector<float> mix;
            mix_into(mix, fm, game.sound()->fm_rate(), 48000);
            mix_into(mix, pcm, game.sound()->pcm_rate(), 48000);
            write_wav(wav_path, mix, 48000);
            std::printf("  wrote %s (%.1f s)\n", wav_path.c_str(), double(mix.size() / 2) / 48000.0);
        }
        return 0;
    } catch (const std::exception &e) {
        std::printf("m2run: stopped: %s\n", e.what());
        return 1;
    }
}
