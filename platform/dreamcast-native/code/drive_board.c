// The console's drive board stream (include/drive_board.h) and its rumble pack player (include/rumble.h). The queue
// lives here from power-on: the scenes push their phase bytes and run one send an iteration (nm_drive_stream_ticks),
// the race its update through nm_race_scene_t.drive (race_scene.h). Each sent byte goes to the player; its power goes
// to every controller port with a pack (enDjinn's list) until that pack has taken it. NATIVE_RUMBLE=0 (local.cfg.mk)
// builds the stream without the player. Outside the LTO unit (Makefile): the calls stay calls, the race's code its own.
#include <enDjinn/enj_enDjinn.h>
#include <kos/thread.h>
#include <stdio.h>
#include "drive_board.h"
#include "rumble.h"
#ifndef NM_RUMBLE
#define NM_RUMBLE 1
#endif
#ifndef NM_RUMBLE_JOLT_TICKS
#define NM_RUMBLE_JOLT_TICKS 9
#endif
#ifndef NM_RUMBLE_FREQ
#define NM_RUMBLE_FREQ 26
#endif
_Static_assert(NM_RUMBLE_JOLT_TICKS >= 1 && NM_RUMBLE_JOLT_TICKS <= 255, "NATIVE_RUMBLE_JOLT_TICKS: 1..255");
_Static_assert(NM_RUMBLE_FREQ >= 1 && NM_RUMBLE_FREQ <= 255, "NATIVE_RUMBLE_FREQ: 1..255");

/* What this file adds outside .text moves the statics unless it sits behind them: .rodata ends where .data starts,
 * and the large statics' O-cache sets move with both (docs/NEXT_LLM_HANDOFF.md, "Where the statics sit"). So no
 * initialised data, no .rodata (the end line's text in .text, no switch tables: Makefile), and this file's .bss last
 * in .bss, in the padding before the heap (config/hot_layout.tsv @bss_last). */
#define NM_DRIVE_TEXT __attribute__((section(".text.nm_drive_stream_text")))
#define NM_FNV_BASIS UINT32_C(2166136261)
alignas(32) static nm_drive_board_t queue; /* zero at power-on, as the original's work RAM */
static uint32_t sent, sent_fnv; /* the sent bytes' FNV-1a, xor its offset basis (zero-initialised) */
#if NM_RUMBLE
static nm_rumble_t rumble;
static uint8_t given[MAPLE_PORT_COUNT]; /* the power each port's pack last took (0 from power-on: they stand) */
static uint32_t commands, refused;

/* cont 1, motor 1, forward power, the build's frequency, no inclination; 0: the stop word (motor 1 only). */
static uint32_t effect(unsigned power) {
    purupuru_effect_t e = {.raw = 0};
    e.motor = 1;
    if (power) { e.cont = 1; e.fpow = power; e.freq = NM_RUMBLE_FREQ; e.inc = 0; }
    return e.raw;
}
/* KOS's purupuru_rumble_raw queues the frame for the next vblank and refuses (MAPLE_EAGAIN) while the pack's previous
 * frame is unanswered; enDjinn's own queue drops a refused effect, which could leave a pack running after a stop. So
 * each pack keeps what it took, and the next call tries again. A port without a pack counts as standing: a pack
 * plugged in later gets the power then. */
static void play(unsigned power) {
    maple_device_t **packs = enj_rumble_states_get();
    for (size_t port = 0; port < enj_rumble_states_length() && port < MAPLE_PORT_COUNT; ++port) {
        if (!packs[port]) { given[port] = 0; continue; }
        if (given[port] == power) continue;
        if (purupuru_rumble_raw(packs[port], effect(power)) == MAPLE_EOK) { given[port] = (uint8_t)power; ++commands; }
        else ++refused;
    }
}
static int packs_running(void) {
    maple_device_t **packs = enj_rumble_states_get();
    for (size_t port = 0; port < enj_rumble_states_length() && port < MAPLE_PORT_COUNT; ++port)
        if (packs[port] && given[port]) return 1;
    return 0;
}
#endif

nm_drive_board_t *nm_drive_stream(void) { return &queue; }

void nm_drive_stream_phase(unsigned phase) { nm_drive_board_phase(&queue, phase); }

void nm_drive_stream_ticks(unsigned count) {
    while (count--) {
        uint8_t c = 0;
        if (nm_drive_board_send(&queue, &c)) {
            ++sent;
            sent_fnv = (((sent_fnv ^ NM_FNV_BASIS) ^ c) * UINT32_C(16777619)) ^ NM_FNV_BASIS;
        }
#if NM_RUMBLE
        nm_rumble_tick(&rumble, c, NM_RUMBLE_JOLT_TICKS);
#endif
    }
#if NM_RUMBLE
    play(rumble.power);
#endif
}

void nm_drive_stream_pause(void) {
#if NM_RUMBLE
    nm_rumble_stop(&rumble);
    play(0);
#endif
}

#if NM_RUMBLE
static const char end_line[] NM_DRIVE_TEXT = "Native drive board: %lu bytes sent (FNV-1a %08lx); rumble pack commands "
    "%lu, %lu refused and tried again (jolt %u ticks, frequency %u)%s\n";
static const char end_stuck[] NM_DRIVE_TEXT = "; a pack did not take the stop";
#else
static const char end_line[] NM_DRIVE_TEXT = "Native drive board: %lu bytes sent (FNV-1a %08lx); no rumble player "
    "(NATIVE_RUMBLE=0)\n";
#endif

void nm_drive_stream_end(void) {
#if NM_RUMBLE
    /* The stop, until every pack has taken it (a frame is answered within a vblank or two; 10 tries 200 ms). */
    nm_rumble_stop(&rumble);
    for (int tries = 0; tries < 10; ++tries) {
        play(0);
        if (!packs_running()) break;
        thd_sleep(20);
    }
    printf(end_line, (unsigned long)sent, (unsigned long)(sent_fnv ^ NM_FNV_BASIS), (unsigned long)commands,
        (unsigned long)refused, (unsigned)NM_RUMBLE_JOLT_TICKS, (unsigned)NM_RUMBLE_FREQ,
        packs_running() ? end_stuck : end_stuck + sizeof end_stuck - 1);
#else
    printf(end_line, (unsigned long)sent, (unsigned long)(sent_fnv ^ NM_FNV_BASIS));
#endif
}
