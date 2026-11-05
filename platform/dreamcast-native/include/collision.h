#ifndef DAYTONA_NATIVE_COLLISION_H
#define DAYTONA_NATIVE_COLLISION_H
#include "force_math.h"
#include "road.h"
#include "yaw.h"
#include "damage.h"
#include "wheels.h"
#include "nm_mem.h"
#ifdef __DREAMCAST__
#include <sh4zam/shz_sh4zam.h>
#endif

typedef struct {
    uint32_t flags;
    float position[3], world[4][3];
    int16_t heading, roll, inhibit_ticks;
    uint8_t side;
} nm_collision_shape_t;
typedef struct {
    uint32_t flags, quarter, angle[2], side[2], corner[2], countdown;
    float depth[2];
} nm_collision_contact_t;

/*14180..14630, including1469c/14794: prepare cached wheel corners and keep
 * the FIRST contained vertex in each direction, in storage order0,1,2,3.
 * The tested perimeter is0,1,3,2. This is not a generic overlap/SAT solver.
 * flags bit5 caches world[] across pair responses.13f08 clears it only in
 * the reset pool; a primary outside that pool retains its flags here.
 * A pair response moving position must NOT invalidate that cache mid-pass.
 * Preserve unselected record fields and second.side; publish atomically.
 * second_primary is pointer identity in the caller, NOT byte entity_id. */
static inline int nm_collision_detect(const nm_drive_params_t *p,
        nm_collision_shape_t *first, nm_collision_shape_t *second,
        int second_primary, nm_collision_contact_t *contact, uint16_t *signal) {
    if (!p || !first || !second || !contact || !signal) return 0;
    unsigned map = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (p->collision_corner[i] >= 4) return 0;
        map |= 1u<<p->collision_corner[i];
    }
    if (map != 15) return 0;
    nm_collision_shape_t a = *first, b = *second;
    nm_collision_shape_t *body[2] = {&a, first == second ? &a : &b};
    nm_collision_contact_t out = *contact; out.flags = 0;
    uint16_t next_signal = *signal;
    for (unsigned i = 0; i < 2; ++i) if (!(body[i]->flags&32)) {
        if (!nm_wheels_transform(p,body[i]->position,body[i]->heading,body[i]->roll,body[i]->world)) return 0;
        body[i]->flags |= 32;
    }
    for (unsigned direction = 0; direction < 2; ++direction) {
        const nm_collision_shape_t *point = body[direction], *quad = body[1-direction];
        float perimeter[4][3];
        for (unsigned v = 0; v < 4; ++v) nm_memcpy(perimeter[v],quad->world[v^(v>>1)],sizeof(perimeter[v]));
        for (unsigned v = 0; v < 4; ++v) {
            const int hit = nm_contact_contains(perimeter,point->world[v][0],point->world[v][2]);
            if (hit < 0) return 0;
            if (!hit) continue;
            unsigned corner = 0;
            while (p->collision_corner[corner] != v) ++corner;
            out.corner[direction] = corner;
            uint32_t quarter;
            if (!direction) {
                body[1]->side = 255; out.flags = 1;
                out.quarter = (uint32_t)(a.heading-body[1]->heading)>>14;
                quarter = out.quarter;
            } else {
                if (second_primary && !body[1]->inhibit_ticks) next_signal = p->collision_signal[v];
                out.countdown = 4-v; body[1]->side = (uint8_t)corner;
                quarter = out.flags == 1 ? 3u-out.quarter : (uint32_t)(body[1]->heading-a.heading-1)>>14;
                out.flags |= 2;
            }
            const unsigned side = (corner+quarter+2u)&3u;
            const uint32_t angle = (((side+1u)&3u)<<14)+(uint32_t)(int32_t)quad->heading;
            out.side[direction] = side; out.angle[direction] = angle;
            const float dx = point->world[v][0]-quad->world[p->collision_corner[side]][0];
            const float dz = point->world[v][2]-quad->world[p->collision_corner[side]][2];
            out.depth[direction] = nm_drive_sine(p,(uint16_t)angle)*dx -
                nm_drive_sine(p,(uint16_t)(angle+16384u))*dz;
            if (!isfinite(out.depth[direction])) return 0;
            break;
        }
    }
    *first = a; if (first != second) *second = b;
    *contact = out; *signal = next_signal;
    return 1;
}

typedef struct { float position[2]; uint8_t side; } nm_collision_body_t;
typedef struct { float previous[2], travel[2]; } nm_collision_travel_t;

typedef struct { float position[2]; uint32_t word5c; } nm_collision_probe_t;

/*140a4..14138: commands40/41 followed by the raw5c-word difference gate.
 * point is current g8 before the player swap. circle.position is g9's SNAPSHOT
 * at140a4, held across inner-loop responses even if g9 moves. Both word5c
 * inputs are current. Reuse the table-refined length,
 * not squared-distance comparison or sqrtf: rounding changes boundary hits.
 * Radius40a54413 and limit3fd9999a are immediate operands in the source.
 * Test the clearance SIGN BIT, including negative zero. Only consume5c
 * on a distance hit; its meaning is caller-dependent, not assumed world Y.
 * This does not traverse pairs, detect quad contact, or charge the work budget. */
