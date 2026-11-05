"""Slot machine lockstep: the native race (tests/trace_race_native.c, production mode) against the original's runs with
START presses (spec: build/dc-native-world-anim.*/spec-slot.md section 8).

usage: test_slot_lockstep.py TRACE_RACE_NATIVE ASSET_DIR "TRACE_DIR[;TRACE_DIR...]" [--short]

Each TRACE_DIR is searched for the reference traces (m2segment --trace-race, ROM output, never committed):
  reference-finish-fast.trace  press-free fast race (summary must stay hash=af7f84a6 commands=107 remaining=434)
  reference-slot-A.trace       inputs-slot-A.txt: BAR BAR BAR at 2697 (+320), restart, an undrawn press, no second pay
  reference-slot-B.trace       inputs-slot-B.txt: 7 7 7 at 2749 (+448)
  reference-slot-C-3200.trace  inputs-slot-C.txt: reel 1 stops at 64 (draws "7", pays nothing), held press = one edge
  reference-slot-D-3200.trace  inputs-slot-D.txt: an undrawn press is lost, then 7 7 7
  reference-slot-E-4000.trace  inputs-slot-E.txt: BAR BAR BAR in lap 2, clock running (+319 in that tick)
Native row i (tick i) pairs with reference row n = 4+i (FIRST_ROW 5). `remaining` must be equal on every paired row,
the bonus must land on the expected row, the end summary must match, and no race tick may show a reference 5010b0
other than 2368+tick (the counter base the port assumes). --short skips the two full races with presses (A, B).
Exits 77 when no trace is found.
"""
import re, subprocess, sys, tempfile
from pathlib import Path

FIRST = 5
# name: (reference file, (bonus native row, remaining before, remaining after) or None, end remaining, commands, hash)
CASES = {
    'baseline': ('reference-finish-fast.trace', None, 434, 107, 'af7f84a6'),
    # The event tail's commands (17 October 2026: with the wall rub 501a04, the time checks and the wrong-way
    # cue 501a02 the original writes, 81 -> 108); every sound command of both races equals the original's
    # (trace_race_native --sound against m2segment --trace-sound, tools/compare_race_sound.py: 778/778).
    'A': ('reference-slot-A.trace', (114, 3200, 3520), 754, 108, 'f4facc26'),
    'B': ('reference-slot-B.trace', (166, 3200, 3648), 882, 108, 'ae4da87f'),
    'C': ('reference-slot-C-3200.trace', None, 3147, 0, '811c9dc5'),
    'D': ('reference-slot-D-3200.trace', (166, 3200, 3648), 3595, 1, 'f260beb2'),
    'E': ('reference-slot-E-4000.trace', (1146, 2618, 2937), 2667, 14, 'f011026f'),
}
ROW = re.compile(rb'^T n=(\d+) .*? remaining=(\d+) ')


def remaining_rows(path):
    out = {}
    with open(path, 'rb') as f:
        for line in f:
            if line.startswith(b'T '):
                m = ROW.match(line)
                out[int(m.group(1))] = int(m.group(2))
    return out


def main():
    args = [a for a in sys.argv[1:] if a != '--short']
    short = '--short' in sys.argv[1:]
    if len(args) != 3:
        print(__doc__); return 2
    harness, assets, dirs = args[0], args[1], [Path(d) for d in args[2].split(';') if d]
    failures = ran = 0
    for name, (file, bonus, end_remaining, commands, digest) in CASES.items():
        if short and name in ('A', 'B'):
            continue
        ref = next((d / file for d in dirs if (d / file).is_file()), None)
        if ref is None:
            print(f'{name}: no {file}, skipped'); continue
        ran += 1
        with tempfile.TemporaryDirectory() as tmp:
            nat = Path(tmp) / 'native.trace'
            run = subprocess.run([harness, assets, str(ref), str(nat), str(FIRST), '14000', 'production'],
                                 capture_output=True, text=True)
            if run.returncode:
                print(f'FAIL {name}: harness exit {run.returncode}\n{run.stdout}{run.stderr}'); failures += 1; continue
            native = remaining_rows(nat)
        reference = remaining_rows(ref)
        problems = []
        paired = differ = 0; first = None
        for i, value in sorted(native.items()):
            n = FIRST - 1 + i
            if n not in reference:
                continue
            paired += 1
            if reference[n] != value:
                differ += 1
                if first is None:
                    first = (i, n, reference[n], value)
        if differ:
            problems.append(f'remaining differs on {differ}/{paired} rows, first native row {first[0]} (reference n={first[1]}): '
                            f'reference {first[2]} native {first[3]}')
        if paired < len(native) - 1:
            problems.append(f'only {paired} of {len(native)} native rows paired')
        if bonus:
            row, before, after = bonus
            if (native.get(row - 1), native.get(row)) != (before, after):
                problems.append(f'bonus row {row}: expected {before}->{after}, got {native.get(row - 1)}->{native.get(row)}')
        summary = next((l for l in run.stdout.splitlines() if l.startswith('native_race_trace ')), '')
        want = f'remaining={end_remaining} '
        if want not in summary or f'commands={commands} hash={digest} ' not in summary:
            problems.append(f'summary: expected {want}commands={commands} hash={digest}, got: {summary}')
        if 'slot counter:' in run.stdout:
            problems.append(next(l for l in run.stdout.splitlines() if l.startswith('slot counter:')))
        if problems:
            failures += 1
            print(f'FAIL {name}: ' + '; '.join(problems))
        else:
            print(f'{name}: remaining equal on {paired}/{paired} rows; {summary.split(" coverage")[0]}')
    if not ran:
        print('SKIP: no reference trace found'); return 77
    print('PASS' if not failures else f'FAIL: {failures} of {ran} runs')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
