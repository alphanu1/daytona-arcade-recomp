/* Host-only free-running trace of the native attract loop (include/attract.h), for tests/test_attract_lockstep.py
 * against the original's (m2segment --trace-race FILE --trace-attract). NOT a pass/fail test: it runs what
 * code/practice.c runs in the attract, loop after loop: the course's packs, the cinematic of its graph
 * (podium_data.h nm_attract_courses, intro_scene.h) for NM_ATTRACT_TICKS ticks with the latched phase flags, the
 * V.R. demo (vr_demo.h: its start and 834 ticks, each with the camera pass; rows as the race trace's), the title
 * counted (the frame counter 5010b0), then the next course (nm_attract_next_course), with what the scenes carry
 * (nm_attract_carry_t).
 *
 * usage: trace_attract_native ASSET_DIR NEW_OUTPUT LOOPS [--course C] [--clock CLOCK_OUT REFERENCE_TRACE [--actors C]]
 *            [--enter STATE] [--credit FRONTEND_TRACE --leave NEW_STATE]
 *   --enter: the loop after a game (trace_race_native --leave, tests/game_chain.h) instead of power-on: its frame
 *   counter, course and pool. --credit: the original's next credit in the front-end trace (the first game start after
 *   the entry, m2segment --trace-frontend) ends the run: the credit's iteration runs as any other, the game start
 *   (phases 12, 17) on the scene it found, and --leave writes the state for trace_race_native --enter.
 *   One T row a tick (and one for the scene start, ticks=0) with the 5010b0 the original shows after the pass that
 *   made that state (scene= object_clock + ticks: its tick reads object_clock + ticks + 1 before the increment), the loop's course, the latched phase, the stage, the camera's
 *   key tick (ctick, 5014a0), the view matrix (501560), the cell bitmaps (501520 draw, 501540 near) and the scene
 *   light (camera object +60..+68, +84: tunnel_light.h through the course's ranges); C rows the cars' state (spawn index = entity order).
 *   --clock: the frame-counter animations' frames (clock_scene.h, nm_clock_frame) of every tick on a course with a
 *   courseN-actors.dna, keyed by the reference frame whose pass had that 5010b0 (test_clock_lockstep.py).
 *   --actors C (with --clock): course C alone, its cinematic and V.R. demo, with the props and actors stepped as
 *   code/practice.c steps them (code/props_scene.c) and each actor group's frame and "P" placement as
 *   tests/trace_race_native.c --clock writes them, the demo's ticks from 100000 (test_expert_lockstep.py).
 * Build with -fno-fast-math -ffp-contract=off (as trace_race_native). ROM-derived headers and packs stay under build/. */
#undef NDEBUG
#include "intro_scene.h"
#include "podium_data.h"
#include "race_start_data.h"
#include "attract.h"
#include "visibility.h"
#include "course_assets.h"
#include "clock_scene.h"
#include "vr_demo.h"
#include "frontend.h"
#include "game_chain.h"
#include "props_scene.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nm_intro_scene_t scene;
static nm_race_scene_t race; /* empty, as practice.c's in the attract (clock_scene.h reads it) */
static nm_drive_params_t *math;
static nm_contact_pack_t contact;
static nm_road_pack_t road;
static nm_path_pack_t paths[8];
static nm_scenery_pack_t scenery;
static int loaded = -1;
/* The V.R. demo (vr_demo.h): its race scene, the presentation arrays of its camera pass, the road marks. */
static nm_race_scene_t demo_race;
static nm_intro_scene_t demo_view;
static nm_demo_t demo;
static nm_road_marks_t marks;

static uint32_t bits(float value) { uint32_t out; memcpy(&out, &value, 4); return out; }

static void unload(void) {
    if (loaded < 0) return;
    nm_scenery_unload(&scenery);
    for (unsigned i = 0; i < 8; ++i) nm_path_unload(paths + i);
    nm_road_unload(&road); nm_contact_unload(&contact);
    loaded = -1;
}
static int load(const char *assets, unsigned course) {
    char path[1024];
    if ((int)course == loaded) return 1;
    unload();
#define ASSET(kind, route) (nm_course_asset(path, sizeof(path), assets, course, kind, route) ? path : NULL)
    if (!nm_contact_load(&contact, ASSET(NM_COURSE_CONTACT, 0)) || !nm_road_load(&road, ASSET(NM_COURSE_ROAD, 0))) return 0;
    for (unsigned i = 0; i < 8; ++i) if (!nm_path_load(paths + i, ASSET(NM_COURSE_ROUTE, i))) return 0;
    if (!nm_scenery_load(&scenery, ASSET(NM_COURSE_VISIBILITY, 0))) return 0;
#undef ASSET
    loaded = (int)course;
    free(marks.values);
    marks.count = road.profile_count - 1u; marks.timer = 0;
    marks.values = calloc(road.profile_count, 1);
    return marks.values && road.slot == course && contact.slot == course && scenery.slot == course;
}

