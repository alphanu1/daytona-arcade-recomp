"""Fixed placement of the race's hot code and data (config/hot_layout.tsv; local.cfg.mk NATIVE_LAYOUT, default 1).

Why: under NATIVE_LTO=one GCC emits the whole program in one ltrans object, in an order that changes whenever any
unit changes, and the SH4's caches are direct-mapped (I-cache 8 KiB, O-cache 16 KiB, 32-byte lines, index = address
bits 12..5 and 13..5; KOS leaves CCR.IIX/OIX/ORA clear). Unrelated changes moved registration by 0.1-0.3 ms a frame
on hardware (17 October 2026: 33ac4dd 7.43 ms, the front end merged 7.68, the sfx fix 7.72; the same front-end code
linked with 33ac4dd's placement 7.46). The sfx fix kept 26 of 32 registration functions at 33ac4dd's relative
I-cache index and still lost 0.29 ms, so data placement counts as much as code: both caches are fixed here.
Hardware, 18 October (scripted fast race, registration): 78c1e0e 7.72 ms; this layout 7.45, the same with a 4.4 KB
function added elsewhere 7.44; the whole .text content 0x3500 higher (hot data block at residue 0x3600 instead of
0x100; the code shifted with it, so every I-cache relation stays, and .rodata/.data/.bss moved by 16 KiB, the heap
and the stack not at all) 7.34. That change moves only the block and the code's literal pools against the rest of
the data, so the gain is on the O-cache side; it is the default (data_residue). It refuted the first guess, the
registration's stack frames (residues about 14.0-16 KiB) against the hot statics: the block now sits there.

  .text   KOS startup (0x8c010000), the hot data block at a fixed offset and residue (data_residue; the space before
          it stays empty),
          the registration functions, enDjinn's and KOS's per-frame functions, the updater's functions, each at a
          fixed offset in a slot of fixed size (its size rounded to 32 bytes plus slack); then everything else.
  .bss    everything else, then padding so that the end of the image (KOS's heap start, mm.c sbrk_base) lands at
          heap_residue mod 16 KiB: the heap's objects keep their cache sets against the statics and the code.

The hot data block holds the registration's and updater's small hot statics (the renderer's stream and table
pointers, counters, the car streams' scratch); it lies inside .text (zero bytes for .bss variables in the image,
writable like any RAM on the SH4) because .text's start is the only address no other change can move. Large
statics (practice, the HUD records) cover the whole O-cache anyway and stay in .bss.

Matching under LTO: a text entry places input sections .text.NAME and .text.NAME.* (GCC's clones and LTO's
.lto_priv.N renumbering keep the base name); a data entry .bss/.data/.rodata.NAME[.*]; an entry may name another
pattern (library archive members). `check` verifies the link: every entry's symbol exists once (by base name), at
its slot, no larger than its slot; the data block and the heap at their residues. A missing entry (renamed,
inlined away) or a second match fails `make check` and is a loud warning at every link.

Slots: the size rounded up to 32 bytes plus 32 (SLACK), plus the config's `@pad NAME BYTES` where a build option
grows a function (NATIVE_DIAG_PMU inlines its counters into sq_submit_object; practice_update varies with the
front end): the NATIVE_SAMPLE_PROFILE and PMU builds then keep every other entry in place, so their profiles and
counters describe the default build's placement.

Choice of the set and order (`derive`): weights from a NATIVE_SAMPLE_PROFILE run (ms a frame per function and
samples per 32-byte line in the race phase, mapped by base name onto the current build). The registration order
minimises a direct-mapped miss estimate: per context (an inner loop: world stream emission OP/PT, world batch
preparation, car preparation, car emission OP/PT, shadows; and per object, preparation plus emission, with a line's
rate capped at the objects a frame) and per I-cache index, the independent-reference estimate sum r(1 - r/R) over
the lines sharing the index (r: samples of a line). Contexts are curated in the config (column contexts).

usage: hot_layout.py ld CONFIG -o ORDER.ld
       hot_layout.py check CONFIG ELF [--warn]
       hot_layout.py report CONFIG ELF... (cache indices and sizes of the hot set, side by side)
       hot_layout.py score CONFIG ELF... --profile SAMPLES --profile-elf ELF (the miss estimate of each ELF)
       hot_layout.py derive CONFIG --elf ELF --map MAP --profile SAMPLES --profile-elf ELF [-o NEW_CONFIG]
"""
import argparse
import bisect
import collections
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sample_profile_report as spr  # noqa: E402

