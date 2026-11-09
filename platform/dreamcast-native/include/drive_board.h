/* The drive board's command stream: the bytes the original sends to the force-feedback drive board (the I/O board's
 * dual-port byte 0x11, 01c00022), as the program makes them. Output only: these functions read their arguments and
 * write the queue, nothing else, so the race and every scene run the same with or without them.
 *  - The queue: the ring 502900..5029ff (a zero slot is empty), its write index 5028fe and send index 5028ff, the last
 *    byte queued with a nonzero type 5028fd and the signed counter 5028fc (the grass pulse's phase and the kerb's
 *    hold-off). Work RAM, zero from power-on and never reset by a scene or a race: the boot's queue init 1ab44 clears
 *    only the ring and the indices, before the first push, so a race after another starts with the last byte and the
 *    counter the one before left (its grid's 0x27 can be dropped). Nothing checks for a full ring: the race can queue
 *    two bytes an iteration (a contact ahead or behind), the queue then grows by one an iteration and drains after it
 *    (64 deep in the original's longest push against a car ahead).
 *  - The sender 1abbc: the main loop's (0x1288), once an iteration after the object chain, at most one byte.
 *  - The race update 1a470 (nm_drive_board_race): the drive task 504a80 (handler 4e0c), every iteration of phases
 *    20..22, reading the player as the previous iteration left it (race_scene.h nm_race_scene_drive).
 *  - The race start's parameter 3a58 (phase 21's handler 34a8) and the fixed 0x0- bytes seven phase handlers push at
 *    their start (nm_drive_board_phase).
 * Power-on's 0xff (1bf08) and the test mode's DRIVE BD TEST write the port directly, not through the queue.
 * Shared by the console (code/drive_board.c, which plays the sent bytes on a rumble pack: include/rumble.h) and the
 * host harnesses (tests/drive_trace.h, checked against the original by tests/test_drive_lockstep.py). */
#ifndef DAYTONA_NATIVE_DRIVE_BOARD_H
#define DAYTONA_NATIVE_DRIVE_BOARD_H
#include <stdint.h>

typedef struct {
    uint8_t ring[256]; /* 502900..5029ff */
    int8_t counter;    /* 5028fc */
    uint8_t last;      /* 5028fd */
    uint8_t write;     /* 5028fe: the next slot to write */
    uint8_t read;      /* 5028ff: the next slot to send */
    uint8_t pad[28];   /* to nine cache lines */
} nm_drive_board_t;
_Static_assert(sizeof(nm_drive_board_t) == 288, "drive board queue layout");

/* The push the program inlines at every constant site (197c, 1e5c, 2b0c, 3010, 3ab8, 3b18, 4110, 4328, 4b40 and in
 * 1a470): a 0x0- byte always, the last byte untouched; any other only when it differs from the last one queued. */
static inline void nm_drive_board_push(nm_drive_board_t *q, uint8_t c) {
    if (!(c & 0xf0u)) { q->ring[q->write++] = c; return; }
    if (q->last == c) return;
    q->ring[q->write++] = c; q->last = c;
}
/* The same without the type test: the speed path 1a4f0, the kerb pull 1aaf4, 3a58's first push 3a88. C is the
 * program's 32-bit value: compared with the zero-extended last byte (cmpibe), its low byte stored. */
static inline void nm_drive_board_push_typed(nm_drive_board_t *q, uint32_t c) {
    if ((uint32_t)q->last == c) return;
    q->ring[q->write++] = (uint8_t)c; q->last = (uint8_t)c;
}

/* 1abbc: the byte in the send slot, if any (1, *out), the slot emptied; 0 when the slot is empty. The program
 * stores the byte to 01c00022 (1abd4). */
static inline int nm_drive_board_send(nm_drive_board_t *q, uint8_t *out) {
    const uint8_t c = q->ring[q->read];
    if (!c) return 0;
    q->ring[q->read++] = 0;
    *out = c;
    return 1;
}