static inline int nm_collision_candidate(const nm_drive_params_t *p,
        const nm_collision_probe_t *point, const nm_collision_probe_t *circle,
        unsigned *candidate) {
    if (!p || !point || !circle || !candidate) return 0;
    const float dx = circle->position[0]-point->position[0];
    const float dz = circle->position[1]-point->position[1];
    if (!isfinite(dx) || !isfinite(dz)) return 0;
    const float clearance = nm_force_length(p,dx,dz)-nm_force_float(0x40a54413u);
    if (!isfinite(clearance)) return 0;
    unsigned hit = nm_force_bits(clearance)>>31;
    if (hit) {
        const float difference = nm_force_float(circle->word5c)-nm_force_float(point->word5c);
        hit = (nm_force_bits(difference)&0x7fffffffu) <= 0x3fd9999au;
    }
    *candidate = hit;
    return 1;
}

/*13f90..13fdc, one body: measure current1c/24 minus saved28c/290 BEFORE
 * replacing that saved position. This is not the14-vehicle list traversal.
 * Do not substitute the physics9c/a4 deltas. Publish both axes atomically. */
static inline int nm_collision_track(const float position[2], nm_collision_travel_t *state) {
    if (!position || !state) return 0;
    nm_collision_travel_t next = *state;
#ifdef __DREAMCAST__
    const shz_vec2_t delta=shz_vec2_sub(shz_vec2_init(position[0],position[1]),
        shz_vec2_init(state->previous[0],state->previous[1]));
    next.travel[0]=delta.x;next.travel[1]=delta.y;
#endif
    for (unsigned axis = 0; axis < 2; ++axis) {
#ifndef __DREAMCAST__
        next.travel[axis] = position[axis]-state->previous[axis];
#endif
        if (!isfinite(next.travel[axis])) return 0;
        next.previous[axis] = position[axis];
    }
    *state = next; return 1;
}

enum { NM_COLLISION_WINDOW = 14 };
typedef struct {
    int32_t control, event[2];
    int16_t short_ticks;
} nm_race_timers_t;

/*12150: signed saturating decrements, preserving the source16/32-bit widths.
 * Negative values clear, including INT_MIN; never decrement them in C. */
static inline int nm_race_timers_step(nm_race_timers_t *s) {
    if (!s) return 0;
    s->short_ticks = s->short_ticks > 0 ? s->short_ticks-1 : 0;
    s->control = s->control > 0 ? s->control-1 : 0;
    for (unsigned i = 0; i < 2; ++i) s->event[i] = s->event[i] > 0 ? s->event[i]-1 : 0;
    return 1;
}

typedef struct {
    uint32_t progress;
    float position[2];
    int16_t lateral; /* Signed13e, shared with the road/collision correction. */
    uint16_t next, previous;
} nm_traffic_pass_body_t;
typedef struct { uint16_t next, previous, signal; } nm_traffic_pass_t;

/*125b0..127fc: detect reversal of either cached player neighbor and update
 * shared signal501a04. First (previous == saved next) branch has precedence.
 * Always refresh both cached identities; otherwise preserve the signal.
 * Wrapped abs(INT_MIN) stays negative for signed40/80/120 comparisons.
 * This publishes an intent only; no sound playback or board runtime. */
static inline int nm_traffic_pass(const nm_drive_params_t *p, const nm_traffic_pass_body_t *body,
        unsigned count, unsigned player, nm_traffic_pass_t *state) {
    if (!p || !body || !state || !count || count > UINT16_MAX || player >= count) return 0;
    const nm_traffic_pass_body_t *car = &body[player];
    if (car->next >= count || car->previous >= count) return 0;
    nm_traffic_pass_t out = *state;
    const int first = car->previous == state->next;
    if (first || car->next == state->previous) {
        const nm_traffic_pass_body_t *other = &body[first ? car->previous : car->next];
        const float dx = other->position[0]-car->position[0], dz = other->position[1]-car->position[1];
        if (!isfinite(dx) || !isfinite(dz)) return 0;
        const float distance = nm_force_length(p,dx,dz);
        if (!isfinite(distance)) return 0;
        if ((nm_force_bits(distance)&0x7fffffffu) <= 0x41700000u) {
            uint32_t raw = other->progress-car->progress;
            if (raw>>31) raw = 0u-raw;
            int32_t gap; nm_memcpy(&gap,&raw,4);
            if (first) {
                if (car->lateral < other->lateral) out.signal = gap <= 80 ? 13 : 12;
                else out.signal = gap <= 40 ? 4 : gap <= 80 ? 8 : gap <= 120 ? 7 : 3;
            } else {
                if (car->lateral < other->lateral) out.signal = gap <= 40 ? 14 : gap <= 80 ? 15 : 16;
                else out.signal = gap <= 40 ? 9 : gap <= 80 ? 10 : 11;
            }
        }
    }
    out.next = car->next; out.previous = car->previous; *state = out;
    return 1;
}

