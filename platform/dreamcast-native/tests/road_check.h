#ifndef DAYTONA_NATIVE_ROAD_CHECK_H
#define DAYTONA_NATIVE_ROAD_CHECK_H
#include "../include/road.h"
#include "../include/vehicle.h"
#include "../include/checkpoint.h"
#include <string.h>

#define NM_CHECKPOINT_AWARD_CASES 65536u
static inline uint32_t nm_checkpoint_award_expected_hash(unsigned slot) {
    const uint32_t hashes[] = {0xf78b8a4cu,0xaa4a741bu,0x5bc10b71u,0x948195deu};
    return slot < 4 ? hashes[slot] : 0;
}
static inline void nm_checkpoint_award_test_input(unsigned i, nm_checkpoint_award_input_t *in,
        nm_checkpoint_award_t *s, int16_t *signal) {
    memset(in,0,sizeof(*in)); memset(s,0,sizeof(*s));
    s->counter = (uint8_t)i; s->condition = (i>>8)&1;
    s->remaining = i&1024 ? UINT32_MAX-i : i*15485863u;
    s->display = 0xabcdef12; s->calls = i&2048 ? UINT32_MAX : i; s->announcement_ticks = 65535;
    s->cue_count = 3; s->cues[0] = s->cues[1] = s->cues[2] = 0xabcdef01;
    in->difficulty = (i>>9)&3; in->blocked = i&4096 ? 0x80000001u : 0x80000000u;
    in->penalty = i&8192 ? UINT32_MAX : 0; in->player_flags = i&16384 ? (1u<<28) : 0;
    in->path_flags = (i>>1)&64; in->suppression = i&32768 ? 255 : 0;
    *signal = -12345;
}
static inline uint32_t nm_checkpoint_award_hash_state(uint32_t hash, const nm_checkpoint_award_t *s, int16_t signal) {
    const uint32_t words[] = {s->remaining,s->display,s->calls,s->announcement_ticks,s->counter,(uint16_t)signal,s->cue_count};
    for (unsigned n = 0; n < sizeof(words)/sizeof(words[0]); ++n) for (unsigned b = 0; b < 4; ++b)
        hash = (hash^((words[n]>>(8*b))&255))*16777619u;
    for (unsigned n = 0; n < s->cue_count; ++n) for (unsigned b = 0; b < 4; ++b)
        hash = (hash^((s->cues[n]>>(8*b))&255))*16777619u;
    return hash;
}
static inline uint32_t nm_checkpoint_award_test_hash(const nm_road_awards_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_CHECKPOINT_AWARD_CASES; ++i) {
        nm_checkpoint_award_t s; nm_checkpoint_award_input_t in; int16_t signal;
        nm_checkpoint_award_test_input(i,&in,&s,&signal);
        if (!nm_checkpoint_award_step(p,&in,&s,&signal)) return 0;
        hash = nm_checkpoint_award_hash_state(hash,&s,signal);
    }
    return hash;
}

