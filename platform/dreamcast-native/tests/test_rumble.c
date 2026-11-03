/* Host check of the rumble pack's player (include/rumble.h) and of the drive board queue's rules it is fed by
 * (include/drive_board.h): jolts, levels, stops, the output's maximum; the grass pulse through the queue; the queue's
 * dedup, the speed bytes, the inhibit compare that never matches, a contact's growing queue, the race start's
 * parameter. The stream as a whole is checked against the original by tests/test_drive_lockstep.py.
 * Build: cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude -DNM_RUMBLE_JOLT_TICKS=N tests/test_rumble.c */
#include "drive_board.h"
#include "rumble.h"
#include <stdio.h>
#include <string.h>

#ifndef NM_RUMBLE_JOLT_TICKS
#define NM_RUMBLE_JOLT_TICKS 9
#endif

static unsigned failures, checks;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* Feeds BYTE, then COUNT-1 empty iterations; the outputs into out[0..COUNT). */
static void feed(nm_rumble_t *r, uint8_t byte, unsigned count, unsigned jolt, uint8_t *out) {
    for (unsigned i = 0; i < count; ++i) out[i] = (uint8_t)nm_rumble_tick(r, i ? 0 : byte, jolt);
}

static void player(void) {
    const unsigned J = NM_RUMBLE_JOLT_TICKS;
    uint8_t out[300];
    nm_rumble_t r = {0};
    CHECK(nm_rumble_tick(&r, 0, J) == 0);
    /* A pull: power min(n, 7), at least 1, for J iterations (the one it starts in included). */
    feed(&r, 0x57, J + 2, J, out);
    for (unsigned i = 0; i < J; ++i) CHECK(out[i] == 7);
    CHECK(out[J] == 0 && out[J + 1] == 0);
    feed(&r, 0x61, 1, J, out); CHECK(out[0] == 1);
    feed(&r, 0x50, 1, J, out); CHECK(out[0] == 1);
    feed(&r, 0x6f, 1, J, out); CHECK(out[0] == 7);
    feed(&r, 0x64, 1, J, out); CHECK(out[0] == 4);
    /* A new pull restarts the jolt with its power. */
    nm_rumble_stop(&r);
    feed(&r, 0x57, 3, J, out);
    feed(&r, 0x51, J + 1, J, out);
    for (unsigned i = 0; i < J; ++i) CHECK(out[i] == 1);
    CHECK(out[J] == 0);
    /* 0x4-: a continuous level min(n, 7); 0x1-..0x3- set it to 0. */
    nm_rumble_stop(&r);
    feed(&r, 0x43, 50, J, out);
    for (unsigned i = 0; i < 50; ++i) CHECK(out[i] == 3);
    feed(&r, 0x33, 2, J, out); CHECK(out[0] == 0 && out[1] == 0);
    feed(&r, 0x4c, 1, J, out); CHECK(out[0] == 7);
    feed(&r, 0x27, 1, J, out); CHECK(out[0] == 0);
    feed(&r, 0x44, 1, J, out); CHECK(out[0] == 4);
    feed(&r, 0x10, 1, J, out); CHECK(out[0] == 0);
    feed(&r, 0x40, 1, J, out); CHECK(out[0] == 0);
    /* The output is the larger of the jolt and the level. */
    feed(&r, 0x43, 1, J, out); CHECK(out[0] == 3);
    feed(&r, 0x51, J + 1, J, out);
    for (unsigned i = 0; i <= J; ++i) CHECK(out[i] == 3);
    feed(&r, 0x67, J + 1, J, out);
    for (unsigned i = 0; i < J; ++i) CHECK(out[i] == 7);
    CHECK(out[J] == 3);
    /* A level byte during a jolt keeps the jolt; a 0x3- byte keeps it too. */
    feed(&r, 0x57, 2, J, out);
    CHECK(nm_rumble_tick(&r, 0x38, J) == (J > 2 ? 7u : 0u));
    /* 0x0-, 0x7- and any other type stop everything; the pause (nm_rumble_stop) too. */
    static const uint8_t stops[] = {0x01, 0x03, 0x07, 0x08, 0x09, 0x0a, 0x71, 0x79, 0x72, 0x74, 0x82, 0xff};
    for (unsigned k = 0; k < sizeof stops; ++k) {
        nm_rumble_stop(&r);
        feed(&r, 0x44, 1, J, out); feed(&r, 0x57, 1, J, out);
        CHECK(nm_rumble_tick(&r, stops[k], J) == 0 && r.level == 0 && r.jolt == 0 && r.jolt_ticks == 0);
        feed(&r, 0, 3, J, out); CHECK(out[2] == 0);
    }
    feed(&r, 0x44, 1, J, out); feed(&r, 0x57, 1, J, out);
    nm_rumble_stop(&r);
    CHECK(r.power == 0 && nm_rumble_tick(&r, 0, J) == 0);
    /* One iteration's jolt with J = 1. */
    nm_rumble_stop(&r);
    CHECK(nm_rumble_tick(&r, 0x57, 1) == 7 && nm_rumble_tick(&r, 0, 1) == 0);
}