typedef struct {
    uint32_t flags, marker146, progress;
    float position[2];
    uint16_t next, previous;
    uint8_t hold;
} nm_traffic_near_body_t;

/*12464..12588 and139d8: prefix reset, eight neighbors each direction around
 * player, plus the caller SUBJECT's progress check (g13, not necessarily the
 * player). marker146 preserves all32 raw bits: reset clears onlyLOW16, while
 * selected-neighbor stores replace all32 (including the low gear halfword).
 * Follow links and preserve repeated-index write order. Preflight bounded
 * math/links before publication.139d8's eight identical checks can be folded
 * into one because no input to that check changes during this routine.
 * ahead_progress is explicit: a race worker is not a car; its88 field can
 * alias the collision window. A null link reads immutable ROM zeros. */
static inline int nm_traffic_near_neighbors(const nm_drive_params_t *p, nm_traffic_near_body_t *body,
        unsigned count, unsigned reset_count, unsigned player, uint8_t *profile_updates) {
    if (!p || !body || !profile_updates || !count || count > UINT16_MAX ||
        !reset_count || reset_count > count || player >= count) return 0;
    uint16_t selected[8][2]; uint32_t masks[8][2];
    unsigned at[2] = {player,player};
    for (unsigned i = 0; i < 8; ++i) for (unsigned side = 0; side < 2; ++side) {
        at[side] = side ? body[at[side]].previous : body[at[side]].next;
        if (at[side] >= count) return 0;
        selected[i][side] = (uint16_t)at[side];
        const float dx = body[at[side]].position[0]-body[player].position[0];
        const float dz = body[at[side]].position[1]-body[player].position[1];
        if (!isfinite(dx) || !isfinite(dz)) return 0;
        const float distance = nm_force_length(p,dx,dz);
        if (!isfinite(distance)) return 0;
        const uint32_t bits = nm_force_bits(distance);
        const int near = (bits>>31) || bits <= 0x42000000u;
        masks[i][side] = near ? (1u<<10) : 0;
        if (near && !side && ((bits>>31) || bits <= 0x41000000u)) masks[i][side] |= 1u<<14;
        if (near && side && i < 2) masks[i][side] |= 1u<<13;
    }
    *profile_updates = 0;
    for (unsigned i = 0; i < reset_count; ++i) {
        body[i].marker146 &= 0xffff0000u;
        body[i].flags &= ~((1u<<10)|(1u<<14)|(1u<<13));
    }
    for (unsigned i = 0; i < 8; ++i) {
        body[selected[i][0]].marker146 = body[selected[i][1]].marker146 = 8-i;
        body[selected[i][0]].flags |= masks[i][0]; body[selected[i][1]].flags |= masks[i][1];
    }
    return 1;
}

/* Keep139d8's subject effect separate: the native scene has cars, not a guest
 * scheduler object. Existing full-routine callers retain exact subject/alias
 * behavior and atomic failure through this wrapper. */
static inline int nm_traffic_near(const nm_drive_params_t *p, nm_traffic_near_body_t *body,
        unsigned count, unsigned reset_count, unsigned player, unsigned subject,
        const uint32_t ahead_progress[2], uint8_t *profile_updates) {
    if (!p || !body || !ahead_progress || !profile_updates || !count || count > UINT16_MAX ||
        !reset_count || reset_count > count || player >= count || subject >= count) return 0;
    const uint32_t raw1 = body[subject].progress-ahead_progress[0];
    const uint32_t raw2 = body[subject].progress-ahead_progress[1];
    int32_t delta1, delta2; nm_memcpy(&delta1,&raw1,4); nm_memcpy(&delta2,&raw2,4);
    if(!nm_traffic_near_neighbors(p,body,count,reset_count,player,profile_updates)) return 0;
    if(delta1>128 || delta2>128) body[subject].hold=4;
    return 1;
}

typedef struct {
    int16_t rank_key; /* Signed source halfword60; not inferred as a score. */
    uint16_t profile, next, previous;
    int8_t lap;
    uint8_t rank;
} nm_traffic_order_body_t;

/*121d4..1221c: stage entry rebuilds both lists in reverse spawn order.
 * Native indices replace the source base+stride pointers. Only the active
 * prefix is written; lap state, body fields and unused tails survive. */
static inline int nm_traffic_order_init(uint16_t rank_order[44],
        uint16_t track_order[44], unsigned count) {
    if (!rank_order || !track_order || !count || count > 44) return 0;
    for (unsigned i = 0; i < count; ++i)
        rank_order[i] = track_order[i] = (uint16_t)(count-1-i);
    return 1;
}

