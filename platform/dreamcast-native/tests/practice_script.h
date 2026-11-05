#ifndef DAYTONA_NATIVE_PRACTICE_SCRIPT_H
#define DAYTONA_NATIVE_PRACTICE_SCRIPT_H
#include "../include/practice.h"

/* Network diagnostic only: 1800 throttle ticks, 600 brake ticks. Split at the
 * input boundary even when one render spans it. No poses/replay or new physics. */
#define NM_PRACTICE_SCRIPT_TICKS 2400u
static inline int nm_practice_script_advance(nm_practice_t *s, const nm_drive_params_t *math,
        const nm_contact_pack_t *contact, const nm_road_pack_t *road,
        const nm_path_pack_t *path, nm_road_marks_t *marks, uint32_t elapsed_us) {
    if (!s || s->ticks>NM_PRACTICE_SCRIPT_TICKS || s->remainder_us>=NM_PRACTICE_TICK_US) return 0;
    if (s->ticks==NM_PRACTICE_SCRIPT_TICKS) return 1;
    const uint32_t maximum=4u*NM_PRACTICE_TICK_US;
    if (elapsed_us>maximum) { s->dropped_us+=elapsed_us-maximum; elapsed_us=maximum; }
    while (elapsed_us && s->ticks<NM_PRACTICE_SCRIPT_TICKS) {
        const int gas=s->ticks<1800;
        const uint32_t boundary=gas ? 1800u : NM_PRACTICE_SCRIPT_TICKS;
        const uint32_t remaining=(boundary-s->ticks)*NM_PRACTICE_TICK_US-s->remainder_us;
        const uint32_t part=elapsed_us<remaining ? elapsed_us : remaining;
        const nm_controls_t controls={.accel=gas ? 255 : 0,.brake=gas ? 0 : 255};
        if (!nm_practice_advance(s,math,contact,road,path,marks,&controls,part)) return 0;
        elapsed_us-=part;
    }
    return 1;
}

/* Canonical diagnostic fields, not struct padding or a claim of full-state
 * equivalence across architectures. Host cadence checks also compare full state. */
static inline uint32_t nm_practice_script_digest(const nm_practice_t *s) {
    uint32_t words[9];
    memcpy(words,s->car.boundary.ground.position,12);
    words[3]=(uint16_t)s->car.boundary.heading;
    words[4]=(uint32_t)s->car.phase[0];
    words[5]=(uint32_t)s->car.speed_index;
    words[6]=s->car.checkpoints.visited;
    words[7]=s->car.checkpoints.clock;
    words[8]=s->award.remaining;
    uint32_t hash=2166136261u;
    for (unsigned i=0;i<9;++i) hash=(hash^words[i])*16777619u;
    return hash;
}
#endif
