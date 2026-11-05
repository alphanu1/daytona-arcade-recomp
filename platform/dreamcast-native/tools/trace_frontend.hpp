// Host-only trace of the ORIGINAL's front end: one F row per main-loop iteration (sampled when the loop
// at 1240 calls its vblank wait 12a8, after the phase handler 1898 and the object chain 1838 of that
// iteration; lag frames add no row) from the attract to the race entry (phases 2..19) and from the
// results back to the attract (25..30 -> 2), and P rows (the entity pool's carried words) on the
// iterations around the game start. Read-only. Output is ROM-run-derived
// evidence; keep it under ignored build/ directories. Read by platform/dreamcast-native/tests/trace_frontend_native.c
// and trace_race_native.c (frontend mode).
//
// F row: the frame counter 5010b0 (`b0`, one per main-loop iteration: lag frames repeat it), the phase
// bytes, the phase-18 substate 5010f8 and timer 5010a8, the select object 501218 (+62 state, +56/+58/
// +54 timers, +5f, +5e chosen slot), the select outputs (501468 slot, 501460 course, 501464
// transmission, 501a80 time attack), the inputs the select reads (decoded steer 501050 and its previous
// 50105c, accelerator 501054 and its high edge 501068, held buttons 53e120), the sort pass's leading
// lap 5014ba.
// P rows (`P n=ROW id=K ...`): entity K of the car pool (504e00 + K*0x300, K < 44): flags, profile,
// lap byte, position, the carried collision words (+28c/+290 saved XZ, +294/+298 travel, +27d side,
// +2a4 relative) and the updater. Written on the iteration whose active phase is 3 with next 12 (the
// game start), on those with active 12 or 17 (17 falls through into 18's substate 0 in the same
// iteration), and on the first FRONTEND_POOL_SELECT_FRAMES iterations of phase 18.
// The call observer forwards every call to `next_call` (the race trace's observer when both run).
#pragma once
#include "runtime/game_loop.h"

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace daytona::native {

struct FrontendTrace {
    static constexpr unsigned FRONTEND_POOL_SELECT_FRAMES = 4;
    rt::M2Board *board = nullptr;
    std::FILE *file = nullptr;
    uint64_t rows = 0, pool_rows = 0, frame = 0;
    unsigned select_frames = 0;
    void (*next_call)(void *, const rt::Cpu &, uint32_t, int) = nullptr;
    void *next_context = nullptr;

    static void observe_call(void *context, const rt::Cpu &cpu, uint32_t target, int type) {
        auto &self = *static_cast<FrontendTrace *>(context);
        if (!type && target == 0x12a8 && self.file) self.sample();
        if (self.next_call) self.next_call(self.next_context, cpu, target, type);
    }

    void open(rt::M2Board &b, const std::string &path) {
        if (std::filesystem::exists(path)) throw std::runtime_error("front-end trace output already exists; choose a new file");
        file = std::fopen(path.c_str(), "w");
        if (!file) throw std::runtime_error("cannot create front-end trace");
        board = &b;
        std::fputs("# frontend-trace v1 side=reference\n", file);
    }
    ~FrontendTrace() { if (file) std::fclose(file); }

    void pool(uint64_t n) {
        auto &b = *board;
        for (unsigned k = 0; k < 44; ++k) {
            const uint32_t at = 0x504e00 + k * 0x300;
            std::fprintf(file,
                "P n=%" PRIu64 " id=%u flags=%x prof=%u lap=%u x=%08x y=%08x z=%08x spd=%08x sx=%08x sz=%08x tx=%08x tz=%08x"
                " cside=%u crel=%x upd=%x\n",
                n, k, b.read_dword(at), b.read_word(at + 0x128), b.read_byte(at + 0x6e), b.read_dword(at + 0x1c),
                b.read_dword(at + 0x20), b.read_dword(at + 0x24), b.read_dword(at + 0x34), b.read_dword(at + 0x28c),
                b.read_dword(at + 0x290), b.read_dword(at + 0x294), b.read_dword(at + 0x298), b.read_byte(at + 0x27d),
                b.read_dword(at + 0x2a4), b.read_dword(at + 12));
        }
        ++pool_rows;
    }

    // The frame number the input script is consumed at (informational in the rows).
    void set_frame(uint64_t consumed) { frame = consumed; }
    void sample() {
        const uint64_t consumed = frame;
        auto &b = *board;
        const unsigned phase = b.read_byte(0x5010a4), active = b.read_byte(0x5010a0);
        const bool front = (phase >= 2 && phase <= 19) || (active >= 2 && active <= 19) || phase >= 25 || active >= 25;
        if (!front) return;
        const uint32_t sel = b.read_dword(0x501218);
        const bool sel_ok = sel >= 0x500000 && sel < 0x600000;
        std::fprintf(file,
            "F n=%" PRIu64 " frame=%" PRIu64 " b0=%u phase=%u active=%u sub=%u timer=%d selon=%u sel=%u t56=%u t58=%u t54=%u"
            " t5f=%u chosen=%u slot=%u course=%u trans=%u attack=%u steer=%d psteer=%d accel=%u edge=%u held=%x lap=%d\n",
            rows, consumed, b.read_dword(0x5010b0), phase, active, b.read_byte(0x5010f8),
            int(b.read_dword(0x5010a8)), sel_ok ? b.read_dword(sel) >> 31 : 0u, sel_ok ? b.read_byte(sel + 0x62) : 0u,
            sel_ok ? b.read_word(sel + 0x56) : 0u, sel_ok ? b.read_word(sel + 0x58) : 0u, sel_ok ? b.read_word(sel + 0x54) : 0u,
            sel_ok ? b.read_byte(sel + 0x5f) : 0u, sel_ok ? b.read_byte(sel + 0x5e) : 0u, b.read_word(0x501468),
            b.read_byte(0x501460), b.read_byte(0x501464), b.read_dword(0x501a80), int(b.read_dword(0x501050)),
            int(b.read_dword(0x50105c)), b.read_dword(0x501054), b.read_dword(0x501068), b.read_dword(0x53e120),
            int(int8_t(b.read_byte(0x5014ba))));
        if (active == 18) ++select_frames; else select_frames = 0;
        if ((active == 3 && phase == 12) || active == 12 || active == 17 ||
            (active == 18 && select_frames <= FRONTEND_POOL_SELECT_FRAMES))
            pool(rows);
        ++rows;
    }
    void close() {
        if (!file) return;
        const bool failed = std::ferror(file) != 0;
        if (std::fclose(file) || failed) { file = nullptr; throw std::runtime_error("cannot write front-end trace"); }
        file = nullptr;
        std::printf("frontend_trace rows=%" PRIu64 " pool_rows=%" PRIu64 "\n", rows, pool_rows);
    }
};

} // namespace daytona::native
