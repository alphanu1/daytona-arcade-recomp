#ifndef DAYTONA_NATIVE_FRAME_CLOCK_H
#define DAYTONA_NATIVE_FRAME_CLOCK_H
#include <stdint.h>

/* One simulation tick per displayed frame (the NTSC field, 59.94 Hz), counted from the PVR's vblanks instead of
 * a wall-clock accumulator: the arcade's 17384 us tick would give 0- or 2-tick frames whenever the timer read
 * drifted across a boundary, and its 57.5 Hz against the 59.94 Hz display a beat of one skipped frame every
 * 23. The game runs 4% faster than the cabinet; every tick stays bit-exact (NM_PRACTICE_TICK_US is the host
 * harness's unit of one tick, and what a frame hands the updaters as elapsed time: ticks * NM_PRACTICE_TICK_US).
 *
 * KOS counts vbl_count on every vblank and frame_count on the vblanks that flip a finished render, both in the
 * vblank interrupt (pvr_vblank_handler), and a render cannot start before the previous one has flipped
 * (pvr_render_lists), so enDjinn's loop of updater, registration, render is one flip per updater. Their
 * difference, the vblanks that showed the same frame again, changes only in that interrupt and only when a
 * frame is late: sampled anywhere in the updater, a frame on time reads 1 tick and a frame k vblanks late reads
 * 1+k, whatever the updater's own timing. The first frame after a reset primes the counter and runs no tick (as
 * the loading time before it was never simulated); the callers bound the catch-up at 4 ticks. */
typedef struct { uint32_t missed; uint8_t primed; } nm_frame_clock_t;
#define NM_FRAME_TICKS_LIMIT 1000u /* a stall over 16 s is not caught up on; keeps ticks * NM_PRACTICE_TICK_US in 32 bits */
uint32_t nm_frame_ticks(nm_frame_clock_t *clock);
#endif
