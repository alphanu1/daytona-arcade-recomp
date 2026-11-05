/* Which original sequence start plays which music recording.
 *
 * command: the packed sequence start the original program sends. Every music
 *   start is ae10xx (channel 14, bank 0x10); bank 0x10's entries are all
 *   playlists on the music track, banks 0x20..0x22 are effects and voices.
 * track: recipes/build_music.sh's tNN, the vgmrips pack's numbering.
 * loops: the recording has a loop section (pack notes: the four race pieces).
 *   AICAflow flows end, so such a flow starts again from its beginning, which
 *   repeats the intro the original skips on later passes.
 *
 * Evidence (15 October 2026, build/dc-native-music-match.IpgWyB): the native
 * sequencer (src/runtime/native_sound_sequencer.*) given the game's boot bytes
 * and then each ae1000..ae102f, with Revision A's sound program (epr-16720/16721,
 * in your own daytona93 set), against the MultiPCM key-ons of each VGM log:
 * every entry below reproduces its recording's first 60 s of notes (sample,
 * pitch) at multiset similarity 1.000 (0.999 for ae1007, ae100d: one note) and
 * in order for the first 400; no other recording scores above 0.52. ae1000,
 * ae100f and ae101f start no music. The names in comments are the program's own
 * sound-test labels (23d150). */
#ifndef NM_AUDIO_MUSIC_MAP_H
#define NM_AUDIO_MUSIC_MAP_H

#include <stdint.h>

typedef struct { uint32_t command; uint8_t track, loops; } nm_music_entry_t;

static const nm_music_entry_t nm_music_map[] = {
    {0xae1001, 2, 0},  /* START YOUR ENGINES */
    {0xae1002, 15, 0}, /* G A M E O V E R */
    {0xae1003, 8, 0},  /* BITCHING */
    {0xae1004, 4, 1},  /* LET'S GO AWAY[DAYTONA] */
    {0xae1005, 16, 0}, /* GO TO THE VICTORY LANE */
    {0xae1006, 7, 0},  /* BREAK DOWN */
    {0xae1007, 3, 1},  /* THE KING OF SPEED */
    {0xae1008, 1, 0},  /* LET'S GO AWAY[ADV] */
    {0xae1009, 5, 1},  /* SKY HIGH */
    {0xae100a, 9, 0},  /* CHECK IT UP! */
    {0xae100b, 12, 0}, /* DREAM OF THE DAYTONA */
    {0xae100c, 11, 0}, /* ROLLING STARTED */
    {0xae100d, 13, 0}, /* CAN YOU FLY? */
    {0xae100e, 6, 1},  /* DRY WAY */
    {0xae1010, 17, 0}, {0xae1011, 10, 0}, {0xae1012, 14, 0}, {0xae1013, 18, 0}, {0xae1014, 19, 0},
    {0xae1015, 20, 0}, {0xae1016, 21, 0}, {0xae1017, 22, 0}, {0xae1018, 23, 0}, {0xae1019, 24, 0},
    {0xae101a, 25, 0}, {0xae101b, 26, 0}, {0xae101c, 27, 0}, {0xae101d, 28, 0}, {0xae101e, 29, 0},
    {0xae1020, 30, 0}, {0xae1021, 31, 0}, {0xae1022, 32, 0}, {0xae1023, 33, 0}, {0xae1024, 34, 0},
    {0xae1025, 35, 0}, {0xae1026, 40, 0}, {0xae1027, 36, 0}, {0xae1028, 37, 0}, {0xae1029, 38, 0},
    {0xae102a, 39, 0}, /* ae1010..ae102a: other games' pieces (name-entry initials, 23be24) */
    {0xae102b, 45, 0}, {0xae102c, 44, 0}, {0xae102d, 42, 0}, {0xae102e, 41, 0}, {0xae102f, 43, 0},
    /* ae102b..ae102f: no site or table of the program sends them */
};

static inline const nm_music_entry_t *nm_music_find(uint32_t command) {
    for (unsigned i = 0; i < sizeof(nm_music_map) / sizeof(nm_music_map[0]); ++i)
        if (nm_music_map[i].track && nm_music_map[i].command == command) return &nm_music_map[i];
    return 0;
}

#endif
