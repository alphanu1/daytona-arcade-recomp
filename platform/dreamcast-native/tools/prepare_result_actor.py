"""Export authored result animation targets/timelines, not sampled poses.

Usage: prepare_result_actor.py PROGRAM.bin NEW_HEADER.h
Generated ROM-derived data must stay local/ignored.
"""
import argparse
import hashlib
import math
from pathlib import Path
import struct

from inspect_course import IMAGES, words


def extract(program):
    size, digest = IMAGES['program.bin']
    if len(program) != size or hashlib.sha256(program).hexdigest() != digest:
        raise ValueError('unrecognized daytona93 program image')
    addresses = [0x23282c]  # The actor's initial target, before either timeline.
    timelines = []
    for address in (0x23269c, 0x232744):
        table = []
        for i in range(21):
            tick, target = words(program, 0x200000, address+8*i, 2)
            if tick > 10000 or (i and tick <= table[-1][0]):
                raise ValueError('invalid result timeline ordering')
            if target not in addresses:
                addresses.append(target)
            table.append((tick, addresses.index(target)))
        if table[-1][0] != 10000:
            raise ValueError('missing result timeline sentinel')
        timelines.append(table)
    targets = []
    for address in addresses:
        raw = struct.pack('<32I', *words(program, 0x200000, address, 32))
        rows = [struct.unpack_from('<fffhh', raw, 16*i) for i in range(8)]
        if any(not math.isfinite(v) for row in rows for v in row[:3]):
            raise ValueError('nonfinite result target')
        targets.append(rows)
    raw = struct.pack('<16I', *words(program, 0x200000, 0x2327ec, 16))
    initial = [struct.unpack_from('<ff', raw, 8*i) for i in range(8)]
    if any(not math.isfinite(v) for row in initial for v in row):
        raise ValueError('nonfinite initial result position')
    announcements = []
    for address in words(program, 0x200000, 0x2344fc, 44):
        table = []
        for i in range(4):
            raw, command = words(program, 0x200000, address+8*i, 2)
            tick = raw if raw < 0x80000000 else raw-0x100000000
            if command & 0xffff00 != 0xae2200 or (i and tick >= table[-1][0]):
                raise ValueError('invalid result rank announcement')
            table.append((tick, command))
            if tick == -64:
                break
        if table[-1][0] != -64:
            raise ValueError('missing result announcement terminal row')
        announcements.append(table)
    return initial, targets, timelines, announcements


def render(initial, targets, timelines, announcements):
    def f(value):
        return value.hex()+'f'  # Exact float32 values, no decimal rounding.
    lines = ['/* Offline-authored targets; no ROM pointers or recorded frames. */',
             '#include <stdalign.h>', '#include "result_actor.h"',
             'alignas(32) static const nm_result_vec2_t nm_result_initial[8] = {']
    lines += [f'    {{{f(x)},{f(y)}}},' for x, y in initial]
    lines += ['};', f'alignas(32) static const nm_result_target_t nm_result_targets[{len(targets)}][8] = {{']
    for rows in targets:
        lines.append('    {')
        lines += [f'        {{{{{f(x)},{f(y)}}},{f(rate)},{a},{b}}},' for x,y,rate,a,b in rows]
        lines.append('    },')
    lines += ['};', 'static const nm_result_key_t nm_result_keys[2][21] = {']
    for rows in timelines:
        lines.append('    {'+','.join(f'{{{tick},{target}}}' for tick,target in rows)+'},')
    lines += ['};', 'static const uint8_t nm_result_announcement_counts[44] = {'+
              ','.join(str(len(t)) for t in announcements)+'};',
              'static const nm_result_announcement_t nm_result_announcements[44][4] = {']
    for table in announcements:
        lines.append('    {'+','.join(f'{{{tick},0x{command:06x}}}' for tick,command in table)+'},')
    return '\n'.join(lines+['};', ''])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('program', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    data = extract(args.program.read_bytes())
    with args.output.open('x') as out:
        out.write(render(*data))
    print(f'Exported {len(data[1])} targets, 42 keys, 8 initial positions: '
          f'{len(data[1])*128+42*8+64} animation bytes; 44 rank announcement tables')
