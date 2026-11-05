"""Pit stop lockstep: the native race (tests/trace_race_native.c, production and carry modes) against the original's
five runs that drive into the pit lane (spec: build/dc-native-pit.*/spec-pitstop.md, sections 2-9).

usage: test_pit_lockstep.py TRACE_RACE_NATIVE ASSET_DIR "TRACE_DIR[;TRACE_DIR...]"

Each TRACE_DIR is searched for reference-pit-{P,E,C,F,H}.trace (m2segment --trace-race with the damage fields, from
the spec's inputs-pit-*.txt; ROM output, never committed). D is the tick whose player update takes the entry test
(the reference row with handler dc18), S the first pit tick that ends standing, R the release (handler d7fc again):
  P  clean car                       D=4305 S=4751 R=5391 (frames)
  E  damage 0x11 (wall hit)          D=5387 S=5831 R=6471
  C  damage 0x44 (wall hit)          D=5369 S=5815 R=6455
  F  damage 0x1cc, after a crash     D=5713 S=6158 R=6798
  H  damage 0x1cc, crash, lap 0      D=4972 S=5505 R=6145 (entered at 99 km/h)
Native row i (tick i) pairs with reference row n = 4+i (FIRST_ROW 5). Asserted on every paired row of every run:
  - production mode: every field the player's car row (id 0) has on both sides (pose, speed, gear, rpm, desired
    speed, path, flags, damage +208, body +14, crash count, shake, pitch, collision tracking but the carried words
    of rows 0-4), the race timeline of the T row (phase, remaining time 5010d0 frozen on D+2..R+1, clock, 501a84 as
    `hold`, camera mode 5014a4, transition 501713, the aux object's flag, ...) and the keyed camera's tick `ctick`;
    the opponents 1-39 in every field but body/tilt (presentation, as test_damage_lockstep.py) and the tracking words
    sx/sz/tx/tz/crel/cside, on every row (E: rows 0-3102, below);
  - carry mode with --start-lap -1 (the previous scene's tracking words and leading lap, test_damage_lockstep.py):
    the player, the timeline and every opponent field (body/tilt aside) on every row;
  - during the pit (D+1..R-1) the view matrix of the native keyed camera (the race header's tracks 234a9c[2] /
    234aac[2], nm_race_keyed_tracks) within 0.02 of the original's in every element (spec section 5: within 0.012);
  - the native pit state the reference rows do not print (trace_race_native T row `pit=` and `lift=`), by the rules
    the spec checked on the original's logs (check_timeline.py): 50112c = t - D on D+1..R; crew rows 68..73 on and
    rows 74..138 suspended exactly on D+128..S+511; 501a98 = 0xf exactly on S+161..S+352 (0 elsewhere); 501128 = 1
    from S; the keyed track 1 (entry) on D+1..S, 2 (stop) on S+1..R-1, 0 from R; crew time = t - S + 1 on S..S+511
    (0 before, 512 after); +5c = 0.64 (3f23d70a) exactly on S+99..S+460 within the pit, its value at D before S+99
    (a crashed car keeps 0.5388), 0 from S+461.
Exceptions, stated, not hidden:
  - `display` (T row) is not compared: the reference prints 5010dc (the lap message ticks the HUD counts down,
    ab10..ab20), the harness the checkpoint display ticks; it differs on every lap-line crossing (spec section 7);
    a HUD-only word.
  - E at S+161 (native row 3409): the board sampled the frame between crew 2 and crew 3 (the i960 inside the crew
    rows at the vblank), so the reference's +208 still has crew 3's wheel bit 4 (spec section 8): there the
    reference must equal the native value | 0x10, every other field as everywhere.
  - E's opponents in production mode up to row 3102: car 11's tracking words sx/sz are still the previous scene's
    when 13f08 first selects it at row 3103 (the carried-state finding of test_damage_lockstep.py, run A); carry mode
    compares E's opponents on every row.
  - the mid-tick reference rows 2-4 (frames 2585-2587, test_damage_lockstep.py): opponents and ctick.
Exits 77 when no trace is found."""
import struct, subprocess, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_damage_lockstep import rows, compare_opponents, FIRST, NOT_COMPARED, START_CARRY, TRACKING, START_LAP, TIMELINE