TEXT_BASE = 0x8c010000
LINE, ILINES, OLINES = 32, 256, 512
SUFFIX = re.compile(r'(\.(isra|constprop|part|cold|lto_priv)\.\d+)+$')
GROUPS = ('data', 'reg', 'frame', 'upd')
SLACK = 32
# Objects a frame on each list (scripted fast race): rate cap of a line in the per-object contexts, in samples
# of the profile per frame (samples per execution: ~0.6 for a 32-byte line at the profile's 2 kHz).
OBJECT_CAP = {'w-obj': 10, 'c-obj': 17, 'w-objpt': 10, 'c-objpt': 17}


def base(name):
    return SUFFIX.sub('', name)


class Entry:
    def __init__(self, group, name, pattern='-', offset=None, slot=None, size=None, ms=0.0, contexts=''):
        self.group, self.name, self.pattern = group, name, pattern
        self.offset, self.slot, self.size, self.ms = offset, slot, size, ms
        self.contexts = [c for c in contexts.split(',') if c and c != '-']

    def patterns(self):
        if self.pattern != '-':
            return self.pattern
        n = self.name
        if self.group == 'data':
            return f'*(.bss.{n} .bss.{n}.* .data.{n} .data.{n}.* .rodata.{n} .rodata.{n}.*)'
        return f'*(.text.{n} .text.{n}.*)'

    def row(self):
        return '\t'.join([self.group, self.name, self.pattern, f'{self.offset:#07x}', f'{self.slot:#06x}',
                          str(self.size), f'{self.ms:.4f}', ','.join(self.contexts) or '-'])


def read_config(path):
    settings, entries, header = {}, [], []
    for line in Path(path).read_text().splitlines():
        if line.startswith('#') or not line.strip():
            if not entries:
                header.append(line)
            continue
        if line.startswith('@'):
            f = line[1:].split()
            if f[0] == 'pad':
                settings.setdefault('pad', {})[f[1]] = int(f[2], 0)
            else:
                settings[f[0]] = int(f[1], 0)
            continue
        f = line.split('\t')
        if f[0] == 'group':
            continue
        e = Entry(f[0], f[1], f[2] if len(f) > 2 else '-')
        if len(f) > 3 and f[3] != '-':
            e.offset, e.slot, e.size, e.ms = int(f[3], 16), int(f[4], 16), int(f[5]), float(f[6])
        e.contexts = [c for c in (f[7] if len(f) > 7 else '').split(',') if c and c != '-']
        entries.append(e)
    return settings, entries, header


def write_config(path, settings, entries, header):
    lines = list(header)
    lines += [f'@{k} {v:#x}' for k, v in settings.items() if k != 'pad']
    lines += [f'@pad {k} {v:#x}' for k, v in settings.get('pad', {}).items()]
    lines.append('group\tname\tpattern\toffset\tslot\tbytes\tms\tcontexts')
    lines += [e.row() for e in entries]
    Path(path).write_text('\n'.join(lines)+'\n')


def write_ld(settings, entries, out):
    placed = sorted((e for e in entries if e.offset is not None), key=lambda e: e.offset)
    end = max(e.offset+e.slot for e in placed)
    lines = ['/* Generated by tools/hot_layout.py from config/hot_layout.tsv (GNU ld --section-ordering-file; see',
             ' * that tool). Each listed input section at its fixed offset from the start of .text (0x8c010000). */',
             '.text : {', '  *_kos_startup.o(.text)']
    group = None
    for e in placed:
        if e.group != group:
            lines.append(f'  /* {e.group} */')
            group = e.group
        lines.append(f'  . = MAX(., {e.offset:#07x}); {e.patterns()}')
    lines.append(f'  . = MAX(., {end:#07x});')
    lines.append('}')
    if 'heap_residue' in settings:
        r = settings['heap_residue']
        lines += ['/* The rest of .bss, then the image end (the heap start) at heap_residue mod 16 KiB. */',
                  '.bss : {', '  *(.dynbss) *(.bss .bss.* .gnu.linkonce.b.*) *(COMMON)',
                  '  __nm_layout_bss_top = ABSOLUTE(.);',
                  f'  . += ({0x90000000+r:#x} - __nm_layout_bss_top) % 0x4000;', '}']
    Path(out).write_text('\n'.join(lines)+'\n')


