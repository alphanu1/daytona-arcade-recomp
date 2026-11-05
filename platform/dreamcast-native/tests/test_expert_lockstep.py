"""Expert's world actors (include/expert_actors.h, include/clock_actors.h) against the original's draws, frame by frame:
the lists each actor draws and the matrices it draws them with.

usage: test_expert_lockstep.py CLOCK_JSON NATIVE_CLOCK ORIGINAL_DRAWS [--report N] [--rotation E] [--translation E]

CLOCK_JSON      sources/clock.json of tools/prepare_world_objects.py (each group's handler, actor record, per frame the
                lists it draws and their placements, whether it is PLACED; the order of the asset's courseN-actors.dna)
NATIVE_CLOCK    tests/trace_race_native.c --clock on the course: per tick ("K") the frame each group draws (-1 none) and
                ("P") the placement of each drawn group of an actor with state (nm_xa_pose_matrix)
ORIGINAL_DRAWS  m2segment --trace-race --trace-clock --trace-clock-expert over the same run: every call of 1786c of
                Expert's actor code with the TGP's matrix ("X"), the matrix each actor tick starts from, the view
                ("V"), and per frame the counter 5010b0 ("F"); ROM output, never committed

Every native row is compared with the original's logic pass of the counter it used (the counter must be the one the
original's frame ended with): per actor (the task, by its handler's code, position and heading words) the lists drawn
in order must be those of the native's groups of that actor (their frames' lists, in group order), and each draw's
placement, the original's view inverse times its matrix, the native's: a baked group's own (its frames' placements,
include/clock_actors.h), a PLACED group's from its P line, a world group's the identity, within --rotation (default
0.002) on the 3x3 and --translation (default 0.02 units) on the offset. Reported: rows, draws per handler, the largest
differences. The exception counted apart, as tests/test_clock_lockstep.py: a podium stage's first native row, where the
original's actors draw what the native draws on its next row, and a stage's last native row, where the actors that test
the player's profile already see the next stage's car. Exits 1 on any difference, 77 without the files."""
import argparse
import collections
import json
import math
import struct
import sys
from pathlib import Path

import numpy as np

# The code of each handler's ticks (platform/dreamcast-native/tools/trace_race.hpp expert_code).
HANDLER_CODE = {0x22108c: (0x2210ac, 0x221d24), 0x22214c: (0x22216c, 0x222264), 0x22302c: (0x222288, 0x2224b0),
                0x222594: (0x2225b4, 0x222678), 0x222678: (0x222698, 0x22275c), 0x22275c: (0x22277c, 0x222840),
                0x222840: (0x222860, 0x222924), 0x223bc0: (0x223be0, 0x223cc8), 0x223ce0: (0x223cfc, 0x223d44),
                0x223d48: (0x223d94, 0x22430c), 0x22430c: (0x22432c, 0x224730), 0x224730: (0x22475c, 0x224854),
                0x224854: (0x22487c, 0x224cd0), 0x224cd0: (0x224cec, 0x224de8)}


def handler_of(ip):
    for handler, (low, high) in HANDLER_CODE.items():
        if low <= ip < high:
            return handler
    raise ValueError(f'draw at {ip:x} is in no Expert handler')


def f32(word):
    return struct.unpack('<f', struct.pack('<I', word))[0]


def matrix(words):
    """A column-major 3x4 (TGP order: three columns, then the offset) as (3x3, offset) float64."""
    m = np.array([f32(w) for w in words], np.float64)
    return m[:9].reshape(3, 3).T, m[9:12]


def ops_matrix(ops):
    """The placement of an operation list (tools/prepare_world_objects.py matrix_of, float64)."""
    m, t = np.eye(3), np.zeros(3)
    for op in ops or []:
        if op[0] == 'T':
            t = t+m @ np.array(op[1:], np.float64)
        elif op[0] == 'S':
            m = m*np.array(op[1:], np.float64)[None, :]
        else:
            a = float(int(op[1]) & 0xffff)*math.pi/32768.0
            c, s = math.cos(a), math.sin(a)
            r = {'RX': [[1, 0, 0], [0, c, s], [0, -s, c]], 'RY': [[c, 0, -s], [0, 1, 0], [s, 0, c]],
                 'RZ': [[c, s, 0], [-s, c, 0], [0, 0, 1]]}[op[0]]
            m = m @ np.array(r)
    return m, t


