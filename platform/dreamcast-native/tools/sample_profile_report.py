"""Report for a NATIVE_SAMPLE_PROFILE run (include/sample_profile.h).

Input: sample-profile.bin written by the console, or a console log containing
the SAMPLE-PROFILE-TEXT fallback lines, plus the ELF that produced it.
Self time per linker symbol comes from the symbol table; inlined routines and
source lines come from addr2line -i, so static inline code in headers is
reported under its own name instead of the function it was inlined into.
Times are estimates: samples / rate, per frame at --fps over the sampled time.
"""
import argparse
import bisect
import collections
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

SECTIONS = ('other', 'updater', 'wait', 'registration')
PHASES = ('other', 'rolling start', 'race', 'finish', 'podium', 'results', 'attract', 'phase7')
BASE = 0x8c000000


def read_samples(path):
    data = path.read_bytes()
    if data[:4] == b'NMSP':
        magic, version, rate, count, dropped = struct.unpack_from('<5I', data)
        if version != 1 or len(data) != 20+4*count:
            raise SystemExit('sample file is truncated or has another version')
        return rate, dropped, list(struct.unpack_from(f'<{count}I', data, 20))
    text = data.decode('latin-1')
    header = re.search(r'SAMPLE-PROFILE-TEXT header (\d+) (\d+) (\d+)', text)
    if not header:
        raise SystemExit('neither a sample file nor a log with SAMPLE-PROFILE-TEXT lines')
    words = [int(w, 16) for line in re.findall(r'^SP((?: [0-9a-f]{8})+)\s*$', text, re.M) for w in line.split()]
    if len(words) != int(header.group(2)):
        raise SystemExit(f'log has {len(words)} samples, header says {header.group(2)}')
    return int(header.group(1)), int(header.group(3)), words


def tool(name, override):
    for candidate in (override, os.environ.get('KOS_'+name.upper()), f'/opt/toolchains/dc/sh-elf/bin/sh-elf-{name}',
                      shutil.which(f'sh-elf-{name}')):
        if candidate and Path(candidate).exists():
            return candidate
    raise SystemExit(f'sh-elf-{name} not found; pass --{name}')


def symbols(nm, elf):
    out = subprocess.run([nm, '-n', '-S', '--defined-only', str(elf)], capture_output=True, text=True, check=True).stdout
    table = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in 'tTwW':
            table.append((int(parts[0], 16), int(parts[1], 16), parts[3].removeprefix('_')))
        elif len(parts) == 3 and parts[1] in 'tTwW':
            table.append((int(parts[0], 16), 0, parts[2].removeprefix('_')))
    table.sort()
    return table


def symbol_at(table, starts, address):
    i = bisect.bisect_right(starts, address)-1
    if i < 0:
        return '[outside the image]'
    start, size, name = table[i]
    if size and address >= start+size:
        # Sizeless assembly labels and padding: attribute to the preceding symbol.
        return name if address < start+size+64 else f'[after {name}]'
    return name


def inline_frames(addr2line, elf, addresses):
    """address -> (innermost function, file, line, outer chain)."""
    result = {}
    addresses = sorted(addresses)
    for at in range(0, len(addresses), 4000):
        chunk = addresses[at:at+4000]
        out = subprocess.run([addr2line, '-e', str(elf), '-f', '-i', '-a', *[f'{a:#x}' for a in chunk]],
                             capture_output=True, text=True, check=True).stdout.splitlines()
        current, frames = None, []

        def flush():
            if current is not None:
                function, where = frames[0] if frames else ('??', '??:0')
                file, _, line = where.rpartition(':')
                line = line.split()[0] if line else '0'
                result[current] = (function, os.path.basename(file) or '??', int(line) if line.isdigit() else 0,
                                   tuple(f for f, _ in frames[1:]))
        i = 0
        while i < len(out):
            if out[i].startswith('0x'):
                flush()
                current, frames = int(out[i], 16), []
                i += 1
            else:
                frames.append((out[i], out[i+1] if i+1 < len(out) else '??:0'))
                i += 2
        flush()
    return result


