// Host-only whole-race trace of the ORIGINAL game: one row set per board frame
// while the race phases are selected. Read-only: never writes guest memory,
// except on request: M2_POKE_ROW=N M2_POKE_SPEED=HEX writes the player's speed
// (+34) at its block entry (d834) in the frame of trace row N, a test input for
// states live play reaches rarely (trace_race_native --poke-speed N:HEX).
// Output is ROM-run-derived evidence; keep it under ignored build/ directories.
// Entity offsets are the ones already proven by check_player.hpp/check_race.hpp
// (PlayerMemory::fields, checkpoint_fields, traffic_order_fields).
// The same text format is written by platform/dreamcast-native/tests/trace_race_native.c
// and compared by platform/dreamcast-native/tools/compare_race_trace.py.
#pragma once
#include "runtime/game_loop.h"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <cstdlib>

namespace daytona::native {

struct RaceTrace {
    rt::M2Board *board = nullptr;
    std::FILE *file = nullptr;
    uint64_t rows = 0, frames_seen = 0;
    unsigned previous_phase = 256, previous_active = 256;
    // Per-frame observations from the read-only call/return observers.
    unsigned normal_entries = 0, rolling_entries = 0, finish_entries = 0;
    int32_t entry_steer = 0;
    unsigned entry_accel = 0, entry_brake = 0;
    uint32_t entry_remaining = 0;
    std::array<uint32_t,16> entry_cells{}; // Bitmap501520 as the player's update sees it.
    bool entry_valid = false;
    // --trace-clock: the calls of 1786c the frame-counter animations' ticks make (include/clock_actors.h;
    // tests/test_clock_lockstep.py): "D frame ip task list 5010b0" a call, "F frame 5010b0 501090 5010a4" a frame.
    std::FILE *clock_file = nullptr;
    bool expert_clock = false;   // --trace-clock-expert: also Expert's actors (X and V lines, the longer F line)
    uint64_t running = 0;   // the frame passed to the input script, set before each run_frame
    // --trace-props: every tick of the course props in execution order, as a binary run of tests/test_props.c: records
    // tag, frame (u64), body size (u32), body. C the course and I 222d30 at the manager's init (a scene's first tick),
    // M the manager's tick (the player entity 5012f0 and the view 501560), T a prop's tick (its task and the draw
    // bitmap 501520), X a prop's draw (its task, 1, the TGP's matrix), S a call of 19bdc (the command), E the sound
    // manager's entry (the tasks from the manager 50ef80 to its own, then 503600..50367f), then I 22302c.
    std::FILE *props_file = nullptr;
    uint32_t props_body[2048];
    // --trace-attract: rows also in the attract loop (phases 2..11): the cinematic (3..5) and the V.R. demo (6, 7)
    // with the pool's cars, the title and the rest (2, 8..11) with the T row alone (the timeline, 5010b0, course).
    bool attract = false;

