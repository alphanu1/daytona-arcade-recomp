"""Write the hot-function section ordering file for NATIVE_HOT_ORDER=1 (local.cfg.mk) and its hot list.

Weights: a NATIVE_SAMPLE_PROFILE run (sample file and the ELF that produced it; self time per linked function and
frame section, tools/sample_profile_report.py), mapped onto the functions of a current ELF by name (exact, else
by the name without GCC's clone suffixes .isra.N/.constprop.N/.part.N). Code that the profile predates is
weighted from the instruction model of the change that introduced it (--model NAME=INSTRUCTIONS, at --ns-per-insn
nanoseconds an instruction), and functions it made cold are scaled (--scale NAME=FACTOR).

Hot set: every function of at least --min-ms milliseconds a frame in the updater, registration or "other" frame
sections (the wait section is the idle thread), plus the --model functions and the --extra names (enDjinn's frame
loop, KOS's PVR and store-queue helpers). Each function goes to the group of the section where it weighs most:
frame (enDjinn and KOS per-frame code), registration (list callbacks: world and car stream paths, transforms,
emitters, assembly writers), updater (game update and render enqueue).

Order: groups frame, registration, updater; inside a group a depth-first walk of the call graph (a function's
references in the disassembly of the current ELF: bsr targets and constant-pool words naming a function), roots
by descending weight of what they reach, callees in the order of their first call site; so a caller is followed
by its hot callees and every inner loop with its helpers is contiguous.

Then, per group, a local search moves single functions to other positions of the group while that lowers the
interleaving cost: for every hot caller/callee pair (weight: the smaller ms/frame of the two) and every pair of
hot callees of one caller (half that weight), the I-cache line indices (address bits 12..5) the two share in the
packed layout. A caller and the helpers of its inner loop then sit in different lines whenever their sizes allow
(tools/icache_layout.py reports the same measure for any ELF).

Output: the ordering file (a GNU ld --section-ordering-file fragment: input sections of .text, matched by section
name, qualified by object file or archive member where the name is not unique in the link map), and a hot list
(TSV: group, function, ms/frame, bytes, section, pattern) that tools/icache_layout.py reads.

usage: hot_order.py --profile SAMPLES --profile-elf ELF --elf CURRENT_ELF --map CURRENT_MAP -o ORDER.ld --list HOT.tsv
"""
import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sample_profile_report as spr  # noqa: E402

SUFFIX = re.compile(r'(\.(isra|constprop|part|cold|lto_priv)\.\d+)+$')
GROUPS = ('frame', 'registration', 'updater')
SECTION_GROUP = {0: 'frame', 1: 'updater', 3: 'registration'}


def base(name):
    return SUFFIX.sub('', name)


def profile_weights(samples, elf, fps):
    rate, _, words = spr.read_samples(samples)
    table = spr.symbols(spr.tool('nm', None), elf)
    starts = [row[0] for row in table]
    counts = collections.Counter()
    for word in words:
        if word & 0x800000:
            continue
        address = spr.BASE+((word & 0x7fffff) << 1)
        section = (word >> 24) & 3
        if section == 2:
            continue  # wait: the idle thread
        counts[(section, spr.symbol_at(table, starts, address))] += 1
    frames = len(words)/rate*fps
    per_frame = 1000.0/rate/frames
    return {key: value*per_frame for key, value in counts.items()}, {name for _, _, name in table}


def text_symbols(elf):
    out = subprocess.run([spr.tool('nm', None), '-n', '-S', '--defined-only', str(elf)], capture_output=True,
                         text=True, check=True).stdout
    funcs = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in 'tTwW':
            funcs[parts[3].removeprefix('_')] = (int(parts[0], 16), int(parts[1], 16))
    return funcs