/* The player's fields on the road at speed index V. */
static nm_drive_board_input_t road(uint32_t v) {
    nm_drive_board_input_t e;
    memset(&e, 0, sizeof e);
    e.side = -1; e.speed = v; e.mask[0] = e.mask[1] = 0x002;
    return e;
}
/* One iteration: the race update, then the send; returns the byte sent (0: none). */
static uint8_t iteration(nm_drive_board_t *q, const nm_drive_board_input_t *e) {
    uint8_t c = 0;
    nm_drive_board_race(q, e);
    return nm_drive_board_send(q, &c) ? c : 0;
}
static unsigned depth(const nm_drive_board_t *q) { return (uint8_t)(q->write - q->read); }

static void stream(void) {
    nm_drive_board_t q;
    memset(&q, 0, sizeof q);
    /* The speed bytes: OR, not add; one step per 64 from 63 + 64k; from 575 the OR reaches the type bits. */
    static const struct { uint32_t v; uint8_t c; } speeds[] = {
        {0, 0x27}, {7, 0x27}, {8, 0x26}, {55, 0x21}, {56, 0x20}, {63, 0x20}, {64, 0x38}, {126, 0x38}, {127, 0x39},
        {190, 0x39}, {191, 0x3a}, {255, 0x3b}, {319, 0x3c}, {383, 0x3d}, {574, 0x3f}, {575, 0x38}, {700, 0x39}};
    for (unsigned i = 0; i < sizeof speeds / sizeof speeds[0]; ++i) {
        memset(&q, 0, sizeof q);
        const nm_drive_board_input_t e = road(speeds[i].v);
        CHECK(iteration(&q, &e) == speeds[i].c);
        CHECK(iteration(&q, &e) == 0); /* the same byte again is dropped */
    }
    /* A negative speed index: a value above 0xff never equals the last byte, its low byte queued every iteration. */
    memset(&q, 0, sizeof q);
    const nm_drive_board_input_t back = road(UINT32_MAX);
    const uint8_t low = (uint8_t)(((UINT32_MAX - 0x3fu) >> 6) | 0x38u);
    CHECK(iteration(&q, &back) == low && iteration(&q, &back) == low);
    /* The input inhibit: 0x0190 (direction +1) queues 0x27 on its start value only; 0xff90 never matches. */
    memset(&q, 0, sizeof q);
    nm_drive_board_input_t e = road(200);
    e.inhibit = (int8_t)0x90; e.inhibit_word = (int16_t)0xff90;
    CHECK(iteration(&q, &e) == 0);
    e.inhibit_word = 0x0190;
    CHECK(iteration(&q, &e) == 0x27);
    e.inhibit = (int8_t)0x8f; e.inhibit_word = 0x018f;
    CHECK(iteration(&q, &e) == 0);
    /* A contact ahead: 0x54 and 0x44 every iteration, one sent: the queue grows by one an iteration. */
    memset(&q, 0, sizeof q);
    e = road(200); e.side = 3;
    for (unsigned i = 0; i < 40; ++i) CHECK(iteration(&q, &e) == (i & 1 ? 0x44 : 0x54));
    CHECK(depth(&q) == 40);
    e = road(200);
    CHECK(iteration(&q, &e) == 0x54 && depth(&q) == 40); /* 0x3a queued behind the backlog */
    /* The 0x0- bytes are always queued and leave the last byte alone. */
    memset(&q, 0, sizeof q);
    nm_drive_board_push(&q, 0x54); nm_drive_board_push(&q, 0x09); nm_drive_board_push(&q, 0x09); nm_drive_board_push(&q, 0x54);
    CHECK(depth(&q) == 3 && q.last == 0x54);
    /* The race start: 0x70 | course | transmission, 0x07, 0x10; the parameter is dropped like any typed byte. The last
     * three rows are the tables' other entries (3b7c[3] 00, 3b80[2], [3] ff), which no native race reaches. */
    static const struct { unsigned course, transmission; uint8_t c; } starts[] = {
        {0, 0, 0x71}, {0, 1, 0x79}, {2, 0, 0x72}, {2, 1, 0x7a}, {1, 0, 0x74}, {1, 1, 0x7c},
        {3, 0, 0x70}, {1, 2, 0xff}, {3, 3, 0xff}};
    for (unsigned i = 0; i < sizeof starts / sizeof starts[0]; ++i) {
        memset(&q, 0, sizeof q);
        nm_drive_board_start(&q, starts[i].course, starts[i].transmission);
        uint8_t a = 0, b = 0, c = 0, d = 0;
        CHECK(nm_drive_board_send(&q, &a) && nm_drive_board_send(&q, &b) && nm_drive_board_send(&q, &c) && !nm_drive_board_send(&q, &d));
        CHECK(a == starts[i].c && b == 0x07 && c == 0x10);
    }
    memset(&q, 0, sizeof q); q.last = 0x71;
    nm_drive_board_start(&q, 0, 0);
    CHECK(depth(&q) == 2);
    /* The kerb: one pull and 32 iterations of hold-off; the speed path ends it. */
    memset(&q, 0, sizeof q);
    e = road(300); e.mask[1] = 0x100;
    CHECK(iteration(&q, &e) == 0x51 && q.counter == 32);
    CHECK(iteration(&q, &e) == 0 && q.counter == 32);
    e = road(300);
    CHECK(iteration(&q, &e) == 0x3b && q.counter == 0);
}