#define NM_CHECKPOINT_CASES 65536u
#define NM_CHECKPOINT_HASH 0x37831200u
typedef struct {
    uint16_t points[31], current, previous;
    uint32_t count, profiles, flags;
    nm_lap_t lap;
    nm_checkpoints_t race;
    nm_lap_write_t write;
} nm_checkpoint_case_t;
static inline void nm_checkpoint_test_case(unsigned i, nm_checkpoint_case_t *v) {
    memset(v,0,sizeof(*v));
    const uint32_t clocks[] = {0,1,65535,65536,UINT32_MAX,0x80000000,12345};
    const uint16_t wraps[] = {1,2,195,990,486,249,4096};
    const unsigned k = i/16, wrap = wraps[k%7];
    v->profiles = wrap+1; v->count = k%32;
    v->current = (uint16_t)((k*73)%(wrap+1));
    v->previous = v->current;
    switch (i%8) {
    case 1: v->previous = (uint16_t)(v->current-1u); break;
    case 2: v->previous = (uint16_t)(v->current+1u); break;
    case 3: v->current = 0; v->previous = (uint16_t)wrap; break;
    case 4: v->current = (uint16_t)wrap; v->previous = 0; break;
    case 5: v->current = (uint16_t)(wrap/2); v->previous = (uint16_t)(v->current+1u); break;
    case 6: v->current = (uint16_t)(wrap/2+4); v->previous = (uint16_t)(v->current-1u); break;
    case 7: v->previous = (uint16_t)(v->current+2u); break;
    }
    for (unsigned n = 0; n < v->count; ++n)
        v->points[n] = n%3 ? v->current : (uint16_t)((v->current+1u)%(wrap+1));
    v->lap.furthest = (uint16_t)(k%4 == 0 ? wrap : k%4 == 1 ? wrap/2 :
        k%4 == 2 ? (unsigned)(uint16_t)(v->current-6u) : (unsigned)(uint16_t)(v->current-7u));
    v->lap.counter = (uint8_t)(i>>3); v->lap.lap = (uint8_t)(i>>4);
    v->lap.completed = (uint8_t)(k%4 == 0 ? 255 : k%4);
    v->lap.best_ticks = k%3 ? (uint16_t)(k*17) : 0;
    v->lap.last_ticks = 0x5678; v->lap.start_ticks = (uint16_t)(k*31);
    v->race.clock = clocks[(k/4)%7]; v->race.total_laps = (uint8_t)(k%5 == 0 ? 255 : k%5);
    v->race.completed = v->lap.completed;
    v->race.last_lap = (uint8_t)(v->lap.lap+(k%3 == 0 ? 1u : 0u));
    v->race.required = v->count == 31 ? UINT32_MAX : (1u<<(v->count+1))-1;
    v->race.visited = i&8 ? v->race.required : 0;
    v->flags = i*2654435761u; v->race.finish_flags = i*73u;
    v->race.display_ticks = 17; v->race.reverse = 0xa5a5a5a5;
    v->write.ticks = 0x98765432; v->write.index = 123; v->write.written = 1;
}
static inline int nm_checkpoint_test_step(nm_checkpoint_case_t *v) {
    nm_road_pack_t road; memset(&road,0,sizeof(road));
    road.profile_count = v->profiles; road.checkpoint_count = v->count; road.checkpoints = v->points;
    return nm_checkpoint_step(&road,v->current,&v->previous,&v->flags,&v->lap,&v->race,&v->write);
}
static inline uint32_t nm_checkpoint_test_hash(void) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_CHECKPOINT_CASES; ++i) {
        nm_checkpoint_case_t v; nm_checkpoint_test_case(i,&v);
        if (!nm_checkpoint_test_step(&v)) return 0;
        const uint32_t values[] = {v.previous,v.flags,v.lap.furthest,v.lap.best_ticks,v.lap.last_ticks,
            v.lap.start_ticks,v.lap.counter,v.lap.completed,v.lap.lap,v.race.visited,v.race.required,
            v.race.clock,v.race.finish_flags,v.race.display_ticks,v.race.reverse,v.race.last_lap,
            v.race.completed,v.race.total_laps,v.write.ticks,v.write.index,v.write.written};
        for (unsigned n = 0; n < sizeof(values)/sizeof(values[0]); ++n) for (unsigned b = 0; b < 4; ++b)
            hash = (hash^((values[n]>>(8*b))&255))*16777619u;
    }
    return hash;
}

/* ROM-free vectors. The host i960/TGP oracle checks these same inputs before
 * accepting the digest as a portable SH4 regression gate. */
