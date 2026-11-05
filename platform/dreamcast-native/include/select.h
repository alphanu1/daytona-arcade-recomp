#ifndef DAYTONA_NATIVE_SELECT_H
#define DAYTONA_NATIVE_SELECT_H
#include <stdint.h>
#include <string.h>
#include "nm_mem.h"

/* The course and transmission select of the original (daytona93), one call per main-loop iteration
 * (build/dc-native-frontend.PLAN.md). Gameplay: the chosen course 501460, the transmission 501464 and
 * the number of iterations the screens take (the frame counter 5010b0 the race inherits); the rest is
 * the presentation state the select renderer reads. No ROM data.
 *
 * Phase 17 (2b0c) disables every attract object and falls through into phase 18's handler 2e24 in the
 * same iteration: substate 0 enables the select object 501218 with the updater 225188 and arms the
 * phase timer 5010a8 = 1600, substate 1 decrements it every iteration (225188, then 225234 from the
 * next iteration on, run in the object chain after the handler). The select object's own state +62
 * (table 2252d4):
 *   0 225318  voice ae215a, timer +56 = 640                                   (course select)
 *   1 225368  slot from the wheel, course = slot 0/1/2 -> 0/2/1 (23addc); accelerator edge or the timer
 *             running out -> 2 with the slot's voice (23b504)
 *   2 225418  voice ae2158, +58 = 128;  3 225458: the chosen panel blinks for 128 iterations
 *   4 225494  +56 = 640 (the transmission timer), +54 = 0;  5 2254e8: +54 += 25 until 500 (20 iterations)
 *   6 225534  the transmission screen's layouts, +54 = 10;  7 22558c: 10 iterations
 *   8 22561c  slot 0, voice ae215b
 *   9 2256ac  transmission from the wheel; 501a80 = START held (time attack); the accelerator edge counts
 *             once +56 <= 624 (16 iterations in), or the timer running out -> 10 with the voice 23b514
 *             (23b53c with time attack)
 *  10 2257e4  voice ae2158, +56 = 128;  11 225824: 128 iterations
 *  12 = 4, 13 = 5 (fade out);  14 225864: voice ae2100, 5010a8 = 192 ("GENTLEMEN START YOUR ENGINES")
 *  15 2258b0  9 iterations (+5f), then the course load 1354 (31 lag frames in the original, one iteration)
 *  16 22590c  sounds at 5010a8 == 128 (9e037f) and 64 (9e117f, ae2001, 23b52c[course])
 * Phase 18's substate 1 goes to 6 on the iteration that finds 5010a8 negative; substate 6 disables the
 * select object and selects phase 19 on the next one (the race entry, 3008, one iteration later).
 * The timer digit is +56 >> 6, drawn in states 0-4 and 9 (226304).
 *
 * Inputs per iteration (4e18, before the phase handler): the decoded steering 501050 and the previous
 * iteration's 50105c (nm_controls_decode's steer), the accelerator's high edge 501068
 * (nm_controls_t.accel_high_edge), START held (53e120 bit 4). */

typedef struct {
    int32_t steer, previous_steer; /* 501050, 50105c */
    uint8_t accel_edge;            /* 501068 */
    uint8_t start_held;            /* 53e120 bit 4 */
} nm_select_input_t;

enum { NM_SELECT_SOUNDS = 4 };
typedef struct {
    uint32_t sound[NM_SELECT_SOUNDS]; /* 19bf4 commands in order */
    unsigned sounds;
} nm_select_output_t;

typedef struct {
    /* Phase 18 handler 2e24. */
    uint8_t substate;     /* 5010f8 */
    int32_t phase_timer;  /* 5010a8 */
    uint8_t enabled;      /* 501218 flags bit 31 */
    uint8_t started;      /* 225188 ran: the updater is 225234 */
    uint8_t phase;        /* 5010a4: 18, then 19 */
    /* The select object 501218. */
    uint8_t state;        /* +62 */
    uint16_t timer;       /* +56 */
    uint16_t blink;       /* +58 */
    uint16_t fade;        /* +54 */
    uint8_t countdown;    /* +5f */
    uint8_t chosen;       /* +5e: the course slot */
    uint16_t spin;        /* +5a: the transmission screen's cars turn 128 a call of 225b6c */
    int32_t frame[2];     /* +34/+38: the highlight frame's position (2259ec), the scroll 50280c */
    /* Outputs. */
    uint16_t slot;        /* 501468 */
    uint8_t course;       /* 501460 */
    uint8_t transmission; /* 501464 */
    uint32_t time_attack; /* 501a80 */
} nm_select_t;

