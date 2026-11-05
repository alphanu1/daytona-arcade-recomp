/* The props (code/props.c) against the original game's own runs: m2segment --trace-race R --trace-props RUN.bin
 * (tools/trace_race.hpp; the inputs of a run that hits props, e.g. recipes/inputs/inputs-prop-*.txt; E holds the
 * tasks from the manager to the sound manager, then 5036xx). The first runs (course 0's, build/dc-native-world-anim.LwFeBn/cones,
 * since deleted) came from a separate tool, cone_run.cpp, in this format. Each run's records, in execution order, drive the native props: M (the
 * manager's entry: the player entity's words, the view matrix), T (a cone's tick: the draw bitmap),
 * E (the sound manager's entry: the twelve prop tasks and 5036xx after the cones), X (a cone's draw:
 * the twelve matrix words the geometrizer got), S (19bdc), I (init entries). Compared after every
 * tick: every cone field and the whole 5036xx block (counter, speeds, sound, masks, cooldown, queue);
 * every draw matrix bit for bit (the pose composed with the TGP's formulas from the native state);
 * every hit sound on its tick.
 *
 * usage: test_props ASSET_DIR [--course N] RUN.bin... [--course N RUN.bin...]   (ASSET_DIR: drive.dnd,
 *        courseN.dnc; the runs after --course N are of course slot N, before any of course 0; a run's C record names
 *        the course of the scene that follows); exit 77 without data.
 * Build: cc -std=c11 -O1 -fno-fast-math -ffp-contract=off -Iinclude -I<race-start-data> -I<race-header>
 *        tests/test_props.c code/props.c code/drive.c code/contact.c -lm */
#include "props.h"
#include "race_start_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NM_PROP_COURSES
#error "the race-start header has no prop data by course (tools/prepare_intro.py --race-start)"
#endif

