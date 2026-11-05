"""Run the gameplay harness compiled by sh-elf-gcc in the GDB SH simulator (sh-elf-run).

tests/trace_race_native.c and code/{drive,contact,road,path,visibility}.c, built with the console's SH4 code
generation flags (KOS_CFLAGS: -m4-single -ml -mfsrra -mfsca, function/data sections; -fno-fast-math
-ffp-contract=off -include finite.h) plus one variant's optimisation flags, replay the scripted races in the
simulator; the checkpoint rows are compared byte for byte with the expected files the console logs are compared
with. This is the SH4 compiler's output for the gameplay C, executed instruction by instruction.

Not covered: the __DREAMCAST__ paths (SH4ZAM helpers in a few headers). The GDB SH simulator does not implement
FTRV (it prints "ftrv xmtrx, FV8" and leaves the vector unchanged), so the harness is built without
__DREAMCAST__, i.e. with the portable C forms the host harness uses. Those paths are inline assembly and
intrinsics whose meaning no optimisation flag changes; the console checkpoint comparison covers them.

Instruction counts: the simulator prints 32-bit counters. The count of the first N ticks (default 4000) of the
fast race, minus a 1-tick run (asset load and reference parsing), stays below 2^32 and is exact; the whole-race
count is unwrapped with the per-tick rate of that window and labelled an estimate.

usage: sh4_gameplay_sim.py WORK_DIR VARIANT [VARIANT ...]   (repo root; VARIANT = name=flags, flags separated by
       commas, e.g. o3=-O3  lto=-O3,-flto  o2=-O2)
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[3]
NATIVE = REPO/'platform/dreamcast-native'
LOCKSTEP = REPO/'build/dc-native-race-lockstep7.hoCZmg'
ASSETS = REPO/'build/dc-native-road-filter.H1hcIW/assets-nomip'
INCLUDES = ['build/dc-native-race-assets.uhUxz3', 'build/dc-native-announcement-assets.Ho8t4S',
            'build/dc-native-race-lockstep7.hoCZmg/race-start-data', 'build/dc-native-record-defaults.V34YTh',
            'build/dc-native-podium-data.TTjJG8']
SOURCES = ['tests/trace_race_native.c'] + [f'code/{n}.c' for n in ('drive', 'contact', 'road', 'path', 'visibility')]
def sh_tool(name):
    """sh-elf-NAME: $KOS_NAME (as sample_profile_report.py), the KOS default path, then PATH."""
    for candidate in (os.environ.get('KOS_'+name.upper()), f'/opt/toolchains/dc/sh-elf/bin/sh-elf-{name}',
                      shutil.which(f'sh-elf-{name}')):
        if candidate and Path(candidate).exists():
            return candidate
    raise SystemExit(f'sh-elf-{name} not found; set KOS_{name.upper()}')
KOS_CODEGEN = ['-std=gnu23', '-fno-PIC', '-fno-PIE', '-fomit-frame-pointer', '-m4-single', '-ml', '-mfsrra', '-mfsca',
               '-ffunction-sections', '-fdata-sections', '-matomic-model=soft-gusa', '-fno-fast-math', '-ffp-contract=off']
# Hottest gameplay routines of the updater section (build/dc-native-cell-streams.SMXQl9/hw/report-stream-prof.txt,
# self time by linked function); in the harness most of them are inlined into main or keep their own name.
HOT = ['nm_car_guided_motion', 'nm_opponent_finish_step_inplace', 'nm_force_angle', 'nm_contact_contains',
       'nm_race_traffic_step', 'nm_intro_speed_step', 'nm_car_detail_step', 'nm_player_longitudinal_step',
       'nm_contact_cell', 'nm_contact_query', 'nm_player_rolling_path_step', 'nm_car_part_pose', 'nm_intro_scene_step',
       'nm_collision_pair', 'nm_race_collide', 'nm_collision_prepare', 'nm_race_scene_step', 'main']
WRAP = 2**32


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def filtered_reference(work, race):
    out = work/f'reference-{race}.T.trace'
    if not out.exists():
        with open(LOCKSTEP/f'reference-finish-{race}.trace', 'rb') as src, open(out, 'wb') as dst:
            for line in src:
                if line.startswith(b'T ') or line.startswith(b'C n=4 '):
                    dst.write(line)
    return out


def build(work, name, flags, first_includes=()):
    elf = work/f'trace-{name}.elf'
    # The simulator support is compiled on its own, never with LTO (GNU ld's --wrap does not reach wrappers
    # defined inside LTO objects).
    support = []
    for src in ('shim', 'discard'):
        obj = work/f'{src}.o'
        run([sh_tool('gcc'), *KOS_CODEGEN, '-O2', '-c', str(NATIVE/f'tests/sh4sim/{src}.c'), '-o', str(obj)])
        support.append(str(obj))
    cmd = [sh_tool('gcc'), *KOS_CODEGEN, *flags, '-include', str(NATIVE/'include/finite.h'), '-I'+str(NATIVE/'include'),
           *['-I'+str(Path(i).resolve()) for i in first_includes],
           *['-I'+str(REPO/i) for i in INCLUDES], *[str(NATIVE/s) for s in SOURCES], *support,
           '-Wl,--gc-sections', '-Wl,--defsym=_stack=0x07fffff0',
           '-Wl,--wrap=fopen,--wrap=fprintf,--wrap=fputs,--wrap=fputc', '-lm', '-o', str(elf)]
    if '-flto' in flags:
        cmd.insert(-2, '-flto=4')
    subprocess.run(cmd, check=True, stderr=subprocess.DEVNULL if os.environ.get('QUIET') else None)
    return elf


def simulate(work, elf, race, ticks, tag):
    out = work/f'{tag}.discard'
    cp = work/f'{tag}.checkpoints'
    for f in (out, cp):
        f.unlink(missing_ok=True)
    before = resource.getrusage(resource.RUSAGE_CHILDREN).ru_utime
    res = subprocess.run([sh_tool('run'), '-v', str(elf), str(ASSETS), str(filtered_reference(work, race)), str(out),
                          '+5', f'+{ticks}', 'production', '--checkpoints', str(cp)], capture_output=True, text=True)
    cpu = resource.getrusage(resource.RUSAGE_CHILDREN).ru_utime-before
    text = res.stdout+res.stderr
    insns = int(re.search(r'# instructions executed\s+(-?\d+)', text).group(1)) % WRAP
    cycles = int(re.search(r'# cycles\s+(-?\d+)', text).group(1)) % WRAP
    line = re.search(r'native_race_trace .*', text)
    out.unlink(missing_ok=True)
    return {'insns32': insns, 'cycles32': cycles, 'seconds': round(cpu, 2), 'summary': line.group(0) if line else None,
            'exit': res.returncode, 'checkpoints': cp}


def sizes(elf):
    text = subprocess.run([sh_tool('size'), '-A', str(elf)], capture_output=True, text=True, check=True).stdout
    total = sum(int(m.group(1)) for m in re.finditer(r'^\.text\S*\s+(\d+)', text, re.M))
    nm = subprocess.run([sh_tool('nm'), '-S', '--size-sort', str(elf)], capture_output=True, text=True, check=True).stdout
    funcs = {}
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in 'tT':
            base = parts[3].lstrip('_').split('.')[0]
            funcs[base] = funcs.get(base, 0)+int(parts[1], 16)
    return total, funcs


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('work', type=Path)
    parser.add_argument('variants', nargs='+')
    parser.add_argument('--window', type=int, default=4000, help='ticks of the exact instruction count (fast race)')
    parser.add_argument('--races', default='fast,tenth')
    parser.add_argument('--include', action='append', default=[],
                        help='header directory searched before the built-in ones (e.g. a recipe\'s '
                             'headers/race-start-data: race_start_data.h with the pit tables, include/pit.h)')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    results = []
    for spec in args.variants:
        name, _, flags = spec.partition('=')
        flags = [f for f in flags.split(',') if f]
        elf = build(args.work, name, flags, args.include)
        total, funcs = sizes(elf)
        row = {'variant': name, 'flags': flags, 'text_bytes': total, 'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest()[:16],
               'hot_bytes': {f: funcs.get(f, 0) for f in HOT}}
        base = simulate(args.work, elf, 'fast', 1, f'{name}-fast-1')
        window = simulate(args.work, elf, 'fast', args.window, f'{name}-fast-{args.window}')
        exact = window['insns32']-base['insns32']
        row['window_ticks'] = args.window
        row['window_insns'] = exact
        row['window_cycles'] = (window['cycles32']-base['cycles32']) % WRAP
        row['window_insns_per_tick'] = round(exact/(args.window-1))
        for race in args.races.split(','):
            full = simulate(args.work, elf, race, 14000, f'{name}-{race}')
            expected = LOCKSTEP/f'expected-checkpoints-finish-{race}.txt'
            same = full['checkpoints'].exists() and full['checkpoints'].read_bytes() == expected.read_bytes()
            ticks = int(re.search(r'ticks=(\d+)', full['summary']).group(1)) if full['summary'] else 0
            # Unwrap with the simulator's speed in the window run (instructions per CPU second of sh-elf-run): the
            # 32-bit counter wraps every 4.29e9 instructions, about 13 s of simulation.
            rate = window['insns32']/max(window['seconds'], 1e-3)
            guess = rate*full['seconds']
            whole = full['insns32']+WRAP*round((guess-full['insns32'])/WRAP)
            row[race] = {'checkpoints_identical': same, 'ticks': ticks, 'summary': full['summary'],
                         'insns_estimate': whole-base['insns32'], 'sim_seconds': full['seconds'],
                         'rate_guess': round(guess), 'wraps': round((whole-full['insns32'])/WRAP)}
            print(f'{name} {race}: checkpoints {"IDENTICAL" if same else "DIFFERENT"} to {expected.name}; '
                  f'{ticks} ticks, about {(whole-base["insns32"])/1e9:.3f}e9 instructions', flush=True)
        print(f'{name}: text {total} bytes; ticks 2..{args.window} of the fast race: {exact} instructions '
              f'({row["window_insns_per_tick"]} a tick), {row["window_cycles"]} simulator cycles', flush=True)
        results.append(row)
        out = args.work/'results.json'
        old = json.loads(out.read_text()) if out.exists() else []
        old = [r for r in old if r['variant'] != name]
        out.write_text(json.dumps(old+[row], indent=1, default=str))


if __name__ == '__main__':
    main()
