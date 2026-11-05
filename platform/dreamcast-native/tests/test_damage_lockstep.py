"""Damage and wrong-way lockstep: the native race (tests/trace_race_native.c, production and carry modes) against the
original's runs that drive into the walls and against the traffic (spec: build/dc-native-damage.*/spec-damage.md
sections 4, 9).

usage: test_damage_lockstep.py TRACE_RACE_NATIVE ASSET_DIR "TRACE_DIR[;TRACE_DIR...]"

Each TRACE_DIR is searched for the reference traces (m2segment --trace-race with the damage fields dmg/body/crashes/
tilt/pitch of tools/trace_race.hpp; ROM output, never committed). Inputs: recipes/inputs/inputs-damage-*.txt, the
fast race's inputs to frame 3149, then steer/brake segments to frame 7600; every run times out in lap 1.
  reference-damage-A.trace  wall hits 3396/3458, wrong way on 823 rows
  reference-damage-C.trace  wall hits 3448/3702, wrong way on 772 rows, two spins with contact (9148 clears, below)
  reference-damage-E.trace  hits 3473/4589/5635/6078, crashes at 5635 and 6170, wrong way on 1601 rows
  reference-damage-F.trace  hits 3396/3570/4425, crashes at 4425 and 4911, wrong way on 2176 rows
  reference-damage-G.trace  hits 3396/3570/5350, crash at 5350, wrong way on 910 rows
  reference-damage-H.trace  hits 3470/4227/4279/6014, crashes at 4227 and 6014, wrong way on 2097 rows
Native row i (tick i) pairs with reference row n = 4+i (FIRST_ROW 5). On every paired row, every field the player's
car row (id 0) has on both sides must be equal (pose, speed, decisions, flags with bit 28 = a818's wrong way, gear,
road, surface, damage +208, body +14, crash count, shake, pitch, collision tracking), and the race timeline fields of
the T row too. One exception, outside this rule's reach and reported, not hidden:
  - native rows 0-4: the collision tracking words sx/sz/tx/tz/cside. The original's grid reset keeps the
    previous scene's words in the reused entity pool (trace_race_native.c "carry" mode); a cold native start zeroes
    them.
The opponents (ids 1-39, every row up to the run's limit) must be equal in every field too, the results phases
included: at time up the phase handler 4110 parks the opponents (flags bit 31 cleared, 41b4..41dc) while the
interaction object 1210c keeps running (sort, timers, passing, collision pass 13f08 on the parked field;
race_scene.h nm_race_scene_results_field). Not compared for them:
  - body (+14: rewritten from the level-of-detail lists on every drawn frame, 7668..77a0) and tilt (the reference
    prints the shared shake word 501844 on every car row; the native row carries the car's own tilt) - presentation;
  - the collision tracking words sx/sz/tx/tz/crel/cside: until 13f08 first selects a car into its 14-car window they
    hold the previous scene's values (attract and select screens run 13f08 on the same pool), and the race's first
    sort pass reads the previous scene's leading lap 5014ba; neither is reset at the race entry. A cold start differs
    there; run A's contact of cars 16 and 20 at native row 1752 consumes car 16's carried words, so A's opponents are
    compared up to row 1751 here;
  - the reference rows whose sample is mid-tick: m2segment samples at the vblank, and on these frames the i960 was
    still inside the frame's task list (between the frame start 1758 and the call of 15310 after the collision pass):
    native rows 2-4 (frames 2585-2587, the opponents' updates cde0 of cars 34-39 not yet run) in every run, and G's
    row 3628 (frame 6211, inside 13f08 called from 12144). Probe: build/dc-native-opp.m2 (S rows with mid=1).
Each run is checked a second time in carry mode with --start-lap -1: the harness starts from the previous scene's
tracking words of the reference row FIRST_ROW-1 and 5014ba = -1 (the attract's last sort pass before START at frame
1290; build/dc-native-opp.m2 logs it per frame). Then the player, the timeline and every opponent field (body/tilt
aside) must be equal on every row but the mid-tick ones (and on those the player's tracking words, which 13f08 writes
after the sample), run A included: its opponent divergence is that start state alone.
Run C is the case of the contact-timer clear of 9148 (include/contact_clear.h): the player's draw clears +98
(response_ticks) and +99 (boundary.condition) while the spin angle +d6 is outside +-0x1000. C has 19 clears, on the
ticks of native rows 3580-3592 (+99 of the wall hit, +d6 -8977..-18620) and 3626-3658 (+98), as the original's log
(spec-effects.md section 4); without the clear C differs from row 3581 on (x, z, yaw). The other runs have none.
Exits 77 when no trace is found.
"""
import subprocess, sys, tempfile
from pathlib import Path