/* --clock */
static FILE *clock_out;
static void *clock_data;
static unsigned clock_course = 99, actors_course = 99;
static long *frame_of;          /* reference frame by 5010b0 (the last frame whose row ended with it), -1: none */
static uint32_t frame_of_count;
static int clock_open(const char *assets, unsigned course) {
    char path[1024];
    free(clock_data); clock_data = NULL; clock_course = course;
    if (actors_course != 99 && course != actors_course) return 1;
    if (!nm_course_asset(path, sizeof(path), assets, course, NM_COURSE_ACTORS, 0)) return 0;
    FILE *in = fopen(path, "rb");
    if (!in) return 1; /* no animations on this course */
    fseek(in, 0, SEEK_END); const long size = ftell(in); fseek(in, 0, SEEK_SET);
    clock_data = size > 0 ? aligned_alloc(32, ((size_t)size + 31u) & ~(size_t)31u) : NULL;
    const int ok = clock_data && fread(clock_data, 1, (size_t)size, in) == (size_t)size &&
        nm_clock_validate(clock_data, (size_t)size, NULL, 65535u);
    fclose(in);
    return ok;
}
static int reference_frames(const char *path) {
    FILE *in = fopen(path, "r");
    if (!in) return 0;
    static char line[1 << 16];
    while (fgets(line, sizeof(line), in)) {
        if (line[0] != 'T' || line[1] != ' ') continue;
        const char *f = strstr(line, " frame="), *c = strstr(line, " scene=");
        if (!f || !c) continue;
        const long frame = strtol(f + 7, NULL, 10);
        const uint32_t counter = (uint32_t)strtoul(c + 7, NULL, 10);
        if (counter >= frame_of_count) {
            const uint32_t grown = counter + 4096u;
            frame_of = realloc(frame_of, grown * sizeof(*frame_of));
            for (uint32_t i = frame_of_count; i < grown; ++i) frame_of[i] = -1;
            frame_of_count = grown;
        }
        frame_of[counter] = frame;
    }
    fclose(in);
    return frame_of_count != 0;
}
/* One tick's K row (and P rows): CINEMATIC 1 the attract's scene, 0 the demo's race scene with its camera's cells. */
static void clock_row_of(uint32_t counter, long tick, const nm_race_scene_t *r, const nm_intro_scene_t *v, int cinematic) {
    if (!clock_out || !clock_data || counter >= frame_of_count || frame_of[counter] < 0) return;
    uint32_t bitmaps[16];
    nm_clock_input_t in;
    const int have = nm_scenery_bitmaps(&v->cells_previous, bitmaps) &&
        nm_clock_scene_input(&in, r, v, cinematic, 0, bitmaps);
    fprintf(clock_out, "K tick=%ld frame=%ld c=%" PRIu32 " stage=-1", tick, frame_of[counter], have ? in.counter : 0u);
    const nm_clock_header_t *h = clock_data;
    const nm_xa_draw_t *draws = actors_course != 99 ? nm_props_world_actor_draws(cinematic) : NULL;
    int frames[NM_CLOCK_GROUPS_MAX];
    for (unsigned g = 0; g < h->groups; ++g) {
        const nm_clock_group_t *group = nm_clock_groups(clock_data) + g;
        frames[g] = !have ? -1 : group->cond != NM_CLOCK_ACTOR ? nm_clock_frame(group, &in) : draws ? draws[g].frame : -1;
        fprintf(clock_out, " %d", frames[g]);
    }
    fputc('\n', clock_out);
    for (unsigned g = 0; g < h->groups && draws; ++g) {
        if (frames[g] < 0 || !draws[g].count) continue;
        float m[12]; nm_xa_pose_matrix(draws + g, m);
        fprintf(clock_out, "P tick=%ld g=%u", tick, g);
        for (unsigned i = 0; i < 12; ++i) fprintf(clock_out, " %08" PRIx32, bits(m[i]));
        fputc('\n', clock_out);
    }
}

