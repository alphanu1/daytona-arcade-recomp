"""Read the FT lines of a NATIVE_DIAG_FRAME_TRACE console log (include/frame_trace.h).

Without options: the frames of the run whose updater + registration passed a vblank period in a row (streaks), the
late frames in them and, per streak, the sum over the period ("excess", what pushes registration's end later after
the flip), then one row per FT-W frame with the updater's scopes and registration's lists and groups (us).

usage: frame_trace_report.py LOG [--from RUN] [--to RUN] [--streaks N] [--no-window]
"""
import argparse
import re

KV = re.compile(r'(\w+)=(-?\d+)(?:/(\d+))?')


def parse(path):
    rows, window = [], []
    for line in open(path, errors='replace'):
        if line.startswith('FT-R '):
            f = line.split()[1:]
            if len(f) >= 9:
                rows.append(dict(zip(('run', 'upd', 'wait', 'reg', 'rnd', 'late', 'vbl', 'other', 'phase'), map(int, f))))
        elif line.startswith('FT-W '):
            w = {}
            for k, a, b in KV.findall(line):
                w[k] = int(a)
                if b:
                    w[k + '_n'] = int(b)
            m = re.search(r'ce=(\d+),(\d+),(\d+),(\d+)/(\d+),(\d+),(\d+),(\d+)', line)
            if m:
                w['ce'] = tuple(map(int, m.groups()))
            window.append(w)
    return rows, window


def streaks(rows, period=16683):
    """Runs of frames with updater + registration over the period; breaks after 3 frames under it."""
    out, cur, under = [], None, 0
    for r in rows:
        over = r['upd'] + r['reg'] > period
        if over:
            if cur is None:
                cur = {'first': r['run'], 'frames': 0, 'excess': 0, 'late': []}
            under = 0
        if cur is not None:
            cur['last'] = r['run'] if over else cur.get('last', r['run'])
            cur['frames'] += 1
            cur['excess'] += r['upd'] + r['reg'] - period
            cur['maxl'] = max(cur.get('maxl', -99999), r['late'] + r['rnd'])
            if r['vbl'] >= 2:
                cur['late'].append(r['run'])
            if not over:
                under += 1
                if under >= 3:
                    out.append(cur)
                    cur, under = None, 0
    if cur:
        out.append(cur)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--from', dest='lo', type=int, default=0)
    ap.add_argument('--to', dest='hi', type=int, default=1 << 30)
    ap.add_argument('--streaks', type=int, default=12)
    ap.add_argument('--no-window', action='store_true')
    a = ap.parse_args()
    rows, window = parse(a.log)
    racing = [r for r in rows if r['reg'] > 2000 and r['upd'] < 50000]
    print('%s: %d rows, %d window rows, %d late' % (a.log, len(rows), len(window), sum(r['vbl'] >= 2 for r in rows)))
    if racing:
        n = len(racing)
        print('mean upd %d wait %d reg %d rnd %d; upd+reg over the period %d frames, late+render over the period %d' % (
            sum(r['upd'] for r in racing)/n, sum(r['wait'] for r in racing)/n, sum(r['reg'] for r in racing)/n,
            sum(r['rnd'] for r in racing)/n, sum(r['upd'] + r['reg'] > 16683 for r in racing),
            sum(r['late'] + r['rnd'] > 16683 for r in racing)))
        near = sorted(racing, key=lambda r: -(r['late'] + r['rnd']))[:8]
        print('largest registration end + render (limit 16683 us):',
              ' '.join('%d@%d' % (r['late'] + r['rnd'], r['run']) for r in near))
    ss = sorted(streaks(racing), key=lambda s: -s['excess'])[:a.streaks]
    print('streaks (first..last, frames, excess us, largest reg end + render, late):')
    for s in sorted(ss, key=lambda s: s['first']):
        print('  %6d..%-6d %4d frames %7d us  %6d  %s' % (s['first'], s['last'], s['frames'], s['excess'], s['maxl'], s['late']))
    if a.no_window:
        return
    groups = ('wprep', 'wop', 'wpt', 'pprep', 'pop', 'ppt', 'oprep', 'oop', 'opt', 'tr', 'classify', 'marks', 'points', 'walk', 'wide',
              'general', 'sparts', 'splayer', 'sshadows', 'sscenery', 'sclocks', 'sframe', 'slight')
    print('   run  vbl   upd   sim setup  prep  wait   reg   cpu   rnd  late  L+P |   op   om   tr   pt |' +
          ''.join('%7s' % g[:6] for g in groups) + ' |  opp world parts  ptris  phdrs  pvtx    ta   qf')
    for w in window:
        if not a.lo <= w['run'] <= a.hi:
            continue
        ce = w.get('ce', (0,)*8)
        print('%6d %4d %5d %5d %5d %5d %5d %5d %5d %5d %5d %5d | %4d %4d %4d %4d |' % (
            w['run'], w['vbl'], w['upd'], w['sim'], w['setup'], w['prep'], w['wait'], w['reg'], w['cpu'], w['rnd'], w['late'],
            w['late'] + w['rnd'], w['l0'], w['l1'], w['l2'], w['l4']) +
            ''.join('%7d' % w.get(g, 0) for g in groups) +
            ' | %4d %5d %5d %6d %6d %5d %5d %4d' % (w['opp'], w['world'], w['parts'], w['tri4'], w['hdr4'], w['vtx4'],
                                                 (w['ta4'] - w['ta0'])//1024, ce[4] + ce[5]))


if __name__ == '__main__':
    main()
