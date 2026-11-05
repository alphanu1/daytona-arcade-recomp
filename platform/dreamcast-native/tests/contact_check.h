#ifndef DAYTONA_NATIVE_CONTACT_CHECK_H
#define DAYTONA_NATIVE_CONTACT_CHECK_H
#include "../include/contact.h"
#include "../include/wheels.h"
#include "../include/collision.h"
#include "../include/player.h"
#include <stddef.h>
#include <stdalign.h>
#include "../include/race_intents.h"

/* Shared host/SH4 boundary gate. Source-routine parity is separately checked
 * by tools/check_contact.hpp; this does not connect a race dispatcher. */
static inline int nm_race_intents_test(void) {
    nm_race_intent_row_t rows[31]={0};
    rows[1]=(nm_race_intent_row_t){{111,222},-1,0,0};
    rows[30]=(nm_race_intent_row_t){{333,444},2,0,0};
    const uint32_t special[2]={555,666};
    nm_race_intents_t state={{{1,0,0},{1,0,0}},0};
    nm_race_intent_output_t out;
    if(!nm_race_intents_step(rows,31,0,special,256,0,&state,&out) ||
       state.lane[0].pending!=1 || state.lane[0].active!=1 || state.lane[0].timer ||
       state.lane[1].pending || state.lane[1].active!=1 || state.lane[1].timer!=-1 || out.count) return 0;
    state=(nm_race_intents_t){{{1,1,30},{1,1,1}},26};
    if(!nm_race_intents_step(rows,31,1,special,0,-1,&state,&out) || out.count!=4 ||
       out.command[0]!=666 || out.command[1]!=666 || out.command[2]!=666 ||
       out.command[3]!=222 || state.repeats!=27) return 0;
    rows[30].trigger=INT16_MAX;
    for(int timer=INT16_MIN;timer<=INT16_MAX;++timer) {
        state=(nm_race_intents_t){{{0,(int16_t)timer,30},{0,0,0}},0};
        if(!nm_race_intents_step(rows,31,0,special,256,0,&state,&out)) return 0;
        const int expected=timer==INT16_MIN ? INT16_MAX : timer ? timer-1 : 0;
        /* Wide decrement can never equal32767; low16 wrap must not trigger. */
        if(state.lane[0].timer!=expected || state.lane[0].active!=30 || out.count) return 0;
    }
    state.lane[1].pending=-1;
    const nm_race_intents_t saved=state;
    memset(&out,0x5a,sizeof(out));const nm_race_intent_output_t saved_out=out;
    if(nm_race_intents_step(rows,31,0,special,256,0,&state,&out) ||
       memcmp(&state,&saved,sizeof(state)) || memcmp(&out,&saved_out,sizeof(out))) return 0;
    return !nm_race_intents_step(NULL,31,0,special,256,0,&state,&out) &&
        !nm_race_intents_step(rows,0,0,special,256,0,&state,&out) &&
        !nm_race_intents_step(rows,31,2,special,256,0,&state,&out);
}

#define NM_PLAYER_END_CASES 262144u
#define NM_PLAYER_END_HASH 0x2679f69cu
static inline void nm_player_end_test_input(unsigned i, nm_player_t *s) {
    memset(s,0,sizeof(*s));
    const unsigned n = i&65535;
    const uint32_t bits[] = {0,0x80000000u,1,0x80000001u,0x7f800000u,0xff800000u,
        0x7fc12345u,0xff812345u,0x3f800000u,0xc0200000u};
    memcpy(&s->boundary.ground.position[0],&bits[n%10],4);
    s->path_flags = 128; s->previous_path_flags = 65535;
    s->lap.lap = 0; s->checkpoints.total_laps = 3;
    s->body_yaw = nm_yaw_angle(n*197); s->boundary.ground.yaw = nm_yaw_angle((uint32_t)(int32_t)s->body_yaw+n);
    if (i < 65536) {
        s->lap.lap = (uint8_t)n; s->checkpoints.total_laps = (uint8_t)(n>>8);
    } else if (i < 131072) s->held = 1;
    else if (i < 196608) {
        s->boundary.heading = nm_yaw_angle(n);
        s->boundary.ground.road_heading = n&1 ? -30000 : 30000;
    } else {
        const int angles[] = {0xffe,0xfff,0x1000,0x5ffe,0x5fff,0x6000,0x3fff};
        s->boundary.heading = (int16_t)(angles[n%7]-0x3fff);
        const int8_t tick = n%256 < 128 ? (int8_t)(n%256) : (int8_t)((int)(n%256)-256);
        switch ((n/7)%9) {
            case 0: s->path_flags = 0xff7f; break;
            case 1: s->previous_path_flags = s->lap.lap; break;
            case 2: s->boundary.damage.inhibit_ticks = nm_yaw_angle(n); break;
            case 3: s->boundary.condition = tick; break;
            case 4: s->boundary.damage.spin_ticks = (uint8_t)n; break;
            case 5: s->input_inhibit = tick; break;
            case 6: s->response_ticks = tick; break;
            case 7: s->input_inhibit = -1; s->boundary.damage.inhibit_ticks = 1; break;
            default: break;
        }
    }
}
static inline uint32_t nm_player_end_test_hash(void) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_PLAYER_END_CASES; ++i) {
        nm_player_t s; nm_player_end_test_input(i,&s); nm_player_end_step(&s);
        const uint32_t words[] = {(uint16_t)s.body_yaw,s.route_transition,
            nm_force_bits(s.proximity_circle[0]),nm_force_bits(s.proximity_circle[1]),nm_force_bits(s.proximity_circle[2])};
        for (unsigned n = 0; n < sizeof(words)/sizeof(words[0]); ++n) for (unsigned b = 0; b < 4; ++b)
            hash = (hash^((words[n]>>(8*b))&255))*16777619u;
    }
    return hash;
}

