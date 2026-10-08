// PS Vita replacement of src/runtime/geo.h: the geometrizer on its own core.
//
// The Vita build puts platform/vita/src before src on the include path, so
// every "runtime/geo.h" of the shared runtime resolves here, and geo.cpp of
// this directory replaces src/runtime/geo.cpp. The original header and
// geo.cpp are used unmodified, compiled under the name rt::GeoCore. rt::Geo
// below is a thin wrapper with exactly the interface M2Board uses
// (constructor, zclip_w, parse, polys, windows, set_wide_margin): nothing in
// src/ changes. The wrapper runs GeoCore::parse on its own thread, pinned to
// one Vita core:
//
//   * parse() (vblank start) copies buffer RAM (0x8000 dwords) into a private
//     snapshot, the only buffer GeoCore ever reads, and starts the parse. The
//     i960/TGP keep running on the main core and may rewrite the live RAM.
//   * GeoCore's persistent state is touched only by the parse while it runs;
//     every other access joins it first.
//   * Output is double buffered: GeoCore::polys is the back list, the shown
//     list is swapped in (published), never copied.
//   * polys.size() (the polygon count register read by the i960) joins the
//     newest parse and returns its count, as the reference does.
//
// Two modes, nothing in between:
//   Sync       single core: the parse on the main core, the reference image
//              (the CPU build, main.cpp, and the fallback without a thread).
//   Pipelined  two cores (the GPU builds, main_gpu.cpp): shown list = the
//              previous parse; the parse of frame N overlaps the whole next game
//              frame. Game-visible state is unchanged; the vitaGL renderer shows
//              the 2D layers (HUD, sky tilemap) one frame late too, in phase.
#ifndef DAYTONA_VITA_GEO_CORE_INCLUDED
#define DAYTONA_VITA_GEO_CORE_INCLUDED

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <vector>

// The unmodified geometrizer, renamed rt::GeoCore. (Standard headers above are
// included first so the macro never reaches them.)
#define Geo GeoCore
#include "../../../../src/runtime/geo.h"
#ifndef DAYTONA_VITA_GEO_CORE_TU // geo.cpp keeps the rename while it compiles the original
#undef Geo
#endif

#endif // DAYTONA_VITA_GEO_CORE_INCLUDED

#if !defined(DAYTONA_VITA_GEO_CORE_TU) && !defined(DAYTONA_VITA_GEO_WRAPPER_INCLUDED)
#define DAYTONA_VITA_GEO_WRAPPER_INCLUDED
namespace rt {

enum class GeoMode : uint8_t { Sync = 0, Pipelined = 1 };
const char *geo_mode_name(GeoMode mode);

class Geo {
public:
    using Clock = uint64_t (*)();
    static constexpr uint32_t kBufferWords = 0x8000;

    // Cumulative counters, main thread only. Worker values are folded in when
    // the parse is joined. Timestamps use the configured clock (0 without one).
    struct Stats {
        uint64_t parse_calls = 0;      // parse() calls (vblank starts that parse)
        uint64_t jobs = 0;             // parses run on the geometry core
        uint64_t worker_ticks = 0;     // parse time on the geometry core
        uint64_t inline_ticks = 0;     // parse time on the main core (Sync)
        uint64_t latency_ticks = 0;    // start -> parse finished
        uint64_t latency_max = 0;
        uint64_t snapshot_ticks = 0;   // buffer RAM copies (main core)
        uint64_t wait_ticks = 0;       // main core time spent in joins
        uint64_t wait_max = 0;
        uint64_t blocked_waits = 0;    // joins where the parse was still running
        uint64_t count_reads = 0;      // polygon count register reads
        uint64_t polys = 0;            // polygons produced
        uint64_t polys_max = 0;
        uint64_t failures = 0;         // parses that threw
        uint64_t last_parse_call = 0, last_parse_return = 0; // last vblank start (parse)
        uint64_t last_output_call = 0; // last vblank end (list read by the board)
    };
    // The geometry thread, as created by configure().
    struct Worker {
        bool threaded = false;
        int core = -1, create_result = 0, affinity_mask = 0, priority = 0;
        uint32_t fpscr_main = 0, fpscr_worker = 0;
    };

    // Same signature as the reference: buffer is the live buffer RAM.
    Geo(const std::vector<uint8_t> &polygons, const std::vector<uint8_t> &textures, uint32_t *buffer);
    ~Geo(); // joins the parse and stops the thread
    Geo(const Geo &) = delete;
    Geo &operator=(const Geo &) = delete;

    // ---- The reference interface, as M2Board uses it ----
    void parse(uint32_t read_start);
    void zclip_w(uint32_t data) { zclip_ = data; } // used by the next parse, as on the reference
    void set_wide_margin(int pixels);
    int windows() const;                           // window count of the shown list
    class PolyList {
    public:
        size_t size() const;                           // newest parse's count (joins)
        operator const std::vector<GeoPoly> &() const; // the shown list
    private:
        friend class Geo;
        explicit PolyList(Geo *geo) : geo_(geo) {}
        Geo *geo_;
    };
    PolyList polys{this};

    // ---- Vita control (main thread) ----
    // The geometrizer of the board whose TGP buffer RAM is `live_buffer`.
    static Geo *find(const uint32_t *live_buffer);
    // Joins first. Pipelined creates the geometry thread on the first call,
    // pinned to `core` (0-2; < 0: any application core). Returns false (and
    // stays Sync) if the thread cannot be created.
    bool configure(GeoMode mode, int core, Clock clock);
    GeoMode mode() const { return mode_; }
    const Stats &stats() const { return stats_; }
    const Worker &worker() const { return worker_; }
    // Join the running parse without publishing it (failures kept for later).
    void wait();

private:
    void rethrow_pending();
    void publish();
    bool start_thread(int core);
    void stop_thread() noexcept;
    // vblank end reads the list and the window count once each (either order).
    enum class Read : uint8_t { None, List, Windows };
    void output_read(Read kind);
    uint64_t now() const { return clock_ ? clock_() : 0; }
    static void run_job(Geo &self);
    friend int geo_thread_entry(unsigned, void *);

    uint32_t *live_;
    std::unique_ptr<uint32_t[]> snapshot_; // declared before core_: constructed first
    GeoCore core_;                         // reads only snapshot_
    GeoMode mode_ = GeoMode::Sync;
    Clock clock_ = nullptr;
    uint32_t zclip_ = 0;
    bool in_flight_ = false, unpublished_ = false;
    unsigned job_age_ = 0; // vblank ends seen since the unpublished parse started
    Read pair_ = Read::None;
    std::vector<GeoPoly> front_;
    int front_windows_ = 0;
    uint32_t latest_count_ = 0;
    std::exception_ptr pending_error_;
    Stats stats_;
    Worker worker_;
    // Geometry thread (kernel UIDs) and the job it runs.
    int thread_ = -1, start_sema_ = -1, done_sema_ = -1;
    std::atomic<bool> stop_{false};
    uint32_t job_read_start_ = 0, job_zclip_ = 0;
    uint64_t job_start_ = 0, job_end_ = 0, job_ticks_ = 0;
    uint32_t job_count_ = 0;
    int job_windows_ = 0;
    std::exception_ptr job_error_;
};

} // namespace rt
#endif // wrapper
