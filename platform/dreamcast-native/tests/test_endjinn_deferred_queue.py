"""Run the real enDjinn callback scheduler with checked, host-only PVR stubs.

Usage: python3 test_endjinn_deferred_queue.py /path/to/enDJinn
Checks queue timing/lifetime/order, NOT SH4/PVR rendering or performance.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).resolve() / "code/enj_render.c"
assert source.is_file(), source
header = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdalign.h>
typedef int pvr_list_t;
typedef int pvr_palfmt_t;
enum { PVR_LIST_OP_POLY, PVR_LIST_OP_MOD, PVR_LIST_TR_POLY,
       PVR_LIST_TR_MOD, PVR_LIST_PT_POLY };
typedef struct { void (*mode_updater)(void *); void *data; } enj_mode_t;
#define ENJ_DEBUG_PRINT(...) ((void)0)
int pvr_wait_ready(void);
void pvr_scene_begin(void);
void pvr_list_begin(int);
void pvr_list_finish(void);
void pvr_scene_finish(void);
int pvr_wait_render_done(void);
void pvr_set_pal_format(int);
void enj_render_list_add(int,void (*)(void *),void *);
void enj_render_next_frame(enj_mode_t *);
void enj_render_post_callback_set(void (*)(void *),void *);
"""
harness = r"""
#include <enDjinn/enj_enDjinn.h>
#include <assert.h>
#include <stdio.h>
static int ready,opened,list=-1,next_list,prepared,post_count;
static struct { unsigned count,submitted,pt,generation; } scene;
static struct item { unsigned index,generation; } items[130];
int pvr_wait_ready(void) {
    assert(!opened && !prepared && list==-1); ready=1; return 0;
}
void pvr_scene_begin(void) { assert(ready && !opened); opened=1; }
void pvr_list_begin(int next) {
    assert(opened && list==-1 && next==next_list++); list=next;
}
void pvr_list_finish(void) { assert(opened && list>=0); list=-1; }
void pvr_scene_finish(void) {
    assert(opened && list==-1 && next_list==5); opened=0;
}
int pvr_wait_render_done(void) { assert(opened); return 0; }
void pvr_set_pal_format(int format) { (void)format; assert(opened); }
static void draw_op(void *data) {
    const struct item *item=data;
    assert(opened && list==PVR_LIST_OP_POLY && prepared==1);
    assert(item->generation==scene.generation);
    assert(item->index==scene.submitted++);
}
static void draw_pt(void *data) {
    assert(data==&scene && opened && list==PVR_LIST_PT_POLY);
    assert(scene.submitted==scene.count); ++scene.pt;
}
static void prepare(void *data) {
    assert(data==&scene && opened && ready && list==PVR_LIST_OP_POLY);
    assert(!prepared && !scene.submitted); ++prepared;
    for(unsigned i=0;i<scene.count;++i) {
        items[i]=(struct item){i,scene.generation};
        enj_render_list_add(PVR_LIST_OP_POLY,draw_op,items+i);
    }
    enj_render_list_add(PVR_LIST_PT_POLY,draw_pt,&scene);
    assert(!scene.submitted && !scene.pt); /* queued, not called recursively */
}
static void update(void *data) {
    assert(data==&scene && !opened && !ready && !prepared);
    ++scene.generation;
    enj_render_list_add(PVR_LIST_OP_POLY,prepare,&scene);
    assert(!prepared && !scene.submitted && !scene.pt);
}
static void post(void *data) {
    assert(data==&scene && !opened && list==-1);
    assert(prepared==1 && scene.pt==1 && scene.submitted==scene.count);
    ++post_count;
}
int main(void) {
    const unsigned counts[]={130,0,1,62,63,64,65,2};
    enj_mode_t mode={update,&scene};
    enj_render_post_callback_set(post,&scene);
    for(unsigned i=0;i<sizeof(counts)/sizeof(counts[0]);++i) {
        ready=opened=next_list=prepared=0; list=-1;
        scene.count=counts[i]; scene.submitted=scene.pt=0;
        enj_render_next_frame(&mode);
        assert(post_count==(int)i+1);
    }
    puts("PASS: actual enDjinn deferred OP append/PT order, borrowed data, 64-entry boundaries and frame clearing");
}
"""
with tempfile.TemporaryDirectory(prefix="daytona-enj-queue-") as temporary:
    work = Path(temporary)
    (work / "dc").mkdir()
    (work / "enDjinn").mkdir()
    (work / "dc/video.h").write_text("/* PVR is stubbed by the harness. */\n")
    (work / "enDjinn/enj_enDjinn.h").write_text(header)
    (work / "malloc.h").write_text(
        "#include <stdlib.h>\n"
        "static inline void *memalign(size_t a,size_t n) {\n"
        " void *p=NULL; return posix_memalign(&p,a,n) ? NULL : p; }\n")
    (work / "check.c").write_text(harness)
    executable = work / "check"
    subprocess.run([os.environ.get("HOST_CC", "cc"), "-std=c11",
                    "-D_POSIX_C_SOURCE=200112L", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(work),
                    str(source), str(work / "check.c"), "-o", str(executable)], check=True)
    # enDjinn intentionally retains its callback blocks for subsequent frames.
    subprocess.run([str(executable)], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
