/* The course props ("pylons": course 0's ten traffic cones, Advanced's 15 and Expert's 35 props of
 * four kinds), handlers 222d30 (collision manager, task 50ef80), 223114, 223078, 2230ac, 2230e0
 * (the init entries of the four kinds, one task a prop, 50f000 + 80 (id-1)), 2231c0 (every kind's
 * tick) and 22302c (sound manager, the task after the last prop): the pool rows after every car and
 * the camera, before the slot machine (row 109). Specification:
 * build/dc-native-world-anim.LwFeBn/spec-cones.md. The kinds differ only in what their init entry
 * stores (task +5c rest record, +64 knock sets, +68 hit height limit, +70 sound) and in their object
 * list (the model); the manager's argument is the footprint (22382c on course 0, 22385c on the others).
 *
 * Presentation only. A cone writes nothing a car, the race or the random number reads:
 * its own tasks, the block 503604..503663 and one sound command (9e1d7f). Only the player
 * car (5012f0, car 0) knocks cones: a cone whose centre lies inside the car's footprint
 * (22382c, by its position, yaw +58 and roll +2c) while its motion record is low (height
 * <= 1.2) picks a recorded trajectory (the slow set below speed index 70, else one of the
 * fast set by a shared counter) and replays it one 16-byte record a tick, turned to the
 * car's heading, with a decaying excess speed (2 x car speed - record speed), mirrored
 * off ground quads of surface type 0 or 5; then it rests in its last pose until the
 * scene is rebuilt. The float arithmetic is the TGP's, one operation at a time, so the
 * state is the arcade's bit for bit (tests/test_props.c replays the original game's
 * own runs). Built without fast math (local.cfg.mk), outside the LTO unit (Makefile).
 *
 * The ROM tables (records, sets, footprint, actor positions) come with the race-start
 * header (tools/prepare_intro.py --race-start: NM_PROP_DATA, nm_race_props). */
#ifndef DAYTONA_NATIVE_PROPS_H
#define DAYTONA_NATIVE_PROPS_H
#include <stdint.h>
#include "contact.h"

#define NM_PROPS_MAX 35u   /* Expert's */
#define NM_PROPS_QUEUE 64u
#define NM_PROPS_SETS 4u
#define NM_PROP_KINDS 4u   /* the init entries 223114, 223078, 2230ac, 2230e0 */

/* One 16-byte motion record (+0 speed, +4 height offset as float bits; +8 travel direction,
 * +a/+c/+e rotations about X, Y, Z). */
typedef struct { uint32_t w0, w4; int16_t h8, ha, hc, he; } nm_prop_record_t;
/* records[first..]; count = the table's u32; address: the table's ROM address (tests/test_props.c). */
typedef struct { uint16_t first, count; uint32_t address; } nm_prop_table_t;
/* What one kind's init entry stores (223114: 2896d84, 2896dc4, 1.2, 9e1d7f). */
typedef struct {
    uint16_t rest;                       /* task +5c at init: the rest record (index) */
    uint8_t set_count[2];                /* +64: [0] slow set (speed index <= 70), [1] fast set */
    uint8_t set[2][NM_PROPS_SETS];       /* table indices of each set, in the set's order */
    uint32_t limit;                      /* task +68: the highest record still hit (223114: 0x3f99999a, 1.2) */
    uint32_t sound;                      /* task +70: the hit sound (223114: 9e1d7f) */
    uint32_t rest_address;               /* the rest record's ROM address (tests/test_props.c) */
} nm_prop_kind_t;
typedef struct {
    const nm_prop_record_t *records;     /* every course's records, shared */
    const nm_prop_table_t *tables;
    uint16_t record_count, table_count;
    nm_prop_kind_t kind[NM_PROP_KINDS];
    float corners[4][3];                 /* the manager's argument: the footprint corners in the car's frame */
    uint8_t cones;                       /* the props of the course, in task order */
    struct { float x, y, z; int16_t heading; uint8_t id, kind; } cone[NM_PROPS_MAX];
} nm_prop_data_t;

/* A cone task (spec section 4) and the pose its last tick drew. */
typedef struct {
    float x, y, z, v60;                  /* +1c, +20 (ground height), +24; +60 excess speed */
    uint32_t face;                       /* +6c ground quad */
    uint16_t count, cell, rec;           /* +6 records left, +2e cell, +5c record (index) */
    int16_t h52, h54, h56, h58, h5a;     /* motion heading, slope X, Z, drawn yaw, previous direction */
    uint8_t id, hit, kind;               /* +4; flags bit 1; the init entry (nm_prop_data_t.kind) */
    /* The draw of the last tick (223528): drawn, at (x, draw_y, z) with the angles. */
    uint8_t drawn;
    float draw_y;
    int16_t yaw, he, hc, ha;
} nm_prop_t;
typedef struct {
    nm_prop_t cone[NM_PROPS_MAX];
    float perimeter[4][3];               /* TGP 0x31 quad: corners 0, 1, 3, 2 of the footprint */
    float speed2, speed01;               /* 503604, 503608 */
    uint32_t counter, sound, lo, hi;     /* 50360c, 503614, 503618, 50361c */
    uint16_t cooldown;                   /* 503620 */
    uint8_t write, read;                 /* 503622, 503623 */
    uint8_t queue[NM_PROPS_QUEUE];       /* 503624.. */
    uint8_t count, started;
    uint32_t last_tick;                  /* the caller's clock: a smaller one is a new scene */
    uint8_t course;                      /* the caller's course of the last tick: another one is a new scene */
    uint32_t hits, sent;                 /* counters for the exit report */
} nm_props_t;
/* The player entity's words the props read (+1c..+24, +2c, +34, +18, +48, +58). */
typedef struct { float position[3], speed; uint32_t speed_index; int16_t roll, yaw, heading; } nm_props_player_t;

#ifdef __cplusplus
extern "C" {
#endif
/* 2002c and the three init entries (222d30, 223114 + 223144, 22302c): the first tick of a scene. */
int nm_props_init(nm_props_t *s, const nm_prop_data_t *data, const nm_drive_params_t *math,
                  const nm_contact_pack_t *contact);
/* 222dd0: the manager's tick. */
int nm_props_manager(nm_props_t *s, const nm_drive_params_t *math, const nm_prop_data_t *data,
                     const nm_props_player_t *player);
/* 2231c0: cone i's tick; bitmaps: the draw bitmap 501520 the actors see (the previous pass's). */
int nm_props_cone(nm_props_t *s, unsigned i, const nm_prop_data_t *data, const nm_drive_params_t *math,
                  const nm_contact_pack_t *contact, const nm_props_player_t *player, const uint32_t bitmaps[16]);
/* 22302c: the sound manager; the command 19bdc is asked to queue this tick (0: none). */
uint32_t nm_props_sound_manager(nm_props_t *s);
/* One whole tick in row order, init on the first. *sound: the command for 19bdc (0: none). */
int nm_props_tick(nm_props_t *s, const nm_prop_data_t *data, const nm_drive_params_t *math,
                  const nm_contact_pack_t *contact, const nm_props_player_t *player, const uint32_t bitmaps[16],
                  uint32_t *sound);

/* The game's two instances (code/props.c): the race's, stepped by the race tick, and the cinematic
 * scenes' (attract, podium). 0 when the build has no prop data (an older race-start header). */
const nm_props_t *nm_props_race(void);
const nm_props_t *nm_props_intro(void);
#ifdef __cplusplus
}
#endif
#endif
