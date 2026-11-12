// SDL3 output for the shared native sequencer/mixer. The device clock advances
// audio independently of graphics; only this callback may access the engine.
#pragma once

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace app {

template<class Engine> class NativeAudio {
public:
    static constexpr uint32_t kQueueSize = 16384;
    static constexpr unsigned kChunkFrames = 256;
    struct Stats {
        uint32_t callbacks = 0, frames = 0, queued = 0, overflows = 0, failed = 0;
        uint32_t unsupported = 0, invalid = 0;
    };
    NativeAudio() = default;
    NativeAudio(const NativeAudio &) = delete;
    NativeAudio &operator=(const NativeAudio &) = delete;
    ~NativeAudio() { close(); }

    bool open(std::unique_ptr<Engine> engine) {
        close();
        if (!engine) return false;
        engine_ = std::move(engine);
        read_ = write_ = callbacks_ = frames_ = overflows_ = failed_ = unsupported_ = invalid_ = 0;
        paused_ = true;
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 48000};
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, callback, this);
        if (!stream_) { engine_.reset(); return false; }
        return true; // SDL_OpenAudioDeviceStream starts paused.
    }
    bool available() const { return stream_ != nullptr; }
    bool resume() {
        if (!stream_) return false;
        if (!paused_) return true;
        if (!SDL_ResumeAudioStreamDevice(stream_)) return false;
        paused_ = false; return true;
    }
    bool pause() {
        if (!stream_ || paused_) return true;
        if (!SDL_PauseAudioStreamDevice(stream_)) return false;
        paused_ = true; return true;
    }
    void close() {
        if (stream_) {
            // The setter acquires the stream lock and waits for an in-flight
            // callback. No callback can retain the engine when it is freed.
            SDL_SetAudioStreamGetCallback(stream_, nullptr, nullptr);
            SDL_DestroyAudioStream(stream_); // also closes its device
        }
        stream_ = nullptr;
        paused_ = true;
        engine_.reset();
    }
    void volume(float gain) {
        if (!std::isfinite(gain)) gain = 0;
        gain_.store(uint32_t(std::clamp(gain, 0.f, 1.f) * 65536.f), std::memory_order_relaxed);
    }
    void mute(bool muted) { muted_.store(muted, std::memory_order_relaxed); }
    // Both channels the mean of the two (a mono output setting).
    void mono(bool on) { mono_.store(on, std::memory_order_relaxed); }
    // The music and effects volumes (0..1), for an engine that has them.
    void volumes(float music, float effects) {
        auto level = [](float v) { return uint32_t((std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f) * 65536.f); };
        music_.store(level(music), std::memory_order_relaxed);
        effects_.store(level(effects), std::memory_order_relaxed);
    }

    // Main is the single producer. Overflow rejects the entire packet and
    // must be reported by the caller: losing a note-off is not recoverable.
    bool send(const uint8_t *bytes, size_t size) {
        if (!size) return true;
        if (!stream_ || !bytes || failed_.load(std::memory_order_relaxed)) return false;
        const uint32_t write = write_.load(std::memory_order_relaxed);
        const uint32_t read = read_.load(std::memory_order_acquire);
        if (size > kQueueSize || size > kQueueSize - uint32_t(write - read)) {
            overflows_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        for (size_t i = 0; i < size; ++i) queue_[(write + uint32_t(i)) % kQueueSize] = bytes[i];
        write_.store(write + uint32_t(size), std::memory_order_release);
        return true;
    }
    Stats stats() const {
        Stats s;
        s.callbacks = callbacks_.load(std::memory_order_relaxed);
        s.frames = frames_.load(std::memory_order_relaxed);
        const uint32_t read = read_.load(std::memory_order_acquire);
        s.queued = write_.load(std::memory_order_acquire) - read;
        s.overflows = overflows_.load(std::memory_order_relaxed);
        s.failed = failed_.load(std::memory_order_relaxed);
        s.unsupported = unsupported_.load(std::memory_order_relaxed);
        s.invalid = invalid_.load(std::memory_order_relaxed);
        return s;
    }

private:
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "native audio requires lock-free counters");
    SDL_AudioStream *stream_ = nullptr;
    bool paused_ = true; // main-thread lifecycle state only
    std::unique_ptr<Engine> engine_;
    std::array<uint8_t, kQueueSize> queue_{};
    std::atomic<uint32_t> read_{0}, write_{0}, callbacks_{0}, frames_{0}, overflows_{0}, failed_{0};
    std::atomic<uint32_t> gain_{52428}, muted_{0}, mono_{0}, unsupported_{0}, invalid_{0};
    std::atomic<uint32_t> music_{65536}, effects_{65536};

    void health() {
        if constexpr (requires { engine_->stats().unsupported; engine_->stats().invalid; }) {
            const auto stats = engine_->stats(); // callback owns all engine state
            unsupported_.store(uint32_t(std::min<uint64_t>(stats.unsupported, UINT32_MAX)), std::memory_order_relaxed);
            invalid_.store(uint32_t(std::min<uint64_t>(stats.invalid, UINT32_MAX)), std::memory_order_relaxed);
        }
    }

    void receive() {
        uint32_t read = read_.load(std::memory_order_relaxed);
        const uint32_t write = write_.load(std::memory_order_acquire);
        while (read != write) {
            const uint32_t count = std::min(write - read, kQueueSize - read % kQueueSize);
            engine_->send(queue_.data() + read % kQueueSize, count);
            read += count;
        }
        read_.store(read, std::memory_order_release);
    }
    static void SDLCALL callback(void *context, SDL_AudioStream *stream, int additional, int) noexcept {
        if (additional <= 0) return;
        auto &self = *static_cast<NativeAudio *>(context);
        unsigned remaining = (unsigned(additional) + sizeof(float) * 2 - 1) / (sizeof(float) * 2);
        const float gain = self.muted_.load(std::memory_order_relaxed) ? 0.f
            : float(self.gain_.load(std::memory_order_relaxed)) / 65536.f;
        float mix[kChunkFrames * 2];
        if constexpr (requires { self.engine_->set_volumes(1.f, 1.f); })
            self.engine_->set_volumes(float(self.music_.load(std::memory_order_relaxed)) / 65536.f,
                                      float(self.effects_.load(std::memory_order_relaxed)) / 65536.f);
        try {
            if (!self.failed_.load(std::memory_order_relaxed)) self.receive();
            while (remaining) {
                const unsigned count = std::min(remaining, kChunkFrames);
                std::fill_n(mix, count * 2, 0.f);
                if (!self.failed_.load(std::memory_order_relaxed)) {
                    self.engine_->render(mix, count);
                    for (unsigned i = 0; i < count * 2; ++i)
                        mix[i] = std::isfinite(mix[i]) ? std::clamp(mix[i] * gain, -1.f, 1.f) : 0.f;
                    if (self.mono_.load(std::memory_order_relaxed))
                        for (unsigned i = 0; i < count; ++i) mix[2 * i] = mix[2 * i + 1] = 0.5f * (mix[2 * i] + mix[2 * i + 1]);
                    self.frames_.fetch_add(count, std::memory_order_relaxed);
                }
                if (!SDL_PutAudioStreamData(stream, mix, int(count * sizeof(float) * 2))) {
                    self.failed_.store(1, std::memory_order_relaxed);
                    return;
                }
                remaining -= count;
            }
            self.health();
            self.callbacks_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            // Never unwind through SDL. Main observes the fault and pauses
            // with a reset-required error; subsequent requests receive silence.
            self.failed_.store(1, std::memory_order_relaxed);
            self.health();
        }
    }
};

} // namespace app
