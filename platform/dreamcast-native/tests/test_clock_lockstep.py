"""The frame-counter animations of Advanced (include/clock_actors.h) against the original's draws, frame by frame.

usage: test_clock_lockstep.py CLOCK_JSON NATIVE_CLOCK ORIGINAL_DRAWS [--report N]

CLOCK_JSON      sources/clock.json of tools/prepare_world_objects.py (each group's handler and per frame the lists
                it draws, in the order of the asset's courseN-actors.dna)
NATIVE_CLOCK    tests/trace_race_native.c --clock: per tick the frame each group draws (-1 none), keyed by reference
                frame, as render/scene/practice.c picks it from the native race and podium
ORIGINAL_DRAWS  the original game's calls of 1786c from the eight handlers' ticks over the same run ("D frame ip task
                list counter" a call, "F frame counter flags phase" a frame: build/dc-native-actors.work/clock/
                clock_run.cpp, the recompiled game with the run's input script; ROM output, never committed)

Every native row is compared with the original's frame of the same number: the counter 5010b0 the native row used
must be the one the original's frame ended with, and the lists each group drew in the original's logic pass of that
counter must be those of the native frame (a group the native draws nothing for draws nothing there). A group is
identified by its handler (the call's address), the list it drew first in the frame and, where two groups of one
handler could draw it, the task (learned from its unambiguous draws); the native frame's lists (221f10: three, in
their order) must be the ones drawn (a table may hold one list twice: 222cf0's frames 6 and 7). The one exception,
counted apart: a podium stage's first native row, where the original's actors already see the new shot's cells (its
pool holds the next stage a tick earlier) and draw what the native draws on its next row, with this row's counter. Exits 1 on any difference, 77 without the files."""
import argparse
import collections
import json
import sys
from pathlib import Path

HANDLER_CODE = {0x220304: (0x220320, 0x2203a4), 0x2209e4: (0x220a00, 0x220a7c), 0x220a7c: (0x220a98, 0x220b14),
                0x221f10: (0x221f30, 0x222030), 0x222030: (0x222050, 0x222090), 0x222090: (0x2220b0, 0x22214c),
                0x222c48: (0x222c64, 0x222cf0), 0x222cf0: (0x222d10, 0x222d30)}


def handler_of(ip):
    for handler, (low, high) in HANDLER_CODE.items():
        if low <= ip < high:
            return handler
    raise ValueError(f'call at {ip:x} is in no clock handler')


def original(path, groups):
    """({counter: {group: lists drawn}}, {frame: counter after it}) of the original's run: the draws of each logic
    pass by the counter 5010b0 its handlers read (a board frame may hold two passes: the loading frames)."""
    by_first = collections.defaultdict(list)   # (handler, first list) -> [(group, k)]
    for g, group in enumerate(groups):
        for k, lists in enumerate(group['frames']):
            by_first[group['handler'], lists[0]].append((g, k))
    frames, task_group, pending = {}, {}, []
    draws = collections.defaultdict(list)
    counters = {}
    for line in Path(path).read_text().splitlines():
        f = line.split()
        if f[0] == 'D':
            draws[int(f[5])].append((handler_of(int(f[2], 16)), int(f[3], 16), int(f[4], 16), int(f[5])))
        elif f[0] == 'F':
            counters[int(f[1])] = int(f[2])
    for frame in sorted(draws):
        calls, at, drawn = draws[frame], 0, {}
        while at < len(calls):
            handler, task, first, counter = calls[at]
            # A group's table may hold one list twice (222cf0's frames 6 and 7): the group decides, not the frame.
            candidates = sorted({c[0] for c in by_first.get((handler, first), [])})
            if len(candidates) > 1:   # one actor (task) may draw several groups: 222090's alternatives
                candidates = [c for c in candidates if c in task_group.get(task, ())] or candidates
            if len(candidates) != 1:
                raise ValueError(f'frame {frame}: list {first:x} of {handler:x} matches {len(candidates)} groups')
            g = candidates[0]
            k = next(k for gg, k in by_first[handler, first] if gg == g)
            task_group.setdefault(task, set()).add(g)
            lists = groups[g]['frames'][k]
            got = [c[2] for c in calls[at:at+len(lists)]]
            if got != lists or any(c[1] != task or c[3] != counter for c in calls[at:at+len(lists)]):
                raise ValueError(f'frame {frame}: group {g} drew {[hex(x) for x in got]}, its frame {k} is {[hex(x) for x in lists]}')
            if g in drawn:
                raise ValueError(f'frame {frame}: group {g} drawn twice')
            drawn[g] = tuple(lists)
            at += len(lists)
        frames[frame] = drawn
    return frames, counters


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('clock_json', type=Path)
    parser.add_argument('native', type=Path)
    parser.add_argument('draws', type=Path)
    parser.add_argument('--report', type=int, default=10, help='differences printed (default 10)')
    args = parser.parse_args()
    if not all(p.exists() for p in (args.clock_json, args.native, args.draws)):
        print('SKIP: files missing')
        return 77
    groups = json.loads(args.clock_json.read_text())['groups']
    drawn, counters = original(args.draws, groups)
    native_rows = []
    for line in args.native.read_text().splitlines():
        f = line.split()
        if f[0] != 'K':
            continue
        frame, counter, stage = int(f[2].split('=')[1]), int(f[3].split('=')[1]), int(f[4].split('=')[1])
        if len(f[5:]) != len(groups):
            raise ValueError('native rows and clock.json disagree on the groups')
        native_rows.append((frame, counter, stage, {g: int(v) for g, v in enumerate(f[5:]) if int(v) >= 0}))
    rows = equal = setup = 0
    with_draw = collections.Counter()
    differences = []
    lists_at = lambda g, counter: tuple(groups[g]['frames'][(counter >> groups[g]['shift']) & (len(groups[g]['frames'])-1)])
    for i, (frame, counter, stage, native) in enumerate(native_rows):
        if frame not in counters:
            continue
        rows += 1
        theirs = drawn.get(counter, {})
        mine = {g: tuple(groups[g]['frames'][k]) for g, k in native.items()}
        for g in native:
            with_draw[g] += 1
        if mine == theirs and counter == counters[frame]:
            equal += 1
            continue
        # A podium stage's setup row (the native's first row of a stage): the original's pool already holds the next
        # stage on the tick before (compare_race_trace.py --podium, timeline-only rows), so the cells its actors see
        # here are the new shot's, the native's still the old one's: the original draws there what the native draws
        # on its next row, with this row's counter.
        following = native_rows[i+1] if i+1 < len(native_rows) else None
        if stage >= 0 and i and native_rows[i-1][2] >= 0 and native_rows[i-1][2] != stage and following and \
                counter == counters[frame] and theirs == {g: lists_at(g, counter) for g in following[3]}:
            setup += 1
            continue
        differences.append((frame, counter, counters[frame], native,
                            {g: [hex(x) for x in lists] for g, lists in theirs.items()}))
    for d in differences[:args.report]:
        print('frame %d: native counter %d (original %d) frames %s, original lists %s' % d)
    names = ', '.join('%d (%x) %d' % (g, groups[g]['handler'], with_draw[g]) for g in range(len(groups)))
    print(f'clock lockstep: {equal} of {rows} rows equal, {setup} podium stage setup rows as the native\'s next row; '
          f'rows drawn per group: {names}')
    return 0 if rows and equal+setup == rows else 1


if __name__ == '__main__':
    sys.exit(main())
