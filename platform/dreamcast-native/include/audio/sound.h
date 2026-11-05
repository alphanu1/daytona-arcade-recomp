/* Sound on the AICA through AICAflow (extern/aicaflow, platform/dreamcast-native/scripts/fetch_aicaflow.sh).
 *
 * Gameplay hands over the original program's packed three-byte sound commands:
 * the words 19bf4 queues for the sound board's UART (ae1005, be1a10, ...). This
 * module alone decides what each one plays. Music is the original board's
 * recorded output, prepared offline by recipes/build_music.sh as
 * ASSET_ROOT/music/tNN.afb (samples) + tNN.afx (control flow); which command
 * starts which recording is the table in audio/music_map.h. Every command also
 * goes to audio/sfx.h: the effects, voices and engine layers (ASSET_ROOT/sfx).
 *
 * One music flow is resident at a time. Starting another recording stops the
 * current one, waits until AICAflow has freed it (AICAflow recycles an instance
 * that ended by itself and reports it from its event thread; nm_sound_update
 * takes the report, a few frames), then loads and starts the new one. The load
 * is spread over frames: each idle wait for the PVR reads one chunk of the files,
 * as much as the wait has time for, and the bank's chunks go to AICA RAM by DMA
 * while the frame goes on, so no frame waits for the music (the recording starts once it is
 * resident; the game's own timing never waits for it). At a scene's loading pause
 * nm_sound_settle does all of that at once. Every call comes from the enDjinn
 * updater thread. Without AICAflow (start failure) or without the music files
 * the game stays silent. */
#ifndef NM_AUDIO_SOUND_H
#define NM_AUDIO_SOUND_H

#include <stdint.h>

/* Starts AICAflow with its firmware; asset_root ends in '/'. 0: silent from here on. */
int nm_sound_init(const char *asset_root);
/* One packed command, status byte in bits 23..16 (e.g. 0xae1005). */
void nm_sound_command(uint32_t command);
/* Once per updater call, in every mode, at its start. */
void nm_sound_update(void);
/* 1: a wanted recording loads (nm_sound_settle) but does not start until 0 (the attract's music with its cinematic). */
void nm_sound_hold(int on);
/* The music's share of enDjinn's idle wait for the PVR (code/audio/idle_wait.c): one step of a load (an open,
 * a read, the flow's upload) within budget_us. Returns the microseconds it took (0: nothing to do). */
uint32_t nm_sound_idle(uint32_t budget_us);
/* A recording is on its way into AICA RAM. */
int nm_sound_loading(void);
/* At a loading pause, after the scene's commands: finishes a pending change of
 * recording now (waits at most about two seconds). */
void nm_sound_settle(void);
/* Stops and releases everything, then AICAflow itself (waits up to about a second). */
void nm_sound_shutdown(void);

#endif
