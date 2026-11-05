// Host-only oracle of Expert's world actors (include/expert_actors.h, code/expert_actors.c): each handler's tick of the
// original run on the reference i960 with the live TGP after an Expert scenario (its ground in RAM and the TGP), from
// random task and player words, against nm_xa_tick on the same words: every task word the tick writes, the lists it
// draws and the matrices it draws them with (the TGP's own after an identity, against nm_xa_pose_matrix). Covers the
// paths the recorded races do not reach: the horses' running, re-latching, waiting and restart (224854), the big bird
// growing on START (224730), the statue's every rotation (222288: reverse finish, the 64th entry, the START count on
// a standing car), the podium pair (224cd0), the birds' podium pair and the bearing's two points (223bc0).
// The actor records are read from the ROM (236940[1]); nothing here is ROM data.
#pragma once
#include "runtime/game_loop.h"
#include "refcore/i960_ref.h"
extern "C" {
#include "expert_actors.h"
#include "contact.h"
}
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

namespace daytona::native {

struct ExpertActorOracle {
    rt::M2Board &board;
    const nm_drive_params_t *math;
    const nm_contact_pack_t *contact;
    static constexpr uint32_t task = 0x5fc000, player = 0x5fd000, camera = 0x5fd800;
    struct Draw { uint32_t list; float m[12]; };
    std::vector<Draw> draws;
    unsigned cases = 0, compared_draws = 0;
    float worst_rotation = 0, worst_offset = 0;

    float f(uint32_t at) { return std::bit_cast<float>(board.read_dword(at)); }
    void wf(uint32_t at, float v) { board.write_dword(at, std::bit_cast<uint32_t>(v)); }

    // The original's tick from `entry` until it returns; each call of 1786c is skipped after reading the matrix.
    void run(uint32_t entry) {
        board.write_dword(0x880100, 0x1010);   // identity: the matrix of a draw is its placement
        draws.clear();
        rt::I960Ref cpu(&board);
        cpu.m_IP = entry; cpu.m_r[27] = 0x880000; cpu.m_r[28] = 0x4000; cpu.m_r[29] = task;
        unsigned steps = 0;
        while (board.fetch(cpu.m_IP) != 0x0a000000 || cpu.m_rcache_pos) {
            if (++steps > 100000) throw std::runtime_error("expert actor original instruction guard");
            const uint32_t w = board.fetch(cpu.m_IP);
            if ((w >> 24) == 0x09) {
                const uint32_t disp = w & 0xfffffcu;
                const uint32_t target = (cpu.m_IP + (disp & 0x800000u ? disp - 0x1000000u : disp)) & 0xffffffu;
                if (target == 0x1786c) {
                    Draw d{cpu.m_r[16], {}};
                    board.write_dword(0x880110, 0x1111);
                    for (auto &v : d.m) v = std::bit_cast<float>(board.read_dword(0x884000));
                    draws.push_back(d);
                    cpu.m_IP += 4;
                    continue;
                }
            }
            cpu.execute_one();
        }
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("expert actor oracle unread TGP output");
    }

    // A DNA1 image holding one actor of `kind` with `groups` ACTOR groups (nm_xa_tick reads only the actors).
    static std::vector<uint32_t> dna_of(const nm_clock_actor_t &actor) {
        const unsigned groups = actor.groups, offset = 32 + 104 * groups;
        std::vector<uint32_t> words((offset + sizeof(nm_clock_actor_t)) / 4, 0);
        auto *h = reinterpret_cast<nm_clock_header_t *>(words.data());
        h->magic = NM_CLOCK_MAGIC; h->size = uint32_t(words.size() * 4); h->groups = groups; h->flags = NM_CLOCK_HAS_ACTORS;
        h->reserved[0] = 1; h->reserved[1] = offset;
        for (unsigned g = 0; g < groups; ++g) reinterpret_cast<nm_clock_group_t *>(words.data() + 8 + 26 * g)->cond = NM_CLOCK_ACTOR;
        std::memcpy(reinterpret_cast<uint8_t *>(words.data()) + offset, &actor, sizeof(actor));
        return words;
    }

