#ifndef DAYTONA_NATIVE_SAVE_STORE_H
#define DAYTONA_NATIVE_SAVE_STORE_H
#include "save_data.h"

/* Loading and writing the save file through a memory card's operations (code/save/save_store.c): the VMU through
 * KOS's vmufs on the console (code/save/game_save.c), a fake card in tests/test_game_save.c. Blocking; the game
 * calls the writer from a worker thread, never from the race. */
typedef struct {
    void *context;
    /* The card to use, NULL when there is none. */
    void *(*find)(void *context);
    /* Blocks free for files, <0 on error. */
    int (*free_blocks)(void *context,void *card);
    /* The named file's size in blocks: 0 when absent, <0 on error. */
    int (*file_blocks)(void *context,void *card,const char *name);
    /* The whole named file (whole blocks) in a buffer the caller hands back to release: 0, or <0. */
    int (*read)(void *context,void *card,const char *name,uint8_t **data,size_t *size);
    void (*release)(void *context,uint8_t *data);
    /* Replace the named file: 0, or <0. */
    int (*write)(void *context,void *card,const char *name,const uint8_t *data,size_t size);
} nm_save_device_t;

typedef enum {
    NM_SAVE_OK,          /* loaded / written */
    NM_SAVE_NO_CARD,     /* no memory card */
    NM_SAVE_NO_FILE,     /* load: the card has no save of this game (defaults kept) */
    NM_SAVE_DAMAGED,     /* load: the file is not whole, of another version or not ours (defaults kept; the next
                          * save replaces it) */
    NM_SAVE_FULL,        /* write: not enough free blocks (nothing written, the card unchanged) */
    NM_SAVE_IO_ERROR,    /* the card did not answer as it should */
} nm_save_result_t;
const char *nm_save_result_name(nm_save_result_t result);

/* *save keeps what it held (the defaults) unless the result is NM_SAVE_OK. */
nm_save_result_t nm_save_store_load(const nm_save_device_t *device,nm_save_t *save);

/* A record the game inserted since the last write: its table and the record as it is now (with the name entered). */
typedef struct {
    uint8_t table;
    nm_race_record_t record;
} nm_save_insert_t;

/* 2268dc/2269d0's insertion of a whole record into a table (the ranks stay with the slots, an equal time goes
 * before the old one, slower than slot 23: not inserted). Returns the slot, or NM_SAVE_NO_SLOT. */
unsigned nm_save_insert(nm_race_record_t table[NM_SAVE_SLOTS],const nm_race_record_t *record);

/* Write `snapshot` (the game's state, nm_save_file_build) unless `merge` and the card already holds a valid save of
 * this game: then that save with the `inserts` (in order) and `settings`, so that a card put in after the boot, or
 * another card, keeps its own records and gets this session's. Without `merge` (the records were cleared) the
 * snapshot replaces it. A damaged file is replaced. scratch: NM_SAVE_FILE_BLOCKS*512 bytes. */
nm_save_result_t nm_save_store_write(const nm_save_device_t *device,const uint8_t *snapshot,
        const nm_save_insert_t *inserts,unsigned count,const nm_settings_t *settings,int merge,uint8_t *scratch);
#endif
