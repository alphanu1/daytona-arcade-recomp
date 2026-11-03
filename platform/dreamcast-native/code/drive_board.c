// The console's drive board stream (include/drive_board.h) and its rumble pack player (include/rumble.h). The queue
// lives here from power-on: the scenes push their phase bytes and run one send an iteration (nm_drive_stream_ticks),
// the race its update through nm_race_scene_t.drive (race_scene.h). Each sent byte goes to the player; its power goes
// to every controller port with a pack, through enDjinn, only when it changes. NATIVE_RUMBLE=0 (local.cfg.mk) builds
// the stream without the player. Outside the LTO unit (Makefile): the calls stay calls, the race's code its own.
#include <enDjinn/enj_enDjinn.h>
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

alignas(32) static nm_drive_board_t queue; /* zero at power-on, as the original's work RAM */
static uint32_t sent, sent_hash = UINT32_C(2166136261);
#if NM_RUMBLE
static nm_rumble_t rumble;
static unsigned output; /* the power the packs were last given (0 from power-on: they stand) */
static uint32_t commands;

/* cont 1, motor 1, forward power, the build's frequency, no inclination; 0: the stop word (motor 1 only). */
static uint32_t effect(unsigned power) {
    purupuru_effect_t e = {.raw = 0};
    e.motor = 1;
    if (power) { e.cont = 1; e.fpow = power; e.freq = NM_RUMBLE_FREQ; e.inc = 0; }
    return e.raw;
}
static void play(unsigned power) {
    if (power == output) return;
    output = power;
    maple_device_t **packs = enj_rumble_states_get();
    for (size_t port = 0; port < enj_rumble_states_length(); ++port)
        if (packs[port] && (enj_rumble_effect_set_raw((enj_ctrl_port_name_e)port, effect(power)) & ENJ_RUMBLE_SET)) ++commands;
}
#endif

nm_drive_board_t *nm_drive_stream(void) { return &queue; }

void nm_drive_stream_phase(unsigned phase) { nm_drive_board_phase(&queue, phase); }

void nm_drive_stream_ticks(unsigned count) {
    while (count--) {
        uint8_t c = 0;
        if (nm_drive_board_send(&queue, &c)) { ++sent; sent_hash = (sent_hash ^ c) * UINT32_C(16777619); }
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

void nm_drive_stream_end(void) {
#if NM_RUMBLE
    /* enDjinn's loop has ended, so its rumble update no longer sends a pending effect: the stop goes to the packs. */
    nm_rumble_stop(&rumble);
    maple_device_t **packs = enj_rumble_states_get();
    if (output) for (size_t port = 0; port < enj_rumble_states_length(); ++port)
        if (packs[port]) (void)purupuru_rumble_raw(packs[port], effect(0));
    output = 0;
    printf("Native drive board: %lu bytes sent (FNV-1a %08lx); rumble pack commands %lu (jolt %u ticks, frequency %u)\n",
        (unsigned long)sent, (unsigned long)sent_hash, (unsigned long)commands, (unsigned)NM_RUMBLE_JOLT_TICKS,
        (unsigned)NM_RUMBLE_FREQ);
#else
    printf("Native drive board: %lu bytes sent (FNV-1a %08lx); no rumble player (NATIVE_RUMBLE=0)\n",
        (unsigned long)sent, (unsigned long)sent_hash);
#endif
}