    void open_clock(const std::string &path) {
        if (std::filesystem::exists(path)) throw std::runtime_error("clock trace output already exists; choose a new file");
        clock_file = std::fopen(path.c_str(), "w");
        if (!clock_file) throw std::runtime_error("cannot create clock trace");
    }
    void open_props(const std::string &path) {
        if (std::filesystem::exists(path)) throw std::runtime_error("props trace output already exists; choose a new file");
        props_file = std::fopen(path.c_str(), "wb");
        if (!props_file) throw std::runtime_error("cannot create props trace");
    }
    void props_record(char tag, const uint32_t *body, uint32_t words) {
        const uint64_t frame = running; const uint32_t bytes = words*4;
        std::fputc(tag, props_file);
        std::fwrite(&frame, 8, 1, props_file); std::fwrite(&bytes, 4, 1, props_file);
        std::fwrite(body, 4, words, props_file);
    }
    uint32_t props_words(uint32_t *out, uint32_t from, uint32_t bytes) {
        for (uint32_t i = 0; i < bytes/4; ++i) out[i] = board->read_dword(from+4*i);
        return bytes/4;
    }
    void props_call(const rt::Cpu &cpu, uint32_t target) {
        const uint32_t task = cpu.m_r[29];
        uint32_t *b = props_body;
        if (target == 0x222d30) {
            b[0] = board->read_byte(0x501460); props_record('C', b, 1);
            b[0] = target; props_record('I', b, 1);
        } else if (target == 0x222dd0) {
            const uint32_t n = props_words(b, board->read_dword(0x5012f0), 0x340);
            props_record('M', b, n + props_words(b+n, 0x501560, 48));
        } else if (target == 0x2231c0) {
            b[0] = task; props_record('T', b, 1 + props_words(b+1, 0x501520, 64));
        } else if (target == 0x1786c && cpu.m_IP >= 0x2231c0 && cpu.m_IP < 0x223620) {
            b[0] = task; b[1] = 1; tgp_matrix(b+2); props_record('X', b, 14);
        } else if (target == 0x19bdc) { b[0] = cpu.m_r[16]; props_record('S', b, 1); }
        else if (target == 0x22302c) {
            const uint32_t n = props_words(b, 0x50ef80, task + 0x80 - 0x50ef80);
            props_record('E', b, n + props_words(b+n, 0x503600, 0x80));
            b[0] = target; props_record('I', b, 1);
        }
    }
    static bool clock_code(uint32_t ip) {
        // The ticks of 220304, 2209e4, 220a7c, 221f10, 222030, 222090, 222c48, 222cf0 (prepare_world_objects.py).
        static const uint32_t ranges[][2] = {{0x220320, 0x2203a4}, {0x220a00, 0x220a7c}, {0x220a98, 0x220b14},
            {0x221f30, 0x222030}, {0x222050, 0x222090}, {0x2220b0, 0x22214c}, {0x222c64, 0x222cf0}, {0x222d10, 0x222d30}};
        for (auto &r : ranges) if (ip >= r[0] && ip < r[1]) return true;
        return false;
    }
    // Expert's world actors (include/expert_actors.h): the code of each handler's ticks, every state of it, and the
    // statue routine 222288 of the sound manager. Their draws are logged with the TGP's matrix at the call
    // ("X frame ip task list counter x z heading m0..m11 w0..w6": the task's position and heading words, the
    // matrix the list is drawn with, the task words +c, +34, +38, +3c, +40, +44, +48), and the matrix each tick
    // starts from, the view ("V frame target m0..m11", at every such tick: a camera task may run between them).
    static bool expert_code(uint32_t ip) {
        static const uint32_t ranges[][2] = {{0x2210ac, 0x221d24}, {0x22216c, 0x222264}, {0x222288, 0x2224b0},
            {0x2225b4, 0x222678}, {0x222698, 0x22275c}, {0x22277c, 0x222840}, {0x222860, 0x222924},
            {0x223be0, 0x223cc8}, {0x223cfc, 0x223d44}, {0x223d94, 0x22430c}, {0x22432c, 0x224730},
            {0x22475c, 0x224854}, {0x22487c, 0x224cd0}, {0x224cec, 0x224de8}};
        for (auto &r : ranges) if (ip >= r[0] && ip < r[1]) return true;
        return false;
    }
    static bool expert_tick(uint32_t target) {
        static const uint32_t ticks[] = {0x2210ac, 0x22216c, 0x222288, 0x2225b4, 0x222698, 0x22277c, 0x222860, 0x223be0,
            0x223cfc, 0x223d94, 0x22432c, 0x22475c, 0x22487c, 0x224ab0, 0x224b1c, 0x224b60, 0x224cec};
        for (uint32_t t : ticks) if (target == t) return true;
        return false;
    }
    // The TGP's current matrix (command 11: twelve words out), read between the game's commands: the TGP program
    // runs when its output is read (m2_tgp_board.h), after every command the i960 has written, so this changes no
    // state of the game.
    void tgp_matrix(uint32_t out[12]) {
        board->write_dword(0x880110, 0x1111);
        for (unsigned i = 0; i < 12; ++i) out[i] = board->read_dword(0x884000);
    }

    void open(rt::M2Board &b, const std::string &path) {
        if (std::filesystem::exists(path)) throw std::runtime_error("race trace output already exists; choose a new file");
        file = std::fopen(path.c_str(), "w");
        if (!file) throw std::runtime_error("cannot create race trace");
        board = &b;
        std::fputs("# race-trace v1 side=reference\n", file);
    }
    ~RaceTrace() { if (file) std::fclose(file); if (clock_file) std::fclose(clock_file); if (props_file) std::fclose(props_file); }

