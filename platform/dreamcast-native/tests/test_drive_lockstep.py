"""The drive board's command stream against the original's: every byte the native scenes send (include/drive_board.h,
the harnesses' --drive, tests/drive_trace.h) and the main-loop iteration it is sent in, against the bytes the original
wrote to the drive board in the same runs (m2segment --trace-drive, tools/trace_drive.hpp): whole games from power-on,
the queue carried from scene to scene and from game to game.

usage: test_drive_lockstep.py TRACE_RACE_NATIVE TRACE_ATTRACT_NATIVE ASSET_DIR "TRACE_DIR[;TRACE_DIR...]"

Each TRACE_DIR is searched for the runs X that have drive-X.trace, frontend-X.trace and race-X.trace of one input
script from power-on (ROM output, never committed):
  m2segment ROM N --inputs X.txt --trace-frontend frontend-X.trace --trace-race race-X.trace --trace-attract
            --trace-drive drive-X.trace
For each game of a run (tests/test_games_lockstep.py games()): trace_attract_native from power-on, or from the state
the game before left, to the credit and the game start (--enter, --credit, --leave), then trace_race_native through the
select, the race, the podium and the results to the game's end and its name entry (--enter, --frontend, --leave), each
with --drive; after the last game, when the run goes on into the attract, trace_attract_native --enter once more. The
native files cover consecutive iterations (their R lines, 5010b0 from power-on); every byte the original sent in those
iterations up to the run's last (its last F or T row), power-on's direct 0xff included, must be sent by the native in
the same iteration and order, and no other byte. Exits 77 when no run is found."""
import concurrent.futures, difflib, os, re, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_games_lockstep import games, ATTRACT_LOOPS

LINE = re.compile(r'^D b0=(\d+)(?: frame=\d+)? byte=([0-9a-f]{2})( direct)?$')
RANGE = re.compile(r'^R first=(\d+) last=(\d+)$')


def read(path):
    """[(b0, byte, direct)] and the R ranges of a drive trace."""
    sent, ranges = [], []
    with open(path) as source:
        for line in source:
            line = line.rstrip('\n')
            if line.startswith('#') or not line:
                continue
            m = LINE.match(line)
            if m:
                sent.append((int(m.group(1)), int(m.group(2), 16), m.group(3) is not None))
                continue
            m = RANGE.match(line)
            if not m:
                raise ValueError(f'{path}: unexpected line {line!r}')
            ranges.append((int(m.group(1)), int(m.group(2))))
    return sent, ranges


def reference_last(frontend, race):
    """5010b0 of the run's last iteration: its last F row's b0 or T row's scene."""
    last = 0
    for path, key in ((frontend, ' b0='), (race, ' scene=')):
        with open(path) as source:
            for line in source:
                if line[:2] in ('F ', 'T '):
                    at = line.find(key)
                    if at >= 0:
                        last = max(last, int(line[at + len(key):].split(' ', 1)[0]))
    return last


def run(command):
    return subprocess.run([str(c) for c in command], capture_output=True, text=True)


def native_stream(name, race_harness, attract_harness, assets, frontend, race, directory):
    """The native drive files of every game of the run, in order, or a problem."""
    played = games(race)
    if not played:
        return None, f'{name}: no game in the reference'
    files, enter = [], []
    for g, (first, over, course, podium) in enumerate(played):
        game = f'{name} game {g + 1}'
        state, attract_drive = directory / f'attract-{g}.state', directory / f'attract-{g}.drive'
        r = run([attract_harness, assets, directory / f'attract-{g}.trace', ATTRACT_LOOPS] + enter +
                ['--credit', frontend, '--leave', state, '--drive', attract_drive])
        if r.returncode:
            return None, f'{game} attract: exit {r.returncode}\n{r.stdout}{r.stderr}'
        (directory / f'attract-{g}.trace').unlink()
        files.append(attract_drive)
        left, race_drive = directory / f'game-{g}.state', directory / f'race-{g}.drive'
        ticks = (over - first) if over is not None else 100000 if g + 1 == len(played) else played[g + 1][0] - first
        command = [race_harness, assets, race, directory / f'native-{g}.trace', first, ticks, 'production',
                   '--course', course, '--enter', state, '--frontend', frontend, '--drive', race_drive]
        if over is not None:
            command += ['--leave', left]
        r = run(command)
        if r.returncode:
            return None, f'{game} race: exit {r.returncode}\n{r.stdout}{r.stderr}'
        (directory / f'native-{g}.trace').unlink()
        files.append(race_drive)
        if over is None:
            break
        enter = ['--enter', left]
    if played[-1][1] is not None:
        # The attract after the last game, to the run's end (the comparison stops at the run's last iteration).
        tail = directory / 'attract-tail.drive'
        r = run([attract_harness, assets, directory / 'attract-tail.trace', '1'] + enter + ['--drive', tail])
        if r.returncode:
            return None, f'{name} attract after the last game: exit {r.returncode}\n{r.stdout}{r.stderr}'
        (directory / 'attract-tail.trace').unlink()
        files.append(tail)
    return files, None


