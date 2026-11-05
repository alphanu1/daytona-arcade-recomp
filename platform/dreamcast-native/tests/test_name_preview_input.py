"""Run actual preview updater with controller/render stubs; no gameplay math."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
code=(root/'tests/name_preview/main.c').read_text()
updater=code[code.index('static nm_pack_t models;'):code.index('\nint main(void)')]
harness=r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "gameplay/name_scene.h"
typedef int nm_pack_t;
static const nm_name_layout_t nm_name_layout_data={0};
#define ENJ_BUTTON_DOWN_THIS_FRAME 3
#define PVR_LIST_PT_POLY 4
typedef struct { struct { unsigned A,B; } button;int8_t joyx; } enj_ctrlr_state_t;
static enj_ctrlr_state_t control;
static enj_ctrlr_state_t *controllers[4]={&control};
static struct { struct { int shut_down; } flags; } engine;
#define enj_state_get() (&engine)
static enj_ctrlr_state_t **enj_ctrl_get_states(void) { return controllers; }
static void enj_state_flag_shutdown(void *p) { (void)p;engine.flags.shut_down=1; }
static void enj_font_scale_set(int scale) { (void)scale; }
static void enj_qfont_write(const char *s,int x,int y,int list) { (void)s;(void)x;(void)y;(void)list; }
static void enj_render_list_add(int list,void (*fn)(void *),void *p) { (void)list;(void)fn;(void)p; }
static int nm_name_render_release(void) { return 1; }
static int nm_name_labels_unload(void) { return 1; }
static void nm_pack_unload(nm_pack_t *p) { (void)p; }
static int nm_name_labels_enqueue(int seconds) { assert(seconds>=-1 && seconds<=20);return 1; }
static int nm_name_render_enqueue_intro(const nm_drive_params_t *p,const int16_t *c,unsigned n) { (void)p;(void)c;assert(n<32);return 1; }
static nm_name_entry_t drawn;
static int nm_name_render_enqueue(const nm_name_entry_t *s,const nm_name_marker_t *m,
 const nm_drive_params_t *p,const int16_t *c,uint32_t f) { (void)m;(void)p;(void)c;(void)f;drawn=*s;return 1; }
'''
checks=r'''
int main(void) {
    assert(nm_name_scene_init(&scene,&nm_name_layout_data));
    for(int remaining=31;remaining>=0;--remaining) {
        assert(nm_name_scene_step(&scene,&nm_name_layout_data,127,1)==1);
        assert(scene.draw_intro==remaining && scene.entry.count==0);
        assert(scene.remaining==1280);
        assert(!scene.commands[0] && !scene.commands[1]);
    }
    for(int tick=0;tick<1280;++tick) {
        assert(nm_name_scene_step(&scene,&nm_name_layout_data,-127,0)==1);
        assert(scene.draw_seconds==(unsigned)(1280-tick)/64);
    }
    assert(scene.remaining==0 && !scene.finish_requested);
    assert(nm_name_scene_step(&scene,&nm_name_layout_data,-127,0)==2);
    assert(scene.remaining==-1 && scene.draw_seconds==0);
    assert(nm_name_scene_step(&scene,&nm_name_layout_data,-127,1)==2);
    assert(scene.remaining==-1 && !scene.entry.count);
    frames=32;control.joyx=-128;control.button.B=3;update(NULL);
    assert(interactive && !failed && scene.entry.index==0 && scene.entry.glyph=='A');
    control.button.B=0;control.button.A=3;update(NULL);
    assert(!drawn.count && scene.entry.count==1 && scene.entry.name[0]=='A');
    control.button.A=2;update(NULL); /* Held, not a new edge. */
    assert(drawn.count==1 && scene.entry.count==1);
    control.button.A=0;control.joyx=-111;update(NULL);
    assert(drawn.index==0 && scene.entry.index==2 && scene.entry.glyph=='C');
    control.button.A=3;update(NULL);
    assert(drawn.count==1 && scene.entry.count==2 && scene.entry.name[1]=='C');
    control.button.A=0;control.joyx=89;update(NULL);
    assert(scene.entry.glyph=='}');control.button.A=3;update(NULL);
    assert(scene.entry.count==1 && !scene.entry.name[1]);
    control.button.A=0;control.joyx=127;update(NULL);
    control.button.A=3;update(NULL);
    assert(scene.finish_requested && scene.entry.count==1 && !failed && !engine.flags.shut_down);
    control.joyx=0;update(NULL);assert(scene.entry.glyph=='{'); /* Finished preview freezes input. */
    assert(!scene.commands[0] && !scene.commands[1]);
    control.button.B=3;control.button.A=0;update(NULL);
    assert(!interactive && !scene.finish_requested && !failed);
    frames=900;control.button.B=0;update(NULL);assert(engine.flags.shut_down);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    source=Path(directory)/'check.c';source.write_text(harness+updater+checks)
    binary=Path(directory)/'check'
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-I',str(root/'include'),
                    str(source),str(root/'code/gameplay/name_scene.c'),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: scene intro/countdown/timeout, preview input, draw-before-input, clamp, press/hold, back, END and auto exit')