#define NM_ROAD_MARKS_CASES 65536u
#define NM_ROAD_MARKS_HASH 0x96708b40u
static inline void nm_road_marks_test_input(unsigned i, nm_road_marks_t *m, nm_player_t *s) {
    const uint32_t counts[] = {1,2,15,16,17,195,249,486,990,4096};
    const int32_t speeds[] = {0,1,127,128,129,-1,INT32_MIN,INT32_MAX};
    const uint32_t timers[] = {0,1,159,160,161,0x80000000u,UINT32_MAX};
    memset(s,0,sizeof(*s)); m->count = counts[(i/8)%10]; m->timer = timers[(i/80)%7];
    s->speed_index = speeds[i%8]; s->phase[0] = nm_yaw_angle(i);
    s->input_inhibit = i&1024 ? -128 : 0;
    s->boundary.damage.inhibit_ticks = i&2048 ? -32768 : 0;
}
static inline uint32_t nm_road_marks_test_hash(void) {
    uint8_t values[4096]; uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_ROAD_MARKS_CASES; ++i) {
        nm_player_t s; nm_road_marks_t m = {values,0,0}; nm_road_marks_test_input(i,&m,&s);
        memset(values,0xa5,m.count);
        if (!nm_road_marks_step(&m,(uint16_t)s.phase[0],s.speed_index,s.input_inhibit,s.boundary.damage.inhibit_ticks)) return 0;
        for (unsigned b = 0; b < 4; ++b) hash = (hash^((m.timer>>(8*b))&255))*16777619u;
        for (unsigned behind = 0; behind <= 16; ++behind)
            hash = (hash^values[((uint32_t)(uint16_t)s.phase[0]+m.count-behind)%m.count])*16777619u;
    }
    return hash;
}

#define NM_TILT_CASES 65536u
#define NM_TILT_HASH 0x319265b9u
static inline uint32_t nm_tilt_test_input(unsigned i, nm_player_t *s) {
    memset(s,0,sizeof(*s));
    const int32_t speeds[] = {INT32_MIN,-32769,-150,-65,-64,-33,-32,-1,0,1,31,32,63,64,127,128,149,150,151,INT32_MAX};
    const uint32_t flags[] = {0,0x200000,0x400000,0x800000,0x1000000,~0x1e00000u};
    s->speed_index = speeds[i%20]; s->surface_phase = (uint8_t)(i*197);
    s->tilt_phase = nm_yaw_angle(i); s->tilt[0] = -1234; s->tilt[1] = 2345;
    s->boundary.ground.combined_mask = (uint16_t)((i/20)%16);
    s->boundary.damage.damage = i&0x1ff; s->boundary.damage.override_count = 231;
    s->force.rpm_boost = i&1024 ? (i&2048 ? UINT32_MAX : 1) : 0;
    if (i < 512) {
        /* Every signed-byte lookup, including zero-speed writes/no phase advance. */
        s->surface_phase = (uint8_t)i; s->speed_index = i < 256 ? 0 : -1;
        s->boundary.ground.combined_mask = 4; return 0x200000;
    }
    return flags[(i/320)%6];
}
static inline uint32_t nm_tilt_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_TILT_CASES; ++i) {
        nm_player_t s; const uint32_t flags = nm_tilt_test_input(i,&s);
        if (!nm_player_tilt_step(p,flags,&s)) return 0;
        const uint32_t words[] = {(uint16_t)s.tilt[0],(uint16_t)s.tilt[1],(uint16_t)s.tilt_phase,
            s.surface_phase,s.boundary.damage.override_count};
        for (unsigned n = 0; n < sizeof(words)/sizeof(words[0]); ++n) for (unsigned b = 0; b < 4; ++b)
            hash = (hash^((words[n]>>(8*b))&255))*16777619u;
    }
    return hash;
}

