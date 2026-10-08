#pragma once
// Reference audio output pacing (audio.h), without SDL so the host tests run it.
//
// The emulation never runs ahead of real time (FrameClock drops lost time), so the
// sound board produces at most real-time audio. A cushion consumed by a slow frame
// was therefore never rebuilt before: the queue stayed nearly empty and every later
// slow frame cut the output (zero-filled callbacks: crackles). Now:
//   * AudioRate keeps the queue near kTargetFrames by playing it very slightly
//     slower or faster (resampling ratio 1 -/+ a few per mille, smoothed). Slower
//     goes down to -kMaxSlowDown when the emulation stays below full speed (the
//     queue keeps falling); faster never exceeds +kMaxSpeedUp (an excess, e.g.
//     after re-priming, drains over a few seconds instead of raising the pitch).
//   * After an underrun the output is silent until kPrimeFrames are queued again:
//     one short gap instead of a crackle at every callback.
#include <algorithm>
#include <cmath>

namespace vita {

class AudioRate {
public:
    // Cushion in the queue: 64 ms at 48 kHz (+ the 512-frame device buffer, 10.7 ms).
    // Absorbs late frames up to ~60 ms without a gap; longer hitches give one short gap.
    static constexpr int kTargetFrames = 3072;
    static constexpr int kPrimeFrames = 3072;   // queued before playing (again, after an underrun)
    // Largest speed changes. 1% is about a sixth of a semitone; the slow-down limit is
    // only reached when the emulation stays below full speed (56 fps = -2.6%, 54.6 fps
    // = -5%): the sound then follows the game's speed instead of crackling.
    static constexpr double kMaxSlowDown = 0.05;
    static constexpr double kMaxSpeedUp = 0.005;
    static constexpr double kGain = 0.05;       // proportional: ratio change per relative level error
    static constexpr double kIntegral = 0.0002;   // integral: learns a lasting speed difference (slow emulation)
    static constexpr double kSmoothing = 0.05;  // level average per update (~20 callbacks, ~0.2 s)
    static constexpr double kSlew = 0.0005;     // ratio change per update (~4.7% per second at most)

    // Playback (re)starts with this queue level. The ratio is kept: after an underrun
    // the emulation is still as slow as before.
    void reset(int level) { average_ = double(level); }
    // Once per audio callback with the queued frames; returns the input frames consumed
    // per output frame (< 1: slower, the queue refills; > 1: faster).
    double update(int level) {
        average_ += (double(level) - average_) * kSmoothing;
        const double error = (average_ - double(kTargetFrames)) / double(kTargetFrames);
        integral_ = std::clamp(integral_ + error * kIntegral, -kMaxSlowDown, kMaxSpeedUp);
        const double want = 1.0 + std::clamp(error * kGain + integral_, -kMaxSlowDown, kMaxSpeedUp);
        ratio_ += std::clamp(want - ratio_, -kSlew, kSlew);
        return ratio_;
    }
    double ratio() const { return ratio_; }
    double average() const { return average_; }

private:
    double average_ = double(kTargetFrames);
    double ratio_ = 1.0;
    double integral_ = 0.0;
};

// Linear interpolation of interleaved stereo float frames at a variable ratio.
// At ratio 1 it is an exact copy delayed by two frames (42 us).
class StereoResampler {
public:
    void reset() {
        prev_[0] = prev_[1] = cur_[0] = cur_[1] = 0.0f;
        frac_ = 0.0;
    }
    // Input frames that run() will consume to produce out_frames (same arithmetic).
    int needed(int out_frames, double ratio) const {
        double frac = frac_;
        int consumed = 0;
        for (int i = 0; i < out_frames; ++i) {
            frac += ratio;
            while (frac >= 1.0) { frac -= 1.0; ++consumed; }
        }
        return consumed;
    }
    // in: needed(out_frames, ratio) frames. Returns the frames consumed.
    int run(const float *in, int in_frames, float *out, int out_frames, double ratio) {
        int used = 0;
        for (int i = 0; i < out_frames; ++i) {
            const float t = float(frac_);
            out[2 * i] = prev_[0] + (cur_[0] - prev_[0]) * t;
            out[2 * i + 1] = prev_[1] + (cur_[1] - prev_[1]) * t;
            frac_ += ratio;
            while (frac_ >= 1.0) {
                frac_ -= 1.0;
                prev_[0] = cur_[0];
                prev_[1] = cur_[1];
                if (used < in_frames) {
                    cur_[0] = in[2 * used];
                    cur_[1] = in[2 * used + 1];
                    ++used;
                }
            }
        }
        return used;
    }

private:
    float prev_[2] = {}, cur_[2] = {};
    double frac_ = 0.0;
};

} // namespace vita