/* The byte a phase handler pushes at its start, once per visit: 197c (2, each attract loop), 1e5c (6, the V.R.
 * demo), 2b0c (17, a credit: on to the selects), 3008 (19, the race entry), 4328 (23, the player's goal), 4110 (25,
 * the results, after a goal too), 4b40 (29, the game's end). 0: the phase pushes none (21's are 3a58's). */
static inline uint8_t nm_drive_board_phase_byte(unsigned phase) {
    switch (phase) {
    case 2: case 6: return 0x01;
    case 17: return 0x03;
    case 19: return 0x07;
    case 23: case 25: return 0x09;
    case 29: return 0x08;
    default: return 0;
    }
}
static inline void nm_drive_board_phase(nm_drive_board_t *q, unsigned phase) {
    const uint8_t c = nm_drive_board_phase_byte(phase);
    if (c) nm_drive_board_push(q, c);
}

/* 3a58, from phase 21's handler 34a8 (the green light, or the rolling start's release): the parameter 0x70 | the
 * course's bits 3b7c[501460] | the transmission's 3b80[501464], then 0x07 and 0x10. COURSE is the road pack's slot
 * (0 Beginner, 1 Expert, 2 Advanced), TRANSMISSION 501464 (0 automatic, 1 manual): the natively possible indices. */
static inline void nm_drive_board_start(nm_drive_board_t *q, unsigned course, unsigned transmission) {
    /* 3b7c's bytes 01 04 02 00 and 3b80's 00 08 ff ff, as arithmetic: a table would sit in .rodata, whose end places
     * the console's .data and .bss (code/drive_board.c). */
    const uint32_t course_bits = (UINT32_C(0x0241) >> ((course & 3u) * 4u)) & 15u;
    const uint32_t gear_bits = (transmission & 2u) ? 0xffu : (transmission & 1u) << 3;
    /* 3a78..3a84: 7 << 4, or'd with both (a 32-bit value, 0xff when the transmission's bits are). */
    nm_drive_board_push_typed(q, 0x70u | course_bits | gear_bits);
    nm_drive_board_push(q, 0x07); /* 3ab8 */
    nm_drive_board_push(q, 0x10); /* 3b18 */
}

/* The player's fields 1a470 reads (e = *501220, the car pool's entity 0), as the previous iteration left them. */
typedef struct {
    uint32_t flags;  /* e+0, ld: bit 25 wheel 0 (front left), bit 23 wheel 2 (rear left) hit the wall */
    uint32_t speed;  /* e+18, ld: the speed index, compared unsigned (cmpobge) */
    uint16_t mask[2];/* e+1ba, e+1bc: the front wheels' surface masks (1 << the quad's surface & 15) */
    int16_t crash;   /* e+c8, ldis: the crash timer */
    int16_t inhibit_word; /* e+d4 | e+d5 << 8, ldis: the input inhibit timer and its direction */
    int8_t side;     /* e+27d, ldib: the car contact's side, -1 (255) none */
    int8_t inhibit;  /* e+d4, ldib: the input inhibit (spin-out) timer */
    int8_t spin;     /* e+ec, ldib: the wall-spin timer */
    int8_t scrape;   /* e+99, ldib: the wall-scrape timer */
    uint8_t scrape_side; /* e+ed, ldob: bit 1 the scraped wheel's side */
} nm_drive_board_input_t;

/* 1a470..1aae4: the race update, a priority chain over the player's state; the first active input decides. Every
 * comparison is the program's, signedness included. Every byte it queues has a nonzero type, for which the program's
 * inline push and push_typed are the same; the speed path's 32-bit value is the one that needs push_typed, so the chain
 * picks its bytes and pushes once (and a second byte for a contact ahead or behind). */