    void capture_entry() {
        auto &b = *board;
        entry_steer = std::bit_cast<int32_t>(b.read_dword(0x501050));
        entry_accel = b.read_word(0x501054); entry_brake = b.read_word(0x501058);
        entry_remaining = b.read_dword(0x5010d0);
        for (unsigned i = 0; i < 16; ++i) entry_cells[i] = b.read_dword(0x501520+4*i);
        entry_valid = true;
    }
    // Same entry points LiveDriveCheck uses: rolling d354->d3f4, normal player
    // block entered when e938 returns to d834.
    static void observe_call(void *context, const rt::Cpu &cpu, uint32_t target, int type) {
        if (type) return;
        auto &self = *static_cast<RaceTrace *>(context);
        if (self.props_file) self.props_call(cpu, target);
        if (target == 0xd3f4 && cpu.m_IP == 0xd354 && cpu.m_r[29] == self.board->read_dword(0x501220)) {
            ++self.rolling_entries; self.capture_entry();
        }
        if (self.clock_file && target == 0x1786c && clock_code(cpu.m_IP))
            std::fprintf(self.clock_file, "D %" PRIu64 " %x %x %x %u\n", self.running, cpu.m_IP, cpu.m_r[29], cpu.m_r[16],
                         self.board->read_dword(0x5010b0));
        if (self.clock_file && self.expert_clock && expert_tick(target)) {
            uint32_t m[12]; self.tgp_matrix(m);
            std::fprintf(self.clock_file, "V %" PRIu64 " %x", self.running, target);
            for (uint32_t w : m) std::fprintf(self.clock_file, " %08x", w);
            std::fputc('\n', self.clock_file);
        }
        if (self.clock_file && self.expert_clock && target == 0x1786c && expert_code(cpu.m_IP)) {
            const uint32_t task = cpu.m_r[29];
            uint32_t m[12]; self.tgp_matrix(m);
            std::fprintf(self.clock_file, "X %" PRIu64 " %x %x %x %u %08x %08x %d", self.running, cpu.m_IP, task, cpu.m_r[16],
                self.board->read_dword(0x5010b0), self.board->read_dword(task+0x1c), self.board->read_dword(task+0x24),
                int(std::bit_cast<int16_t>(self.board->read_word(task+0x2a))));
            for (uint32_t w : m) std::fprintf(self.clock_file, " %08x", w);
            for (uint32_t at : {0xcu, 0x34u, 0x38u, 0x3cu, 0x40u, 0x44u, 0x48u})
                std::fprintf(self.clock_file, " %08x", self.board->read_dword(task+at));
            std::fputc('\n', self.clock_file);
        }
    }
    static void observe_return(void *context, const rt::Cpu &cpu, uint32_t source) {
        auto &self = *static_cast<RaceTrace *>(context);
        if (source == 0xe938 && cpu.m_IP == 0xd834 && cpu.m_r[29] == self.board->read_dword(0x501220)) {
            ++self.normal_entries; self.capture_entry();
            if (const char *row = std::getenv("M2_POKE_ROW"))
                if (std::strtoull(row,nullptr,10) == self.rows) {
                    const uint32_t bits = uint32_t(std::strtoul(std::getenv("M2_POKE_SPEED"),nullptr,16));
                    self.board->write_dword(self.board->read_dword(0x501220)+0x34,bits);
                    std::printf("race_trace_poke row=%llu speed=%08x\n",(unsigned long long)self.rows,bits);
                }
        }
    }

    static bool entity_address(uint32_t at) { return at >= 0x500000 && at <= 0x5ffd00 && !(at & 3); }

    void car(uint32_t at, unsigned slot) {
        auto &b = *board;
        std::fprintf(file,
            "C n=%" PRIu64 " id=%u slot=%u upd=%x flags=%x prof=%u prev=%u far=%u lap=%u done=%u cnt=%u rank=%u"
            " key=%u keyhi=%u si=%d spd=%08x x=%08x y=%08x z=%08x yaw=%u head=%u path=%u lat=%d gear=%u rpm=%d"
            " acc=%u brk=%x str=%d want=%08x road=%u ms=%08x gs=%08x surf=%x mask=%x sx=%08x sz=%08x tx=%08x tz=%08x cside=%u crel=%x"
            " dmg=%x body=%x crashes=%u tilt=%d pitch=%d\n",
            rows, b.read_byte(at+4), slot, b.read_dword(at+12), b.read_dword(at), b.read_word(at+0x128),
            b.read_word(at+0x12a), b.read_word(at+0x12e), b.read_byte(at+0x6e), b.read_byte(at+0x6d), b.read_byte(at+0x6c),
            b.read_byte(at+0x6a), b.read_word(at+0x60), b.read_word(at+0x62),
            std::bit_cast<int32_t>(b.read_dword(at+0x18)), b.read_dword(at+0x34),
            b.read_dword(at+0x1c), b.read_dword(at+0x20), b.read_dword(at+0x24), b.read_word(at+0x58), b.read_word(at+0x48),
            b.read_word(at+0x134), std::bit_cast<int16_t>(b.read_word(at+0x13e)), b.read_dword(at+0x148),
            std::bit_cast<int32_t>(b.read_dword(at+0x150)), b.read_word(at+0xac), b.read_dword(at+0xa8),
            std::bit_cast<int32_t>(b.read_dword(at+0xb0)), b.read_dword(at+0x44), b.read_dword(at+0x30),
            b.read_dword(at+0x1f8), b.read_dword(at+0x238), b.read_dword(at+0x1a4), b.read_word(at+0x1b8),
            b.read_dword(at+0x28c), b.read_dword(at+0x290), b.read_dword(at+0x294), b.read_dword(at+0x298),
            b.read_byte(at+0x27d), b.read_dword(at+0x2a4),
            // Damage (spec-damage.md): +208 corner/wheel/crash bits, +14 body list, +c7 crash count, the
            // shake 501844 (shared) and the pitch +52 it feeds (11b7c).
            b.read_dword(at+0x208), b.read_dword(at+0x14), b.read_byte(at+0xc7),
            int(std::bit_cast<int16_t>(b.read_word(0x501844))), int(std::bit_cast<int16_t>(b.read_word(at+0x52))));
    }

