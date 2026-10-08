#pragma once

#include <atomic>
#ifdef __vita__
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#endif

namespace vita {
// Core placement of the GPU builds (main_gpu.cpp). By default each busy thread
// has its own application core: main 0, geometry 1, sound board and vitaGL 2D
// worker 2 (DAYTONA_VITA_*_CORE). scripts/build_vita.py --free-core leaves
// them all unpinned (-1): they then follow core_mask below, so the optional
// fourth core can take any of them. In both builds the SDL audio callbacks
// follow core_mask.
constexpr int core_pin_mask(int core) {
#ifdef __vita__
    return core >= 0 && core <= 2 ? SCE_KERNEL_CPU_MASK_USER_0 << core : 0;
#else
    return core >= 0 && core <= 2 ? 0x10000 << core : 0;
#endif
}

// Firmware/plugin requests must be verified, not inferred from installation.
template<class Set, class Get>
int request_cpu_clock(int requested, Set set, Get get) {
    const int result = set(requested);
    if (requested == 500 && (result < 0 || get() != 500)) return set(444);
    return result;
}

template<class Set, class Get>
bool request_core_mask(int requested, int stock, Set set, Get get) {
    if (set(requested) >= 0 && get() == requested) return true;
    set(stock);
    return false;
}

#ifdef __vita__
inline std::atomic<int> core_mask{SCE_KERNEL_CPU_MASK_USER_ALL};
inline std::atomic<bool> core_rejected{false};
inline int set_cpu_clock(int mhz) {
    return request_cpu_clock(mhz, scePowerSetArmClockFrequency, scePowerGetArmClockFrequency);
}
// home_mask: the calling thread's own core when the build pins it (main_gpu.cpp,
// default build: core 0). The fourth core is probed on that thread, which then
// goes back home: only the threads left free (audio callbacks, and every worker
// of a --free-core build) follow core_mask through apply_core_policy.
inline bool configure_fourth_core(bool enabled, int home_mask = 0) {
    const int stock = SCE_KERNEL_CPU_MASK_USER_ALL;
    const int wanted = stock | (enabled ? SCE_KERNEL_CPU_MASK_SYSTEM : 0);
    const auto thread = sceKernelGetThreadId();
    core_rejected.store(false);
    const bool ok = request_core_mask(wanted, stock,
        [=](int mask) { return sceKernelChangeThreadCpuAffinityMask(thread, mask); },
        [=] { return sceKernelGetThreadCpuAffinityMask(thread); });
    core_mask.store(ok ? wanted : stock);
    core_rejected.store(!ok);
    if (home_mask) sceKernelChangeThreadCpuAffinityMask(thread, home_mask);
    return ok;
}
inline bool fourth_core_active() {
    return (core_mask.load() & SCE_KERNEL_CPU_MASK_SYSTEM) && !core_rejected.load();
}
inline void apply_core_policy(int &applied) {
    const int wanted = core_mask.load(std::memory_order_relaxed);
    if (applied == wanted) return;
    const auto thread = sceKernelGetThreadId();
    if (!request_core_mask(wanted, SCE_KERNEL_CPU_MASK_USER_ALL,
            [=](int mask) { return sceKernelChangeThreadCpuAffinityMask(thread, mask); },
            [=] { return sceKernelGetThreadCpuAffinityMask(thread); }))
        core_rejected.store(true);
    applied = wanted; // Never retry a rejected syscall on every audio callback.
}
#else
inline void apply_core_policy(int &) {}
#endif
} // namespace vita
