"""Summarise the dropped-frame log (NATIVE_DIAG_DROP_LOG=1) and the PMU lines (NATIVE_DIAG_PMU=1) of console logs.

Where frames were dropped (phases, scenes, 500-tick ranges of the run's tick counter, worst records) and why
(cause counts; for the records: updater time, simulation ticks, registration, PVR render time, wait). With
several logs, one column each (A/B comparisons, e.g. control against NATIVE_HOT_ORDER=1).

Line formats (code/practice.c; version 1 lines are read too):
  DROP-LOG v2: ... period_ns=N (measured over N vblanks); records kept=N of N
  DROP seq= run= tick= phase= scene= vbl= cause= upd= ticks= wait= reg= rnd= late= other= prev_rnd= camera=M/T [img=KIND]
       [untimed=1]   (us)
  DROP-PHASE phase= frames= late= missed= avg_upd= avg_reg= avg_rnd= <cause>=N ...
  DROP-TICKS run=LO-[end] frames= late= [last=HI] <cause>=N ...
  DROP-SUMMARY frames= late= late_permille= missed_vblanks= <cause>=N ... unmatched= run_ticks= flips= displayed=
       other_mode= unflipped= overrun= vblanks= <anomaly>=N ...   (v2: every flip is accounted for; "unmatched" frames
       are displayed frames whose registration/render events are incomplete, counted all the same)
  LATE-CAPTURE v1: slots= ... (NATIVE_DIAG_LATE_CAPTURE: pictures kept, copies and their time)
  PMU phase= section= n=a/b/c imiss= omiss= istall= dstall= insns= cycles=   (per-frame means)

usage: drop_log_report.py LOG [LOG ...] [--records N] [--json OUT]
"""
import argparse
import collections
import json
import re
from pathlib import Path

CAUSES = ('pvr', 'cpu-update', 'cpu-registration', 'cpu-other', 'load', 'other', 'capture')
ANOMALIES = ('regdone_repeat', 'regdone_stray', 'rndstart_early', 'rndstart_stray', 'rnddone_stray', 'flip_stray')
KV = re.compile(r'(\w[\w-]*)=(-?[\w.-]+)')


def values(line):
    out = {}
    for k, v in KV.findall(line):
        try:
            out[k] = int(v)
        except ValueError:
            out[k] = v
    return out


def parse(path):
    text = Path(path).read_text(errors='replace')
    log = {'path': str(path), 'records': [], 'phases': [], 'ticks': [], 'summary': None, 'header': None, 'pmu': [],
           'render': [l for l in text.splitlines() if l.startswith('DIAG PVR render')],
           'checkpoints': re.findall(r'Native checkpoints: .*', text),
           'displayed': [values(l) for l in text.splitlines() if l.startswith('Native displayed frames')],
           'capture': [values(l) for l in text.splitlines() if l.startswith('LATE-CAPTURE v1')]}
    for line in text.splitlines():
        line = line.strip()
        if line.startswith('DROP-LOG v'):
            log['header'] = values(line)
        elif line.startswith('DROP seq='):
            log['records'].append(values(line))
        elif line.startswith('DROP-PHASE '):
            log['phases'].append(values(line))
        elif line.startswith('DROP-TICKS '):
            m = re.match(r'DROP-TICKS run=(\d+)-(\w*)', line)
            row = values(line)
            row['lo'] = int(m.group(1))
            row['hi'] = row.get('last', None)
            log['ticks'].append(row)
        elif line.startswith('DROP-SUMMARY '):
            log['summary'] = values(line)
        elif line.startswith('PMU phase='):
            row = values(line)
            row['n'] = [int(x) for x in re.search(r'n=(\d+)/(\d+)/(\d+)', line).groups()]
            log['pmu'].append(row)
    return log


def mean(xs):
    return sum(xs)/len(xs) if xs else 0.0


