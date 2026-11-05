"""Static instruction-cache layout of the hot functions of an ELF (SH4: 8 KiB direct-mapped I-cache, 32-byte
lines, 256 lines; line index = address bits 12..5 with CCR.IIX clear, as KOS leaves it).

Input: an ELF and a hot list (tools/hot_order.py --list: group, function, ms/frame). Functions are matched by
name in the ELF (exact, else by the name without GCC clone suffixes).

Report per group and for the whole hot set: functions found, bytes, address span, 8 KiB windows touched (distinct
address>>13, i.e. how many times the set wraps around the cache), cache line indices covered (of 256), and
conflicts: line indices that two or more different hot functions of the same group map to, with the bytes
involved, plus the heaviest conflicting pairs (weight = the smaller ms/frame of the two, times shared lines).
Interleaving cost (the figure placement can change; tools/hot_order.py minimises the same): for each hot
caller/callee pair (references from the disassembly: bsr targets and function addresses in constant pools) the
line indices the two share, times the smaller ms/frame of the two; pairs of hot callees of one caller count half.
A callee called inside its caller's loop evicts the caller's lines it shares on every call.
Groups run at different times of the frame (updater, then registration), so conflicts are counted inside a group;
the whole-set line counts are given for reference.

usage: icache_layout.py ELF HOT.tsv [--pairs N] [--json OUT]
"""
import argparse
import collections
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

LINE, LINES = 32, 256
SUFFIX = re.compile(r'(\.(isra|constprop|part|cold|lto_priv)\.\d+)+$')


def sh_tool(name):
    """sh-elf-NAME: $KOS_NAME (as sample_profile_report.py), the KOS default path, then PATH."""
    for candidate in (os.environ.get('KOS_'+name.upper()), f'/opt/toolchains/dc/sh-elf/bin/sh-elf-{name}',
                      shutil.which(f'sh-elf-{name}')):
        if candidate and Path(candidate).exists():
            return candidate
    raise SystemExit(f'sh-elf-{name} not found; set KOS_{name.upper()}')


def text_symbols(elf):
    out = subprocess.run([sh_tool('nm'), '-n', '-S', '--defined-only', str(elf)],
                         capture_output=True, text=True, check=True).stdout
    funcs = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in 'tTwW':
            funcs.setdefault(parts[3].removeprefix('_'), (int(parts[0], 16), int(parts[1], 16)))
    return funcs


def lines_of(address, size):
    first, last = address//LINE, (address+max(size, 1)-1)//LINE
    return range(first, last+1)


def analyse(rows, funcs, refs):
    from hot_order import interleave_pairs
    found = [(g, n, ms, *funcs[n]) for g, n, ms in rows if n in funcs and funcs[n][1]]
    result = {}
    for group in sorted({r[0] for r in found})+['ALL']:
        members = [r for r in found if group == 'ALL' or r[0] == group]
        if not members:
            continue
        index_owners = collections.defaultdict(set)
        index_bytes = collections.Counter()
        for g, n, ms, address, size in members:
            for line in lines_of(address, size):
                index_owners[line % LINES].add(n)
                index_bytes[line % LINES] += LINE
        lo = min(r[3] for r in members)
        hi = max(r[3]+r[4] for r in members)
        windows = {a >> 13 for r in members for a in range(r[3] & ~8191, r[3]+r[4], 8192)}
        conflict = {i: o for i, o in index_owners.items() if len(o) > 1}
        pairs = collections.Counter()
        weights = {r[1]: r[2] for r in members}
        for i, owners in conflict.items():
            owners = sorted(owners)
            for a in range(len(owners)):
                for b in range(a+1, len(owners)):
                    pairs[(owners[a], owners[b])] += 1
        weighted = sum(min(weights[a], weights[b])*n for (a, b), n in pairs.items())
        group_of = {r[1]: r[0] for r in members}
        pair_w = interleave_pairs(sorted(weights), refs, weights, group_of)
        lines_by = {r[1]: {line % LINES for line in lines_of(r[3], r[4])} for r in members}
        inter = {}
        for (a, b), w in pair_w.items():
            shared = len(lines_by[a] & lines_by[b])
            if shared:
                inter[(a, b)] = (shared, w*shared)
        result[group] = {
            'functions': len(members), 'bytes': sum(r[4] for r in members), 'span': [hex(lo), hex(hi)],
            'span_bytes': hi-lo, 'windows_8k': len(windows), 'lines_covered': len(index_owners),
            'conflict_lines': len(conflict),
            'conflict_line_bytes': sum(index_bytes[i] for i in conflict),
            'weighted_conflict': round(weighted, 3),
            'interleaved_pairs': len(pair_w), 'interleaved_conflict_pairs': len(inter),
            'interleaved_shared_lines': sum(v[0] for v in inter.values()),
            'interleaving_cost': round(sum(v[1] for v in inter.values()), 3),
            'interleaved_top': [(a, b, v[0], round(v[1], 4)) for (a, b), v in
                                sorted(inter.items(), key=lambda kv: -kv[1][1])[:40]],
            'top_pairs': [(a, b, n, round(min(weights[a], weights[b])*n, 4)) for (a, b), n in
                          sorted(pairs.items(), key=lambda kv: -min(weights[kv[0][0]], weights[kv[0][1]])*kv[1])[:40]],
        }
    missing = [n for g, n, ms in rows if n not in funcs]
    return result, missing, found


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('elf', type=Path)
    parser.add_argument('hot', type=Path)
    parser.add_argument('--pairs', type=int, default=8)
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    funcs = text_symbols(args.elf)
    by_base = collections.defaultdict(list)
    for name in funcs:
        by_base[SUFFIX.sub('', name)].append(name)
    rows = []
    for line in args.hot.read_text().splitlines()[1:]:
        group, name, ms = line.split('\t')[:3]
        if name not in funcs and len(by_base.get(SUFFIX.sub('', name), [])) == 1:
            name = by_base[SUFFIX.sub('', name)][0]
        rows.append((group, name, float(ms)))
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from hot_order import references, interleave_pairs
    result, missing, found = analyse(rows, funcs, references(args.elf, funcs))
    print(f'{args.elf.name}: {len(found)} of {len(rows)} hot functions found'
          + (f'; missing: {", ".join(missing)}' if missing else ''))
    for group, r in result.items():
        print(f'  {group:<13} {r["functions"]:3d} functions {r["bytes"]:7d} bytes, span {r["span"][0]}..{r["span"][1]} '
              f'({r["span_bytes"]} bytes, {r["windows_8k"]} 8-KiB windows), lines {r["lines_covered"]}/256, '
              f'conflict lines {r["conflict_lines"]} ({r["conflict_line_bytes"]} bytes), weighted {r["weighted_conflict"]}')
        print(f'  {"":<13} interleaving: {r["interleaved_pairs"]} pairs, {r["interleaved_conflict_pairs"]} share lines, '
              f'{r["interleaved_shared_lines"]} shared lines, cost {r["interleaving_cost"]}')
        for a, b, n, w in r['interleaved_top'][:args.pairs]:
            print(f'      {n:3d} shared lines  {w:8.4f}  {a} / {b}')
    if args.json:
        args.json.write_text(json.dumps({'elf': str(args.elf), 'groups': result, 'missing': missing}, indent=1))


if __name__ == '__main__':
    main()
