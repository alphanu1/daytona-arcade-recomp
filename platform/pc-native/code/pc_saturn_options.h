/* The PC build's Circuit Edition options (Saturn Daytona USA Circuit Edition's Options and Sound pages; the menus
 * draw them in the original Saturn game's style, pc_saturn_menu.h). Kept in the SDL pref folder (options.cfg).
 *
 * CCE's rows and defaults (SLCT.BIN 060306FC; BKUP.BIN 0602B550 the new backup's values):
 *   handling P1, P2   SLOW / NORMAL / QUICK, NORMAL     game mode   NORMAL / GRANDPRIX / ENDURANCE, NORMAL
 *   difficulty        EASY / NORMAL / HARD, NORMAL      arcade time compare, section lap   OFF, OFF
 *   ghost type        A..F / OFF, A                     time attack time compare, section lap   ON, ON
 * Sound page: SOUND MODE STEREO / MONO; BGM VOLUME OFF, 1..7 (6); BGM SELECT a song per course; Key Config TYPE A..E. */
#ifndef DAYTONA_PC_SATURN_OPTIONS_H
#define DAYTONA_PC_SATURN_OPTIONS_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
enum { PC_HANDLING_SLOW, PC_HANDLING_NORMAL, PC_HANDLING_QUICK };
enum { PC_GAME_NORMAL, PC_GAME_GRANDPRIX, PC_GAME_ENDURANCE };
enum { PC_DIFFICULTY_EASY, PC_DIFFICULTY_NORMAL, PC_DIFFICULTY_HARD };
enum { PC_GHOST_OFF = 6 };
enum { PC_BGM_SONGS = 4 };      /* the arcade's race songs: THE KING OF SPEED, SKY HIGH, LET'S GO AWAY, DRY WAY */
enum { PC_SATURN_COURSES = 5 };

typedef struct {
    uint8_t handling[2], game_mode, difficulty;
    uint8_t arcade_compare, arcade_section, ghost, ta_compare, ta_section;
    uint8_t stereo, bgm_volume, bgm[PC_SATURN_COURSES], key_type[2];
    uint8_t last_mode;          /* the main menu's cursor: the last mode played (CCE 002FC233) */
} pc_saturn_options_t;

/* The options (loaded on first use; CCE's defaults without a file). */
const pc_saturn_options_t *pc_saturn_options(void);
/* Store new options and apply what takes effect at once (volume, mono, difficulty); saves the file. */
void pc_saturn_options_set(const pc_saturn_options_t *o);
/* The race music index (nm_music_race[] and its finish and podium tables, 0..3) of a course slot. */
unsigned pc_saturn_bgm(unsigned course);
/* GAME MODE in a race started from the menus (not the attract): its laps and its starting time from the
 * arcade's (include/race.h nm_race_settings). pc_saturn_race_mode: 1 for a race from the select, 0 otherwise. */
void pc_saturn_race_mode(int from_menu);
unsigned pc_saturn_race_laps(unsigned laps);
uint32_t pc_saturn_race_time(uint32_t ticks);
/* The steering response factor of HANDLING (pc_steering.cpp). */
float pc_saturn_handling_factor(void);
/* The song names of BGM SELECT and the course names, for the menus. */
const char *pc_saturn_bgm_name(unsigned song);
const char *pc_saturn_course_name(unsigned course);
#ifdef __cplusplus
}
#endif
#endif