TIMELINE = [k for k in TIMELINE if k != 'display'] + ['ctick']
MID_TICK_START = (2, 3, 4)
# name: (file, opponents' rows checked in production (None = all), {native row: (field, bits the reference adds)})
CASES = {
    'P': ('reference-pit-P.trace', None, {}),
    'E': ('reference-pit-E.trace', 3103, {3409: ('dmg', 0x10)}),
    'C': ('reference-pit-C.trace', None, {}),
    'F': ('reference-pit-F.trace', None, {}),
    'H': ('reference-pit-H.trace', None, {}),
}
LIFT = 0x3f23d70a


def bits_float(h):
    return struct.unpack('<f', struct.pack('<I', int(h, 16)))[0]


def run_harness(harness, assets, ref, mode):
    with tempfile.TemporaryDirectory() as tmp:
        nat = Path(tmp) / 'native.trace'
        run = subprocess.run([harness, assets, str(ref), str(nat), str(FIRST), '14000'] + mode,
                             capture_output=True, text=True)
        if run.returncode:
            return None, f'harness exit {run.returncode} ({" ".join(mode)})\n{run.stdout}{run.stderr}'
        return rows(nat), None


def ticks(reference_c):
    """D, S, R as native rows (the reference handler and speed index)."""
    n = sorted(k for k in reference_c)
    d = next(k for k in n if reference_c[k]['upd'] == 'dc18')
    s = next(k for k in n if k > d and reference_c[k]['si'] == '0')
    r = next(k for k in n if k > d and reference_c[k]['upd'] == 'd7fc')
    return d - FIRST + 1, s - FIRST + 1, r - FIRST + 1


def check_player(native, reference, exceptions, carry):
    native_t, native_c, _ = native
    reference_t, reference_c, _ = reference
    paired = fields = 0
    first = None
    for i in sorted(native_c):
        n = FIRST - 1 + i
        if n not in reference_c or n not in reference_t or i not in native_t:
            continue
        paired += 1
        a, b = reference_c[n], native_c[i]
        skip = (START_CARRY if not carry and i < 5 else set()) | (TRACKING if carry and i in MID_TICK_START else set())
        keys = [k for k in a if k in b and k not in NOT_COMPARED and k not in skip]
        timeline = [k for k in TIMELINE if not (k == 'ctick' and i in MID_TICK_START)]
        fields += len(keys) + len(timeline)
        differ = [k for k in keys if a[k] != b[k]] + [f'T.{k}' for k in timeline if reference_t[n][k] != native_t[i][k]]
        if i in exceptions:
            field, extra = exceptions[i]
            if field in differ and int(a[field], 16) == int(b[field], 16) | extra and int(b[field], 16) & extra == 0:
                differ.remove(field)
            elif field not in differ:
                differ.append(f'{field} (expected the mid-list sample: reference = native | {extra:#x})')
        if differ and first is None:
            first = (i, n, differ[:6], {k: (reference_t[n] if k.startswith('T.') else a).get(k.removeprefix('T.')) for k in differ[:6]},
                     {k: (native_t[i] if k.startswith('T.') else b).get(k.removeprefix('T.')) for k in differ[:6]})
    return paired, fields, first


def check_camera(native, reference, d, r):
    native_t = native[0]
    reference_t = reference[0]
    worst = 0.0
    for i in range(d + 1, r):
        a = [bits_float(x) for x in reference_t[FIRST - 1 + i]['view'].split('.')]
        b = [bits_float(x) for x in native_t[i]['view'].split('.')]
        worst = max(worst, max(abs(x - y) for x, y in zip(a, b)))
    return worst


