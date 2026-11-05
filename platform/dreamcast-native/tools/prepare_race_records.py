"""Expand original record defaults offline; generated ROM data stays local.

Usage: prepare_race_records.py PROGRAM.bin NEW_HEADER.h
"""
import argparse
import hashlib
from pathlib import Path
import struct

from inspect_course import IMAGES, words


def extract(program):
    size, digest = IMAGES['program.bin']
    if len(program) != size or hashlib.sha256(program).hexdigest() != digest:
        raise ValueError('unrecognized daytona93 program image')
    directory = words(program, 0x200000, 0x2266e0, 6)
    if directory[1::2] != (0x1d00360, 0x1d005a0, 0x1d007e0):
        raise ValueError('unexpected record owners')
    bases = words(program, 0x200000, 0x2266d0, 3)
    tables = []
    for course, source in enumerate(directory[::2]):
        table = []
        for i in range(24):
            # The source row is unaligned and rank is loaded as ONE byte.
            offset = source - 0x200000 + 12*i
            rank = program[offset]
            name, flags = struct.unpack_from('<II', program, offset+4)
            if rank != i+1:
                raise ValueError('unexpected default rank ordering')
            time = (bases[course] + (rank-1)*320) & 0xffffffff
            table.append((rank, time, flags, name))
        tables.append(table)
    return tables


def render(tables):
    lines = ['/* Generated offline from original2265d0; view is preserved on reset. */',
             '#include "race_records.h"',
             'static const nm_race_record_seed_t nm_race_record_seeds[3][24] = {']
    for table in tables:
        lines.append('    {')
        for row in table:
            lines.append('        {'+','.join(f'UINT32_C(0x{v:08x})' for v in row)+'},')
        lines.append('    },')
    return '\n'.join(lines+['};', ''])


def extract_names(program):
    extract(program)  # Same fingerprint and source-owner gate.
    names = program[0x3bda8:0x3be20]
    if len(names) != 120 or program[0x3be20:0x3be24] != b'\xff'*4:
        raise ValueError('name-rule sentinel changed')
    timers = words(program, 0x200000, 0x23bd04, 40)
    commands = words(program, 0x200000, 0x23be24, 41)
    rules = [(int.from_bytes(names[3*i:3*i+3], 'little'), timers[i], commands[i]) for i in range(40)]
    replacements = []
    for pointer in words(program, 0x200000, 0x23bec8, 2):
        at = pointer-0x200000
        if at < 0 or at+12 > len(program):
            raise ValueError('name replacement outside program')
        replacements.append([int.from_bytes(program[at+3*i:at+3*i+3], 'little') for i in range(4)])
    if len({r[0] for r in rules}) != 40 or any(c > 0xffffff for c in commands):
        raise ValueError('ambiguous name rules or invalid sound command')
    return rules, replacements, commands[-1]


def render_names(data):
    rules, replacements, default = data
    lines = ['/* Offline227b1c/227934 rules; no runtime ROM lookup. */',
             '#include <stdalign.h>', '#include "name_entry.h"',
             'alignas(32) static const nm_name_rules_t nm_name_rules = {', '    .rules={']
    lines += ['        {'+','.join(f'0x{v:08x}u' for v in row)+'},' for row in rules]
    lines += ['    }, .replacements={']
    lines += ['        {'+','.join(f'0x{v:08x}u' for v in row)+'},' for row in replacements]
    return '\n'.join(lines+['    },', f'    .default_command=0x{default:08x}u', '};', ''])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('program', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--name-output', type=Path, help='also expand name completion rules into this new header')
    args = parser.parse_args()
    program = args.program.read_bytes()
    tables = extract(program)
    names = render_names(extract_names(program)) if args.name_output else None
    if args.output.exists() or (args.name_output and (args.name_output.exists() or args.name_output==args.output)):
        raise ValueError('output must be a new, distinct path')
    with args.output.open('x') as out:
        out.write(render(tables))
    print('Exported 3x24 record seeds: 1152 constant bytes; no runtime ROM/math')
    if args.name_output:
        with args.name_output.open('x') as out:
            out.write(names)
        print('Exported40 name rules/8 replacement triples/default command:516 constant bytes')
