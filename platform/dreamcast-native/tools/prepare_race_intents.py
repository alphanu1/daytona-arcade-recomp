"""Export silent race-event tables from verified daytona93 program.bin.

Usage: prepare_race_intents.py PROGRAM.bin NEW_HEADER.h
ROM-derived output must remain ignored/local. No guest code or audio playback.
"""
import argparse
import hashlib
from pathlib import Path
import struct

from inspect_course import IMAGES, words

PREDICATES = (0, 0x193c4, 0x193ec, 0x19414, 0x19438, 0x1947c, 0x194c8, 0x19504, 0x19534)


def extract_announcements(program):
    directory = words(program, 0, 0x36260, 4)
    addresses = sorted(set(directory))
    tables = []
    for address in addresses:
        rows = []
        for i in range(128):
            threshold, signal, callback, reserved = words(program, 0x200000, address+16*i, 4)
            threshold = struct.unpack('<i', struct.pack('<I', threshold))[0]
            if callback not in PREDICATES or reserved:
                raise ValueError('invalid announcement row or unknown predicate')
            rows.append((threshold, signal, PREDICATES.index(callback)))
            if threshold == 100000:
                break
            if threshold > 32767:
                raise ValueError('unexpected unreachable announcement threshold')
        else:
            raise ValueError('missing announcement sentinel')
        tables.append(rows)
    return tables, [addresses.index(p) for p in directory], words(program, 0, 0x1956c, 9)


def extract(program):
    # CPU alias235ec0 maps to program offset35ec0; the table ends at236250.
    raw = struct.pack('<228I', *words(program, 0, 0x35ec0, 57*4))
    rows = [struct.unpack_from('<IIih', raw, 16*i) for i in range(57)]
    special = [(words(program, 0, 0x191c8+4*v, 1)[0],
                words(program, 0, 0x191d0+4*v, 1)[0]) for v in range(2)]
    alternate = [int(v != 0) for v in words(program, 0, 0x36250, 4)]
    return rows, special, alternate


def render(program):
    rows, special, alternate = extract(program)
    lines = ['/* Generated offline; numeric commands only, no device writes. */',
             '#include "race_intents.h"',
             'static const nm_race_intent_row_t nm_race_intent_rows[57] = {']
    for a, b, duration, trigger in rows:
        lines.append(f'    {{{{UINT32_C(0x{a:08x}),UINT32_C(0x{b:08x})}},{duration},{trigger},0}},')
    lines += ['};', 'static const uint32_t nm_race_intent_special[2][2] = {']
    for a, b in special:
        lines.append(f'    {{UINT32_C(0x{a:08x}),UINT32_C(0x{b:08x})}},')
    lines += ['};', 'static const uint8_t nm_race_intent_alternate[4] = {'+
              ','.join(map(str, alternate))+'};', '']
    tables, selectors, ranks = extract_announcements(program)
    flat = [row for table in tables for row in table]
    offsets = [sum(map(len, tables[:s])) for s in selectors]
    lines.append(f'static const nm_race_announcement_row_t nm_race_announcement_rows[{len(flat)}] = {{')
    for threshold, signal, predicate in flat:
        lines.append(f'    {{{threshold},UINT32_C(0x{signal:08x}),{predicate}}},')
    lines += ['};', 'static const uint16_t nm_race_announcement_offsets[4] = {'+
              ','.join(map(str, offsets))+'};',
              'static const uint16_t nm_race_announcement_counts[4] = {'+
              ','.join(str(len(tables[s])) for s in selectors)+'};',
              'static const uint32_t nm_race_announcement_ranks[9] = {'+
              ','.join(f'UINT32_C(0x{v:08x})' for v in ranks)+'};', '']
    return '\n'.join(lines)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('program', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    program = args.program.read_bytes()
    size, digest = IMAGES['program.bin']
    if len(program) != size or hashlib.sha256(program).hexdigest() != digest:
        raise ValueError('unrecognized daytona93 program image')
    header = render(program)
    with args.output.open('x') as out:
        out.write(header)
    tables, _, _ = extract_announcements(program)
    size = 932 + sum(map(len, tables))*12 + 16 + 36
    print(f'Consumers + {sum(map(len, tables))} creator rows: {size} constant bytes; source SHA256 {digest}')
