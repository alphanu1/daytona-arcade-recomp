#ifndef DAYTONA_NATIVE_BOUNDARY_H
#define DAYTONA_NATIVE_BOUNDARY_H
#include "nm_mem.h"
#include "force_math.h"
#include "wheels.h"
#include "damage.h"

enum { NM_BOUNDARY_NONE, NM_BOUNDARY_RECOVER, NM_BOUNDARY_PIVOT, NM_BOUNDARY_SPIN };
typedef struct {
    float height;
    uint32_t selected;
    uint16_t center_mask;
    uint8_t action;
} nm_boundary_selection_t;

/* 16060 prefix through dispatch: one bounded center query, then ordered wheel,
 * height and heading tests. This selects a response; it does not execute the
 * recovery/spin branches. Only height/center_mask are persistent writes here;
 * selected is meaningful for PIVOT/SPIN. Do not change wheel/road identities
 * with the center query, whose returned identities the source discards. */
static inline int nm_boundary_select(const nm_contact_pack_t *pack,
        const nm_drive_params_t *p, const nm_wheels_state_t *wheels,
        int16_t heading, nm_boundary_selection_t *state) {
    if (!pack || !pack->storage || !p || !wheels || !state) return 0;
    nm_boundary_selection_t next = *state;
    next.height = wheels->position[1]; next.action = NM_BOUNDARY_NONE;
    const unsigned common = wheels->mask[0]&wheels->mask[1]&wheels->mask[2]&wheels->mask[3];
    if (!(common&2)) {
        if (common&33) next.action = NM_BOUNDARY_RECOVER;
        else if (!(common&12)) {
            nm_contact_result_t center = {next.height,wheels->road,wheels->selected_surface,0};
            if (!nm_contact_query(pack,p,wheels->position[0],wheels->position[2],&center)) return 0;
            next.height = center.height; next.center_mask = (uint16_t)center.mask;
            if (center.mask&32) next.action = NM_BOUNDARY_RECOVER;
            else {
                unsigned v = 0;
                while (v < 4 && !(wheels->mask[v]&33)) ++v;
                next.selected = v < 4 ? wheels->surface[v] : wheels->selected_surface;
                if (v < 4) {
                    if (next.selected >= pack->quad_count) return 0;
                    const int angle = nm_yaw_angle(pack->quads[next.selected].heading-(uint32_t)(int32_t)heading);
                    next.action = ((angle >= 4096 && angle <= 28671) ||
                                   (angle >= -28671 && angle <= -4096)) ? NM_BOUNDARY_SPIN : NM_BOUNDARY_PIVOT;
                } else {
                    const unsigned pairs[4][2] = {{0,2},{1,3},{0,1},{2,3}};
                    for (unsigned i = 0; i < 4; ++i) {
                        const float a = wheels->height[pairs[i][0]], b = wheels->height[pairs[i][1]];
                        if (!isfinite(a) || !isfinite(b)) return 0;
                        const uint32_t difference = nm_force_bits(a-b)&0x7fffffffu;
                        if (difference > nm_force_bits(i < 2 ? 3.5f : 2.5f)) {
                            next.action = NM_BOUNDARY_SPIN;
                            break;
                        }
                    }
                }
            }
        }
    }
    *state = next;
    return 1;
}

typedef struct { float point[2]; int hit; } nm_boundary_hit_t;

typedef struct {
    float height, speed, displacement[3];
    int32_t hold_ticks, recovery_ticks;
    int16_t rotation;
} nm_boundary_recovery_t;

/* 161f8..1632c. Caller broadcasts height to the body and all four ground
 * samples, and uses displacement/rotation for held movement. The authored
 * center height (command2c) is distinct from command2b's averaged vertices,
 * which are already folded offline into recovery_center.
 * Its unused angular result is overwritten by the wide signed delta/21.
 * Keep the exact sum/subtract/refined-division order; no target TGP runtime. */
static inline int nm_boundary_recover(const nm_drive_params_t *p,
        const nm_contact_quad_t *quad, const nm_wheels_state_t *wheels,
        nm_boundary_recovery_t *state) {
    if (!p || !quad || !wheels || !state || !isfinite(state->speed) ||
        !isfinite(quad->recovery_height)) return 0;
    nm_boundary_recovery_t next = *state;
    next.speed = 0.5f*state->speed; next.height = quad->recovery_height;
    for (unsigned a = 0; a < 3; ++a) {
        const float position = a == 1 ? next.height : wheels->position[a];
        if (!isfinite(position) || !isfinite(quad->recovery_center[a])) return 0;
        next.displacement[a] = nm_force_divide(p,quad->recovery_center[a]-position,21.0f);
        if (!isfinite(next.displacement[a])) return 0;
    }
    next.hold_ticks = next.recovery_ticks = 21;
    next.rotation = (int16_t)(((int)wheels->road_heading-wheels->yaw)/21);
    *state = next;
    return 1;
}

