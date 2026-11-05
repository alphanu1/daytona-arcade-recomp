"""Pit stop presentation against the original's pit log (spec: build/dc-native-pit.*/spec-pitstop.md sections 3-5, 8).

usage: test_pit_render.py TRACE_RACE_NATIVE ASSET_DIR "TRACE_DIR[;...]" "LOG_DIR[;...]"

TRACE_DIR: reference-pit-P.trace (m2segment --trace-race of inputs-pit-P.txt). LOG_DIR: log-P.bin, the pit log of the
same run (m2pit: damage_log v3 frame headers and display-list objects, plus the CPU records C (pit routine calls),
D (draw calls 1786c: list, return address), A (polygon dumps) and S (end of frame: player, 40 globals, task rows,
crew objects)); ROM output, never committed. ASSET_DIR: an asset set with car-damage.dnb. Exits 77 when a file is
missing. Runs E, C, F and H are checked the same way where both of their files are found (ref/record.sh re-records
the logs). The native race of run P (trace_race_native production --parts --hud) against the log, frame by frame (the
native row of trace frame F is the display list logged at F+2, test_damage_render.py; the S record of log frame F+1
is the state at the end of trace frame F):
  - the player's draw: the same models in the same order and every matrix within test_damage_render.py's tolerances
    on every logged frame: the wheels the crew has off (501a98, 7384) left out, the jack's lift (+5c 0.64, the crash
    path of the body pose) from crew time 99 to 460, and the draw before the crew rows (a toggle shows one tick
    late); the wheels' spin compared from S+162 on (crew 0 zeroes both phases at S+161, and the log starts inside
    the race, so the attract's wheel phase is unknown before);
  - opponents drawn (795c..84dc) exactly when the native 501a84 after the tick is <= 1 (7684, render/scene/practice.c);
  - "PIT WORK!" (ab2c): the log's tile row 20 (columns 20..40) drawn exactly on the frames the native HUD rule draws it;
  - no world actor (220000..223fff) drawn on the frames the native suspends rows 74..138;
  - the pit crew (rows 68..73, include/pit_crew.h, the `crew=` field of --parts): on every logged frame the crew
    objects of the display list (after the player's shadow, the models the native's crew draws) are the native's,
    the same models in the same order (crew 0..5: held items, then the 16 parts) and every matrix within
    test_damage_render.py's tolerances, also reported against CREW_TOLERANCE (1e-4); on a frame the native's crew
    does not draw, the log has none of the crew's own models (the held wheel 8d7c9a aside: the opponents' too)."""
import mmap, struct, subprocess, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_damage_render as t


