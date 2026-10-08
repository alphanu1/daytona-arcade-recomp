#pragma once
#include "core_policy.h"

#include <SDL.h>
#include <cstdint>
#include <exception>
#ifdef __vita__
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace vita {

// A single in-flight sound frame. Main owns all submission/completion calls;
// the worker owns execute_deferred_sound and optional audio conversion until
// finish joins both stages. Main must not change or close that audio meanwhile.
// Keep this object alive until after finish, and finish before destroying the
// submitted game. The SDL playback callback still consumes converted samples.
class SoundWorker {
public:
    SoundWorker() = default;
    SoundWorker(const SoundWorker &) = delete;
    SoundWorker &operator=(const SoundWorker &) = delete;
    ~SoundWorker() { close(); }

    bool open() {
        close();
        mutex_ = SDL_CreateMutex();
        ready_ = SDL_CreateCond();
        completed_ = SDL_CreateCond();
        if (!mutex_ || !ready_ || !completed_) { close(); return false; }
        stopping_ = initialized_ = false;
        affinity_before_ = affinity_mask_ = affinity_result_ = 0;
        thread_ = SDL_CreateThreadWithStackSize(entry, "Daytona sound", 512u * 1024u, this);
        if (!thread_) { close(); return false; }
        SDL_LockMutex(mutex_);
        while (!initialized_) SDL_CondWait(completed_, mutex_);
        SDL_UnlockMutex(mutex_);
        return true;
    }
    bool threaded() const { return thread_ != nullptr; }
    // Affinity applied by the next open(): a SCE_KERNEL_CPU_MASK_USER_n mask
    // pins the sound board to one core (main_gpu.cpp: core 2, away from the
    // main and geometry threads); 0 keeps all application cores and follows
    // the fourth-core policy (core_policy.h, --free-core builds).
    void set_cpu_mask(int mask) { requested_mask_ = mask; }
    int affinity_before() const { return affinity_before_; }
    int affinity_mask() const { return affinity_mask_; }
    int affinity_result() const { return affinity_result_; }
    uint64_t last_sound_ticks() const { return last_sound_ticks_; }
    uint64_t last_audio_ticks() const { return last_audio_ticks_; }

    template<class Game>
    void dispatch(Game &game) {
        dispatch(&game,
            [](void *p) { return static_cast<Game *>(p)->execute_deferred_sound(); },
            [](void *p, uint64_t ticks) { static_cast<Game *>(p)->complete_deferred_sound(ticks); });
    }

    template<class Game, class Output>
    void dispatch(Game &game, Output &audio, uint64_t (*clock)()) {
        dispatch(&game,
            [](void *p) { return static_cast<Game *>(p)->execute_deferred_sound(); },
            [](void *p, uint64_t ticks) { static_cast<Game *>(p)->complete_deferred_sound(ticks); },
            &audio, [](void *p, void *output) {
                if (auto *sound = static_cast<Game *>(p)->sound())
                    static_cast<Output *>(output)->push(*sound);
            }, clock);
    }

    // packet must remain alive and unmoved until finish. Unlike dispatch(Game),
    // detached execution/completion never reads or writes GameLoop state, so
    // the owner may prepare the next main-board frame while this runs. Keep at
    // most one active packet plus that next packet; join before re-submission.
    template<class Packet, class Output>
    void dispatch_packet(Packet &packet, Output &audio, uint64_t (*clock)()) {
        dispatch(&packet,
            [](void *p) { return static_cast<Packet *>(p)->execute(); },
            [](void *, uint64_t) {},
            &audio, [](void *p, void *output) {
                if (auto *sound = static_cast<Packet *>(p)->sound())
                    static_cast<Output *>(output)->push(*sound);
            }, clock);
    }

    // Completes profiling on the owner thread and rethrows worker failures
    // there, so the frontend's normal runtime-error path remains effective.
    void finish() {
        if (!thread_) return;
        SDL_LockMutex(mutex_);
        if (!busy_) { SDL_UnlockMutex(mutex_); return; }
        while (!done_) SDL_CondWait(completed_, mutex_);
        void *context = context_;
        Complete complete = complete_;
        const uint64_t ticks = ticks_;
        last_sound_ticks_ = ticks;
        last_audio_ticks_ = queue_ticks_;
        std::exception_ptr error = error_;
        context_ = nullptr;
        error_ = nullptr;
        busy_ = false;
        SDL_UnlockMutex(mutex_);
        complete(context, ticks);
        if (error) std::rethrow_exception(error);
    }

    void close() noexcept {
        // Runtime callers normally use finish so failures reach the UI.
        // Destruction must nevertheless join safely during stack unwinding.
        try { finish(); } catch (...) {}
        if (thread_) {
            SDL_LockMutex(mutex_);
            stopping_ = true;
            SDL_CondSignal(ready_);
            SDL_UnlockMutex(mutex_);
            SDL_WaitThread(thread_, nullptr);
            thread_ = nullptr;
        }
        if (completed_) SDL_DestroyCond(completed_);
        if (ready_) SDL_DestroyCond(ready_);
        if (mutex_) SDL_DestroyMutex(mutex_);
        completed_ = ready_ = nullptr;
        mutex_ = nullptr;
    }

private:
    using Execute = uint64_t (*)(void *);
    using Complete = void (*)(void *, uint64_t);
    using Queue = void (*)(void *, void *);
    using Clock = uint64_t (*)();
    struct Result { uint64_t sound = 0, audio = 0; std::exception_ptr error; };
    SDL_Thread *thread_ = nullptr;
    SDL_mutex *mutex_ = nullptr;
    SDL_cond *ready_ = nullptr, *completed_ = nullptr;
    void *context_ = nullptr;
    Execute execute_ = nullptr;
    Complete complete_ = nullptr;
    void *queue_context_ = nullptr;
    Queue queue_ = nullptr;
    Clock queue_clock_ = nullptr;
    uint64_t queue_ticks_ = 0, last_audio_ticks_ = 0, last_sound_ticks_ = 0;
    uint64_t ticks_ = 0;
    std::exception_ptr error_;
    bool queued_ = false, busy_ = false, done_ = false, stopping_ = false, initialized_ = false;
    int affinity_before_ = 0, affinity_mask_ = 0, affinity_result_ = 0, requested_mask_ = 0;

    static Result run(void *context, Execute execute, void *output, Queue queue, Clock clock) {
        Result result;
        try {
            result.sound = execute(context);
            if (queue) {
                const uint64_t begin = clock ? clock() : 0;
                queue(context, output);
                const uint64_t end = clock ? clock() : 0;
                result.audio = end >= begin ? end - begin : 0;
            }
        } catch (...) { result.error = std::current_exception(); }
        return result;
    }

    void dispatch(void *context, Execute execute, Complete complete,
                  void *output = nullptr, Queue queue = nullptr, Clock clock = nullptr) {
        finish(); // also makes accidental re-submission safe
        if (!thread_) {
            const Result result = run(context, execute, output, queue, clock);
            last_sound_ticks_ = result.sound;
            last_audio_ticks_ = result.audio;
            complete(context, result.sound);
            if (result.error) std::rethrow_exception(result.error);
            return;
        }
        SDL_LockMutex(mutex_);
        context_ = context;
        execute_ = execute;
        complete_ = complete;
        queue_context_ = output;
        queue_ = queue;
        queue_clock_ = clock;
        done_ = false;
        queued_ = busy_ = true;
        SDL_CondSignal(ready_);
        SDL_UnlockMutex(mutex_);
    }

    static int SDLCALL entry(void *opaque) {
        auto &self = *static_cast<SoundWorker *>(opaque);
        int applied_core_mask = 0;
#ifdef __vita__
        const SceUID thread = sceKernelGetThreadId();
        self.affinity_before_ = sceKernelGetThreadCpuAffinityMask(thread);
        // The requested core if any (set_cpu_mask); otherwise permit all
        // application CPUs rather than inheriting the rendering thread's core.
        if (self.requested_mask_) {
            if (self.affinity_before_ != self.requested_mask_)
                self.affinity_result_ = sceKernelChangeThreadCpuAffinityMask(thread, self.requested_mask_);
        } else if (self.affinity_before_ < 0 ||
            (self.affinity_before_ & SCE_KERNEL_CPU_MASK_USER_ALL) != SCE_KERNEL_CPU_MASK_USER_ALL) {
            self.affinity_result_ = sceKernelChangeThreadCpuAffinityMask(thread, SCE_KERNEL_CPU_MASK_USER_ALL);
        }
        self.affinity_mask_ = sceKernelGetThreadCpuAffinityMask(thread);
#endif
        SDL_LockMutex(self.mutex_);
        self.initialized_ = true;
        SDL_CondSignal(self.completed_);
        for (;;) {
            while (!self.queued_ && !self.stopping_) SDL_CondWait(self.ready_, self.mutex_);
            if (self.stopping_) break;
            void *context = self.context_;
            Execute execute = self.execute_;
            void *output = self.queue_context_;
            Queue queue = self.queue_;
            Clock clock = self.queue_clock_;
            self.queued_ = false;
            SDL_UnlockMutex(self.mutex_);
            if (!self.requested_mask_) apply_core_policy(applied_core_mask); // pinned: stays on its core
            const Result result = run(context, execute, output, queue, clock);
            SDL_LockMutex(self.mutex_);
            self.ticks_ = result.sound;
            self.queue_ticks_ = result.audio;
            self.error_ = result.error;
            self.done_ = true;
            SDL_CondSignal(self.completed_);
        }
        SDL_UnlockMutex(self.mutex_);
        return 0;
    }
};

} // namespace vita