/* 167b4..16ba8: intersection used by the surface/pivot response. Points are
 * A.x,A.z,B.x,B.z,C.x,C.z,D.x,D.z. Endpoint acceptance depends on direction;
 * parallel (including collinear) segments miss. Preserve partially updated candidate
 * coordinates on a miss: the source publishes g8 before its range checks.
 * Miss candidates may be nonfinite and must not be used as positions. This
 * is not a generic geometric predicate: raw +0 branches, bit-order
 * range checks and table-refined division are observable source behavior. */
static inline int nm_boundary_intersection(const nm_drive_params_t *p,
        const float points[8], nm_boundary_hit_t *state) {
    if (!p || !points || !state) return 0;
    for (unsigned i = 0; i < 8; ++i) if (!isfinite(points[i])) return 0;
    const float ax = points[0], az = points[1], bx = points[2], bz = points[3];
    const float cx = points[4], cz = points[5], dx = points[6], dz = points[7];
    const float ab = bx-ax, cd = dx-cx;
    nm_boundary_hit_t result = *state;
    result.hit = 0;
    if (nm_force_bits(ab) == 0) {
        if (nm_force_bits(cd) != 0) {
            const float slope = nm_force_divide(p,dz-cz,cd);
            result.point[0] = ax;
            result.point[1] = ((ax-cx)*slope)+cz;
            const float z = result.point[1];
            result.hit = (z >= az ? bz >= z : z >= bz) && (ax >= cx ? dx >= ax : ax >= dx);
        }
    } else if (nm_force_bits(cd) == 0) {
        const float slope = nm_force_divide(p,bz-az,ab);
        result.point[0] = cx;
        result.point[1] = ((cx-ax)*slope)+az;
        const float z = result.point[1];
        result.hit = (cx >= ax ? bx >= cx : cx >= bx) && (z >= cz ? dz >= z : z >= dz);
    } else {
        const float first = nm_force_divide(p,bz-az,ab);
        const float second = nm_force_divide(p,dz-cz,cd);
        const float difference = first-second;
        if (nm_force_bits(difference) != 0) {
            const float product = cx*second;
            float numerator = first*ax;
            numerator += cz; numerator -= az; numerator -= product;
            result.point[0] = nm_force_divide(p,numerator,difference);
            const uint32_t x = nm_force_bits(result.point[0]);
            int inside = 1;
            for (unsigned segment = 0; segment < 2; ++segment) {
                const uint32_t a = nm_force_bits(points[4*segment]);
                const uint32_t b = nm_force_bits(points[4*segment+2]);
                if (x & 0x80000000u) inside &= a < x ? b >= x : x >= b;
                else {
                    int32_t sa, sb, sx;
                    nm_memcpy(&sa,&a,4); nm_memcpy(&sb,&b,4); nm_memcpy(&sx,&x,4);
                    inside &= sx < sa ? sx >= sb : sb >= sx;
                }
            }
            if (inside) {
                result.point[1] = ((result.point[0]-ax)*first)+az;
                result.hit = 1;
            }
        }
    }
    if (result.hit && (!isfinite(result.point[0]) || !isfinite(result.point[1]))) return 0;
    *state = result;
    return 1;
}

typedef struct {
    float position[3], speed, impact_speed;
    int16_t yaw, rotation, signal;
    uint8_t side, override_count;
    int8_t condition, shake_ticks;
    uint8_t sound_request;
} nm_boundary_pivot_t;

/* 1638c..167b0, after16060 chooses a boundary (not recovery/spin setup).
 * sides[] and speed_divisor are authored parameters supplied by the caller,
 * never ROM pointers. sound_request is an event only: the app remains silent.
 * Ordered edges and even the source's degenerate-edge register overwrite are
 * retained. Caller owns the surrounding contact/recovery/damage branches. */
