#ifndef PC_ENDJINN_DC_SOUND_SFXMGR_H
#define PC_ENDJINN_DC_SOUND_SFXMGR_H

#include <pc_endjinn/types.h>

typedef int sfxhnd_t;
#define SFXHND_INVALID (-1)

PC_ENDJINN_BEGIN_DECLS
sfxhnd_t snd_sfx_load_raw_buf(void *samples, size_t size,
                              uint32_t sample_rate, uint8_t bits,
                              uint8_t channels);
void snd_sfx_unload(sfxhnd_t handle);
int snd_sfx_play(sfxhnd_t handle, uint8_t volume, uint8_t pan);
PC_ENDJINN_END_DECLS

#endif
