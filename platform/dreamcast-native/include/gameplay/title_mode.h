#ifndef DAYTONA_NATIVE_GAMEPLAY_TITLE_MODE_H
#define DAYTONA_NATIVE_GAMEPLAY_TITLE_MODE_H

/* Replace the current enDjinn mode with the resident no-credit title (the original's phase 9). */
int nm_title_mode_start(void);
/* The original's phase 2: the attract loop's entry (boot, each loop, after a game), then its cinematic. */
int nm_attract_mode_start(void);

#endif