/*1225c..12460, the first two calls of1224c. Two source arrays occupy44 slots
 * each. ONE descending adjacent pass, not a complete sort. A new signed lap
 * rotates a leading group of1..4 instead of sorting the profile list. Rebuild
 * the ring from its first ring_count entries, with next pointing backward in
 * that array. Source reset_count+1 is the ring length, not sort_count by fiat.
 * Preserve source store order for repeated indices (first ring entry LAST).
 * Plan arrays before publication; no heap, board or new immutable data.
 * Valid source configuration has ring_count>=3. Reject undersized wrap tails
 * instead of emulating the source do-while reading beyond the configured list. */
static inline int nm_traffic_order(nm_traffic_order_body_t *body, unsigned count,
        uint16_t rank_order[44], uint16_t track_order[44], unsigned sort_count,
        unsigned ring_count, uint32_t flags, int8_t *last_lap) {
    if (!body || !rank_order || !track_order || !last_lap || !count || count > UINT16_MAX ||
        sort_count < 3 || sort_count > 44 || ring_count < 3 || ring_count > sort_count) return 0;
    uint16_t ranks[44], track[44];
    nm_memcpy(track,track_order,sort_count*sizeof(*track));
    for (unsigned i = 0; i < sort_count; ++i) if (track[i] >= count) return 0;
    if (flags&0x500000u) {
        nm_memcpy(ranks,rank_order,sort_count*sizeof(*ranks));
        for (unsigned i = 0; i < sort_count; ++i) if (ranks[i] >= count) return 0;
        for (unsigned i = 1; i < sort_count; ++i) if (body[ranks[i]].rank_key > body[ranks[i-1]].rank_key) {
            const uint16_t old = ranks[i-1]; ranks[i-1] = ranks[i]; ranks[i] = old;
        }
    }
    if (body[track[0]].lap > *last_lap) {
        unsigned prefix = 1;
        while (prefix < 4) {
            if (prefix >= sort_count) return 0;
            if (body[track[prefix]].lap != body[track[0]].lap || body[track[prefix]].profile >= 8) break;
            ++prefix;
        }
        if (prefix >= sort_count) return 0;
        uint16_t saved[4]; nm_memcpy(saved,track,prefix*sizeof(*track));
        nm_memmove(track,track+prefix,(sort_count-prefix)*sizeof(*track));
        nm_memcpy(track+sort_count-prefix,saved,prefix*sizeof(*track));
    } else for (unsigned i = 1; i < sort_count; ++i) if (body[track[i]].profile > body[track[i-1]].profile) {
        const uint16_t old = track[i-1]; track[i-1] = track[i]; track[i] = old;
    }
    if (flags&0x500000u) {
        nm_memcpy(rank_order,ranks,sort_count*sizeof(*ranks));
        for (unsigned i = 0; i < sort_count; ++i) body[ranks[i]].rank = (uint8_t)i;
    }
    nm_memcpy(track_order,track,sort_count*sizeof(*track));
    for (unsigned i = 1; i < ring_count; ++i) {
        body[track[i]].next = track[i-1]; body[track[i]].previous = track[i+1 == ring_count ? 0 : i+1];
    }
    body[track[0]].next = track[ring_count-1]; body[track[0]].previous = track[1];
    *last_lap = body[track[0]].lap;
    return 1;
}

typedef struct {
    uint32_t flags, relative;
    float position[2];
    nm_collision_travel_t motion;
    uint16_t next, previous, profile, adjusted, candidates;
    uint8_t contact_side;
} nm_collision_rank_t;

/*13f08..14068. Native indices replace source pointers; array prefix[reset_count]
 * is the contiguous source pool. The primary car can be OUTSIDE that prefix.
 * Follow five previous links, fourteen next links, then fourteen previous
 * links. Do not sort by profile or substitute physical array order. Only the
 * low byte of relative is reset outside the selected window. Keep signed
 * initial threshold vs unsigned profile loads, and low16 wrapping after add.
 * Plan bounded writes before publishing, including repeated linked indices.
 * This prepares candidates; it does not execute the pair/work-budget loop. */
