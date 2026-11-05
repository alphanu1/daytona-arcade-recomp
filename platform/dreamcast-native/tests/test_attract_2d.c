/* The attract's 2D layer (include/attract_2d.h) against the original's tile RAM, iteration by iteration.
 *
 * usage: test_attract_2d ATTRACT_TRACE DUMP_DIR LOOPS
 *   ATTRACT_TRACE  m2segment --trace-race FILE --trace-attract (its T rows map a frame to its 5010b0 and phase)
 *   DUMP_DIR       m2segment --dump-frames DIR FIRST LAST EVERY (tiles_N.bin: the tile RAM after frame N)
 * For every dumped frame of phases 2..7 whose 5010b0 the native loop reaches (courses 0, 2, 1, 0 ...; the records
 * the defaults of race_record_data.h): layer A's 4096 words equal, and the line table's scanlines 0..383 hidden
 * (all ones) exactly where the native hides them (every other entry zero). Exits 1 on a difference, 77 when an
 * input is missing. Host only; the dumps are ROM-run output (build/). */
#undef NDEBUG
#include "attract_2d.h"
#include "attract.h"
#include "race_record_data.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

enum { MAX_FRAMES = 40000 };
static uint32_t frame_scene[MAX_FRAMES];
static uint8_t frame_phase[MAX_FRAMES], frame_known[MAX_FRAMES];
static const char *dumps;
static unsigned checked, failed, frames_seen;

static void compare(const nm_a2d_t *s, uint32_t scene, const char *what) {
    for (unsigned f = 0; f < MAX_FRAMES; ++f) {
        if (!frame_known[f] || frame_scene[f] != scene || frame_phase[f] < 2 || frame_phase[f] > 7) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/tiles_%05u.bin", dumps, f);
        FILE *file = fopen(path, "rb");
        if (!file) continue;
        static uint8_t ram[0x10000];
        const size_t got = fread(ram, 1, sizeof ram, file);
        fclose(file);
        if (got != sizeof ram) { printf("%s: short dump\n", path); ++failed; continue; }
        ++frames_seen;
        unsigned bad = 0, first = 0;
        for (unsigned i = 0; i < 4096; ++i) {
            const uint16_t word = (uint16_t)(ram[2 * i] | ram[2 * i + 1] << 8);
            if (word != s->tiles[i]) { if (!bad++) first = i; }
        }
        unsigned lines_bad = 0, line_first = 0;
        for (unsigned line = 0; line < NM_A2D_LINES; ++line) {
            const uint8_t *e = ram + 0xc000 + 8 * line;
            int ones = 1, zero = 1;
            for (unsigned k = 0; k < 8; ++k) { ones &= e[k] == 0xff; zero &= e[k] == 0; }
            const int hidden = line >= s->hidden_first && line < s->hidden_end;
            if (!(hidden ? ones : zero)) { if (!lines_bad++) line_first = line; }
        }
        ++checked;
        if (bad || lines_bad) {
            if (failed++ < 16) {
                const uint16_t word = (uint16_t)(ram[2 * first] | ram[2 * first + 1] << 8);
                printf("frame %u (5010b0 %" PRIu32 ", %s): %u words differ (first row %u column %u: %04x, native %04x), "
                       "%u lines (first %u; native hides %u..%u)\n", f, scene, what, bad, first / 64, first % 64, word,
                       s->tiles[first], lines_bad, line_first, s->hidden_first, s->hidden_end);
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 4) { fputs("usage: test_attract_2d ATTRACT_TRACE DUMP_DIR LOOPS\n", stderr); return 2; }
    FILE *trace = fopen(argv[1], "r");
    if (!trace) { puts("SKIP: no attract trace"); return 77; }
    dumps = argv[2];
    const long loops = strtol(argv[3], NULL, 10);
    static char line[65536];
    while (fgets(line, sizeof line, trace)) {
        if (line[0] != 'T') continue;
        unsigned frame = 0, active = 0; uint32_t scene = 0;
        const char *f = strstr(line, " frame="), *a = strstr(line, " active="), *s = strstr(line, " scene=");
        if (!f || !a || !s) continue;
        frame = (unsigned)strtoul(f + 7, NULL, 10); active = (unsigned)strtoul(a + 8, NULL, 10);
        scene = (uint32_t)strtoul(s + 7, NULL, 10);
        if (frame >= MAX_FRAMES) continue;
        frame_scene[frame] = scene; frame_phase[frame] = (uint8_t)active; frame_known[frame] = 1;
    }
    fclose(trace);
    static nm_race_record_t records[3][24];
    for (unsigned c = 0; c < 3; ++c) nm_race_records_reset(records[c], nm_race_record_seeds[c]);
    static nm_a2d_t s;
    uint32_t counter = 1;
    unsigned course = 0, view = 0;
    for (long loop = 0; loop < loops; ++loop) {
        ++counter; /* phase 2 */
        nm_a2d_phase2(&s, course, records[course < 3 ? course : 0], 1, view);
        compare(&s, counter, "phase 2");
        const uint32_t object_clock = counter + 1u;
        nm_a2d_cinematic(&s, 3, object_clock, 0); /* the scene start: phase 3's first iteration */
        compare(&s, object_clock, "scene start");
        for (uint32_t t = 1; t <= NM_ATTRACT_TICKS; ++t) {
            const unsigned phase = nm_attract_phase(t - 1u);
            nm_a2d_cinematic(&s, phase, object_clock + t, t == NM_ATTRACT_TICKS);
            compare(&s, object_clock + t, phase == 3 ? "cinematic" : phase == 4 ? "ranking entry" : "ranking");
        }
        const uint32_t demo = object_clock + NM_ATTRACT_TICKS + 1u;
        nm_a2d_demo(&s, 6, demo);
        compare(&s, demo, "demo start");
        for (uint32_t t = 1; t <= 834u; ++t) {
            nm_a2d_demo(&s, 7, demo + t);
            compare(&s, demo + t, "demo");
        }
        view = s.view;
        counter = object_clock + NM_ATTRACT_TICKS + NM_ATTRACT_DEMO_ITERATIONS + NM_ATTRACT_TITLE_TICKS + 1u;
        course = nm_attract_next_course(course);
    }
    printf("attract 2D: %u dumped frames compared, %u with differences\n", checked, failed);
    return failed || !checked ? 1 : 0;
}
