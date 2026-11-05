/* The settings screen (include/gameplay/settings_mode.h). Text only (enDjinn's qfont); no gameplay state but the
 * settings and records of include/game_save.h, which it changes only when it is left with START or EXIT. */
#include <enDjinn/enj_enDjinn.h>
#include <stdio.h>
#include "gameplay/settings_mode.h"
#include "gameplay/title_mode.h"
#include "audio/sound.h"
#include "game_save.h"

enum { ITEM_DIFFICULTY, ITEM_ADVERTISE, ITEM_CLEAR, ITEM_EXIT, ITEM_COUNT };
static struct {
    nm_settings_t settings;
    unsigned item, clear; /* clear: 0 no, 1 asked to confirm, 2 confirmed (done on leaving) */
    int leave;            /* 1 save and leave, -1 leave unchanged */
} screen;

static const char *card_line(void) {
    const nm_game_save_status_t s=nm_game_save_status();
    if(s.busy) return "VMU: SAVING...";
    if(s.last!=NM_SAVE_NO_FILE)
        return s.last==NM_SAVE_OK ? "VMU: SAVED" : s.last==NM_SAVE_FULL ? "VMU: FULL, NOT SAVED (5 BLOCKS NEEDED)" :
               s.last==NM_SAVE_NO_CARD ? "VMU: NONE, NOT SAVED" : "VMU: ERROR, NOT SAVED";
    return s.boot==NM_SAVE_OK ? "VMU: RECORDS AND SETTINGS LOADED" :
           s.boot==NM_SAVE_NO_FILE ? "VMU: NO SAVE YET (5 BLOCKS)" :
           s.boot==NM_SAVE_DAMAGED ? "VMU: SAVE DAMAGED, DEFAULTS IN USE" :
           s.boot==NM_SAVE_NO_CARD ? "VMU: NONE" : "VMU: ERROR";
}

static void line(unsigned item,const char *name,const char *value,int y) {
    enj_qfont_color_set(item==screen.item ? 255 : 160,item==screen.item ? 220 : 160,item==screen.item ? 64 : 160);
    char text[64];
    snprintf(text,sizeof(text),"%s %-18s %s",item==screen.item ? ">" : " ",name,value);
    enj_qfont_write(text,96,y,PVR_LIST_PT_POLY);
}

static void render(void *unused) {
    (void)unused;
    enj_font_scale_set(1);
    enj_qfont_color_set(255,255,255);
    enj_qfont_write("GAME ASSIGNMENTS",96,80,PVR_LIST_PT_POLY);
    line(ITEM_DIFFICULTY,"DIFFICULTY",nm_settings_difficulty_name(screen.settings.difficulty),140);
    line(ITEM_ADVERTISE,"ADVERTISE SOUND",nm_settings_advertise_name(screen.settings.advertise_sound),170);
    line(ITEM_CLEAR,"CLEAR RECORDS",screen.clear==2 ? "YES (ON EXIT)" : screen.clear==1 ? "A AGAIN TO CONFIRM" : "",200);
    line(ITEM_EXIT,"EXIT","",230);
    enj_qfont_color_set(160,160,160);
    enj_qfont_write(card_line(),96,300,PVR_LIST_PT_POLY);
    enj_qfont_write("UP/DOWN SELECT  LEFT/RIGHT CHANGE",96,360,PVR_LIST_PT_POLY);
    enj_qfont_write("START SAVE AND EXIT  B CANCEL",96,384,PVR_LIST_PT_POLY);
    enj_qfont_color_set(255,255,255);
}

static unsigned step(unsigned value,unsigned count,int direction) {
    return (value+count+(unsigned)direction)%count;
}

static void change(int direction) {
    switch(screen.item) {
    case ITEM_DIFFICULTY: screen.settings.difficulty=(uint8_t)step(screen.settings.difficulty,NM_DIFFICULTY_COUNT,direction);break;
    case ITEM_ADVERTISE: screen.settings.advertise_sound=(uint8_t)step(screen.settings.advertise_sound,NM_ADVERTISE_COUNT,direction);break;
    case ITEM_CLEAR: screen.clear=screen.clear ? 2 : 1;break;
    case ITEM_EXIT: screen.leave=1;break;
    }
}

static void update(void *unused) {
    (void)unused;
    nm_sound_update();
    enj_ctrlr_state_t **controllers=enj_ctrl_get_states();
    for(size_t i=0;i<enj_ctrl_states_length();++i) if(controllers[i]) {
        const enj_ctrlr_state_t *c=controllers[i];
#define PRESSED(b) (c->button.b==ENJ_BUTTON_DOWN_THIS_FRAME)
        if(PRESSED(UP)) { screen.item=step(screen.item,ITEM_COUNT,-1);if(screen.clear==1) screen.clear=0; }
        if(PRESSED(DOWN)) { screen.item=step(screen.item,ITEM_COUNT,1);if(screen.clear==1) screen.clear=0; }
        if(PRESSED(LEFT) && screen.item<ITEM_CLEAR) change(-1);
        if(PRESSED(RIGHT) && screen.item<ITEM_CLEAR) change(1);
        if(PRESSED(A)) change(1);
        if(PRESSED(START)) screen.leave=1;
        if(PRESSED(B)) screen.leave=-1;
#undef PRESSED
        break;
    }
    if(screen.leave) {
        if(screen.leave>0) {
            const int changed=screen.settings.difficulty!=nm_game_settings()->difficulty ||
                screen.settings.advertise_sound!=nm_game_settings()->advertise_sound || screen.clear==2;
            nm_game_settings_set(&screen.settings);
            if(screen.clear==2) nm_game_records_clear();
            if(changed) nm_game_save_request();
            printf("Native settings: difficulty %s, advertise sound %s%s%s\n",
                nm_settings_difficulty_name(screen.settings.difficulty),nm_settings_advertise_name(screen.settings.advertise_sound),
                screen.clear==2 ? ", records cleared" : "",changed ? ", saving" : ", unchanged");
        }
        if(!nm_title_mode_start()) enj_state_flag_shutdown(NULL);
        return;
    }
    enj_render_list_add(PVR_LIST_PT_POLY,render,NULL);
}

int nm_settings_mode_start(void) {
    static enj_mode_t mode={.name="Daytona settings",.mode_updater=update};
    screen.settings=*nm_game_settings();
    screen.item=ITEM_DIFFICULTY;screen.clear=0;screen.leave=0;
    enj_mode_set(&mode);return 1;
}
