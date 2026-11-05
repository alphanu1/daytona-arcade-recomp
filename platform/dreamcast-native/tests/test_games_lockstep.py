"""Games one after another: the native scenes chained as the console runs them, against the original's runs of more
than one game, or of a game started from the V.R. demo or the title.

usage: test_games_lockstep.py TRACE_RACE_NATIVE TRACE_ATTRACT_NATIVE ASSET_DIR "TRACE_DIR[;TRACE_DIR...]"

Each TRACE_DIR is searched for frontend-X.trace and race-X.trace of the inputs recipes/inputs/games-X.txt (ROM output,
never committed): m2segment ROM N --inputs games-X.txt --trace-frontend frontend-X.trace --trace-race race-X.trace
--trace-attract, N the script's frames.
  A  first game, credit in the title of the first loop                    Beginner
  B  first game, credit in the V.R. demo of the first loop                Beginner
  C  Beginner, contacts, the time-out at speed; cinematic credit          Beginner
  D  Beginner; cinematic credit                                           Advanced, manual
  E  Advanced; cinematic credit                                           Beginner
  F  Expert; cinematic credit                                             Advanced
  G  Beginner; credit in the ranking overlay                              Beginner
  H  Beginner; credit in the V.R. demo                                    Beginner
  I  Beginner; credit in the title                                        Beginner
  J  Beginner to the finish: podium, name entry; cinematic credit         Beginner
For each game of a run, from power-on: trace_attract_native (--enter the state the game before left, --credit) runs the
attract to the credit and the game start, trace_race_native (--enter, --frontend, --leave) the select and the race
through its results; the state files carry only the pool, the counter and the course (tests/game_chain.h).
Checked, for each game:
  1. tools/compare_race_trace.py --require-equal (--podium for a finish on the podium): every car on every row;
  2. every field of every car row and the race timeline, as tests/test_frontend_lockstep.py, tracking words included
     (the lap display word aside, as there), up to the podium;
  3. tests/test_attract_lockstep.py on the attract before it, tick by tick, the V.R. demo included.
Not compared, the rows the reference samples inside a pass at the race's load: native rows 2-4 of a rolling start
(a run's sample can fall inside a car's update, which compare_race_trace.py's evidence rule does not cover), rows 0-1
of a standing start. Row 0 of a rolling start is compared.
Exits 77 when no trace is found."""
import subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_damage_lockstep import rows, NOT_COMPARED, OPPONENT_PRESENTATION, TIMELINE

TIMELINE = [k for k in TIMELINE if k != 'display']
RUNS = 'ABCDEFGHIJ'
ROLLING_MID_TICK = (2, 3, 4)
STANDING_MID_TICK = (0, 1)
ATTRACT_LOOPS = '4'


def games(trace):
    """[(first row with active 20, the row of the game's return to phase 2 or None, course, podium)], by T rows."""
    found, last = [], None
    with open(trace) as source:
        for line in source:
            if not line.startswith('T '):
                continue
            values = dict(p.split('=', 1) for p in line.split()[1:] if '=' in p)
            n, active = int(values['n']), values['active']
            if active == '20' and last != n - 1:
                found.append([n, None, int(values['course']), False])
            if active == '20':
                last = n
            if found and found[-1][1] is None:
                if active == '2':
                    found[-1][1] = n
                elif active in ('27', '28'):
                    found[-1][3] = True
    return found


def compare(reference, native, first, mid_tick, podium):
    """Every field, as test_frontend_lockstep.compare_race: (rows, fields, first difference or None)."""
    reference_t, reference_c, reference_o = reference
    native_t, native_c, native_o = native
    paired = fields = 0
    for i in sorted(native_c):
        if podium and native_t[i]['active'] in ('27', '28', '25', '26', '29'):
            break
        n = first - 1 + i
        if i in mid_tick or n not in reference_c or n not in reference_t or i not in native_t:
            continue
        paired += 1
        for car in range(int(native_t[i]['count'])):
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


def run(command):
    return subprocess.run([str(c) for c in command], capture_output=True, text=True)


def check_run(name, race_harness, attract_harness, assets, frontend, race, directory):
    problems = []
    reference = None
    enter = []
    python = sys.executable
    played = games(race)
    if not played:
        return [f'{name}: no game in the reference']
    for g, (first, over, course, podium) in enumerate(played):
        game = f'{name} game {g + 1}'
        before, state = directory / f'attract-{g}.trace', directory / f'attract-{g}.state'
        r = run([attract_harness, assets, before, ATTRACT_LOOPS] + enter + ['--credit', frontend, '--leave', state])
        if r.returncode:
            return problems + [f'{game} attract: exit {r.returncode}\n{r.stdout}{r.stderr}']
        credit = r.stdout.strip().splitlines()[-1]
        r = run([python, HERE / 'test_attract_lockstep.py', race, before])
        if r.returncode:
            problems.append(f'{game} attract lockstep:\n{r.stdout[-1500:]}')
        attract_line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ''
        native, left = directory / f'native-{g}.trace', directory / f'game-{g}.state'
        ticks = (over - first) if over is not None else 12000 if g + 1 == len(played) else played[g + 1][0] - first
        command = [race_harness, assets, race, native, first, ticks, 'production', '--course', course,
                   '--enter', state, '--frontend', frontend]
        if over is not None:
            command += ['--leave', left]
        r = run(command)
        if r.returncode:
            return problems + [f'{game} race: exit {r.returncode}\n{r.stdout}{r.stderr}']
        rolling = not course
        mid_tick = ROLLING_MID_TICK if rolling else STANDING_MID_TICK
        skip = ','.join(str(i) for i in ((0,) + mid_tick if rolling else mid_tick))
        tool = [python, HERE.parent / 'tools/compare_race_trace.py', race, native, '--first', first, '--require-equal',
                '--skip-rows', skip] + (['--podium'] if podium else [])
        r = run(tool)
        tail = [l for l in r.stdout.splitlines() if l.startswith('require-equal:')]
        if r.returncode or not tail:
            problems.append(f'{game} compare_race_trace: {tail or r.stdout[-800:]}')
        if reference is None:
            reference = rows(race)
        paired, fields, difference = compare(reference, rows(native), first, mid_tick, podium)
        if difference:
            problems.append(f'{game} race: {difference}')
        elif paired < 1000:
            problems.append(f'{game} race: only {paired} rows paired')
        else:
            print(f'{game}: {credit}; course {course}; {tail[0] if tail else ""}; {paired} rows, {fields} fields equal; {attract_line[:60]}')
        native.unlink()
        before.unlink()
        enter = ['--enter', left]
    return problems


def main():
    if len(sys.argv) != 5:
        print(__doc__); return 2
    race_harness, attract_harness, assets = sys.argv[1], sys.argv[2], sys.argv[3]
    dirs = [Path(d) for d in sys.argv[4].split(';') if d]
    def find(name):
        return next((d / name for d in dirs if (d / name).is_file()), None)
    found = 0
    problems = []
    for name in RUNS:
        frontend, race = find(f'frontend-{name}.trace'), find(f'race-{name}.trace')
        if frontend is None or race is None:
            continue
        found += 1
        with tempfile.TemporaryDirectory() as tmp:
            problems += check_run(name, race_harness, attract_harness, assets, frontend, race, Path(tmp))
    if not found:
        print('no traces of several games, skipped'); return 77
    for p in problems:
        print('FAIL:', p)
    if problems:
        return 1
    print(f'PASS games lockstep: {found} runs')
    return 0


if __name__ == '__main__':
    sys.exit(main())
