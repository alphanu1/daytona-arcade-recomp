/* The game's props (include/props.h): the race's instance, stepped once a race tick, and the cinematic
 * scenes' (attract and podium), once an intro tick. Presentation only; outside the LTO unit (Makefile). */
#include "props.h"
#include "props_scene.h"
#include "expert_actors.h"
#include "race_scene.h"
#include "intro_scene.h"
#include "race_start_data.h" /* NM_PROP_DATA: nm_race_prop_courses */
#include <stdio.h>

static nm_props_t race_props, intro_props;
static uint8_t race_failed, intro_failed;
/* Expert's world actors (include/expert_actors.h): the course's DNA1 file with actors (render/scene/practice_assets.c
 * hands it over at the world's load), the race's and the cinematic scenes' instances, the statue's words from
 * power-on, the START edges practice.c counts (53e584 bit 4) and the counter base of the last race. */
static const void *xa_dna;
static unsigned xa_course;
static nm_xa_state_t xa_race, xa_intro;
static uint8_t xa_race_failed, xa_intro_failed;
static nm_xa_shared_t xa_shared;
static const unsigned *xa_start_edges;
static unsigned xa_start_seen;
static uint32_t xa_counter_base;

static void player_words(const nm_player_t *car, nm_props_player_t *out) {
    for (unsigned a = 0; a < 3; ++a) out->position[a] = car->boundary.ground.position[a];
    out->speed = car->boundary.speed;
    out->speed_index = (uint32_t)car->speed_index;
    out->roll = car->boundary.ground.roll;
    out->yaw = car->boundary.ground.yaw;
    out->heading = car->boundary.heading;
}

/* The course's props (the race-start header's nm_race_prop_courses[course]), or NULL. */
static const nm_prop_data_t *course_props(unsigned course) {
#ifdef NM_PROP_COURSES
    return course < 3 && (NM_PROP_COURSES >> course & 1u) ? nm_race_prop_courses[course] : NULL;
#elif defined(NM_PROP_DATA)
    return course ? NULL : &nm_race_props;   /* an older header: course 0's cones */
#else
    (void)course; return NULL;
#endif
}

/* One tick of a scene's props; a smaller clock than the last or another course is a new scene (2002c made the
 * tasks again). */
static uint32_t step(nm_props_t *s, uint8_t *failed, uint32_t clock, unsigned course, const nm_player_t *car,
                     const uint32_t bitmaps[16], const nm_drive_params_t *math, const nm_contact_pack_t *contact) {
    const nm_prop_data_t *data = course_props(course);
    if (!s->started || clock <= s->last_tick || course != s->course) { s->started = 0; *failed = 0; }
    s->last_tick = clock; s->course = (uint8_t)course;
    if (*failed || !data) return 0;
    nm_props_player_t player;
    player_words(car, &player);
    uint32_t sound = 0;
    if (!nm_props_tick(s, data, math, contact, &player, bitmaps, &sound)) {
        printf("Native props: tick refused at %lu; the cones stay at rest\n", (unsigned long)clock);
        *failed = 1; s->started = 0;
        return 0;
    }
    return sound;
}

/* 53e584 bit 4 on this tick of the race, the podium or the results: a START edge counted since the last tick that
 * stepped the actors (the original's tasks read the same word in every scene: 22231c, 22475c). */
static uint8_t xa_start_edge(void) {
    const unsigned edges = xa_start_edges ? *xa_start_edges : 0u;
    const uint8_t edge = edges != xa_start_seen;
    xa_start_seen = edges;
    return edge;
}

/* The player entity's words Expert's actors read. */
static void xa_player(const nm_player_t *car, nm_xa_input_t *in) {
    in->player_flags = car->boundary.damage.flags;
    for (unsigned a = 0; a < 3; ++a) in->player_position[a] = car->boundary.ground.position[a];
    in->player_speed = car->boundary.speed;
    in->player_speed_index = car->speed_index;
    in->player_heading = (uint16_t)car->boundary.heading;
    in->player_profile = (uint16_t)car->phase[0];
}
static void xa_clear(nm_xa_state_t *s) {
    for (unsigned g = 0; g < NM_CLOCK_GROUPS_MAX; ++g) s->draw[g].frame = -1;
}
/* One tick of a scene's Expert actors; a smaller clock than the last or another course is a new scene. */
static void xa_step(nm_xa_state_t *s, uint8_t *failed, uint32_t clock, unsigned course, const nm_xa_input_t *in,
                    const nm_drive_params_t *math, const nm_contact_pack_t *contact, int initialised) {
    if (!s->started || clock <= s->last_tick || course != xa_course) { s->started = 0; *failed = 0; }
    s->last_tick = clock;
    xa_clear(s);
    if (*failed || !xa_dna || course != xa_course) return;
    /* initialised: the scene's tasks ran their init entries on the tick before this first step (the podium). */
    if ((!s->started && initialised && !nm_xa_tick(s, &xa_shared, xa_dna, math, contact, in)) ||
        !nm_xa_tick(s, &xa_shared, xa_dna, math, contact, in)) {
        printf("Native world actors: tick refused at %lu; they draw nothing\n", (unsigned long)clock);
        *failed = 1; s->started = 0;
        xa_clear(s);
    }
}