#define NM_BODY_ANGLES_CASES 65536u
#define NM_BODY_ANGLES_HASH 0x18c387f4u
static inline void nm_body_angles_test_input(unsigned i, nm_player_t *s, int16_t *offset) {
    memset(s,0,sizeof(*s));
    /* First half covers every16-bit filtering/offset input plus quarter-unit
     * bank forces; second half spans conversion limits, NaNs and float exponents. */
    const uint32_t bits[] = {0,0x80000000u,0x3f000000u,0xbf000000u,0x3fc00000u,0xbfc00000u,
        0x4effffffu,0x4f000000u,0xcf000000u,0xcf000001u,0x7f800000u,0xff800000u,
        0x7fc00000u,0xffc00000u,1,0x80000001u,0x7f7fffffu,0xff7fffffu};
    s->force.rear_bank_force = (int)(i%32768)-16384;
    s->force.rear_bank_force *= 0.75f;
    s->force.longitudinal_delta = (int)(i%8192)-4096;
    s->force.longitudinal_delta *= 0.000125f;
    if (i&0x8000) {
        s->force.rear_bank_force = nm_force_float(bits[i%18]);
        s->force.longitudinal_delta = nm_force_float(bits[(i/18)%18]);
    }
    s->filtered_pitch = nm_yaw_angle(i); s->bank = -123;
    s->render_yaw_offset = nm_yaw_angle(i*193); s->render_roll_offset = nm_yaw_angle(i*137);
    s->boundary.ground.pitch = nm_yaw_angle(i*71); s->boundary.ground.roll = nm_yaw_angle(i*73);
    s->boundary.ground.yaw = nm_yaw_angle(i*79);
    s->render_angles[0] = 123; s->render_angles[1] = 456; s->render_angles[2] = -789;
    *offset = nm_yaw_angle(i*257);
}
static inline uint32_t nm_body_angles_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_BODY_ANGLES_CASES; ++i) {
        nm_player_t s; int16_t offset; nm_body_angles_test_input(i,&s,&offset);
        const int accepted = nm_player_body_angles_step(p,offset,&s);
        const uint32_t words[] = {(uint32_t)accepted,(uint16_t)s.filtered_pitch,(uint16_t)s.bank,
            (uint16_t)s.render_angles[0],(uint16_t)s.render_angles[1],(uint16_t)s.render_angles[2]};
        for (unsigned n = 0; n < sizeof(words)/sizeof(words[0]); ++n) for (unsigned b = 0; b < 4; ++b)
            hash = (hash^((words[n]>>(8*b))&255))*16777619u;
    }
    return hash;
}