def check_run(name, race_harness, attract_harness, assets, frontend, race, drive):
    end = reference_last(frontend, race)
    with tempfile.TemporaryDirectory() as tmp:
        files, problem = native_stream(name, race_harness, attract_harness, assets, frontend, race, Path(tmp))
        if problem:
            return None, problem
        native, covered = [], []
        for f in files:
            sent, ranges = read(f)
            if len(ranges) != 1:
                return None, f'{name}: {f.name} has {len(ranges)} R lines'
            native += [s for s in sent if s[0] <= end]
            covered += ranges
    first, last = covered[0][0], min(covered[-1][1], end)
    for (a, b), (c, d) in zip(covered, covered[1:]):
        if c != b + 1:
            return None, f'{name}: the native runs leave a gap or overlap: {a}..{b} then {c}..{d}'
    reference = [s for s in read(drive)[0] if first <= s[0] <= last]
    if first != 0:
        return None, f'{name}: the native stream starts at iteration {first}, not at power-on'
    # Bytes of either side outside the longest matching blocks: a byte missing, added or sent at another iteration.
    matched = sum(b.size for b in difflib.SequenceMatcher(None, reference, native, autojunk=False).get_matching_blocks())
    differing = max(len(reference), len(native)) - matched
    if differing:
        at = next((i for i, (a, b) in enumerate(zip(reference, native)) if a != b), min(len(reference), len(native)))
        show = lambda s: ' '.join(f'{b0}:{c:02x}{"d" if d else ""}' for b0, c, d in s)
        return None, (f'{name}: {differing} differing of {len(reference)} bytes the original sent in iterations '
                      f'{first}..{last} (native {len(native)}); first at byte {at}:\n'
                      f'  original {show(reference[max(0, at - 3):at + 6])}\n  native   {show(native[max(0, at - 3):at + 6])}')
    games_run = len(files) // 2
    return (len(reference), first, last), f'{name}: {games_run} game{"s" if games_run > 1 else ""}, iterations {first}..{last}: {len(reference)} bytes equal, 0 differing'


def main():
    if len(sys.argv) != 5:
        print(__doc__); return 2
    race_harness, attract_harness, assets = sys.argv[1], sys.argv[2], sys.argv[3]
    dirs = [Path(d) for d in sys.argv[4].split(';') if d]
    runs = {}
    for d in dirs:
        for drive in sorted(d.glob('drive-*.trace')) if d.is_dir() else []:
            name = drive.name[len('drive-'):-len('.trace')]
            frontend, race = d / f'frontend-{name}.trace', d / f'race-{name}.trace'
            if name not in runs and frontend.is_file() and race.is_file():
                runs[name] = (frontend, race, drive)
    if not runs:
        print('no drive board traces, skipped'); return 77
    jobs = max(1, min(len(runs), (os.cpu_count() or 2) // 2))
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        results = dict(zip(runs, pool.map(lambda n: check_run(n, race_harness, attract_harness, assets, *runs[n]), runs)))
    problems, total = [], 0
    for name, (counts, line) in results.items():
        if counts is None:
            problems.append(line)
        else:
            total += counts[0]
            print(line)
    for p in problems:
        print('FAIL:', p)
    if problems:
        return 1
    print(f'PASS drive lockstep: {len(runs)} runs, {total} bytes equal, 0 differing')
    return 0


if __name__ == '__main__':
    sys.exit(main())