uint32_t nm_props_race_step(const nm_race_scene_t *race, const nm_drive_params_t *math, const nm_contact_pack_t *contact) {
    const uint8_t edge = xa_start_edge();
    /* Rows 75..86 are world-actor rows: suspended while the pit crew works (dd9c), as the slot machine. */
    if (race->shared.pit.suspended || !race->count) { xa_clear(&xa_race); return 0; }
    const nm_player_t *car = &race->cars[0].car;
    if (xa_dna) {
        nm_xa_input_t in = {.bitmaps = race->shared.visible_cells, .counter = race->counter_base + race->ticks - 1u,
            .flags = race->shared.flags, .phase_timer = 0, .reverse = car->checkpoints.reverse, .start_edge = edge};
        xa_player(car, &in);
        /* The camera entity 501240's x, z: the follow view copies the player's position into it; while a keyed
         * camera runs (camera mode 1: the rolling start, the crash camera) it is 0, 0 (the original's
         * 501240 +1c/+24 through a crash of the Expert time-out run). */
        if (car->camera_mode != 1) { xa_race.origin[0] = car->boundary.ground.position[0]; xa_race.origin[1] = car->boundary.ground.position[2]; }
        else xa_race.origin[0] = xa_race.origin[1] = 0.0f;
        in.camera[0] = xa_race.origin[0]; in.camera[1] = xa_race.origin[1];
        xa_counter_base = race->counter_base;
        xa_step(&xa_race, &xa_race_failed, race->ticks, race->course, &in, math, contact, 0);
    }
    return step(&race_props, &race_failed, race->ticks, race->course, car, race->shared.visible_cells,
                math, contact);
}

void nm_props_results_step(const nm_race_scene_t *race, uint32_t after, unsigned results_phase,
                           const nm_drive_params_t *math, const nm_contact_pack_t *contact) {
    if (!xa_dna || !race->count) return;
    /* The results of a race that ends there: its pool runs on, the actors with it (5010b0 counting on, 501090 the
     * results phase's bit; the player stands). */
    const nm_player_t *car = &race->cars[0].car;
    nm_xa_input_t in = {.bitmaps = race->shared.visible_cells, .counter = race->counter_base + race->ticks - 1u + after,
        .flags = UINT32_C(1) << (results_phase & 31u), .phase_timer = 0, .reverse = car->checkpoints.reverse, .start_edge = xa_start_edge()};
    xa_player(car, &in);
    in.camera[0] = xa_race.origin[0]; in.camera[1] = xa_race.origin[1];
    xa_step(&xa_race, &xa_race_failed, race->ticks + after, race->course, &in, math, contact, 0);
}

void nm_props_intro_step(const nm_intro_scene_t *intro, const nm_drive_params_t *math, const nm_contact_pack_t *contact) {
    uint32_t bitmaps[16];
    if (!nm_scenery_bitmaps(&intro->cells_previous, bitmaps)) return;
    (void)step(&intro_props, &intro_failed, intro->ticks, intro->course, &intro->cars[0], bitmaps, math, contact); /* sound gated off */
    if (!xa_dna) return;
    /* The cinematic scenes: the podium (501090 bit 28) counts on from the race (clock_scene.h); its phase timer
     * 5010a8 is 1598 - ticks (podium.h: the spawn tick is the scene's tick 0). The camera entity stays at 0, 0. */
    const int podium = (intro->flags & UINT32_C(0x10000000)) != 0;
    nm_xa_input_t in = {.bitmaps = bitmaps, .counter = (podium ? xa_counter_base : 0u) + intro->object_clock + intro->ticks,
        .flags = intro->flags, .phase_timer = podium ? 1598 - (int32_t)intro->ticks : 0, .reverse = 0, .start_edge = xa_start_edge()};
    xa_player(&intro->cars[0], &in);
    in.camera[0] = in.camera[1] = 0.0f;
    /* The podium's tasks are made on its entry tick (03b84: their init entries run there, the original's first draws
     * are on the spawn tick, the scene's tick 0: 224cd0's pair in the Expert finish run). */
    xa_step(&xa_intro, &xa_intro_failed, intro->ticks, intro->course, &in, math, contact, podium);
}

void nm_props_world_actors(const void *dna, unsigned course) {
    xa_dna = dna && nm_clock_actor_count(dna) ? dna : NULL; xa_course = course;
    xa_race.started = xa_intro.started = 0;
    xa_clear(&xa_race); xa_clear(&xa_intro);
}
void nm_props_start_edges(const unsigned *edges) { xa_start_edges = edges; xa_start_seen = edges ? *edges : 0u; }
const nm_xa_draw_t *nm_props_world_actor_draws(int cinematic) {
    if (!xa_dna) return NULL;
    const nm_xa_state_t *s = cinematic ? &xa_intro : &xa_race;
    return s->started ? s->draw : NULL;
}

const nm_props_t *nm_props_race(void) { return race_props.started ? &race_props : NULL; }
const nm_props_t *nm_props_intro(void) { return intro_props.started ? &intro_props : NULL; }

void nm_props_report(void) {
    printf("Native props: race %u cones, %lu hits, %lu sounds; cinematic %lu hits\n", (unsigned)race_props.count,
           (unsigned long)race_props.hits, (unsigned long)race_props.sent, (unsigned long)intro_props.hits);
    if (xa_dna) printf("Native world actors: statue entries %u, zone %u\n", (unsigned)xa_shared.entries, (unsigned)(xa_shared.inside & 1u));
}