    // The native draws of a tick against the original's: lists from the tables, placements within the tolerances.
    void compare_draws(const nm_xa_state_t &s, unsigned groups, uint32_t table, const char *what) {
        std::vector<const nm_xa_draw_t *> mine;
        for (unsigned g = 0; g < groups; ++g) if (s.draw[g].frame >= 0) mine.push_back(s.draw + g);
        if (mine.size() != draws.size()) {
            std::fprintf(stderr, "%s case %u: %zu native draws, %zu original\n", what, cases, mine.size(), draws.size());
            throw std::runtime_error("expert actor draws differ from the original");
        }
        for (size_t k = 0; k < mine.size(); ++k) {
            const uint32_t list = table == 0x288f614 ? table : board.read_dword(table + 4 * uint32_t(mine[k]->frame));
            float m[12]; nm_xa_pose_matrix(mine[k], m);
            for (unsigned i = 0; i < 12; ++i) {
                const float e = std::fabs(m[i] - draws[k].m[i]);
                float &worst = i < 9 ? worst_rotation : worst_offset;
                if (e > worst) worst = e;
                if (!(e <= (i < 9 ? 2e-3f : 2e-2f))) {
                    std::fprintf(stderr, "%s case %u draw %zu component %u: native %g original %g\n", what, cases, k, i,
                                 double(m[i]), double(draws[k].m[i]));
                    throw std::runtime_error("expert actor placement differs from the original");
                }
            }
            if (list != draws[k].list) {
                std::fprintf(stderr, "%s case %u draw %zu: native list %x original %x\n", what, cases, k, list, draws[k].list);
                throw std::runtime_error("expert actor list differs from the original");
            }
            ++compared_draws;
        }
    }

    void words_equal(uint32_t at, uint32_t native, const char *what, const char *word) {
        if (board.read_dword(at) != native) {
            std::fprintf(stderr, "%s case %u %s: native %08x original %08x\n", what, cases, word, native, board.read_dword(at));
            throw std::runtime_error("expert actor state differs from the original");
        }
    }

    // The common inputs: player and camera entities, globals, bitmaps.
    nm_xa_input_t inputs(std::mt19937 &random, const float centre[3], float spread, uint32_t bitmaps[16], unsigned cell) {
        nm_xa_input_t in{};
        for (unsigned i = 0; i < 16; ++i) bitmaps[i] = uint32_t(random());
        if (random() % 4) bitmaps[cell >> 5] |= 1u << (cell & 31); else bitmaps[cell >> 5] &= ~(1u << (cell & 31));
        in.bitmaps = bitmaps;
        in.counter = uint32_t(random());
        static const uint32_t flags[] = {0x400000u, 0x10000000u, 1u << 25, 1u << 3, 0x400000u};
        in.flags = flags[random() % 5];
        in.phase_timer = int32_t(random() % 1700) - 50;
        in.reverse = random() % 6 == 0;
        in.start_edge = random() % 3 == 0;
        in.player_flags = random() % 3 == 0 ? 0x10000000u : 0u;
        for (unsigned a = 0; a < 3; a += 2) in.player_position[a] = centre[a] + (float(int(random() % 20001) - 10000) / 10000.0f) * spread;
        in.player_position[1] = centre[1];
        in.player_speed = float(random() % 4000) / 1000.0f;
        in.player_speed_index = random() % 3 ? 0 : int32_t(random() % 300);
        in.player_heading = uint32_t(random()) ;
        in.player_profile = uint16_t(random() % 4 ? 200 + random() % 200 : random());
        in.camera[0] = in.player_position[0] + float(int(random() % 201) - 100);
        in.camera[1] = in.player_position[2] + float(int(random() % 201) - 100);
        // The board's words.
        for (unsigned i = 0; i < 16; ++i) board.write_dword(0x501520 + 4 * i, bitmaps[i]);
        board.write_dword(0x5010b0, in.counter); board.write_dword(0x501090, in.flags);
        board.write_dword(0x5010a8, uint32_t(in.phase_timer)); board.write_dword(0x501a8c, in.reverse);
        board.write_dword(0x53e584, in.start_edge ? 16u : 0u); board.write_dword(0x501a80, 0);
        board.write_dword(0x501220, player); board.write_dword(0x501240, camera);
        board.write_dword(player, in.player_flags);
        for (unsigned a = 0; a < 3; ++a) wf(player + 0x1c + 4 * a, in.player_position[a]);
        wf(player + 0x34, in.player_speed); board.write_dword(player + 0x18, uint32_t(in.player_speed_index));
        board.write_dword(player + 0x48, in.player_heading); board.write_word(player + 0x128, in.player_profile);
        wf(camera + 0x1c, in.camera[0]); wf(camera + 0x24, in.camera[1]);
        return in;
    }