#define NM_RACE_TIMERS_CASES 65536u
#define NM_TRAFFIC_PASS_CASES 65536u
#define NM_RACE_TIMERS_HASH 0x3d715d7cu
#define NM_TRAFFIC_PASS_HASH 0xe66d34cdu
static inline void nm_race_timers_test_input(unsigned i, nm_race_timers_t *s) {
    const uint32_t limits[] = {0,1,2,127,32767,65535,0x7fffffff,0x80000000,0xffffffff};
    const uint32_t a = limits[i%9], b = limits[(i/9)%9], c = i*93737u;
    memcpy(&s->control,&a,4); memcpy(&s->event[0],&b,4); memcpy(&s->event[1],&c,4);
    s->short_ticks = nm_yaw_angle(i);
}
static inline uint32_t nm_race_timers_test_hash(void) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_RACE_TIMERS_CASES; ++i) {
        nm_race_timers_t s; nm_race_timers_test_input(i,&s);
        if (!nm_race_timers_step(&s)) return 0;
        for (unsigned j = 0; j < 14; ++j) hash = (hash^((const uint8_t *)&s)[j])*16777619u;
    }
    return hash;
}
static inline void nm_traffic_pass_test_input(unsigned i, nm_traffic_pass_body_t body[3], nm_traffic_pass_t *s) {
    memset(body,0,3*sizeof(*body)); body[0].next = 1; body[0].previous = 2;
    if (i%17 == 0) body[0].previous = 1;
    if (i%19 == 0) body[0].next = 0;
    s->next = i&1 ? body[0].previous : UINT16_MAX;
    s->previous = i&2 ? body[0].next : UINT16_MAX;
    s->signal = (uint16_t)(0xa500u+(i&255));
    const uint32_t gaps[] = {0,39,40,41,79,80,81,119,120,121,0x7fffffff,0x80000000,0xffffffff};
    body[0].progress = i*999983u;
    body[0].lateral = (i/4)%3 == 0 ? -32768 : (i/4)%3 == 1 ? 32767 : 0;
    for (unsigned j = 1; j < 3; ++j) {
        const uint32_t gap = gaps[(i/12+j-1)%13];
        body[j].progress = body[0].progress+(i&16 ? gap : 0u-gap);
        const uint32_t distances[] = {0,0x41700000u-1,0x41700000u,0x41700000u+1,0x41800000u};
        body[j].position[0] = nm_force_float(distances[(i/156+j-1)%5]);
        if (i&0x4000) body[j].position[1] = (float)((int)(i%17)-8);
    }
}
static inline uint32_t nm_traffic_pass_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_TRAFFIC_PASS_CASES; ++i) {
        nm_traffic_pass_body_t body[3]; nm_traffic_pass_t s;
        nm_traffic_pass_test_input(i,body,&s);
        if (!nm_traffic_pass(p,body,3,0,&s)) return 0;
        for (unsigned j = 0; j < 6; ++j) hash = (hash^((const uint8_t *)&s)[j])*16777619u;
    }
    return hash;
}

#define NM_TRAFFIC_NEAR_CASES 8192u
#define NM_TRAFFIC_NEAR_HASH 0x8b84943fu
static inline void nm_traffic_near_test_input(unsigned i, nm_traffic_near_body_t body[44],
        unsigned *count, unsigned *reset, unsigned *player, unsigned *subject) {
    const unsigned sizes[] = {1,3,7,14,20,21,41,44};
    *count = sizes[i%8]; *reset = *count > 1 ? *count-1 : 1;
    *player = *count-1; *subject = (i/8)%*count; memset(body,0,44*sizeof(*body));
    for (unsigned j = 0; j < *count; ++j) {
        body[j].next = (uint16_t)((j+1)%*count); body[j].previous = (uint16_t)((j+*count-1)%*count);
        if (i&8) { const uint16_t n = body[j].next; body[j].next = body[j].previous; body[j].previous = n; }
        if (i%11 == 0) body[j].next = body[j].previous = (uint16_t)j;
        body[j].flags = 0xfffffff0u-i*19u-j; body[j].marker146 = 0x12340000u+i*17u+j;
        body[j].hold = (uint8_t)(i+j); body[j].progress = i*999983u+j*129u;
        body[j].position[0] = (float)((int)(j%7)-3)*(float)(i%19);
        body[j].position[1] = (float)((int)(j/7)-3)*(float)((i/19)%7);
        if (i&16) { // One ULP below/at/above the two exact axial thresholds.
            body[j].position[0] = j == *player ? 0 : nm_force_float((i&32 ? 0x42000000u : 0x41000000u)+(i/64)%3-1);
            body[j].position[1] = 0;
        }
    }
    if (i%11) {
        const unsigned a = body[*subject].next, b = body[a].next;
        const uint32_t deltas[] = {127,128,129,0x7fffffff,0x80000000,0xffffffff,0,1};
        body[a].progress = body[*subject].progress-deltas[(i/32)%8];
        body[b].progress = body[*subject].progress-deltas[(i/256)%8];
    }
}
static inline uint32_t nm_traffic_near_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_TRAFFIC_NEAR_CASES; ++i) {
        nm_traffic_near_body_t body[44]; unsigned count, reset, player, subject; uint8_t updates = 99;
        nm_traffic_near_test_input(i,body,&count,&reset,&player,&subject);
        const unsigned a = body[subject].next, b = body[a].next;
        const uint32_t ahead[2] = {body[a].progress,body[b].progress};
        if (!nm_traffic_near(p,body,count,reset,player,subject,ahead,&updates) || updates) return 0;
        for (unsigned j = 0; j < count; ++j) for (unsigned k = 0; k <= offsetof(nm_traffic_near_body_t,hold); ++k)
            hash = (hash^((const uint8_t *)&body[j])[k])*16777619u;
    }
    return hash;
}

