// Reference audio pacing (platform/vita/audio_rate.h) in a simulated device: the
// sound board's output arrives once per emulated frame (late frames lose their time,
// as vita::FrameClock drops it), the device pulls 512 frames every 10.67 ms through
// the same steps as audio.h's callback (prime, rate update, resampler, underrun).
#include "../platform/vita/audio_rate.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "check failed: %s\n", #x); std::exit(1); } } while (0)

namespace {
constexpr double kRate = 48000.0, kFrameHz = 16000000.0 / (656.0 * 424.0); // 57.52 Hz
constexpr int kChunk = 512;

struct Result { unsigned gaps = 0; double min_ratio = 1, max_ratio = 1; };

// speed: emulated frames per real frame (1 = full speed). hitch_ms every hitch_every_s:
// a frame that late, its time lost.
Result simulate(double seconds, double speed, double hitch_ms, double hitch_every_s) {
    vita::AudioRate rate;
    vita::StereoResampler resampler;
    std::vector<float> in(kChunk * 4, 0.0f), out(kChunk * 2);
    Result r;
    double queued = 0, next_frame = 0, next_callback = 0, next_hitch = hitch_every_s;
    bool buffering = true;
    for (double t = 0; t < seconds;) {
        if (next_frame <= next_callback) {
            t = next_frame;
            queued += kRate / kFrameHz;
            next_frame += 1.0 / (kFrameHz * speed);
            if (hitch_ms > 0 && t >= next_hitch) { next_frame += hitch_ms / 1000.0; next_hitch += hitch_every_s; }
        } else {
            t = next_callback;
            next_callback += kChunk / kRate;
            const int level = int(queued);
            if (buffering) {
                if (level < vita::AudioRate::kPrimeFrames) continue;
                buffering = false;
                rate.reset(level);
            }
            const double ratio = rate.update(level);
            r.min_ratio = std::fmin(r.min_ratio, ratio);
            r.max_ratio = std::fmax(r.max_ratio, ratio);
            const int need = resampler.needed(kChunk, ratio);
            if (need > level) { ++r.gaps; buffering = true; resampler.reset(); continue; }
            resampler.run(in.data(), need, out.data(), kChunk, ratio);
            queued -= need;
        }
    }
    return r;
}
} // namespace

int main() {
    // Resampler at ratio 1: an exact copy, two frames late.
    vita::StereoResampler copy;
    float in[64], out[64];
    for (int i = 0; i < 64; ++i) in[i] = float(i + 1);
    CHECK(copy.needed(32, 1.0) == 32 && copy.run(in, 32, out, 32, 1.0) == 32);
    for (int i = 2; i < 32; ++i) CHECK(out[2 * i] == in[2 * (i - 2)] && out[2 * i + 1] == in[2 * (i - 2) + 1]);

    const Result steady = simulate(120, 1.0, 0, 0);
    CHECK(steady.gaps == 0);
    const Result late30 = simulate(120, 1.0, 30, 1.7);
    CHECK(late30.gaps == 0);
    const Result late20 = simulate(120, 1.0, 20, 0.5);
    CHECK(late20.gaps == 0);
    const Result slow = simulate(120, 0.97, 0, 0);
    CHECK(slow.gaps == 0);
    for (const Result *r : {&steady, &late30, &late20, &slow})
        CHECK(r->min_ratio >= 1.0 - vita::AudioRate::kMaxSlowDown - 1e-9 &&
              r->max_ratio <= 1.0 + vita::AudioRate::kMaxSpeedUp + 1e-9);
    const Result hitch = simulate(120, 1.0, 100, 10);
    std::printf("Vita audio pacing: no gap at full speed, 30 ms late every 1.7 s, 20 ms every 0.5 s, 97%% speed "
                "(lowest ratio %.4f); %u gaps for a 100 ms hitch every 10 s\n", slow.min_ratio, hitch.gaps);
}
