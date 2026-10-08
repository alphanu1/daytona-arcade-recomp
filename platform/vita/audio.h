#pragma once
#include "core_policy.h"
#include <SDL.h>
#include "runtime/sound_board.h"
#include "audio_rate.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vita {
// The SDL playback callback consumes only converted samples. The sound worker
// may call push(); join it before main changes settings, pauses or closes audio.
// SDL's device lock serializes stream conversion with the playback callback.
// Output pacing (cushion kept by a slight speed change, silent re-priming after an
// underrun): audio_rate.h.
// Silent FM is not queued: Daytona's sound program leaves the YM3438 silent (perf.log on
// the console: 0 non-zero FM frames over whole races), yet resampling that silence from
// 55.6 to 48 kHz in SDL_AudioStreamPut cost ~1.65 ms per frame on the sound core. The PCM
// stream sets the pace; the FM stream holds only the FM that was not silent, aligned on
// the head of the PCM queue (padded with silence when it starts again), and the callback
// adds whatever FM it has. Not silent FM goes through the same SDL conversion as before.
class Audio {
public:
    struct Stats {
        unsigned underruns = 0;  // queue ran dry during playback (one short gap each)
        double ratio = 1.0;      // playback speed (input frames per output frame)
        double queued_ms = 0.0;  // averaged queue level
    };
    Audio() = default;
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;
    ~Audio() { close(); }
    bool open() {
        applied_core_mask_ = 0;
        SDL_AudioSpec want{}, got{};
        want.freq = 48000; want.format = AUDIO_S16SYS;
        want.channels = 2; want.samples = 512; // 10.7 ms: smaller steps, the cushion is in the queue
        want.callback = callback; want.userdata = this;
        device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
        if (!device_) return false;
        rate_ = got.freq;
        fm_rate_ = int(snd::SoundBoard::kYmClock / 144.0 + 0.5);
        fm_ = SDL_NewAudioStream(AUDIO_F32SYS, 2, fm_rate_, AUDIO_F32SYS, 2, rate_);
        pcm_ = SDL_NewAudioStream(AUDIO_F32SYS, 2, int(snd::SoundBoard::kPcmClock / 224.0 + 0.5),
                                  AUDIO_F32SYS, 2, rate_);
        if (!fm_ || !pcm_) { close(); return false; }
        restart_locked(); // the callback is not running yet: the device starts paused
        return true; // device starts paused; prime it before playing
    }
    void push(snd::SoundBoard &board) {
        const auto fm = board.take_fm(), pcm = board.take_pcm();
        if (!device_) return; // drain the board even when the device failed
        SDL_LockAudioDevice(device_);
        const int limit = rate_ * 2 * int(sizeof(float)) / 4; // at most 250 ms
        if (SDL_AudioStreamAvailable(fm_) > limit || SDL_AudioStreamAvailable(pcm_) > limit) {
            SDL_AudioStreamClear(fm_); SDL_AudioStreamClear(pcm_);
            fm_active_ = false;
        }
        bool fm_ok = true;
        if (std::any_of(fm.begin(), fm.end(), [](float v) { return v != 0.0f; })) {
            if (!fm_active_) {
                // FM starts again after silence: pad it with the silence it skipped, so its
                // first sample plays with the PCM sample queued at the same time.
                const int behind = (SDL_AudioStreamAvailable(pcm_) - SDL_AudioStreamAvailable(fm_)) / kFrameBytes;
                if (behind > 0) {
                    const std::vector<float> silence(size_t(int64_t(behind) * fm_rate_ / rate_) * 2u, 0.0f);
                    if (!silence.empty())
                        fm_ok = SDL_AudioStreamPut(fm_, silence.data(), int(silence.size() * sizeof(float))) >= 0;
                }
                fm_active_ = true;
            }
            fm_ok = fm_ok && SDL_AudioStreamPut(fm_, fm.data(), int(fm.size() * sizeof(float))) >= 0;
        } else {
            fm_active_ = false; // what is still queued plays out, aligned on the PCM
        }
        if (!fm_ok || SDL_AudioStreamPut(pcm_, pcm.data(), int(pcm.size() * sizeof(float))) < 0)
            std::fprintf(stderr, "audio queue: %s\n", SDL_GetError());
        const int prime = AudioRate::kPrimeFrames * kFrameBytes;
        const bool ready = SDL_AudioStreamAvailable(pcm_) >= prime;
        SDL_UnlockAudioDevice(device_);
        if (!playing_ && ready) { SDL_PauseAudioDevice(device_, 0); playing_ = true; }
    }
    void pause() {
        if (!device_) return;
        SDL_PauseAudioDevice(device_, 1);
        SDL_LockAudioDevice(device_);
        SDL_AudioStreamClear(fm_); SDL_AudioStreamClear(pcm_);
        fm_active_ = false;
        restart_locked();
        SDL_UnlockAudioDevice(device_);
        playing_ = false;
    }
    Stats stats() {
        Stats out;
        if (!device_) return out;
        SDL_LockAudioDevice(device_);
        out.underruns = underruns_;
        out.ratio = rate_control_.ratio();
        out.queued_ms = rate_control_.average() * 1000.0 / double(rate_);
        SDL_UnlockAudioDevice(device_);
        return out;
    }
    void volume(float value) {
        if (device_) SDL_LockAudioDevice(device_);
        volume_ = std::clamp(value, 0.0f, 1.0f);
        if (device_) SDL_UnlockAudioDevice(device_);
    }
    void mute(bool muted) {
        if (device_) SDL_LockAudioDevice(device_);
        muted_ = muted;
        if (device_) SDL_UnlockAudioDevice(device_);
    }
    void close() {
        if (device_) SDL_CloseAudioDevice(device_); // joins callback before freeing streams
        device_ = 0;
        if (fm_) SDL_FreeAudioStream(fm_);
        if (pcm_) SDL_FreeAudioStream(pcm_);
        fm_ = pcm_ = nullptr; playing_ = false;
    }
private:
    static constexpr int kFrameBytes = 2 * int(sizeof(float)); // one stereo float frame
    static constexpr int kChunk = 512;                         // output frames per step
    static constexpr int kMaxInput = kChunk * 2;               // input frames per step (ratio < 2)