    void task_actor(const nm_clock_actor_t &a) {
        for (unsigned j = 0; j < 0x80; j += 4) board.write_dword(task + j, 0);
        for (unsigned k = 0; k < 3; ++k) wf(task + 0x1c + 4 * k, a.position[k]);
        board.write_word(task + 0x2a, uint16_t(a.heading)); board.write_word(task + 0x2e, a.cell);
    }

    static nm_clock_actor_t actor_of(rt::M2Board &b, uint32_t record, unsigned kind, unsigned groups) {
        nm_clock_actor_t a{};
        a.handler = b.read_dword(record + 16);
        for (unsigned k = 0; k < 3; ++k) a.position[k] = std::bit_cast<float>(b.read_dword(record + 4 * k));
        a.heading = int16_t(b.read_word(record + 12));
        uint16_t cell = 0;
        if (!nm_contact_scenery_cell(a.position[0], a.position[2], &cell)) throw std::runtime_error("expert actor cell");
        a.cell = cell; a.kind = uint8_t(kind); a.groups = uint8_t(groups);
        return a;
    }

    void horses(std::mt19937 &random) {
        static const uint32_t ticks[] = {0x224854, 0x22487c, 0x224ab0, 0x224b1c, 0x224b60, 0x224c94, 0x224ca8};
        const nm_clock_actor_t a = actor_of(board, 0x2379c8, NM_CLOCK_HORSE, 1);
        const auto dna = dna_of(a);
        unsigned per_state[7] = {};
        for (unsigned i = 0; i < 20000; ++i, ++cases) {
            task_actor(a);
            nm_xa_state_t s{}; nm_xa_shared_t shared{};
            s.started = 1;
            nm_xa_horse_t &h = s.horse[0];
            h.x = a.position[0]; h.y = a.position[1]; h.z = a.position[2];
            h.state = uint8_t(random() % 7);
            const float near = random() % 2 ? 6.0f : 40.0f;
            for (unsigned k = 0; k < 3; k += 2) h.o[k] = (h.state >= NM_XA_HORSE_LATCH && h.state <= NM_XA_HORSE_FLEE ? a.position[k] : 0.0f)
                + float(int(random() % 20001) - 10000) / 10000.0f * 30.0f;
            h.o[1] = h.state >= NM_XA_HORSE_LATCH && h.state <= NM_XA_HORSE_FLEE ? a.position[1] : 0.0f;
            h.heading = uint32_t(random()); h.speed = float(random() % 3000) / 1000.0f;
            h.drawn = random() % 4 ? uint32_t(random() % 5000) : uint32_t(random());
            h.wait = random() % 2 ? 2558 + random() % 3 : uint32_t(random() % 2560);
            uint16_t cell = a.cell;
            if (h.state >= NM_XA_HORSE_LATCH) (void)nm_contact_scenery_cell(h.o[0], h.o[2], &cell);
            h.cell = cell;
            const float wx = h.state == NM_XA_HORSE_CIRCLE || h.state == NM_XA_HORSE_FLEE_START ? h.x + h.o[0] : h.o[0];
            const float wz = h.state == NM_XA_HORSE_CIRCLE || h.state == NM_XA_HORSE_FLEE_START ? h.z + h.o[2] : h.o[2];
            const float centre[3] = {wx, a.position[1], wz};
            uint32_t bitmaps[16];
            const nm_xa_input_t in = inputs(random, centre, near, bitmaps, h.cell);
            board.write_dword(task + 0xc, ticks[h.state]);
            for (unsigned k = 0; k < 3; ++k) wf(task + 0x34 + 4 * k, h.o[k]);
            board.write_dword(task + 0x48, h.heading); wf(task + 0x44, h.speed);
            board.write_dword(task + 0x40, h.drawn); board.write_dword(task + 0x4c, h.wait);
            board.write_word(task + 0x2e, h.cell);
            ++per_state[h.state];
            run(ticks[h.state]);
            if (!nm_xa_tick(&s, &shared, dna.data(), math, contact, &in)) throw std::runtime_error("native horse tick refused");
            const char *what = "horse 224854";
            words_equal(task + 0xc, ticks[h.state], what, "+c");
            for (unsigned k = 0; k < 3; ++k) words_equal(task + 0x34 + 4 * k, std::bit_cast<uint32_t>(h.o[k]), what, "+34..3c");
            words_equal(task + 0x40, h.drawn, what, "+40");
            if (h.state >= NM_XA_HORSE_LATCH && h.state <= NM_XA_HORSE_FLEE) {
                words_equal(task + 0x44, std::bit_cast<uint32_t>(h.speed), what, "+44");
                words_equal(task + 0x48, h.heading, what, "+48");
            }
            if (h.state >= NM_XA_HORSE_WAIT_START || board.read_dword(task + 0xc) >= 0x224c94) words_equal(task + 0x4c, h.wait, what, "+4c");
            if ((board.read_word(task + 0x2e)) != h.cell) throw std::runtime_error("expert horse cell differs");
            for (unsigned k = 0; k < 3; ++k) words_equal(task + 0x1c + 4 * k, std::bit_cast<uint32_t>(a.position[k]), what, "+1c..24");
            compare_draws(s, 1, 0x236840, what);
        }
        std::printf("expert_actor_horses PASS states %u/%u/%u/%u/%u/%u/%u\n", per_state[0], per_state[1], per_state[2],
                    per_state[3], per_state[4], per_state[5], per_state[6]);
    }

