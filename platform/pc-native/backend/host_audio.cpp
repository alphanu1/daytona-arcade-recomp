/* KOS sfxmgr entry points for enDjinn's enj_sound.c.  Daytona never plays
 * through them: its audio is the arcade sound board's own program and
 * samples (platform/pc-native/audio/pc_sound.cpp, the recomp's
 * NativeSoundEngine), so these stay inert on PC. */
#include "host_audio.h"

#include <kos.h>

namespace enj_host {
void audio_shutdown() {}
}  // namespace enj_host

extern "C" {

void snd_init(void) {}

sfxhnd_t snd_sfx_load_raw_buf(void *, size_t, uint32_t, uint8_t, uint8_t)
{
    return SFXHND_INVALID;
}

void snd_sfx_unload(sfxhnd_t) {}

int snd_sfx_play(sfxhnd_t, uint8_t, uint8_t)
{
    return -1;
}

}  // extern "C"