#define NM_TRAFFIC_ORDER_CASES 8192u
#define NM_TRAFFIC_ORDER_HASH 0x203b7e22u
static inline void nm_traffic_order_test_input(unsigned i, nm_traffic_order_body_t body[44],
        uint16_t ranks[44], uint16_t track[44], unsigned *count, unsigned *sort_count,
        unsigned *ring_count, uint32_t *flags, int8_t *lap) {
    const unsigned sizes[] = {5,7,14,20,21,41,43,44};
    const uint32_t masks[] = {0,0x100000,0x400000,0x500000,UINT32_MAX,0x8000};
    *count = sizes[i%8]; *sort_count = *count-(i/8)%2; *ring_count = *sort_count-(i/16)%2;
    *flags = masks[(i/32)%6]; memset(body,0,44*sizeof(*body));
    for (unsigned j = 0; j < 44; ++j) {
        ranks[j] = track[j] = UINT16_MAX;
        if (j >= *count) continue;
        body[j].rank_key = nm_yaw_angle(i*937u+j*17891u);
        body[j].profile = (uint16_t)(i*7u+j*271u);
        if (i%11 == 0) { body[j].rank_key = -32768; body[j].profile = 65535; }
        body[j].next = (uint16_t)((j+1)%*count); body[j].previous = (uint16_t)((j+*count-1)%*count);
        body[j].lap = (int8_t)((int)((i+j)%256)-128); body[j].rank = 0xa5;
    }
    for (unsigned j = 0; j < *sort_count; ++j) {
        ranks[j] = (uint16_t)((j+i)%*count); track[j] = (uint16_t)((*count-1-j+i)%*count);
        if (i%13 == 0 && j) ranks[j] = track[j] = track[0]; // Repeated aliases retain source store order.
    }
    *lap = body[track[0]].lap;
    if (i&64) {
        const int8_t first_lap = i&128 ? 127 : 0; *lap = (int8_t)(first_lap-1);
        const unsigned prefix = 1+(i/256)%4;
        for (unsigned j = 0; j < prefix; ++j) { body[track[j]].lap = first_lap; body[track[j]].profile = 7; }
        if (prefix < *sort_count && track[prefix] != track[0]) body[track[prefix]].lap = *lap;
        // Source rotation with no tail is not a valid configured operation.
        if (*sort_count == 4) *lap = first_lap;
    }
}
static inline uint32_t nm_traffic_order_test_hash(void) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_TRAFFIC_ORDER_CASES; ++i) {
        nm_traffic_order_body_t body[44]; uint16_t ranks[44], track[44];
        unsigned count, sort_count, ring_count; uint32_t flags; int8_t lap;
        nm_traffic_order_test_input(i,body,ranks,track,&count,&sort_count,&ring_count,&flags,&lap);
        if (!nm_traffic_order(body,count,ranks,track,sort_count,ring_count,flags,&lap)) return 0;
        for (unsigned j = 0; j < sizeof(body); ++j) hash = (hash^((const uint8_t *)body)[j])*16777619u;
        for (unsigned j = 0; j < 44; ++j) { hash = (hash^ranks[j])*16777619u; hash = (hash^track[j])*16777619u; }
        hash = (hash^(uint8_t)lap)*16777619u;
    }
    return hash;
}

#define NM_COLLISION_PREPARE_CASES 8192u
#define NM_COLLISION_PREPARE_HASH 0x91a31cc8u
static inline void nm_collision_prepare_test_input(unsigned i, nm_collision_rank_t body[64],
        unsigned *count, unsigned *reset, unsigned *primary, uint32_t *wrap) {
    const unsigned sizes[] = {1,7,14,20,40,44,31,64};
    const uint32_t wraps[] = {0,1,196,991,65535,65536,UINT32_MAX,0x80000000u};
    *count = sizes[i%8]; *reset = i&8 && *count > 1 ? *count-1 : *count;
    *primary = i&8 ? *count-1 : (i/16)%*count; *wrap = wraps[(i>>4)&7];
    memset(body,0,64*sizeof(*body));
    for (unsigned j = 0; j < *count; ++j) {
        nm_collision_rank_t *b = body+j;
        b->next = (uint16_t)((j+1)%*count); b->previous = (uint16_t)((j+*count-1)%*count);
        if (i&128) { const uint16_t n = b->next; b->next = b->previous; b->previous = n; }
        if (i%11 == 0) b->next = b->previous = (uint16_t)j; // Repeated linked indices, not fourteen distinct cars.
        b->flags = 0xffffffffu-i*17u-j; b->relative = 0x81923400u+i+j;
        b->contact_side = (uint8_t)(i+j); b->adjusted = (uint16_t)(i+j*17); b->candidates = 0xfefe;
        const uint16_t base = (uint16_t)(i*17);
        b->profile = (uint16_t)(base+((i>>8)%4 == 0 ? 0 : (i>>8)%4 == 1 ? j/3 : (i>>8)%4 == 2 ? j : j*7));
        if (i%13 == 0) b->profile = (uint16_t)(0x7ffdu+j%6);
        if (i%17 == 0) b->profile = (uint16_t)(0xfffdu+j%6);
        for (unsigned axis = 0; axis < 2; ++axis) {
            b->position[axis] = (float)((int)(i*11+j*3+axis)-45000)*0.125f;
            b->motion.previous[axis] = b->position[axis]+(float)((int)((i+j+axis)%7)-3)*0.0625f;
            b->motion.travel[axis] = 123;
        }
    }
}
static inline uint32_t nm_collision_prepare_test_hash(void) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_COLLISION_PREPARE_CASES; ++i) {
        nm_collision_rank_t body[64]; unsigned count, reset, primary; uint32_t wrap; uint16_t order[14];
        nm_collision_prepare_test_input(i,body,&count,&reset,&primary,&wrap);
        if (!nm_collision_prepare(body,count,reset,primary,wrap,order)) return 0;
        for (unsigned j = 0; j < count; ++j) for (unsigned k = 0; k < offsetof(nm_collision_rank_t,contact_side)+1; ++k)
            hash = (hash^((const unsigned char *)&body[j])[k])*16777619u;
        for (unsigned j = 0; j < 14; ++j) hash = (hash^order[j])*16777619u;
    }
    return hash;
}