def elf_symbols(elf):
    """name -> [(address, size, type)] for every defined symbol (readelf: FUNC/OBJECT/NOTYPE)."""
    out = subprocess.run([spr.tool('readelf', None), '-sW', str(elf)], capture_output=True, text=True,
                         check=True).stdout
    syms = collections.defaultdict(list)
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 8 and f[0].endswith(':') and f[6] != 'UND' and f[3] in ('FUNC', 'OBJECT', 'NOTYPE'):
            syms[f[7].removeprefix('_')].append((int(f[1], 16), int(f[2], 0), f[3]))
    return syms


def by_base(syms, kind):
    """base name -> [(full name, address, size)] of functions (kind text) or objects (kind data)."""
    want = 'FUNC' if kind == 'text' else 'OBJECT'
    out = collections.defaultdict(list)
    for name, rows in syms.items():
        if name.endswith('.cold') or '.cold.' in name:
            continue
        for a, s, t in rows:
            if t == want:
                out[base(name)].append((name, a, s))
    return out


def locate(e, syms, funcs, objs):
    """(address, size, problem) of an entry in an ELF."""
    if e.pattern != '-':
        rows = syms.get(e.name, [])
        if len(rows) != 1:
            return None, None, f'{len(rows)} symbols named {e.name}'
        return rows[0][0], rows[0][1], None
    rows = (objs if e.group == 'data' else funcs).get(e.name, [])
    if not rows:
        return None, None, 'missing (renamed or inlined away)'
    if len(rows) > 1:
        return rows[0][1], sum(r[2] for r in rows), 'matched by ' + ', '.join(r[0] for r in rows)
    return rows[0][1], rows[0][2], None


def check(settings, entries, elf, warn):
    syms = elf_symbols(elf)
    funcs, objs = by_base(syms, 'text'), by_base(syms, 'data')
    problems, ok = [], 0
    for e in entries:
        if e.offset is None:
            continue
        address, size, problem = locate(e, syms, funcs, objs)
        want = TEXT_BASE+e.offset
        if problem:
            problems.append(f'{e.group} {e.name}: {problem}')
        elif e.pattern != '-' and not want <= address < want+e.slot:
            problems.append(f'{e.group} {e.name}: at {address:#x}, slot {want:#x}+{e.slot:#x}')
        elif e.pattern == '-' and address != want:
            problems.append(f'{e.group} {e.name}: at {address:#x}, not at its slot {want:#x}')
        elif e.pattern == '-' and size > e.slot:
            problems.append(f'{e.group} {e.name}: {size} bytes, more than its slot {e.slot} (what follows moves)')
        else:
            ok += 1
    if 'heap_residue' in settings:
        end = syms.get('end', [(None,)])[0][0]
        if end is None or end % 16384 != settings['heap_residue']:
            problems.append(f'image end {end:#x}: not at heap residue {settings["heap_residue"]:#x} mod 16 KiB')
    placed = sum(1 for e in entries if e.offset is not None)
    if problems:
        bar = '!'*100
        print(f'{bar}\nLAYOUT: {len(problems)} of {placed} hot entries not placed as config/hot_layout.tsv lists '
              f'(tools/hot_layout.py; rerun its derive after a deliberate change of the hot code):', file=sys.stderr)
        for p in problems:
            print(f'LAYOUT: {p}', file=sys.stderr)
        print(bar, file=sys.stderr)
        return 0 if warn else 1
    print(f'PASS: hot layout, {ok} of {placed} entries at their slots'
          + (f', heap at {settings["heap_residue"]:#x} mod 16 KiB' if 'heap_residue' in settings else ''))
    return 0


