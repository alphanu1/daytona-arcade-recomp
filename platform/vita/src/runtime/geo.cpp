// PS Vita replacement of src/runtime/geo.cpp (see geo.h in this directory):
// 1. the unmodified src/runtime/geo.cpp, compiled as rt::GeoCore;
// 2. rt::Geo, which runs GeoCore::parse on a thread pinned to its own core.

// Standard and SDK headers first, so the rename below never reaches them.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include "../../core_policy.h" // unpinned (--free-core): the fourth-core policy

// ---- 1. The original geometrizer as rt::GeoCore --------------------------------
#define DAYTONA_VITA_GEO_CORE_TU 1
#include "../../../../src/runtime/geo.cpp" // its "runtime/geo.h" is the Vita one: renames Geo
#undef Geo
#undef DAYTONA_VITA_GEO_CORE_TU

// ---- 2. rt::Geo -------------------------------------------------------------------
#include "runtime/geo.h" // now declares the wrapper

namespace rt {

namespace {
// Boards alive (normally one), so the frontend reaches a board's geometrizer
// from its TGP buffer RAM without changing M2Board.
std::vector<Geo *> &registry() {
    static std::vector<Geo *> geos;
    return geos;
}

// VFP status/control (rounding, flush-to-zero, default NaN): copied from the
// main thread so the parse computes exactly as it would there.
uint32_t read_fpscr() {
#if defined(__arm__) && !defined(__SOFTFP__)
    uint32_t value;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(value));
    return value;
#else
    return 0;
#endif
}
void write_fpscr(uint32_t value) {
#if defined(__arm__) && !defined(__SOFTFP__)
    __asm__ volatile("vmsr fpscr, %0" : : "r"(value));
#else
    (void)value;
#endif
}
} // namespace

const char *geo_mode_name(GeoMode mode) {
    switch (mode) {
    case GeoMode::Sync: return "SYNC";
    case GeoMode::Pipelined: return "ASYNC_PIPELINED";
    }
    return "?";
}

Geo::Geo(const std::vector<uint8_t> &polygons, const std::vector<uint8_t> &textures, uint32_t *buffer)
    : live_(buffer), snapshot_(new uint32_t[kBufferWords]), core_(polygons, textures, snapshot_.get()) {
    std::memcpy(snapshot_.get(), live_, kBufferWords * sizeof(uint32_t));
    registry().push_back(this);
}

Geo::~Geo() {
    try { wait(); } catch (...) {}
    stop_thread();
    auto &geos = registry();
    geos.erase(std::remove(geos.begin(), geos.end(), this), geos.end());
}

Geo *Geo::find(const uint32_t *live_buffer) {
    for (Geo *geo : registry())
        if (geo->live_ == live_buffer) return geo;
    return nullptr;
}

// ---- Geometry thread ------------------------------------------------------------

int geo_thread_entry(unsigned, void *argp) {
    Geo &self = **static_cast<Geo **>(argp);
    write_fpscr(self.worker_.fpscr_main);
    self.worker_.fpscr_worker = read_fpscr();
    self.worker_.affinity_mask = sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
    sceKernelSignalSema(self.done_sema_, 1); // start_thread continues
    int applied_core_mask = 0;
    for (;;) {
        sceKernelWaitSema(self.start_sema_, 1, nullptr);
        if (self.stop_.load()) break;
        if (self.worker_.core < 0) vita::apply_core_policy(applied_core_mask); // pinned: stays on its core
        Geo::run_job(self); // never throws
        sceKernelSignalSema(self.done_sema_, 1);
    }
    return 0;
}

bool Geo::start_thread(int core) {
    if (thread_ >= 0) return true;
    worker_ = {};
    worker_.core = core;
    worker_.fpscr_main = read_fpscr();
    stop_.store(false);
    start_sema_ = sceKernelCreateSema("daytona_geo_start", 0, 0, 1, nullptr);
    done_sema_ = sceKernelCreateSema("daytona_geo_done", 0, 0, 1, nullptr);
    if (start_sema_ < 0 || done_sema_ < 0) {
        worker_.create_result = start_sema_ < 0 ? start_sema_ : done_sema_;
        stop_thread();
        return false;
    }
    // Same priority as the main thread: neither starves the other.
    int priority = sceKernelGetThreadCurrentPriority();
    if (priority < 0) priority = 0x10000100; // default user priority
    worker_.priority = priority;
    const int mask = core >= 0 && core <= 2 ? SCE_KERNEL_CPU_MASK_USER_0 << core : SCE_KERNEL_CPU_MASK_USER_ALL;
    thread_ = sceKernelCreateThread("daytona_geo", reinterpret_cast<SceKernelThreadEntry>(&geo_thread_entry),
                                    priority, 256 * 1024, 0, mask, nullptr);
    worker_.create_result = thread_;
    if (thread_ < 0) { thread_ = -1; stop_thread(); return false; }
    Geo *self = this;
    const int started = sceKernelStartThread(thread_, sizeof(self), &self);
    if (started < 0) {
        worker_.create_result = started;
        sceKernelDeleteThread(thread_);
        thread_ = -1;
        stop_thread();
        return false;
    }
    sceKernelWaitSema(done_sema_, 1, nullptr); // the thread has copied FPSCR and reported its mask
    worker_.threaded = true;
    return true;
}