static inline int nm_collision_prepare(nm_collision_rank_t *body, unsigned count,
        unsigned reset_count, unsigned primary, uint32_t profile_wrap,
        uint16_t order[NM_COLLISION_WINDOW]) {
    if (!body || !order || !count || count > UINT16_MAX || !reset_count ||
        reset_count > count || primary >= count) return 0;
    unsigned at = primary;
    for (unsigned i = 0; i < 5; ++i) {
        at = body[at].previous; if (at >= count) return 0;
    }
    const int threshold = (int)nm_yaw_angle(body[at].profile)-3;
    uint16_t selected[14], backward[14], adjusted[14], candidates[14];
    nm_collision_travel_t travel[14];
    for (unsigned i = 0; i < 14; ++i) {
        selected[i] = (uint16_t)at; travel[i] = body[at].motion;
        for (unsigned j = 0; j < i; ++j) if (selected[j] == at) travel[i] = travel[j];
        if (!nm_collision_track(body[at].position,&travel[i])) return 0;
        uint32_t profile = body[at].profile;
        if ((int32_t)profile < threshold) profile += profile_wrap;
        adjusted[i] = (uint16_t)profile;
        at = body[at].next; if (i+1 < 14 && at >= count) return 0;
    }
    at = selected[13];
    unsigned current = adjusted[13], same = 0, adjacent = 0;
    for (unsigned i = 0; i < 14; ++i) {
        backward[i] = (uint16_t)at;
        unsigned profile = body[at].adjusted;
        for (unsigned j = 0; j < 14; ++j) if (selected[j] == at) profile = adjusted[j];
        if (profile != current) {
            adjacent = (int)profile == (int)current-1 ? same : 0;
            same = 0; current = profile;
        }
        candidates[i] = (uint16_t)(same+adjacent); ++same;
        at = body[at].previous; if (i+1 < 14 && at >= count) return 0;
    }
    body[primary].contact_side = 255;
    for (unsigned i = 0; i < reset_count; ++i) {
        body[i].flags &= ~((1u<<26)|32u);
        body[i].relative = (body[i].relative&0xffffff00u)|127u;
        body[i].contact_side = 255;
    }
    for (unsigned i = 0; i < 14; ++i) {
        nm_collision_rank_t *b = body+selected[i];
        b->motion = travel[i]; b->relative = i-5u; b->adjusted = adjusted[i];
    }
    for (unsigned i = 0; i < 14; ++i) body[backward[i]].candidates = candidates[i];
    nm_memcpy(order,selected,sizeof(selected)); return 1;
}

typedef struct { float travel[2], world[4][3]; } nm_collision_choice_body_t;
typedef struct { nm_collision_probe_t probe; uint16_t next, candidates; } nm_collision_walk_body_t;
typedef struct {
    float centre[2];
    float circle_radius;
    int32_t budget;
    uint16_t centre_id, current, remaining;
    uint8_t outer, pending;
} nm_collision_walk_t;

/*1406c..1417c bounded native pair iterator, NOT a board/CPU scheduler.
 * Start with {.budget=1000}. Each returned pair must finish detection/response
 * before the next call, passing its150/300 work charge and publishing changed
 * body probe data. Pass0 initially. Return1=pair,0=finished,-1=invalid (atomic).
 * Preserve the centre snapshot across responses, but read both5c words live.
 * Seed centre/circle_radius with the preceding circle owner when its lifetime
 * crosses this pass. Empty groups preserve it; even a group that exhausts the
 * budget before testing a candidate publishes command40's new circle.
 * A hit swaps the primary to second. Five units per nonempty outer group,
 * ten per candidate, and the response charge are source work units, not time.
 * Bodies/order/phase and links stay live/valid for this pass; do not precompute
 * all hit pairs before responses, which can change later broad-phase results. */
static inline int nm_collision_walk_next(const nm_drive_params_t *p,
        const nm_collision_walk_body_t *body, unsigned count, const uint16_t order[14],
        unsigned primary, unsigned phase, unsigned response_cost,
        nm_collision_walk_t *state, uint16_t pair[2]) {
    if (!p || !body || !order || !state || !pair || !count || count > UINT16_MAX ||
        primary >= count || phase >= 4 || state->outer > 14 || state->pending > 1 ||
        state->budget < -310 || state->budget > 1000 || state->budget%5 ||
        (state->pending && (state->budget <= 0 || !state->outer)) ||
        (state->pending ? response_cost != 150 && response_cost != 300 : response_cost != 0)) return -1;
    nm_collision_walk_t s = *state;
    if (s.pending) { s.budget -= (int32_t)response_cost+10; s.pending = 0; }
    for (;;) {
        if (!s.remaining) {
            if (s.outer == 14) { *state = s; return 0; }
            const unsigned slot = p->collision_schedule[phase][s.outer++];
            if (slot >= 14 || order[slot] >= count) return -1;
            s.centre_id = s.current = order[slot];
            s.remaining = body[s.centre_id].candidates;
            if (!s.remaining) continue;
            nm_memcpy(s.centre,body[s.centre_id].probe.position,sizeof(s.centre));
            s.circle_radius=nm_force_float(UINT32_C(0x40a54413));
            s.budget -= 5;
        }
        if (s.budget <= 0) { s.outer = 14; s.remaining = 0; *state = s; return 0; }
        if (s.current >= count || s.centre_id >= count || body[s.current].next >= count) return -1;
        s.current = body[s.current].next;
        nm_collision_probe_t circle = body[s.centre_id].probe;
        nm_memcpy(circle.position,s.centre,sizeof(s.centre));
        unsigned hit;
        if (!nm_collision_candidate(p,&body[s.current].probe,&circle,&hit)) return -1;
        --s.remaining;
        if (hit) {
            pair[s.current == primary ? 1 : 0] = s.current;
            pair[s.current == primary ? 0 : 1] = s.centre_id;
            s.pending = 1; *state = s; return 1;
        }
        s.budget -= 10;
    }
}

