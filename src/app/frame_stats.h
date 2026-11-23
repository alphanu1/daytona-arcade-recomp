// daytona --frame-stats: how the game's frames reached the screen, printed on
// exit. Host timing only; it never changes what the game does.
//
//   present interval  time between presented frames while the game plays
//                     (not in the launcher); at 60 Hz about 16.7 ms
//   game frames per presented frame
//                     0 = the last picture shown again, 1 = one new frame,
//                     2+ = catch-up after a late present (the pacer keeps
//                     57.52 frames/s on the wall clock)
//   emulation         host time for one game frame (run_frame)
#pragma once

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace app {

class FrameStats {
public:
    void game_frame(uint64_t ns) { emulation_.push_back(ns); }
    void presented(uint64_t now_ns, int game_frames) {
        if (last_ns_) intervals_.push_back(now_ns - last_ns_);
        last_ns_ = now_ns;
        ++per_present_[std::clamp(game_frames, 0, 3)];
    }
    void paused() { last_ns_ = 0; } // the launcher's time is not a present interval

    void print() const {
        if (intervals_.empty()) {
            std::printf("daytona: frame stats: no frames presented while playing\n");
            return;
        }
        auto ms = [](uint64_t ns) { return double(ns) / 1e6; };
        std::vector<uint64_t> iv = intervals_, em = emulation_;
        std::sort(iv.begin(), iv.end());
        std::sort(em.begin(), em.end());
        auto pct = [](const std::vector<uint64_t> &v, double p) { return v[std::min(v.size() - 1, size_t(p * double(v.size())))]; };
        const uint64_t median = pct(iv, 0.5);
        size_t late = 0;
        for (uint64_t v : iv) late += v > median + median / 2;
        uint64_t total = 0;
        for (uint64_t v : iv) total += v;
        std::printf("daytona: frame stats: %zu presents over %.1f s (%.2f/s); interval ms p50 %.2f, p95 %.2f, p99 %.2f, "
                    "max %.2f; %zu over 1.5x the median\n",
                    iv.size(), double(total) / 1e9, double(iv.size()) * 1e9 / double(total), ms(median), ms(pct(iv, 0.95)),
                    ms(pct(iv, 0.99)), ms(iv.back()), late);
        std::printf("daytona: frame stats: game frames per present: 0: %" PRIu64 ", 1: %" PRIu64 ", 2: %" PRIu64
                    ", 3+: %" PRIu64 "\n", per_present_[0], per_present_[1], per_present_[2], per_present_[3]);
        if (!em.empty())
            std::printf("daytona: frame stats: emulation ms per game frame p50 %.3f, p99 %.3f, max %.3f (%zu frames)\n",
                        ms(pct(em, 0.5)), ms(pct(em, 0.99)), ms(em.back()), em.size());
    }

private:
    std::vector<uint64_t> intervals_, emulation_;
    uint64_t per_present_[4] = {};
    uint64_t last_ns_ = 0;
};

} // namespace app