    // Under the device lock: silent until kPrimeFrames are queued again.
    void restart_locked() {
        buffering_ = true;
        resampler_.reset();
    }
    // The PCM stream sets the pace (the FM one is empty while the FM is silent).
    int queued_frames_locked() const { return SDL_AudioStreamAvailable(pcm_) / kFrameBytes; }

    int applied_core_mask_ = 0; // the callback thread's fourth-core policy (core_policy.h)
    static void callback(void *userdata, Uint8 *buffer, int bytes) {
        auto &self = *static_cast<Audio *>(userdata);
        apply_core_policy(self.applied_core_mask_);
        std::memset(buffer, 0, size_t(bytes));
        auto *out = reinterpret_cast<int16_t *>(buffer);
        int frames = bytes / (2 * int(sizeof(int16_t)));
        // SDL invokes the callback under the device lock. The producer
        // uses the same lock, including stream clearing and mute changes.
        int queued = self.queued_frames_locked();
        if (self.buffering_) {
            if (queued < AudioRate::kPrimeFrames) return; // silence while the cushion builds up
            self.buffering_ = false;
            self.rate_control_.reset(queued);
        }
        const double ratio = self.rate_control_.update(queued);
        while (frames > 0) {
            const int n = std::min(frames, kChunk);
            const int need = self.resampler_.needed(n, ratio);
            if (need > queued) {
                // Underrun: stop cleanly (the rest stays silent) and re-prime.
                ++self.underruns_;
                self.restart_locked();
                return;
            }
            float *fm = self.fm_in_, *pcm = self.pcm_in_, *mixed = self.mixed_;
            const int got = std::min(need, kMaxInput); // need <= kChunk * (1 + kMaxSpeedUp) + 1
            if (got > 0) {
                SDL_AudioStreamGet(self.pcm_, pcm, got * kFrameBytes);
                // FM queued only while it was not silent: what there is lines up with this PCM.
                const int fm_frames = std::min(got, SDL_AudioStreamAvailable(self.fm_) / kFrameBytes);
                if (fm_frames > 0) {
                    SDL_AudioStreamGet(self.fm_, fm, fm_frames * kFrameBytes);
                    for (int i = 0; i < fm_frames * 2; ++i) pcm[i] += fm[i];
                }
            }
            queued -= got;
            self.resampler_.run(pcm, got, mixed, n, ratio);
            for (int i = 0; i < n * 2; ++i) {
                float sample = self.muted_ ? 0.f : mixed[i] * self.volume_;
                if (!std::isfinite(sample)) sample = 0.f;
                out[i] = int16_t(std::clamp(sample, -1.f, 1.f) * 32767.f);
            }
            frames -= n; out += n * 2;
        }
    }
    SDL_AudioDeviceID device_ = 0;
    SDL_AudioStream *fm_ = nullptr, *pcm_ = nullptr;
    int rate_ = 48000, fm_rate_ = 55556;
    bool fm_active_ = false; // the last FM block was queued (not silent); worker side, under the device lock
    bool playing_ = false, muted_ = false;
    float volume_ = 0.8f;
    // Output pacing, touched by the callback and under the device lock only.
    AudioRate rate_control_;
    StereoResampler resampler_;
    bool buffering_ = true;
    unsigned underruns_ = 0;
    float fm_in_[kMaxInput * 2] = {}, pcm_in_[kMaxInput * 2] = {}, mixed_[kChunk * 2] = {}; // callback scratch
};
} // namespace vita
