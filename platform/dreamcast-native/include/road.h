#ifndef DAYTONA_NATIVE_ROAD_H
#define DAYTONA_NATIVE_ROAD_H
#include <math.h>
#include <stdint.h>
#include "path.h"
#include "nm_mem.h"

/* Authored daytona93 profile consumed by 0x13b28, not a render mesh or a
 * collision surface. Coordinates/angles remain in reference units. The last
 * short in each lane is retained. profile_control selects collision correction. */
typedef struct { float xyz[3]; int16_t angles[4]; } nm_road_lane_t;
typedef struct {
    int8_t first_lane, last_lane;
    int16_t profile_control;
    nm_road_lane_t lanes[8];
} nm_road_profile_t;
typedef struct {
    int16_t current, target, control, step; /* entity +13e/+140/+142/+144 */
} nm_road_lateral_t;
typedef struct { float xyz[3]; int32_t angles[3]; } nm_road_pose_t;
typedef struct { uint16_t profile, flags; } nm_road_map_t;
/* Four authored difficulty settings. Include index256: source increments a
 * byte counter in a32-bit register before lookup, only then stores low8. */
typedef struct {
    uint32_t ticks[4][257], adjustment[2][4], cue[16];
    uint8_t signal[16];
} nm_road_awards_t;
/* DNR5 aligned32-byte grid record; ID is its index. Speed remains original
 * parameter bits, not final physical velocity. previous also initializes furthest. */
typedef struct {
    uint32_t speed_parameter, profile, previous, path_offset, lateral;
    int32_t lap;
    uint32_t route, style;
} nm_road_grid_t;
/* DNR5:32-byte little-endian header (magic,file bytes,slot,profile/map/checkpoint
 * counts,total laps,grid count), then164-byte profiles,4-byte pre-folded map records
 * then4224-byte award parameters and ordered uint16 checkpoint profiles.
 * Zero padding to32 bytes precedes grid records. DNR3/4 have no grid or padding.
 * DNR6 appends grid_count float32 speed products, zero padding to32 bytes,
 * then one32-byte nm_road_start_t. DNR5 ends after the grid records.
 * DNR7 inserts aligned grid_count float32 guided scales before the start block.
 * Profile count includes the distinct, map-addressable terminal record. */
typedef struct {
    uint32_t body_model[4];
    float default_speed, rolling_speed;
    uint32_t reserved[2];
} nm_road_start_t; /* DNR6 aligned32-byte immutable startup constants. */
typedef struct {
    void *storage;
    const nm_road_profile_t *profiles;
    const nm_road_map_t *mapping;
    const uint16_t *checkpoints;
    const nm_road_awards_t *awards;
    uint32_t slot, profile_count, map_count, payload_bytes, checkpoint_count;
    uint32_t total_laps; /* Zero for legacy DNR3: unavailable, not a race default. */
    const nm_road_grid_t *grid;
    uint32_t grid_count;
    const float *grid_speeds; /* DNR6 offline e298 products, aligned array. */
    const nm_road_start_t *start; /* NULL for legacy DNR3/4/5. */
    const float *grid_guided_scales; /* DNR7 offline e2c0 division results. */
} nm_road_pack_t;
typedef struct {
    uint32_t road; /* entity30 */
    uint16_t offset, phase, previous_phase, flags, previous_flags; /*134,128,12a,138,13a*/
} nm_road_selection_t;
/* Shared mutable course state, separate from immutable DNR2 and each car.
 * count is profile_count-1; caller owns count bytes, disjoint from car state. */
typedef struct { uint8_t *values; uint32_t count, timer; } nm_road_marks_t;

/*13e3c..13ec0. Always clear the sixteen-behind slot first. Only unsigned
 * speed<128 writes the remaining ramp, in descending order (aliases matter
 * on short courses). Preserve32-bit wrapping BEFORE remainder. */
static inline int nm_road_marks_step(nm_road_marks_t *marks, uint16_t profile,
        int32_t speed, int8_t input_inhibit, int16_t inhibit) {
    if (!marks || !marks->values || !marks->count || marks->count > 4096) return 0;
    if (marks->timer) --marks->timer;
    if (input_inhibit || inhibit) marks->timer = 160;
    marks->values[((uint32_t)profile+marks->count-16u)%marks->count] = 0;
    if ((uint32_t)speed < 128) {
        for (unsigned behind = 16; behind-- > 0;)
            marks->values[((uint32_t)profile+marks->count-behind)%marks->count] = (uint8_t)(behind>>2);
    }
    return 1;
}
#ifdef __cplusplus
extern "C" {
#endif
int nm_road_load(nm_road_pack_t *p, const char *path);
void nm_road_unload(nm_road_pack_t *p);
#ifdef __cplusplus
}
#endif

