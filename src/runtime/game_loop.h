// The game running on the native board, one frame at a time: the frame
// pacing m2run and the windowed game share. No clock: vblank starts when the
// game waits in its wait-for-vblank loop (or, for a CPU-bound frame, after
// one frame's worth of i960 work), and the frame is done when the vblank
// handler has returned. The caller shows the frame and calls again; with a
// window that is the display's vsync, so the frame rate is the only limit.
#pragma once

#include "runtime/enhance.h"

#include <algorithm>
#include <cmath>

#include "runtime/frame_profile.h"
#include "runtime/gen_support.h"
#include "runtime/lockstep.h"
#include "runtime/m2_board.h"
#include "runtime/sound_board.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rt {

class GameLoop {
public:
    // Detached, single-use work for one sound-board frame. Only its SoundBoard
    // is shared with the GameLoop: UART bytes and clock are owned snapshots.
    // The caller must execute packets in order, never concurrently, and join
    // before accessing sound() or destroying the GameLoop. Moving/overwriting
    // a packet also requires its worker to have joined.
    class SoundPacket {
    public:
        SoundPacket() = default;
        SoundPacket(const SoundPacket &) = delete;
        SoundPacket &operator=(const SoundPacket &) = delete;
        SoundPacket(SoundPacket &&other) noexcept { *this = std::move(other); }
        SoundPacket &operator=(SoundPacket &&other) noexcept {
            if (this != &other) {
                sound_ = std::exchange(other.sound_, nullptr);
                bytes_ = std::move(other.bytes_);
                clock_ = std::exchange(other.clock_, nullptr);
                pending_ = std::exchange(other.pending_, false);
            }
            return *this;
        }
        uint64_t execute();
        snd::SoundBoard *sound() const { return sound_; }

    private:
        friend class GameLoop;
        SoundPacket(snd::SoundBoard *sound, std::vector<uint8_t> &&bytes, FrameProfiler::Clock clock)
            : sound_(sound), bytes_(std::move(bytes)), clock_(clock), pending_(sound != nullptr) {}
        snd::SoundBoard *sound_ = nullptr;
        std::vector<uint8_t> bytes_;
        FrameProfiler::Clock clock_ = nullptr;
        bool pending_ = false;
    };

    // images_dir: the importer's output (build/rom_cache/daytona93).
    // Disable only when a frontend supplies its own native audio engine and
    // consumes board().take_sound_bytes(); no reference sound board is built.
    explicit GameLoop(const std::string &images_dir, bool sound_enabled = true);
    // Images already loaded (rt::import_rom_set: straight from the ROM zip).
    explicit GameLoop(M2Board::Images images, bool sound_enabled = true);

    // Run until the next screen is composed. `inputs` are latched by the I/O
    // board at the start of this frame's vblank.
    void run_frame(const Inputs &inputs);
    // Vita may overlap the independent sound board with GPU submission.
    // Finish/complete sound before another frame, reading sound/profiling,
    // or destroying this GameLoop. Only execute runs on the worker.
    void run_frame_deferred_sound(const Inputs &inputs);
    uint64_t execute_deferred_sound();
    void complete_deferred_sound(uint64_t ticks);

    // Opt-in cross-frame pipeline: the previous detached packet may still run
    // while this advances only the independent main board. Finish it before
    // dispatching this packet. last_profile() remains main-board work only;
    // collect detached sound timings separately, never into a later frame.
    SoundPacket run_frame_sound_packet(const Inputs &inputs);

    // Opt-in profiling in caller-defined host ticks; no SDL dependency here.
    void set_profile_clock(FrameProfiler::Clock clock) { profiler_.set_clock(clock); sound_profile_clock_ = clock; }
    const FrameProfile &last_profile() const { return profiler_.frame; }

    const std::vector<uint32_t> &screen() const { return board_->video().screen(); } // screen_width() x 384, 0xAARRGGBB
    static constexpr int kWidth = Video::W, kHeight = Video::H; // the original screen
    int screen_width() const { return board_->video().width(); }
    // Widescreen (enhancement): the screen widened to `aspect` (width / height,
    // square pixels as displayed) by showing more of the scene at the sides;
    // 0 or anything at most 496:384 is the original screen.
    void set_aspect(double aspect) { board_->set_wide_margin(wide_margin(aspect)); }
    // Draw distance (enhancement; rt::Enhance): 0 = the game's own, -2..+2.
    static void set_draw_distance(int level) {
        Enhance::draw_distance = std::clamp(level, Enhance::kDrawMin, Enhance::kDrawMax);
    }
    void set_draw_budget(uint32_t value) {
        if (value > kMaxSceneryBudget) throw std::invalid_argument("polygon budget exceeds supported maximum");
        board_->scenery()->custom_budget = value;
    }
    // With widescreen, in 3D scenes: the tile backdrop stretched across the width (else plain sky margins).
    void set_stretch_backdrop(bool on) { board_->video().set_stretch_backdrop(on); }
    // Draw mode (enhancement): 0 every frame (the game's), 1 every 2nd, 2 every 3rd.
    void set_frame_skip(int skip) { board_->set_frame_skip(skip); }
    // With widescreen: the race HUD's side groups at the screen edges.
    void set_hud_edges(bool on) { board_->video().set_hud_edges(on); }
    static int wide_margin(double aspect) {
        if (!std::isfinite(aspect) || aspect <= 0) return 0;
        // Headless capture tools can exercise the scenery path at 32:9;
        // the launcher keeps its supported choices at 21:9 and below.
        aspect = std::min(aspect, 32.0 / 9.0);
        const int width = 2 * int(kHeight * aspect / 2 + 0.5);
        return width > kWidth ? (width - kWidth) / 2 : 0;
    }
    M2Board &board() { return *board_; }
    snd::SoundBoard *sound() { return sound_.get(); } // null without the sound ROMs
    static constexpr double kFrameHz = 16000000.0 / (656.0 * 424.0); // the board's video timing
    uint64_t frames() const { return frames_; }
    uint64_t instructions() const { return ls_->count; }
    int interrupts() const { return ls_->interrupts(); }

private:
    void probe();
    FrameProfiler profiler_;
    std::unique_ptr<M2Board> board_;
    std::unique_ptr<Cpu> cpu_;
    std::unique_ptr<Lockstep> ls_;
    std::unique_ptr<gen::Env> env_;
    std::unique_ptr<snd::SoundBoard> sound_;
    std::vector<uint8_t> pending_sound_bytes_;
    FrameProfiler::Clock sound_profile_clock_ = nullptr;
    bool sound_frame_pending_ = false;
    Inputs inputs_;
    bool in_vblank_ = false, frame_done_ = false;
    uint64_t frame_start_ = 0, vblank_start_ = 0, frames_ = 0;
};

} // namespace rt
