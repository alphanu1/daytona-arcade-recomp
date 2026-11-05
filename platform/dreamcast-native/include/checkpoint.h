#ifndef DAYTONA_NATIVE_CHECKPOINT_H
#define DAYTONA_NATIVE_CHECKPOINT_H
#include "road.h"

typedef struct {
    uint16_t furthest, best_ticks, last_ticks, start_ticks;
    uint8_t counter, completed, lap;
} nm_lap_t;
typedef struct {
    uint32_t visited, required, clock, finish_flags, display_ticks, reverse;
    uint8_t last_lap, completed, total_laps;
} nm_checkpoints_t;
/* One explicit write to the lap-time table, not a target guest address. */
typedef struct { uint32_t ticks; uint8_t index, written; } nm_lap_write_t;

typedef struct {
    uint32_t remaining, display, calls;
    uint16_t announcement_ticks;
    uint8_t counter, condition;
    uint32_t cues[3]; /* Ordered silent audio IDs, never source code pointers. */
    uint8_t cue_count;
} nm_checkpoint_award_t;
typedef struct {
    uint32_t blocked, player_flags, penalty;
    uint16_t path_flags;
    uint8_t difficulty, suppression;
} nm_checkpoint_award_input_t;

/*134f8/135e0. Authored time extensions and presentation intents, no audio
 * playback or board state. This caller is the player, so cue selection uses
 * its OLD counter. The time lookup uses OLD+1 before byte truncation.
 * All32-bit additions/subtractions deliberately wrap, including call count. */
static inline int nm_checkpoint_award_step(const nm_road_awards_t *p,
        const nm_checkpoint_award_input_t *in, nm_checkpoint_award_t *state, int16_t *signal) {
    if (!in || !state || !signal) return 0;
    const int award = !(in->blocked&1) && !(state->condition && !in->suppression && (in->path_flags&64));
    if (award && (!p || in->difficulty >= 4)) return 0;
    nm_checkpoint_award_t s = *state;
    uint16_t next_signal = (uint16_t)*signal;
    s.cue_count = 0;
    if (award) {
        const unsigned index = s.counter&15, next = (unsigned)s.counter+1;
        if (p->cue[index] == 0xae2108) {
            s.cues[s.cue_count++] = 0xae207c;
            s.cues[s.cue_count++] = 0xae2108;
            s.announcement_ticks = 220;
        } else {
            s.cues[s.cue_count++] = p->cue[index];
            next_signal = (uint16_t)((next_signal&0xff00)|p->signal[index]);
        }
        s.cues[s.cue_count++] = 0xae2010;
        s.counter = (uint8_t)next;
        s.display = 128;
        uint32_t ticks = p->ticks[in->difficulty][next];
        if (in->penalty) ticks -= p->adjustment[0][in->difficulty];
        if (in->player_flags&(1u<<28)) ticks += p->adjustment[1][in->difficulty];
        s.remaining += ticks;
    }
    ++s.calls;
    *signal = next_signal < 32768 ? (int16_t)next_signal : (int16_t)((int)next_signal-65536);
    *state = s;
    return 1;
}

static inline void nm_checkpoint_mark(unsigned index, nm_lap_t *lap,
        nm_checkpoints_t *race, uint32_t *flags) {
    if (!index && race->visited == race->required && lap->lap != race->last_lap) {
        race->last_lap = lap->lap;
        const unsigned completed = (unsigned)race->completed+1;
        race->completed = lap->completed = (uint8_t)completed;
        /* Compare BEFORE byte truncation; reset for another lap, not at finish. */
        if (completed < race->total_laps) race->visited = 0;
    }
    const uint32_t mask = 1u<<(index&31);
    if (!(race->visited&mask)) {
        race->visited |= mask;
        *flags |= 1u<<21;
        lap->counter = (uint8_t)(lap->counter+1u);
    }
}

/* Full132b8..134f4. One source update; course checkpoints are immutable DNR2
 * data. Preserve byte wrapping, countdown checkpoint indices and full32-bit
 * elapsed comparison despite16-bit body times. No float/TGP/heap/VRAM work.
 * Validate before mutation. The caller applies the optional lap-table write. */
static inline int nm_checkpoint_step(const nm_road_pack_t *road, uint16_t current,
        uint16_t *previous, uint32_t *flags, nm_lap_t *lap,
        nm_checkpoints_t *race, nm_lap_write_t *out) {
    if (!road || road->profile_count < 2 || road->profile_count > 4097 ||
        road->checkpoint_count > 31 || (road->checkpoint_count && !road->checkpoints) ||
        !previous || !flags || !lap || !race || !out) return 0;
    const int wrap = (int)road->profile_count-1, delta = (int)current-*previous;
    out->written = 0;
    if (!delta) return 1;
    int finish = 0;
    if (delta < -1) {
        if (delta == -wrap) {
            lap->lap = (uint8_t)(lap->lap+1u);
            finish = lap->furthest == wrap;
        }
    } else if (delta > 1) {
        if (delta == wrap) {
            lap->lap = (uint8_t)(lap->lap-1u);
            finish = lap->furthest >= (unsigned)(wrap/2);
        }
    } else {
        const int distance = (int)current-lap->furthest;
        if ((distance >= 0 && distance < 7) || current == wrap/2 || current == wrap/2+4)
            lap->furthest = current;
        for (unsigned i = 0; i < road->checkpoint_count; ++i)
            if (current == road->checkpoints[i])
                nm_checkpoint_mark(road->checkpoint_count-i,lap,race,flags);
    }
    if (finish) {
        lap->furthest = 0;
        nm_checkpoint_mark(0,lap,race,flags);
        if ((int)lap->completed > (int)race->total_laps-1) race->finish_flags |= 3;
        race->display_ticks = 128;
        const uint32_t elapsed = race->clock-lap->start_ticks;
        lap->last_ticks = (uint16_t)elapsed;
        out->ticks = elapsed; out->index = lap->completed; out->written = 1;
        if (!lap->best_ticks || lap->best_ticks > elapsed) lap->best_ticks = (uint16_t)elapsed;
        lap->start_ticks = (uint16_t)race->clock;
        if (delta > 1) race->reverse = 1;
    }
    *previous = current;
    return 1;
}
#endif
