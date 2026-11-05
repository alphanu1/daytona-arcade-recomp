#!/usr/bin/env python3
"""The native race's sound commands against the original's, for the frames the native race ran.

    compare_race_sound.py REFERENCE.sound NATIVE.sound [--require CLASSES | --all] [--max-lag FRAMES]

REFERENCE.sound: `m2segment ... --trace-sound` (the UART bytes the i960 sent, per frame; this
decodes them as NativeSoundSequencer::send does: f8 ignored, ff reset, running status, one data
byte for 0xC0/0xD0, two otherwise). NATIVE.sound: tests/trace_race_native.c --sound (one
command per line, the reference frame of the tick that queued it).

Messages are grouped by class (status and first byte: be17, the note 9e06, the sequence bank ae21) and each class is
compared as an ordered sequence over the frames the native race covers. For each class it prints
matched/native/reference counts and the frame lag (reference minus native: the original's UART
sends a queued word up to a frame later). --require: comma-separated classes that must match
exactly (same values in the same order, every lag within --max-lag, default 2); exits 1 if one
does not. Classes the native game does not produce yet are listed as such, not as failures.
"""
import argparse
import re
import sys
from collections import Counter, defaultdict


def reference_messages(path):
    status = 0
    data = []
    for line in open(path):
        if not line.startswith('S '):
            continue
        head, _, tail = line.partition(' : ')
        frame = int(re.search(r'frame=(\d+)', head).group(1))
        for token in tail.split():
            v = int(token, 16)
            if v == 0xf8:
                continue
            if v == 0xff:
                status, data = 0, []
                continue
            if v & 0x80:
                status, data = v, []
                continue
            if not status:
                continue
            data.append(v)
            if len(data) == (1 if (status & 0xe0) == 0xc0 else 2):
                yield frame, (status << 16) | (data[0] << 8) | (data[1] if len(data) > 1 else 0)
                data = []


def native_messages(path):
    for line in open(path):
        m = re.match(r'frame=(\d+) tick=\d+ cmd=([0-9a-f]+)', line)
        if m:
            yield int(m.group(1)), int(m.group(2), 16)


def klass(command):
    """Status and first byte: the bank of a sequence start, the controller, the note."""
    status, a = command >> 16, (command >> 8) & 0xff
    return f'{status:02x}{a:02x}'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('reference')
    ap.add_argument('native')
    ap.add_argument('--require', default='')
    ap.add_argument('--max-lag', type=int, default=2)
    ap.add_argument('--all', action='store_true', help='every class of the window must match (count, order, lag)')
    args = ap.parse_args()
    native = list(native_messages(args.native))
    if not native:
        sys.exit('no native commands')
    first, last = native[0][0], native[-1][0]
    reference = [(f, c) for f, c in reference_messages(args.reference) if first <= f <= last + args.max_lag]
    groups_n, groups_r = defaultdict(list), defaultdict(list)
    for f, c in native:
        groups_n[klass(c)].append((f, c))
    for f, c in reference:
        groups_r[klass(c)].append((f, c))
    required = [c for c in args.require.split(',') if c]
    if args.all:
        required = sorted(set(groups_n) | set(groups_r))
    failed = []
    print(f'frames {first}..{last}: native {len(native)} commands, reference {len(reference)}')
    for name in sorted(set(groups_n) | set(groups_r)):
        n, r = groups_n[name], groups_r[name]
        matched = 0
        lags = Counter()
        for (fn, cn), (fr, cr) in zip(n, r):
            if cn != cr:
                break
            matched += 1
            lags[fr - fn] += 1
        exact = matched == len(n) == len(r)
        lag_ok = all(abs(l) <= args.max_lag for l in lags)
        state = 'exact' if exact and lag_ok else 'not native yet' if not n else 'differs'
        lag_text = ' '.join(f'{l:+d}:{k}' for l, k in sorted(lags.items()))
        line = f'  {name:6s} native {len(n):4d} reference {len(r):4d} in order {matched:4d}  lag {lag_text or "-"}  {state}'
        if not exact and n and r and matched < min(len(n), len(r)):
            (fn, cn), (fr, cr) = n[matched], r[matched]
            line += f' (first difference: native {cn:06x} at {fn}, reference {cr:06x} at {fr})'
        print(line)
        if name in required and state != 'exact':
            failed.append(name)
    for name in required:
        if name not in groups_n and name not in groups_r:
            failed.append(name)
    if failed:
        print('FAIL: ' + ', '.join(failed))
        sys.exit(1)
    if required:
        print(('PASS: every command, ' + str(len(native)) + ', ' if args.all else 'PASS: ') + ', '.join(required) +
          ' as the original sends them')


if __name__ == '__main__':
    main()
