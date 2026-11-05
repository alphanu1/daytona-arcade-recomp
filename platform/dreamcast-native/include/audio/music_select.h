/* The original program's music commands outside the result and name owners.
 * Sites and conditions from the sound-command trace of 15 October 2026
 * (build/dc-native-sound-commands.BTQx3Q: m2segment --trace-sound plus the
 * program's enqueue sites); what each command plays: audio/music_map.h.
 *
 * Music choice: 30c0, at the start of the rolling start (phase 19), stores at
 * 501a94: VR1 0, else VR2 2, else VR3 1, else VR4 3; with no VR button held,
 * the course (501460). The race, finish and podium tables are indexed by it.
 * The native race has no VR buttons yet, so the choice is the course. Not
 * reproduced: choice 2 without START held also sends b70700 (channel 7 of the
 * music silent); a recording cannot drop one channel. */
#ifndef NM_AUDIO_MUSIC_SELECT_H
#define NM_AUDIO_MUSIC_SELECT_H

#include <stdint.h>

/* 1cb8 (phase 2 handler 197c): every entry to phase 2 (boot, each attract
 * loop, after the name entry), with ADVERTISE SOUND on; off sends be1a10. */
#define NM_MUSIC_ADVERTISE UINT32_C(0xae1008)
/* 2850 (phase 12): the credit starts a game. */
#define NM_MUSIC_GAME_START UINT32_C(0xae1001)
/* Music fade, interval 16: sent by 3070 just before the race music. */
#define NM_MUSIC_FADE UINT32_C(0xbe1a10)
/* 30c8 (phase 19): the race, after the fade. */
static const uint32_t nm_music_race[4] = {0xae1007, 0xae1009, 0xae1004, 0xae100e};      /* 231e70 */
/* 44b0 (object 44a8 of the phase 23 handler): the player crosses the line,
 * after the handler's own commands. Not sent on time over (22 -> 25). */
static const uint32_t nm_music_finish[4] = {0xae1006, 0xae100a, 0xae1003, 0xae1011};    /* 231e38 */
/* b9e8 (object b948) while phase 27 (the podium) is active, then ae211c. */
static const uint32_t nm_music_podium[4] = {0xae100c, 0xae100d, 0xae100b, 0xae1012};    /* 2344ec */
#define NM_VOICE_YOU_MADE_IT UINT32_C(0xae211c)

#endif