static inline int nm_boundary_pivot(const nm_drive_params_t *p,
        const nm_contact_quad_t *quad, uint32_t selected,
        const nm_wheels_state_t *wheels, const uint8_t sides[4],
        uint32_t speed_index, uint32_t speed_divisor, nm_boundary_pivot_t *state) {
    if (!p || !quad || !wheels || !sides || !state) return 0;
    nm_boundary_pivot_t result = *state;
    result.sound_request = 0;
    result.side = selected == wheels->surface[0] ? 0 : selected == wheels->surface[1] ? 3 :
                  selected == wheels->surface[3] ? 2 : 1;
    const unsigned wheel = result.side == 0 ? 2 : result.side == 1 ? 0 : result.side == 2 ? 1 : 3;
    const float pivot[2] = {wheels->world[wheel][0],wheels->world[wheel][2]};
    float edge[4][2];
    unsigned order[4];
    if (result.side < 2) {
        order[0] = 1; order[1] = 2; order[2] = 0; order[3] = 3;
    } else if (nm_force_bits(quad->xyz[2][0]) == nm_force_bits(quad->xyz[3][0]) &&
               nm_force_bits(quad->xyz[2][2]) == nm_force_bits(quad->xyz[3][2])) {
        order[0] = 1; order[1] = 0; order[2] = 2; order[3] = 2;
    } else {
        order[0] = 0; order[1] = 3; order[2] = 1; order[3] = 2;
    }
    for (unsigned i = 0; i < 4; ++i) {
        edge[i][0] = quad->xyz[order[i]][0]; edge[i][1] = quad->xyz[order[i]][2];
    }
    //164bc/164c0 overwrite r6 before copying it to r7, so the last pair is X/X.
    if (result.side >= 2 && order[2] == order[3]) edge[3][1] = edge[3][0];
    result.impact_speed = result.speed;
    const unsigned first = sides[result.side] ? 2 : 0;
    float points[8] = {wheels->world[first][0],wheels->world[first][2],
        wheels->world[first+1][0],wheels->world[first+1][2],0,0,0,0};
    const unsigned pairs[4][2] = {{0,1},{0,2},{3,2},{3,1}};
    nm_boundary_hit_t hit = {{pivot[0],pivot[1]},0};
    for (unsigned i = 0; i < 4; ++i) {
        nm_memcpy(points+4,edge[pairs[i][0]],8); nm_memcpy(points+6,edge[pairs[i][1]],8);
        if (!nm_boundary_intersection(p,points,&hit)) return 0;
        if (hit.hit) break;
    }
    if (hit.hit) {
        const float z = sides[result.side] ? pivot[1]-hit.point[1] : hit.point[1]-pivot[1];
        const float x = sides[result.side] ? hit.point[0]-pivot[0] : pivot[0]-hit.point[0];
        const int32_t angle = nm_force_angle(p,z,x);
        if (angle == INT32_MIN) return 0;
        const int rotation = angle-result.yaw;
        const float c = nm_drive_sine(p,(uint16_t)(rotation+16384));
        const float s = nm_drive_sine(p,(uint16_t)rotation);
        const float relative[3] = {result.position[0]-pivot[0],result.position[1],result.position[2]-pivot[1]};
        const float identity[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
        for (unsigned a = 0; a < 3; ++a) {
            float value = 0.0f + (c*identity[0][a]+s*identity[2][a])*relative[0];
            value += identity[1][a]*relative[1];
            value += (c*identity[2][a]-s*identity[0][a])*relative[2];
            if (a != 1) value = pivot[a/2]+value;
            if (!isfinite(value)) return 0;
            result.position[a] = value;
        }
        const int delta = nm_yaw_angle((uint32_t)((int)result.rotation-rotation));
        result.rotation = (int16_t)(delta < -3072 ? -3072 : delta > 3072 ? 3072 : delta);
        result.yaw = (int16_t)angle;
        if (result.condition) result.signal = 31;
        else {
            if (!speed_divisor) return 0;
            result.override_count = (uint8_t)(speed_index/speed_divisor);
            result.condition = 16;
            int32_t signed_speed; nm_memcpy(&signed_speed,&speed_index,4);
            if (sides[result.side] && signed_speed >= 31) {
                result.speed = result.speed*0.8f;
                result.sound_request = 1;
                if (!result.shake_ticks) result.shake_ticks = 24;
            }
        }
    }
    if (!isfinite(result.speed) || !isfinite(result.impact_speed)) return 0;
    *state = result;
    return 1;
}
/* Native course adapter: no ROM lookup, and lap is deliberately unsigned.
 * A zero divisor is rejected only if the underlying branch consumes it. */
static inline int nm_boundary_pivot_course(const nm_drive_params_t *p,
        const nm_contact_pack_t *pack, uint32_t selected, const nm_wheels_state_t *wheels,
        uint32_t speed_index, uint8_t lap, nm_boundary_pivot_t *state) {
    if (!p || !pack || pack->slot >= 4 || !pack->quads || selected >= pack->quad_count) return 0;
    return nm_boundary_pivot(p,pack->quads+selected,selected,wheels,p->pivot_sides,
                            speed_index,p->pivot_divisor[pack->slot][lap],state);
}

typedef struct {
    nm_wheels_state_t ground;
    nm_damage_state_t damage;
    float speed, impact_speed, displacement[3];
    int32_t hold_ticks, recovery_ticks;
    int16_t heading, rotation, recovery_rotation, pivot_signal;
    uint16_t center_mask;
    int8_t condition;
    uint8_t action, pivot_sound;
} nm_boundary_state_t;
typedef struct {
    int32_t speed_index;
    uint32_t global_flags;
    uint8_t lap, curve, entity_id;
} nm_boundary_input_t;

/* Complete16060: select, then exactly one recovery/pivot/spin response.
 * Heights, hold/recovery timers and damage's shared fields must be published
 * to the shared game state between cars. No per-car copies of those globals.
 * Event bytes are cleared per call; publish nothing on any rejected branch. */
static inline int nm_boundary_step(const nm_contact_pack_t *pack, const nm_drive_params_t *p,
        const nm_boundary_input_t *in, nm_boundary_state_t *state) {
    if (!pack || !p || !in || !state) return 0;
    nm_boundary_state_t s; NM_COPY(&s,state);
    s.damage.auxiliary_request = s.damage.sound_requests = s.pivot_sound = 0;
    nm_boundary_selection_t selected = {s.ground.position[1],0,s.center_mask,NM_BOUNDARY_NONE};
    if (!nm_boundary_select(pack,p,&s.ground,s.heading,&selected)) return 0;
    s.ground.position[1] = selected.height; s.center_mask = selected.center_mask; s.action = selected.action;
    if (s.action == NM_BOUNDARY_RECOVER) {
        nm_boundary_recovery_t r; nm_memset(&r,0,sizeof(r)); r.speed = s.speed;
        if (s.ground.road >= pack->quad_count || !pack->quads ||
            !nm_boundary_recover(p,pack->quads+s.ground.road,&s.ground,&r)) return 0;
        s.ground.position[1] = r.height; s.speed = r.speed;
        for (unsigned v = 0; v < 4; ++v) s.ground.height[v] = r.height;
        nm_memcpy(s.displacement,r.displacement,sizeof(s.displacement));
        s.hold_ticks = r.hold_ticks; s.recovery_ticks = r.recovery_ticks; s.recovery_rotation = r.rotation;
    } else if (s.action == NM_BOUNDARY_PIVOT) {
        nm_boundary_pivot_t v; nm_memset(&v,0,sizeof(v));
        nm_memcpy(v.position,s.ground.position,sizeof(v.position));
        v.speed = s.speed; v.impact_speed = s.impact_speed; v.yaw = s.ground.yaw;
        v.rotation = s.rotation; v.signal = s.pivot_signal; v.side = s.damage.side;
        v.override_count = s.damage.override_count; v.condition = s.condition; v.shake_ticks = s.damage.shake_ticks;
        if (!nm_boundary_pivot_course(p,pack,selected.selected,&s.ground,(uint32_t)in->speed_index,in->lap,&v)) return 0;
        nm_memcpy(s.ground.position,v.position,sizeof(v.position));
        s.speed = v.speed; s.impact_speed = v.impact_speed; s.ground.yaw = v.yaw;
        s.rotation = v.rotation; s.pivot_signal = v.signal; s.damage.side = v.side;
        s.damage.override_count = v.override_count; s.condition = v.condition; s.damage.shake_ticks = v.shake_ticks;
        s.pivot_sound = v.sound_request;
    } else if (s.action == NM_BOUNDARY_SPIN) {
        nm_damage_input_t v; nm_memset(&v,0,sizeof(v));
        v.selected = selected.selected; nm_memcpy(v.wheel_surface,s.ground.surface,sizeof(v.wheel_surface));
        v.global_flags = in->global_flags; v.speed_index = in->speed_index; v.heading = s.heading;
        v.curve = in->curve; v.entity_id = in->entity_id;
        if (!nm_boundary_spin(p,pack,&v,&s.damage)) return 0;
    }
    NM_COPY(state,&s);
    return 1;
}
#endif
