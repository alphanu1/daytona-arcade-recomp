"""NATIVE_DIAG_DROP_LOG=1 (code/practice.c) on the host: the drop log's own code, cut from practice.c, replays a
synthetic frame timeline built with KOS's PVR rules (pvr_irq.c: a render starts when its lists are in and the
previous render has been flipped; the flip is the first vblank after a render completes; pvr_wait_ready returns
when the previous scene's render started), compiled with ASan/UBSan. The pipeline gives the CPU slack: registration k may
end after the previous flip as long as that delay plus the render time fits in a vblank period, so the designed
CPU frames are long enough to miss it (render 10 ms: more than about 6.7 ms late). Frames designed to be late for one reason
must be logged with that cause; on-time frames must not be logged; tools/drop_log_report.py must read the output.

Event bookkeeping: the same timeline with a repeated registration-end event while a render waits for its flip, and
with one registration-end event missing (each event of version 1's log counted on its own: either left every later
scene unmatched). Every frame must still be counted, the late ones found, the anomalies counted.

NATIVE_DIAG_LATE_CAPTURE (late_capture.c compiled in, 320x240 and full resolution): a model of KOS's two frame
buffers (each render writes the buffer not on display, garbage while it runs, its scene number when done; the flip
event reads the frame buffer address before the display switches). Every picture written must hold the scene it
claims (late frames and the frames shown before them), none torn; the file must parse with
tools/late_capture_report.py, which must make its PNGs and contact sheet.
"""
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'code/practice.c').read_text()
shared = source.split('#if defined(NM_DIAG_DROP_LOG) || defined(NM_DIAG_PMU)\n/* Measurement builds', 1)[1]
shared = '/* Measurement builds' + shared.split('#endif', 1)[0]
drop = source.split('#ifdef NM_DIAG_DROP_LOG\n/* NATIVE_DIAG_DROP_LOG=1', 1)[1]
drop = '/* NATIVE_DIAG_DROP_LOG=1' + drop.split('\n#endif\n#if defined(NM_DIAG_DROP_LOG) || defined(NM_DIAG_PMU)', 1)[0]

PERIOD = 16683000
MS = 1000000