#define NM_COLLISION_DETECT_CASES 65536u
#define NM_COLLISION_DETECT_HASH 0x0f8c0042u
static inline int nm_collision_detect_test_input(const nm_drive_params_t *p, unsigned i,
                                                 nm_collision_shape_t body[2]) {
    memset(body,0,2*sizeof(*body));
    for (unsigned j = 0; j < 2; ++j) {
        body[j].heading = nm_yaw_angle(j ? i*17u+16383u : i);
        body[j].roll = nm_yaw_angle(i%19 ? (i*7+j*13)%2048 : i*101);
        body[j].position[0] = (float)((int)(i%257)-128)*4.0f;
        body[j].position[1] = (float)((int)(i%29)-14)*0.5f;
        body[j].position[2] = (float)((int)(i/257)-128)*4.0f;
        if (j) {
            body[j].position[0] += (float)((int)(i%17)-8)*0.5f;
            body[j].position[2] += (float)((int)((i/17)%17)-8)*0.5f;
        }
        body[j].side = (uint8_t)(i*13+j);
        body[j].inhibit_ticks = i&16 ? -1 : 0;
        body[j].flags = 0x81000400u | ((i&(1u<<j)) ? 32u : 0u);
        if (!nm_wheels_transform(p,body[j].position,body[j].heading,body[j].roll,body[j].world)) return 0;
        // Cached points retain their old pose when current body position moves.
        if (body[j].flags&32) body[j].position[0] += 16;
    }
    return 1;
}
static inline uint32_t nm_collision_detect_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_COLLISION_DETECT_CASES; ++i) {
        nm_collision_shape_t body[2]; nm_collision_contact_t contact;
        memset(&contact,0x5a,sizeof(contact)); uint16_t signal = 0x1234;
        if (!nm_collision_detect_test_input(p,i,body) ||
            !nm_collision_detect(p,&body[0],&body[i%8 != 0],(i&8)!=0,&contact,&signal)) return 0;
        for (unsigned b = 0; b < 2; ++b)
            for (unsigned k = 0; k < offsetof(nm_collision_shape_t,side)+1; ++k)
                hash = (hash^((const unsigned char *)&body[b])[k])*16777619u;
        for (unsigned k = 0; k < sizeof(contact); ++k) hash = (hash^((const unsigned char *)&contact)[k])*16777619u;
        hash = (hash^signal)*16777619u;
    }
    return hash;
}

#define NM_COLLISION_CANDIDATE_CASES 196608u
#define NM_COLLISION_CANDIDATE_HASH 0x6caa7b7au
/* Shared host/SH4 inputs, not recorded game poses. Circle coordinates are the
 * saved outer-loop centre, while both raw5c words are current pair inputs. */