/* 23add4 / 23ae88 (+0, +4): the highlight frame's target per slot; 23addc (+8): the course of a slot. */
static const int32_t nm_select_course_frame[3][2] = {{95, 396}, {247, 396}, {399, 396}};
static const int32_t nm_select_transmission_frame[2][2] = {{111, 388}, {381, 388}};
static const uint8_t nm_select_course_of_slot[3] = {0, 2, 1};
/* Voices: 23b504 per course slot, 23b514 per transmission, 23b53c with time attack (53e59a bit 0),
 * 23b52c the course's "gentlemen" call. */
static const uint32_t nm_select_course_voice[3] = {0xae215e, 0xae215f, 0xae2160};
static const uint32_t nm_select_transmission_voice[2] = {0xae215d, 0xae215c};
static const uint32_t nm_select_time_attack_voice[2] = {0xae2173, 0xae221c};
static const uint32_t nm_select_course_call[3] = {0xae2112, 0xae2113, 0xae2113};
enum {
    NM_SELECT_TIMER = 640,          /* 225324 / 225494: 5 << 7 */
    NM_SELECT_CONFIRM_AFTER = 0x270, /* 22579c: the transmission accepts an edge once +56 <= 624 */
    NM_SELECT_PHASE_TIMER = 1600,   /* 2ed0: 25 << 6 */
    NM_SELECT_START_TIMER = 192,    /* 225888: 3 << 6 */
    NM_SELECT_COURSE_SPLIT = 108,   /* 2253ac: 27 << 2 */
    NM_SELECT_MANUAL_ABOVE = 30     /* 2256e8 */
};

static inline void nm_select_sound(nm_select_output_t *out, uint32_t command) {
    if (out->sounds < NM_SELECT_SOUNDS) out->sound[out->sounds++] = command;
}

/* 2259ec: the highlight frame glides toward the slot's target, half the distance a call, clamped to
 * +-40 (shri: arithmetic). Presentation only. */
static inline void nm_select_glide(nm_select_t *s, const int32_t target[2]) {
    for (unsigned axis = 0; axis < 2; ++axis) {
        int32_t step = (int32_t)((uint32_t)target[axis] - (uint32_t)s->frame[axis]) >> 1;
        step = step < -40 ? -40 : step > 40 ? 40 : step;
        s->frame[axis] = (int32_t)((uint32_t)s->frame[axis] + (uint32_t)step);
    }
}
static inline void nm_select_course_glide(nm_select_t *s) {
    nm_select_glide(s, nm_select_course_frame[s->slot < 3 ? s->slot : 0]);
}
/* 225b6c (the transmission screen's cars): +5a += 128, then the frame glide of 2259ec beside it. */
static inline void nm_select_transmission_glide(nm_select_t *s) {
    nm_select_glide(s, nm_select_transmission_frame[s->slot < 2 ? s->slot : 0]);
    s->spin = (uint16_t)(s->spin + 128u);
}

/* (501050 + 50105c) >> 1, addi then shri. */
static inline int32_t nm_select_wheel(const nm_select_input_t *in) {
    return (int32_t)((uint32_t)in->steer + (uint32_t)in->previous_steer) >> 1;
}

/* 16-bit countdown (ldos, subo, stos; the branch tests the 32-bit difference): 1 when it reached 0. */
static inline int nm_select_count(uint16_t *timer) {
    const uint32_t next = (uint32_t)*timer - 1u;
    *timer = (uint16_t)next;
    return next == 0;
}

/* The iteration of phase 17 after its handler's object switch-off: the fall-through into 2e24
 * substate 0 and 1, then the select object's 225188 in the object chain. */
static inline void nm_select_enter(nm_select_t *s) {
    nm_memset(s, 0, sizeof(*s));
    s->phase = 18;
    s->enabled = 1;
    s->substate = 1;
    s->phase_timer = NM_SELECT_PHASE_TIMER - 1;     /* 2ed4, then 2ee0..2eec once */
    s->countdown = 9;                               /* 2251c4 */
    s->frame[0] = nm_select_course_frame[0][0];     /* 225210: 23add4 */
    s->frame[1] = nm_select_course_frame[0][1];
    s->started = 1;
}

