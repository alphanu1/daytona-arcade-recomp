"""Live-play edge states against the original: the native race (tests/trace_race_native.c, carry mode) with the
original's runs (m2segment --trace-race) that reach states the scripted races never do. Before the fixes each of
these made a live tick reject ("Native practice rejected tick N", the first live play session).

usage: test_negative_speed_lockstep.py TRACE_RACE_NATIVE ASSET_DIR "TRACE_DIR[;TRACE_DIR...]"

Reference traces (ROM output, never committed), made from the repo root with m2segment of this branch:
  reference-negspeed-base.trace     M2_POKE_ROW=662 M2_POKE_SPEED=bb8e38e4 m2segment build/rom_cache/daytona93 4200
                                    --inputs platform/dreamcast-native/recipes/inputs/inputs-damage-C.txt --trace-race NEW
                                    speed -0.0043 at the player's block entry on the road: speed index -1, whose drag
                                    word (0x28b5d1c) is the erased 0xffffffff, a NaN (include/drive.h, forces.h)
  reference-negspeed-surface.trace  the same with M2_POKE_ROW=670 M2_POKE_SPEED=bcf5c28f: speed index -7 with wheels
                                    on the grass, the surface table's word (0x28f6d04) is the NaN
  reference-negspeed-table.trace    M2_POKE_ROW=662 M2_POKE_SPEED=bcf5c28f: speed index -7 on the road, the base
                                    word is the 10dd8 table before the drag table (finite)
  reference-yaw-overflow.trace      no poke, inputs inputs-yaw-overflow.txt (a fuzz race, first native frame 2584):
                                    equal front and rear wheel angles in motion at native tick 709, the yaw step's
                                    turn radius divide wraps to infinity and fbd0's cvtri saturates (drive.h)
The speed write is a test input standing in for the collision impulse that leaves the player moving backwards in
live play (fuzz race 14641 reached index -20 that way, 128165 index -1); everything after it is the original's own
rules on both sides. Native row i pairs with reference row n = 4+i (FIRST_ROW 5). On every paired row the player's
fields and the race timeline must be equal, and every opponent field but body/tilt (presentation, as
test_damage_lockstep.py) on the rows that are not mid-tick samples (rows 2-4: the tracking words are skipped there).
Exits 77 when no trace is found.
"""
import subprocess, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_damage_lockstep import (FIRST, MID_TICK_START, NOT_COMPARED, OPPONENT_PRESENTATION, START_LAP, TIMELINE,
                                  TRACKING, compare_opponents, rows)

# name: (reference file, --poke-speed argument or None, native rows sampled mid-tick by the reference beyond
# MID_TICK_START: the player's tracking words still hold the previous tick's, 13f08 has not run yet)
CASES = {
    'base': ('reference-negspeed-base.trace', '662:bb8e38e4', (661, 662)),
    'surface': ('reference-negspeed-surface.trace', '670:bcf5c28f', (669, 670)),
    'table': ('reference-negspeed-table.trace', '662:bcf5c28f', ()),
    'yaw': ('reference-yaw-overflow.trace', None, ()),
}


def check(native, reference, mid_tick):
    native_t, native_c, native_o = native
    reference_t, reference_c, reference_o = reference
    paired = fields = 0
    for i in sorted(native_c):
        n = FIRST - 1 + i
        if n not in reference_c or n not in reference_t or i not in native_t:
            continue
        paired += 1
        a, b = reference_c[n], native_c[i]
        skip = TRACKING if i in mid_tick else set()
        keys = [k for k in a if k in b and k not in NOT_COMPARED and k not in skip]
        fields += len(keys) + len(TIMELINE)
        differ = [k for k in keys if a[k] != b[k]] + [f'T.{k}' for k in TIMELINE if reference_t[n][k] != native_t[i][k]]
        if differ:
            shown = ', '.join(f'{k}=ref:{(reference_t[n] if k.startswith("T.") else a)[k.removeprefix("T.")]}'
                              f'/nat:{(native_t[i] if k.startswith("T.") else b)[k.removeprefix("T.")]}'
                              for k in differ[:6])
            return f'first difference at native row {i} (reference n={n}): {shown}', paired, fields
    opponent_fields, first = compare_opponents(reference_o, native_o, None, mid_tick, set())
    if first:
        i, n, car, differ = first
        return (f'opponent {car} differs at native row {i} (reference n={n}): ' +
                ', '.join(f'{k}=ref:{x}/nat:{y}' for k, x, y in differ)), paired, fields
    if paired < 800:
        return f'only {paired} rows paired', paired, fields
    return None, paired, fields + opponent_fields


def main():
    if len(sys.argv) != 4:
        print(__doc__); return 2
    harness, assets, dirs = sys.argv[1], sys.argv[2], [Path(d) for d in sys.argv[3].split(';') if d]
    failures = ran = 0
    for name, (file, poke, mid_tick) in CASES.items():
        ref = next((d / file for d in dirs if (d / file).is_file()), None)
        if ref is None:
            print(f'{name}: no {file}, skipped'); continue
        ran += 1
        with tempfile.TemporaryDirectory() as tmp:
            nat = Path(tmp) / 'native.trace'
            extra = ['--poke-speed', poke] if poke else []
            run = subprocess.run([harness, assets, str(ref), str(nat), str(FIRST), '14000', 'carry',
                                  '--start-lap', str(START_LAP)] + extra, capture_output=True, text=True)
            if run.returncode:
                failures += 1
                print(f'FAIL {name}: harness exit {run.returncode}\n{run.stdout}{run.stderr}'); continue
            problem, paired, fields = check(rows(nat), rows(ref), MID_TICK_START + mid_tick)
        if problem:
            failures += 1
            print(f'FAIL {name}: {problem}')
        else:
            print(f'{name}: player, timeline and opponents equal on {paired} rows ({fields} fields)')
    if not ran:
        print('SKIP: no reference trace found'); return 77
    print('PASS' if not failures else f'FAIL: {failures} of {ran} runs')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