def map_sections(path):
    """Input sections of the .text output section: (name, address, size, file)."""
    lines = Path(path).read_text().splitlines()
    rows, inside, pending = [], False, None
    for line in lines:
        if line.startswith('.text '):
            inside = True
            continue
        if inside and line and not line.startswith(' '):
            break
        if not inside:
            continue
        m = re.match(r'^ (\.text\S*)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S.*)$', line)
        if m:
            rows.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
            pending = None
            continue
        m = re.match(r'^ (\.text\S*)$', line)
        if m:
            pending = m.group(1)
            continue
        m = re.match(r'^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S.*)$', line)
        if pending and m:
            rows.append((pending, int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
            pending = None
    return rows


def pattern(section, file, unique):
    """Unique function section names match with their clone/LTO suffixes too (.text.NAME .text.NAME.*), so the
    same file places the same functions when GCC renames them (LTO: .lto_priv.N; another clone)."""
    if unique:
        name = section.removeprefix('.text.')
        b = base(name)
        return f'*(.text.{b} .text.{b}.*)'
    m = re.match(r'^(.*/)?([^/(]+\.a)\(([^)]+)\)$', file)
    if m:
        return f'*{m.group(2)}:{m.group(3)}({section})'
    return f'*{Path(file).name}({section})'


def references(elf, funcs):
    """name -> referenced function names in first-reference order (bsr/bra targets, constant-pool words)."""
    out = subprocess.run([spr.tool('objdump', None), '-d', '--no-show-raw-insn', str(elf)], capture_output=True,
                         text=True, check=True).stdout
    refs, current = collections.defaultdict(list), None
    for line in out.splitlines():
        m = re.match(r'^[0-9a-f]+ <_?([^>]+)>:$', line)
        if m:
            current = m.group(1)
            continue
        if current is None:
            continue
        for target in re.findall(r'<_?([^>+]+)>', line):
            if target != current and target in funcs and target not in refs[current]:
                refs[current].append(target)
    return refs


def interleave_pairs(names, refs, weight, group):
    """(a, b) -> weight: hot caller/callee pairs (smaller ms/frame of the two) and pairs of hot callees of one
    caller (half that), inside one group."""
    pairs = collections.Counter()
    for caller in names:
        callees = []
        for c in refs.get(caller, []):
            if c in weight and c != caller and group[c] == group[caller] and c not in callees:
                callees.append(c)
        for c in callees:
            pairs[tuple(sorted((caller, c)))] += min(weight[caller], weight[c])
        for i in range(len(callees)):
            for j in range(i+1, len(callees)):
                pairs[tuple(sorted((callees[i], callees[j])))] += 0.5*min(weight[callees[i]], weight[callees[j]])
    return pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--profile-elf', type=Path, required=True)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--map', type=Path, required=True)
    parser.add_argument('-o', '--output', type=Path, required=True)
    parser.add_argument('--list', type=Path, required=True)
    parser.add_argument('--fps', type=float, default=60.0)
    parser.add_argument('--min-ms', type=float, default=0.008, help='ms a frame to be hot (default 0.008)')
    parser.add_argument('--model', action='append', default=[], help='GROUP:NAME=INSTRUCTIONS a frame')
    parser.add_argument('--ns-per-insn', type=float, default=7.0)
    parser.add_argument('--scale', action='append', default=[], help='NAME=FACTOR for the profile weight')
    parser.add_argument('--extra', action='append', default=[], help='GROUP:NAME always hot')
    parser.add_argument('--passes', type=int, default=200, help='local search passes (0: call order only)')
    args = parser.parse_args()

    weights, old_names = profile_weights(args.profile, args.profile_elf, args.fps)
    funcs = text_symbols(args.elf)
    by_base = collections.defaultdict(list)
    for name in funcs:
        by_base[base(name)].append(name)
    scale = {k: float(v) for k, v in (s.split('=') for s in args.scale)}
    weight = collections.defaultdict(lambda: collections.Counter())  # name -> group -> ms
    for (section, old), ms in weights.items():
        targets = [old] if old in funcs else by_base.get(base(old), [])
        if not targets:
            continue
        ms *= scale.get(base(old), 1.0)
        for t in targets:
            weight[t][SECTION_GROUP[section]] += ms/len(targets)
    for spec in args.model:
        group, _, rest = spec.partition(':')
        name, _, insns = rest.partition('=')
        for t in ([name] if name in funcs else by_base.get(base(name), [])):
            weight[t][group] += float(insns)*args.ns_per_insn/1e6/len(by_base.get(base(name), [name]) or [1])
    for spec in args.extra:
        group, _, name = spec.partition(':')
        for t in ([name] if name in funcs else by_base.get(base(name), [])):
            weight[t][group] += 0.0
            weight[t]['__extra__'] = 1
    hot = {}
    for name, groups in weight.items():
        extra = groups.pop('__extra__', 0)
        total = sum(groups.values())
        if total >= args.min_ms or extra:
            hot[name] = (max(GROUPS, key=lambda g: (groups[g], -GROUPS.index(g))), total)

    sections = map_sections(args.map)
    section_count = collections.Counter(base(s[0]) for s in sections)
    starts = sorted((s[1], s) for s in sections if s[2])
    keys = [a for a, _ in starts]

    def section_of(name):
        address = funcs[name][0]
        import bisect
        i = bisect.bisect_right(keys, address)-1
        if i < 0:
            return None
        s = starts[i][1]
        return s if s[1] <= address < s[1]+s[2] else None

    refs = references(args.elf, funcs)
    order, seen = [], set()
    for group in GROUPS:
        members = {n for n, (g, _) in hot.items() if g == group}
        reached = {}

        def reach(n, stack=()):
            if n in reached:
                return reached[n]
            total, todo, visited = 0.0, [n], set()
            while todo:
                x = todo.pop()
                if x in visited:
                    continue
                visited.add(x)
                total += hot[x][1]
                todo.extend(c for c in refs.get(x, []) if c in members)
            reached[n] = total
            return total
        called = {c for n in members for c in refs.get(n, []) if c in members and c != n}
        roots = sorted(members-called, key=lambda n: -reach(n)) + sorted(members & called, key=lambda n: -hot[n][1])

        def walk(n):
            if n in seen:
                return
            seen.add(n)
            order.append(n)
            for c in refs.get(n, []):
                if c in members:
                    walk(c)
        sys.setrecursionlimit(10000)
        for r in roots:
            walk(r)

    # Local search on the packed layout (4-byte alignment assumed; the start of .text after KOS's startup code).
    start = 0x8c010000+next(s[2] for s in sections if s[3].endswith('_kos_startup.o'))
    weight_of = {n: hot[n][1] for n in order}
    pairs = interleave_pairs(order, refs, weight_of, {n: hot[n][0] for n in order})
    full = (1 << 256)-1

    def mask(address, size):
        first, last = address//32, (address+max(size, 1)-1)//32
        if last-first+1 >= 256:
            return full
        run = (1 << (last-first+1))-1
        shift = first % 256
        return ((run << shift) | (run >> (256-shift))) & full

    def cost(seq, base_address):
        address, masks = base_address, {}
        for n in seq:
            address = (address+3) & ~3
            masks[n] = mask(address, funcs[n][1])
            address += funcs[n][1]
        return sum(w*bin(masks[a] & masks[b]).count('1') for (a, b), w in pairs.items() if a in masks and b in masks), address
    final, address = [], start
    for group in GROUPS:
        seq = [n for n in order if hot[n][0] == group]
        best, end = cost(seq, address)
        before = best
        for _ in range(args.passes):
            improved = False
            for i in range(len(seq)):
                for j in range(len(seq)):
                    if i == j:
                        continue
                    trial = seq[:i]+seq[i+1:]
                    trial.insert(j, seq[i])
                    c, _ = cost(trial, address)
                    if c < best-1e-9:
                        seq, best, improved = trial, c, True
                        break
            if not improved:
                break
        print(f'{group}: interleaving cost {before:.3f} in call order, {best:.3f} after the local search')
        final += seq
        address = end
    order = final

    lines = ['/* NATIVE_HOT_ORDER=1 section ordering file (GNU ld --section-ordering-file). Generated by',
             ' * tools/hot_order.py; see that file and local.cfg.mk. Input sections of the listed hot functions are placed',
             ' * at the start of .text in this order, after KOS\'s startup code (which stays at 0x8c010000). */',
             '.text : {', '  *_kos_startup.o(.text)']
    tsv = ['group\tfunction\tms_per_frame\tbytes\tsection\tpattern']
    emitted, group_now = set(), None
    for name in order:
        group, ms = hot[name]
        s = section_of(name)
        if s is None:
            print(f'warning: {name} lies in no input section of the map', file=sys.stderr)
            continue
        pat = pattern(s[0], s[3], section_count[base(s[0])] == 1 and s[0] != '.text')
        if group != group_now:
            lines.append(f'  /* {group} */')
            group_now = group
        if pat not in emitted:
            lines.append(f'  {pat}')
            emitted.add(pat)
        tsv.append(f'{group}\t{name}\t{ms:.4f}\t{funcs[name][1]}\t{s[0]}\t{pat}')
    lines.append('}')
    args.output.write_text('\n'.join(lines)+'\n')
    args.list.write_text('\n'.join(tsv)+'\n')
    for group in GROUPS:
        names = [n for n in order if hot[n][0] == group]
        print(f'{group}: {len(names)} functions, {sum(funcs[n][1] for n in names)} bytes, '
              f'{sum(hot[n][1] for n in names):.3f} ms a frame')


if __name__ == '__main__':
    main()