static inline void nm_collision_candidate_test_input(unsigned i, nm_collision_probe_t probe[2]) {
    const uint32_t words[] = {0,0x80000000,0x3fd99999,0x3fd9999a,0x3fd9999b,
        0xbfd99999,0xbfd9999a,0xbfd9999b,0x7f800000,0xff800000,0x7fc12345,
        0xffc12345,0x00234567,0x00800000,0x007fffff,0x3f800000};
    memset(probe,0,2*sizeof(*probe));
    if (i < 65536) {
        probe[0].position[0] = (float)((int)(i%257)-128)*0.0625f;
        probe[0].position[1] = (float)((int)(i/257)-128)*0.0625f;
    } else if (i < 131072) {
        /* Both edges/axes, translations and one-ULP boundary perturbations. */
        const unsigned j = i-65536, axis = (j>>3)&1;
        const float centre = (float)((int)(j/16)-2048)*0.125f;
        const float radius = nm_force_float(0x40a54413);
        float edge = centre + (j&4 ? -radius : radius);
        if ((j&3) == 1) edge = nextafterf(edge,-INFINITY);
        if ((j&3) == 2) edge = nextafterf(edge,INFINITY);
        probe[1].position[axis] = centre; probe[0].position[axis] = edge;
    } else {
        /* Small/subnormal components and a broad inverse-sqrt table domain. */
        const unsigned j = i-131072;
        probe[0].position[0] = nm_force_float((j&1 ? 0x80000000u : 0u) | ((j*977u)&0x7fffffu) | ((j%140)<<23));
        probe[0].position[1] = nm_force_float((j&2 ? 0x80000000u : 0u) | ((j*503u)&0x7fffffu) | (((j*7)%140)<<23));
    }
    probe[0].word5c = words[(i>>4)&15]; probe[1].word5c = words[i&15];
}
static inline uint32_t nm_collision_candidate_test_hash(const nm_drive_params_t *math) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_COLLISION_CANDIDATE_CASES; ++i) {
        nm_collision_probe_t probe[2]; unsigned hit;
        nm_collision_candidate_test_input(i,probe);
        if (!nm_collision_candidate(math,&probe[0],&probe[1],&hit)) return 0;
        hash = (hash^hit)*16777619u;
    }
    return hash;
}

#define NM_COLLISION_WALK_CASES 8192u
#define NM_COLLISION_WALK_HASH UINT32_C(0x577933dd)
/* Same bounded traversal fixtures as the independent i960 loop oracle.
 * Responses below are deliberate test mutations, NOT gameplay responses. */
static inline uint32_t nm_collision_walk_test_hash(const nm_drive_params_t *math) {
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<NM_COLLISION_WALK_CASES;++i) {
        alignas(32) nm_collision_rank_t rank[64];
        unsigned count,reset,primary;uint32_t wrap;uint16_t order[14];
        nm_collision_prepare_test_input(i,rank,&count,&reset,&primary,&wrap);
        if(!nm_collision_prepare(rank,count,reset,primary,wrap,order)) return 0;
        alignas(32) nm_collision_walk_body_t body[64]={0};
        for(unsigned j=0;j<count;++j) {
            body[j].next=rank[j].next;body[j].candidates=rank[j].candidates;
            if(i%7==0) body[j].candidates=0;
            if(i%7==1) body[j].candidates=13;
            body[j].probe.position[0]=(float)((int)(j%7)-3)*(i&4 ? 2.0f : 0.0f);
            body[j].probe.position[1]=(float)((int)(j/7)-3)*(i&8 ? 1.0f : 0.0f);
            body[j].probe.word5c=nm_force_bits((float)((j+i)%5)*(i&16 ? 1.0f : 0.0f));
        }
        const int budgets[]={1000,5,10,15,160,310,315,1000};
        nm_collision_walk_t state={0};state.budget=budgets[(i/4)%8];
        unsigned cost=0,hits=0;
        for(;;) {
            uint16_t pair[2];
            const int found=nm_collision_walk_next(math,body,count,order,primary,i%4,cost,&state,pair);
            if(found<0) return 0;
            if(!found) break;
            if(++hits>7) return 0; /* Each response consumes at least160 of1000 units. */
            hash=(hash^pair[0])*16777619u;hash=(hash^pair[1])*16777619u;
            cost=(i+hits)&1 ? 150 : 300;
            for(unsigned b=0;b<2;++b) {
                nm_collision_probe_t *probe=&body[pair[b]].probe;
                if(i&32) probe->position[b]+=b ? -8.0f : 8.0f;
                if(i&64) probe->word5c=nm_force_bits((float)((hits+b)%4));
            }
        }
        hash=(hash^(uint32_t)state.budget)*16777619u;
    }
    return hash;
}

/* Complete116d0 inputs, shared with the independent whole-entity oracle.
 * The final struct padding is not part of the canonical output digest. */
