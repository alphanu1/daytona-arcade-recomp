/* The PC build's Circuit Edition options (pc_saturn_options.h). */
#include "pc_saturn_options.h"
#include "game_save.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pc_sound_options(float music_level, int mono); /* audio/pc_sound.cpp */

#define FILE_MAGIC "DUSAOPT1"
static pc_saturn_options_t options;
static int loaded;

static const pc_saturn_options_t defaults = {
    .handling = {PC_HANDLING_NORMAL, PC_HANDLING_NORMAL}, .game_mode = PC_GAME_NORMAL,
    .difficulty = PC_DIFFICULTY_NORMAL, .arcade_compare = 0, .arcade_section = 0, .ghost = 0,
    .ta_compare = 1, .ta_section = 1, .stereo = 1, .bgm_volume = 6,
    /* each course its own song: the arcade's by course slot (Beginner THE KING OF SPEED, Expert SKY HIGH,
     * Advanced LET'S GO AWAY), National Park LET'S GO AWAY, Desert City DRY WAY (CCE's own songs are not the
     * arcade's) */
    .bgm = {0, 1, 2, 2, 3}, .key_type = {0, 0}, .last_mode = 0};

static char *options_path(void) {
    char *dir = SDL_GetPrefPath("daytona", "Daytona USA");
    if (!dir) return NULL;
    size_t n = strlen(dir) + 16;
    char *path = malloc(n);
    if (path) snprintf(path, n, "%soptions.cfg", dir);
    SDL_free(dir);
    return path;
}

static int valid(const pc_saturn_options_t *o) {
    if (o->handling[0] > 2 || o->handling[1] > 2 || o->game_mode > 2 || o->difficulty > 2 || o->ghost > PC_GHOST_OFF ||
        o->arcade_compare > 1 || o->arcade_section > 1 || o->ta_compare > 1 || o->ta_section > 1 || o->stereo > 1 ||
        o->bgm_volume > 7 || o->key_type[0] > 4 || o->key_type[1] > 4 || o->last_mode > 5) return 0;
    for (unsigned c = 0; c < PC_SATURN_COURSES; ++c)
        if (o->bgm[c] >= PC_BGM_SONGS) return 0;
    return 1;
}

static void apply(void) {
    pc_sound_options((float)(options.bgm_volume * 0x20) / 255.0f, !options.stereo);   /* CCE 06011A76: 0x00..0xE0 */
    /* DIFFICULTY: the arcade's own setting (EASY, NORMAL, HARD of its four), saved with the records. */
    const nm_settings_t *current = nm_game_settings();
    if (current && current->difficulty != options.difficulty) {
        nm_settings_t s = *current;
        s.difficulty = options.difficulty;
        if (nm_game_settings_set(&s)) nm_game_save_request();
    }
}

const pc_saturn_options_t *pc_saturn_options(void) {
    if (!loaded) {
        loaded = 1;
        options = defaults;
        char *path = options_path();
        FILE *f = path ? fopen(path, "rb") : NULL;
        if (f) {
            char magic[8];
            pc_saturn_options_t o;
            if (fread(magic, 1, 8, f) == 8 && !memcmp(magic, FILE_MAGIC, 8) && fread(&o, 1, sizeof(o), f) == sizeof(o) &&
                valid(&o))
                options = o;
            fclose(f);
        }
        free(path);
        const nm_settings_t *s = nm_game_settings();
        if (s && s->difficulty <= PC_DIFFICULTY_HARD) options.difficulty = s->difficulty;
        apply();
    }
    return &options;
}

void pc_saturn_options_set(const pc_saturn_options_t *o) {
    if (!o || !valid(o)) return;
    (void)pc_saturn_options();
    options = *o;
    apply();
    char *path = options_path();
    FILE *f = path ? fopen(path, "wb") : NULL;
    if (f) {
        fwrite(FILE_MAGIC, 1, 8, f);
        fwrite(&options, 1, sizeof(options), f);
        fclose(f);
    }
    free(path);
}

unsigned pc_saturn_bgm(unsigned course) {
    const pc_saturn_options_t *o = pc_saturn_options();
    return course < PC_SATURN_COURSES ? o->bgm[course] : (course & 3u);
}

float pc_saturn_handling_factor(void) {
    /* CCE RACE 060370D4: a pad's steering closes 1/22, 1/16, 1/10 of the gap to its target a tick (SLOW, NORMAL,
     * QUICK): against NORMAL, 16/22 and 16/10. */
    static const float factor[3] = {16.0f / 22.0f, 1.0f, 1.6f};
    return factor[pc_saturn_options()->handling[0]];
}

static int race_from_menu;
void pc_saturn_race_mode(int from_menu) { race_from_menu = from_menu; }

unsigned pc_saturn_race_laps(unsigned laps) {
    /* CCE RACE 0604EF2B: NORMAL 8, 4, 2, 6, 4 (the arcade's), GRANDPRIX 20, 10, 5, 15, 10, ENDURANCE 80, 40, 20, 60, 40. */
    static const unsigned times4[3] = {4, 10, 40};
    return race_from_menu ? laps * times4[pc_saturn_options()->game_mode] / 4u : laps;
}

uint32_t pc_saturn_race_time(uint32_t ticks) {
    /* CCE RACE 0604EF4E: the starting time against NORMAL's, about 2.15 (GRANDPRIX) and 7.9 (ENDURANCE) times. */
    static const uint32_t per100[3] = {100, 215, 790};
    return race_from_menu ? (uint32_t)((uint64_t)ticks * per100[pc_saturn_options()->game_mode] / 100u) : ticks;
}

const char *pc_saturn_bgm_name(unsigned song) {
    static const char *const names[PC_BGM_SONGS] = {"THE KING OF SPEED", "SKY HIGH", "LET'S GO AWAY", "DRY WAY"};
    return song < PC_BGM_SONGS ? names[song] : "?";
}

const char *pc_saturn_course_name(unsigned course) {
    static const char *const names[PC_SATURN_COURSES] = {"THREE SEVEN SPEEDWAY", "SEA-SIDE STREET GALAXY",
        "DINOSAUR CANYON", "NATIONAL PARK SPEEDWAY", "DESERT CITY"};
    return course < PC_SATURN_COURSES ? names[course] : "?";
}