#define NM_ROAD_CHECK_CASES (4096u + 216u)
#define NM_ROAD_CHECK_HASH 0x84d8b661u
static inline void nm_road_test_case(unsigned i, nm_road_profile_t *p, nm_road_lateral_t *s) {
    const int16_t deltas[] = {-17,-16,-15,0,15,16,17};
    const int16_t steps[] = {-17,-16,-1,0,1,16,17};
    const int16_t extremes[] = {-32768,-1792,-1,0,1792,32767};
    memset(p, 0, sizeof(*p));
    p->first_lane = i % 3; p->last_lane = 5 + i % 3;
    for (int lane = 0; lane < 8; ++lane) for (int a = 0; a < 3; ++a) {
        p->lanes[lane].xyz[a] = (lane-3)*(a+1)*1.25f + a*0.03125f;
        p->lanes[lane].angles[a] = (int16_t)((lane-4)*(a+1)*1024);
    }
    s->current = (int16_t)((int)i - 1024);
    s->target = s->current + deltas[i%7];
    s->step = steps[(i/7)%7]; s->control = 224 + i%2;
    if (i >= 4096) {
        const unsigned j = i-4096;
        s->current = extremes[j/36]; s->target = extremes[(j/6)%6]; s->step = extremes[j%6];
    }
}
static inline uint32_t nm_road_test_hash(void) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_ROAD_CHECK_CASES; ++i) {
        nm_road_profile_t p; nm_road_lateral_t s; nm_road_pose_t pose;
        nm_road_test_case(i, &p, &s);
        if (!nm_road_pose_step(&p, &s, &pose)) return 0;
        uint8_t bytes[sizeof(s)+sizeof(pose)];
        memcpy(bytes, &s, sizeof(s)); memcpy(bytes+sizeof(s), &pose, sizeof(pose));
        for (unsigned n = 0; n < sizeof(bytes); ++n) hash = (hash ^ bytes[n]) * 16777619u;
    }
    return hash;
}

#define NM_CONTROLS_CHECK_CASES 65536u
#define NM_CONTROLS_CHECK_HASH 0x053a560bu
typedef struct {
    uint8_t adc[3], previous[2];
    nm_vehicle_input_t state;
    int8_t condition_d4;
    int16_t condition_c8;
    int32_t counter, speed;
} nm_vehicle_test_t;
static inline void nm_vehicle_test_case(unsigned i, nm_vehicle_test_t *v) {
    const int32_t wheels[] = {INT32_MIN,INT32_MAX,-32768,-1821,-1820,-1,0,1,1820,1821,32767};
    const int32_t counters[] = {-1,0,63,64,65,INT32_MAX};
    const int32_t speeds[] = {-1,0,63,64,65,INT32_MAX};
    const int8_t d4[] = {-1,0,1,127,-128};
    const int16_t c8[] = {1,0,-1,0,0};
    memset(v, 0, sizeof(*v));
    v->adc[0] = (uint8_t)(i>>8); v->adc[1] = (uint8_t)i; v->adc[2] = (uint8_t)(i*73);
    v->previous[0] = v->previous[1] = (uint8_t)(i>>8);
    v->state.steer = wheels[i%11]; v->state.override_count = (uint8_t)i;
    v->state.accel = 0xaaaa; v->state.brake = 0xbbbb;
    v->condition_d4 = d4[(i/11)%5]; v->condition_c8 = c8[(i/11)%5];
    v->counter = counters[(i/55)%6]; v->speed = speeds[(i/330)%6];
}
static inline uint32_t nm_controls_test_hash(void) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_CONTROLS_CHECK_CASES; ++i) {
        nm_vehicle_test_t v; nm_vehicle_test_case(i, &v);
        const nm_controls_t input = nm_controls_decode(v.adc[0],v.adc[1],v.adc[2],v.previous[0],v.previous[1]);
        if (!nm_vehicle_input_step(&v.state,&input,v.condition_d4+v.condition_c8 != 0,v.counter,v.speed)) return 0;
        const uint32_t values[] = {(uint32_t)input.steer,input.accel,input.brake,input.accel_high_edge,input.brake_high_edge,
            (uint32_t)v.state.steer,v.state.accel,v.state.brake,v.state.override_count};
        for (unsigned n = 0; n < 9; ++n) for (unsigned b = 0; b < 4; ++b)
            hash = (hash ^ ((values[n] >> (8*b)) & 255)) * 16777619u;
    }
    return hash;
}
#endif
