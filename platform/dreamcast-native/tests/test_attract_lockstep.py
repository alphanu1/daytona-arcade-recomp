#!/usr/bin/env python3
"""The native attract loop against the original's, tick by tick, over every course stretch.

usage: test_attract_lockstep.py REFERENCE NATIVE [--view-tolerance U] [--report N]

REFERENCE  m2segment --trace-race FILE --trace-attract over the attract (no input: courses 0, 2, 1, 0 ...)
NATIVE     tests/trace_attract_native.c over the same number of loops

Rows are paired by the frame counter 5010b0 (scene=): the native row of a tick carries the 5010b0 of the pass that
made its state, the reference's last row with that value is the state after that pass (a lag frame repeats it).
Checked on every native row (the scene start and each cinematic tick, phases 3..5):
  - the timeline: the reference's latched phase 5010a0 is the native row's (2 on the scene start), its course
    501460 the native loop's, its car count 501080 the stage's;
  - every car (spawn index = entity order) in flags, prof, prev, lap, done, si, spd, x, y, z, yaw, head, path, lat,
    want, road, bit for bit (lap: the native cinematic counts from 0, the original's spawns write 255);
  - the camera's key tick ctick (5014a0) and the cell bitmaps 501520/501540 (cells=);
  - the view matrix 501560 within --view-tolerance (default 0.25 units a word; the largest difference is printed:
    the camera's TGP products against the native's single-precision forms);
  - the scene light (camera object +60..+68 and its range index +84: tunnel_light.h on Advanced's tunnel): the index
    exact, the components within 1e-6 (the TGP's normalise; the largest difference is printed).
The V.R. demo's rows (phases 6/7: its start, the phase-6 iteration, then its 834 ticks; vr_demo.h): the camera mode,
the view preset 501710, the drawn-car count 5014f7 (guided=), the cell bitmaps, the view matrix within the tolerance,
and every car in DEMO_CAR (the race trace's fields; not body=, the race's body word the native gives every car alike,
nor str=, the player's steering word the demo keeps from the cinematic and the native cinematic does not keep).
Rows accepted on evidence, counted apart:
  - a stage setup row (the reference's camera key tick 0): the original's script object (2264f4, after the cars in
    the task list) has spawned the next stage at the end of its pass, the native enters it on its next tick; only
    the timeline is compared, and the next row must be equal (as compare_race_trace.py --podium does);
  - a script-ahead row: the same object's speed event (st +44) at the end of the pass; the reference's want= of a car
    differs and is the native's on the next row (the native sets it at the start of that tick, before the car reads
    it);
  - the road word of a car on its spawn's first row (the native sets it in the car's first step: road=0);
  - the cells of a scene start row (the original's bitmaps are the previous scene's until its scenery object has
    run: the native's start row holds its initial detail pass);
  - a mid-pass row: the board's frame ended inside the scenery object 5890 (after the camera): the reference's
    bitmaps are a subset of the native's (partly written) while everything else is equal;
  - a demo row sampled mid-pass: the board's frame ended after the cars' updaters, before the field pass and the
    camera: every field that differs is the native row's before (the collision words, the camera, the view).
The scenes' carried state (frontend_pool.h: rank keys, laps, the leading lap, the view) is native.
Also printed: where each stretch begins and ends in both (5010b0 of the first and last cinematic tick of each
course). Exits 1 on any difference, 77 when a file is missing."""
import argparse
import struct
import sys
from pathlib import Path

CAR = ["flags", "prof", "prev", "lap", "done", "si", "spd", "x", "y", "z", "yaw", "head", "path", "lat", "want", "road"]
DEMO_CAR = ["flags", "prof", "prev", "far", "lap", "done", "cnt", "rank", "key", "keyhi", "si", "spd", "x", "y", "z",
            "yaw", "head", "path", "lat", "gear", "rpm", "acc", "brk", "want", "road", "gs", "surf", "mask", "sx", "sz",
            "tx", "tz", "cside", "crel", "dmg", "crashes", "tilt", "pitch"]
DEMO_T = ["camera", "preset", "guided", "cells"]


