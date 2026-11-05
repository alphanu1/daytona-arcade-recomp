"""Section ordering file that keeps the code and data placement of a reference build (GNU ld
--section-ordering-file; local.cfg.mk NATIVE_HOT_ORDER=1 NATIVE_HOT_ORDER_FILE=<this output>).

Why: under NATIVE_LTO=one GCC decides inlining and emission order for the whole program at once, so a
change anywhere (a few calls in code/audio/sound.c) reorders the renderer's functions and the data. The SH4's
caches are direct-mapped (I-cache 8 KiB, O-cache 16 KiB), so a function's misses depend on where it lands
relative to the others: the same code measured 7.54 and 7.78 ms of registration a frame on hardware by
placement alone (17 October 2026). An ordering file made from the reference build's link map (the same
source without the change) puts every input section the two builds share back in the reference's order.
Sections only the new build has go after them (ld places the listed ones first), so a function or variable
the change adds lands at the end of its output section and moves nothing before it.

What it cannot pin: an input section whose size differs (LTO inlined something else into it) shifts what
follows it; names that exist once in each build but are different functions (GCC's .lto_priv.N numbering) are
matched by name. tools/icache_layout.py and the --check option report what is left.

usage: pin_layout.py REFERENCE.map -o ORDER.ld [--new FIRST.map] [--extra OBJECT.o ...]
       (two passes: link once with the order file, then again with --new that link's map)
       pin_layout.py REFERENCE.map --check NEW.map     (input sections of .text/.rodata/.data/.bss that moved)
"""
import argparse
import collections
import re
from pathlib import Path

