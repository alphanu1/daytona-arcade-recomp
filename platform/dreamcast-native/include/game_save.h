#ifndef DAYTONA_NATIVE_GAME_SAVE_H
#define DAYTONA_NATIVE_GAME_SAVE_H
#include "save_store.h"

/* What the game keeps between games (code/save/game_save.c, outside the race's LTO unit): the operator settings
 * the original keeps in its EEPROM and the records it keeps in backup RAM (include/save_data.h), on the VMU.
 *   - nm_game_save_boot, once before the title: the defaults (factory settings, the original's reset records),
 *     then the VMU's save when there is one (blocking, before the first frame). use_card 0 (scripted races):
 *     defaults only, and nothing is ever written.
 *   - The race reads the difficulty when it starts (code/practice.c), the title the advertise sound.
 *   - The race's end inserts into nm_game_records(course) (race_records.h) and reports the slot; the name entry
 *     writes the name into that slot; then nm_game_save_request (gameplay/name_mode.c) writes in the background
 *     (a KOS thread: the game loop never waits for the VMU). No VMU, a full VMU: nothing written, the next request
 *     tries again with everything since the last write. */
void nm_game_save_boot(const nm_race_record_seed_t seeds[NM_SAVE_TABLES][NM_SAVE_SLOTS],int use_card);
const nm_settings_t *nm_game_settings(void);
int nm_game_settings_set(const nm_settings_t *settings);
nm_race_record_t *nm_game_records(unsigned course);
void nm_game_record_inserted(unsigned course,unsigned slot);
/* The original's record reset (2265cc, test mode BACKUP DATA CLEAR without the bookkeeping): the next write
 * replaces the card's records too. */
void nm_game_records_clear(void);
void nm_game_save_request(void);

typedef struct {
    nm_save_result_t boot, last; /* the boot load; the last write (NM_SAVE_NO_FILE: none yet) */
    int busy;                    /* a write is running or waiting */
} nm_game_save_status_t;
nm_game_save_status_t nm_game_save_status(void);
#ifndef _arch_dreamcast
/* Hosts (tests/test_game_save.c): the card the writer, which runs inside the request there, uses; NULL none. */
void nm_game_save_device_set(const nm_save_device_t *card);
#endif
#endif
