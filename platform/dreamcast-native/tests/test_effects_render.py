"""The player's car effects (include/car_effects.h) against the original's effects logs (spec:
build/dc-native-effects.11xv92/spec-effects.md sections 3 and 8).

usage: test_effects_render.py CAR_EFFECTS_CHECK TRACE_RACE_NATIVE ASSET_DIR "TRACE_DIR[;...]" "LOG_DIR[;...]" ROM_DIR

ASSET_DIR: drive.dnd and the course0 packs. TRACE_DIR: reference-damage-{A,C,E,F,G,H}.trace (m2segment --trace-race of
recipes/inputs/inputs-damage-*.txt) and reference-finish-fast.trace. LOG_DIR: fx-{A,C,E,F,G,H,V,fast}.fx, the effects
logs of the spec (m2fx: damage_log v3 display lists plus, in call order, P = the player's 768 bytes and the view
preset 501710 at every 9088 entry, D = every draw call 1786c with its model list and return address; record them
with build/dc-native-effects.11xv92/ref/record.sh). ROM_DIR: program.bin and main_data.bin (the effect tables and
model lists). Whatever logs are present are checked; exits 77 when none is. ROM output, never committed.

1. Rules: every 9088 call of every log, the draws that follow it (return addresses 9000..9a00: list and routine)
   equal what nm_car_effects (through tests/car_effects_check.c) gives for the logged player bytes and view: dust,
   tyre smoke and sparks, all six spark places (views 1..3 of log V), the 9148 clears.
2. Local matrices: every logged dust / smoke / spark object, relative to the effects' frame of the same display list
   (the shadow 288b6f4, which 6f0c draws with the matrix the wheel routine leaves; the body times the undo
   Rx(-(+52)) Rz(-(+56)) during a crash; the hood in the hood view), equals the rule's T [Rx Ry] S within
   LOCAL_TOLERANCE. Display list of frame F+1 = draw calls of frame F (two-tick board frames excepted).
3. Native: the race of each run with a trace (trace_race_native production --effects: the rule on the native state
   after every tick, in the frame code/render/scene/practice.c uses) against the log's display lists: the same
   effect models in the same order on every race frame, view-space matrices within ROTATION_TOLERANCE /
   TRANSLATION_TOLERANCE (relative) of the original's. The native row of reference frame F is the display list
   logged at F+2. Run V (VR views) is not compared: the port draws the follow view (preset 2) only. The spin
   smoke's drawn particles (232206[life], view T S Rx Ry Rz) are part of that list, after the contact effects.
4. Spin smoke (task 9e2c, car_effects.h nm_spin_smoke_step through car_effects_check --spin): every 9f28 tick of
   every log from its Q record equals the next Q record (particle floats bit-exact, emitted points within
   EMIT_TOLERANCE, lives, ring, angle halfwords) and draws the lists logged at a420 (the counter rule).
5. Native spin smoke: the harness's task state after every race tick equals the original's (lives, ring, angles
   exact, particles within EMIT_TOLERANCE relative), the source car being race.shared.inhibit_owner.
"""
import math, mmap, struct, subprocess, sys, tempfile
from pathlib import Path

FH = struct.Struct('<QIIIII64I10I52II')
CAR = struct.Struct('<25I')
OH = struct.Struct('<6I12f2f3fII2h4hII')
RUNS = ('A', 'C', 'E', 'F', 'G', 'H', 'V', 'fast')
TABLES = {0: 0x231f06, 1: 0x231f86, 2: 0x232006}            # car_effects.h NM_FX_DUST, _SMOKE, _SPARK
CALL_RET = {0: 0x95b4, 1: 0x9678, 2: 0x9878, 3: 0x9908}       # NM_FX_CALL_* -> return address of its 1786c
SPARK_RET = {(0, 1): 0x92a4, (0, 2): 0x92e8, (0, 3): 0x932c, (3, 1): 0x93cc, (3, 2): 0x9410, (3, 3): 0x9454}
LOCAL_TOLERANCE = 1e-4
ROTATION_TOLERANCE = 1e-3      # view-space matrix elements 0-8
TRANSLATION_TOLERANCE = 4e-3   # elements 9-11, relative to max(1, |original|)
RACE = range(20, 25)