def demo_differences(r, n, tolerance):
    """[(key, reference, native)] of a demo row; key 'view' for the matrix beyond the tolerance."""
    out = [(k, r["T"].get(k), n["T"].get(k)) for k in DEMO_T if r["T"].get(k) != n["T"].get(k)]
    worst = max(abs(word(a) - word(b)) for a, b in zip(r["T"]["view"].split("."), n["T"]["view"].split(".")))
    if worst > tolerance:
        out.append(("view", r["T"]["view"], n["T"]["view"]))
    for car, c in n["C"].items():
        rc = r["C"].get(car)
        if rc is None:
            out.append((f"car {car}", None, "present"))
            continue
        out += [(f"car {car} {k}", rc.get(k), c.get(k)) for k in DEMO_CAR if rc.get(k) != c.get(k)]
    return out, worst


def demo_value(row, key):
    if key.startswith("car "):
        _, car, field = key.split()
        return row["C"].get(int(car), {}).get(field)
    return row["T"].get(key)


def rows(path, reference):
    """{5010b0: row} (the last row of each value), rows = {"T": dict, "C": {id: dict}}."""
    by_scene, current, order = {}, None, []
    with open(path) as source:
        for line in source:
            if line.startswith("#"):
                continue
            kind, *pairs = line.split()
            values = dict(p.split("=", 1) for p in pairs if "=" in p)
            if kind == "T":
                current = {"T": values, "C": {}}
                scene = int(values["scene"])
                if reference and scene not in by_scene:
                    order.append(scene)
                by_scene[scene] = current
                if not reference:
                    order.append(scene)
            elif kind == "C" and current is not None:
                current["C"][int(values["id"])] = values
    return by_scene, order