def table_lines(counter, total, scale, limit, label):
    lines = [f'    {"samples":>8} {"share":>7} {"ms/frame":>9}  {label}']
    for key, count in counter.most_common(limit):
        lines.append(f'    {count:8d} {100*count/max(total, 1):6.2f}% {count*scale:9.3f}  {key}')
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('samples', type=Path)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--fps', type=float, default=60.0, help='frames per second of the run (default 60)')
    parser.add_argument('--top', type=int, default=25)
    parser.add_argument('--addr2line')
    parser.add_argument('--nm')
    args = parser.parse_args()
    rate, dropped, words = read_samples(args.samples)
    if not words:
        raise SystemExit('no samples')
    table = symbols(tool('nm', args.nm), args.elf)
    starts = [row[0] for row in table]
    decoded = []
    for word in words:
        outside = word & 0x800000
        address = None if outside else BASE+((word & 0x7fffff) << 1)
        decoded.append((address, (word >> 24) & 3, (word >> 26) & 7))
    inline = inline_frames(tool('addr2line', args.addr2line), args.elf, {a for a, _, _ in decoded if a is not None})
    seconds = len(words)/rate
    frames = seconds*args.fps
    per_frame = 1000.0/rate/frames  # samples -> ms per frame over the whole run
    print(f'{len(words)} samples at {rate} Hz = {seconds:.1f} s sampled ({frames:.0f} frames at {args.fps:g} fps); '
          f'{dropped} dropped after the buffer filled')
    print('ms/frame = samples / rate / frames of the scope named in each heading. Sampling excludes time with interrupts masked.\n')

    by_section = collections.Counter(section for _, section, _ in decoded)
    print('== Frame sections (whole run) ==')
    for index, name in enumerate(SECTIONS):
        print(f'    {name:<13} {by_section[index]:8d} {100*by_section[index]/len(words):6.2f}% {by_section[index]*per_frame:8.3f} ms/frame')
    print()

    def describe(address):
        if address is None:
            return '[outside main RAM]', '[outside main RAM]', '[outside main RAM]'
        function, file, line, _ = inline.get(address, ('??', '??', 0, ()))
        symbol = symbol_at(table, starts, address)
        routine = function if function != '??' else symbol
        return symbol, f'{routine}  ({file})', (routine, file, line)

    for index, name in enumerate(SECTIONS):
        rows = [a for a, section, _ in decoded if section == index]
        if not rows:
            continue
        selfs, routines, lines = collections.Counter(), collections.Counter(), collections.defaultdict(collections.Counter)
        for address in rows:
            symbol, routine, where = describe(address)
            selfs[symbol] += 1
            routines[routine] += 1
            if isinstance(where, tuple):
                lines[(where[0], where[1])][where[2]] += 1
        print(f'== Section {name}: {len(rows)} samples, {len(rows)*per_frame:.3f} ms/frame ==')
        print('  Self time by linked function (inlined code counted in its container):')
        print('\n'.join(table_lines(selfs, len(rows), per_frame, args.top, 'function')))
        print('  By inlined routine (innermost source function):')
        print('\n'.join(table_lines(routines, len(rows), per_frame, args.top, 'routine  (file)')))
        print('  Hottest source line ranges (per routine, lines holding 80% of its samples):')
        for (routine, file), counts in sorted(lines.items(), key=lambda item: -sum(item[1].values()))[:args.top]:
            total = sum(counts.values())
            kept, acc = [], 0
            for line, count in counts.most_common():
                kept.append(line)
                acc += count
                if acc >= 0.8*total:
                    break
            hottest = ', '.join(f'{line}:{count}' for line, count in counts.most_common(3))
            print(f'    {total:8d} {100*total/len(rows):6.2f}%  {file}:{min(kept)}-{max(kept)}  {routine}  [top lines {hottest}]')
        print()

    print('== Scene phases ==')
    for index, name in enumerate(PHASES):
        rows = [(a, section) for a, section, phase in decoded if phase == index]
        if not rows:
            continue
        phase_frames = len(rows)/rate*args.fps
        scale = 1000.0/rate/phase_frames
        parts = collections.Counter(section for _, section in rows)
        split = ', '.join(f'{SECTIONS[s]} {parts[s]*scale:.2f}' for s in range(4))
        print(f'  {name}: {len(rows)} samples = {len(rows)/rate:.1f} s; ms/frame: {split}')
        routines = collections.Counter(describe(a)[1] for a, section in rows if section in (1, 3))
        for key, count in routines.most_common(8):
            print(f'      {count:8d} {count*scale:7.3f} ms/frame  {key}')
    print()

    print('== Top 30 inlined routines overall (updater and registration only) ==')
    work = [a for a, section, _ in decoded if section in (1, 3)]
    routines = collections.Counter(describe(a)[1] for a in work)
    containers = collections.defaultdict(collections.Counter)
    for address in work:
        symbol, routine, _ = describe(address)
        containers[routine][symbol] += 1
    print(f'    {"samples":>8} {"share":>7} {"ms/frame":>9}  routine  (file)  <- mostly inside')
    for key, count in routines.most_common(30):
        inside = containers[key].most_common(1)[0][0]
        print(f'    {count:8d} {100*count/max(len(work), 1):6.2f}% {count*per_frame:9.3f}  {key}  <- {inside}')


if __name__ == '__main__':
    main()
