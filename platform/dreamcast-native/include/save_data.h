#ifndef DAYTONA_NATIVE_SAVE_DATA_H
#define DAYTONA_NATIVE_SAVE_DATA_H
#include <stddef.h>
#include <stdint.h>
#include "race_records.h"
#include "settings.h"

/* What the original keeps between games, and its save file (code/save/save_data.c). Pure data: no allocation, no
 * device, no ROM data (the record defaults come from the generated race_record_data.h at run time).
 *
 * The original:
 *   - settings: the I/O board's EEPROM (include/settings.h);
 *   - records: backup RAM, three tables of 24 slots of 24 bytes (rank, time, view, flags, time copy, name;
 *     race_records.h), 1d00360, 1d005a0, 1d007e0. 226a5c picks them by the course byte 501460 & 3: 0 (Beginner)
 *     -> 1d00360, 1 (Expert) -> 1d005a0, 2 (Advanced) and 3 -> 1d007e0 (tables 23b544/23b554; nm_save_table).
 *     2265d0 resets them to the defaults 2266e0 (rank, name, flags) with the time base 2266d0[table] + 320 per
 *     rank (0x2800, 0x3c00, 0x3c00 at 1/64 s: 2:40.00, 4:00.00, 4:00.00 for the first place); the view word is
 *     left as the RAM holds it (erased: all ones, as the reference runs show);
 *   - selected: the slot pointer per course that 2268dc stores after an insertion (*5013b4: 1d00a20, 1d00a24,
 *     1d00a2c), here the slot number; NM_SAVE_NO_SLOT while the RAM is erased;
 *   - time attack (501a80 != 0): 16 words 1d00b60..1d00b9f, kept as they lie: +0x00 best lap automatic
 *     (courses 0, 2, 1, 3: 234a7c[course] = b60, b68, b64, b6c), +0x10 best race automatic (231e80: b70, b78, b74,
 *     b7c), +0x20 and +0x30 the same for manual (234a8c, 231e90). 3750 and aa0c lower them; 226654 resets the
 *     first three courses' words to the defaults below, course 3's stay erased. The native game has no time
 *     attack yet; the words are kept so the file needs no new version when it does.
 * Boot (5234): a signature mismatch at 1d00200 resets records and time-attack words (2265cc). Bookkeeping
 * (1d00100..) is the operator's and not kept. */
#define NM_SAVE_TABLES 3
#define NM_SAVE_SLOTS 24
#define NM_SAVE_NO_SLOT 0xffu
#define NM_SAVE_TIME_ATTACK_WORDS 16

typedef struct {
    nm_settings_t settings;
    uint8_t selected[NM_SAVE_TABLES];
    uint8_t zero[3];                     /* no padding: whole structures compare */
    nm_race_record_t records[NM_SAVE_TABLES][NM_SAVE_SLOTS];
    uint32_t time_attack[NM_SAVE_TIME_ATTACK_WORDS];
} nm_save_t;

/* 226a5c: the record table of a course byte. */
static inline unsigned nm_save_table(unsigned course) {
    course&=3u;
    return course<2 ? course : 2;
}

/* Factory settings and the original's reset records (2265cc) over erased memory. */
void nm_save_defaults(nm_save_t *save,const nm_race_record_seed_t seeds[NM_SAVE_TABLES][NM_SAVE_SLOTS]);
/* The ranks stay attached to their slots (rank i+1 in slot i), the selected slots and settings in range. */
int nm_save_valid(const nm_save_t *save);

/* The payload: little-endian words, a magic, a version and a CRC-32 (IEEE) over everything before it. */
#define NM_SAVE_PAYLOAD_BYTES 1812u
#define NM_SAVE_PAYLOAD_VERSION 1u
size_t nm_save_encode(const nm_save_t *save,uint8_t out[NM_SAVE_PAYLOAD_BYTES]);
/* 1 and *save filled when the payload is whole and valid; 0 leaves *save as it was. */
int nm_save_decode(const uint8_t *payload,size_t size,nm_save_t *save);

/* The VMS file around the payload (the Dreamcast's file header: descriptions, application id, one 32x32 icon of
 * 16 colours, CRC-16/XMODEM of the file with its CRC field zero, as KOS's vmu_pkg_build writes it). */
#define NM_SAVE_FILE_NAME "DAYTONA93NAT"
#define NM_SAVE_VMS_HEADER_BYTES 128u
#define NM_SAVE_ICON_BYTES 512u
#define NM_SAVE_FILE_BYTES (NM_SAVE_VMS_HEADER_BYTES+NM_SAVE_ICON_BYTES+NM_SAVE_PAYLOAD_BYTES)
#define NM_SAVE_FILE_BLOCKS ((NM_SAVE_FILE_BYTES+511u)/512u)
size_t nm_save_file_build(const nm_save_t *save,uint8_t out[NM_SAVE_FILE_BYTES]);
/* A file as read (whole blocks: size may exceed the file's own length). 1: *save filled; 0: not ours, damaged or
 * of another version, *save unchanged. */
int nm_save_file_parse(const uint8_t *file,size_t size,nm_save_t *save);

uint16_t nm_save_crc16(const uint8_t *data,size_t size);
uint32_t nm_save_crc32(const uint8_t *data,size_t size);
#endif