def original(path):
    """({counter: {task: [(handler, list, model3x3, offset, words)]}}, {frame: counter after it}, task keys)."""
    draws, counters, keys = collections.defaultdict(lambda: collections.defaultdict(list)), {}, {}
    pending, view = [], None
    for line in Path(path).read_text().splitlines():
        f = line.split()
        if f[0] == 'V':   # the view of the tick that follows (its draws use the last one before them)
            view = (int(f[1]), matrix([int(w, 16) for w in f[3:15]]))
        elif f[0] == 'X':
            pending.append((f, view))
        elif f[0] == 'F':
            counters[int(f[1])] = int(f[2])
    for f, view in pending:
        frame, ip, task, lst, counter = int(f[1]), int(f[2], 16), int(f[3], 16), int(f[4], 16), int(f[5])
        handler = handler_of(ip)
        x, z, heading = f32(int(f[6], 16)), f32(int(f[7], 16)), int(f[8])
        m, t = matrix([int(w, 16) for w in f[9:21]])
        # A logic pass may run on past the board frame's end (the frame number moves on mid-pass): the view is the
        # last one logged before the draw, whatever its frame.
        if view is None:
            raise ValueError(f'frame {frame}: a draw without the view')
        v, vt = view[1]
        vi = np.linalg.inv(v)
        draws[counter][task].append((handler, lst, vi @ m, vi @ (t-vt), [int(w, 16) for w in f[21:28]]))
        keys.setdefault(task, (handler, x, z, heading))
    return draws, counters, keys


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('clock_json', type=Path)
    parser.add_argument('native', type=Path)
    parser.add_argument('draws', type=Path)
    parser.add_argument('--report', type=int, default=10, help='differences printed (default 10)')
    parser.add_argument('--rotation', type=float, default=0.002)
    parser.add_argument('--translation', type=float, default=0.02)
    args = parser.parse_args()
    if not all(p.exists() for p in (args.clock_json, args.native, args.draws)):
        print('SKIP: files missing')
        return 77
    groups = json.loads(args.clock_json.read_text())['groups']
    records = collections.defaultdict(list)   # actor record -> its groups, in order
    for g, group in enumerate(groups):
        records[group['record']].append(g)
    drawn, counters, task_keys = original(args.draws)
    native_rows, placements = [], {}
    for line in args.native.read_text().splitlines():
        f = line.split()
        if f[0] == 'K':
            tick, frame, counter, stage = (int(x.split('=')[1]) for x in f[1:5])
            if len(f[5:]) != len(groups):
                raise ValueError('native rows and clock.json disagree on the groups')
            native_rows.append((tick, frame, counter, stage, [int(v) for v in f[5:]]))
        elif f[0] == 'P':
            tick, g = int(f[1].split('=')[1]), int(f[2].split('=')[1])
            placements[tick, g] = matrix([int(w, 16) for w in f[3:15]])
    # A task is the actor record of its handler whose position (x; z unless the actor moves its z: 224cd0) and
    # heading its words hold.
    def record_of(task):
        handler, x, z, heading = task_keys[task]
        found = [r for r, gs in records.items() if groups[gs[0]]['handler'] == handler and
                 abs(groups[gs[0]]['position'][0]-x) < 1e-3 and groups[gs[0]]['heading'] == heading and
                 (handler == 0x224cd0 or abs(groups[gs[0]]['position'][2]-z) < 1e-3)]
        if len(found) != 1:
            raise ValueError(f'task {task:x} ({handler:x} at {x}, {z}, {heading}) matches {len(found)} actor records')
        return found[0]
    task_record = {task: record_of(task) for task in task_keys}

    def native_draws(row, tick):
        """{record: [(list, model3x3, offset)]} of a native row."""
        out = collections.defaultdict(list)
        for record, gs in records.items():
            for g in gs:
                k = row[g]
                if k < 0:
                    continue
                group = groups[g]
                for i, lst in enumerate(group['frames'][k]):
                    if group['placed']:
                        m, t = placements[tick, g]
                    elif group.get('ops'):
                        m, t = ops_matrix(group['ops'][k][i])
                    elif (tick, g) in placements:
                        m, t = placements[tick, g]
                    else:
                        m, t = np.eye(3), np.zeros(3)
                    out[record].append((lst, m, t))
        return out

    rows = equal = setup = cut = 0
    per_handler = collections.Counter()
    worst_r = worst_t = 0.0
    differences = []

    def compare(mine, theirs):
        """None when equal, else what differs; updates the largest matrix differences."""
        nonlocal worst_r, worst_t
        if set(mine) != set(theirs):
            return f'actors {sorted("%x" % r for r in mine)} against {sorted("%x" % r for r in theirs)}'
        for record in mine:
            a, b = mine[record], theirs[record]
            if [x[0] for x in a] != [x[1] for x in b]:
                return f'actor {record:x} lists {[hex(x[0]) for x in a]} against {[hex(x[1]) for x in b]}'
            for (lst, m, t), (_, _, rm, rt, _) in zip(a, b):
                er, et = float(np.abs(m-rm).max()), float(np.abs(t-rt).max())
                worst_r, worst_t = max(worst_r, er), max(worst_t, et)
                if not (er <= args.rotation and et <= args.translation):
                    return f'actor {record:x} list {lst:x}: placement differs by {er:.5f} / {et:.4f}'
        return None
    for i, (tick, frame, counter, stage, row) in enumerate(native_rows):
        if frame not in counters:
            continue
        rows += 1
        theirs = {task_record[task]: d for task, d in drawn.get(counter, {}).items()}
        mine = native_draws(row, tick)
        why = compare(mine, theirs) if counter == counters[frame] else f'counter {counter} against {counters[frame]}'
        if why is None:
            equal += 1
            for record in mine:
                per_handler[groups[records[record][0]]['handler']] += 1
            continue
        following = native_rows[i+1] if i+1 < len(native_rows) else None
        if stage >= 0 and i and native_rows[i-1][3] >= 0 and native_rows[i-1][3] != stage and following and \
                counter == counters[frame] and compare(native_draws(following[4], following[0]), theirs) is None:
            setup += 1
            continue
        # A podium stage's last native row: the original's player entity 501220 is already the next stage's car (its
        # pool holds the next stage a tick earlier: compare_race_trace.py --podium), so the actors that test its
        # profile decide as the native does on its next row. Accepted when every other actor is equal and each actor
        # drawn on one side only is drawn (in its lists) or not as the native's next row has it.
        if stage >= 0 and following and following[3] >= 0 and following[3] != stage and counter == counters[frame]:
            nxt = native_draws(following[4], following[0])
            alone = set(mine) ^ set(theirs)
            common_mine = {r: mine[r] for r in mine if r not in alone}
            common_theirs = {r: theirs[r] for r in theirs if r not in alone}
            if alone and compare(common_mine, common_theirs) is None and \
                    all((r in theirs) == (r in nxt) and (r not in theirs or [x[1] for x in theirs[r]] == [x[0] for x in nxt[r]])
                        for r in alone):
                cut += 1
                continue
        differences.append(f'tick {tick} frame {frame}: {why}')
    for d in differences[:args.report]:
        print(d)
    names = ', '.join('%x %d' % (h, n) for h, n in sorted(per_handler.items()))
    print(f'expert lockstep: {equal} of {rows} rows equal, {setup} podium stage setup rows and {cut} stage cut rows as '
          f'the native\'s next row; largest placement difference {worst_r:.5f} (3x3), {worst_t:.4f} units; rows drawn '
          f'per handler: {names}')
    return 0 if rows and equal+setup+cut == rows else 1


if __name__ == '__main__':
    sys.exit(main())
