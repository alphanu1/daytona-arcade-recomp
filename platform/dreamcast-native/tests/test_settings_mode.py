"""Settings screen: the real updater (code/gameplay/settings_mode.c) with mock enDjinn input and text, over the real
game save state (code/save/, no card): edits apply only on START/EXIT, B leaves everything as it was."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
code = (root / 'code/gameplay/settings_mode.c').read_text()
body = code[code.index('enum { ITEM_DIFFICULTY'):]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "game_save.h"
#define ENJ_BUTTON_DOWN_THIS_FRAME 3
#define PVR_LIST_PT_POLY 4
typedef struct { unsigned A,B,START,UP,DOWN,LEFT,RIGHT; } buttons_t;
typedef struct { buttons_t button; } enj_ctrlr_state_t;
typedef struct { const char *name;void (*mode_updater)(void *); } enj_mode_t;
static enj_ctrlr_state_t controller;
static enj_ctrlr_state_t *controllers[1]={&controller};
static enj_ctrlr_state_t **enj_ctrl_get_states(void) { return controllers; }
static size_t enj_ctrl_states_length(void) { return 1; }
static enj_mode_t *active;
static void enj_mode_set(enj_mode_t *mode) { active=mode; }
static unsigned shutdowns,title_starts,sound_updates,queued,texts;
static char shown[16][64];
static void enj_state_flag_shutdown(void *unused) { (void)unused;++shutdowns; }
static int nm_title_mode_start(void) { ++title_starts;active=NULL;return 1; }
static void nm_sound_update(void) { ++sound_updates; }
static void (*pending)(void *);
static void enj_render_list_add(int list,void (*renderer)(void *),void *data) {
    assert(list==PVR_LIST_PT_POLY && !data);pending=renderer;++queued;
}
static void enj_font_scale_set(int scale) { assert(scale==1); }
static void enj_qfont_color_set(int r,int g,int b) { (void)r;(void)g;(void)b; }
static int enj_qfont_write(const char *text,int x,int y,int list) {
    (void)x;(void)y;assert(list==PVR_LIST_PT_POLY);
    if(texts<16) snprintf(shown[texts++],64,"%s",text);return 1;
}
static int showing(const char *part) {
    for(unsigned i=0;i<texts;++i) if(strstr(shown[i],part)) return 1;
    return 0;
}
'''
checks = r'''
static nm_race_record_seed_t seeds[NM_SAVE_TABLES][NM_SAVE_SLOTS];
static unsigned frames;
static void frame(void) {
    ++frames;texts=0;pending=NULL;active->mode_updater(NULL);
    if(pending) pending(NULL);
    controller.button=(buttons_t){0};
}
static void press(unsigned *button) { *button=ENJ_BUTTON_DOWN_THIS_FRAME;frame(); }
int main(void) {
    for(unsigned t=0;t<NM_SAVE_TABLES;++t) for(unsigned i=0;i<NM_SAVE_SLOTS;++i)
        seeds[t][i]=(nm_race_record_seed_t){.rank=i+1,.time=1000+i,.name=0x414141};
    nm_game_save_boot(seeds,1);
    nm_race_record_t *beginner=nm_game_records(0);
    beginner[0].name=0x585858;
    /* B: nothing changes. */
    assert(nm_settings_mode_start());frame();
    assert(showing("GAME ASSIGNMENTS") && showing("> DIFFICULTY") && showing("NORMAL") && showing("ADVERTISE SOUND") &&
           showing("VMU: NONE"));
    press(&controller.button.RIGHT);assert(showing("HARD"));
    press(&controller.button.B);
    assert(!active && title_starts==1 && nm_game_settings()->difficulty==NM_DIFFICULTY_NORMAL);
    /* Change both settings, clear the records, START. */
    assert(nm_settings_mode_start());frame();
    press(&controller.button.LEFT);assert(showing("EASY"));
    press(&controller.button.LEFT);assert(showing("HARDEST"));
    press(&controller.button.DOWN);press(&controller.button.A);assert(showing("OFF"));
    press(&controller.button.DOWN);press(&controller.button.A);assert(showing("A AGAIN TO CONFIRM"));
    press(&controller.button.UP);press(&controller.button.DOWN);assert(!showing("CONFIRM")); /* moving away cancels */
    press(&controller.button.A);press(&controller.button.A);assert(showing("YES (ON EXIT)"));
    assert(beginner[0].name==0x585858);
    press(&controller.button.START);
    assert(!active && title_starts==2 && !shutdowns);
    assert(nm_game_settings()->difficulty==NM_DIFFICULTY_HARDEST && nm_game_settings()->advertise_sound==NM_ADVERTISE_OFF);
    assert(beginner[0].name==0x414141);
    assert(nm_game_save_status().last==NM_SAVE_NO_CARD); /* a write was requested; the host has no card */
    /* EXIT with A, the screen shows the last write. */
    assert(nm_settings_mode_start());frame();assert(showing("VMU: NONE, NOT SAVED") && showing("HARDEST"));
    for(unsigned i=0;i<3;++i) press(&controller.button.DOWN);
    assert(showing("> EXIT"));
    press(&controller.button.A);assert(!active && title_starts==3);
    assert(sound_updates==frames && queued==frames-3); /* every frame updates the sound; the 3 leaving draw nothing */
    puts("PASS: settings screen edits on START/EXIT only, B cancels, records clear after confirmation, save requested");
}
'''
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'check.c'
    source.write_text(harness + body + checks)
    binary = Path(directory) / 'check'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', str(root / 'include'),
                    str(source)] + [str(root / 'code/save' / f) for f in ('save_data.c', 'save_store.c', 'game_save.c')]
                   + ['-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, stdout=subprocess.DEVNULL)
    print('PASS: settings screen edits on START/EXIT only, B cancels, records clear after confirmation, save requested')
