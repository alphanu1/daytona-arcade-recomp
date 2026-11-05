"""Execute the actual result updater with offline tables and fake render callbacks."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
data = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="daytona-results-") as tmp:
    tmp = Path(tmp)
    (tmp / "practice_scene.h").write_text('''
#include "practice.h"
#include "race_scene.h"
#include "gameplay/results.h"
#include <assert.h>
typedef struct nm_practice_scene {
    nm_practice_t state;
    nm_race_scene_t race;
    nm_results_t results;
    nm_drive_params_t *math;
    struct { int16_t center[2]; } instance;
    int failed;
    const char *asset_root;
    uint32_t name_notification;
    nm_race_record_t *name_record;
} nm_practice_scene_t;
static unsigned releases,shutdowns,labels,glyphs,names;
static int render_ok=1,glyph_ok=1,probe_pending,name_ok=1;
static unsigned pending;
static void nm_practice_render_release(void) { assert(!pending); ++releases; }
void nm_practice_game_over(void) {} /* the pool and the frame counter (frontend.h) */
/* audio/sound.h: every counted command is also handed to sound. */
static unsigned sound_commands;
static uint32_t sound_hash=UINT32_C(2166136261);
static void nm_sound_command(uint32_t command) { ++sound_commands; sound_hash=(sound_hash^command)*UINT32_C(16777619); }
static void enj_state_flag_shutdown(void *unused) { (void)unused; ++shutdowns; }
/* audio/race_sound_hooks.h (code/audio/race_sound.c): counted in the results' counters, then to sound. */
static void hook_counted(uint32_t *commands,uint32_t *hash,uint32_t command) {
    ++*commands; *hash=(*hash^command)*UINT32_C(16777619); nm_sound_command(command);
}
static void nm_race_sound_hook_results_off(uint32_t *commands,uint32_t *hash) {
    hook_counted(commands,hash,0xbe1700u); hook_counted(commands,hash,0xbe1900u);
}
static void nm_race_sound_hook_results_gated(const nm_race_scene_t *race,uint32_t *commands,uint32_t *hash,uint32_t command) {
    if(command && (race->shared.flags&0x500000u)) hook_counted(commands,hash,command);
}
static void nm_race_sound_hook_events(const nm_race_scene_t *race,uint32_t frame_counter) { (void)race;(void)frame_counter; }
static int nm_name_mode_start(const char *root,nm_drive_params_t *math,uint32_t notification,nm_race_record_t *record,uint16_t best,unsigned completed,int *failed) {
(void)root;(void)math;(void)notification;(void)record;(void)best;(void)completed;(void)failed;
    assert(!pending);++names;return name_ok;
}
static int nm_result_labels_enqueue(unsigned rank,uint32_t clock,int16_t best,int16_t scroll) {
    (void)rank; (void)clock; (void)best; (void)scroll; ++labels;
    if(probe_pending) ++pending; /* May fail after queueing part of its work. */
    return render_ok;
}
static int nm_result_render_enqueue(const nm_result_actor_t *actor,const nm_drive_params_t *math,const int16_t *center) {
    (void)actor; (void)math; (void)center; ++glyphs; return glyph_ok;
}
/* code/practice.c: the race field's results tick (4110/1210c) and the checkpoint row, once per results tick
 * after its phase handler: phase 25 first, then 26; collision phase (race ticks + earlier results ticks) & 3. */
static unsigned field_ticks;
int nm_practice_results_tick(struct nm_practice_scene *s,unsigned phase,unsigned collision,nm_race_intent_output_t *out) {
    assert(phase==(field_ticks ? 26u : 25u) && collision==((s->race.ticks+field_ticks)&3u) && s->results.ticks==field_ticks+1);
    ++field_ticks; out->count=0; return 1;
}
''')
    for name in ("render/scene/practice.h", "render/ui/result_render.h", "render/ui/result_labels.h", "gameplay/name_mode.h",
                 "audio/sound.h", "audio/race_sound_hooks.h"):
        header = tmp / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text("/* Renderer stub declarations live in practice_scene.h. */\n")
    checks = r'''
#include <assert.h>
int main(void) {
    static nm_practice_scene_t scene;
    scene.race.phase=22;
    assert(!nm_results_update(&scene,NM_PRACTICE_TICK_US));
    assert(!labels && !glyphs && !releases && !shutdowns);
    scene.race.phase=25; scene.race.cars[0].rank=1;
    scene.results.command_hash=UINT32_C(2166136261);
    assert(nm_results_update(&scene,0));
    assert(scene.race.phase==25 && !scene.results.ticks);
    assert(nm_results_update(&scene,NM_PRACTICE_TICK_US));
    assert(scene.race.phase==26 && scene.results.mode==1);
    assert(scene.results.timer==800 && scene.results.scroll==512);
    assert(scene.results.ticks==1 && labels==1 && !glyphs);
    assert(nm_results_update(&scene,10*NM_PRACTICE_TICK_US));
    assert(scene.results.ticks==5 && scene.results.timer==796);
    assert(scene.state.dropped_us==6*NM_PRACTICE_TICK_US);
    while(scene.race.phase==26) {
        assert(scene.results.ticks<1000);
        assert(nm_results_update(&scene,NM_PRACTICE_TICK_US));
        assert(!scene.failed && !releases && !shutdowns);
    }
    assert(scene.race.phase==29 && scene.results.ticks==674 && field_ticks==674);
    assert(scene.race.ticks==0 && scene.state.ticks==0);
    assert(scene.results.playback.clock==449 && glyphs && labels);
    assert(sound_commands && sound_commands==scene.results.commands && sound_hash==scene.results.command_hash);
    const unsigned drawn=labels+glyphs;
    assert(nm_results_update(&scene,NM_PRACTICE_TICK_US));
    assert(releases==1 && names==1 && shutdowns==0 && labels+glyphs==drawn);
    for(unsigned failure=0;failure<2;++failure) {
        scene.race.phase=26; scene.failed=0; probe_pending=1;
        render_ok=failure==0; glyph_ok=0;
        assert(nm_results_update(&scene,0));
        assert(scene.failed && pending==1 && releases==1+failure && shutdowns==failure);
        pending=0; /* enDjinn finishes callbacks before the next updater. */
        assert(nm_results_update(&scene,0));
        assert(releases==2+failure && shutdowns==1+failure);
    }
    scene.race.phase=29;scene.failed=0;name_ok=0;
    const unsigned stopped=shutdowns;
    assert(nm_results_update(&scene,0));
    assert(scene.failed && shutdowns==stopped+1 && names==2);
    puts("PASS: actual result updater, 674-tick lifecycle with the race field tick on each, partial enqueue failures retain resources until next updater");
}
'''
    source = tmp / "check.c"
    source.write_text('#include "' + str(root / "code/gameplay/results.c") + '"\n' + checks)
    exe = tmp / "check"
    subprocess.run([os.environ.get("HOST_CC", "cc"), "-std=c11", "-O1", "-g",
                    "-fsanitize=address,undefined",
                    "-I", str(tmp), "-I", str(root / "include"), "-I", str(data),
                    str(source), "-lm", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