static inline void nm_drive_board_race(nm_drive_board_t *q, const nm_drive_board_input_t *e) {
    uint32_t c = 0;
    uint8_t second = 0;
    /* 1a7b8 (the spin's pull, the crash's above 80): by the wheel that hit, at spin timer 32 and 10 (1a7b8..1a7c4);
     * bit 25 wheel 0, bit 23 wheel 2: the left wheels (1a7c8..1a838). */
    const uint8_t spin_pull = e->spin != 32 && e->spin != 10 ? 0 :
        (e->flags & (UINT32_C(1) << 25 | UINT32_C(1) << 23)) ? 0x67 : 0x57;
    if (e->side >= 0) {                                                 /* 1a478..1a47c: bbc 31 */
        c = e->side == 2 || e->side != 0 ? 0x54 : 0x64;                 /* 1a5fc right, 1a660 left, 1a538 the rest */
        if (e->side != 2 && e->side != 0) second = 0x44;                /* 1 and 3: behind, ahead (any other too) */
    } else if (e->crash != 0) {                                         /* 1a480..1a484 */
        c = e->crash > 0x50 ? spin_pull : 0x10;                         /* 1a6c4..1a6cc: cmpibg; 1a6d0 */
    } else if (e->inhibit != 0) {                                       /* 1a488..1a48c */
        /* 1a734..1a74c: lda 0xff90 is the zero-extended 0x0000ff90, never equal to the sign-extended halfword, so
         * only the start value of direction +1 (0x0190) queues 0x27. */
        const int32_t word = e->inhibit_word;
        if (word == (int32_t)0xff90 || word == 0x0190) c = 0x27;
    } else if (e->spin != 0) {                                          /* 1a490..1a494 */
        c = spin_pull;
    } else if (e->scrape != 0) {                                        /* 1a498..1a49c */
        if (e->scrape == 16 || e->scrape == 10) c = (e->scrape_side & 2u) ? 0x54 : 0x64; /* 1a89c..1a918 */
    } else if ((e->mask[0] | e->mask[1]) & 0x100u) {
        /* 1a4a0..1a4ac: the unaligned words at e+1ba and e+1bc hold mask 0 and mask 1 in their low halves; bit 8 of
         * wheel 0 (1aaf0: 0x61) before wheel 1's (1aae8: 0x51). 1aaf4: a kerb's pull, then 32 iterations of hold-off
         * (no decrement here: the speed path ends it), the counter set also when the push was dropped. */
        if (q->counter != 0) return;
        c = (e->mask[0] & 0x100u) ? 0x61 : 0x51;
        q->counter = 32;
    } else if ((e->mask[0] | e->mask[1]) & 12u) {                       /* 1a4b0..1a4bc: grass or sand */
        if (e->speed == 0) c = 0x10;                                    /* 1a97c..1a980 -> 1aa84 */
        else if (q->counter == 0) { q->counter = 32; c = 0x43; }        /* 1a984..1a9fc */
        else {
            q->counter = (int8_t)(q->counter - 1);                      /* 1aa00..1aa0c: subi */
            if (q->counter == 12) c = 0x33;                             /* 1aa14..1aa80 */
        }
    } else {
        const uint32_t v = e->speed;                                    /* 1a4c0..1a4e8: OR, not add */
        c = v <= 0x3fu ? ((0x3fu - v) >> 3) | 0x20u : ((v - 0x3fu) >> 6) | 0x38u;
        q->counter = 0;                                                 /* 1a520..1a524: also when dropped */
    }
    if (!c) return;
    nm_drive_board_push_typed(q, c);
    if (second) nm_drive_board_push_typed(q, second);
}

/* The console's stream (code/drive_board.c): one queue from power-on, each sent byte to the rumble player
 * (include/rumble.h; NATIVE_RUMBLE=0: nowhere). Called where the scenes run their iterations. */
nm_drive_board_t *nm_drive_stream(void);
void nm_drive_stream_phase(unsigned phase); /* a phase handler's byte (nm_drive_board_phase) */
void nm_drive_stream_ticks(unsigned count); /* COUNT main-loop iterations' sends (1abbc), then the pack's output */
void nm_drive_stream_pause(void);           /* the inspector's pause: the pack stops */
void nm_drive_stream_end(void);             /* the run's end: the pack stops, the stream's line */
#endif