static void row(FILE *out, long n, unsigned course, unsigned phase) {
    const uint32_t counter = scene.object_clock + scene.ticks; /* the original's 5010b0 after the pass */
    fprintf(out, "T n=%ld scene=%" PRIu32 " course=%u phase=%u ticks=%" PRIu32 " stage=%" PRIu32 " count=%" PRIu32
        " ctick=%" PRIu32 " view=", n, counter, course, phase, scene.ticks, scene.stage, scene.count, scene.camera_cursor.tick);
    for (unsigned i = 0; i < 12; ++i) fprintf(out, "%s%08" PRIx32, i ? "." : "", bits(scene.view[i]));
    uint32_t bitmaps[16] = {0};
    (void)nm_scenery_bitmaps(&scene.cells, bitmaps);
    fputs(" cells=", out);
    for (unsigned i = 0; i < 16; ++i) fprintf(out, "%s%" PRIx32, i ? "." : "", bitmaps[i]);
    const nm_tunnel_light_t *l = &scene.scene_light;
    fprintf(out, " light=%08" PRIx32 ".%08" PRIx32 ".%08" PRIx32 ".%u\n", bits(l->light[0]), bits(l->light[1]), bits(l->light[2]),
        (unsigned)l->index);
    for (unsigned id = 0; id < scene.count; ++id) {
        const nm_player_t *p = scene.cars + id;
        fprintf(out, "C n=%ld id=%u slot=%u flags=%" PRIx32 " prof=%u prev=%u lap=%u done=%u si=%" PRId32 " spd=%08" PRIx32
            " x=%08" PRIx32 " y=%08" PRIx32 " z=%08" PRIx32 " yaw=%u head=%u path=%u lat=%d want=%08" PRIx32 " road=%" PRIu32 "\n",
            n, id, id, p->boundary.damage.flags, (uint16_t)p->phase[0], (uint16_t)p->phase[1], p->lap.lap, p->lap.completed,
            p->speed_index, bits(p->boundary.speed), bits(p->boundary.ground.position[0]), bits(p->boundary.ground.position[1]),
            bits(p->boundary.ground.position[2]), (uint16_t)p->boundary.ground.yaw, (uint16_t)p->boundary.heading,
            p->path_offset, p->lateral.current, bits(p->desired_speed), p->boundary.ground.road);
    }
}

/* The demo's rows: the race trace's T and C fields the original prints (trace_race.hpp) for the demo. */
static void demo_row(FILE *out, long n, uint32_t counter, unsigned phase) {
    const nm_race_scene_t *r = &demo_race; /* course: 501460 (the next one on the last tick, nm_demo_step) */
    fprintf(out, "T n=%ld scene=%" PRIu32 " course=%u phase=%u ticks=%" PRIu32 " count=%u camera=%" PRId32 " preset=%u guided=%u"
        " ctick=%" PRIu32 " view=", n, counter, (unsigned)r->course, phase, r->ticks, r->count, r->cars[0].car.camera_mode, r->view,
        r->shared.guided_mode, demo_view.camera_cursor.tick);
    for (unsigned i = 0; i < 12; ++i) fprintf(out, "%s%08" PRIx32, i ? "." : "", bits(demo_view.view[i]));
    fputs(" cells=", out);
    for (unsigned i = 0; i < 16; ++i) fprintf(out, "%s%" PRIx32, i ? "." : "", r->shared.visible_cells[i]);
    fprintf(out, " light=%08" PRIx32 ".%08" PRIx32 ".%08" PRIx32 ".%u\n", bits(r->scene_light.light[0]), bits(r->scene_light.light[1]),
        bits(r->scene_light.light[2]), (unsigned)r->scene_light.index);
    for (unsigned id = 0; id < r->count; ++id) {
        const nm_race_car_t *c = r->cars + id; const nm_player_t *p = &c->car;
        fprintf(out, "C n=%ld id=%u slot=%u flags=%" PRIx32 " prof=%u prev=%u far=%u lap=%u done=%u cnt=%u rank=%u"
            " key=%u keyhi=%u si=%" PRId32 " spd=%08" PRIx32 " x=%08" PRIx32 " y=%08" PRIx32 " z=%08" PRIx32
            " yaw=%u head=%u path=%u lat=%d gear=%" PRIu32 " rpm=%" PRId32 " acc=%u brk=%" PRIx32 " str=%" PRId32
            " want=%08" PRIx32 " road=%" PRIu32 " gs=%08" PRIx32 " surf=%" PRIx32 " mask=%x sx=%08" PRIx32 " sz=%08" PRIx32
            " tx=%08" PRIx32 " tz=%08" PRIx32 " cside=%u crel=%" PRIx32 " dmg=%" PRIx32 " body=%" PRIx32 " crashes=%u tilt=%d pitch=%d\n",
            n, id, id, p->boundary.damage.flags, (uint16_t)p->phase[0], (uint16_t)p->phase[1], p->lap.furthest, p->lap.lap,
            p->lap.completed, p->lap.counter, c->rank, (uint16_t)p->rank_key, p->rank_key_high, p->speed_index,
            bits(p->boundary.speed), bits(p->boundary.ground.position[0]), bits(p->boundary.ground.position[1]),
            bits(p->boundary.ground.position[2]), (uint16_t)p->boundary.ground.yaw, (uint16_t)p->boundary.heading,
            p->path_offset, p->lateral.current, p->gear, p->rpm, p->accel, p->brake, p->steer, bits(p->desired_speed),
            p->boundary.ground.road, bits(c->guided_scale), p->boundary.ground.selected_surface, p->boundary.ground.combined_mask,
            bits(c->collision_motion.previous[0]), bits(c->collision_motion.previous[1]), bits(c->collision_motion.travel[0]),
            bits(c->collision_motion.travel[1]), c->collision_side, c->relative, p->boundary.damage.damage,
            p->boundary.damage.body_model, p->boundary.damage.crash_count, p->tilt[0], p->filtered_pitch);
    }
}