/*10980..10a10. Normal map flags already include the immutable profile flags,
 * folded offline. Held-surface routing preserves BOTH flag fields; only that
 * branch needs path/math/position. Source uses full road ID normally, low16
 * after cursor lookup. Keep previous values and publish only on success. */
static inline int nm_road_select(const nm_road_pack_t *p, const nm_path_pack_t *path,
        const nm_drive_params_t *math, uint16_t surface, uint32_t entity_flags,
        float x, float z, nm_road_selection_t *state) {
    if (!p || !p->mapping || !p->profile_count || !p->map_count || !state) return 0;
    nm_road_selection_t s = *state;
    s.previous_phase = s.phase;
    uint32_t road = s.road;
    if (surface&0x50c) {
        if (!path || path->slot != p->slot) return 0;
        nm_path_cursor_t cursor = {s.offset,0,s.road};
        if (!nm_path_cursor_step(path,math,x,z,(entity_flags>>28)&1,&cursor)) return 0;
        s.road = cursor.road; s.offset = cursor.offset; road = s.road&65535;
    }
    if (road >= p->map_count || p->mapping[road].profile >= p->profile_count) return 0;
    s.phase = p->mapping[road].profile;
    if (!(surface&0x50c)) { s.previous_flags = s.flags; s.flags = p->mapping[road].flags; }
    *state = s;
    return 1;
}

/*13a14..13b24: signed lateral position from authored lane4's X/Z/yaw.
 * Keep ordered float32 operations, truncate BEFORE wrapped+1024/clamping.
 * Reject undefined reference conversions without publishing a partial result. */
static inline int nm_road_measure_lateral(const nm_drive_params_t *math,
        const nm_road_profile_t *profile, float x, float z, int16_t *out) {
    if (!math || !profile || !out) return 0;
    const nm_road_lane_t *lane = &profile->lanes[4];
    const uint16_t angle = (uint16_t)((int)lane->angles[1]*4);
    const float dx = x-lane->xyz[0], dz = z-lane->xyz[2];
    const float a = nm_drive_sine(math,(uint16_t)(angle+16384u))*dx;
    const float b = nm_drive_sine(math,angle)*dz;
    const float scaled = -85.0f*(a+b);
    if (!isfinite(scaled) || scaled < -2147483648.0f || scaled >= 2147483648.0f) return 0;
    const uint32_t wrapped = (uint32_t)(int32_t)scaled+1024u;
    int32_t lateral; nm_memcpy(&lateral,&wrapped,4);
    *out = (int16_t)(lateral < 0 ? 0 : lateral > 1792 ? 1792 : lateral);
    return 1;
}

/* One reference lateral/spawn step, NOT player physics or elapsed-time
 * integration. Keep strict float operations and contraction disabled. */
static inline int nm_road_pose_step(const nm_road_profile_t *profile,
                                    nm_road_lateral_t *state, nm_road_pose_t *pose) {
    if (!profile || !state || !pose || profile->first_lane < 0 ||
        profile->last_lane > 7 || profile->first_lane > profile->last_lane) return 0;
    int delta = (int)state->target - state->current, step = state->step;
    if (delta <= -16) { if (step > -16) --step; }
    else if (delta >= 16) { if (step < 16) ++step; }
    else step = 0;
    const int change = delta > -16 && delta < 16 ? delta : step;
    /* i960 SHRI rounds a negative half downward, unlike signed C division. */
    const int move = state->control > 224 ? (change < 0 ? -((-change + 1)/2) : change/2) : change;
    int current = (int)state->current + move;
    if (current < 0) current = 0;
    if (current > 7*256) current = 7*256;
    int low = current >> 8, high = low + 1;
    if (low < profile->first_lane) low = profile->first_lane;
    if (low > profile->last_lane) low = profile->last_lane;
    if (high < profile->first_lane) high = profile->first_lane;
    if (high > profile->last_lane) high = profile->last_lane;
    /* All 256 ROM entries at 0x028b7d20 are float32(i/255), not i/256. */
    const float weight = (float)(current & 255) / 255.0f;
    nm_road_pose_t result;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const float a = profile->lanes[high].xyz[axis] * weight;
        const float b = profile->lanes[low].xyz[axis] * (1.0f - weight);
        result.xyz[axis] = b + a;
        if (!isfinite(result.xyz[axis])) return 0;
        result.angles[axis] = profile->lanes[high].angles[axis];
    }
    result.angles[1] *= 4; /* Reference takes upper-lane angles, not a blend. */
    state->current = (int16_t)current;
    state->step = (int16_t)step;
    *pose = result;
    return 1;
}
#endif