def log_frames(path):
    """Per log frame: number, phase, objects (address, matrix), draw calls (list, return) and the S record's
    globals 501a84 (index 0) and 501a98 (index 3), and tile row 20."""
    with open(path, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        p, n, cur = 0, len(m), None
        while p < n:
            tag = m[p]; p += 1
            if tag == 0x46:
                if cur: yield cur
                h = t.FH.unpack_from(m, p); p += t.FH.size + t.CAR.size * h[132]
                words = [v for w in h[80:132] for v in (w & 0xffff, w >> 16)]
                cur = dict(frame=h[0], phase=h[3], objs=[], draws=[], row20=words[0:32], hold=None)
            elif tag == 0x4f:
                o = t.OH.unpack_from(m, p); p += t.OH.size + 16 * o[-1]
                cur['objs'].append((o[0], o[6:18]))
            elif tag == 0x43:
                p += 20
            elif tag == 0x53:
                g = struct.unpack_from('<40I', m, p + 4 + 768)
                cur['hold'], cur['wheels'] = g[0], g[3]
                p += 4 + 768 + 160 + 72 * 12 + 3840 + 128
            elif tag == 0x44:
                cur['draws'].append(struct.unpack_from('<2I', m, p)); p += 8
            elif tag == 0x41:
                _, polys = struct.unpack_from('<2I', m, p); p += 8
                for _ in range(polys):
                    p += 7 + 12 * m[p]
            else:
                raise ValueError(f'{path}: bad record tag {tag:#x} at {p - 1}')
        if cur: yield cur


CREW_TOLERANCE = 1e-4    # rotation, and translation relative to max(1, |original|): reported, not required
CREW_WHEEL = 0x8d7c9a    # held wheel 288b730: also an opponent's wheel


def matrices(text):
    """Model and matrix of each item of a --parts list (address:12 float words)."""
    items = []
    for item in text.split(','):
        addr, words = item.split(':')
        items.append((int(addr, 16), struct.unpack('<12f', struct.pack('<12I', *(int(w, 16) for w in words.split('.'))))))
    return items


def find(dirs, name):
    return next((Path(x) / name for x in dirs.split(';') if x and (Path(x) / name).is_file()), None)


def fields(line):
    return dict(p.split('=', 1) for p in line.split()[1:] if '=' in p)


def main():
    if len(sys.argv) != 5:
        print(__doc__); return 2
    harness, assets = sys.argv[1], sys.argv[2]
    if not find(sys.argv[3], 'reference-pit-P.trace') or not find(sys.argv[4], 'log-P.bin') or \
            not (Path(assets) / 'car-damage.dnb').is_file():
        print('skipped: reference-pit-P.trace, log-P.bin or the asset set (car-damage.dnb) missing'); return 77
    # Run P always; E, C, F, H where their trace and log are given (ref/record.sh re-records the logs).
    results = []
    for name in 'PECFH':
        trace, log = find(sys.argv[3], f'reference-pit-{name}.trace'), find(sys.argv[4], f'log-{name}.bin')
        if trace and log:
            results.append(check(harness, assets, trace, log, name))
    return 0 if all(r == 0 for r in results) else 1


def check(harness, assets, trace, log, name):
    player = {}
    with open(trace) as f:
        for line in f:
            if line.startswith('C ') and ' id=0 ' in line:
                v = fields(line); player[int(v['n'])] = v
            elif line.startswith('T '):
                v = fields(line); player.setdefault('frames', {})[int(v['n'])] = int(v['frame'])
    frame_of = player.pop('frames')
    d = next(k for k in sorted(player) if player[k]['upd'] == 'dc18')
    s = next(k for k in sorted(player) if k > d and player[k]['si'] == '0')
    wheel_from = frame_of[s] + 162
    with tempfile.TemporaryDirectory() as tmp:
        parts, hud, nat = Path(tmp) / 'parts.txt', Path(tmp) / 'hud.txt', Path(tmp) / 'native.trace'
        run = subprocess.run([harness, assets, str(trace), str(nat), '5', '14000', 'production', '--parts', str(parts),
                              '--hud', str(hud)], capture_output=True, text=True)
        if run.returncode:
            print(f'FAIL: harness exit {run.returncode}\n{run.stdout}{run.stderr}'); return 1
        native_parts, native_crew = {}, {}
        for line in parts.read_text().splitlines():
            v = fields(line)
            native_parts[int(v['frame'])] = matrices(v['parts'])
            native_crew[int(v['frame'])] = matrices(v['crew']) if 'crew' in v else []
        native_hud = {int(v['frame']): v.get('pit') for v in map(fields, hud.read_text().splitlines())}
        native_t = {}
        with open(nat) as f:
            for line in f:
                if line.startswith('T '):
                    v = fields(line); native_t[int(v['frame'])] = (int(v['hold']), v['pit'].split('.')[4] == '1')
    counts = dict(parts=[0, 0], opponents=[0, 0], message=[0, 0], actors=[0, 0], crew=[0, 0])
    bad, worst = [], [0.0, 0.0]
    crew_models = {a for items in native_crew.values() for a, _ in items}
    crew_frames = crew_close = crew_draws = 0
    crew_worst = [0.0, 0.0]
    shown = suspended_frames = 0
    frames = {fr['frame']: fr for fr in log_frames(log)}
    for number, fr in sorted(frames.items()):
        state_frame = number - 1  # the S record of this log frame: the end of trace frame number - 1
        if state_frame in native_t and state_frame in native_hud:
            hold, suspended = native_t[state_frame]
            opponents = sum(1 for _, ret in fr['draws'] if 0x795c <= ret < 0x84dc)
            actors = sum(1 for _, ret in fr['draws'] if 0x220000 <= ret < 0x224000)
            drawn = any(w != 0x20 for w in fr['row20'][4:25])
            for key, ok in (('opponents', (opponents > 0) == (hold <= 1)), ('message', drawn == (native_hud[state_frame] == '1')),
                            ('actors', not suspended or actors == 0)):
                counts[key][0] += 1; counts[key][1] += ok
                if not ok and len(bad) < 8: bad.append((key, state_frame, hold, opponents, drawn, actors))
            shown += drawn; suspended_frames += suspended
        items = native_parts.get(number - 2)
        if items is None or fr['phase'] not in t.RACE:
            continue
        got = [(g[1], g[2]) for g in t.player_parts(fr['objs'])]
        # The crew's held wheels (288b730, the wheel model) follow the player's run, which ends at its shadow.
        end = next((i for i, g in enumerate(got) if g[0] == t.SHADOW), len(got) - 1)
        got = got[:end + 1]
        # The crew: the objects after the player's shadow whose models the native's crew draws.
        shadow = next((i for i, (a, _) in enumerate(fr['objs']) if a == t.SHADOW), len(fr['objs']))
        logged = [(a, m) for a, m in fr['objs'][shadow + 1:] if a in crew_models]
        want = native_crew.get(number - 2, [])
        if want:
            same = [a for a, _ in want] == [a for a, _ in logged]
            errors = [t.matrix_error(a, b) for (_, a), (_, b) in zip(want, logged)] if same else [(9.0, 9.0)]
            error = [max(e[0] for e in errors), max(e[1] for e in errors)]
            ok = same and error[0] <= t.ROTATION_TOLERANCE and error[1] <= t.TRANSLATION_TOLERANCE
            crew_frames += 1; crew_draws += len(want)
            crew_close += same and error[0] <= CREW_TOLERANCE and error[1] <= CREW_TOLERANCE
            crew_worst = [max(crew_worst[0], error[0]), max(crew_worst[1], error[1])]
        else:
            ok = not any(a != CREW_WHEEL for a, _ in logged)
        counts['crew'][0] += 1; counts['crew'][1] += ok
        if not ok and len(bad) < 8: bad.append(('crew', number, [hex(a) for a, _ in want][:20], [hex(a) for a, _ in logged][:20]))
        same = [a for a, _ in items] == [a for a, _ in got]
        errors = [t.matrix_error(a, b) for (addr, a), (_, b) in zip(items, got)
                  if addr not in t.WHEELS or number - 2 >= wheel_from] if same else [(9.0, 9.0)]
        error = [max([e[0] for e in errors] + [0.0]), max([e[1] for e in errors] + [0.0])]
        ok = same and error[0] <= t.ROTATION_TOLERANCE and error[1] <= t.TRANSLATION_TOLERANCE
        counts['parts'][0] += 1; counts['parts'][1] += ok
        if not ok and len(bad) < 8: bad.append(('parts', number, [hex(a) for a, _ in items], [hex(a) for a, _ in got], error))
        worst = [max(worst[0], error[0]), max(worst[1], error[1])]
    passed = (all(n == good and n > 1000 for n, good in counts.values()) and shown > 300 and suspended_frames > 500 and
              crew_frames > 800)
    print(f"{'PASS' if passed else 'FAIL'} {name}: player's parts equal on {counts['parts'][1]}/{counts['parts'][0]} frames "
          f"(largest {worst[0]:.3g} rotation / {worst[1]:.3g} translation, wheels' spin from frame {wheel_from}); "
          f"opponents drawn iff 501a84 <= 1 on {counts['opponents'][1]}/{counts['opponents'][0]}; PIT WORK! on "
          f"{counts['message'][1]}/{counts['message'][0]} (drawn on {shown}); no world actor while suspended on "
          f"{counts['actors'][1]}/{counts['actors'][0]} ({suspended_frames} suspended); pit crew equal on "
          f"{counts['crew'][1]}/{counts['crew'][0]} frames ({crew_frames} drawn, {crew_draws} draws of "
          f"{len(crew_models)} models; largest {crew_worst[0]:.3g} rotation / {crew_worst[1]:.3g} translation, "
          f"{crew_close} frames within {CREW_TOLERANCE})")
    for b in bad:
        print('   ', b)
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