def check_pit_state(native, reference, d, s, r):
    """Problems of the native pit state against the spec's rules, and the number of rows checked."""
    native_t, native_c, _ = native
    reference_c = reference[1]
    problems = []
    lift_d = int(native_t[d]['lift'], 16)
    checked = 0
    for i in range(max(1, d - 50), min(max(native_t), r + 50) + 1):
        t = native_t[i]
        ticks_, crew, time, wheels, suspended, armed, camera = t['pit'].split('.')
        ticks_, crew, time, wheels, suspended, armed, camera = (int(ticks_), int(crew), int(time), int(wheels, 16),
                                                                 int(suspended), int(armed), int(camera))
        lift = int(t['lift'], 16)
        rules = [
            ('50112c = t - D', i <= d or ticks_ == min(i, r) - d),
            ('crew rows on D+128..S+511', bool(crew) == (d + 128 <= i < s + 512)),
            ('rows 74..138 suspended D+128..S+511', bool(suspended) == (d + 128 <= i < s + 512)),
            ('501a98 = 0xf on S+161..S+352, else 0', wheels == (0xf if s + 161 <= i < s + 353 else 0)),
            ('501128 = 1 from S', i <= d or bool(armed) == (i >= s)),
            ('keyed track: 1 on D+1..S, 2 on S+1..R-1, 0 from R',
             camera == (1 if d < i <= s else 2 if s < i < r else 0) or i <= d),
            ('crew time 0 before S, t - S + 1 on S..S+511, 512 after', i <= d + 128 or
             time == (0 if i < s else i - s + 1 if i < s + 512 else 512)),
            ('+5c: its D value, 0.64 on S+99..S+460, 0 from S+461', i <= d or i >= r or
             lift == (LIFT if s + 99 <= i < s + 461 else lift_d if i < s + 99 else 0)),
        ]
        checked += 1
        for name, ok in rules:
            if not ok:
                problems.append(f'native row {i} (D{i - d:+d}, S{i - s:+d}): {name}: pit={t["pit"]} lift={t["lift"]}')
                return problems, checked
    if native_c[r]['si'] != '0' or reference_c[FIRST - 1 + r]['upd'] != 'd7fc':
        problems.append('release row R is not standing / not d7fc')
    return problems, checked


def main():
    if len(sys.argv) != 4:
        print(__doc__); return 2
    harness, assets, dirs = sys.argv[1], sys.argv[2], [Path(x) for x in sys.argv[3].split(';') if x]
    failures = ran = 0
    for name, (file, opponent_limit, exceptions) in CASES.items():
        ref = next((x / file for x in dirs if (x / file).is_file()), None)
        if ref is None:
            print(f'{name}: no {file}, skipped'); continue
        ran += 1
        reference = rows(ref)
        d, s, r = ticks(reference[1])
        lines, problems = [f'D=row {d} S=D+{s - d} R=S+{r - s}'], []
        for mode, carry in ((['production'], False), (['carry', '--start-lap', str(START_LAP)], True)):
            native, error = run_harness(harness, assets, ref, mode)
            if error:
                problems.append(error); continue
            paired, fields, first = check_player(native, reference, exceptions, carry)
            if paired != len(native[1]) or paired < r + 100:
                problems.append(f'{mode[0]}: {paired} rows paired of {len(native[1])}')
            if first:
                i, n, differ, a, b = first
                problems.append(f'{mode[0]}: first player/timeline difference at native row {i} (reference n={n}): '
                                + ', '.join(f'{k}=ref:{a[k]}/nat:{b[k]}' for k in differ))
            limit = None if carry else opponent_limit
            opponent_fields, opponent_first = compare_opponents(reference[2], native[2], limit, MID_TICK_START,
                                                                set() if carry else TRACKING)
            if opponent_first:
                i, n, car, differ = opponent_first
                problems.append(f'{mode[0]}: opponent {car} differs at native row {i} (reference n={n}): ' +
                                ', '.join(f'{k}=ref:{x}/nat:{y}' for k, x, y in differ))
            opponent_rows = (limit if limit is not None else paired) - len(MID_TICK_START)
            if carry:
                lines.append(f'carry: player, timeline and opponents equal on {paired} rows ({fields} + {opponent_fields} fields)')
                continue
            worst = check_camera(native, reference, d, r)
            if not worst <= 0.02:
                problems.append(f'pit camera view differs by {worst:.4g} (> 0.02)')
            state_problems, checked = check_pit_state(native, reference, d, s, r)
            problems += state_problems
            lines.append(f'production: player and timeline equal on {paired}/{paired} rows ({fields} fields), '
                         f'opponents on {opponent_rows} rows ({opponent_fields} fields, tracking words aside); '
                         f'pit camera within {worst:.4g}; pit state rules on {checked} rows')
        if problems:
            failures += 1
            print(f'FAIL {name}: ' + '; '.join(problems))
        else:
            print(f'{name}: ' + '; '.join(lines))
    if not ran:
        print('SKIP: no reference trace found'); return 77
    print('PASS' if not failures else f'FAIL: {failures} of {ran} runs')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
