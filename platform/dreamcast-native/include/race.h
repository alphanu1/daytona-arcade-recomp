#ifndef DAYTONA_NATIVE_RACE_H
#define DAYTONA_NATIVE_RACE_H
#include <stdint.h>
#include "road.h"

typedef struct {
    uint32_t initial_ticks;
    uint8_t total_laps, difficulty;
} nm_race_settings_t;

typedef struct {
    uint16_t profile, previous, furthest;
    int8_t lap;
} nm_race_grid_profile_t;

/* e23c..e284, following the per-car authored grid record selection. Offset is
 * signed, but ADDI wraps before unsigned REMO. Starting lap is zero from reset. */
static inline int nm_race_grid_profile(uint32_t authored, int16_t offset,
        uint32_t count, nm_race_grid_profile_t *out) {
    if (!out || !count || count > 4096) return 0;
    const uint32_t profile=(authored+(uint32_t)(int32_t)offset)%count;
    *out=(nm_race_grid_profile_t){.profile=(uint16_t)profile,
        .previous=(uint16_t)authored,.furthest=(uint16_t)authored,
        .lap=(int8_t)(profile >= (count>>1) ? -1 : 0)};
    return 1;
}

/* 03848..03874 selects course/difficulty; 033f4..03400 reads table entry0.
 * DNR3 lacks authored laps, so cannot silently stand in for race settings.
 * Selection only: grid placement and phase-entry side effects are separate. */
static inline int nm_race_settings(const nm_road_pack_t *road, unsigned difficulty,
        nm_race_settings_t *out) {
    if (!road || !out || !road->awards || road->slot >= 4 || difficulty >= 4 ||
        !road->total_laps || road->total_laps > 255) return 0;
    *out = (nm_race_settings_t){.initial_ticks=road->awards->ticks[difficulty][0],
        .total_laps=(uint8_t)road->total_laps,.difficulty=(uint8_t)difficulty};
    return 1;
}

/* 03560..035f4: phase22 timer/finish decision AFTER its UI/bookkeeping calls.
 * Returns the next phase; caller handles display/results and phase entry.
 * Unsigned storage preserves i960 wrapping; bit31 implements its signed test.
 * No board scheduling or sound commands belong in this native operation. */
static inline uint8_t nm_race_active_timer(uint8_t mode74, uint16_t player204,
        uint32_t hold, uint32_t *remaining, uint32_t *clock, uint32_t *finish) {
    if (mode74 || !player204) {
        if (!hold) --*remaining;
        /* Both original speed branches converge on timeout for negative time. */
        if (*remaining & UINT32_C(0x80000000)) return 25;
    }
    const uint32_t completed = *finish & 1u;
    *finish &= ~UINT32_C(1);
    if (completed) return 23;
    ++*clock;
    return 22;
}
/* Phase23 (4328..444c). Explicit owners/intents replace guest updater pointers
 * da18 and44a8. 19bf4 queues three-byte SOUND commands, not graphics addresses.
 * Keep these as silent intents until AICAFlow. The caller must enable the
 * post-finish player/result updates; this is not a timer-only transition.
 * Flags belong to the objects referenced by50121c and501230 respectively. */
typedef struct {
    uint32_t camera_mode, timer, previous_object_flags, result_object_flags;
    uint32_t audio_commands[2];
    int16_t countdown;
    uint8_t phase, player_finishing, result_pending;
    uint8_t audio_count, command;
} nm_race_finish_entry_t;

static inline int nm_race_finish_enter(uint32_t inhibit, int8_t rank,
        nm_race_finish_entry_t *s) {
    if(!s || s->phase!=23) return 0;
    s->camera_mode=0;s->timer=384;s->countdown=-1;
    s->previous_object_flags&=UINT32_C(0x7fffffff);
    s->result_object_flags|=UINT32_C(0x80000000);
    s->player_finishing=1;s->result_pending=1;
    const int winner=!inhibit && rank==0;
    s->audio_count=(uint8_t)(1+winner);
    s->audio_commands[0]=winner ? UINT32_C(0xae2124) : UINT32_C(0xbe3700);
    s->audio_commands[1]=winner ? UINT32_C(0xbe3700) : 0;
    s->command=9;
    s->phase=24;return 1;
}

/* 04454..0449c phase24, after display bookkeeping37e8. Rank6a is SIGNED.
 * Zero retains phase24; transition happens only after decrement below zero.
 * Caller owns phase23 entry (timer384) and subsequent results/timeout setup. */
static inline uint8_t nm_race_finish_timer(uint32_t *timer, int8_t rank, uint32_t mode) {
    --*timer;
    if (!(*timer&UINT32_C(0x80000000))) return 24;
    return rank>2 || mode ? 25 : 27;
}
/*42a4 selects phase25's delay;42e8..4320 advances phase26 below128. */
static inline uint32_t nm_race_timeout_ticks(uint32_t mode) { return mode ? 608u : 800u; }
static inline uint8_t nm_race_timeout_timer(uint32_t *timer, uint32_t *mode) {
    --*timer;
    if ((*timer&UINT32_C(0x80000000)) || *timer<128) {
        *mode=0;
        return 29;
    }
    return 26;
}

/* Phase29 tail4d00 and phase30 at4d60. Caller must first retire the race
 * owners/load the next scene (4b40..4cfc). Name actor226a84 and return
 * callee228490 are separate owners, NOT implemented by this control step.
 * In particular, signed-negative phase30 time returns to phase2; zero only
 * disables the name actor, positive time waits. Do not replace with a usual
 * countdown-to-zero rule. Audio is an intent, never playback here. */
typedef struct {
    uint32_t timer, name_flags, audio_command;
    uint8_t phase, cabinet_flags, start_name, return_to_attract;
} nm_race_postresult_t;
static inline int nm_race_postresult_step(nm_race_postresult_t *s,uint32_t notification) {
    if(!s || (s->phase!=29 && s->phase!=30)) return 0;
    s->audio_command=0;s->start_name=0;s->return_to_attract=0;
    if(s->phase==29) {
        s->phase=30;s->timer=1920;
        if(notification) {
            s->name_flags|=UINT32_C(0x80000000);
            s->audio_command=UINT32_C(0xae1005);s->start_name=1;
            return 1;
        }
    } else {
        --s->timer;
        if(!s->timer) { s->name_flags&=UINT32_C(0x7fffffff);return 1; }
        if(!(s->timer&UINT32_C(0x80000000))) return 1;
    }
    s->cabinet_flags&=128u;
    s->audio_command=UINT32_C(0xbe1a10);
    s->return_to_attract=1;s->phase=2;
    return 1;
}
#endif