typedef struct { float depth[2]; uint32_t corner[2], angle; } nm_collision_choice_input_t;

/*14904..14a20 chooses14a5c(0) or14b58(1), NOT contact detection. first=g8,
 * second=g9. Travel294/298 is current position minus saved28c/290, not the
 * physics deltas9c/a4. Depth0/1
 * are record9c/98; corners0/1 are48/54. Compare magnitude BITS with strict>
 * (equality with distance takes the angular path), then test bit15 of wrapped angle differences.
 * Unused corner indices/points need not be valid on the early-depth branch.
 * Only the branch is observable by the native caller; failures publish nothing. */
static inline int nm_collision_choose(const nm_drive_params_t *p,
        const nm_collision_choice_input_t *in, const nm_collision_choice_body_t *first,
        const nm_collision_choice_body_t *second, unsigned *branch) {
    if (!p || !in || !first || !second || !branch) return 0;
    const float dx = second->travel[0]-first->travel[0], dz = second->travel[1]-first->travel[1];
    if (!isfinite(dx) || !isfinite(dz)) return 0;
    const float distance = nm_force_length(p,0.0f-dx,0.0f-dz);
    if (!isfinite(distance)) return 0;
    int32_t distance_bits; nm_memcpy(&distance_bits,&distance,4);
    const int32_t depth0 = (int32_t)(nm_force_bits(in->depth[0])&0x7fffffffu);
    const int32_t depth1 = (int32_t)(nm_force_bits(in->depth[1])&0x7fffffffu);
    if ((depth0 > depth1 ? depth0 : depth1) > distance_bits) {
        *branch = depth0 > depth1; return 1;
    }
    if (in->corner[0] >= 4 || in->corner[1] >= 4) return 0;
    const unsigned a = p->collision_corner[in->corner[0]], b = p->collision_corner[in->corner[1]];
    if (a >= 4 || b >= 4) return 0;
    const float cosine = first->world[a][2]-second->world[b][2];
    const float sine = second->world[b][0]-first->world[a][0];
    if (!isfinite(cosine) || !isfinite(sine)) return 0;
    const int32_t motion_angle = nm_force_angle(p,dx,dz), edge_angle = nm_force_angle(p,cosine,sine);
    if (motion_angle == INT32_MIN || edge_angle == INT32_MIN) return 0;
    const uint32_t relative = (uint32_t)motion_angle-16384u-(uint32_t)edge_angle;
    *branch = ((relative^(in->angle-(uint32_t)edge_angle))>>15)&1u;
    return 1;
}

enum { NM_COLLISION_FIRST = 0, NM_COLLISION_SECOND = 1, NM_COLLISION_NONE = 2 };
typedef struct {
    nm_collision_choice_input_t contact;
    uint32_t flags; /* record34, not entity flags */
    int16_t heading[2]; /* first/second58 */
} nm_collision_route_input_t;
typedef struct { uint16_t caller_result; uint8_t branch; } nm_collision_route_t;

/*14630..14688 dispatch and eventual g3 return. flags3 bypasses the heading
 * gate and uses14904. Other nonzero flags suppress a wrapped[-64,64] heading
 * difference; exactly2 selects second, all other values first. The caller
 * receives150 without a response,300 AFTER the selected response completes.
 *14164 deducts this from13f08's1000-unit pair-work budget, not a cooldown.
 * Detection and executing that response remain the caller's responsibility. */
static inline int nm_collision_route(const nm_drive_params_t *p,
        const nm_collision_route_input_t *in, const nm_collision_choice_body_t *first,
        const nm_collision_choice_body_t *second, nm_collision_route_t *result) {
    if (!p || !in || !result) return 0;
    unsigned branch = NM_COLLISION_NONE;
    if (in->flags == 3) {
        if (!nm_collision_choose(p,&in->contact,first,second,&branch)) return 0;
    } else if (in->flags && (uint32_t)((int32_t)nm_yaw_angle((uint32_t)(in->heading[0]-in->heading[1]))+64) > 128u)
        branch = in->flags == 2 ? NM_COLLISION_SECOND : NM_COLLISION_FIRST;
    result->caller_result = branch == NM_COLLISION_NONE ? 150 : 300;
    result->branch = (uint8_t)branch;
    return 1;
}

/*14a5c /14b58 pair separation. The selector/caller supplies the actual contact
 *angle, overlap and side; this does NOT detect collision or choose a response.
 *14a5c: plus=g8, minus=g9, fields38/9c/3c.14b58 swaps bodies, fields4c/98/50.
 *Keep half-depth before sine/cosine multiplication, sign-bit inversion and
 *ordered add/subtract (including signed zero). No new table or allocation.
 *Same-body aliases retain source store ordering; failures publish neither body. */