/* The arcade's address of a record (spec-cones.md section 3): a kind's rest record or one of a table's. */
static const nm_prop_data_t *data;
static uint32_t record_address(unsigned index) {
    for (unsigned k = 0; k < NM_PROP_KINDS; ++k)
        if (index == data->kind[k].rest) return data->kind[k].rest_address;
    for (unsigned t = 0; t < data->table_count; ++t) {
        const nm_prop_table_t *table = data->tables + t;
        if (index >= table->first && index < table->first + table->count)
            return table->address + 4u + 16u * (index - table->first);
    }
    return 0;
}
static int prop_sound(uint32_t command) {
    for (unsigned k = 0; k < NM_PROP_KINDS; ++k) if (command == data->kind[k].sound) return 1;
    return 0;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t fbits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static float bitsf(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

/* TGP 0x12, 0x14, 0x15, 0x16 on a matrix of columns c0, c1, c2, t (spec section 6). */
static void tgp_translate(float m[12], float x, float y, float z) {
    for (unsigned r = 0; r < 3; ++r) { float t = m[9 + r] + m[r] * x; t += m[3 + r] * y; t += m[6 + r] * z; m[9 + r] = t; }
}
static void tgp_rotate(float m[12], const nm_drive_params_t *p, unsigned axis, int16_t angle) {
    const float s = nm_drive_sine(p, (uint16_t)angle), c = nm_drive_sine(p, (uint16_t)((uint16_t)angle + 16384u));
    float *a = m + (axis == 0 ? 3 : 0), *b = m + (axis == 2 ? 3 : 6);
    for (unsigned r = 0; r < 3; ++r) {
        const float u = a[r], v = b[r];
        if (axis == 1) { const float p0 = u * c, p1 = v * s, q0 = v * c, q1 = u * s; a[r] = p0 + p1; b[r] = q0 - q1; }
        else { const float p0 = u * c, p1 = v * s, q0 = u * s, q1 = v * c; a[r] = p0 - p1; b[r] = q0 + q1; }
    }
}

static unsigned failures;
static unsigned long knocks[NM_PROP_KINDS][2];   /* knock starts by kind, slow set / fast set */
#define CHECK(cond, ...) do { if (!(cond)) { if (++failures <= 12) { printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } } while (0)

static int load_course(const char *assets, unsigned course, nm_contact_pack_t *contact) {
    char path[1024];
    if (course > 2 || !(NM_PROP_COURSES >> course & 1u)) { printf("SKIP: no props of course %u in the header\n", course); return 0; }
    data = nm_race_prop_courses[course];
    if (contact->storage) nm_contact_unload(contact);
    snprintf(path, sizeof path, "%s/course%u.dnc", assets, course);
    if (!nm_contact_load(contact, path)) { printf("SKIP: no %s\n", path); return 0; }
    return 1;
}

static int run(const char *path, const char *assets, const nm_drive_params_t *math, nm_contact_pack_t *contact,
               unsigned long totals[8]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    nm_props_t s;
    memset(&s, 0, sizeof s);
    nm_props_player_t player = {0};
    float view[12] = {0};
    uint32_t bitmaps[16];
    unsigned long ticks = 0, equal = 0, draws = 0, exact = 0, moving = 0;
    uint32_t tick = 0;
    uint32_t arcade_sounds[512], native_sounds[512];
    unsigned arcade_count = 0, native_count = 0;
    uint8_t knocked[NM_PROPS_MAX] = {0}, pending[NM_PROPS_MAX] = {0};
    static uint8_t body[1 << 16];
    int scene = 0;
    unsigned knocked_total = 0;
    for (;;) {
        uint8_t head[13];
        if (fread(head, 1, 13, f) != 13) break;
        const uint32_t n = rd32(head + 9);
        if (n > sizeof body || fread(body, 1, n, f) != n) { fclose(f); return 0; }
        const char tag = (char)head[0];
        if (tag == 'C') {   /* props_run: the course of the scene whose init follows */
            if (!load_course(assets, rd32(body), contact)) { fclose(f); return 0; }
        } else if (tag == 'I') {
            const uint32_t target = rd32(body);
            if (target == 0x2002c) scene = 0;
            else if (target == 0x222d30) {   /* the manager's init: the scene's first tick (all inits) */
                CHECK(nm_props_init(&s, data, math, contact), "%s: init", path);
                for (unsigned i = 0; i < NM_PROPS_MAX; ++i) knocked_total += knocked[i];
                memset(knocked, 0, sizeof knocked);
                scene = 1;
            } else if (target == 0x22302c && scene) {
                const uint32_t sound = nm_props_sound_manager(&s);
                if (sound && native_count < 512) native_sounds[native_count++] = tick;
            }
        } else if (tag == 'M' && scene) {
            ++tick;
            const uint8_t *P = body;
            for (unsigned a = 0; a < 3; ++a) player.position[a] = bitsf(rd32(P + 0x1c + 4 * a));
            player.speed = bitsf(rd32(P + 0x34));
            player.speed_index = rd32(P + 0x18);
            player.roll = (int16_t)rd16(P + 0x2c);
            player.yaw = (int16_t)rd16(P + 0x58);
            player.heading = (int16_t)rd16(P + 0x48);
            for (unsigned i = 0; i < 12; ++i) view[i] = bitsf(rd32(body + 4 * (0xd0 + i)));
            CHECK(nm_props_manager(&s, math, data, &player), "%s tick %u: manager", path, tick);
        } else if (tag == 'T' && scene) {
            const unsigned i = (rd32(body) - 0x50f000u) / 0x80u;
            for (unsigned w = 0; w < 16; ++w) bitmaps[w] = rd32(body + 4 + 4 * w);
            const unsigned before = i < s.count ? s.cone[i].rec : 0u;
            CHECK(i < s.count && nm_props_cone(&s, i, data, math, contact, &player, bitmaps), "%s tick %u: cone %u", path, tick, i);
            if (i < s.count && s.cone[i].count && s.cone[i].rec != before + 1u)   /* a table (re)started this tick */
                ++knocks[s.cone[i].kind][player.speed_index > 70u];
            if (i < NM_PROPS_MAX) { pending[i] = s.cone[i].drawn; if (s.cone[i].count) knocked[i] = 1; }
        } else if (tag == 'X' && scene) {
            const unsigned i = (rd32(body) - 0x50f000u) / 0x80u;
            if (!rd32(body + 4) || i >= s.count) continue;   /* skipped by the polygon budget */
            CHECK(pending[i], "%s tick %u: cone %u drawn by the arcade only", path, tick, i + 1);
            pending[i] = 0;
            const nm_prop_t *c = s.cone + i;
            float m[12];
            memcpy(m, view, sizeof m);
            tgp_translate(m, c->x, c->draw_y, c->z);
            tgp_rotate(m, math, 0, c->h54); tgp_rotate(m, math, 2, c->h56); tgp_rotate(m, math, 1, c->yaw);
            tgp_rotate(m, math, 2, c->he); tgp_rotate(m, math, 1, c->hc); tgp_rotate(m, math, 0, c->ha);
            unsigned same = 1;
            for (unsigned w = 0; w < 12; ++w) same &= fbits(m[w]) == rd32(body + 8 + 4 * w);
            ++draws; exact += same;
            CHECK(same, "%s tick %u: cone %u draw matrix", path, tick, i + 1);
        } else if (tag == 'S' && scene && prop_sound(rd32(body))) {
            if (arcade_count < 512) arcade_sounds[arcade_count++] = tick;
        } else if (tag == 'E' && scene) {
            for (unsigned i = 0; i < NM_PROPS_MAX; ++i)
                CHECK(!pending[i], "%s tick %u: cone %u drawn natively only", path, tick, i + 1);
            memset(pending, 0, sizeof pending);
            unsigned ok = 1;
            const uint32_t tasks = n - 0x80u;   /* the manager, the props, the sound manager */
            CHECK(tasks == 0x80u * (s.count + 2u), "%s tick %u: %lu bytes of tasks for %u props", path, tick,
                  (unsigned long)tasks, (unsigned)s.count);
            if (tasks != 0x80u * (s.count + 2u)) continue;
            for (unsigned i = 0; i < s.count; ++i) {
                const uint8_t *t = body + 0x80 * (i + 1);
                const nm_prop_t *c = s.cone + i;
                const uint32_t got[] = {rd32(t) & 2u, rd32(t + 0x1c), rd32(t + 0x20), rd32(t + 0x24), rd16(t + 6), rd16(t + 0x2e),
                    rd16(t + 0x52), rd16(t + 0x54), rd16(t + 0x56), rd16(t + 0x58), rd16(t + 0x5a), rd32(t + 0x5c),
                    rd32(t + 0x60), rd32(t + 0x6c)};
                const uint32_t want[] = {c->hit ? 2u : 0u, fbits(c->x), fbits(c->y), fbits(c->z), c->count, c->cell,
                    (uint16_t)c->h52, (uint16_t)c->h54, (uint16_t)c->h56, (uint16_t)c->h58, (uint16_t)c->h5a, record_address(c->rec),
                    fbits(c->v60), c->face};
                static const char *const names[] = {"hit", "x", "y", "z", "count", "cell", "h52", "h54", "h56", "h58", "h5a",
                    "record", "v60", "face"};
                for (unsigned k = 0; k < sizeof got / sizeof got[0]; ++k) {
                    if ((k == 10 || k == 12) && !knocked[i]) continue;   /* +5a, +60: not written before a knock */
                    if (got[k] != want[k]) {
                        ok = 0;
                        CHECK(0, "%s tick %u: cone %u %s arcade %08lx native %08lx", path, tick, i + 1, names[k],
                              (unsigned long)got[k], (unsigned long)want[k]);
                    }
                }
            }
            const uint8_t *g = body + tasks;
            const uint32_t gw[] = {rd32(g + 4), rd32(g + 8), rd32(g + 0xc), rd32(g + 0x14), rd32(g + 0x18), rd32(g + 0x1c),
                rd16(g + 0x20), g[0x22], g[0x23]};
            const uint32_t nw[] = {fbits(s.speed2), fbits(s.speed01), s.counter, s.sound, s.lo, s.hi, s.cooldown, s.write, s.read};
            for (unsigned k = 0; k < sizeof gw / sizeof gw[0]; ++k)
                if (gw[k] != nw[k]) { ok = 0; CHECK(0, "%s tick %u: 5036xx word %u arcade %08lx native %08lx", path, tick, k,
                                                     (unsigned long)gw[k], (unsigned long)nw[k]); }
            if (memcmp(g + 0x24, s.queue, NM_PROPS_QUEUE)) { ok = 0; CHECK(0, "%s tick %u: queue", path, tick); }
            ++ticks; equal += ok;
            unsigned any = 0;
            for (unsigned i = 0; i < s.count; ++i) any |= s.cone[i].count != 0;
            moving += any;
        }
    }
    fclose(f);
    unsigned same_sounds = arcade_count == native_count;
    for (unsigned k = 0; same_sounds && k < arcade_count; ++k) same_sounds = arcade_sounds[k] == native_sounds[k];
    CHECK(same_sounds, "%s: hit sounds arcade %u native %u", path, arcade_count, native_count);
    unsigned cones = knocked_total;
    for (unsigned i = 0; i < NM_PROPS_MAX; ++i) cones += knocked[i];
    printf("%s: ticks %lu/%lu equal (%lu with a moving cone), draws %lu/%lu exact, sounds %u/%u on their ticks, %u knock-overs of a cone (one a cone a scene)\n",
           path, equal, ticks, moving, exact, draws, same_sounds ? arcade_count : 0, arcade_count, cones);
    totals[0] += equal; totals[1] += ticks; totals[2] += exact; totals[3] += draws; totals[4] += same_sounds ? arcade_count : 0;
    totals[5] += arcade_count; totals[6] += moving;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) { fputs("usage: test_props ASSET_DIR [--course N] RUN.bin...\n", stderr); return 2; }
    char path[1024];
    snprintf(path, sizeof path, "%s/drive.dnd", argv[1]);
    const nm_drive_params_t *math = nm_drive_load(path);
    nm_contact_pack_t contact = {0};
    if (!math || !load_course(argv[1], 0, &contact)) { printf("SKIP: no drive.dnd/course0.dnc in %s\n", argv[1]); return 77; }
    unsigned long totals[8] = {0};
    unsigned runs = 0, usable = 1;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--course") && i + 1 < argc) { usable = load_course(argv[1], (unsigned)atoi(argv[++i]), &contact); continue; }
        if (!usable || !run(argv[i], argv[1], math, &contact, totals)) { printf("SKIP: %s unreadable\n", argv[i]); continue; }
        ++runs;
    }
    if (!runs) return 77;
    printf("props: knock starts by kind (slow set/fast set): 223114 %lu/%lu, 223078 %lu/%lu, 2230ac %lu/%lu, 2230e0 %lu/%lu\n",
           knocks[0][0], knocks[0][1], knocks[1][0], knocks[1][1], knocks[2][0], knocks[2][1], knocks[3][0], knocks[3][1]);
    printf("props: %u runs, ticks %lu/%lu equal (%lu with a moving cone), draw matrices %lu/%lu, hit sounds %lu/%lu: %s\n",
           runs, totals[0], totals[1], totals[6], totals[2], totals[3], totals[4], totals[5], failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
