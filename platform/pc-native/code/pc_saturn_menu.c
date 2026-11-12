/* The PC build's Saturn-style front end (pc_saturn_menu.h). */
#include "pc_saturn_menu.h"
#include <enDjinn/enj_defs.h>
#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Saturn's screen, 352 x 224, on the 640 x 480 frame. */
#define SATURN_W 352.0f
#define SATURN_H 224.0f

typedef struct {
    enj_texture_info_t texture;
    pvr_poly_hdr_t header;
    int loaded;
} sm_texture_t;


static int load_texture(const char *root, const char *name, int opaque, sm_texture_t *out) {
    char path[512];
    if (snprintf(path, sizeof(path), "%ssaturn-menu/%s.dt", root, name) >= (int)sizeof(path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    void *blob = NULL;
    long size = -1;
    if (!fseek(f, 0, SEEK_END)) size = ftell(f);
    int ok = size > 32 && !fseek(f, 0, SEEK_SET) && (blob = malloc((size_t)size)) && fread(blob, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    ok = ok && enj_texture_load_blob(blob, &out->texture);
    free(blob);
    if (!ok) { printf("FAIL: Saturn menus: %s\n", path); return 0; }
    pvr_poly_cxt_t context;
    pvr_poly_cxt_txr(&context, opaque ? PVR_LIST_OP_POLY : PVR_LIST_PT_POLY, out->texture.pvrformat, out->texture.width,
        out->texture.height, fDtAdjustPVRPointer(&out->texture.hdr, out->texture.ptr), PVR_FILTER_NONE);
    context.gen.culling = PVR_CULLING_NONE;
    context.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(&out->header, &context);
    out->loaded = 1;
    return 1;
}

static void unload_texture(sm_texture_t *t) {
    if (t->loaded) enj_texture_unload(&t->texture);
    t->loaded = 0;
}

/* ======================================================================================================== menus */
#include "pc_saturn_options.h"
#include "gameplay/select_mode.h"
#include "gameplay/title_mode.h"
#include "practice.h"
#include "audio/sound.h"
#include "audio/music_select.h"
#include "game_save.h"

enum { TICKS_PER_SECOND = 60 };
/* The pieces (prepare_saturn_menus.py): name, opaque. */
enum {
    T_BG, T_BANNER, T_MAIN_ARCADE, T_MAIN_ARCADE_LIT, T_MAIN_TA, T_MAIN_TA_LIT, T_MAIN_RECORDS, T_MAIN_RECORDS_LIT,
    T_MAIN_OPTIONS, T_MAIN_OPTIONS_LIT, T_FONT_LABEL, T_FONT_CURSOR, T_FONT_VALUE, T_FONT_VALUE_CHOSEN,
    T_FONT_VALUE_ROW, T_VALUES, T_COUNT = T_VALUES + 3 * 7
};
static const char *const value_names[7] = {"normal", "grandprix", "endurance", "easy", "normal-b", "hard", "stereo"};
enum { V_NORMAL, V_GRANDPRIX, V_ENDURANCE, V_EASY, V_NORMAL_B, V_HARD, V_STEREO };
static sm_texture_t menu_tex[T_COUNT + 3];
#define T_MONAURAL (T_COUNT)
static int menu_loaded;

static int menu_load(const char *root) {
    if (menu_loaded) return 1;
    static const char *const fixed[T_VALUES] = {"menu-bg", "banner-mode", "main-arcade-0", "main-arcade-1",
        "main-time-attack-0", "main-time-attack-1", "main-records-0", "main-records-1", "main-options-0",
        "main-options-1", "font-label", "font-cursor", "font-value-0", "font-value-1", "font-value-2"};
    int ok = 1;
    for (unsigned i = 0; ok && i < T_VALUES; ++i) ok = load_texture(root, fixed[i], i == T_BG, menu_tex + i);
    char name[64];
    for (unsigned v = 0; ok && v < 7; ++v)
        for (unsigned k = 0; ok && k < 3; ++k) {
            snprintf(name, sizeof(name), "value-%s-%u", value_names[v], k);
            ok = load_texture(root, name, 0, menu_tex + T_VALUES + 3 * v + k);
        }
    for (unsigned k = 0; ok && k < 3; ++k) {
        snprintf(name, sizeof(name), "value-monaural-%u", k);
        ok = load_texture(root, name, 0, menu_tex + T_MONAURAL + k);
    }
    if (!ok) {
        for (unsigned i = 0; i < T_COUNT + 3; ++i) unload_texture(menu_tex + i);
        return 0;
    }
    menu_loaded = 1;
    return 1;
}

static int menu_release(void) {
    if (!menu_loaded) return 1;
    if (pvr_wait_ready() || pvr_wait_render_done()) return 0;
    for (unsigned i = 0; i < T_COUNT + 3; ++i) unload_texture(menu_tex + i);
    menu_loaded = 0;
    return 1;
}

/* A texture's piece (its top-left w x h texels) at x, y; argb tints it. */
static void piece(const sm_texture_t *t, float x, float y, float w, float h, float z, uint32_t argb) {
    pvr_poly_hdr_t *header = pvr_dr_target();
    *header = t->header;
    pvr_dr_commit(header);
    for (unsigned corner = 0; corner < 4; ++corner) {
        const float dx = corner >= 2 ? w : 0.0f, dy = corner & 1 ? 0.0f : h;
        const shz_vec4_t p = shz_xmtrx_transform_vec4(shz_vec4_init(x + dx, y + dy, z, 1));
        pvr_vertex_t *v = pvr_dr_target();
        *v = (pvr_vertex_t){.flags = corner == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, .x = p.x, .y = p.y, .z = p.z,
            .u = dx / (float)t->texture.width, .v = dy / (float)t->texture.height, .argb = argb};
        pvr_dr_commit(v);
    }
}

static float text_z = 4.0f;
/* Font C text (ASCII 0x20..0x5A, lower case as upper; '@' the cursor) from cell x, y (8 x 16 a glyph). */
static void text(const sm_texture_t *font, int cx, int cy, const char *s, uint32_t argb) {
    pvr_poly_hdr_t *header = pvr_dr_target();
    *header = font->header;
    pvr_dr_commit(header);
    for (int i = 0; s[i]; ++i) {
        int c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 0x20 || c > 0x5A || c == ' ') continue;
        const unsigned g = (unsigned)(c - 0x20);
        const float u0 = (float)(g % 16u) * 8.0f, v0 = (float)(g / 16u) * 16.0f;
        const float x = (float)(cx + i) * 8.0f, y = (float)cy * 8.0f;
        for (unsigned corner = 0; corner < 4; ++corner) {
            const float dx = corner >= 2 ? 8.0f : 0.0f, dy = corner & 1 ? 0.0f : 16.0f;
            const shz_vec4_t p = shz_xmtrx_transform_vec4(shz_vec4_init(x + dx, y + dy, text_z, 1));
            pvr_vertex_t *v = pvr_dr_target();
            *v = (pvr_vertex_t){.flags = corner == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, .x = p.x, .y = p.y,
                .z = p.z, .u = (u0 + dx) / 128.0f, .v = (v0 + dy) / 64.0f, .argb = argb};
            pvr_dr_commit(v);
        }
    }
}

/* ---- the screens' state ---- */
enum { SCREEN_MAIN, SCREEN_OPTIONS, SCREEN_SOUND, SCREEN_RECORDS };
enum { MAIN_ARCADE, MAIN_TIME_ATTACK, MAIN_2P, MAIN_LINK, MAIN_RECORDS, MAIN_OPTIONS, MAIN_ITEMS };
/* Not in this port yet: shown, greyed, passed over by the cursor. */
static const uint8_t main_enabled[MAIN_ITEMS] = {1, 0, 0, 0, 1, 1};

static struct {
    unsigned screen, cursor, ticks, idle, confirm, page_cursor[4], records_course;
    pc_saturn_options_t edit;
    uint64_t frame;
    uint32_t remainder_us;
    nm_frame_clock_t clock;
    int leave;             /* 1 go on (the chosen item), -1 back to the title */
    int bgm_test, playing;
} menu;

static int ok_pressed(const enj_ctrlr_state_t *c) {
    return c->button.A == ENJ_BUTTON_DOWN_THIS_FRAME || c->button.START == ENJ_BUTTON_DOWN_THIS_FRAME ||
           c->button.X == ENJ_BUTTON_DOWN_THIS_FRAME;   /* the Saturn's A / C / Start (X stands for C on a pad) */
}

/* ---- main menu (Circuit Edition's items, the original's Mode Select) ---- */
static int main_row(unsigned item) {
    static const int rows[MAIN_ITEMS] = {4, 8, 12, 14, 16, 20};
    return rows[item];
}

static void main_render(int opaque) {
    if (opaque) {
        piece(menu_tex + T_BG, 0, 0, SATURN_W, SATURN_H, 1.0f, 0xffffffff);
        return;
    }
    piece(menu_tex + T_BANNER, 80.0f, 0.0f, 192.0f, 32.0f, 2.0f, 0xffffffff);
    static const unsigned big[MAIN_ITEMS] = {T_MAIN_ARCADE, T_MAIN_TA, 0, 0, T_MAIN_RECORDS, T_MAIN_OPTIONS};
    static const unsigned widths[MAIN_ITEMS] = {18, 30, 0, 0, 21, 19};
    for (unsigned i = 0; i < MAIN_ITEMS; ++i) {
        const int lit = i == menu.cursor && !(menu.confirm && (menu.confirm / 10u) % 2u);
        if (big[i]) {
            const float x = (float)((44 - (int)widths[i]) / 2 * 8);
            const uint32_t tint = main_enabled[i] ? 0xffffffff : 0xff8a8a8a;
            piece(menu_tex + big[i] + (lit && main_enabled[i] ? 1 : 0), x, (float)(main_row(i) * 8), (float)widths[i] * 8.0f,
                32.0f, 3.0f, tint);
        } else {
            const char *s = i == MAIN_2P ? "2PLAYER BATTLE" : "LINK BATTLE";
            text(menu_tex + T_FONT_LABEL, (44 - (int)strlen(s)) / 2, main_row(i), s, 0xff8a8a8a);
        }
    }
}

static void main_input(const enj_ctrlr_state_t *c) {
    if (menu.confirm) return;
    int step = 0;
    if (c->button.UP == ENJ_BUTTON_DOWN_THIS_FRAME) step = -1;
    if (c->button.DOWN == ENJ_BUTTON_DOWN_THIS_FRAME) step = 1;
    if (step) {
        do menu.cursor = (menu.cursor + MAIN_ITEMS + (unsigned)step) % MAIN_ITEMS;
        while (!main_enabled[menu.cursor]);
        menu.idle = 0;
    }
    if (ok_pressed(c)) { menu.confirm = 1; menu.idle = 0; }
    if (c->button.B == ENJ_BUTTON_DOWN_THIS_FRAME) menu.leave = -1;
}

/* ---- options (Circuit Edition's rows, the original's Options) ---- */
enum { OPT_HANDLING1, OPT_HANDLING2, OPT_GAME_MODE, OPT_DIFFICULTY, OPT_COMPARE, OPT_SECTION, OPT_GHOST, OPT_TA_COMPARE,
    OPT_TA_SECTION, OPT_SOUND, OPT_KEYS, OPT_EXIT, OPT_ROWS };
static const char *const opt_labels[OPT_ROWS] = {"HANDLING 1P", "HANDLING 2P", "GAME MODE", "DIFFICULTY",
    "TIME COMPARE", "SECTION LAP", "TA GHOST", "TA COMPARE", "TA SECT.LAP", "SOUND", "KEY CONFIG", "EXIT"};
/* Not in this port yet (greyed, passed over): 2P handling, the time-attack and timing displays, key config. */
static const uint8_t opt_enabled[OPT_ROWS] = {1, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 1};

/* A value chip: a value word of the original's (texture index) or font C text, laid out from cell x. */
typedef struct { int map; const char *text; int cells; } chip_t;
static int chips_of(unsigned row, chip_t out[8], unsigned *chosen) {
    const pc_saturn_options_t *o = &menu.edit;
    static const char *const ghosts[7] = {"A", "B", "C", "D", "E", "F", "OFF"};
    switch (row) {
    case OPT_HANDLING1: case OPT_HANDLING2:
        out[0] = (chip_t){-1, "SLOW", 4}; out[1] = (chip_t){V_NORMAL, NULL, 7}; out[2] = (chip_t){-1, "QUICK", 5};
        *chosen = o->handling[row - OPT_HANDLING1]; return 3;
    case OPT_GAME_MODE:
        out[0] = (chip_t){V_NORMAL, NULL, 7}; out[1] = (chip_t){V_GRANDPRIX, NULL, 10}; out[2] = (chip_t){V_ENDURANCE, NULL, 11};
        *chosen = o->game_mode; return 3;
    case OPT_DIFFICULTY:
        out[0] = (chip_t){V_EASY, NULL, 4}; out[1] = (chip_t){V_NORMAL_B, NULL, 6}; out[2] = (chip_t){V_HARD, NULL, 4};
        *chosen = o->difficulty; return 3;
    case OPT_COMPARE: case OPT_SECTION: case OPT_TA_COMPARE: case OPT_TA_SECTION: {
        const uint8_t v = row == OPT_COMPARE ? o->arcade_compare : row == OPT_SECTION ? o->arcade_section :
                          row == OPT_TA_COMPARE ? o->ta_compare : o->ta_section;
        out[0] = (chip_t){-1, "ON", 2}; out[1] = (chip_t){-1, "OFF", 3};
        *chosen = v ? 0u : 1u; return 2;
    }
    case OPT_GHOST:
        for (unsigned i = 0; i < 7; ++i) out[i] = (chip_t){-1, ghosts[i], i < 6 ? 1 : 3};
        *chosen = o->ghost; return 7;
    }
    return 0;
}

static void chips_render(unsigned row, int y, int on_row, int enabled) {
    chip_t chips[8];
    unsigned chosen = 0;
    const int n = chips_of(row, chips, &chosen);
    int x = 14 + (strlen(opt_labels[row]) > 11);   /* the original's column 14, past a 12-letter label */
    for (int i = 0; i < n; ++i) {
        const int state = (unsigned)i == chosen ? (on_row && (menu.frame / 8u) % 2u ? 2 : 1) : 0;
        const uint32_t tint = enabled ? 0xffffffff : 0xff808080;
        if (chips[i].map >= 0)
            piece(menu_tex + T_VALUES + 3 * chips[i].map + (enabled ? state : 0), (float)(x * 8), (float)(y * 8),
                  (float)menu_tex[T_VALUES + 3 * chips[i].map].texture.width, 16.0f, 3.0f, tint);
        else
            text(menu_tex + T_FONT_VALUE + (enabled ? state : 0), x, y, chips[i].text, tint);
        x += chips[i].cells + 1;
    }
}

static void options_render(int opaque) {
    if (opaque) {
        piece(menu_tex + T_BG, 0, 0, SATURN_W, SATURN_H, 1.0f, 0xffffffff);
        return;
    }
    for (unsigned r = 0; r < OPT_ROWS; ++r) {
        const int y = 2 + 2 * (int)r;
        const int on = r == menu.cursor;
        char label[16];
        snprintf(label, sizeof(label), "%c%s", on ? '@' : ' ', opt_labels[r]);
        text(menu_tex + (on ? T_FONT_CURSOR : opt_enabled[r] ? T_FONT_LABEL : T_FONT_VALUE), 1, y, label, 0xffffffff);
        chips_render(r, y, on, opt_enabled[r]);
    }
}

static void step_value(unsigned row, int d) {
    pc_saturn_options_t *o = &menu.edit;
#define WRAP(v, n) (v) = (uint8_t)(((v) + (n) + d) % (n))
    switch (row) {
    case OPT_HANDLING1: WRAP(o->handling[0], 3); break;
    case OPT_GAME_MODE: WRAP(o->game_mode, 3); break;
    case OPT_DIFFICULTY: WRAP(o->difficulty, 3); break;
    }
#undef WRAP
}

static void options_input(const enj_ctrlr_state_t *c) {
    int step = 0;
    if (c->button.UP == ENJ_BUTTON_DOWN_THIS_FRAME) step = -1;
    if (c->button.DOWN == ENJ_BUTTON_DOWN_THIS_FRAME) step = 1;
    if (step) do menu.cursor = (menu.cursor + OPT_ROWS + (unsigned)step) % OPT_ROWS; while (!opt_enabled[menu.cursor]);
    if (c->button.LEFT == ENJ_BUTTON_DOWN_THIS_FRAME) step_value(menu.cursor, -1);
    if (c->button.RIGHT == ENJ_BUTTON_DOWN_THIS_FRAME) step_value(menu.cursor, 1);
    const int ok = c->button.A == ENJ_BUTTON_DOWN_THIS_FRAME || c->button.X == ENJ_BUTTON_DOWN_THIS_FRAME;
    if ((ok && menu.cursor == OPT_EXIT) || c->button.B == ENJ_BUTTON_DOWN_THIS_FRAME ||
        c->button.START == ENJ_BUTTON_DOWN_THIS_FRAME) {
        /* EXIT, B and Start: keep the changes, save, back to the main menu (CCE 060356A6, 060365F2). */
        pc_saturn_options_set(&menu.edit);
        menu.screen = SCREEN_MAIN;
        menu.cursor = MAIN_OPTIONS;
    } else if (ok && menu.cursor == OPT_SOUND) {
        menu.page_cursor[SCREEN_OPTIONS] = menu.cursor;
        menu.screen = SCREEN_SOUND;
        menu.cursor = 0;
    }
}

/* ---- the Sound page ---- */
enum { SND_MODE, SND_VOLUME, SND_BGM0, SND_BGM_TEST = SND_BGM0 + PC_SATURN_COURSES, SND_EXIT, SND_ROWS };
static const char *const bgm_labels[PC_SATURN_COURSES] = {"THREE SEVEN", "SEA-SIDE", "DINOSAUR", "NATIONAL PK",
    "DESERT CITY"};

static void sound_render(int opaque) {
    if (opaque) {
        piece(menu_tex + T_BG, 0, 0, SATURN_W, SATURN_H, 1.0f, 0xffffffff);
        return;
    }
    const pc_saturn_options_t *o = &menu.edit;
    text(menu_tex + T_FONT_LABEL, 18, 1, "SOUND", 0xffffffff);
    static const unsigned bgm_order[PC_SATURN_COURSES] = {0, 2, 1, 3, 4};   /* CCE's order: by course, not slot */
    for (unsigned r = 0; r < SND_ROWS; ++r) {
        const int y = 4 + 2 * (int)r;
        const int on = r == menu.cursor;
        const char *label = r == SND_MODE ? "SOUND MODE" : r == SND_VOLUME ? "BGM VOLUME" : r == SND_BGM_TEST ?
            "BGM TEST" : r == SND_EXIT ? "EXIT" : bgm_labels[bgm_order[r - SND_BGM0]];
        char line[16];
        snprintf(line, sizeof(line), "%c%s", on ? '@' : ' ', label);
        text(menu_tex + (on ? T_FONT_CURSOR : T_FONT_LABEL), 1, y, line, 0xffffffff);
        const int blink = on && (menu.frame / 8u) % 2u;
        if (r == SND_MODE) {
            piece(menu_tex + T_VALUES + 3 * V_STEREO + (o->stereo ? 1 + blink : 0), 14 * 8, (float)(y * 8), 56, 16, 3, 0xffffffff);
            piece(menu_tex + T_MONAURAL + (!o->stereo ? 1 + blink : 0), 22 * 8, (float)(y * 8), 72, 16, 3, 0xffffffff);
        } else if (r == SND_VOLUME) {
            static const char *const vols[8] = {"OFF", "1", "2", "3", "4", "5", "6", "7"};
            int x = 14;
            for (unsigned v = 0; v < 8; ++v) {
                text(menu_tex + T_FONT_VALUE + (v == o->bgm_volume ? 1 + blink : 0), x, y, vols[v], 0xffffffff);
                x += (int)strlen(vols[v]) + 1;
            }
        } else if (r >= SND_BGM0 && r < SND_BGM_TEST) {
            const unsigned course = bgm_order[r - SND_BGM0];
            text(menu_tex + T_FONT_VALUE + 1 + blink, 14, y, pc_saturn_bgm_name(o->bgm[course]), 0xffffffff);
        } else if (r == SND_BGM_TEST) {
            char s[32];
            snprintf(s, sizeof(s), "%s%s", pc_saturn_bgm_name((unsigned)menu.bgm_test), menu.playing ? "  PLAYING" : "");
            text(menu_tex + T_FONT_VALUE + 1 + blink, 14, y, s, 0xffffffff);
        }
    }
}

static void sound_input(const enj_ctrlr_state_t *c) {
    pc_saturn_options_t *o = &menu.edit;
    static const unsigned bgm_order[PC_SATURN_COURSES] = {0, 2, 1, 3, 4};
    int step = 0, d = 0;
    if (c->button.UP == ENJ_BUTTON_DOWN_THIS_FRAME) step = -1;
    if (c->button.DOWN == ENJ_BUTTON_DOWN_THIS_FRAME) step = 1;
    if (step) menu.cursor = (menu.cursor + SND_ROWS + (unsigned)step) % SND_ROWS;
    if (c->button.LEFT == ENJ_BUTTON_DOWN_THIS_FRAME) d = -1;
    if (c->button.RIGHT == ENJ_BUTTON_DOWN_THIS_FRAME) d = 1;
    if (d) {
        if (menu.cursor == SND_MODE) o->stereo = (uint8_t)!o->stereo;
        else if (menu.cursor == SND_VOLUME) o->bgm_volume = (uint8_t)((o->bgm_volume + 8 + d) % 8);
        else if (menu.cursor >= SND_BGM0 && menu.cursor < SND_BGM_TEST) {
            uint8_t *b = &o->bgm[bgm_order[menu.cursor - SND_BGM0]];
            *b = (uint8_t)((*b + PC_BGM_SONGS + d) % PC_BGM_SONGS);
        } else if (menu.cursor == SND_BGM_TEST) menu.bgm_test = (menu.bgm_test + PC_BGM_SONGS + d) % PC_BGM_SONGS;
        /* SOUND MODE and the volume take effect at once (CCE: the course choices on leaving Options). */
        if (menu.cursor <= SND_VOLUME) {
            pc_saturn_options_t live = *pc_saturn_options();
            live.stereo = o->stereo;
            live.bgm_volume = o->bgm_volume;
            pc_saturn_options_set(&live);
        }
    }
    const int ok = c->button.A == ENJ_BUTTON_DOWN_THIS_FRAME || c->button.X == ENJ_BUTTON_DOWN_THIS_FRAME;
    if (ok && menu.cursor == SND_BGM_TEST) {
        nm_sound_command(NM_MUSIC_FADE);
        nm_sound_command(nm_music_race[menu.bgm_test]);
        menu.playing = 1;
    }
    if ((ok && menu.cursor == SND_EXIT) || c->button.B == ENJ_BUTTON_DOWN_THIS_FRAME ||
        c->button.START == ENJ_BUTTON_DOWN_THIS_FRAME) {
        if (menu.playing) { nm_sound_command(NM_MUSIC_FADE); menu.playing = 0; }
        menu.screen = SCREEN_OPTIONS;
        menu.cursor = menu.page_cursor[SCREEN_OPTIONS];
    }
}

/* ---- records (the arcade's own course records; Circuit Edition's Records page) ---- */
static void records_render(int opaque) {
    if (opaque) {
        piece(menu_tex + T_BG, 0, 0, SATURN_W, SATURN_H, 1.0f, 0xffffffff);
        return;
    }
    static const unsigned order[PC_SATURN_COURSES] = {0, 2, 1, 3, 4};
    const unsigned course = order[menu.records_course];
    text(menu_tex + T_FONT_LABEL, 18, 1, "RECORDS", 0xffffffff);
    char line[48];
    snprintf(line, sizeof(line), "@ %s", pc_saturn_course_name(course));
    text(menu_tex + T_FONT_CURSOR, (44 - (int)strlen(line)) / 2, 4, line, 0xffffffff);
    nm_race_record_t *r = nm_game_records(course);
    if (!r) {
        text(menu_tex + T_FONT_VALUE, 12, 12, "NO RECORDS THIS SESSION", 0xffffffff);
        return;
    }
    static const char *const ord[10] = {"1ST", "2ND", "3RD", "4TH", "5TH", "6TH", "7TH", "8TH", "9TH", "10TH"};
    for (unsigned i = 0; i < 10; ++i) {
        const uint32_t hs = (r[i].time * 100u) >> 6;
        const char n0 = (char)(r[i].name & 0xff), n1 = (char)((r[i].name >> 8) & 0xff), n2 = (char)((r[i].name >> 16) & 0xff);
        snprintf(line, sizeof(line), "%-5s%c%c%c   %u'%02u\"%02u", ord[i], n0 ? n0 : ' ', n1 ? n1 : ' ', n2 ? n2 : ' ',
                 (unsigned)(hs / 6000u), (unsigned)(hs / 100u % 60u), (unsigned)(hs % 100u));
        text(menu_tex + (i == 0 ? T_FONT_VALUE + 1 : T_FONT_LABEL), 12, 7 + 2 * (int)i, line, 0xffffffff);
    }
}

static void records_input(const enj_ctrlr_state_t *c) {
    int d = 0;
    if (c->button.LEFT == ENJ_BUTTON_DOWN_THIS_FRAME || c->button.UP == ENJ_BUTTON_DOWN_THIS_FRAME) d = -1;
    if (c->button.RIGHT == ENJ_BUTTON_DOWN_THIS_FRAME || c->button.DOWN == ENJ_BUTTON_DOWN_THIS_FRAME) d = 1;
    if (d) menu.records_course = (menu.records_course + PC_SATURN_COURSES + (unsigned)d) % PC_SATURN_COURSES;
    if (c->button.B == ENJ_BUTTON_DOWN_THIS_FRAME || c->button.START == ENJ_BUTTON_DOWN_THIS_FRAME ||
        c->button.A == ENJ_BUTTON_DOWN_THIS_FRAME) {
        menu.screen = SCREEN_MAIN;
        menu.cursor = MAIN_RECORDS;
    }
}

/* ---- the mode ---- */
static void menu_render(void *data) {
    const int opaque = (int)(uintptr_t)data;
    shz_xmtrx_init_scale((640.0f / SATURN_W) * ENJ_XSCALE, 480.0f / SATURN_H, 1.0f);
    switch (menu.screen) {
    case SCREEN_MAIN: main_render(opaque); break;
    case SCREEN_OPTIONS: options_render(opaque); break;
    case SCREEN_SOUND: sound_render(opaque); break;
    case SCREEN_RECORDS: records_render(opaque); break;
    }
}

static void menu_go(void) {
    const unsigned item = menu.cursor;
    if (!menu_release()) { enj_state_flag_shutdown(NULL); return; }
    pc_saturn_options_t o = *pc_saturn_options();
    o.last_mode = (uint8_t)item;
    pc_saturn_options_set(&o);
    printf("Saturn menu: %s\n", item == MAIN_ARCADE ? "ARCADE" : "?");
    if (!nm_select_mode_start(0, nm_frontend_counter() + menu.ticks + 1u)) enj_state_flag_shutdown(NULL);
}

static void menu_update(void *unused) {
    (void)unused;
    nm_sound_update();
    if (enj_state_get()->flags.shut_down) { menu_release(); return; }
    uint32_t ticks = nm_frame_ticks(&menu.clock);
    if (ticks > 4) ticks = 4;
    menu.ticks += ticks;
    menu.frame += ticks;
    enj_ctrlr_state_t **controllers = enj_ctrl_get_states();
    for (size_t i = 0; i < enj_ctrl_states_length(); ++i) if (controllers[i]) {
        const enj_ctrlr_state_t *c = controllers[i];
        const unsigned before = menu.screen;
        const unsigned was = menu.cursor;
        switch (menu.screen) {
        case SCREEN_MAIN: main_input(c); break;
        case SCREEN_OPTIONS: options_input(c); break;
        case SCREEN_SOUND: sound_input(c); break;
        case SCREEN_RECORDS: records_input(c); break;
        }
        if (c->button.raw & 0x55555u) menu.idle = 0;
        if (menu.screen != before || menu.cursor != was) printf("Saturn menu: screen %u, cursor %u\n", menu.screen, menu.cursor);
        break;
    }
    if (menu.screen == SCREEN_MAIN) {
        menu.idle += ticks;
        if (menu.confirm) {
            menu.confirm += ticks;
            if (menu.confirm > 30) {        /* the item blinks 3 x 10 ticks (CCE), then its screen */
                menu.confirm = 0;
                if (menu.cursor == MAIN_ARCADE) { menu_go(); return; }
                if (menu.cursor == MAIN_OPTIONS) {
                    menu.edit = *pc_saturn_options();
                    menu.screen = SCREEN_OPTIONS;
                    menu.cursor = OPT_HANDLING1;
                } else if (menu.cursor == MAIN_RECORDS) {
                    menu.screen = SCREEN_RECORDS;
                    menu.records_course = 0;
                }
            }
        }
        if (menu.idle > 18000u) menu.leave = -1;   /* CCE: 18000 ticks without input, back to the title */
    }
    if (menu.leave < 0) {
        if (!menu_release() || !nm_title_mode_start()) enj_state_flag_shutdown(NULL);
        return;
    }
    enj_render_list_add(PVR_LIST_OP_POLY, menu_render, (void *)(uintptr_t)1);
    enj_render_list_add(PVR_LIST_PT_POLY, menu_render, NULL);
}

int pc_saturn_menu_available(void) {
    const char *root = nm_practice_root();
    char path[512];
    if (!root || snprintf(path, sizeof(path), "%ssaturn-menu/menu-bg.dt", root) >= (int)sizeof(path)) return 0;
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

int pc_saturn_menu_start(void) {
    static enj_mode_t mode = {.name = "Saturn main menu", .mode_updater = menu_update};
    if (!menu_load(nm_practice_root())) return 0;
    memset(&menu, 0, sizeof(menu));
    const unsigned last = pc_saturn_options()->last_mode;
    menu.cursor = last < MAIN_ITEMS && main_enabled[last] ? last : MAIN_ARCADE;
    /* Testing without hands: DAYTONA_MENU_SCREEN=options, sound or records opens that screen. */
    const char *test = getenv("DAYTONA_MENU_SCREEN");
    if (test && *test) {
        menu.edit = *pc_saturn_options();
        if (!strcmp(test, "options")) { menu.screen = SCREEN_OPTIONS; menu.cursor = OPT_HANDLING1; }
        else if (!strcmp(test, "sound")) { menu.screen = SCREEN_SOUND; menu.cursor = 0; menu.page_cursor[SCREEN_OPTIONS] = OPT_SOUND; }
        else if (!strcmp(test, "records")) menu.screen = SCREEN_RECORDS;
    }
    puts("Saturn menu: main menu");
    enj_mode_set(&mode);
    return 1;
}

/* ======================================================================================================== pause */
/* Circuit Edition's pause (RACE.BIN 060295DE): "EXIT ?" over YES, NO, RESTART, the cursor from NO, up and down stop at
 * the ends, Start confirms; drawn as the original Saturn game's PAUSE (APROG 060096DC: font C in the race's palette,
 * PAUSE at cells 19, 10), the cursor's item in the menus' yellow. */
static sm_texture_t pause_font[2];
static int pause_loaded;

int pc_saturn_pause_load(void) {
    if (pause_loaded) return 1;
    const char *root = nm_practice_root();
    if (!root || !load_texture(root, "font-pause", 0, pause_font) || !load_texture(root, "font-cursor", 0, pause_font + 1)) {
        unload_texture(pause_font);
        unload_texture(pause_font + 1);
        return 0;
    }
    pause_loaded = 1;
    return 1;
}

int pc_saturn_pause_release(void) {
    if (!pause_loaded) return 1;
    if (pvr_wait_ready() || pvr_wait_render_done()) return 0;
    unload_texture(pause_font);
    unload_texture(pause_font + 1);
    pause_loaded = 0;
    return 1;
}

static unsigned pause_cursor;
static void pause_render(void *unused) {
    (void)unused;
    shz_xmtrx_init_scale((640.0f / SATURN_W) * ENJ_XSCALE, 480.0f / SATURN_H, 1.0f);
    text_z = 4096.0f;
    static const char *const items[3] = {"YES", "NO", "RESTART"};
    text(pause_font, 19, 8, "PAUSE", 0xffffffff);
    text(pause_font, 19, 12, "EXIT ?", 0xffffffff);
    for (unsigned i = 0; i < 3; ++i) {
        char line[16];
        snprintf(line, sizeof(line), "%c%s", i == pause_cursor ? '@' : ' ', items[i]);
        text(pause_font + (i == pause_cursor), 18, 15 + 2 * (int)i, line, 0xffffffff);
    }
    text_z = 4.0f;
}

int pc_saturn_pause_enqueue(unsigned cursor) {
    if (!pause_loaded) return 0;
    pause_cursor = cursor;
    enj_render_list_add(PVR_LIST_PT_POLY, pause_render, NULL);
    return 1;
}
