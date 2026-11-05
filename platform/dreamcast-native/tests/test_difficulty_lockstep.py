"""Difficulty lockstep: the native race (tests/trace_race_native.c, production mode with --difficulty, as code/practice.c
takes the difficulty from the native settings) against the original's races recorded with that operator setting.

usage: test_difficulty_lockstep.py TRACE_RACE_NATIVE ASSET_DIR "TRACE_DIR[;TRACE_DIR...]"

The reference traces (m2segment --trace-race --nvram DIR, DIR written by tools/settings_eeprom.py --difficulty D;
ROM output, never committed) found in the TRACE_DIRs are checked; at least one must exist (else exit 77):
  reference-difficulty-{easy,hard,hardest}.trace  recipes/inputs/inputs-finish-fast.txt, 17500 frames: 8 laps,
      2nd place, podium; remaining at the finish 982/362/297 (start 3200/3072/2944)
  reference-timeout-{easy,normal,hardest}.trace   scripts/inputs/race_to_end.txt, 12000 frames: the accelerator is
      let go at frame 6000 and the race runs out of time (the time-out path 37c0, results)
  reference-ninth-{easy,normal,hard,hardest}.trace  the 9th-place script (tests/drive_race_native.c), 17500 frames.
      In carry mode (--start-lap -1): its contact of opponent 11 on the tick it enters the 14-car window (row 1503)
      reads the collision tracking words the original carries from the scene before the race, which a cold start
      zeroes (platform/dreamcast-native/HANDOFF.md, "Open"); that is no difficulty matter.
The difficulty (53e59c) sets the start time (3860: 2870304[course][d]) and every checkpoint award (13574..135b0),
nothing else the race reads. Native row i (tick i) pairs with reference row n = 4+i (FIRST_ROW 5). Asserted:
  - every row of the reference shows the difficulty of its name (the harness checks it too);
  - the player's car row (id 0) in every field both sides have but the carried collision tracking words of rows
    0-4, and the race timeline of the T row (test_pit_lockstep.py's list: `display` aside), on every native row up to
    the podium handoff (its row included) or, without a podium, on every native row (results included);
  - the opponents 1-39 in every field but body/tilt and the tracking words, on the same rows but the mid-tick
    reference rows 2-4 (test_damage_lockstep.py).
"""
import subprocess, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_damage_lockstep import rows, compare_opponents, FIRST, NOT_COMPARED, START_CARRY, TRACKING, TIMELINE

TIMELINE = [k for k in TIMELINE if k != 'display']
MID_TICK_START = (2, 3, 4)
DIFFICULTY = {'easy': 0, 'normal': 1, 'hard': 2, 'hardest': 3}
SCRIPTS = {'difficulty': ['production'], 'timeout': ['production'], 'ninth': ['carry', '--start-lap', '-1']}


def check(harness, assets, ref, difficulty, mode):
    reference_t, reference_c, reference_o = rows(ref)
    problems = []
    if any(t['difficulty'] != str(DIFFICULTY[difficulty]) for t in reference_t.values()):
        problems.append(f'the reference is not a {difficulty} race on every row')
    with tempfile.TemporaryDirectory() as tmp:
        nat = Path(tmp) / 'native.trace'
        run = subprocess.run([harness, assets, str(ref), str(nat), str(FIRST), '14000'] + mode +
                             ['--difficulty', difficulty], capture_output=True, text=True)
        if run.returncode:
            return [f'harness exit {run.returncode}\n{run.stdout}{run.stderr}'], ''
        native_t, native_c, native_o = rows(nat)
    handoff = next((i for i in sorted(native_t) if native_t[i]['phase'] == '27'), None)
    last = handoff if handoff is not None else max(native_c)
    paired = fields = 0
    first = None
    for i in sorted(native_c):
        if i > last:
            break
        n = FIRST - 1 + i
        if n not in reference_c or n not in reference_t or i not in native_t:
            problems.append(f'native row {i} has no reference row {n}'); break
        paired += 1
        a, b = reference_c[n], native_c[i]
        skip = START_CARRY if i < 5 else set()
        keys = [k for k in a if k in b and k not in NOT_COMPARED and k not in skip]
        keys += [f'T.{k}' for k in TIMELINE]
        fields += len(keys)
        differ = [k for k in keys if (reference_t[n][k[2:]] != native_t[i][k[2:]] if k.startswith('T.')
                                      else a[k] != b[k])]
        if differ and first is None:
            first = (i, n, differ)
    if first:
        i, n, differ = first
        shown = ', '.join(f'{k}=ref:{(reference_t[n] if k.startswith("T.") else reference_c[n])[k.removeprefix("T.")]}'
                          f'/nat:{(native_t[i] if k.startswith("T.") else native_c[i])[k.removeprefix("T.")]}'
                          for k in differ[:6])
        problems.append(f'first difference at native row {i} (reference n={n}): {shown}')
    opponent_fields, opponent_first = compare_opponents(reference_o, native_o, last + 1, MID_TICK_START, TRACKING)
    if opponent_first:
        i, n, car, differ = opponent_first
        problems.append(f'opponent {car} differs at native row {i} (reference n={n}): ' +
                        ', '.join(f'{k}=ref:{x}/nat:{y}' for k, x, y in differ))
    end = native_t[last]
    summary = (f'{paired}/{paired} rows ({fields} player and timeline fields, {opponent_fields} opponent fields), '
               f'{"podium at row " + str(handoff) if handoff is not None else "no podium, last row " + str(last)}, '
               f'phase {end["phase"]} remaining {end["remaining"]} clock {end["clock"]}')
    return problems, summary


def main():
    if len(sys.argv) != 4:
        print(__doc__); return 2
    harness, assets, dirs = sys.argv[1], sys.argv[2], [Path(d) for d in sys.argv[3].split(';') if d]
    cases = []
    for script in SCRIPTS:
        for difficulty in DIFFICULTY:
            ref = next((d / f'reference-{script}-{difficulty}.trace' for d in dirs
                        if (d / f'reference-{script}-{difficulty}.trace').is_file()), None)
            if ref is not None:
                cases.append((script, difficulty, ref, SCRIPTS[script]))
    if not cases:
        print('no reference-{difficulty,timeout,ninth}-*.trace, skipped'); return 77
    failed = 0
    for script, difficulty, ref, mode in cases:
        problems, summary = check(harness, assets, ref, difficulty, mode)
        if problems:
            failed += 1
            for p in problems:
                print(f'FAIL {script} {difficulty}: {p}')
        else:
            print(f'PASS {script} {difficulty} ({mode[0]}): {summary}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