FIRST = 5
# name: (reference file, native rows checked (None = all), opponents' rows checked in production (None = all),
#        mid-tick reference rows)
MID_TICK_START = (2, 3, 4)
CASES = {
    'A': ('reference-damage-A.trace', None, 1752, MID_TICK_START),
    'C': ('reference-damage-C.trace', None, None, MID_TICK_START),
    'E': ('reference-damage-E.trace', None, None, MID_TICK_START),
    'F': ('reference-damage-F.trace', None, None, MID_TICK_START),
    'G': ('reference-damage-G.trace', None, None, MID_TICK_START + (3628,)),
    'H': ('reference-damage-H.trace', None, None, MID_TICK_START),
}
NOT_COMPARED = {'n', 'id', 'slot'}
START_CARRY = {'sx', 'sz', 'tx', 'tz', 'cside'}
OPPONENT_PRESENTATION = {'body', 'tilt'}
TRACKING = {'sx', 'sz', 'tx', 'tz', 'cside', 'crel'}
# 5014ba when the original's race starts in these recordings (START at frame 1290, 1224c's last pass in the attract).
START_LAP = -1
TIMELINE = ['phase', 'remaining', 'clock', 'count', 'laps', 'finish', 'display', 'hold', 'award', 'camera', 'stage',
            'ctr', 'aux', 'notify']


def rows(path):
    """T rows, the player's C rows by n and the opponents' C rows by (n, id), as dicts of the raw text fields."""
    timeline, player, opponents = {}, {}, {}
    with open(path) as f:
        for line in f:
            if line.startswith('T '):
                v = dict(p.split('=', 1) for p in line.split()[1:])
                timeline[int(v['n'])] = v
            elif line.startswith('C '):
                v = dict(p.split('=', 1) for p in line.split()[1:])
                if v['id'] == '0':
                    player[int(v['n'])] = v
                else:
                    opponents[int(v['n']), int(v['id'])] = v
    return timeline, player, opponents


def compare_opponents(reference_c, native_c, limit, mid_tick, skip):
    """Rows and fields compared, and the first difference (row, n, car, fields) or None."""
    rows_ok = fields = 0
    for (i, car) in sorted(native_c):
        if (limit is not None and i >= limit) or i in mid_tick:
            continue
        a = reference_c.get((FIRST - 1 + i, car))
        if a is None:
            continue
        b = native_c[i, car]
        keys = [k for k in a if k in b and k not in NOT_COMPARED and k not in OPPONENT_PRESENTATION and k not in skip]
        fields += len(keys)
        differ = [k for k in keys if a[k] != b[k]]
        if differ:
            return fields, (i, FIRST - 1 + i, car, [(k, a[k], b[k]) for k in differ[:6]])
    return fields, None


def wrong_way(flags):
    return (int(flags, 16) >> 28) & 1


def run_harness(harness, assets, ref, mode):
    """The harness's trace of ref in mode (argument list), parsed; or the failure text."""
    with tempfile.TemporaryDirectory() as tmp:
        nat = Path(tmp) / 'native.trace'
        run = subprocess.run([harness, assets, str(ref), str(nat), str(FIRST), '14000'] + mode,
                             capture_output=True, text=True)
        if run.returncode:
            return None, f'harness exit {run.returncode} ({" ".join(mode)})\n{run.stdout}{run.stderr}'
        return rows(nat), None


