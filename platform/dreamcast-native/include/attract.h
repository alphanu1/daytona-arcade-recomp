/* The original's attract loop (phase handlers 018cc), one loop a course:
 *   2 (197c, one iteration): setup, the advertisement music, the course's data (39c0: 3a48[501460]);
 *   3 (1d24): the cinematic, 226360's stage graph 2865a60[501460], 1602 iterations (its entry, the scene start,
 *     then 1601 ticks);
 *   4/5 (1d8c, 1dfc): the ranking overlay 2266f8 over the same cinematic, 1 + 320 iterations;
 *   6/7 (1e5c, 2028): the V.R. demo race, 1 + 834 iterations; at its end 2058 steps the course,
 *     501460 = 20b8[501460] % 4 (0 -> 2 -> 1 -> 0);
 *   8/9/10 (20d4, 2338, 2374): the title, 1 + 386 + 1 iterations, then phase 2 again (country JPN; USA shows its
 *     warning in phase 11 first).
 * 3147 iterations a loop on every course (m2segment --trace-frontend, no input: course 0 from frame 179, 2 from
 * 3361, 1 from 6541, 0 again from 9723). The cinematic's first tick of a loop is the iteration after phase 2's:
 * 5010b0 = object_clock + ticks. A game keeps 501460 (the raced course): the attract after it shows that course.
 * The V.R. demo runs natively (vr_demo.h; its phase-8 iteration is counted), the 2D layer over all of it is
 * attract_2d.h. Shared by code/practice.c and tests/trace_attract_native.c. */
#ifndef DAYTONA_NATIVE_ATTRACT_H
#define DAYTONA_NATIVE_ATTRACT_H
#include <stdint.h>

enum {
    NM_ATTRACT_PHASE3_TICKS = 1601,  /* the cinematic's ticks after its scene start in phase 3 */
    NM_ATTRACT_TICKS = 1922,         /* + the ranking overlay's 1 + 320 */
    NM_ATTRACT_DEMO_ITERATIONS = 836,/* phases 6 (1), 7 (834) and 8 (1) */
    NM_ATTRACT_TITLE_TICKS = 386     /* phase 9; phase 10 one more iteration */
};

/* The cinematic's latched flags of the tick after `ticks` (phase 3, 4 or 5). */
static inline unsigned nm_attract_phase(uint32_t ticks) {
    return ticks < NM_ATTRACT_PHASE3_TICKS ? 3u : ticks == NM_ATTRACT_PHASE3_TICKS ? 4u : 5u;
}

/* 2058..209c: the next loop's course slot, 20b8[501460] (bytes 2, 0, 1, 3) modulo 4. */
static inline unsigned nm_attract_next_course(unsigned course) {
    static const uint8_t next[4] = {2, 0, 1, 3};
    return next[course & 3u] % 4u;
}
#endif