/* What the scenes leave each other (frontend_pool.h). */
static nm_entity_pool_t pool;
/* --credit: 5010b0 and the active phase of the iteration that saw it; UINT32_MAX: no credit. */
static uint32_t credit_counter = UINT32_MAX;
static unsigned credit_phase;
static const char *leave_file;
static int credit_find(const char *path, uint32_t after) {
    FILE *in = fopen(path, "r");
    if (!in) return 0;
    static char line[4096];
    while (fgets(line, sizeof(line), in)) {
        if (line[0] != 'F' || line[1] != ' ') continue;
        const char *b0 = strstr(line, " b0="), *next = strstr(line, " phase="), *active = strstr(line, " active=");
        if (!b0 || !next || !active) break;
        if ((uint32_t)strtoul(b0 + 4, NULL, 10) <= after || strtoul(next + 7, NULL, 10) != 12) continue;
        credit_counter = (uint32_t)strtoul(b0 + 4, NULL, 10);
        credit_phase = (unsigned)strtoul(active + 8, NULL, 10);
        break;
    }
    fclose(in);
    return credit_counter != UINT32_MAX;
}
static uint32_t entered_notification;
static int leave(unsigned course) {
    static nm_game_chain_t chain;
    chain.magic = NM_CHAIN_MAGIC; chain.counter = credit_counter; chain.course = course; chain.pool = pool;
    chain.notification = entered_notification;
    if (!nm_chain_write(leave_file, &chain)) { fputs("cannot write --leave (it must be new)\n", stderr); return 1; }
    printf("credit at 5010b0 %" PRIu32 " in phase %u, course %u\n", credit_counter, credit_phase, course);
    return 0;
}