def timeline(frames, repeat_regdone=(), drop_regdone=()):
    """frames: (U, R, P, ticks) in ns -> sorted events (time, priority, kind, argument). X: the frame's last
    punch-through callback, just before its registration ends."""
    events, cpu, prev_rnd_start, prev_flip, tick = [], 0, 0, 0, 0
    last_flip = 0
    for k, (u, r, p, ticks) in enumerate(frames):
        upd_start = cpu
        upd_end = upd_start+u
        reg_start = max(upd_end, prev_rnd_start)
        reg_done = reg_start+r
        rnd_start = max(reg_done, prev_flip)
        rnd_done = rnd_start+p
        flip = (rnd_done//PERIOD+1)*PERIOD
        tick += ticks
        events += [(upd_start, 3, 'B', 0), (upd_end, 3, 'E', ticks), (reg_start, 4, 'S', 4), (reg_done-1000, 4, 'X', 0),
                   (rnd_start, 2, 'S', 6), (rnd_done, 0, 'S', 7), (flip, 1, 'S', 8)]
        # Registration end before a render start at the same time (one interrupt: REGDONE, then the render).
        if k not in drop_regdone:
            events.append((reg_done, 2, 'S', 5))
        if k in repeat_regdone:
            assert rnd_start > reg_done+2000, 'the repeat must come while the render waits'
            events.append((reg_done+1000, 2, 'S', 5))
        prev_rnd_start, prev_flip, cpu = rnd_start, flip, reg_done+50000
        last_flip = flip
    events += [((v+1)*PERIOD, 0, 'S', 1) for v in range(last_flip//PERIOD)]
    return sorted(events)


normal = (3*MS, 9*MS, 10*MS, 1)
frames = [normal]*40
designed = {}
def put(index, frame, cause):
    frames[index] = frame
    designed[sum(f[3] for f in frames[:index+1])] = cause
put(10, (3*MS, 9*MS, 19*MS, 1), 'pvr')
put(18, (24*MS, 9*MS, 10*MS, 2), 'cpu-update')
put(20, (3*MS, 26*MS, 10*MS, 1), 'cpu-registration')   # late capture builds: "capture" (its registration held a copy)
put(26, (3*MS, 26*MS, 10*MS, 1), 'cpu-registration')
put(34, (400*MS, 9*MS, 10*MS, 1), 'load')
frames += [normal]*10
# The repeat comes while render 5 waits for frame 4's flip (registration 4 ms: done long before it).
frames[5] = (3*MS, 4*MS, 10*MS, 1)

harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NM_DIAG_DROP_LOG 1
enum { NM_SAMPLE_PHASE_OTHER, NM_SAMPLE_PHASE_ROLLING, NM_SAMPLE_PHASE_RACE, NM_SAMPLE_PHASE_FINISH,
       NM_SAMPLE_PHASE_PODIUM, NM_SAMPLE_PHASE_RESULTS, NM_SAMPLE_PHASE_ATTRACT };
static struct {
    struct { uint32_t ticks; unsigned phase; struct { struct { int32_t camera_mode; uint8_t camera_transition; } car; } cars[1]; } race;
    struct { int32_t timer; uint8_t phase; } podium;
    struct { uint32_t ticks; } results;
    struct { uint32_t ticks; } intro;
    int attract_active;
} practice;
static uint64_t now_ns;
static uint64_t timer_ns_gettime64(void) { return now_ns; }
static unsigned real_calls;
void __real_pvr_sync_stats(int event) { (void)event; ++real_calls; }
/* Two frame buffers in a fake VRAM; the address register as KOS's vid_set_start leaves it. */
enum { FB_BYTES=640*480*3, FB_A=0x1000, FB_B=FB_A+FB_BYTES };
static uint8_t *fake_vram;
static uint32_t fake_fb_addr=FB_A,render_target,renders;
#define DROP_FB_ADDR() (fake_fb_addr)
static void fill(uint32_t at,uint8_t b,uint8_t g,uint8_t r) {
    for(uint32_t i=0;i<640u*480u;++i) { fake_vram[at+3*i]=b;fake_vram[at+3*i+1]=g;fake_vram[at+3*i+2]=r; }
}
''' + shared + drop + r'''
int main(int argc,char **argv) {
    fake_vram=calloc(1,FB_B+FB_BYTES);
#ifdef NM_DIAG_LATE_CAPTURE
    nm_late_host_vram=fake_vram;
    if(!nm_late_reserve()) return 2;
#endif
    practice.race.phase=22;practice.race.ticks=700;practice.race.cars[0].car.camera_mode=2;
    unsigned long long t;char kind;int arg;
    while(scanf("%llu %c %d",&t,&kind,&arg)==3) {
        now_ns=t;
        if(kind=='B') drop_updater_begin();
        else if(kind=='E') { practice.race.ticks+=(uint32_t)arg;drop_updater_end(); }
        else if(kind=='X') {
#ifdef NM_DIAG_LATE_CAPTURE
            drop_capture_point();
#endif
        } else {
            __wrap_pvr_sync_stats(arg);
            /* The PVR: a render writes the buffer not on display (garbage until done, then its scene number:
             * red = low 5 bits, green = next 6, blue 0xa8); the flip shows it. */
            if(arg==6) { render_target=fake_fb_addr==FB_A ? FB_B : FB_A;++renders;fill(render_target,0xff,0xff,0xff); }
            if(arg==7) fill(render_target,0xa8,(uint8_t)(((renders>>5)&63u)<<2),(uint8_t)((renders&31u)<<3));
            if(arg==8) fake_fb_addr=fake_fb_addr==FB_A ? FB_B : FB_A;
        }
    }
    drop_print();
#ifdef NM_DIAG_LATE_CAPTURE
    if(argc>1) drop_capture_write(argv[1]);
#else
    (void)argc;(void)argv;
#endif
    free(fake_vram);
    if(!real_calls) return 1;
    return 0;
}
'''


def build(tmp, name, defines=()):
    (tmp/(name+'.c')).write_text(harness)
    extra = [str(root/'code/late_capture.c')] if defines else []
    subprocess.run([os.environ.get('HOST_CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-O1', '-fsanitize=address,undefined', '-I', str(root/'include'), *defines, str(tmp/(name+'.c')), *extra,
                    '-o', str(tmp/name)], check=True)
    return tmp/name


def run(binary, events, *arguments):
    text = '\n'.join(f'{t} {kind} {arg}' for t, _, kind, arg in events)+'\n'
    return subprocess.run([str(binary), *map(str, arguments)], input=text, capture_output=True, text=True, check=True).stdout


def summary_of(out):
    return dict(re.findall(r'(\w[\w-]*)=(\d+)', next(l for l in out.splitlines() if l.startswith('DROP-SUMMARY'))))


def check_log(out, label, expect=designed):
    records = [dict(re.findall(r'(\w+)=(\S+)', l)) for l in out.splitlines() if l.startswith('DROP seq=')]
    got = {int(r['tick'])-700: r['cause'] for r in records}
    for tick, cause in expect.items():
        assert got.get(tick) == cause, (label, tick, cause, got, out)
    summary = summary_of(out)
    late = int(summary['late'])
    # A late frame can make the next one late too (the CPU waits for the late flip): at most one follower each.
    assert len(designed) <= late <= 2*len(designed), (label, out)
    assert int(summary['frames']) == len(frames)-1, (label, out)
    assert int(summary['flips']) == int(summary['displayed']) == len(frames), (label, out)
    assert sum(int(summary[c]) for c in ('pvr', 'cpu-update', 'cpu-registration', 'cpu-other', 'load', 'other', 'capture')) == late
    header = next(l for l in out.splitlines() if l.startswith('DROP-LOG v2'))
    assert 'period_ns=16683000' in header, (label, header)
    return summary, late


with tempfile.TemporaryDirectory(prefix='drop-log-') as tmp:
    tmp = Path(tmp)
    plain = build(tmp, 'check')
    out = run(plain, timeline(frames))
    (tmp/'console.log').write_text(out)
    summary, late = check_log(out, 'plain')
    assert int(summary['unmatched']) == 0 and not any(int(summary[a]) for a in (
        'regdone_repeat', 'regdone_stray', 'rndstart_early', 'rndstart_stray', 'rnddone_stray', 'flip_stray')), out
    report = subprocess.run([sys.executable, str(root/'tools/drop_log_report.py'), str(tmp/'console.log')],
                            capture_output=True, text=True, check=True).stdout
    assert 'why (cause of each late frame)' in report and 'cpu-update' in report, report
    # Bookkeeping: a repeated registration end (frame 5) and a missing one (frame 30).
    out = run(plain, timeline(frames, repeat_regdone={5}))
    summary, _ = check_log(out, 'repeated registration end')
    assert int(summary['regdone_repeat']) == 1 and int(summary['unmatched']) == 0, out
    out = run(plain, timeline(frames, drop_regdone={30}))
    summary, _ = check_log(out, 'missing registration end')
    assert int(summary['rndstart_early']) == 1 and int(summary['unmatched']) == 1, out

    sys.path.insert(0, str(root/'tools'))
    import late_capture_report
    pictures = {}
    for label, defines, slots, size in (('320x240', (), 6, (320, 240)), ('full', ('-DNM_LATE_CAPTURE_FULL_RES=1',), 3, (640, 480))):
        binary = build(tmp, 'capture-'+label, ('-DNM_DIAG_LATE_CAPTURE=2', '-DNM_LATE_CAPTURE_FIRST=2',
                                               f'-DNM_LATE_CAPTURE_SLOTS={slots}', '-DNM_LATE_CAPTURE_NAME="lc-'+label+'"', *defines))
        out = run(binary, timeline(frames), str(tmp)+'/')
        (tmp/f'console-{label}.log').write_text(out)
        # The frame designed late right after the copies of the cpu-update frame's pictures: "capture", not captured.
        expect = dict(designed)
        expect[sum(f[3] for f in frames[:21])] = 'capture'
        summary, late_frames = check_log(out, label, expect)
        capture = dict(re.findall(r'(\w[\w-]*)=(\d+)', next(l for l in out.splitlines() if l.startswith('LATE-CAPTURE v1'))))
        header, names, records, raws = late_capture_report.parse(tmp/f'lc-{label}.bin')
        assert (header['width'], header['height']) == size, header
        late_records = {int(r['seq']): r for r in (dict(re.findall(r'(\w+)=(\S+)', l)) for l in out.splitlines()
                                                   if l.startswith('DROP seq='))}
        kept = [r for r in records if r['kind_name'] != 'before']
        # first 2 eligible (pvr, cpu-update), then the worst as slots allow; the load frame is not eligible.
        eligible = [s for s, r in sorted(late_records.items()) if r['cause'] not in ('load', 'capture')]
        assert int(capture['affected']) >= 1 and int(capture['eligible']) == len(eligible), (label, capture)
        assert [r['seq'] for r in kept if r['kind_name'] == 'first'] == eligible[:2], (label, kept, eligible)
        assert len(kept) == min(len(eligible), slots) or slots < len(eligible), (label, kept)
        for r, raw in zip(records, raws):
            rgb = late_capture_report.to_rgb(header, raw)
            assert (rgb == rgb[0, 0]).all(), (label, r)
            red, green, blue = (int(c) for c in rgb[0, 0])
            assert blue == 0xad or blue == 0xa8, (label, r, rgb[0, 0])   # 0xa8 through 565 and back: 0xad
            seq = (red >> 3) | ((green >> 2) << 5)
            assert seq == r['seq'] and not r['torn'] and r['copy_scene'] in (r['seq']+1, r['seq']+2), (label, r, seq)
            if r['kind_name'] == 'before':
                assert r['pair_seq'] == r['seq']+1 and r['pair_seq'] in [k['seq'] for k in kept], (label, r)
            else:
                assert late_records[r['seq']]['cause'] == r['cause_name'] and int(late_records[r['seq']]['vbl']) == r['vbl'], r
                assert late_records[r['seq']]['img'] == r['kind_name'], (label, r)
        if label == '320x240':
            # Six slots: room for every "before" picture. Late by the CPU: known when the next scene's registration
            # ends (the render started after the vblank it should have made). Late by the render (19 ms): not yet.
            assert sorted(r['pair_seq'] for r in records if r['kind_name'] == 'before') == \
                [r['seq'] for r in kept if r['cause_name'].startswith('cpu')], (label, records)
        assert int(capture['images']) == len(records) and int(capture['torn']) == 0, (label, capture)
        assert (tmp/f'lc-{label}.csv').read_text().count('\n') == len(records)+1
        made = subprocess.run([sys.executable, str(root/'tools/late_capture_report.py'), str(tmp/f'lc-{label}.bin'),
                               '--out', str(tmp/f'png-{label}'), '--log', str(tmp/f'console-{label}.log')],
                              capture_output=True, text=True, check=True).stdout
        pngs = sorted((tmp/f'png-{label}').glob('0*.png'))
        assert len(pngs) == len(records) and (tmp/f'png-{label}'/'contact.png').exists(), made
        pictures[label] = (len(kept), len(records)-len(kept))
print(f'PASS: drop log causes {sorted(designed.values())} logged at their ticks; {late} late frames of {len(frames)-1}; '
      'no on-time frame logged; repeated and missing registration-end events counted without losing frames; report parsed; '
      f'late capture (late, before pictures) {pictures}: every picture holds its own scene, none torn, PNGs made (ASan/UBSan)')
