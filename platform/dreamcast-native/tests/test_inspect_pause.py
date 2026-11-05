"""Run the real updater prefix: frozen frames render, never advance or free early."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
game = (root / "code/practice.c").read_text()
prefix = game.split("static void practice_update(void *unused) {", 1)[1]
prefix = prefix.split("    if (practice.intervals>=5", 1)[0]
source = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
static struct {
    int failed,inspect_paused;
    uint64_t last_us,interval_sum_us,interval_max_us,frame_window_us;
    unsigned intervals,frame_tenths_ms;
    uint64_t work_last_us,work_window_us,render_window_us;
    unsigned work_tenths_ms,render_tenths_ms,late_frames;
    size_t late_flip,late_vblank;
    struct { uint32_t missed; uint8_t primed; } frame_clock;
} practice;
/* frame_clock.h: one tick per displayed frame; paused frames sample it like any other. */
#define NM_PRACTICE_TICK_US 17384u
static unsigned frame_ticks_calls;
static uint32_t nm_frame_ticks(void *clock) { (void)clock; return ++frame_ticks_calls>1; }
/* Budget indicator inputs: the prefix only reads these PVR counters. */
typedef struct { uint64_t reg_last_time,rnd_last_time; size_t frame_count,vbl_count; } pvr_stats_t;
static int pvr_get_stats(pvr_stats_t *stats) { *stats=(pvr_stats_t){0}; return 0; }
static struct { struct { int shut_down; } flags; } engine;
static unsigned renders,releases,advances,sound_updates;
/* Sound (audio/sound.h) keeps time on every updater call, paused frames included. */
static void nm_sound_update(void) { ++sound_updates; }
static int render_ok=1,resume;
static uint64_t now;
#define enj_state_get() (&engine)
static void nm_practice_render_release(void) { ++releases; }
static void enj_state_flag_shutdown(void *p) { (void)p; engine.flags.shut_down=1; }
static uint64_t timer_ns_gettime64(void) { return now*1000; }
static int nm_practice_render_enqueue(void *p) {
    assert(p==&practice); ++renders;
    if(resume) practice.inspect_paused=0;
    return render_ok;
}
static void update(void *unused) {
''' + prefix + r'''
    (void)frame_us; /* the simulation after the prefix steps it */
    ++advances; /* All simulation/reset/finite-test code follows the prefix. */
}
int main(void) {
    practice.inspect_paused=1;
    for(unsigned i=1;i<=600;++i) { now=i*17000; update(0); }
    assert(renders==600 && !advances && !releases && practice.last_us==now);
    resume=1; now+=17000; update(0);
    assert(!advances && !practice.inspect_paused);
    now+=17000; update(0);
    assert(advances==1 && practice.interval_max_us==17000);
    practice.inspect_paused=1; render_ok=0; resume=0;
    now+=17000; update(0);
    assert(practice.failed && !releases && advances==1);
    update(0); assert(releases==1 && engine.flags.shut_down);
    assert(sound_updates==604);
}
'''
with tempfile.TemporaryDirectory(prefix="daytona-inspect-pause-") as tmp:
    path = Path(tmp)
    (path / "check.c").write_text(source)
    subprocess.run([os.environ.get("HOST_CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", "-fsanitize=address,undefined", str(path / "check.c"),
                    "-o", str(path / "check")], check=True)
    subprocess.run([str(path / "check")], check=True)
print("PASS: 600 frozen renders, no simulation/catch-up, deferred failure cleanup")
