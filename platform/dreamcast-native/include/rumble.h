/* The rumble pack's player: the drive board's sent bytes (include/drive_board.h, the arcade's stream) to the power of
 * the pack's one motor. This mapping is ours, not the arcade's: the pack has no direction and no steady force. The
 * ROM's test menu (DRIVE BD TEST) names the types 1- SPRING, 2- CLUTCH, 3- CENTERING, 4- UNCENTERING, 5- ROLL LEFT,
 * 6- ROLL RIGHT. Per sent byte, type and level n:
 *   0x5-, 0x6-  a jolt of power min(n, 7), at least 1, for the build's jolt ticks: the kerb (0x51, 0x61), the wall's
 *               spin and scrape (0x57, 0x67, 0x54, 0x64), a car on the side (0x54, 0x64);
 *   0x4-        the continuous level min(n, 7): the grass pulse's first part (0x43), a contact ahead or behind (0x44);
 *   0x1- 0x2- 0x3-  the continuous level 0 (a wheel's steady forces: the speed, the standstill, the crash's 0x10, the
 *               grass pulse's second part 0x33);
 *   0x0-, 0x7-  everything stops: the scene changes (01 03 07, the goal's and the results' 09, the game end's 08) and
 *               the race start's parameter; any other type too (the queue holds none).
 * The inspector's pause stops everything as well (nm_rumble_stop). The output is the larger of an active jolt's
 * power and the continuous level; the console sends it to each pack until that pack has it (code/drive_board.c).
 * Shared by code/drive_board.c and tests/test_rumble.c. */
#ifndef DAYTONA_NATIVE_RUMBLE_H
#define DAYTONA_NATIVE_RUMBLE_H
#include <stdint.h>

typedef struct {
    uint8_t jolt;       /* the active jolt's power, 0 none */
    uint8_t jolt_ticks; /* its iterations left */
    uint8_t level;      /* the continuous level */
    uint8_t power;      /* the output: max(jolt, level), 0..7 */
} nm_rumble_t;

static inline void nm_rumble_stop(nm_rumble_t *r) { r->jolt = r->jolt_ticks = r->level = r->power = 0; }

/* One main-loop iteration: BYTE the one the sender sent in it (0: none), JOLT_TICKS the iterations a jolt lasts
 * (the one it starts in included; 0: no jolts). The jolt running down first, then the byte. Returns the output. */
static inline unsigned nm_rumble_tick(nm_rumble_t *r, uint8_t byte, unsigned jolt_ticks) {
    if (r->jolt_ticks && !--r->jolt_ticks) r->jolt = 0;
    if (byte) {
        const unsigned type = byte >> 4, n = byte & 15u;
        if (type == 5 || type == 6) {
            if (jolt_ticks) { r->jolt = (uint8_t)(n > 7 ? 7 : n ? n : 1); r->jolt_ticks = (uint8_t)(jolt_ticks > 255 ? 255 : jolt_ticks); }
        } else if (type == 4) r->level = (uint8_t)(n > 7 ? 7 : n);
        else if (type >= 1 && type <= 3) r->level = 0;
        else r->jolt = r->jolt_ticks = r->level = 0;
    }
    r->power = r->jolt > r->level ? r->jolt : r->level;
    return r->power;
}
#endif