    // Call after run_frame. `consumed` is the frame number passed to the input script.
    void sample(uint64_t consumed) {
        auto &b = *board;
        if (clock_file && !expert_clock)
            std::fprintf(clock_file, "F %" PRIu64 " %u %x %u\n", consumed, b.read_dword(0x5010b0), b.read_dword(0x501090),
                         b.read_byte(0x5010a4));
        // With Expert's actors also the statue's shared words 5036fc/5036fe, the phase timer 5010a8 and the input
        // word 53e584 (bit 4: a START edge).
        // And the camera entity 501240's x, z (+1c, +24: the bearing actor's).
        if (clock_file && expert_clock) {
            const uint32_t camera = b.read_dword(0x501240);
            std::fprintf(clock_file, "F %" PRIu64 " %u %x %u %u %u %d %x %08x %08x\n", consumed, b.read_dword(0x5010b0),
                         b.read_dword(0x501090), b.read_byte(0x5010a4), b.read_word(0x5036fc), b.read_word(0x5036fe),
                         std::bit_cast<int32_t>(b.read_dword(0x5010a8)), b.read_dword(0x53e584),
                         entity_address(camera) ? b.read_dword(camera+0x1c) : 0u, entity_address(camera) ? b.read_dword(camera+0x24) : 0u);
        }
        // The camera object (pointer 501240, updater 6510): +60..+68 the world light 72c4/709c keep, +84 the
        // tunnel light index of 7030 (byte).
        const auto camera_word = [&](uint32_t offset) -> uint32_t {
            const uint32_t object = b.read_dword(0x501240);
            return entity_address(object) ? b.read_dword(object + offset) : 0u;
        };
        const unsigned phase = b.read_byte(0x5010a4), active = b.read_byte(0x5010a0);
        if (phase != previous_phase || active != previous_active) {
            std::printf("race_trace_transition frame=%" PRIu64 " active=%u next=%u flags=%08x remaining=%u clock=%u\n",
                consumed, active, phase, b.read_dword(0x501090), b.read_dword(0x5010d0), b.read_dword(0x5010cc));
            previous_phase = phase; previous_active = active;
        }
        // 19..22 entry/race, 23/24 finish, 27/28 podium, 25/26 results, 29/30 after results.
        const bool racing = (phase >= 19 && phase <= 30) || (active >= 19 && active <= 30);
        const bool attract_rows = attract && ((phase >= 2 && phase <= 11) || (active >= 2 && active <= 11));
        const bool attract_cars = attract_rows && ((phase >= 3 && phase <= 7) || (active >= 3 && active <= 7));
        const uint32_t player = b.read_dword(0x501220), others = b.read_dword(0x501224);
        const unsigned count = b.read_dword(0x501080);
        if ((racing || attract_rows) && entity_address(player) && entity_address(others) && count && count <= 44) {
            const uint32_t stride = b.read_dword(others+8);
            const int view = std::bit_cast<int8_t>(b.read_byte(0x501710));
            std::fprintf(file,
                "T n=%" PRIu64 " frame=%" PRIu64 " phase=%u active=%u flags=%x remaining=%u clock=%u"
                " steer=%d accel=%u brake=%u entry=%u esteer=%d eaccel=%u ebrake=%u eremaining=%u"
                " normal=%u rolling=%u count=%u reset=%u laps=%u finish=%x display=%u hold=%u"
                " gearsel=%u manual=%u difficulty=%u penalty=%u suppress=%u blocked=%u variant=%u recover=%u"
                " guided=%u camera=%d view=%d scene=%u award=%u gate=%u fixed=%u raw=%02x/%02x/%02x course=%u light=%08x.%08x.%08x.%u",
                rows, consumed, phase, active, b.read_dword(0x501090), b.read_dword(0x5010d0), b.read_dword(0x5010cc),
                std::bit_cast<int32_t>(b.read_dword(0x501050)), b.read_word(0x501054), b.read_word(0x501058),
                entry_valid, entry_steer, entry_accel, entry_brake, entry_remaining,
                normal_entries, rolling_entries, count, b.read_dword(0x501084), b.read_byte(0x5010ec),
                b.read_dword(0x5010d8), b.read_dword(0x5010dc), b.read_dword(0x501a84),
                (b.read_dword(0x53e120)>>20)&7, b.read_byte(0x501464), b.read_word(0x53e59c), b.read_dword(0x501a80),
                b.read_byte(0x501074), b.read_dword(0x501854), b.read_byte(0x503690), (b.read_dword(0x53e584)>>4)&1,
                b.read_byte(0x5014f7), std::bit_cast<int32_t>(b.read_dword(0x5014a4)), view, b.read_dword(0x5010b0),
                b.read_dword(0x5010e0), b.read_word(player+0x204), b.read_byte(0x5018a8), b.io().inputs.steer, b.io().inputs.accel, b.io().inputs.brake,
                b.read_byte(0x501460), camera_word(0x60), camera_word(0x64), camera_word(0x68), unsigned(camera_word(0x84) & 255u));
            // Camera owner state: key tick5014a0, start stage501850, view matrix501560, cell bitmaps501520.
            std::fprintf(file, " target=%d ptimer=%d notify=%x vticks=%u.%u.%u.%u", entity_address(b.read_dword(0x5012f0)) ? int(b.read_byte(b.read_dword(0x5012f0)+4)) : -1,
                std::bit_cast<int32_t>(b.read_dword(0x5010a8)), b.read_dword(0x501398),
                b.read_dword(0x501700), b.read_dword(0x501704), b.read_dword(0x501708), b.read_dword(0x50170c));
            // View buttons held 53e120 (bit 0 START, bits 1-4 VR1..VR4: 53e120 bits 4-7 and 16) and the view
            // preset 501710 they select (8814), for the view replay of trace_race_native.
            const uint32_t held = b.read_dword(0x53e120);
            std::fprintf(file, " vr=%u preset=%d", unsigned((held >> 4) & 15u) | unsigned((held >> 16) & 1u) << 4, view);
            std::fprintf(file, " ctick=%u stage=%d ctr=%u ckey=%x aux=%x view=", b.read_dword(0x5014a0), std::bit_cast<int16_t>(b.read_word(0x501850)),
                b.read_byte(0x501713), b.read_dword(0x50149c), entity_address(b.read_dword(0x501270)) ? b.read_dword(b.read_dword(0x501270))>>31 : 0);
            for (unsigned i = 0; i < 12; ++i) std::fprintf(file, "%s%08x", i ? "." : "", b.read_dword(0x501560+4*i));
            std::fputs(" cells=", file);
            for (unsigned i = 0; i < 16; ++i) std::fprintf(file, "%s%x", i ? "." : "", b.read_dword(0x501520+4*i));
            std::fputs(" ecells=", file);
            for (unsigned i = 0; i < 16; ++i) std::fprintf(file, "%s%x", i ? "." : "", entry_valid ? entry_cells[i] : 0u);
            std::fputc('\n', file);
            if (racing || attract_cars) {
                car(player, 0);
                if (stride == 768) for (unsigned i = 1; i < count; ++i) car(others+(i-1)*stride, i);
            }
            ++rows;
        }
        normal_entries = rolling_entries = finish_entries = 0; entry_valid = false;
        ++frames_seen;
    }
    void close() {
        if (props_file) {
            const bool failed = std::ferror(props_file) != 0;
            if (std::fclose(props_file) || failed) { props_file = nullptr; throw std::runtime_error("cannot write props trace"); }
            props_file = nullptr;
        }
        if (clock_file) {
            const bool failed = std::ferror(clock_file) != 0;
            if (std::fclose(clock_file) || failed) { clock_file = nullptr; throw std::runtime_error("cannot write clock trace"); }
            clock_file = nullptr;
        }
        if (!file) return;
        const bool failed = std::ferror(file) != 0;
        if (std::fclose(file) || failed) { file = nullptr; throw std::runtime_error("cannot write race trace"); }
        file = nullptr;
        std::printf("race_trace rows=%" PRIu64 " frames=%" PRIu64 " coverage=reference-only\n", rows, frames_seen);
    }
};

} // namespace daytona::native
