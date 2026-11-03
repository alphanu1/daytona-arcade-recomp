/* Host harnesses only (tests/trace_race_native.c and trace_attract_native.c, --drive NEW_FILE): the drive board's
 * command stream (include/drive_board.h) over the main-loop iterations the harness runs, in the format of
 * m2segment --trace-drive (dreamcast-native/tools/trace_drive.hpp), for tests/test_drive_lockstep.py:
 *   "D b0=N byte=XX"          a byte the sender 1abbc sent in the iteration whose frame counter 5010b0 is N;
 *   "D b0=0 byte=ff direct"   power-on's write of 1bf08 (not queued);
 *   "R first=A last=B"        the iterations this run covered (each harness of a chain covers the next ones).
 * The pushes of an iteration come before its send (the phase handler 1898 and the object chain 1838 run before
 * 1abbc), so a harness runs the sends up to the iteration before its pushes, pushes, then sends that iteration's. */
#ifndef DAYTONA_NATIVE_DRIVE_TRACE_H
#define DAYTONA_NATIVE_DRIVE_TRACE_H
#include "drive_board.h"
#include <inttypes.h>
#include <stdio.h>

typedef struct {
    nm_drive_board_t queue; /* from power-on, carried from scene to scene (tests/game_chain.h) */
    uint32_t through;       /* the last iteration whose send ran */
    uint32_t sent, hash;    /* the bytes sent, their FNV-1a (code/drive_board.c prints the console's the same way) */
    uint32_t pad[5];
} nm_drive_run_t;

/* The sends of the iterations after `through` up to B0. */
static inline void nm_drive_run_to(nm_drive_run_t *d, FILE *out, uint32_t b0) {
    while (d->through < b0) {
        uint8_t c;
        ++d->through;
        if (!nm_drive_board_send(&d->queue, &c)) continue;
        if (!d->sent++) d->hash = UINT32_C(2166136261);
        d->hash = (d->hash ^ c) * UINT32_C(16777619);
        if (out) fprintf(out, "D b0=%" PRIu32 " byte=%02x\n", d->through, c);
    }
}
/* Iteration B0 of a phase handler that pushes its fixed byte (nm_drive_board_phase), and its send. */
static inline void nm_drive_run_phase(nm_drive_run_t *d, FILE *out, uint32_t b0, unsigned phase) {
    nm_drive_run_to(d, out, b0 - 1u);
    nm_drive_board_phase(&d->queue, phase);
    nm_drive_run_to(d, out, b0);
}
static inline FILE *nm_drive_run_open(const char *path) {
    FILE *out = fopen(path, "wx");
    if (out) fputs("# drive-trace v1 side=native\n", out);
    return out;
}
/* The covered range and the file's end; 0 on a write error. */
static inline int nm_drive_run_close(FILE *out, uint32_t first, uint32_t last) {
    if (!out) return 1;
    fprintf(out, "R first=%" PRIu32 " last=%" PRIu32 "\n", first, last);
    const int failed = ferror(out);
    return !fclose(out) && !failed;
}
#endif