def word(text):
    return struct.unpack("<f", struct.pack("<I", int(text, 16)))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("reference", type=Path)
    parser.add_argument("native", type=Path)
    parser.add_argument("--view-tolerance", type=float, default=0.25)
    parser.add_argument("--report", type=int, default=12)
    args = parser.parse_args()
    if not args.reference.exists() or not args.native.exists():
        print("SKIP: files missing")
        return 77
    ref, _ = rows(args.reference, True)
    nat, order = rows(args.native, False)
    problems, checked, cars = [], 0, 0
    view_worst = (0.0, None)
    stretches = {}
    accepted = {"setup": 0, "script": 0, "spawn road": 0, "start cells": 0, "mid-pass": 0}
    light_worst = 0.0
    spawned = set()   # native rows whose cars were (re)spawned on the row before: the start, a stage setup
    demo_rows = demo_cars = demo_mid = 0
    demo_view = (0.0, None)
    for i, scene in enumerate(order):
        n = nat[scene]
        t = n["T"]
        r = ref.get(scene)
        loop_key = (int(t["scene"]) - int(t["ticks"]), t["phase"] in ("6", "7"))
        course, first, last = stretches.get(loop_key, (t["course"], scene, scene))
        stretches[loop_key] = (course, min(first, scene), max(last, scene))
        if r is None:
            problems.append(f"5010b0 {scene}: no reference row")
            continue
        if t["phase"] in ("6", "7"):
            checked += 1
            demo_rows += 1
            demo_cars += len(n["C"])
            why = []
            if r["T"]["active"] != t["phase"]:
                why.append(f"phase {r['T']['active']} (native {t['phase']})")
            if r["T"]["course"] != t["course"]:
                why.append(f"course {r['T']['course']} (native {t['course']})")
            if int(r["T"]["count"]) != int(t["count"]):
                why.append(f"cars {r['T']['count']} (native {t['count']})")
            diff, worst = demo_differences(r, n, args.view_tolerance)
            before = nat.get(order[i - 1]) if i else None
            def as_before(key, value):
                if key != "view":
                    return demo_value(before, key) == value
                return max(abs(word(a) - word(b)) for a, b in zip(before["T"]["view"].split("."), value.split("."))) \
                    <= args.view_tolerance
            if diff and not why and before is not None and before["T"]["phase"] in ("6", "7") and \
                    all(as_before(k, a) for k, a, _ in diff):
                demo_mid += 1
                continue
            if worst > demo_view[0]:
                demo_view = (worst, scene)
            why += [f"{k} {a}/{b}" for k, a, b in diff]
            if why:
                problems.append(f"5010b0 {scene} (course {t['course']} demo tick {t['ticks']}): " + "; ".join(why[:8]))
            continue
        rt = r["T"]
        checked += 1
        why = []
        if rt["active"] != t["phase"]:
            why.append(f"phase {rt['active']} (native {t['phase']})")
        if rt["course"] != t["course"]:
            why.append(f"course {rt['course']} (native {t['course']})")
        setup = rt["ctick"] == "0" and t["ctick"] != "0" and t["ticks"] != "0"
        if setup:
            accepted["setup"] += 1
            spawned.add(order[i + 1] if i + 1 < len(order) else None)
            if why:
                problems.append(f"5010b0 {scene} (course {t['course']} tick {t['ticks']}): " + "; ".join(why))
            continue
        if t["ticks"] == "0":
            spawned.add(scene)
        if int(rt["count"]) != int(t["count"]):
            why.append(f"cars {rt['count']} (native {t['count']})")
        if rt["ctick"] != t["ctick"]:
            why.append(f"ctick {rt['ctick']} (native {t['ctick']})")
        rcells = [int(v, 16) for v in rt["cells"].split(".")]
        ncells = [int(v, 16) for v in t["cells"].split(".")]
        cells_note = None
        if rcells != ncells:
            if t["ticks"] == "0":
                cells_note = "start cells"
            elif all(a & ~b == 0 for a, b in zip(rcells, ncells)):
                cells_note = "mid-pass"
            else:
                why.append("cells " + ",".join(str(k) for k in range(16) if rcells[k] != ncells[k]))
        rl, nl = rt.get("light", "").split("."), t.get("light", "").split(".")
        if len(rl) != 4 or len(nl) != 4 or rl[3] != nl[3]:
            why.append(f"light {rt.get('light')} (native {t.get('light')})")
        else:
            d = max(abs(word(a) - word(b)) for a, b in zip(rl[:3], nl[:3]))
            light_worst = max(light_worst, d)
            if d > 1e-6:
                why.append(f"light {rt.get('light')} (native {t.get('light')})")
        worst = max(abs(word(a) - word(b)) for a, b in zip(rt["view"].split("."), t["view"].split(".")))
        if worst > view_worst[0]:
            view_worst = (worst, scene)
        if worst > args.view_tolerance:
            why.append(f"view {worst:.4g}")
        following = nat.get(order[i + 1]) if i + 1 < len(order) else None
        script = False
        for car, c in n["C"].items():
            rc = r["C"].get(car)
            if rc is None:
                why.append(f"car {car} missing")
                continue
            cars += 1
            bad = []
            for key in CAR:
                a, b = rc[key], c[key]
                if key == "lap":
                    b = str((int(b) + 255) % 256)
                if key == "flags":
                    a, b = int(a, 16), int(b, 16)
                if a == b:
                    continue
                if key == "road" and scene in spawned and c[key] == "0":
                    accepted["spawn road"] += 1
                    continue
                if key == "want" and following and following["C"].get(car, {}).get("want") == rc[key]:
                    script = True
                    continue
                bad.append(f"{key} {rc[key]}/{c[key]}")
            if bad:
                why.append(f"car {car}: " + " ".join(bad[:6]))
        if why:
            problems.append(f"5010b0 {scene} (course {t['course']} tick {t['ticks']}): " + "; ".join(why[:8]))
        else:
            accepted["script"] += script
            if cells_note:
                accepted[cells_note] += 1
    for line in problems[:args.report]:
        print(line)
    for (base, demo), (course, first, last) in sorted(stretches.items(), key=lambda kv: kv[1][1]):
        print(f"stretch course {course}{' V.R. demo' if demo else ''}: 5010b0 {first}..{last} "
              f"({last - first} ticks after the scene start)")
    print(f"V.R. demo: {demo_rows} rows ({demo_cars} car rows), {demo_mid} sampled mid-pass; largest view difference "
          f"{demo_view[0]:.4g} (5010b0 {demo_view[1]})")
    print(f"attract lockstep: {checked - len([p for p in problems if 'no reference' not in p])} of {len(order)} rows equal "
          f"({cars} car rows), {len(problems)} with differences; accepted: {accepted['setup']} stage setup rows, "
          f"{accepted['script']} script-ahead rows, {accepted['spawn road']} spawn road words, {accepted['start cells']} "
          f"start rows' cells, {accepted['mid-pass']} mid-pass rows; largest view difference {view_worst[0]:.4g} "
          f"(5010b0 {view_worst[1]}), largest light difference {light_worst:.3g}")
    return 1 if problems or not checked else 0


if __name__ == "__main__":
    sys.exit(main())
