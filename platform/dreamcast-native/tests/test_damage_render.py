"""The player's drawn parts with damage against the original's display lists (spec: build/dc-native-damage.*/
spec-damage.md sections 1, 2 and 8).

usage: test_damage_render.py TRACE_RACE_NATIVE PLAYER_PARTS_CHECK ASSET_DIR "TRACE_DIR[;...]" "LOG_DIR[;...]"

ASSET_DIR: an asset set (drive.dnd, course0.dn*, car.dnm, car-damage.dnb: the damaged bodies, tools/
prepare_damage_bodies.py). TRACE_DIR: reference-damage-E.trace
(m2segment --trace-race, recipes/inputs/inputs-damage-E.txt). LOG_DIR: log-E.bin and log-V.bin, the original's display
lists of the runs E (automatic, wall hits, two crashes) and V (manual, the VR views VR1..VR4 and wall hits), frames
2580-7600: damage_log v3 records (the car render log of build/dc-native-car-render.*: per frame a header with the view
words 501464, 5014a4, 501728, per car record +14 and +208, then every object with its model address and matrix).
ROM output, never committed. Exits 77 when a file is missing.

1. Rules (include/player_parts.h through tests/player_parts_check.c) on every race frame (phases 20-24) of both logs.
   A car record is sampled at the end of frame F; the display list of frame F+1 draws that record's body list with
   the view words of frame F-1 (the camera task runs after the car's draw).
   - the body list +14 = crash_model[v][nm_player_damage_body(+208)] (drive.dnd) or the clean list (course0.dnr);
   - the player's drawn parts = what nm_player_show / nm_player_part_mask give: nothing but the shadow in the
     driver's view, the hood 231eb0[v][nm_player_hood(+208)] (and no wheels) in the hood view, else body, glass 13b,
     shell, four wheels; the shadow in every view, with the body slot's matrix when there are no wheels.
2. The native race of run E (trace_race_native production --parts: the part list of
   code/render/scene/practice.c's player draw after every race tick) against log E: the same models in the same
   order on every race frame (the damaged bodies 8df664, 8e3cb9, 8e9f30 at the hits and crashes), and every matrix
   (view space) within ROTATION_TOLERANCE / TRANSLATION_TOLERANCE of the original's, the wheels' spin and wobble
   included once the native starts with the original's wheel phase (--wheel-phase: the attract's car 0 left it). The native row of reference frame F (the frame
   whose controls fed the tick) is the display list logged at F+2: its car record is the log's record of F+1.
"""
import mmap, struct, subprocess, sys, tempfile
from pathlib import Path

FH = struct.Struct('<QIIIII64I10I52II')
CAR = struct.Struct('<25I')
OH = struct.Struct('<6I12f2f3fII2h4hII')
GLASS, SHADOW, WHEELS = 0x13b, 0x8d7ab7, (0x8d7b13, 0x8d7c9a)
SHELLS = (0x961bbf, 0x961cb0)                        # 286fab8[v]
HOODS = ((0x94fff9, 0xa0092b, 0xa0027c, 0xa00fda),  # 231eb0[0][i] (player_parts.h nm_player_hood_models)
         (0x9506a8, 0xa01d38, 0xa01689, 0xa023e7))  # 231eb0[1][i], the manual car
FAMILY = {GLASS, SHADOW, *WHEELS, *SHELLS, *HOODS[0], *HOODS[1]}
SHOW_BODY, SHOW_HOOD, SHOW_WHEELS = 1, 2, 4
ROTATION_TOLERANCE = 1e-3      # matrix elements 0-8
TRANSLATION_TOLERANCE = 4e-3   # elements 9-11, relative to max(1, |original|)
RACE = range(20, 25)


