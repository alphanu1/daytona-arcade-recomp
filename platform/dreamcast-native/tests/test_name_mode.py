"""Actual enDjinn name updater with mock I/O; lifecycle, not math validation."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
code=(root/'code/gameplay/name_mode.c').read_text()
body=code[code.index('static nm_name_scene_t scene;'):]
harness=r'''
#include <assert.h>
#include <stdio.h>
#include "gameplay/name_scene.h"
#include "practice.h"
#include "race.h"
#include "race_records.h"
typedef int nm_name_label_part_t;
static nm_header_t header={.instances=30};static nm_instance_t instances[30];
static const nm_name_layout_t nm_name_layout_data={0};
static const uint32_t nm_name_model_addresses[30]={0},nm_name_colors[2][2][64]={0};
static const uint16_t nm_name_material_headers[2][4]={0};
static const nm_name_label_part_t nm_name_label_parts[1]={0};
#define NM_NAME_LABEL_TEXTURE_COUNT 1
#define NM_NAME_LABEL_STATIC_COUNT 1
static nm_name_rules_t nm_name_rules;
#define ENJ_BUTTON_DOWN_THIS_FRAME 3
typedef struct { int8_t joyx;struct { unsigned A; } button; } enj_ctrlr_state_t;
static enj_ctrlr_state_t control={.joyx=-127};
static enj_ctrlr_state_t *controllers[1]={&control};
static enj_ctrlr_state_t **enj_ctrl_get_states(void) { return controllers; }
static size_t enj_ctrl_states_length(void) { return 1; }
static struct { struct { int shut_down; } flags; } engine;
static uint64_t clock_us=1;
static uint64_t timer_ns_gettime64(void) { return clock_us*1000; }
/* frame_clock.h (through practice.h): the console counts one tick per displayed frame; here the ticks of the stub
 * clock (missed holds the tick the clock last stood at). */
uint32_t nm_frame_ticks(nm_frame_clock_t *clock) {
    const uint32_t at=(uint32_t)(clock_us/NM_PRACTICE_TICK_US);
    if(!clock->primed) { clock->primed=1;clock->missed=at;return 0; }
    const uint32_t ticks=at-clock->missed;clock->missed=at;return ticks;
}
#define enj_state_get() (&engine)
typedef struct { const char *name;void (*mode_updater)(void *); } enj_mode_t;
static enj_mode_t *active;
static unsigned pending,releases,unloads,draws;
static unsigned title_starts;
static int labels_ok=1;
static int nm_title_mode_start(void) { ++title_starts;return 1; }
static int nm_attract_mode_start(void) { ++title_starts;return 1; } /* phase 29/30 -> 2: the attract */
static uint32_t frontend_counter;
static uint32_t nm_frontend_counter(void) { return frontend_counter; }
static void nm_frontend_counter_set(uint32_t counter) { frontend_counter=counter; }
/* audio/sound.h */
static void nm_sound_command(uint32_t command) { (void)command; }
static void nm_sound_update(void) {}
static void nm_sound_settle(void) {}
/* game_save.h: the VMU write after the record and its name are in place, before the title. */
static unsigned save_requests;
static void nm_game_save_request(void) { assert(title_starts==save_requests);++save_requests; }
static void enj_mode_set(enj_mode_t *mode) { assert(!pending);active=mode; }
static void enj_state_flag_shutdown(void *p) { (void)p;assert(!pending);engine.flags.shut_down=1; }
int nm_pack_load(nm_pack_t *p,const char *path) { (void)path;p->header=&header;p->instances=instances;return 1; }
void nm_pack_unload(nm_pack_t *p) { assert(!pending);p->header=NULL;++unloads; }
static int nm_name_render_release(void) { assert(!pending);++releases;return 1; }
static int nm_name_labels_unload(void) { assert(!pending);return 1; }
static int nm_name_render_init(const nm_pack_t *p,const nm_name_layout_t *l,
 const uint16_t h[2][4],const uint32_t c[2][2][64],unsigned d) {
 (void)p;(void)l;(void)h;(void)c;(void)d;return 1;
}
static int nm_name_labels_load(const char *path,const nm_name_label_part_t *parts,unsigned count,unsigned fixed) {
(void)path;(void)parts;(void)count;(void)fixed;return 1;
}
static unsigned label_rank,label_total,label_best,label_completed;
static int nm_name_labels_record(unsigned rank,uint32_t total,uint16_t best,int completed) {
    label_rank=rank;label_total=total;label_best=best;label_completed=completed;return 1;
}
static int nm_name_render_enqueue_intro(const nm_drive_params_t *p,const int16_t *c,unsigned n) {
 (void)p;(void)c;assert(n<32);++pending;++draws;return 1;
}
static int nm_name_render_enqueue(const nm_name_entry_t *s,const nm_name_marker_t *m,
 const nm_drive_params_t *p,const int16_t *c,uint32_t f) {
 (void)s;(void)m;(void)p;(void)c;(void)f;++pending;++draws;return 1;
}
static int nm_name_labels_enqueue(int seconds) { assert(seconds>=-1 && seconds<=20);++pending;return labels_ok; }
static unsigned exit_draws,last_exit_tick,last_exit_phase;
static int nm_name_render_enqueue_exit(const nm_name_entry_t *s,const nm_drive_params_t *p,
 const int16_t *c,unsigned phase,uint16_t tick) {
 (void)s;(void)p;(void)c;assert(phase==5 || phase==7);
 ++exit_draws;last_exit_tick=tick;last_exit_phase=phase;++pending;return 1;
}
'''
checks=r'''
static void step(unsigned ticks) {
    pending=0;clock_us+=ticks*NM_PRACTICE_TICK_US;active->mode_updater(NULL);
}
int main(void) {
    static nm_drive_params_t params;
    int failed=0;nm_race_record_t record={.rank=2,.time_copy=1237,.name=0xffffffff};
    for(unsigned i=0;i<40;++i) nm_name_rules.rules[i].name=0xffffff;
    assert(nm_name_mode_start("/pc/",&params,1,&record,467,1,&failed));
    assert(label_rank==2 && label_total==1237 && label_best==467 && label_completed==1);
    step(0);assert(!failed && pending && !committed);
    for(unsigned i=0;i<32;++i) step(1);
    assert(!scene.intro_remaining && scene.remaining==1280);
    control.button.A=3;step(0); /* Edge must survive a frame without a tick. */
    control.button.A=0;step(4);
    assert(scene.entry.count==1 && scene.entry.name[0]=='A');
    assert(record.name==0xffffffff && !committed);
    control.joyx=127;control.button.A=3;step(1);
    assert(committed && record.name=='A' && post.timer==192 && !failed);
    control.button.A=0;
    step(1);assert(!exit_draws && draw_phase==1);
    step(1);assert(exit_draws==1 && last_exit_tick==1 && last_exit_phase==5);
    for(unsigned i=2;i<193;++i) step(1);
    assert(post.return_to_attract && !engine.flags.shut_down);
    step(0);assert(!engine.flags.shut_down && releases==1 && unloads==1 && title_starts==1 && save_requests==1);
    nm_name_rules.rules[0]=(nm_name_rule_t){.name='A',.timer=64};
    for(unsigned i=0;i<4;++i) nm_name_rules.replacements[0][i]=0x444342;
    control.joyx=-127;
    assert(nm_name_mode_start("/pc/",&params,1,&record,0,0,&failed));
    step(0);for(unsigned i=0;i<32;++i) step(1);
    control.button.A=3;step(1);
    control.joyx=127;step(1);
    assert(committed && record.name==0x444342 && completion_phase==6);
    control.button.A=0;
    step(1);assert(draw_phase==1);
    for(unsigned i=0;i<25;++i) step(1);
    assert(last_exit_phase==7 && last_exit_tick==20 && !failed);
    engine.flags.shut_down=1;step(0);engine.flags.shut_down=0;
    assert(nm_name_mode_start("/pc/",&params,0,NULL,0,0,&failed));
    step(0);assert(!engine.flags.shut_down && !failed && title_starts==2); /* No qualifying record. */
    assert(save_requests==2); /* the quit above (shut_down) requested none */
    assert(nm_name_mode_start("/pc/",&params,1,&record,0,0,&failed));
    const unsigned old_releases=releases;
    labels_ok=0;step(0);
    assert(failed && pending && releases==old_releases && !engine.flags.shut_down);
    step(0);assert(engine.flags.shut_down && releases==old_releases+1 && save_requests==2);
    puts("PASS: real name mode entry/commit/timeout, retained input edge, one accept per catch-up, deferred cleanup, save request before the title");
}
'''
with tempfile.TemporaryDirectory() as directory:
    source=Path(directory)/'check.c';source.write_text(harness+body+checks)
    binary=Path(directory)/'check'
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-I',str(root/'include'),
                    str(source),str(root/'code/gameplay/name_scene.c'),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