class Rom:
    def __init__(self, path):
        self.program = (Path(path)/'program.bin').read_bytes()
        self.main = (Path(path)/'main_data.bin').read_bytes()

    def pw(self, at):
        return struct.unpack_from('<I', self.program, at-0x200000)[0]

    def model(self, pointer):
        return struct.unpack_from('<I', self.main, pointer-0x2000000)[0]

    def entry(self, kind, index):
        return self.pw(TABLES[kind]+4*index)


def frames(path):
    """Per board frame: number, phase, the display list (model, matrix) and the CPU records P (car bytes, view)
    and D (list, return address) in call order."""
    with open(path, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        p, n, cur = 0, len(m), None
        while p < n:
            t = m[p]; p += 1
            if t == 0x46:
                if cur: yield cur
                h = FH.unpack_from(m, p); p += FH.size
                p += CAR.size*h[132]
                cur = dict(frame=h[0], phase=h[3], objs=[], cpu=[])
            elif t == 0x50:
                car = bytes(m[p+4:p+772]); view = struct.unpack_from('<I', m, p+772)[0]; p += 792
                cur['cpu'].append(('P', car, view))
            elif t == 0x51:
                task = bytes(m[p+8:p+264]); car = bytes(m[p+268:p+1036])
                angles = struct.unpack_from('<24H', m, p+1036); view = struct.unpack_from('<I', m, p+1084)[0]
                p += 8+256+4+768+48+4
                cur['cpu'].append(('Q', task, car, angles, view))
            elif t == 0x47:
                p += 12
            elif t == 0x44:
                g0, ip = struct.unpack_from('<2I', m, p); p += 8
                cur['cpu'].append(('D', g0, ip))
            elif t == 0x41:
                _, npoly = struct.unpack_from('<2I', m, p); p += 8
                for _ in range(npoly):
                    p += 7+12*m[p]
            elif t == 0x4f:
                o = OH.unpack_from(m, p); p += OH.size+16*o[-1]
                cur['objs'].append((o[0], o[6:18]))
            else:
                raise ValueError(f'{path}: bad record tag {t:#x} at {p-1}')
        if cur: yield cur


def s8(b, o): return struct.unpack_from('<b', b, o)[0]
def s16(b, o): return struct.unpack_from('<h', b, o)[0]


def state_line(car, view):
    u = lambda o: car[o]
    return ' '.join(f'{v:x}' for v in (u(0x91), u(0x92), u(0x95), u(0x96), u(0x98), u(0x99),
                                       struct.unpack_from('<H', car, 0xd6)[0], u(0xed),
                                       struct.unpack_from('<I', car, 0x34)[0], struct.unpack_from('<I', car, 0x180)[0],
                                       view))


def parse_draws(line):
    """car_effects_check output: [(call, kind, index, side, local 12 floats)]."""
    if line.strip() == '-':
        return []
    out = []
    for item in line.split():
        call, kind, index, side, words = item.split(':')
        out.append((int(call), int(kind), int(index), int(side),
                    struct.unpack('<12f', struct.pack('<12I', *(int(w, 16) for w in words.split('.'))))))
    return out


def expected_calls(draws, view, rom):
    return [(SPARK_RET[(side, view)] if kind == 2 else CALL_RET[call], rom.entry(kind, index))
            for call, kind, index, side, _ in draws]


def as_affine(m):
    """Column-major 3x4 (DNM, TGP logs) -> 3x3 matrix acting on column vectors (rows of floats) and the translation."""
    return [[float(m[3*c+r]) for c in range(3)] for r in range(3)], [float(v) for v in m[9:12]]


def mul(a, b):
    return [[sum(a[r][k]*b[k][c] for k in range(3)) for c in range(3)] for r in range(3)]


def apply(a, v):
    return [sum(a[r][k]*v[k] for k in range(3)) for r in range(3)]


def undo(pitch, bank):
    a, b = -pitch*2*math.pi/65536, -bank*2*math.pi/65536
    x = [[1, 0, 0], [0, math.cos(a), math.sin(a)], [0, -math.sin(a), math.cos(a)]]
    z = [[math.cos(b), math.sin(b), 0], [-math.sin(b), math.cos(b), 0], [0, 0, 1]]
    return mul(x, z)


def check_log(checker, assets, rom, log, name):
    """Checks 1 and 2 on one log."""
    effect_lists = {rom.entry(k, i): k for k in TABLES for i in range(32)}
    effect_models = {rom.model(l) for l in effect_lists}
    hoods = {rom.model(rom.pw(0x231eb0+4*i)) for i in range(8)}
    log_frames = list(frames(log))
    calls = []                      # (frame, P index) per 9088 call
    lines = []
    for fr in log_frames:
        for i, rec in enumerate(fr['cpu']):
            if rec[0] == 'P':
                calls.append((fr, i)); lines.append(state_line(rec[1], rec[2])+'\n')
    run = subprocess.run([checker, f'{assets}/drive.dnd'], input=''.join(lines), capture_output=True, text=True)
    answers = run.stdout.splitlines()
    if run.returncode or len(answers) != len(calls):
        print(f'FAIL {name}: car_effects_check exit {run.returncode}, {len(answers)} answers for {len(calls)} calls '
              f'{run.stderr[-500:]}'); return False
    ok = 0; bad = []; per_ret = {}; rule = {}
    for (fr, i), answer in zip(calls, answers):
        car, view = fr['cpu'][i][1], fr['cpu'][i][2]
        got = []
        for rec in fr['cpu'][i+1:]:
            if rec[0] != 'D' or not 0x9000 <= rec[2] < 0x9a00:
                break
            got.append((rec[2], rec[1]))
        draws = parse_draws(answer)
        want = expected_calls(draws, view, rom)
        rule.setdefault(fr['frame'], []).append((car, draws))
        for ret, _ in want: per_ret[ret] = per_ret.get(ret, 0)+1
        if got == want:
            ok += 1
        elif len(bad) < 6:
            bad.append((fr['frame'], [(hex(a), hex(b)) for a, b in got], [(hex(a), hex(b)) for a, b in want]))
    rules_ok = ok == len(calls) and len(calls) > 3000
    print(f"{'PASS' if rules_ok else 'FAIL'} {name} rules: {ok}/{len(calls)} 9088 calls draw what nm_car_effects gives; "
          'draws ' + ', '.join(f'{k:x} {v}' for k, v in sorted(per_ret.items())))
    for b in bad: print('   ', b)
    # 2. Local matrices against the effects' frame of the same display list (lag 1).
    by_frame = {fr['frame']: fr for fr in log_frames}
    compared = within = skipped = 0; worst = 0.0; outliers = []
    for fr in log_frames:
        prev = by_frame.get(fr['frame']-1)
        if not prev or prev['frame'] not in rule:
            continue
        draws = [(car, d) for car, ds in rule[prev['frame']] for d in ds]
        objs = [(a, mm) for a, mm in fr['objs'] if a in effect_models]
        if not draws:
            continue
        ref = kind = None; car0 = draws[0][0]
        for rec in prev['cpu']:
            if rec[0] != 'D': continue
            if rec[2] == 0x6f10: ref, kind = rom.model(rec[1]), 'shadow'
            elif rec[2] == 0x7368 and ref is None: ref, kind = rom.model(rec[1]), 'body'
            elif rec[2] == 0x7354 and ref is None: ref, kind = rom.model(rec[1]), 'hood'
        refs = [mm for a, mm in fr['objs'] if a == ref]
        if len(objs) != len(draws) or not refs:
            skipped += len(draws); continue
        r0, t0 = as_affine(refs[0])
        if kind == 'body':
            r0 = mul(r0, undo(s16(car0, 0x52), s16(car0, 0x56)))
        for (car, d), (addr, mm) in zip(draws, objs):
            rl, tl = as_affine(d[4])
            r, t = as_affine(mm)
            rr, tt = mul(r0, rl), [x+y for x, y in zip(apply(r0, tl), t0)]
            e = max(max(abs(rr[i][j]-r[i][j]) for i in range(3) for j in range(3)), max(abs(x-y) for x, y in zip(tt, t)))
            compared += 1; worst = max(worst, e)
            if e <= LOCAL_TOLERANCE: within += 1
            elif len(outliers) < 8: outliers.append((fr['frame'], hex(addr), f'{e:.2e}'))
    local_ok = compared > 300 and within >= compared-4
    print(f"{'PASS' if local_ok else 'FAIL'} {name} local matrices: {within}/{compared} effect objects within "
          f"{LOCAL_TOLERANCE} of frame * T [Rx Ry] S (largest {worst:.2e}); {skipped} draws not paired "
          f"(object count differs in a two-tick board frame, or no reference); off: {outliers}")
    return rules_ok and local_ok


SPIN_TABLE = 0x232206                                        # 27 entries, by particle life
SPIN_DRAW_RET = 0xa420
EMIT_TOLERANCE = 1e-4


def spin_ticks(log_frames):
    """Per 9f28 call (task 9e2c's tick) in call order: (board frame, task bytes, source car bytes, angles, view,
    the player's x and z of that tick (its 9088 entry, row 60, comes before row 66), the lists drawn at a420)."""
    out, player = [], (0.0, 0.0)
    for fr in log_frames:
        for rec in fr['cpu']:
            if rec[0] == 'P':
                player = struct.unpack_from('<f', rec[1], 0x1c)[0], struct.unpack_from('<f', rec[1], 0x24)[0]
            elif rec[0] == 'Q':
                out.append([fr['frame'], rec[1], rec[2], rec[3], rec[4], player, []])
            elif rec[0] == 'D' and rec[2] == SPIN_DRAW_RET and out:
                out[-1][6].append(rec[1])
    return out


def spin_state(task, angles):
    particles = list(struct.unpack_from('<48I', task, 0x34))
    return particles, list(task[0xf8:0x100]), task[0xf4], [a & 0xffff for a in angles[:12]]


def check_spin_log(checker, assets, rom, log_frames, name):
    """4. Task 9e2c: every tick of the log from its Q record (the task at 9f28 entry) through nm_spin_smoke_step
    (tests/car_effects_check.c --spin) equals the next tick's Q record: particle floats bit-exact (an emitted
    particle's point within EMIT_TOLERANCE: the TGP transform), lives, ring, angles; and the particles drawn (the
    a258 counter rule) are the lists the log draws at a420, 232206[life], in slot order."""
    ticks = spin_ticks(log_frames)
    lines = []
    for frame, task, car, angles, view, (px, pz), _ in ticks:
        particles, lives, ring, angle = spin_state(task, angles)
        words = particles+lives+[ring]+angle+[car[0xd4], struct.unpack_from('<H', car, 0xd6)[0],
                 struct.unpack_from('<H', car, 0x58)[0], struct.unpack_from('<H', car, 0x2c)[0]]
        words += list(struct.unpack_from('<3I', car, 0x1c))+[struct.unpack_from('<I', car, 0x9c)[0],
                 struct.unpack_from('<I', car, 0xa4)[0]]
        words += list(struct.unpack('<2I', struct.pack('<2f', px, pz)))+[view]
        lines.append(' '.join(f'{w:x}' for w in words)+'\n')
    run = subprocess.run([checker, f'{assets}/drive.dnd', '--spin'], input=''.join(lines), capture_output=True, text=True)
    answers = run.stdout.splitlines()
    if run.returncode or len(answers) != len(ticks):
        print(f'FAIL {name} spin: car_effects_check exit {run.returncode}, {len(answers)} answers for {len(ticks)} '
              f'ticks {run.stderr[-500:]}'); return False
    spin = [rom.pw(SPIN_TABLE+4*i) for i in range(27)]
    stat = dict(ticks=0, exact=0, drawn=0, emitted=0, particles_drawn=0, particles_alive=0, hidden=0)
    worst, bad = 0.0, []
    for k, answer in enumerate(answers[:-1]):
        w = [int(v, 16) for v in answer.split()]
        got_p, got_l, got_r, got_a, mask = w[:48], w[48:56], w[56], w[57:69], w[69]
        before = spin_state(ticks[k][1], ticks[k][3])
        want_p, want_l, want_r, want_a = spin_state(ticks[k+1][1], ticks[k+1][3])
        emitted = set()
        if got_r != before[2]:
            emitted = {before[2], (before[2]+1) & 7}
        ok = got_l == want_l and got_r == want_r and got_a == want_a
        for s in range(8):
            if s in emitted:
                stat['emitted'] += 1
                ok &= got_p[6*s+3:6*s+6] == want_p[6*s+3:6*s+6]
                g = struct.unpack('<3f', struct.pack('<3I', *got_p[6*s:6*s+3]))
                e = struct.unpack('<3f', struct.pack('<3I', *want_p[6*s:6*s+3]))
                error = max(abs(a-b) for a, b in zip(g, e)); worst = max(worst, error)
                ok &= error <= EMIT_TOLERANCE
            elif got_l[s] or before[1][s]:
                ok &= got_p[6*s:6*s+6] == want_p[6*s:6*s+6]
        drawn = [spin[got_l[s]] for s in range(8) if mask >> s & 1]
        alive = sum(1 for s in range(8) if before[1][s] or s in emitted)   # stepped this tick
        stat['ticks'] += 1; stat['exact'] += ok; stat['drawn'] += drawn == ticks[k][6]
        stat['particles_drawn'] += len(drawn); stat['particles_alive'] += alive; stat['hidden'] += alive-len(drawn)
        if (not ok or drawn != ticks[k][6]) and len(bad) < 6:
            bad.append((ticks[k][0], ok, [hex(v) for v in drawn], [hex(v) for v in ticks[k][6]]))
    passed = stat['ticks'] > 3000 and stat['exact'] == stat['ticks'] and stat['drawn'] == stat['ticks']
    print(f"{'PASS' if passed else 'FAIL'} {name} spin smoke: {stat['exact']}/{stat['ticks']} ticks of task 9e2c "
          f"equal one nm_spin_smoke_step (particle floats bit-exact, {stat['emitted']} emitted points within "
          f"{EMIT_TOLERANCE}: largest {worst:.2e}; lives, ring, angles); {stat['drawn']}/{stat['ticks']} draw the "
          f"logged lists ({stat['particles_drawn']} of {stat['particles_alive']} alive particles drawn, "
          f"{stat['hidden']} hidden near the player)")
    for b in bad: print('   ', b)
    return passed


def matrix_error(a, b):
    rotation = max(abs(x-y) for x, y in zip(a[:9], b[:9]))
    translation = max(abs(x-y)/max(1.0, abs(y)) for x, y in zip(a[9:], b[9:]))
    return rotation, translation


def check_native(harness, assets, rom, trace, log, name):
    effect_models = {rom.model(rom.entry(k, i)) for k in TABLES for i in range(32)}
    effect_models |= {rom.model(rom.pw(SPIN_TABLE+4*i)) for i in range(27)}
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)/'effects.txt'
        run = subprocess.run([harness, assets, str(trace), str(Path(tmp)/'native.trace'), '5', '14000', 'production',
                              '--effects', str(out)], capture_output=True, text=True)
        if run.returncode:
            print(f'FAIL {name} native: harness exit {run.returncode}\n{run.stdout[-2000:]}{run.stderr[-2000:]}')
            return False
        native, spin = {}, {}
        for line in out.read_text().splitlines():
            v = dict(p.split('=', 1) for p in line.split()[1:])
            if line.startswith('S '):
                spin[int(v['frame'])] = v
                continue
            items = []
            if v['fx'] != '-':
                for item in v['fx'].split(','):
                    addr, words = item.split(':')
                    items.append((int(addr.rstrip('ts'), 16), addr[-1] if addr[-1] in 'ts' else '',
                                  struct.unpack('<12f', struct.pack('<12I', *(int(w, 16) for w in words.split('.'))))))
            native[int(v['frame'])] = items
    compared = equal = frames_with = 0; worst = [0.0, 0.0]; bad = []; kinds = {}
    for fr in frames(log):
        items = native.get(fr['frame']-2)
        if items is None or fr['phase'] not in RACE:
            continue
        got = [(a, mm) for a, mm in fr['objs'] if a in effect_models]
        compared += 1; frames_with += bool(items or got)
        same = [a for a, _, _ in items] == [a for a, _ in got]
        errors = [matrix_error(a, b) for (_, _, a), (_, b) in zip(items, got)] if same and items else [(0.0, 0.0)]
        error = [max(e[0] for e in errors), max(e[1] for e in errors)]
        if same and error[0] <= ROTATION_TOLERANCE and error[1] <= TRANSLATION_TOLERANCE:
            equal += 1
            for _, kind, _ in items:
                kind = {'t': 'tyre smoke', 's': 'spin smoke'}.get(kind, 'dust/sparks')
                kinds[kind] = kinds.get(kind, 0)+1
        elif len(bad) < 8:
            bad.append((fr['frame'], [hex(a) for a, _, _ in items], [hex(a) for a, _ in got], error))
        worst = [max(worst[0], error[0]), max(worst[1], error[1])]
    ok = compared > 2500 and equal == compared
    print(f"{'PASS' if ok else 'FAIL'} {name} native: {equal}/{compared} race frames ({frames_with} with effects) with "
          f"the same effect models in the same order and matrices within {ROTATION_TOLERANCE} (rotation) / "
          f"{TRANSLATION_TOLERANCE} (translation, relative): largest {worst[0]:.3g} / {worst[1]:.3g}; draws "
          + ', '.join(f'{k} {n}' for k, n in sorted(kinds.items())))
    for b in bad: print('   ', b)
    # The native task state after race tick n against the original's task at the entry of its next tick: the
    # task's first tick (the init 9e2c falling through into 9f28, no 9f28 entry) is the original's tick before the
    # native tick 1, so Q record n of the log is the state after native tick n. Lives, ring, angles exact, particles within
    # EMIT_TOLERANCE relative (the emission points are the native TGP transform); the source car (0 player, else
    # an opponent).
    qs = [(task, angles) for _, task, _, angles, _, _, _ in spin_ticks(list(frames(log)))]
    st = dict(rows=0, equal=0, opponent=0); spin_worst = 0.0; spin_bad = []
    for frame, v in sorted(spin.items()):
        k = int(v['n'])
        if not 0 <= k < len(qs):
            continue
        q = qs[k]
        particles, lives, ring, angles = spin_state(*q)
        native_p = [int(w, 16) for w in v['p'].split('.')]
        same = ([int(x) for x in v['life'].split('.')] == lives and int(v['ring']) == ring and
                [int(x, 16) for x in v['angles'].split('.')] == angles)
        g = struct.unpack('<48f', struct.pack('<48I', *native_p)); e = struct.unpack('<48f', struct.pack('<48I', *particles))
        error = max((abs(a-b)/max(1.0, abs(b)) for s in range(8) if lives[s] for a, b in
                     zip(g[6*s:6*s+6], e[6*s:6*s+6])), default=0.0)
        spin_worst = max(spin_worst, error)
        st['rows'] += 1; st['opponent'] += v['owner'] != '0'
        if same and error <= EMIT_TOLERANCE: st['equal'] += 1
        elif len(spin_bad) < 6: spin_bad.append((frame, same, f'{error:.2e}'))
    spin_ok = st['rows'] > 2500 and st['equal'] == st['rows']
    print(f"{'PASS' if spin_ok else 'FAIL'} {name} native spin smoke: {st['equal']}/{st['rows']} race ticks with the "
          f"original's task state (lives, ring, angles exact; particles within {EMIT_TOLERANCE} relative: largest "
          f"{spin_worst:.2e}); source an opponent on {st['opponent']} ticks")
    for b in spin_bad: print('   ', b)
    return ok and spin_ok


def main():
    if len(sys.argv) != 7:
        print(__doc__); return 2
    checker, harness, assets, traces, logs, rom_dir = sys.argv[1:7]
    if subprocess.run([checker]).returncode:
        print('FAIL: car_effects_check rules'); return 1
    find = lambda dirs, name: next((Path(d)/name for d in dirs.split(';') if d and (Path(d)/name).is_file()), None)
    present = [(r, find(logs, f'fx-{r}.fx')) for r in RUNS]
    present = [(r, p) for r, p in present if p]
    if not present or not (Path(rom_dir)/'program.bin').is_file() or not (Path(assets)/'drive.dnd').is_file():
        print('skipped: no effects log (fx-*.fx), ROM images or asset set'); return 77
    rom = Rom(rom_dir)
    ok = True
    for r, log in present:
        ok &= check_log(checker, assets, rom, log, r)
        ok &= check_spin_log(checker, assets, rom, list(frames(log)), r)
        trace = find(traces, 'reference-finish-fast.trace' if r == 'fast' else f'reference-damage-{r}.trace')
        if r != 'V' and trace:
            ok &= check_native(harness, assets, rom, trace, log, r)
    print(f"{'PASS' if ok else 'FAIL'}: logs {' '.join(r for r, _ in present)}")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