def frames(path):
    """Per frame: number, phase, the view words, car 0's record and the objects (address, matrix)."""
    with open(path, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        p, n, cur = 0, len(m), None
        while p < n:
            tag = m[p]; p += 1
            if tag == 0x46:
                if cur: yield cur
                h = FH.unpack_from(m, p); p += FH.size
                extra = h[70:80]
                cur = dict(frame=h[0], phase=h[3], manual=extra[0] & 1, mode=extra[1], eye=extra[3], car=None, objs=[])
                for i in range(h[132]):
                    c = CAR.unpack_from(m, p); p += CAR.size
                    if i == 0:
                        cur['car'] = dict(id=c[1], model=c[5], overlay=c[6], damage=c[13], wheel=c[22])
            else:
                if tag != 0x4f:
                    raise ValueError(f'{path}: bad record tag at {p-1}')
                o = OH.unpack_from(m, p); p += OH.size + 16*o[-1]
                cur['objs'].append((o[0], o[6:18]))
        if cur: yield cur


def player_parts(objs):
    """The player's objects of one display list in order; the body is the object drawn just before the glass."""
    out = []
    for i, (addr, mat) in enumerate(objs):
        if addr == GLASS and i and objs[i-1][0] not in FAMILY:
            out.append(('body', objs[i-1][0], objs[i-1][1]))
        if addr in FAMILY:
            out.append((addr, addr, mat))
    return out


def check_rules(checker, assets, log, name):
    race = [f for f in frames(log) if f['car'] and f['car']['id'] == 0]
    by_frame = {f['frame']: f for f in race}
    cases = [(f, by_frame.get(f['frame']-1), by_frame.get(f['frame']+1)) for f in race if f['phase'] in RACE]
    cases = [c for c in cases if c[1] and c[2]]
    lines = ''.join(f"{f['manual']:x} {view['mode']:x} {view['eye']:x} {f['car']['damage']:x}\n" for f, view, _ in cases)
    run = subprocess.run([checker, f'{assets}/drive.dnd', f'{assets}/course0.dnr'], input=lines,
                         capture_output=True, text=True)
    if run.returncode:
        print(f'FAIL {name}: player_parts_check exit {run.returncode}: {run.stderr}'); return False
    answers = [tuple(int(v, 16) for v in line.split()) for line in run.stdout.splitlines()]
    if len(answers) != len(cases):
        print(f'FAIL {name}: {len(answers)} answers for {len(cases)} states'); return False
    lists = drawn = shadows = 0; bodies, kinds, bad = {}, {}, []
    for (f, _, nxt), (show, hood, body_list) in zip(cases, answers):
        v, car = f['manual'], f['car']
        lists += body_list == car['model']
        if body_list != car['model'] and len(bad) < 8:
            bad.append((f['frame'], 'list', hex(car['damage']), hex(body_list), hex(car['model'])))
        want = []
        if show & SHOW_BODY:
            want += ['body', GLASS, SHELLS[v]]
        if show & SHOW_HOOD:
            want.append(HOODS[v][hood])
        if show & SHOW_WHEELS and show & (SHOW_BODY | SHOW_HOOD):
            want += [WHEELS[0], WHEELS[1], WHEELS[0], WHEELS[1]]
        want.append(SHADOW)
        got = player_parts(nxt['objs'])
        kind = 'none' if not show & (SHOW_BODY | SHOW_HOOD) else 'hood' if show & SHOW_HOOD else 'body'
        kinds[kind] = kinds.get(kind, 0)+1
        if [g[0] for g in got] == want:
            drawn += 1
            if show & SHOW_BODY:   # one body model per body list, and a different one per list
                bodies.setdefault(car['model'], set()).add(got[0][1])
            if not show & SHOW_WHEELS and show & SHOW_HOOD and not car['overlay']:
                shadows += got[-1][2] == got[0][2]
        elif len(bad) < 8:
            bad.append((f['frame'], kind, hex(car['damage']), [hex(w) if type(w) is int else w for w in want],
                        [hex(g[1]) for g in got]))
    hood_frames = sum(1 for (f, _, _), (show, _, _) in zip(cases, answers)
                      if show == SHOW_HOOD and not f['car']['overlay'])
    unique = all(len(s) == 1 for s in bodies.values()) and len({next(iter(s)) for s in bodies.values()}) == len(bodies)
    ok = lists == drawn == len(cases) and shadows == hood_frames and unique
    print(f"{'PASS' if ok else 'FAIL'} {name}: {len(cases)} race frames; body list +14 equal on {lists}, drawn parts "
          f"equal on {drawn} ({', '.join(f'{k} {n}' for k, n in sorted(kinds.items()))}); hood-view shadow on the hood's "
          f"matrix {shadows}/{hood_frames}; {len(bodies)} body lists drawn, one model each: {unique}")
    for b in bad:
        print('   ', b)
    return ok


def matrix_error(a, b):
    rotation = max(abs(x-y) for x, y in zip(a[:9], b[:9]))
    translation = max(abs(x-y)/max(1.0, abs(y)) for x, y in zip(a[9:], b[9:]))
    return rotation, translation


def check_native(harness, assets, trace, log):
    # The race starts with the wheel phase +5a that entity 501220 had in the attract when START was pressed (the
    # new scene does not clear it; the logged record before the first race tick); the native starts cold at 0.
    start = None
    for f in frames(log):
        if f['phase'] in RACE:
            break
        if f['car']:
            start = f['car']['wheel'] & 0xffff
    if start is None:
        print('FAIL E native: the log starts after the race start'); return False
    with tempfile.TemporaryDirectory() as tmp:
        parts = Path(tmp)/'parts.txt'
        run = subprocess.run([harness, assets, str(trace), str(Path(tmp)/'native.trace'), '5', '14000', 'production',
                              '--parts', str(parts), '--wheel-phase', str(start)], capture_output=True, text=True)
        if run.returncode:
            print(f'FAIL E native: harness exit {run.returncode}\n{run.stdout}{run.stderr}'); return False
        native = {}
        for line in parts.read_text().splitlines():
            v = dict(p.split('=', 1) for p in line.split()[1:])
            items = []
            for item in v['parts'].split(','):
                addr, words = item.split(':')
                items.append((int(addr, 16), struct.unpack('<12f', struct.pack('<12I', *(int(w, 16) for w in words.split('.'))))))
            native[int(v['frame'])] = (int(v['dmg'], 16), items)
    compared = equal = 0; worst = [0.0, 0.0]; bad = []; damaged = {}
    for f in frames(log):
        record = native.get(f['frame']-2)
        if record is None or f['phase'] not in RACE:
            continue
        damage, items = record
        got = [(g[1], g[2]) for g in player_parts(f['objs'])]
        compared += 1
        same = [a for a, _ in items] == [a for a, _ in got]
        errors = [matrix_error(a, b) for (_, a), (_, b) in zip(items, got)] if same else [(0.0, 0.0)]
        error = [max(e[0] for e in errors), max(e[1] for e in errors)]
        if same and error[0] <= ROTATION_TOLERANCE and error[1] <= TRANSLATION_TOLERANCE:
            equal += 1
            damaged[items[0][0]] = damaged.get(items[0][0], 0)+1
        elif len(bad) < 8:
            bad.append((f['frame'], hex(damage), [hex(a) for a, _ in items], [hex(a) for a, _ in got], error))
        worst = [max(worst[0], error[0]), max(worst[1], error[1])]
    ok = compared > 3000 and equal == compared
    print(f"{'PASS' if ok else 'FAIL'} E native: {equal}/{compared} race frames with the same models in the same order "
          f"and matrices within {ROTATION_TOLERANCE} (rotation) / {TRANSLATION_TOLERANCE} (translation, relative): "
          f"largest {worst[0]:.3g} / {worst[1]:.3g}; start wheel phase {start}; bodies drawn: "
          + ', '.join(f'{a:x} {n}' for a, n in sorted(damaged.items())))
    for b in bad:
        print('   ', b)
    return ok


def main():
    if len(sys.argv) != 6:
        print(__doc__); return 2
    harness, checker, assets = sys.argv[1:4]
    find = lambda dirs, name: next((Path(d)/name for d in dirs.split(';') if d and (Path(d)/name).is_file()), None)
    trace, logs = find(sys.argv[4], 'reference-damage-E.trace'), [find(sys.argv[5], f'log-{r}.bin') for r in 'EV']
    if subprocess.run([checker]).returncode:
        print('FAIL: player_parts_check rules'); return 1
    if not trace or not all(logs) or not all((Path(assets)/f).is_file() for f in ('drive.dnd', 'course0.dnr', 'car.dnm', 'car-damage.dnb')):
        print('skipped: reference-damage-E.trace, log-E.bin, log-V.bin or the asset set (car-damage.dnb) missing'); return 77
    ok = check_rules(checker, assets, logs[0], 'E rules')
    ok &= check_rules(checker, assets, logs[1], 'V rules')
    ok &= check_native(harness, assets, trace, logs[0])
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