static inline int nm_collision_separate(const nm_drive_params_t *p,
        uint32_t angle, float depth, uint32_t side,
        nm_collision_body_t *plus, nm_collision_body_t *minus) {
    if (!p || !plus || !minus || !isfinite(depth)) return 0;
    nm_collision_body_t a = *plus, b = *minus;
    const float half = 0.5f*depth;
    float offset[2]; nm_force_cartesian(p,(uint16_t)angle,half,offset);
    offset[0] = nm_force_float(nm_force_bits(offset[0])^0x80000000u);
    for (unsigned axis = 0; axis < 2; ++axis) {
        if (!isfinite(a.position[axis]) || !isfinite(b.position[axis])) return 0;
        a.position[axis] = offset[axis]+a.position[axis];
        if (plus == minus) b.position[axis] = a.position[axis];
        b.position[axis] = b.position[axis]-offset[axis];
        if (!isfinite(a.position[axis]) || !isfinite(b.position[axis])) return 0;
    }
    b.side = (uint8_t)side; a.side = (uint8_t)((side-2u)&3u);
    if (plus == minus) { b.side = a.side; *plus = b; }
    else { *plus = a; *minus = b; }
    return 1;
}
typedef struct {
    float position[2];
    int16_t heading, lateral; /*48/13e*/
    uint16_t profile; /*128*/
    int8_t correction_ticks; /*214*/
    uint8_t initial_inhibit; /*20d*/
} nm_collision_reaction_t;

/*15238..1530c: after separation, update lateral positions and authored byte
 * timers. first=g8, second=g9; relative_angle preserves the signed g3 output.
 * This does not implement the subsequent force/damage response.
 * Preserve same-body last-store ordering. Any failure publishes nothing. */
static inline int nm_collision_react(const nm_drive_params_t *math,
        const nm_road_pack_t *road, nm_collision_reaction_t *first,
        nm_collision_reaction_t *second, int16_t *relative_angle) {
    if (!math || !road || !road->profiles || !first || !second || !relative_angle ||
        second->profile >= road->profile_count) return 0;
    nm_collision_reaction_t a = *first, b = *second;
    const float dz = a.position[1]-b.position[1], dx = b.position[0]-a.position[0];
    if (!isfinite(dz) || !isfinite(dx)) return 0;
    const int32_t direction = nm_force_angle(math,dz,dx);
    if (direction == INT32_MIN) return 0;
    const int16_t angle = nm_yaw_angle((uint32_t)(direction-b.heading));
    const int control = road->profiles[b.profile].profile_control;
    unsigned index = 0; /* Owned program word0 is verified zero offline. */
    if (control) {
        if (a.profile >= road->profile_count ||
            !nm_road_measure_lateral(math,&road->profiles[a.profile],a.position[0],a.position[1],&a.lateral) ||
            !nm_road_measure_lateral(math,&road->profiles[b.profile],b.position[0],b.position[1],&b.lateral)) return 0;
        index = (control < 0 ? 1u : 2u)+(b.lateral <= a.lateral ? 2u : 0u);
    }
    b.correction_ticks = (int8_t)((int)math->collision_ticks[0][index][0]-
        (math->collision_ticks[0][index][0] >= 128 ? 256 : 0));
    a.correction_ticks = (int8_t)((int)math->collision_ticks[0][index][1]-
        (math->collision_ticks[0][index][1] >= 128 ? 256 : 0));
    b.initial_inhibit = math->collision_ticks[1][index][0];
    a.initial_inhibit = math->collision_ticks[1][index][1];
    if (first == second) *first = a;
    else { *first = a; *second = b; }
    *relative_angle = angle;
    return 1;
}
typedef struct {
    nm_damage_state_t damage;
    float delta[2]; /*9c/a4*/
    int8_t input_inhibit; /*d4*/
    uint8_t entity_id, contact_side; /*4/27d*/
} nm_collision_damage_body_t;
typedef struct {
    uint32_t global_flags, control; /*501090/501124*/
    uint8_t player_mode, curve; /*player6d/501464*/
} nm_collision_damage_input_t;

/*14fcc..15234, including shared crash initialization. first=g8, second=g9.
 * Shared shake/sequence state and per-call event intents live in SECOND.damage;
 * its other shared fields are preserved. severity is the g4 output consumed
 * by14c54. Same-body aliases preserve ordered stores; failures are atomic.
 * This does not yet update velocities or select/detect a contact. */
