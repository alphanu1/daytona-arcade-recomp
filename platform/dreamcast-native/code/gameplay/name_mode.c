#include <enDjinn/enj_enDjinn.h>
#include "gameplay/name_mode.h"
#include "gameplay/title_mode.h"
#include "gameplay/select_mode.h"
#include "gameplay/name_scene.h"
#include "render/ui/name_render.h"
#include "render/ui/name_labels.h"
#include "race.h"
#include "practice.h"
#include "audio/sound.h"
#include "game_save.h"
#include "name_data.h"
#include "name_entry_data.h"
#include "labels/name_label_data.h"

static nm_name_scene_t scene;
static nm_pack_t pack;
static nm_drive_params_t *math;
static nm_race_postresult_t post;
static uint32_t notification,*record_name,frame,commands;
static int *failure,committed;
static nm_frame_clock_t frame_clock;
static uint32_t remainder_us;
static unsigned accept_pending;
static unsigned completion_phase,draw_phase;
static uint16_t completion_tick;

/* The original's sound commands of this mode: counted, and played (audio/sound.h). */
static void name_command(uint32_t command) {
    if(!command) return;
    ++commands;nm_sound_command(command);
}

static void stop(void) {
    /* Updater boundary: previous callbacks are finished; UI unload fences GPU. */
    const int quitting=enj_state_get()->flags.shut_down;
    if(!nm_name_render_release() || !nm_name_labels_unload()) {
        *failure=1;return;
    }
    nm_pack_unload(&pack);
    printf("Native name mode: frames=%lu committed=%d silent-commands=%lu failed=%d; return-to-attract\n",
        (unsigned long)frame,committed,(unsigned long)commands,*failure);
    /* The record and its name are in place: the VMU write runs in the background (include/game_save.h). */
    if(!quitting && !*failure) nm_game_save_request();
    /* Phase 29/30 -> 2 (4c00..4d5c): the attract loop again. 5010b0: phase 29's iteration, then one a frame of
     * phase 30 (nm_practice_game_over left the results' last). */
    nm_frontend_counter_set(nm_frontend_counter()+1u+frame);
    if(!quitting && !*failure && nm_attract_mode_start()) return;
    if(!quitting) { *failure=1;enj_state_flag_shutdown(NULL); }
}

static void update(void *unused) {
    (void)unused;
    nm_sound_update();
    if(*failure || enj_state_get()->flags.shut_down || post.return_to_attract) { stop();return; }
    uint32_t frame_ticks=nm_frame_ticks(&frame_clock); /* one tick per displayed frame (frame_clock.h) */
    if(frame_ticks>4) frame_ticks=4;
    remainder_us+=frame_ticks*NM_PRACTICE_TICK_US;
    int8_t steering=-127;
    enj_ctrlr_state_t **controllers=enj_ctrl_get_states();
    for(size_t i=0;i<enj_ctrl_states_length();++i) if(controllers[i]) {
        steering=controllers[i]->joyx<-127 ? -127 : controllers[i]->joyx;
        accept_pending|=controllers[i]->button.A==ENJ_BUTTON_DOWN_THIS_FRAME;break;
    }
    while(remainder_us>=NM_PRACTICE_TICK_US) {
        remainder_us-=NM_PRACTICE_TICK_US;++frame;
        if(!nm_race_postresult_step(&post,notification)) { *failure=1;return; }
        name_command(post.audio_command);
        if(post.return_to_attract || !(post.name_flags&UINT32_C(0x80000000))) break;
        if(!committed) {
            const int result=nm_name_scene_step(&scene,&nm_name_layout_data,steering,accept_pending);
            accept_pending=0; /* One controller edge cannot accept twice during catch-up. */
            if(!result) { *failure=1;return; }
            for(unsigned i=0;i<2;++i) name_command(scene.commands[i]);
            if(result==2) {
                nm_name_commit_t commit={0};
                if(!nm_name_commit(&scene.entry,&nm_name_rules,frame,notification,record_name,&commit)) {
                    *failure=1;return;
                }
                post.timer=commit.timer;name_command(commit.command);committed=1;
                completion_phase=commit.phase;completion_tick=0;
            }
        } else if(completion_phase==4 || completion_phase==6) {
            ++completion_phase;completion_tick=0;draw_phase=1; /* Setup tick has no models. */
        } else {
            ++completion_tick;
            if(completion_phase==7 && completion_tick>20) completion_tick=20;
            draw_phase=completion_phase;
        }
    }
    if(post.return_to_attract || !(post.name_flags&UINT32_C(0x80000000))) return;
    const int16_t center[2]={248,208};
    const int drawn=draw_phase==1 ? 1 : draw_phase ?
        nm_name_render_enqueue_exit(&scene.entry,math,center,draw_phase,completion_tick) : scene.draw_intro>=0 ?
        nm_name_render_enqueue_intro(math,center,(unsigned)scene.draw_intro) :
        nm_name_render_enqueue(&scene.draw_entry,&scene.marker,math,center,frame);
    if(!drawn || !nm_name_labels_enqueue(scene.draw_intro<0 ? (int)scene.draw_seconds : -1)) *failure=1;
    /* A partial enqueue keeps resources alive until the next updater. */
}

int nm_name_mode_start(const char *root,nm_drive_params_t *params,
        uint32_t notify,nm_race_record_t *record,uint16_t best_lap,
        unsigned completed,int *failed) {
    if(!root || !params || !failed || ((notify&1u) && !record)) return 0;
    math=params;notification=notify;record_name=record ? &record->name : NULL;failure=failed;
    frame=commands=remainder_us=accept_pending=0;committed=0;frame_clock=(nm_frame_clock_t){0};
    completion_phase=draw_phase=completion_tick=0;
    post=(nm_race_postresult_t){.phase=29};
    if(!nm_race_postresult_step(&post,notification)) return 0;
    name_command(post.audio_command);
    if(post.start_name) {
        char path[512];
        if(snprintf(path,sizeof(path),"%sname.dnm",root)>=(int)sizeof(path) ||
           !nm_pack_load(&pack,path)) return 0;
        int ok=pack.header->instances==30;
        for(unsigned i=0;ok && i<30;++i)
            ok=pack.instances[i].source_address==nm_name_model_addresses[i];
        if(!ok || !nm_name_render_init(&pack,&nm_name_layout_data,nm_name_material_headers,nm_name_colors,0) ||
           snprintf(path,sizeof(path),"%sname-labels/",root)>=(int)sizeof(path) ||
           !nm_name_labels_load(path,nm_name_label_parts,NM_NAME_LABEL_TEXTURE_COUNT,
                                NM_NAME_LABEL_STATIC_COUNT) ||
           !nm_name_labels_record(record->rank,record->time_copy,best_lap,completed) ||
           !nm_name_scene_init(&scene,&nm_name_layout_data)) {
            *failure=1;stop();return 0;
        }
    }
    nm_sound_settle(); /* ae1005 (or the fade) at this loading pause, not in the first name frame. */
    static enj_mode_t mode={.name="Name entry",.mode_updater=update};
    enj_mode_set(&mode);return 1;
}