def report(entries, elves):
    tables = []
    for elf in elves:
        syms = elf_symbols(elf)
        tables.append((syms, by_base(syms, 'text'), by_base(syms, 'data')))
    print(f'{"group":5s} {"entry":34s} ' + ' '.join(f'{"elf"+str(i):>17s}' for i in range(len(elves))))
    print(f'{"":5s} {"":34s} ' + ' '.join(f'{"index size":>17s}' for _ in elves)
          + '   (index: I-cache line for code, O-cache line for data; size in bytes)')
    same = 0
    for e in entries:
        cells = []
        for syms, funcs, objs in tables:
            address, size, problem = locate(e, syms, funcs, objs)
            if address is None:
                cells.append('-')
            else:
                index = (address//LINE) % (OLINES if e.group == 'data' else ILINES)
                cells.append(f'{index:3d}+{address % LINE:02d} {size:6d}' + ('*' if problem else ''))
        same += len(set(cells)) == 1 and cells[0] != '-'
        print(f'{e.group:5s} {e.name[:34]:34s} ' + ' '.join(f'{c:>17s}' for c in cells))
    print(f'{same} of {len(entries)} entries at the same cache index and size in every ELF')
    for syms, _, _ in tables:
        end = syms.get('end', [(0,)])[0][0]
        print(f'  image end {end:#x}: residue {end % 16384:#06x} mod 16 KiB')


# ---- the miss estimate ---------------------------------------------------------------------------------

def profile_lines(samples, elf, phase=2, section=3):
    """base name -> (size in the profiled ELF, {line: samples}), race phase, one frame section; and frames."""
    rate, _, words = spr.read_samples(Path(samples))
    table = spr.symbols(spr.tool('nm', None), elf)
    starts = [r[0] for r in table]
    start = {r[2]: (r[0], r[1]) for r in table}
    lines = collections.defaultdict(collections.Counter)
    weight = collections.Counter()
    count = 0
    for w in words:
        if w & 0x800000 or (w >> 26) & 7 != phase:
            continue
        count += 1
        a = spr.BASE+((w & 0x7fffff) << 1)
        s = spr.symbol_at(table, starts, a)
        sec = (w >> 24) & 3
        weight[(sec, base(s))] += 1
        if sec == section and s in start and start[s][1]:
            lines[s][(a-start[s][0])//LINE] += 1
    frames = count/rate*60
    heat = {}
    for s, c in lines.items():
        heat.setdefault(base(s), (start[s][1], c))
    ms = {k: v*1000.0/rate/frames for k, v in weight.items()}
    return heat, frames, ms


def contexts_of(entries):
    ctx = collections.defaultdict(set)
    for e in entries:
        for c in e.contexts:
            ctx[c].add(e.name)
    return ctx


def estimate(placement, heat, ctx):
    """placement: name -> (address, size). Estimated conflict misses (profile samples) per context."""
    mapped = {}
    for n, (a, z) in placement.items():
        if n not in heat:
            continue
        pz, L = heat[n]
        out = collections.Counter()
        for l, c in L.items():
            out[(a+(l*LINE if z == pz else int(l*LINE*z/pz)))//LINE] += c
        mapped[n] = out
    result = {}
    for c, members in ctx.items():
        cap = OBJECT_CAP.get(c)
        buckets = collections.defaultdict(collections.Counter)
        for n in members:
            for line, s in mapped.get(n, {}).items():
                buckets[line % ILINES][line] += s if cap is None else min(s, cap)
        total = 0.0
        for d in buckets.values():
            if len(d) > 1:
                r = sum(d.values())
                total += sum(x*(1-x/r) for x in d.values())
        result[c] = total
    return result


def slot_of(size):
    return (size+LINE-1)//LINE*LINE+SLACK


def map_sizes(path):
    """Input section sizes of library members in a link map: 'archive:member(section)' pattern -> size."""
    sizes, pending = {}, None
    for line in Path(path).read_text(errors='replace').splitlines():
        m = re.match(r'^ (\.text\S*)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$', line)
        if not m and pending:
            m2 = re.match(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$', line)
            if m2:
                m = (pending, m2.group(1), m2.group(2), m2.group(3))
        elif m:
            m = m.groups()
        pending = None
        if not m:
            p = re.match(r'^ (\.text\S*)$', line)
            pending = p.group(1) if p else None
            continue
        section, _, size, file = m
        a = re.match(r'^(.*/)?([^/(]+\.a)\(([^)]+)\)$', file.strip())
        if a and int(size, 16):
            sizes[f'*{a.group(2)}:{a.group(3)}({section})'] = int(size, 16)
    return sizes


def derive(args):
    settings, entries, header = read_config(args.config)
    syms = elf_symbols(args.elf)
    funcs, objs = by_base(syms, 'text'), by_base(syms, 'data')
    lib = map_sizes(args.map)
    heat, frames, ms = profile_lines(args.profile, args.profile_elf)
    keep = []
    for e in entries:
        if e.pattern != '-':
            e.size = lib.get(e.pattern)
            if e.size is None:
                print(f'derive: {e.name}: pattern {e.pattern} matches no input section; dropped', file=sys.stderr)
                continue
        else:
            rows = (objs if e.group == 'data' else funcs).get(e.name, [])
            if len(rows) != 1:
                print(f'derive: {e.name}: {len(rows)} symbols of that base name; dropped', file=sys.stderr)
                continue
            e.size = rows[0][2]
        section = {'reg': 3, 'upd': 1, 'frame': 0, 'data': None}[e.group]
        e.ms = sum(v for (s, n), v in ms.items() if n == e.name and (section is None or s == section)) \
            if e.group != 'frame' else sum(v for (s, n), v in ms.items() if n == e.name)
        keep.append(e)
    entries = keep
    ctx = contexts_of(entries)
    data = [e for e in entries if e.group == 'data']
    offset = settings['data_residue']
    for e in data:
        align = 32 if e.size >= 32 else 8 if e.size % 8 == 0 else 4
        offset = (offset+align-1)//align*align
        e.offset, e.slot = offset, e.size
        offset += e.size
    offset = (offset+LINE-1)//LINE*LINE

    pad = settings.get('pad', {})

    def slot(e):
        return slot_of(e.size)+pad.get(e.name, 0)

    def place(seq, start):
        out, at = {}, start
        for e in seq:
            out[e.name] = (TEXT_BASE+at, e.size)
            at += slot(e)
        return out, at
    reg = [e for e in entries if e.group == 'reg']

    def cost(seq):
        return sum(estimate(place(seq, offset)[0], heat, ctx).values())
    best = cost(reg)
    print(f'registration, listed order: estimate {best/frames:.1f} samples a frame')
    for _ in range(args.passes):
        improved = False
        for i in range(len(reg)):
            for j in range(len(reg)):
                if i == j:
                    continue
                trial = reg[:i]+reg[i+1:]
                trial.insert(j, reg[i])
                c = cost(trial)
                if c < best-1e-6:
                    reg, best, improved = trial, c, True
        if not improved:
            break
    print(f'registration, after the search: estimate {best/frames:.1f} samples a frame')
    at = offset
    order = data+reg+[e for e in entries if e.group == 'frame']+[e for e in entries if e.group == 'upd']
    for e in order:
        if e.group == 'data':
            continue
        e.offset, e.slot = at, slot(e)
        at += e.slot
    write_config(args.output or args.config, settings, order, header)
    print(f'{args.output or args.config}: {len(order)} entries, data block {data[0].offset:#x}..{offset:#x}, '
          f'hot code {offset:#x}..{at:#x}')


def score(args):
    _, entries, _ = read_config(args.config)
    heat, frames, _ = profile_lines(args.profile, args.profile_elf)
    ctx = contexts_of(entries)
    for elf in args.elves:
        syms = elf_symbols(elf)
        funcs = by_base(syms, 'text')
        placement = {n: (rows[0][1], rows[0][2]) for n, rows in funcs.items() if rows}
        r = estimate(placement, heat, ctx)
        print(f'{sum(r.values())/frames:8.1f}  ' + ' '.join(f'{k}:{v/frames:.1f}' for k, v in sorted(r.items()))
              + f'  {elf}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('ld')
    p.add_argument('config')
    p.add_argument('-o', '--output', required=True)
    p = sub.add_parser('check')
    p.add_argument('config')
    p.add_argument('elf')
    p.add_argument('--warn', action='store_true', help='report problems but exit 0 (the link-time check)')
    p = sub.add_parser('report')
    p.add_argument('config')
    p.add_argument('elves', nargs='+')
    p = sub.add_parser('score')
    p.add_argument('config')
    p.add_argument('elves', nargs='+')
    p.add_argument('--profile', required=True)
    p.add_argument('--profile-elf', required=True)
    p = sub.add_parser('derive')
    p.add_argument('config')
    p.add_argument('--elf', required=True)
    p.add_argument('--map', required=True)
    p.add_argument('--profile', required=True)
    p.add_argument('--profile-elf', required=True)
    p.add_argument('--passes', type=int, default=6)
    p.add_argument('-o', '--output')
    args = parser.parse_args()
    if args.command == 'ld':
        settings, entries, _ = read_config(args.config)
        write_ld(settings, entries, args.output)
    elif args.command == 'check':
        settings, entries, _ = read_config(args.config)
        sys.exit(check(settings, entries, args.elf, args.warn))
    elif args.command == 'report':
        _, entries, _ = read_config(args.config)
        report(entries, args.elves)
    elif args.command == 'score':
        score(args)
    else:
        derive(args)


if __name__ == '__main__':
    main()
