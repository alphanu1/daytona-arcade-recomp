// Host-only trace of the bytes the ORIGINAL sends to the drive board (the I/O board's dual-port byte 0x11, 01c00022,
// collected by rt::IoBoard), for dreamcast-native/tests/test_drive_lockstep.py against the native stream
// (include/drive_board.h, tests/drive_trace.h):
//   "D b0=N frame=F byte=XX"         a byte the sender 1abbc wrote in the main-loop iteration whose frame counter
//                                    5010b0 is N (taken at the next call after 1abbc's entry: the loop's vblank wait
//                                    12a8, before the next iteration's 1758 counts on);
//   "D b0=N frame=F byte=XX direct"  a byte written outside the sender (power-on's 0xff from 1bf08, the test mode).
// F is the board frame (informational: the game drops iterations under load). Read-only: the call observer reads RAM
// and the board's command list only, and forwards every call to `next_call` (the front-end or race trace's observer).
// Output is ROM-run-derived evidence; keep it under ignored build/ directories.
#pragma once
#include "runtime/game_loop.h"

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace daytona::native {

struct DriveTrace {
    rt::M2Board *board = nullptr;
    std::FILE *file = nullptr;
    bool sender = false;      // the last observed call was 1abbc's entry
    uint32_t sender_b0 = 0;   // 5010b0 at it
    uint64_t sent = 0, direct = 0;
    void (*next_call)(void *, const rt::Cpu &, uint32_t, int) = nullptr;
    void *next_context = nullptr;

    static void observe_call(void *context, const rt::Cpu &cpu, uint32_t target, int type) {
        auto &self = *static_cast<DriveTrace *>(context);
        if (self.file) {
            self.flush();
            self.sender = target == 0x1abbc;
            if (self.sender) self.sender_b0 = self.board->read_dword(0x5010b0);
        }
        if (self.next_call) self.next_call(self.next_context, cpu, target, type);
    }

    void open(rt::M2Board &b, const std::string &path) {
        if (std::filesystem::exists(path)) throw std::runtime_error("drive trace output already exists; choose a new file");
        file = std::fopen(path.c_str(), "w");
        if (!file) throw std::runtime_error("cannot create drive trace");
        board = &b;
        board->io().drive_commands.clear();
        std::fputs("# drive-trace v1 side=reference\n", file);
    }
    ~DriveTrace() { if (file) std::fclose(file); }

    // The bytes the board collected since the last call, each once (the list is emptied: it keeps at most kMaxDrive).
    void flush() {
        auto &bytes = board->io().drive_commands;
        for (const uint8_t c : bytes) {
            const uint32_t b0 = sender ? sender_b0 : board->read_dword(0x5010b0);
            std::fprintf(file, "D b0=%u frame=%" PRIu64 " byte=%02x%s\n", b0, board->frame(), c, sender ? "" : " direct");
            ++(sender ? sent : direct);
        }
        bytes.clear();
    }
    void close() {
        if (!file) return;
        flush();
        const bool failed = std::ferror(file) != 0;
        if (std::fclose(file) || failed) { file = nullptr; throw std::runtime_error("cannot write drive trace"); }
        file = nullptr;
        std::printf("drive_trace sent=%" PRIu64 " direct=%" PRIu64 "\n", sent, direct);
    }
};

} // namespace daytona::native