static inline void nm_wheels_test_input(const nm_contact_pack_t *p, unsigned i, nm_wheels_state_t *s) {
    memset(s,0,sizeof(*s));
    const nm_contact_quad_t *q = p->quads+i/4;
    for (unsigned v = 0; v < 4; ++v) for (unsigned a = 0; a < 3; ++a) s->position[a] += q->xyz[v][a]*0.25f;
    s->yaw = nm_yaw_angle(i*1999); s->roll = nm_yaw_angle(i*17); s->pitch = nm_yaw_angle(i*31);
    s->road = i/4;
    for (unsigned v = 0; v < 4; ++v) {
        s->height[v] = s->position[1]+(float)((int)((i+v)%3)-1);
        s->surface[v] = (i+v)%p->quad_count;
    }
}
static inline uint32_t nm_wheels_hash_state(uint32_t hash, const nm_wheels_state_t *s) {
    const unsigned char *bytes = (const unsigned char *)s;
    for (unsigned i = 0; i < offsetof(nm_wheels_state_t,road_heading)+sizeof(s->road_heading); ++i)
        hash = (hash^bytes[i])*16777619u;
    return hash;
}
static inline uint32_t nm_wheels_test_hash(const nm_contact_pack_t *p, const nm_drive_params_t *math) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < p->quad_count*4; ++i) {
        nm_wheels_state_t s; nm_wheels_test_input(p,i,&s);
        if (!nm_wheels_ground(p,math,&s)) return 0;
        hash = nm_wheels_hash_state(hash,&s);
    }
    return hash;
}
static inline uint32_t nm_wheels_expected_hash(unsigned slot) {
    const uint32_t hashes[4] = {0xb15e8507,0xa2f28482,0xd420143f,0x053f0039};
    return slot < 4 ? hashes[slot] : 0;
}

/* Same deterministic input set on host/SH4. Digests come from command36,
 * not rounded tolerances. Covers each grid cell and every prepared quad. */
static inline unsigned nm_contact_test_count(const nm_contact_pack_t *p) {
    return 98304+10*p->quad_count;
}
/* Exact next binary32 for finite fixture coordinates. KOS nextafterf performs
 * an underflow operation at zero; DN=1 then turns the intended input into0.
 * Construct bits so host and target actually test the SAME input. */
static inline float nm_contact_test_neighbor(float value,unsigned positive) {
    uint32_t bits=nm_force_bits(value);
    if(!(bits&0x7fffffffu)) bits=positive ? 1u : UINT32_C(0x80000001);
    else if((bits>>31)==positive) --bits;
    else ++bits;
    return nm_force_float(bits);
}
static inline void nm_contact_test_input(const nm_contact_pack_t *p, unsigned i, float out[3]) {
    if (i < 98304) {
        unsigned cell = i/6;
        out[0] = (int)(cell&127)*16-1024 + (i&1 ? -0.5f : 8.0f);
        out[1] = (int)(cell>>7)*16-1024 + (i&1 ? -0.5f : 8.0f);
        out[2] = (float)((int)((i%6)/2)*100-100);
    } else {
        i -= 98304;
        const nm_contact_quad_t *q = p->quads+i/10;
        const unsigned n = i%10;
        if (n < 8) {
            const float *xyz = q->xyz[n/2];
            out[0] = n&1 ? nm_contact_test_neighbor(xyz[0],1) : xyz[0];
            out[1] = n&1 ? nm_contact_test_neighbor(xyz[2],0) : xyz[2];
            out[2] = xyz[1];
        } else {
            float center[3] = {0};
            for (unsigned v = 0; v < 4; ++v) for (unsigned a = 0; a < 3; ++a)
                center[a] += q->xyz[v][a]*0.25f;
            out[0] = center[0]; out[1] = center[2]; out[2] = n == 8 ? center[1] : -center[1];
        }
    }
}
static inline uint32_t nm_contact_test_hash(const nm_contact_pack_t *p, const nm_drive_params_t *math) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < nm_contact_test_count(p); ++i) {
        float input[3]; nm_contact_test_input(p,i,input);
        nm_contact_result_t result = {input[2],0x1234,0x5678,0xabcdef};
        if (!nm_contact_query(p,math,input[0],input[1],&result)) return 0;
        const unsigned char *bytes = (const unsigned char *)&result;
        for (unsigned b = 0; b < sizeof(result); ++b) hash = (hash^bytes[b])*16777619u;
    }
    return hash;
}
static inline uint32_t nm_contact_expected_hash(unsigned slot) {
    const uint32_t hashes[4] = {0x1abbe549,0x0e2a6261,0x29469e92,0x5ffac1ea};
    return slot < 4 ? hashes[slot] : 0;
}
#endif