/* The grass pulse through the queue and the player: 0x43 and 0x33 alternating, 20 iterations on, 13 off. */
static void grass(void) {
    nm_drive_board_t q;
    memset(&q, 0, sizeof q);
    nm_rumble_t r = {0};
    nm_drive_board_input_t e = road(150);
    e.mask[0] = 0x004;
    unsigned on = 0, off = 0, changes = 0, previous = 0;
    for (unsigned i = 0; i < 33 * 4; ++i) {
        const unsigned p = nm_rumble_tick(&r, iteration(&q, &e), NM_RUMBLE_JOLT_TICKS);
        if (i < 20 || (i >= 33 && i < 53)) CHECK(p == 3);
        if (i >= 20 && i < 33) CHECK(p == 0);
        on += p == 3; off += p == 0; changes += i && p != previous; previous = p;
    }
    CHECK(on == 4 * 20 && off == 4 * 13 && changes == 7);
    /* Stopped on grass: 0x10, the level 0. */
    e.speed = 0;
    CHECK(iteration(&q, &e) == 0x10);
}

int main(void) {
    player();
    stream();
    grass();
    if (failures) { printf("FAIL: rumble player: %u of %u checks failed\n", failures, checks); return 1; }
    printf("PASS: rumble player and drive board queue: %u checks (jolt %u ticks)\n", checks, (unsigned)NM_RUMBLE_JOLT_TICKS);
    return 0;
}