OUTPUTS = ('.text', '.rodata', '.data', '.bss')
LINE = re.compile(r'^ (\S+)(?:\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*))?$')
NEXT = re.compile(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$')


def input_sections(path):
    """Per output section: (input section name, file, address, size), in address order, size > 0."""
    lines = Path(path).read_text(errors='replace').splitlines()
    try:
        start = next(i for i, l in enumerate(lines) if l.startswith('Linker script and memory map'))
    except StopIteration:
        raise SystemExit(f'{path}: not a GNU ld map')
    out = collections.defaultdict(list)
    current = None
    i = start
    while i < len(lines):
        l = lines[i]
        if l and not l.startswith(' '):
            name = l.split()[0]
            current = name if name in OUTPUTS else None
        elif current and l.startswith(' ') and not l.startswith('  ') and not l.startswith(' *'):
            m = LINE.match(l)
            if m and not m.group(1).startswith('*'):
                name, address, size, file = m.group(1), m.group(2), m.group(3), m.group(4)
                if address is None and i + 1 < len(lines):
                    n = NEXT.match(lines[i + 1])
                    if n:
                        address, size, file = n.groups()
                        i += 1
                if address is not None and int(size, 16) and name != 'COMMON':
                    out[current].append((name, file.strip(), int(address, 16), int(size, 16)))
        i += 1
    return out


def file_pattern(file):
    """A pattern for the file of an input section that does not depend on temporary paths."""
    m = re.match(r'^(.*/)?([^/(]+\.a)\(([^)]+)\)$', file)
    if m:
        return f'*{m.group(2)}:{m.group(3)}'
    if file.endswith('.ltrans.o'):
        return '*.ltrans.o'
    return '*' + file.rsplit('/', 1)[-1]


def key(name, file):
    return (name, file_pattern(file))


STEM = re.compile(r'(\.(lto_priv|constprop|isra|part)\.\d+)+')


def correspondence(ref, new):
    """Reference input section -> the new build's, per output section: the same name and file, else the one
    section of the new build with the same name up to GCC's clone and .lto_priv numbering and the same size
    (LTO renumbers its private symbols from build to build)."""
    result = {}
    for output in OUTPUTS:
        rows = new.get(output, [])
        exact = {key(n, f): (n, f, size) for n, f, _, size in rows}
        by_stem = collections.defaultdict(list)
        for n, f, _, size in rows:
            by_stem[(STEM.sub('', n), file_pattern(f), size)].append((n, f, size))
        taken = set()
        for n, f, _, size in ref.get(output, []):
            k = key(n, f)
            if k in exact and exact[k][2] == size:
                result[(output, k)] = exact[k]; taken.add(key(*exact[k][:2]))
        for n, f, _, size in ref.get(output, []):
            k = key(n, f)
            if (output, k) in result:
                continue
            candidates = [c for c in by_stem[(STEM.sub('', n), file_pattern(f), size)] if key(*c[:2]) not in taken]
            if len(candidates) == 1:
                result[(output, k)] = candidates[0]; taken.add(key(*candidates[0][:2]))
            elif k in exact:
                result[(output, k)] = exact[k]  # Same name, other size.
    return result


def pattern_for(name, file, names):
    return f'*({name})' if names[name] == 1 else f'{file_pattern(file)}({name})'


def write_order(ref, out, extra, new=None):
    """The ordering file: the reference's input sections at their offsets. With the new build's map (a first
    link), each is written under the new build's name for it (correspondence); one it lacks leaves a gap, one
    it has larger is not pinned (it would push all that follows). Those and the new build's own .text sections
    (the --extra objects' sections of any kind among them) fill the gaps, largest first into the smallest gap
    with room (alignment slack included); what fits nowhere goes after the pinned ones."""
    match = correspondence(ref, new) if new is not None else None
    lines = ['/* Generated by tools/pin_layout.py from a reference build\'s link map: every input section of these',
             ' * output sections at its offset in the reference (see that file). */']
    pinned = filled = 0
    for output in OUTPUTS:
        rows = ref.get(output, [])
        if not rows:
            continue
        named = new.get(output, []) if new is not None else rows
        names = collections.Counter(n for n, _, _, _ in named)
        keys = collections.Counter(key(n, f) for n, f, _, _ in named)
        start = rows[0][2]
        entries = []  # (offset, pattern, size in the new build)
        used = set()
        pool = []
        for name, file, address, size in rows:
            new_size = size
            if match is not None:
                found = match.get((output, key(name, file)))
                if not found:
                    continue
                if found[2] > size:
                    pool.append(found)
                    used.add(key(found[0], found[1]))
                    continue
                name, file, new_size = found
            k = key(name, file)
            used.add(k)
            if keys[k] > 1:
                continue  # Not addressable on its own (two sections of one name in one file): left to the script.
            entries.append((address - start, pattern_for(name, file, names), new_size))
        fillers = collections.defaultdict(list)
        if new is not None and output == '.text':
            extra_files = tuple(extra)
            for o in OUTPUTS:
                for n, f, _, size in new.get(o, []):
                    if o == '.text' and key(n, f) not in used and keys[key(n, f)] == 1:
                        pool.append((n, f, size))
                    elif o != '.text' and f.endswith(extra_files):
                        pool.append((n, f, size))
            # Gaps: after each pinned section, up to the next one's offset.
            room = [entries[i + 1][0] - (entries[i][0] + entries[i][2]) for i in range(len(entries) - 1)] + [0]
            all_names = collections.Counter(n for o in OUTPUTS for n, _, _, _ in new.get(o, []))
            for n, f, size in sorted(pool, key=lambda r: -r[2]):
                need = (size + 31) // 32 * 32 + 32
                best = min((r, i) for i, r in enumerate(room) if r >= need) if any(r >= need for r in room) else None
                if best is None:
                    continue
                i = best[1]
                room[i] -= need
                fillers[i].append(pattern_for(n, f, all_names))
                filled += 1
        lines.append(f'{output} : {{')
        for i, (offset, pattern, _) in enumerate(entries):
            # Back to the reference offset after a section the new build lacks or has smaller.
            lines.append(f'  . = MAX(., {offset:#x}); {pattern}')
            lines.extend(f'    {f}' for f in fillers.get(i, []))
            pinned += 1
        if output == '.text':
            # The rest of the new build's own objects (code, constants, data, zeroed data), after .text: the
            # data output sections then hold the reference's sections only and move by one amount.
            for file in extra:
                lines.append(f'  *{file}(.text .text.* .rodata .rodata.* .data .data.* .bss .bss.*)')
        lines.append('}')
    Path(out).write_text('\n'.join(lines) + '\n')
    return pinned, filled


def check(ref, new):
    """Per output section: the reference's input sections at the same offset in the new build (matched as
    correspondence does), moved, resized, and the sections only one side has."""
    match = correspondence(ref, new)
    for output in OUTPUTS:
        rows = ref.get(output, []); other = new.get(output, [])
        if not rows or not other:
            continue
        where = {key(n, f): (addr, size) for n, f, addr, size in other}
        start_a, start_b = rows[0][2], min(r[2] for r in other)
        same = moved = resized = missing = 0
        first = None
        matched = set()
        for n, f, addr, size in rows:
            found = match.get((output, key(n, f)))
            if not found:
                missing += 1; continue
            matched.add(key(found[0], found[1]))
            y, t = where[key(found[0], found[1])]
            resized += t != size
            if addr - start_a == y - start_b:
                same += 1
            else:
                moved += 1
                first = first or (n, addr - start_a, y - start_b)
        extra = sum(1 for n, f, _, _ in other if key(n, f) not in matched)
        print(f'{output:8s} same offset {same}, moved {moved}, resized {resized}, only in reference {missing}, '
              f'only in new {extra}' + (f'; first moved: {first[0]} {first[1]:#x} -> {first[2]:#x}' if first else ''))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('reference_map')
    parser.add_argument('-o', '--output')
    parser.add_argument('--check')
    parser.add_argument('--new', help='link map of a first link of the new build: its larger sections are not pinned')
    parser.add_argument('--extra', action='append', default=[],
                        help='object file (basename) whose sections all go to the end of .text')
    args = parser.parse_args()
    ref = input_sections(args.reference_map)
    new = input_sections(args.new) if args.new else None
    if args.output:
        n, filled = write_order(ref, args.output, args.extra, new)
        print(f'{args.output}: {n} input sections pinned, {filled} placed in their gaps')
    if args.check:
        check(ref, input_sections(args.check))
