"""Manual transmission lockstep: the native race (tests/trace_race_native.c, production mode with --transmission
manual) against the original's manual race.

usage: test_manual_lockstep.py TRACE_RACE_NATIVE ASSET_DIR "TRACE_DIR[;TRACE_DIR...]"

Each TRACE_DIR is searched for reference-manual.trace (m2segment --trace-race, 17500 frames, of
recipes/inputs/inputs-manual.txt; ROM output, never committed): the race_to_end menu with the wheel right through
the transmission select (frames 2010-2390: 501464 = 1), then a race searched by tests/drive_race_native.c --manual,
8 laps to 3rd place with 20 one-frame shifts (4-3-4) on the cabinet shifter, neutral in between.
Native row i (tick i) pairs with reference row n = 4+i (FIRST_ROW 5). Asserted on every race row (native rows up
to the podium handoff, its row included; the podium rows print the podium scene's pool on the reference side):
  - every row of the reference has manual=1, and the native start gives cars 0-4 the manual body list 288eb4c
    (e5ac: 286fab0[501464]);
  - every field the player's car row (id 0) has on both sides (pose, speed, gear, rpm, brake word, desired speed,
    path, flags, damage, body +14, crash count, shake, pitch, collision tracking but the carried words of rows 0-4),
    and the race timeline of the T row (test_pit_lockstep.py's list: `display` aside);
  - the opponents 1-39 in every field but body/tilt (presentation) and the collision tracking words, on every race
    row but the mid-tick reference rows 2-4 (test_damage_lockstep.py);
  - the shifter is exercised: native gear changes on at least 16 ticks after the rolling start, each on a tick whose
    reference shifter (gearsel) names the new gear.
Exits 77 when no trace is found."""
import subprocess, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_damage_lockstep import rows, compare_opponents, FIRST, NOT_COMPARED, START_CARRY, TRACKING, TIMELINE

TIMELINE = [k for k in TIMELINE if k != 'display']
MID_TICK_START = (2, 3, 4)
GEAR_OF_SELECTOR = {5: 0, 6: 1, 1: 2, 2: 3}
MANUAL_BODY = '288eb4c'


def main():
    if len(sys.argv) != 4:
        print(__doc__); return 2
    harness, assets, dirs = sys.argv[1], sys.argv[2], [Path(d) for d in sys.argv[3].split(';') if d]
    ref = next((d / 'reference-manual.trace' for d in dirs if (d / 'reference-manual.trace').is_file()), None)
    if ref is None:
        print('no reference-manual.trace, skipped'); return 77
    reference_t, reference_c, reference_o = rows(ref)
    problems = []
    if any(t['manual'] != '1' for t in reference_t.values()):
        problems.append('the reference is not a manual race on every row')
    with tempfile.TemporaryDirectory() as tmp:
        nat = Path(tmp) / 'native.trace'
        run = subprocess.run([harness, assets, str(ref), str(nat), str(FIRST), '14000', 'production',
                              '--transmission', 'manual'], capture_output=True, text=True)
        if run.returncode:
            print(f'FAIL: harness exit {run.returncode}\n{run.stdout}{run.stderr}'); return 1
        native_t, native_c, native_o = rows(nat)
    handoff = next((i for i in sorted(native_t) if native_t[i]['phase'] == '27'), None)
    if handoff is None:
        problems.append('the native race never hands off to the podium'); handoff = max(native_c) + 1
    for car in range(5):
        body = (native_c[0] if car == 0 else native_o[0, car])['body']
        if body != MANUAL_BODY:
            problems.append(f'native start body of car {car} is {body}, not {MANUAL_BODY}')
    paired = fields = shifts = 0
    first = None
    for i in sorted(native_c):
        if i > handoff:
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
        if i and i - 1 in native_c and b['gear'] != native_c[i - 1]['gear'] and reference_t[n]['rolling'] == '0':
            if GEAR_OF_SELECTOR.get(int(reference_t[n]['gearsel'])) != int(b['gear']):
                problems.append(f'native row {i}: gear {b["gear"]} without the shifter naming it')
            shifts += 1
    if first:
        i, n, differ = first
        shown = ', '.join(f'{k}=ref:{(reference_t[n] if k.startswith("T.") else reference_c[n])[k.removeprefix("T.")]}'
                          f'/nat:{(native_t[i] if k.startswith("T.") else native_c[i])[k.removeprefix("T.")]}'
                          for k in differ[:6])
        problems.append(f'first difference at native row {i} (reference n={n}): {shown}')
    if shifts < 16:
        problems.append(f'only {shifts} manual gear changes')
    opponent_fields, opponent_first = compare_opponents(reference_o, native_o, handoff + 1, MID_TICK_START, TRACKING)
    if opponent_first:
        i, n, car, differ = opponent_first
        problems.append(f'opponent {car} differs at native row {i} (reference n={n}): ' +
                        ', '.join(f'{k}=ref:{x}/nat:{y}' for k, x, y in differ))
    if problems:
        for p in problems:
            print(f'FAIL: {p}')
        return 1
    print(f'PASS manual race: player and timeline equal on {paired}/{paired} race rows ({fields} fields), '
          f'{shifts} manual gear changes, opponents 1-39 equal ({opponent_fields} fields, tracking words not '
          f'compared), start body {MANUAL_BODY} on cars 0-4')
    return 0


if __name__ == '__main__':
    sys.exit(main())