    void big_bird(std::mt19937 &random) {
        const nm_clock_actor_t a = actor_of(board, 0x237994, NM_CLOCK_BIG_BIRD, 1);
        const auto dna = dna_of(a);
        unsigned grown = 0;
        for (unsigned i = 0; i < 4000; ++i, ++cases) {
            task_actor(a);
            nm_xa_state_t s{}; nm_xa_shared_t shared{}; s.started = 1;
            s.scale = random() % 3 ? 26.0f + 0.5f * float(random() % 160) : 99.5f + 0.5f * float(random() % 3);
            wf(task + 0x34, s.scale);
            uint32_t bitmaps[16];
            const nm_xa_input_t in = inputs(random, a.position, 50, bitmaps, a.cell);
            run(0x22475c);
            const float before = s.scale;
            if (!nm_xa_tick(&s, &shared, dna.data(), math, contact, &in)) throw std::runtime_error("native bird tick refused");
            grown += s.scale != before;
            words_equal(task + 0x34, std::bit_cast<uint32_t>(s.scale), "big bird 224730", "+34");
            compare_draws(s, 1, 0x2368c0, "big bird 224730");
        }
        std::printf("expert_actor_big_bird PASS grown %u\n", grown);
    }

    void statue(std::mt19937 &random) {
        const nm_clock_actor_t a = actor_of(board, 0x23781c, NM_CLOCK_STATUE, 1);
        const auto dna = dna_of(a);
        unsigned drawn = 0, turned = 0;
        for (unsigned i = 0; i < 8000; ++i, ++cases) {
            task_actor(a);
            nm_xa_state_t s{}; nm_xa_shared_t shared{}; s.started = 1;
            s.statue_count = uint16_t(random() % 3 ? random() % 40 : random() % 65536);
            shared.entries = uint16_t(random() % 4 ? random() % 200 : 63 + 64 * (random() % 4));
            shared.inside = uint16_t(random() % 4);
            board.write_word(task + 0x34, s.statue_count);
            board.write_word(0x5036fc, shared.entries); board.write_word(0x5036fe, shared.inside);
            uint32_t bitmaps[16];
            nm_xa_input_t in = inputs(random, a.position, 50, bitmaps, a.cell);
            if (random() % 2) { in.player_profile = uint16_t(230 + random() % 100); board.write_word(player + 0x128, in.player_profile); }
            if (random() % 3 == 0) { in.phase_timer = int32_t(1000 + random() % 230); board.write_dword(0x5010a8, uint32_t(in.phase_timer)); }
            run(0x222288);
            if (!nm_xa_tick(&s, &shared, dna.data(), math, contact, &in)) throw std::runtime_error("native statue tick refused");
            const char *what = "statue 222288";
            if (board.read_word(task + 0x34) != s.statue_count || board.read_word(0x5036fc) != shared.entries ||
                board.read_word(0x5036fe) != shared.inside) {
                std::fprintf(stderr, "%s case %u: +34 %u/%u 5036fc %u/%u 5036fe %u/%u\n", what, cases, s.statue_count,
                    board.read_word(task + 0x34), shared.entries, board.read_word(0x5036fc), shared.inside, board.read_word(0x5036fe));
                throw std::runtime_error("expert statue state differs from the original");
            }
            compare_draws(s, 1, 0x288f614, what);
            drawn += !draws.empty(); turned += !draws.empty() && s.draw[0].count > 1;
        }
        std::printf("expert_actor_statue PASS drawn %u turned %u\n", drawn, turned);
    }