static inline int nm_collision_damage(const nm_drive_params_t *p,
        const nm_collision_damage_input_t *in, nm_collision_damage_body_t *first,
        nm_collision_damage_body_t *second, int32_t *severity) {
    if (!p || !in || !first || !second || !severity) return 0;
    nm_collision_damage_body_t a = *first, b = *second;
    nm_collision_damage_body_t *other = first == second ? &a : &b;
    other->damage.auxiliary_request = other->damage.sound_requests = 0;
    const unsigned selector = (other->entity_id != 0)+2u*(in->player_mode != 0);
    const float dx = a.delta[0]-other->delta[0], dz = a.delta[1]-other->delta[1];
    const float magnitude = nm_force_length(p,dx,dz);
    int32_t strength;
    if (!nm_drive_round_i32(p->speed_index_scale*magnitude,&strength)) return 0;
    a.damage.flags &= ~(1u<<26);
    if (other->entity_id || (other->contact_side == 3 && !a.damage.inhibit_ticks && !in->control)) {
        int directional = 0;
        if (strength <= p->collision_threshold[1][selector]) {
            if (strength >= p->collision_threshold[0][selector] && (a.input_inhibit || other->input_inhibit)) {
                if (!other->entity_id && !other->damage.shake_ticks) other->damage.shake_ticks = 16;
                directional = 1;
            }
        } else {
            if (!other->entity_id && !other->damage.shake_ticks) other->damage.shake_ticks = 30;
            a.damage.override_count = 32;
            if (strength >= p->collision_threshold[2][selector]) {
                a.damage.flags |= 1u<<6;
                if (other->entity_id) {
                    other->damage.override_count = 32; other->damage.flags |= 1u<<6;
                } else if (!other->damage.inhibit_ticks &&
                    !nm_crash_begin(p,in->curve,other->entity_id,in->global_flags,&other->damage)) return 0;
            } else directional = 1;
        }
        if (directional) {
            const unsigned side = other->damage.side&3;
            a.damage.flags |= p->collision_flags[0][side];
            other->damage.flags |= p->collision_flags[1][side];
            other->damage.flags &= ~2u;
        }
    }
    *first = a;
    if (first != second) *second = b;
    *severity = strength;
    return 1;
}
typedef struct {
    float speed, delta[2], correction[2]; /*34,9c/a4,218/21c*/
    int16_t yaw, own_yaw, other_yaw, inhibit_ticks; /*58,216,212,c8*/
    int8_t input_inhibit, response_ticks; /*d4,98*/
    uint8_t entity_id, contact_side; /*4,27d*/
} nm_collision_impulse_body_t;
typedef struct { uint16_t signal; int8_t shake_ticks; } nm_collision_impulse_shared_t;
typedef struct { uint32_t flags, contact_flags; int32_t severity; } nm_collision_impulse_input_t;

/*14c54..14e6c. Consume14fcc's signed g4, but retain the unsigned30 comparison
 * and unsigned FLOAT-BIT speed ordering in the longitudinal branch. Side1/3
 * transfers relative speed and installs next-tick correction vectors. Shared
 * state is explicit, no audio playback. Same-body stores retain their order;
 * invalid arithmetic rejects atomically, including shared/player effects. */
static inline int nm_collision_impulse(const nm_drive_params_t *p,
        const nm_collision_impulse_input_t *in, nm_collision_impulse_body_t *first,
        nm_collision_impulse_body_t *second, nm_collision_impulse_shared_t *shared) {
    if (!p || !in || !first || !second || !shared) return 0;
    nm_collision_impulse_body_t a = *first, b = *second;
    nm_collision_impulse_body_t *other = first == second ? &a : &b;
    nm_collision_impulse_shared_t globals = *shared;
    if (!other->entity_id) {
        if (a.input_inhibit < -48 || a.input_inhibit > 48 || (in->flags & 0x18000u)) return 1;
        if (in->severity >= 25) {
            if ((in->contact_flags & 2) && !other->response_ticks) other->response_ticks = 16;
            globals.signal = 1;
            if (!globals.shake_ticks) globals.shake_ticks = 8;
        }
    }
    float correction[2] = {0.0f,0.0f}; /* Source program word0 verified offline. */
    const int lateral = other->contact_side == 1 || other->contact_side == 3;
    if (lateral || (uint32_t)in->severity >= 30) {
        float transfer;
        if (lateral) {
            correction[0] = a.delta[0]-other->delta[0];
            correction[1] = a.delta[1]-other->delta[1];
            const float factor = nm_force_float(other->inhibit_ticks ? 0x3f0ccccdu : 0x3f4ccccdu);
            transfer = factor*(a.speed-other->speed);
        } else {
            const float sign = nm_force_bits(other->speed) < nm_force_bits(a.speed) ? 1.0f : -1.0f;
            transfer = p->collision_speed_step*sign;
        }
        const float second_speed = transfer+other->speed, first_speed = a.speed-transfer;
        if (!isfinite(first_speed) || !isfinite(second_speed) ||
            !isfinite(correction[0]) || !isfinite(correction[1])) return 0;
        other->speed = second_speed; a.speed = first_speed;
    }
    for (unsigned axis = 0; axis < 2; ++axis) {
        other->correction[axis] = correction[axis];
        a.correction[axis] = nm_force_float(nm_force_bits(correction[axis])^0x80000000u);
    }
    a.own_yaw = a.yaw; other->other_yaw = a.yaw;
    a.other_yaw = other->yaw; other->own_yaw = other->yaw;
    *first = a;
    if (first != second) *second = b;
    *shared = globals;
    return 1;
}
#endif