/* The select object's updater 225234 for one iteration (after 225268). */
static inline int nm_select_object(nm_select_t *s, const nm_select_input_t *in, uint32_t counter,
        nm_select_output_t *out) {
    (void)counter;
    switch (s->state) {
    case 0:
        nm_select_sound(out, 0xae215a);
        s->timer = NM_SELECT_TIMER;
        nm_select_course_glide(s);
        s->state = 1;
        return 1;
    case 1: {
        nm_select_course_glide(s);
        const int32_t wheel = nm_select_wheel(in);
        const uint16_t slot = wheel <= 0 ? 0 : wheel <= NM_SELECT_COURSE_SPLIT ? 1 : 2;
        s->slot = slot; s->chosen = (uint8_t)slot;
        s->course = nm_select_course_of_slot[slot];
        if (in->accel_edge) {
            s->state = 2; s->timer = 0;
            nm_select_sound(out, nm_select_course_voice[s->slot]);
            return 1;
        }
        if (nm_select_count(&s->timer)) {
            s->state = 2;
            nm_select_sound(out, nm_select_course_voice[s->slot]);
        }
        return 1;
    }
    case 2:
        nm_select_sound(out, 0xae2158);
        nm_select_course_glide(s);
        s->blink = 128; s->state = 3;
        return 1;
    case 3:
        nm_select_course_glide(s);
        if (nm_select_count(&s->blink)) s->state = 4;
        return 1;
    case 4: case 12:
        s->timer = NM_SELECT_TIMER;
        s->state = (uint8_t)(s->state + 1);
        s->fade = 0;
        return 1;
    case 5: case 13:
        s->fade = (uint16_t)(s->fade + 25u);
        if (s->fade >= 500) s->state = (uint8_t)(s->state + 1);
        return 1;
    case 6:
        s->fade = 10; s->state = 7;
        return 1;
    case 7:
        if (nm_select_count(&s->fade)) s->state = 8;
        return 1;
    case 8:
        s->slot = 0;
        nm_select_sound(out, 0xae215b);
        s->frame[0] = nm_select_transmission_frame[0][0];   /* 225658: 23ae88 */
        s->frame[1] = nm_select_transmission_frame[0][1];
        s->state = 9;
        nm_select_transmission_glide(s);
        return 1;
    case 9: {
        nm_select_transmission_glide(s);
        const uint8_t manual = nm_select_wheel(in) > NM_SELECT_MANUAL_ABOVE;
        s->slot = manual; s->transmission = manual;
        s->time_attack = in->start_held ? 1u : 0u;
        const int ready = (int32_t)s->timer <= NM_SELECT_CONFIRM_AFTER;
        if ((ready && in->accel_edge) || nm_select_count(&s->timer)) {
            s->state = 10;
            nm_select_sound(out, s->time_attack ? nm_select_time_attack_voice[0] : nm_select_transmission_voice[s->slot]);
        }
        return 1;
    }
    case 10:
        nm_select_sound(out, 0xae2158);
        s->timer = 128; s->state = 11;
        nm_select_transmission_glide(s);
        return 1;
    case 11:
        if (nm_select_count(&s->timer)) { s->state = 12; return 1; }
        nm_select_transmission_glide(s);
        return 1;
    case 14:
        nm_select_sound(out, 0xae2100);
        s->phase_timer = NM_SELECT_START_TIMER;
        s->state = 15;
        return 1;
    case 15:
        s->countdown = (uint8_t)(s->countdown - 1u);
        if (s->countdown) return 1;
        s->state = 16; s->countdown = 9;    /* 1354 loads the course here */
        return 1;
    case 16: {
        const int32_t left = (int8_t)s->countdown - 1;
        s->countdown = (uint8_t)(left > 0 ? left : 0);
        if (s->phase_timer == 128) nm_select_sound(out, 0x9e037f);
        else if (s->phase_timer == 64) {
            nm_select_sound(out, 0x9e117f); nm_select_sound(out, 0xae2001);
            nm_select_sound(out, nm_select_course_call[s->course < 3 ? s->course : 0]);
        }
        return 1;
    }
    }
    return 0;
}

/* One main-loop iteration of phase 18: the handler, then the select object. Returns 0 on a state the
 * original never reaches here (substates 2-5), 1 otherwise; s->phase is 19 once the handler selected
 * the race entry (the select object is then off). */
static inline int nm_select_tick(nm_select_t *s, const nm_select_input_t *in, uint32_t counter,
        nm_select_output_t *out) {
    if (!s || !in || !out || s->phase != 18) return 0;
    out->sounds = 0;
    const int32_t old = s->phase_timer;
    s->phase_timer = (int32_t)((uint32_t)old - 1u);
    if (s->substate == 1) {
        if (old < 0) s->substate = 6;                /* 2ef4..2f20 */
    } else if (s->substate == 6) {
        if (old < 0) {                               /* 2f5c..2fe4 */
            s->enabled = 0; s->phase = 19; s->substate = 0;
        }
    } else return 0;
    return !s->enabled || nm_select_object(s, in, counter, out);
}

#endif
