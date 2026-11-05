"""Static size of functions in SH4 ELFs: bytes and instructions (bytes minus the constant-pool words the function's
PC-relative loads read, halved; SH4 instructions are 16 bits). A function is matched by name, with its GCC clone
suffixes (.isra.N, .constprop.N, .part.N, .lto_priv.N) summed. Columns: one per ELF; the last row is the total.

usage: function_sizes.py --names NAME[,NAME...] ELF [ELF ...]   (or --hot HOT.tsv --group updater)
"""
import argparse
import collections
import os
import re
import shutil
import subprocess
from pathlib import Path

def sh_tool(name):
    """sh-elf-NAME: $KOS_NAME (as sample_profile_report.py), the KOS default path, then PATH."""
    for candidate in (os.environ.get('KOS_'+name.upper()), f'/opt/toolchains/dc/sh-elf/bin/sh-elf-{name}',
                      shutil.which(f'sh-elf-{name}')):
        if candidate and Path(candidate).exists():
            return candidate
    raise SystemExit(f'sh-elf-{name} not found; set KOS_{name.upper()}')
SUFFIX = re.compile(r'(\.(isra|constprop|part|cold|lto_priv)\.\d+)+$')


def functions(elf):
    out = subprocess.run([sh_tool('nm'), '-n', '-S', '--defined-only', str(elf)], capture_output=True, text=True, check=True).stdout
    funcs = collections.defaultdict(list)
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in 'tT':
            funcs[SUFFIX.sub('', p[3].removeprefix('_'))].append((int(p[0], 16), int(p[1], 16), p[3]))
    return funcs


def instructions(elf, start, size):
    out = subprocess.run([sh_tool('objdump'), '-d', '--no-show-raw-insn', f'--start-address={start:#x}',
                          f'--stop-address={start+size:#x}', str(elf)], capture_output=True, text=True, check=True).stdout
    pool = set()
    for m in re.finditer(r'\bmov\.(l|w)\s+([0-9a-f]+) <', out):
        address = int(m.group(2), 16)
        if start <= address < start+size:
            for b in range(4 if m.group(1) == 'l' else 2):
                pool.add(address+b)
    for m in re.finditer(r'\bmova\s+([0-9a-f]+) <', out):
        address = int(m.group(1), 16)
        if start <= address < start+size:
            pool.update(range(address, min(address+4, start+size)))
    return (size-len(pool))//2


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('elfs', nargs='+', type=Path)
    parser.add_argument('--names')
    parser.add_argument('--hot', type=Path)
    parser.add_argument('--group')
    args = parser.parse_args()
    if args.names:
        names = args.names.split(',')
    else:
        names = []
        for line in args.hot.read_text().splitlines()[1:]:
            g, n = line.split('\t')[:2]
            n = SUFFIX.sub('', n)
            if (not args.group or g == args.group) and n not in names:
                names.append(n)
    tables = [functions(e) for e in args.elfs]
    print(f'{"function":<44}' + ''.join(f'{e.parent.parent.name[:22]:>24}' for e in args.elfs))
    total = [[0, 0] for _ in args.elfs]
    for n in names:
        cells = []
        for i, (e, t) in enumerate(zip(args.elfs, tables)):
            rows = t.get(n, [])
            b = sum(r[1] for r in rows)
            ins = sum(instructions(e, r[0], r[1]) for r in rows if r[1])
            total[i][0] += b
            total[i][1] += ins
            cells.append(f'{b:>9} B {ins:>6} i' if rows else f'{"(inlined)":>17}')
        print(f'{n:<44}' + ''.join(f'{c:>24}' for c in cells))
    print(f'{"total":<44}' + ''.join(f'{f"{b} B {i} i":>24}' for b, i in total))


if __name__ == '__main__':
    main()