    void stateless(std::mt19937 &random) {
        struct { uint32_t record, tick; unsigned kind, groups; uint32_t table; } list[] = {
            {0x23786c, 0x22432c, NM_CLOCK_BIRDS, 7, 0x2368c0}, {0x237a28, 0x224cec, NM_CLOCK_PAIR, 2, 0x236840},
            {0x237838, 0x223be0, NM_CLOCK_BEARING, 1, 0}};
        for (auto &e : list) {
            nm_clock_actor_t a = actor_of(board, e.record, e.kind, e.groups);
            if (e.kind == NM_CLOCK_BEARING) {
                for (unsigned k = 0; k < 3; ++k) { a.param[k] = f(0x223cc8 + 4 * k); a.param[3 + k] = f(0x223cd4 + 4 * k); }
                e.table = board.read_dword(e.record + 20);
            }
            const auto dna = dna_of(a);
            for (unsigned i = 0; i < 4000; ++i, ++cases) {
                task_actor(a);
                board.write_dword(task + 0x14, board.read_dword(e.record + 20));   /* 2002c: the argument */
                nm_xa_state_t s{}; nm_xa_shared_t shared{}; s.started = 1;
                if (e.kind == NM_CLOCK_PAIR) { s.pair_z = a.position[2] - 0.2f * float(random() % 1600); wf(task + 0x24, s.pair_z); }
                uint32_t bitmaps[16];
                const nm_xa_input_t in = inputs(random, a.position, 400, bitmaps, a.cell);
                run(e.tick);
                if (!nm_xa_tick(&s, &shared, dna.data(), math, contact, &in)) throw std::runtime_error("native actor tick refused");
                if (e.kind == NM_CLOCK_PAIR) words_equal(task + 0x24, std::bit_cast<uint32_t>(s.pair_z), "pair 224cd0", "+24");
                compare_draws(s, e.groups, e.table, e.kind == NM_CLOCK_BIRDS ? "birds 22430c" : e.kind == NM_CLOCK_PAIR ? "pair 224cd0" : "bearing 223bc0");
            }
        }
        std::printf("expert_actor_birds_pair_bearing PASS\n");
    }
};

// After an Expert scenario (the course's ground in RAM, the TGP booted and idle).
inline void check_expert_actors(rt::M2Board &board, const char *drive_path, const char *contact_path) {
    std::unique_ptr<nm_drive_params_t, decltype(&std::free)> p(nm_drive_load(drive_path), &std::free);
    nm_contact_pack_t contact{};
    if (!p || !nm_contact_load(&contact, contact_path)) throw std::runtime_error("expert actor oracle: packs");
    if (!board.tgp().booted() || !board.tgp().fifo_out_empty()) throw std::runtime_error("expert actor oracle: TGP not idle");
    // Save what the cases overwrite.
    std::vector<uint32_t> saved;
    const uint32_t ranges[][2] = {{0x5fc000, 0x5fe000}, {0x501000, 0x501b00}, {0x503600, 0x503700}, {0x53e584, 0x53e588}};
    for (auto &r : ranges) for (uint32_t at = r[0]; at < r[1]; at += 4) saved.push_back(board.read_dword(at));
    const auto restore = [&] { size_t i = 0; for (auto &r : ranges) for (uint32_t at = r[0]; at < r[1]; at += 4) board.write_dword(at, saved[i++]); };
    ExpertActorOracle oracle{board, p.get(), &contact};
    std::mt19937 random(0x58414354);
    try {
        oracle.horses(random); oracle.big_bird(random); oracle.statue(random); oracle.stateless(random);
    } catch (...) { restore(); nm_contact_unload(&contact); throw; }
    restore(); nm_contact_unload(&contact);
    std::printf("native_expert_actors PASS cases=%u draws=%u largest placement difference %.6f (3x3) %.5f (offset); "
                "against original 222288, 223be0, 224cec, 22432c, 22475c, 224854..224ca8 with the TGP\n",
                oracle.cases, oracle.compared_draws, double(oracle.worst_rotation), double(oracle.worst_offset));
}

} // namespace daytona::native