def check(native, reference, limit, opponent_limit, mid_tick, carry):
    """Problems and the counts of one comparison. carry: the harness had the previous scene's words."""
    native_t, native_c, native_o = native
    reference_t, reference_c, reference_o = reference
    problems = []
    paired = fields = reverse = 0
    first = None
    for i in sorted(native_c):
        if limit is not None and i >= limit:
            break
        n = FIRST - 1 + i
        if n not in reference_c or n not in reference_t or i not in native_t:
            continue
        paired += 1
        a, b = reference_c[n], native_c[i]
        # Production: the carried words of rows 0-4. Carry: on a mid-tick sample 13f08 has not yet written the
        # player's tracking words of that tick (every other player field is written before the sample).
        skip = (START_CARRY if not carry and i < 5 else set()) | (TRACKING if carry and i in mid_tick else set())
        keys = [k for k in a if k in b and k not in NOT_COMPARED and k not in skip]
        keys += [f'T.{k}' for k in TIMELINE]
        fields += len(keys)
        reverse += wrong_way(b['flags'])
        differ = [k for k in keys if (reference_t[n][k[2:]] != native_t[i][k[2:]] if k.startswith('T.')
                                      else a[k] != b[k])]
        if differ and first is None:
            first = (i, n, differ)
    expected = (limit if limit is not None else len(native_c))
    if paired != expected:
        problems.append(f'{paired} rows paired, expected {expected}')
    if first:
        i, n, differ = first
        shown = ', '.join(f'{k}=ref:{(reference_t[n] if k.startswith("T.") else reference_c[n])[k.removeprefix("T.")]}'
                          f'/nat:{(native_t[i] if k.startswith("T.") else native_c[i])[k.removeprefix("T.")]}'
                          for k in differ[:6])
        problems.append(f'first difference at native row {i} (reference n={n}): {shown}')
    if not reverse:
        problems.append('the player never goes the wrong way (flags bit 28) in the compared rows')
    if carry:
        opponent_limit = None
    opponent_fields, opponent_first = compare_opponents(reference_o, native_o, opponent_limit, mid_tick,
                                                        set() if carry else TRACKING)
    if opponent_first:
        i, n, car, differ = opponent_first
        problems.append(f'opponent {car} differs at native row {i} (reference n={n}): ' +
                        ', '.join(f'{k}=ref:{x}/nat:{y}' for k, x, y in differ))
    opponent_rows = (opponent_limit if opponent_limit is not None else expected) - len(mid_tick)
    return problems, (paired, fields, reverse, opponent_rows, opponent_fields)


def main():
    if len(sys.argv) != 4:
        print(__doc__); return 2
    harness, assets, dirs = sys.argv[1], sys.argv[2], [Path(d) for d in sys.argv[3].split(';') if d]
    failures = ran = 0
    for name, (file, limit, opponent_limit, mid_tick) in CASES.items():
        ref = next((d / file for d in dirs if (d / file).is_file()), None)
        if ref is None:
            print(f'{name}: no {file}, skipped'); continue
        ran += 1
        reference = rows(ref)
        lines, problems = [], []
        for mode, carry in ((['production'], False), (['carry', '--start-lap', str(START_LAP)], True)):
            native, error = run_harness(harness, assets, ref, mode)
            if error:
                problems.append(error); continue
            found, (paired, fields, reverse, opponent_rows, opponent_fields) = check(
                native, reference, limit, opponent_limit, mid_tick, carry)
            problems += [f'{mode[0]}: {p}' for p in found]
            if carry:
                lines.append(f'carry (previous scene\'s words, 5014ba {START_LAP}): player, timeline and opponents '
                             f'equal in every field on {opponent_rows} rows ({fields} + {opponent_fields} fields)')
            else:
                lines.append(f'player and timeline equal on {paired}/{paired} rows ({fields} fields), wrong way on '
                             f'{reverse} rows; opponents 1-39 equal on {opponent_rows} rows ({opponent_fields} fields, '
                             f'tracking words not compared)')
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