/* The demo after a cinematic of COUNT cars: phase 6 at 5010b0 COUNTER, then its 834 ticks, each with the camera pass. */
static int run_demo(FILE *out, long *n, unsigned course, unsigned count, uint32_t counter) {
    static nm_race_cameras_t cameras;
    const nm_race_course_start_t *tables = nm_race_course_starts + course;
    cameras = (nm_race_cameras_t){tables->tracks,
#ifdef NM_RACE_VIEW_DATA
        tables->pit_views,
#else
        NULL,
#endif
        nm_race_crash_keys, nm_race_crash_times, NM_RACE_CRASH_KEYS, tables->tunnel};
    nm_frontend_pool_from_intro(&pool, &scene, &road);
    if (!nm_demo_start(&demo_race, &demo, math, &contact, &road, paths, count, pool.view, nm_race_view_presets,
            &marks, road.profile_count, &pool, counter & 3u)) { fprintf(stderr, "course %u: demo start refused\n", course); return 0; }
    demo_race.counter_base = counter + 1u; /* code/practice.c: the clock actors' 5010b0 */
    const char *first = nm_demo_view_start(&demo_race, &demo_view, math, &scenery, &cameras, &pool);
    if (first) { fprintf(stderr, "course %u: demo start camera pass: %s\n", course, first); return 0; }
    demo_row(out, (*n)++, counter, 6);
    for (unsigned t = 0; t < NM_DEMO_TICKS; ++t) {
        const uint32_t b0 = counter + 1u + t;
        if (!nm_demo_step(&demo_race, &demo, math, &nm_intro_speeds, &contact, &road, paths, &marks, road.profile_count,
                b0 & 3u, (uint8_t)(b0 & 1u), nm_intro_crash_sequences[0].rows, nm_intro_crash_sequences[0].count,
                t + 1 < NM_DEMO_TICKS || b0 == credit_counter ? course : nm_attract_next_course(course))) {
            fprintf(stderr, "course %u: demo tick %u refused (car %u)\n", course, t, demo_race.shared.updating); return 0;
        }
        if (actors_course != 99) (void)nm_props_race_step(&demo_race, math, &contact); /* code/practice.c */
        const char *why = nm_race_camera_pass(&demo_race, &demo_view, math, &scenery, &cameras, 0);
        if (why) { fprintf(stderr, "course %u: demo tick %u camera pass: %s\n", course, t, why); return 0; }
        demo_row(out, (*n)++, b0, 7);
        clock_row_of(b0, 100000 + (long)t, &demo_race, &demo_view, 0);
        if (b0 == credit_counter) {
            if (credit_phase != 7) { fprintf(stderr, "the credit at 5010b0 %" PRIu32 " is in phase %u, here 7\n", b0, credit_phase); return 0; }
            if (!nm_demo_game_start(&demo_race, math, &nm_intro_speeds, &contact, &road, paths, &marks, road.profile_count,
                    b0 + 1u, nm_intro_crash_sequences[0].rows, nm_intro_crash_sequences[0].count)) {
                fprintf(stderr, "course %u: the demo's game start refused\n", course); return 0;
            }
            nm_frontend_pool_from_race(&pool, &demo_race, &demo_view.cells);
            return 2;
        }
    }
    if (!nm_demo_field_off(&demo_race, math, &road, 8, counter + 1u + NM_DEMO_TICKS)) { fprintf(stderr, "course %u: phase 8 refused\n", course); return 0; }
    nm_frontend_pool_from_race(&pool, &demo_race, &demo_view.cells);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fputs("usage: trace_attract_native ASSET_DIR NEW_OUTPUT LOOPS [--course C] [--clock CLOCK_OUT REFERENCE_TRACE]\n", stderr);
        return 2;
    }
    const char *assets = argv[1];
    const long loops = strtol(argv[3], NULL, 10);
    unsigned course = 0;
    const char *enter_file = NULL, *credit_file = NULL;
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--course") && i + 1 < argc) course = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--clock") && i + 2 < argc) {
            if (!(clock_out = fopen(argv[++i], "wx")) || !reference_frames(argv[++i])) {
                fputs("--clock: cannot create the output or read the reference trace\n", stderr); return 2;
            }
        } else if (!strcmp(argv[i], "--actors") && i + 1 < argc) actors_course = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--enter") && i + 1 < argc) enter_file = argv[++i];
        else if (!strcmp(argv[i], "--credit") && i + 1 < argc) credit_file = argv[++i];
        else if (!strcmp(argv[i], "--leave") && i + 1 < argc) leave_file = argv[++i];
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    char path[1024];
    if (!nm_course_asset(path, sizeof(path), assets, 0, NM_COURSE_DRIVE, 0) || !(math = nm_drive_load(path))) {
        fputs("cannot load drive.dnd\n", stderr); return 2;
    }
    FILE *out = fopen(argv[2], "wx");
    if (!out) { fputs("cannot create the output (it must be new)\n", stderr); return 2; }
    fputs("# attract-trace v1 side=native\n", out);
    nm_frontend_pool_reset(&pool);
    uint32_t counter = 1; /* 5010b0 after power-on's phase 0 iteration */
    if (enter_file) {
        static nm_game_chain_t chain;
        if (!nm_chain_read(enter_file, &chain)) { fputs("--enter: cannot read the state\n", stderr); return 2; }
        pool = chain.pool; counter = chain.counter; course = chain.course; entered_notification = chain.notification;
    }
    if (credit_file && (!leave_file || !credit_find(credit_file, counter))) {
        fputs("--credit needs --leave and a credit in the trace\n", stderr); return 2;
    }
    long n = 0;
    for (long loop = 0; loop < loops; ++loop) {
        const nm_podium_course_t *graph = course < 3 ? nm_attract_courses + course : NULL;
        if (!graph || !graph->stages || !load(assets, course)) { fprintf(stderr, "course %u: no attract graph or packs\n", course); return 1; }
        if (clock_out && clock_course != course && !clock_open(assets, course)) { fprintf(stderr, "course %u: bad actors file\n", course); return 1; }
        if (actors_course != 99) nm_props_world_actors(clock_data, course);
        ++counter; /* phase 2 */
        if (!nm_intro_scene_start_attract(&scene, graph->stages, graph->count, math, &road, paths) ||
            !nm_intro_scene_detail(&scene, math, &scenery)) { fprintf(stderr, "course %u: attract start refused\n", course); return 1; }
        scene.object_clock = counter + 1u;
        scene.phase_clock = scene.object_clock - 3u; /* 503690/503692: 5010b0 - ticks - 3 (the first loop: 0) */
        if (!nm_attract_carry_into_cinematic(&scene, &pool, &road)) { fprintf(stderr, "course %u: carry refused\n", course); return 1; }
        nm_intro_scene_light(&scene, nm_race_course_starts[course].tunnel);
        row(out, n++, course, 3);
        while (scene.ticks < NM_ATTRACT_TICKS) {
            const unsigned phase = nm_attract_phase(scene.ticks);
            scene.flags = UINT32_C(1) << phase;
            if (!nm_intro_scene_step(&scene, math, &contact, &road, paths) || !nm_intro_scene_detail(&scene, math, &scenery)) {
                fprintf(stderr, "course %u: attract tick %" PRIu32 " refused\n", course, scene.ticks); return 1;
            }
            if (actors_course != 99) nm_props_intro_step(&scene, math, &contact); /* code/practice.c */
            row(out, n++, course, phase);
            clock_row_of(scene.object_clock + scene.ticks, scene.ticks, &race, &scene, 1);
            if (scene.object_clock + scene.ticks == credit_counter) {
                if (credit_phase != phase) { fprintf(stderr, "the credit at 5010b0 %" PRIu32 " is in phase %u, here %u\n", credit_counter, credit_phase, phase); return 1; }
                if (!nm_frontend_game_start(&scene, math, &contact, &road, paths, &pool)) { fprintf(stderr, "course %u: game start refused\n", course); return 1; }
                fclose(out); unload();
                return leave(course);
            }
        }
        printf("loop %ld: course %u, 5010b0 %" PRIu32 "..%" PRIu32 ", %" PRIu32 " stage transitions, %" PRIu32 " collision pairs, %" PRIu32 " responses\n",
            loop, course, scene.object_clock, scene.object_clock + scene.ticks, scene.transitions, scene.collision_pairs,
            scene.collision_responses);
        const int demo_ran = run_demo(out, &n, course, scene.count, scene.object_clock + scene.ticks + 1u);
        if (!demo_ran) return 1;
        if (demo_ran == 2) { fclose(out); unload(); return leave(course); }
        counter = scene.object_clock + scene.ticks + NM_ATTRACT_DEMO_ITERATIONS; /* phases 6, 7 and 8 */
        /* The title: its objects leave the pool as it is, and phases 12, 17 find every car off (the original's pool
         * over phases 8 to 18 of a credit in the title: no entity word changes). 2058 stepped the course. */
        if (credit_counter != UINT32_MAX && credit_counter > counter && credit_counter <= counter + NM_ATTRACT_TITLE_TICKS) {
            if (credit_phase != 9) { fprintf(stderr, "the credit at 5010b0 %" PRIu32 " is in phase %u, here 9\n", credit_counter, credit_phase); return 1; }
            fclose(out); unload();
            return leave(nm_attract_next_course(course));
        }
        counter += NM_ATTRACT_TITLE_TICKS + 1u;                                  /* phases 9 and 10 */
        course = nm_attract_next_course(course);
    }
    fclose(out);
    if (clock_out) fclose(clock_out);
    unload();
    return 0;
}
