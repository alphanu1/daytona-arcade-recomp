"""ROM-free check of the actual PMU scope accumulator, including48-bit wrap."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'code/render/models/model_renderer.c').read_text()
scope = source.split('#elif defined(NATIVE_CACHE_PROFILE)\n', 1)[1].split('#else\n', 1)[0]
with tempfile.TemporaryDirectory(prefix='render-pmu-') as directory:
    directory = Path(directory)
    test = directory/'check.c'
    test.write_text('''
#include <assert.h>
#include <stdint.h>
#define PRFC1 1
static uint64_t ns, events;
static unsigned reads;
static uint64_t perf_cntr_timer_ns(void) { ++reads; return ns; }
static uint64_t perf_cntr_count(unsigned counter) { ++reads; assert(counter==PRFC1); return events; }
''' + scope + '''
int main(void) {
    draw_cache_counter_owned=1;
    assert(draw_cache_counter_owned);
    draw_cache_sampling=1;
    draw_cache_samples=1;
    assert(draw_cache_samples==1);
    for(unsigned source=0;source<2;++source) for(unsigned phase=0;phase<13;++phase) {
        DRAW_PROFILE_SOURCE(source);
        ns=100;events=(1ULL<<48)-4;
        DRAW_PHASE_START(start);
        ns=150;events=2;
        DRAW_PHASE_NEXT(phase,start,next);
        ns=170;events=5;
        DRAW_PHASE_END(phase,next);
        assert(draw_cache_time[source][phase]==70 && draw_cache_stalls[source][phase]==9);
    }
    DRAW_PROFILE_SOURCE(0);
    ns=1000;events=100;
    DRAW_PHASE_START(outer);
    DRAW_PHASE_START(inner);
    DRAW_PROFILE_SOURCE(1);
    ns=1050;events=110;
    DRAW_PHASE_END(8,inner);
    ns=1100;events=120;
    DRAW_PHASE_END(6,outer);
    assert(draw_cache_time[0][6]-draw_cache_time[0][8]==50);
    assert(draw_cache_stalls[0][6]-draw_cache_stalls[0][8]==10);
    assert(draw_cache_time[1][6]==70 && draw_cache_stalls[1][6]==9);
    draw_cache_sampling=0;
    const unsigned before=reads;
    DRAW_PHASE_START(disabled);
    ns+=100;events+=100;
    DRAW_PHASE_END(0,disabled);
    assert(draw_cache_time[0][0]==70 && draw_cache_stalls[0][0]==9);
    assert(draw_cache_time[1][0]==70 && draw_cache_stalls[1][0]==9);
    assert(reads==before);
}
''')
    executable = directory/'check'
    subprocess.run([os.environ.get('HOST_CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-O1', '-fsanitize=address,undefined', str(test),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('PASS paired PMU/time scopes0..12, car/world isolation,48-bit wrap, zero disabled reads and nested subtraction')

practice = (root/'code/practice.c').read_text()
paused = practice.index('    if (practice.inspect_paused) {')
flip = practice.split('#ifdef NATIVE_FRAME_PROFILE\n    if(practice.intervals>64)', 1)[1].split('#endif', 1)[0]
flip = '    if(practice.intervals>64)' + flip
assert practice.index(flip) > paused
assert practice.index(flip) < practice.index('    if (practice.intervals>=5 && practice.intervals<=600)')
declarations = practice.split('static size_t frame_last_flip,frame_last_vblank;', 1)[1].split('#endif', 1)[0]
with tempfile.TemporaryDirectory(prefix='render-page-flip-') as directory:
    directory = Path(directory)
    test = directory/'check.c'
    test.write_text('''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
static struct {unsigned intervals;} practice;
typedef struct {size_t frame_count,vbl_count;uint64_t frame_last_time;} pvr_stats_t;
static pvr_stats_t next;
static unsigned calls;
static int failure;
static int pvr_get_stats(pvr_stats_t *stats) {++calls;*stats=next;return failure;}
static size_t frame_last_flip,frame_last_vblank;
''' + declarations + '\nstatic void sample(void) {\n' + flip + '''
}
int main(void) {
    next=(pvr_stats_t){100,100,16640000};practice.intervals=64;sample();
    assert(!calls && !frame_last_flip && !frame_flip_samples);
    practice.intervals=65;sample();
    assert(calls==1 && frame_last_flip==100 && frame_last_vblank==100);
    assert(!frame_flips && !frame_vblanks && !frame_flip_samples);
    sample();assert(calls==2 && !frame_flips && !frame_flip_samples);
    next=(pvr_stats_t){101,101,16640000};sample();
    assert(frame_flips==1 && frame_vblanks==1 && frame_flip_samples==1);
    assert(frame_flip_sum_ns==16640000 && frame_flip_max_ns==16640000 && !frame_flip_over_20ms);
    sample();assert(frame_flips==1 && frame_flip_samples==1 && frame_flip_sum_ns==16640000);
    next=(pvr_stats_t){102,103,33280000};sample();
    assert(frame_flips==2 && frame_vblanks==3 && frame_flip_samples==2);
    assert(frame_flip_sum_ns==49920000 && frame_flip_max_ns==33280000 && frame_flip_over_20ms==1);
    failure=1;next=(pvr_stats_t){200,300,99999999};sample();
    assert(frame_last_flip==102 && frame_last_vblank==103);
    assert(frame_flips==2 && frame_vblanks==3 && frame_flip_samples==2);
    assert(frame_flip_sum_ns==49920000 && frame_flip_max_ns==33280000 && frame_flip_over_20ms==1);
}
''')
    executable = directory/'check'
    subprocess.run([os.environ.get('HOST_CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-O1', '-fsanitize=address,undefined', str(test),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('PASS: actual PVR flip accumulator warmup, initialization, duplicate counts, missed refresh and stats failure')
