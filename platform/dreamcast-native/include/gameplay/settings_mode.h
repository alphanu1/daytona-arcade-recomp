#ifndef DAYTONA_NATIVE_GAMEPLAY_SETTINGS_MODE_H
#define DAYTONA_NATIVE_GAMEPLAY_SETTINGS_MODE_H

/* The settings screen until a real options menu exists (code/gameplay/settings_mode.c): what the
 * original's test mode GAME ASSIGNMENTS offers a single-player game (include/settings.h), the records' reset
 * (BACKUP DATA CLEAR without the bookkeeping) and the VMU's state. From the title: both triggers held and START.
 * Up/down select, left/right (or A) change, START or EXIT saves to the VMU and returns to the title, B returns
 * without changing anything. */
int nm_settings_mode_start(void);

#endif
