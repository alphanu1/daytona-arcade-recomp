#ifndef DAYTONA_NATIVE_SETTINGS_H
#define DAYTONA_NATIVE_SETTINGS_H
#include <stdint.h>

/* The operator settings of the original that matter to a single-player game (test mode > GAME ASSIGNMENTS;
 * the I/O board's EEPROM, published by 227cf0, tools/settings_eeprom.py has the layout). Values are the
 * original's, the defaults its factory image 23c150:
 *   difficulty       EEPROM +20 -> 53e59c: 0 EASY, 1 NORMAL (factory), 2 HARD, 3 HARDEST. The start time and
 *                    every checkpoint award (3860: 2870304[course][difficulty]; 13574/135a4 the adjustments):
 *                    nm_race_settings and the award input of the player tick take it.
 *   advertise_sound  EEPROM +19 -> 53e5fe: 0 ON (factory), 1 OFF. 1ca8, at the entry of phase 2 (the title):
 *                    ON sends ae1008 (the advertisement music), OFF be1a10 (the music fade).
 * Not here (presentation the native art does not cover; the factory values are what the port shows):
 * COUNTRY +1b (JPN), CABINET +1a (DELUXE). Coins (+10, +15): the native game has none. The race length is no
 * setting in this revision: the laps are program table 03950[course]. */
enum { NM_DIFFICULTY_EASY, NM_DIFFICULTY_NORMAL, NM_DIFFICULTY_HARD, NM_DIFFICULTY_HARDEST, NM_DIFFICULTY_COUNT };
enum { NM_ADVERTISE_ON, NM_ADVERTISE_OFF, NM_ADVERTISE_COUNT };

typedef struct {
    uint8_t difficulty, advertise_sound;
} nm_settings_t;

static inline nm_settings_t nm_settings_factory(void) {
    return (nm_settings_t){.difficulty=NM_DIFFICULTY_NORMAL,.advertise_sound=NM_ADVERTISE_ON};
}

static inline int nm_settings_valid(const nm_settings_t *s) {
    return s && s->difficulty<NM_DIFFICULTY_COUNT && s->advertise_sound<NM_ADVERTISE_COUNT;
}

/* The names of the original's test mode (23c710, 23c6e0), trailing blanks dropped. */
static inline const char *nm_settings_difficulty_name(unsigned difficulty) {
    static const char *const names[NM_DIFFICULTY_COUNT]={"EASY","NORMAL","HARD","HARDEST"};
    return difficulty<NM_DIFFICULTY_COUNT ? names[difficulty] : "?";
}
static inline const char *nm_settings_advertise_name(unsigned advertise) {
    return advertise==NM_ADVERTISE_ON ? "ON" : advertise==NM_ADVERTISE_OFF ? "OFF" : "?";
}

/* A difficulty by its name, either case (tools: trace_race_native --difficulty); -1 when none. */
static inline int nm_settings_difficulty_parse(const char *name) {
    for(unsigned d=0;name && d<NM_DIFFICULTY_COUNT;++d) {
        const char *a=name,*b=nm_settings_difficulty_name(d);
        while(*a && *b && (*a|0x20)==(*b|0x20)) { ++a;++b; }
        if(!*a && !*b) return (int)d;
    }
    return -1;
}
#endif