def report(log, records_shown):
    out = []
    s = log['summary']
    out.append(f'== {log["path"]}')
    if not s:
        out.append('   no DROP-SUMMARY line (not a NATIVE_DIAG_DROP_LOG=1 run, or the run did not end)')
    else:
        frames, late = s.get('frames', 0), s.get('late', 0)
        out.append(f'   frames {frames}, late {late} ({100*late/max(frames, 1):.2f}%), missed vblanks {s.get("missed_vblanks", 0)}, '
                   f'run ticks {s.get("run_ticks", 0)}, unmatched scenes {s.get("unmatched", 0)}, '
                   f'vblank period {log["header"].get("period_ns", 0)/1e6 if log["header"] else 0:.4f} ms')
        out.append('   why (cause of each late frame): ' + ', '.join(f'{c} {s.get(c, 0)}' for c in CAUSES if s.get(c, 0)))
        if 'flips' in s:
            line = (f'   bookkeeping: {s["flips"]} flips, {s["displayed"]} displayed frames ({s.get("other_mode", 0)} of other modes), '
                    f'{s.get("unflipped", 0)} scenes never flipped, {s.get("overrun", 0)} lost to ring overrun; anomalies: ' +
                    (', '.join(f'{a} {s[a]}' for a in ANOMALIES if s.get(a)) or 'none'))
            if log['displayed']:
                d = log['displayed'][-1]
                line += f'; "Native displayed frames after64": flips {d.get("flips")} over {d.get("vblanks")} vblanks'
            out.append(line)
    for c in log['capture']:
        out.append(f'   late capture: {c.get("images")} pictures ({c.get("before")} before) in {c.get("slots")} slots, late {c.get("late")}, '
                   f'eligible {c.get("eligible")}, not kept {c.get("not_kept")}, missed {c.get("missed")}; {c.get("copies")} copies, '
                   f'mean {c.get("copy_us_mean")} us, max {c.get("copy_us_max")} us; {c.get("affected")} late frames caused by copies')
    if log['phases']:
        out.append('   where, by phase:     frames    late   missed  avg upd   avg reg   avg rnd (us)  causes')
        for p in log['phases']:
            causes = ', '.join(f'{c} {p.get(c, 0)}' for c in CAUSES if p.get(c, 0))
            out.append(f'     {p["phase"]:<10} {p["frames"]:10d} {p["late"]:7d} {p["missed"]:8d} {p["avg_upd"]:8d} '
                       f'{p["avg_reg"]:9d} {p["avg_rnd"]:9d}       {causes}')
    if log['ticks']:
        worst = sorted(log['ticks'], key=lambda r: -r['late'])
        out.append('   where, 500-tick ranges of the run (late frames; worst 12): ' + '; '.join(
            f'{r["lo"]}-{r["hi"] if r["hi"] is not None else "end"}: {r["late"]}/{r["frames"]} '
            f'({max(CAUSES, key=lambda c: r.get(c, 0))})' for r in worst[:12] if r['late']))
    recs = log['records']
    if recs:
        out.append(f'   records ({len(recs)} kept of {s.get("late", len(recs)) if s else len(recs)}): mean per cause '
                   '(updater us, ticks, wait us, registration us, render us, registration end after the previous flip us)')
        by = collections.defaultdict(list)
        for r in recs:
            by[r['cause']].append(r)
        for c in CAUSES:
            rs = by.get(c)
            if not rs:
                continue
            two = sum(1 for r in rs if r['ticks'] >= 2)
            out.append(f'     {c:<17} {len(rs):4d}: upd {mean([r["upd"] for r in rs]):7.0f} ticks {mean([r["ticks"] for r in rs]):4.2f} '
                       f'({two} with 2+) wait {mean([r["wait"] for r in rs]):6.0f} reg {mean([r["reg"] for r in rs]):7.0f} '
                       f'rnd {mean([r["rnd"] for r in rs]):7.0f} late {mean([r["late"] for r in rs]):7.0f}')
        scenes = collections.Counter((r['scene'], r['phase']) for r in recs)
        out.append('   records by scene/phase: ' + ', '.join(f'{a}/{b} {n}' for (a, b), n in scenes.most_common()))
        if records_shown:
            out.append(f'   worst {records_shown} records by missed vblanks, then render+registration time:')
            for r in sorted(recs, key=lambda r: (-r['vbl'], -(r['rnd']+r['reg'])))[:records_shown]:
                out.append(f'     run {r["run"]:6d} tick {r["tick"]:6d} {r["phase"]:<8} {r["scene"]:<7} vbl {r["vbl"]} {r["cause"]:<16} '
                           f'upd {r["upd"]:6d} x{r["ticks"]} wait {r["wait"]:5d} reg {r["reg"]:6d} rnd {r["rnd"]:6d} late {r["late"]:6d}')
    if log['pmu']:
        out.append('   PMU per frame (event pairs rotate per frame; n = frames per pair):')
        out.append('     phase      section        imiss    omiss   istall%  dstall%   insns     cycles  imiss/1k insns')
        for p in log['pmu']:
            cyc = p.get('cycles', 0) or 1
            ins = p.get('insns', 0) or 1
            out.append(f'     {p["phase"]:<10} {p["section"]:<13} {p.get("imiss", 0):8d} {p.get("omiss", 0):8d} '
                       f'{100*p.get("istall", 0)/cyc:8.1f} {100*p.get("dstall", 0)/cyc:8.1f} {p.get("insns", 0):8d} '
                       f'{p.get("cycles", 0):10d} {1000*p.get("imiss", 0)/ins:10.2f}')
    if log['render']:
        out.extend('   '+l for l in log['render'] if 'histogram' not in l)
    if log['checkpoints']:
        out.append('   '+log['checkpoints'][-1])
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--records', type=int, default=10, help='worst records to list per log')
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    logs = [parse(p) for p in args.logs]
    for log in logs:
        print('\n'.join(report(log, args.records)))
    if len(logs) > 1:
        print('== comparison (late frames per cause; frames; late %)')
        print('   ' + ' '.join(f'{Path(l["path"]).name[:24]:>26}' for l in logs))
        for key in ('frames', 'late', 'missed_vblanks') + CAUSES:
            print(f'   {key:<17}' + ' '.join(f'{(l["summary"] or {}).get(key, 0):>26}' for l in logs))
    if args.json:
        args.json.write_text(json.dumps(logs, indent=1))


if __name__ == '__main__':
    main()
