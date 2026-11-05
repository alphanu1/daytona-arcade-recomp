"""Front-end lockstep: the native course/transmission select and the start state it hands to the race,
against the original's runs that go through the attract, the game start and the select screens with
different timings and choices.

usage: test_frontend_lockstep.py TRACE_FRONTEND_NATIVE TRACE_RACE_NATIVE ASSET_DIR "TRACE_DIR[;TRACE_DIR...]"

Each TRACE_DIR is searched for frontend-X.trace (m2segment --trace-frontend) and race-X.trace (--trace-race of
the same run) of the inputs recipes/inputs/frontend-X.txt (ROM output, never committed):
  A  credit at frame 1290, Beginner by the accelerator, automatic by the timer        (race)
  B  credit at frame 390, both selects left to time out                              (race)
  C  credit at frame 690 (cinematic stage change), Advanced, manual; an early
     accelerator edge the transmission select ignores                              (select only)
  D  credit at frame 1090 (leading lap 0), wheel between the courses, Expert by the
     timer, manual by the accelerator                                              (select only)
  E  credit at frame 1690, Beginner, manual, race with contacts                     (race)
1. trace_frontend_native on every frontend-X.trace: each select iteration (from phase 17's fall-through to the race
   entry) equal in phase-18 substate and timer, the select object's state and timers, slot, course,
   transmission, time attack, and the iteration count.
2. For the race runs, trace_race_native in production mode with --frontend frontend-X.trace: the native attract
   to the credit, the game start (frontend.h) and the select give the race its start state (the carried
   collision words of all 40 cars, the leading lap 5014ba, the frame counter base, the transmission). Native row i
   pairs with reference row n = 4+i (FIRST_ROW 5); on every row, every field of every car row (player and the 39
   opponents: pose, speed, decisions, flags, damage, and the collision tracking words sx/sz/tx/tz/cside/crel that
   the cold-start lockstep tests skip) and the race timeline (the lap display word aside, as in
   test_manual_lockstep.py) must be equal. Not compared: body/tilt of the
   opponents (presentation, test_damage_lockstep.py), and the reference rows sampled mid-tick at the race's load
   (native rows 2-4).
Exits 77 when no trace is found."""
import subprocess, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_damage_lockstep import rows, FIRST, NOT_COMPARED, OPPONENT_PRESENTATION, TIMELINE

# As test_manual_lockstep.py and test_pit_lockstep.py: the lap display word aside.
TIMELINE = [k for k in TIMELINE if k != 'display']

SELECT_RUNS = 'ABCDE'
RACE_RUNS = 'ABE'
MID_TICK = (2, 3, 4)
RACE_TICKS = '3000'


def compare_race(reference, native):
    reference_t, reference_c, reference_o = reference
    native_t, native_c, native_o = native
    paired = fields = 0
    for i in sorted(native_c):
        n = FIRST - 1 + i
        if i in MID_TICK or n not in reference_c or n not in reference_t or i not in native_t:
            continue
        paired += 1
        for car in range(40):
            a = reference_c[n] if car == 0 else reference_o.get((n, car))
            b = native_c[i] if car == 0 else native_o.get((i, car))
            if a is None or b is None:
                return paired, fields, f'row {i}: car {car} missing on one side'
            skip = NOT_COMPARED | (OPPONENT_PRESENTATION if car else set())
            keys = [k for k in a if k in b and k not in skip]
            fields += len(keys)
            differ = [(k, a[k], b[k]) for k in keys if a[k] != b[k]]
            if differ:
                return paired, fields, f'native row {i} (reference {n}) car {car}: {differ[:6]}'
        for k in TIMELINE:
            fields += 1
            if reference_t[n][k] != native_t[i][k]:
                return paired, fields, f'native row {i} (reference {n}) timeline {k}: {reference_t[n][k]} vs {native_t[i][k]}'
    return paired, fields, None


def main():
    if len(sys.argv) != 5:
        print(__doc__); return 2
    select_harness, race_harness, assets = sys.argv[1], sys.argv[2], sys.argv[3]
    dirs = [Path(d) for d in sys.argv[4].split(';') if d]
    def find(name):
        return next((d / name for d in dirs if (d / name).is_file()), None)
    found = 0
    problems = []
    for run in SELECT_RUNS:
        trace = find(f'frontend-{run}.trace')
        if trace is None:
            continue
        found += 1
        r = subprocess.run([select_harness, str(trace)], capture_output=True, text=True)
        if r.returncode:
            problems.append(f'{run} select: {r.stdout}{r.stderr}'.strip())
        else:
            print(f'{run}: {r.stdout.strip()}')
    for run in RACE_RUNS:
        front, race = find(f'frontend-{run}.trace'), find(f'race-{run}.trace')
        if front is None or race is None:
            continue
        found += 1
        with tempfile.TemporaryDirectory() as tmp:
            nat = Path(tmp) / 'native.trace'
            r = subprocess.run([race_harness, assets, str(race), str(nat), str(FIRST), RACE_TICKS, 'production',
                                '--frontend', str(front)], capture_output=True, text=True)
            if r.returncode:
                problems.append(f'{run} race: harness exit {r.returncode}\n{r.stdout}{r.stderr}'); continue
            native = rows(nat)
        start = next((l for l in r.stdout.splitlines() if l.startswith('front end:')), '')
        paired, fields, first = compare_race(rows(race), native)
        if first:
            problems.append(f'{run} race: {first}')
        elif paired < 1000:
            problems.append(f'{run} race: only {paired} rows paired')
        else:
            print(f'{run}: race rows equal {paired}, fields {fields} (tracking words included); {start}')
    if not found:
        print('no front-end traces, skipped'); return 77
    for p in problems:
        print('FAIL:', p)
    if problems:
        return 1
    print('PASS front-end lockstep')
    return 0


if __name__ == '__main__':
    sys.exit(main())