void Geo::stop_thread() noexcept {
    if (thread_ >= 0) {
        stop_.store(true);
        sceKernelSignalSema(start_sema_, 1);
        sceKernelWaitThreadEnd(thread_, nullptr, nullptr);
        sceKernelDeleteThread(thread_);
        thread_ = -1;
    }
    if (start_sema_ >= 0) sceKernelDeleteSema(start_sema_);
    if (done_sema_ >= 0) sceKernelDeleteSema(done_sema_);
    start_sema_ = done_sema_ = -1;
    worker_.threaded = false;
}

bool Geo::configure(GeoMode mode, int core, Clock clock) {
    wait();
    clock_ = clock;
    if (mode != GeoMode::Sync && !start_thread(core)) mode = GeoMode::Sync;
    if (mode == GeoMode::Sync) publish(); // the newest parse is the shown list, as on the reference
    mode_ = mode;
    return mode == GeoMode::Sync || worker_.threaded;
}

// Geometry thread (or the main thread in Sync). Touches only core_, the
// snapshot and the job_* fields.
void Geo::run_job(Geo &self) {
    const uint64_t begin = self.now();
    try {
        self.core_.zclip_w(self.job_zclip_);
        self.core_.parse(self.job_read_start_);
    } catch (...) {
        self.job_error_ = std::current_exception();
    }
    self.job_count_ = uint32_t(self.core_.polys.size());
    self.job_windows_ = self.core_.windows();
    self.job_end_ = self.now();
    self.job_ticks_ = self.job_end_ >= begin ? self.job_end_ - begin : 0;
}

void Geo::wait() {
    if (!in_flight_) return;
    const uint64_t begin = now();
    bool blocked = false;
    if (sceKernelPollSema(done_sema_, 1) < 0) { // still parsing
        blocked = true;
        sceKernelWaitSema(done_sema_, 1, nullptr);
    }
    const uint64_t end = now();
    in_flight_ = false;
    const uint64_t waited = end >= begin ? end - begin : 0;
    stats_.wait_ticks += waited;
    stats_.wait_max = std::max(stats_.wait_max, waited);
    if (blocked) ++stats_.blocked_waits;
    ++stats_.jobs;
    stats_.worker_ticks += job_ticks_;
    const uint64_t latency = job_end_ >= job_start_ ? job_end_ - job_start_ : 0;
    stats_.latency_ticks += latency;
    stats_.latency_max = std::max(stats_.latency_max, latency);
    stats_.polys += job_count_;
    stats_.polys_max = std::max<uint64_t>(stats_.polys_max, job_count_);
    latest_count_ = job_count_;
    if (job_error_) {
        ++stats_.failures;
        pending_error_ = std::exchange(job_error_, nullptr);
    }
}

void Geo::rethrow_pending() {
    if (pending_error_) std::rethrow_exception(std::exchange(pending_error_, nullptr));
}

void Geo::publish() {
    if (!unpublished_) return;
    front_.swap(core_.polys); // GeoCore keeps the old capacity for its next parse
    front_windows_ = job_windows_;
    unpublished_ = false;
}

// ---- Reference interface (M2Board) ------------------------------------------------

void Geo::parse(uint32_t read_start) {
    stats_.last_parse_call = now();
    ++stats_.parse_calls;
    wait(); // the previous parse; normally finished long ago
    rethrow_pending();
    if (mode_ == GeoMode::Pipelined) publish(); // the previous parse becomes the shown list
    const uint64_t t0 = now();
    std::memcpy(snapshot_.get(), live_, kBufferWords * sizeof(uint32_t));
    const uint64_t t1 = now();
    stats_.snapshot_ticks += t1 - t0;
    job_read_start_ = read_start;
    job_zclip_ = zclip_;
    job_start_ = t1;
    job_age_ = 0;
    pair_ = Read::None;
    unpublished_ = true;
    if (mode_ == GeoMode::Sync) {
        run_job(*this);
        stats_.inline_ticks += job_ticks_;
        stats_.polys += job_count_;
        stats_.polys_max = std::max<uint64_t>(stats_.polys_max, job_count_);
        latest_count_ = job_count_;
        if (job_error_) {
            ++stats_.failures;
            unpublished_ = false;
            std::rethrow_exception(std::exchange(job_error_, nullptr));
        }
        publish();
    } else {
        in_flight_ = true;
        sceKernelSignalSema(start_sema_, 1);
    }
    stats_.last_parse_return = now();
}

void Geo::set_wide_margin(int pixels) {
    wait(); // the parse reads the margin
    core_.set_wide_margin(pixels);
}

void Geo::output_read(Read kind) {
    if (pair_ != Read::None && pair_ != kind) {
        pair_ = Read::None; // the second read of the same vblank end
        return;
    }
    pair_ = kind;
    stats_.last_output_call = now();
    if (mode_ == GeoMode::Pipelined && unpublished_) {
        // Shown one vblank end after its parse started: normally published by
        // the next parse(); at 30 Hz (no parse on odd frames) by this read.
        if (job_age_ >= 1) {
            wait();
            rethrow_pending();
            publish();
        } else {
            ++job_age_;
        }
    }
}

int Geo::windows() const {
    auto &self = const_cast<Geo &>(*this); // Geo objects are never const (owned by M2Board)
    self.output_read(Read::Windows);
    return front_windows_;
}

size_t Geo::PolyList::size() const {
    // polygon_count_r: the newest parse's count, as the reference reports it.
    ++geo_->stats_.count_reads;
    geo_->wait();
    geo_->rethrow_pending();
    return geo_->latest_count_;
}

Geo::PolyList::operator const std::vector<GeoPoly> &() const {
    geo_->output_read(Read::List);
    return geo_->front_;
}

} // namespace rt
