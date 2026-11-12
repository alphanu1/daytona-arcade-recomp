/* The Saturn version's secret horse (pc_horse.h). */
#include "pc_horse.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pc_sound_horse_hoof(unsigned sound); /* audio/pc_sound.cpp: the Saturn's hoof sound 0..2 (effects 3b..3d) */

/* Saturn 0604633c (automatic) and 060461cc (manual), the first 67 entries (16.16), the arcade curve's length:
 * read from ASSET_SET/horse-drive.bin (tools/extract_saturn_horse.py, from your Saturn disc's APROG.BIN); without it
 * the horse keeps the Hornet's curve. */
enum { HORSE_TORQUE_ENTRIES = 67 };
static int32_t horse_torque[2][HORSE_TORQUE_ENTRIES];
static int horse_torque_state;   /* 0 not read yet, 1 read, -1 none */
const char *nm_practice_root(void);

static int horse_torque_load(void) {
    if (horse_torque_state) return horse_torque_state > 0;
    horse_torque_state = -1;
    const char *root = nm_practice_root();
    char path[512];
    if (!root || snprintf(path, sizeof(path), "%shorse-drive.bin", root) >= (int)sizeof(path)) return 0;
    FILE *f = fopen(path, "rb");
    uint8_t raw[4 + 8 * HORSE_TORQUE_ENTRIES];
    const int ok = f && fread(raw, 1, sizeof(raw), f) == sizeof(raw) && !memcmp(raw, "HRS1", 4);
    if (f) fclose(f);
    if (!ok) {
        puts("PC horse: no horse-drive.bin (extract_saturn_horse.py with your Saturn files): the Hornet's torque curve");
        return 0;
    }
    for (unsigned k = 0; k < 2u * HORSE_TORQUE_ENTRIES; ++k) {
        const uint8_t *b = raw + 4 + 4 * k;
        horse_torque[k / HORSE_TORQUE_ENTRIES][k % HORSE_TORQUE_ENTRIES] =
            (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
    }
    horse_torque_state = 1;
    return 1;
}
/* The grip floor bytes +7e/+7f (front, rear): the horse's over the Hornet's. */
static const float horse_grip[2] = {64.0f / 7.0f, 32.0f / 5.0f};
#define HORSE_WALL (0.75f / 0.87f)
/* The gallop: the Saturn's phase step speed / 300 a tick of six frames, at its 20 Hz game tick (taken as such);
 * here per 59.94 Hz tick over 32 frames, the same strides a second. */
#define SATURN_TICK_HZ 20.0f
#define PC_TICK_HZ (60000.0f / 1001.0f)
#define FRAMES 32.0f

static int loaded, enabled;
static int riding;
static unsigned transmission;
static float phase;
static int saturn_frame = -1;
static nm_drive_params_t *horse_math;
static const nm_drive_params_t *horse_base;

static char *settings_path(void) {
    char *dir = SDL_GetPrefPath("daytona", "Daytona USA");
    if (!dir) return NULL;
    size_t n = strlen(dir) + 16;
    char *path = malloc(n);
    if (path) snprintf(path, n, "%shorse.cfg", dir);
    SDL_free(dir);
    return path;
}

int pc_horse_enabled(void) {
    if (!loaded) {
        loaded = 1;
        char *path = settings_path();
        FILE *f = path ? fopen(path, "rb") : NULL;
        if (f) {
            enabled = fgetc(f) == '1';
            fclose(f);
        }
        free(path);
    }
    return enabled;
}

int pc_horse_title_code(uint32_t buttons) {
    /* enDjinn's two-bit states: bit 0 held, 3 pressed this frame. A 0, B 2, X 4, Y 6, UP 8, LEFT 12, START 16. */
    const unsigned held = 0x1155u;  /* A, B, X, Y, UP, LEFT held */
    if (((buttons >> 16) & 3u) != 3u || (buttons & held) != held) return 0;
    enabled = !pc_horse_enabled();
    char *path = settings_path();
    FILE *f = path ? fopen(path, "wb") : NULL;
    if (f) {
        fputc(enabled ? '1' : '0', f);
        fclose(f);
    }
    free(path);
    printf("PC horse: %s (the Saturn's code at the title)\n", enabled ? "on: automatic rides the brown horse, manual the grey" : "off");
    if (enabled) pc_sound_horse_hoof(2);
    return 1;
}

int pc_horse_chord_held(uint32_t buttons) {
    return (buttons & 0x1100u) == 0x1100u;   /* UP and LEFT held: A starts the chord, not the game */
}

void pc_horse_race_begin(unsigned t) {
    riding = pc_horse_enabled();
    transmission = t & 1u;
    /* Temporary debug: DAYTONA_HORSE=brown (or 1) / grey rides that horse whatever the setting and transmission. */
    const char *force = getenv("DAYTONA_HORSE");
    if (force && *force) {
        riding = force[0] != '0';
        if (force[0] == 'b' || force[0] == 'B' || force[0] == '1') transmission = 0;
        else if (force[0] == 'g' || force[0] == 'G') transmission = 1;
    }
    phase = 0.0f;
    saturn_frame = -1;
    if (riding) printf("PC horse: this race on the %s horse\n", transmission ? "grey (manual)" : "brown (automatic)");
}

void pc_horse_race_end(void) { riding = 0; }
int pc_horse_riding(void) { return riding; }
unsigned pc_horse_transmission(void) { return transmission; }

void pc_horse_tick(int32_t speed_kmh) {
    if (!riding) return;
    const float speed = speed_kmh > 0 ? (float)speed_kmh : 0.0f;
    phase += speed / 300.0f * (SATURN_TICK_HZ / 6.0f) * (FRAMES / PC_TICK_HZ);
    if (phase >= FRAMES) phase = 0.0f;   /* 06034754: back to 0, not modulo */
    /* 06034848: the Saturn's six frames, a sound on a change of frame. */
    const int frame = (int)(phase * 6.0f / FRAMES);
    if (frame != saturn_frame) {
        saturn_frame = frame;
        if (frame == 3) pc_sound_horse_hoof(0);
        else if (frame == 4) pc_sound_horse_hoof(1);
        else if (frame == 0 || (frame == 5 && speed_kmh < 260)) pc_sound_horse_hoof(2);
    }
}

unsigned pc_horse_frame(void) { return (unsigned)phase & 31u; }

const nm_drive_params_t *pc_horse_player_math(const nm_drive_params_t *base) {
    if (!riding || !base) return base;
    if (horse_base != base) {
        if (!horse_math) horse_math = malloc(sizeof(*horse_math));
        if (!horse_math) return base;
        memcpy(horse_math, base, sizeof(*horse_math));
        if (horse_torque_load())
            for (unsigned c = 0; c < 2; ++c)
                for (unsigned i = 0; i < HORSE_TORQUE_ENTRIES; ++i) horse_math->curve[c][i] = (float)horse_torque[c][i] / 65536.0f;
        for (unsigned a = 0; a < 2; ++a) {
            const float floor = base->axle.minimum[a] * horse_grip[a];
            horse_math->axle.minimum[a] = floor < base->axle.maximum[a] ? floor : base->axle.maximum[a];
        }
        horse_base = base;
    }
    return horse_math;
}

float pc_horse_wall_factor(const nm_drive_params_t *p) {
    return p && p == horse_math && riding ? 0.8f * HORSE_WALL : 0.8f;
}
