#!/usr/bin/env python3
"""Compare a reference race trace (m2segment --trace-race) with a native one
(tests/trace_race_native.c). Host-only evidence tool: it reports, never repairs.

Row pairing: native row 0 is the state before any tick; native row i (i>=1) was
driven with the controls of reference row FIRST+i-1, so the natural pairing is
native[i] <-> reference[FIRST-1+i]. --shift K pairs native[i] with
reference[FIRST-1+i+K] instead; --search tries a window of shifts and reports
how long the player matches for each (alignment evidence, not a fix).

--require-equal turns the report into a check: exit status 1 unless every
paired row (but those of --skip-rows, e.g. a reference row sampled mid-pass)
has the same timeline and the same prof/lap/done/rank/x/z/spd/yaw for every car.

--podium (with --require-equal) checks a race that ends on the podium: the race
rows as above; the podium rows (native active phase 28) with the podium scene's
cars on both sides (prof/x/z/spd/yaw: the original's podium cars keep the
race pool's lap and rank words) and, after the race, the timeline without the
phase words (they switch one row apart) but with the phase timer and the record
notification; the timeline alone on the podium entry row (active 27: the
original's pool holds the podium scene on that tick, the native's the race
field), on each stage setup row (the reference's camera cursor at 0: the
original's pool already holds the next stage, the native one enters it on the
next tick, with the same state from then on) and on the results rows after the
podium (phases 25/26: the original's pool keeps the podium's cars, the native
trace prints the race field). Reference rows of frames in which the board's
logic did not run (the loads after the race) are dropped first. The podium
camera is reported, not checked: its largest view-matrix difference.

--view-limit U (with --require-equal) also checks the view matrix 501560 of the race rows (phases 20..24) under the
follow and in-car views (camera mode 0, 2) and the crash camera (mode 1, 501713 = 0), within U a word: the keyed
start and pit tracks and the rows the evidence rule below accepts are left out.

Two kinds of reference rows hold no state of a whole logic pass, and --require-equal
accepts them only on that evidence (counted and listed apart):
  mid-pass: the board's frame ended inside a pass (the start's scene loads, the
    podium's heavy frames): the phase handler and the cars up to some car k have
    run, the later cars and the camera have not. Accepted when the timeline is the
    native row's, every car before the split equals the native row and every car
    from the split on the native row before (at least one), and the camera cursor
    ctick is the native row before's.
  caught-up stall: a frame without a pass (5010b0 unchanged) caught up by a double
    pass in the next one: the row repeats the state before. Accepted when the next
    reference row's 5010b0 is two on and every compared field equals the native row
    before.
"""
import argparse
import math
import struct
import sys

TIMELINE = ["phase", "active", "remaining", "clock", "laps", "finish"]
DISCRETE = ["prof", "lap", "done", "rank"]
CONTINUOUS = ["x", "z", "spd", "yaw"]
PLAYER_EXTRA = ["si", "gear", "rpm", "acc", "brk", "str", "lat", "path", "prev", "far", "cnt", "key", "y", "head", "flags"]
OPPONENT_EXTRA = ["si", "lat", "path", "y", "head", "want", "flags"]
FLOATS = {"x", "y", "z", "spd", "want"}
HEX = {"flags", "finish", "brk", "upd", "start"}
ANGLES = {"yaw", "head"}


def parse(path):
    rows, header = [], ""
    with open(path) as source:
        for line in source:
            if line.startswith("#"):
                header = line.strip()
                continue
            kind, *pairs = line.split()
            values = dict(pair.split("=", 1) for pair in pairs)
            n = int(values["n"])
            if kind == "T":
                while len(rows) <= n:
                    rows.append(None)
                rows[n] = {"T": values, "C": {}}
            elif kind == "C":
                rows[n]["C"][int(values["id"])] = values
    if any(row is None for row in rows):
        raise SystemExit(f"{path}: missing rows")
    return header, rows


def number(key, text):
    if key in FLOATS:
        return struct.unpack("<f", struct.pack("<I", int(text, 16)))[0]
    if key in HEX:
        return int(text, 16)
    return int(text)


def delta(key, a, b):
    x, y = number(key, a), number(key, b)
    if key in ANGLES:
        return ((y - x + 32768) & 65535) - 32768
    return y - x


def show(key, text):
    return f"{text}({number(key, text):.6g})" if key in FLOATS else text


def first_difference(pairs, getter, keys):
    for index, (ref, nat) in enumerate(pairs):
        a, b = getter(ref), getter(nat)
        if a is None or b is None:
            continue
        differing = [k for k in keys if k in a and k in b and a[k] != b[k]]
        if differing:
            return index, differing
    return None, []


def growth(pairs, getter, key, start, steps=(0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096)):
    out = []
    for step in steps:
        index = start + step
        if index >= len(pairs):
            break
        a, b = getter(pairs[index][0]), getter(pairs[index][1])
        if a is None or b is None or key not in a or key not in b:
            continue
        out.append(f"+{step}:{delta(key, a[key], b[key]):.6g}")
    return " ".join(out)


def distance(a, b):
    return math.hypot(number("x", b["x"]) - number("x", a["x"]), number("z", b["z"]) - number("z", a["z"]))


def events(rows, label, limit, start=0):
    """Decision milestones of one side, by its own row index."""
    out, previous = [], None
    for index, row in enumerate(rows, start):
        if 0 not in row["C"]:   # --trace-attract's rows without cars (the title)
            continue
        t, p = row["T"], row["C"][0]
        state = (t["phase"], t["active"], p.get("lap"), p.get("done"), t.get("rolling", "?"), t.get("normal", "?"), int(t["clock"]) > 0)
        if previous is not None and state != previous:
            out.append(f"  {label} row={index} frame={t['frame']} phase={t['phase']} active={t['active']} lap={p.get('lap')} done={p.get('done')}"
                       f" rank={p.get('rank')} prof={p.get('prof')} remaining={t['remaining']} clock={t['clock']}"
                       f" rolling={t.get('rolling', '?')} normal={t.get('normal', '?')}")
        previous = state
    for line in out[:limit]:
        print(line)
    if len(out) > limit:
        print(f"  {label} ... {len(out) - limit} more transitions")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reference")
    parser.add_argument("native")
    parser.add_argument("--first", type=int, required=True, help="FIRST_ROW given to trace_race_native")
    parser.add_argument("--shift", type=int, default=0)
    parser.add_argument("--search", type=int, default=0, help="also try shifts -N..N")
    parser.add_argument("--tolerance", type=float, default=0.0, help="report first |player x/z| error above this too")
    parser.add_argument("--events", type=int, default=40)
    parser.add_argument("--require-equal", action="store_true", help="exit 1 unless timeline and every car agree")
    parser.add_argument("--skip-rows", default="", help="native rows the check skips (comma-separated)")
    parser.add_argument("--podium", action="store_true", help="with --require-equal: the race ends on the podium")
    parser.add_argument("--view-limit", type=float, help="with --require-equal: largest view word difference on race rows")
    args = parser.parse_args()
    ref_header, ref = parse(args.reference)
    nat_header, nat = parse(args.native)
    if args.podium:
        # Frames in which the board's logic did not run (5010b0 unchanged and not caught up by a double pass the
        # frame after): the loads after the race (the podium's 2D screen, the results) stall the original's frame;
        # native ticks are logic passes, so these reference rows pair with no native row.
        def stalled(n):
            if n < args.first + 2 or "scene" not in ref[n]["T"] or ref[n]["T"]["scene"] != ref[n - 1]["T"].get("scene"):
                return False
            m = n
            while m + 1 < len(ref) and ref[m + 1]["T"].get("scene") == ref[n]["T"]["scene"]:
                m += 1
            return not (m + 1 < len(ref) and int(ref[m + 1]["T"]["scene"]) - int(ref[n]["T"]["scene"]) >= 2)
        lag = [n for n in range(len(ref)) if stalled(n)]
        ref = [row for n, row in enumerate(ref) if n not in set(lag)]
        print(f"reference frames without a logic pass (dropped): {lag}")
    print(f"reference: {args.reference} rows={len(ref)} {ref_header}")
    print(f"native:    {args.native} rows={len(nat)} {nat_header}")

    def pairing(shift):
        base = args.first - 1 + shift
        return [(ref[base + i], nat[i]) for i in range(len(nat)) if 0 <= base + i < len(ref)], base

    player = lambda row: row["C"].get(0)
    if args.search:
        print("\nalignment search (player prof/lap/x/z/spd/yaw exact, from native row0 and from native row1):")
        for shift in range(-args.search, args.search + 1):
            pairs, base = pairing(shift)
            if not pairs:
                continue
            keys = DISCRETE[:2] + CONTINUOUS
            index, fields = first_difference(pairs, player, keys)
            later, later_fields = first_difference(pairs[1:], player, keys)
            print(f"  shift={shift:+d} native0<->reference row {base}: first player difference at native row "
                  f"{'none' if index is None else index} {fields}; skipping row0: "
                  f"{'none' if later is None else later + 1} {later_fields}")

    pairs, base = pairing(args.shift)
    print(f"\npairing: native row i <-> reference row {base}+i (shift {args.shift:+d}); compared rows={len(pairs)}"
          f" reference frames {pairs[0][0]['T']['frame']}..{pairs[-1][0]['T']['frame']}")

    print("\n== start state (native row0, before any tick) ==")
    r0, n0 = pairs[0]
    print("  timeline: " + " ".join(f"{k}=ref:{r0['T'].get(k)}/nat:{n0['T'].get(k)}" for k in TIMELINE))
    unequal = 0
    for car in sorted(n0["C"]):
        a, b = r0["C"].get(car), n0["C"][car]
        if a is None:
            print(f"  car {car}: missing in reference")
            continue
        keys = [k for k in DISCRETE + CONTINUOUS + PLAYER_EXTRA + ["want"] if k in a and k in b and a[k] != b[k]]
        if keys:
            unequal += 1
            print(f"  car {car}: " + " ".join(f"{k}=ref:{show(k, a[k])}/nat:{show(k, b[k])}" for k in keys))
    print(f"  cars with any differing compared field at start: {unequal}/{len(n0['C'])}")

    print("\n== timeline (phase/timer/clock/laps/finish) ==")
    timeline = lambda row: row["T"]
    for key in TIMELINE:
        index, _ = first_difference(pairs, timeline, [key])
        if index is None:
            print(f"  {key}: equal for all {len(pairs)} rows")
        else:
            a, b = pairs[index]
            print(f"  {key}: first differs at native row {index} (reference frame {a['T']['frame']}): ref={a['T'][key]} nat={b['T'][key]}"
                  f"  growth[nat-ref] {growth(pairs, timeline, key, index)}")

    # Original shared inputs that the routine-level checks read from the board each call.
    print("\n== shared inputs (reference value at the end of the paired frame vs native owner) ==")
    for key in ("guided", "camera", "ctick", "stage", "ctr", "aux", "ptimer", "notify", "vticks", "cells", "view"):
        if key not in pairs[0][0]["T"] or key not in pairs[0][1]["T"]:
            print(f"  {key}: not present in both traces")
            continue
        index, _ = first_difference(pairs[1:], timeline, [key])
        same = sum(a["T"][key] == b["T"][key] for a, b in pairs[1:])
        domain = ""
        if key in ("guided", "camera"):
            values = sorted({int(a["T"][key]) for a, _ in pairs[1:]})
            native_values = sorted({int(b["T"][key]) for _, b in pairs[1:]})
            domain = f"reference values {values}, native values {native_values}; "
        print(f"  {key}: {domain}equal on {same}/{len(pairs) - 1} ticks; "
              f"first differs at native row {'none' if index is None else index + 1}")

    print("\n== player (car 0) ==")
    for group, keys in (("decisions", DISCRETE), ("pose/speed", CONTINUOUS), ("other mapped fields", PLAYER_EXTRA)):
        for key in keys:
            index, _ = first_difference(pairs, player, [key])
            if index is None:
                print(f"  [{group}] {key}: equal for all {len(pairs)} rows")
                continue
            a, b = player(pairs[index][0]), player(pairs[index][1])
            print(f"  [{group}] {key}: matched {index} rows; first differs at native row {index} (reference frame "
                  f"{pairs[index][0]['T']['frame']}): ref={show(key, a[key])} nat={show(key, b[key])}")
            print(f"      growth[nat-ref] {growth(pairs, player, key, index)}")
    index, _ = first_difference(pairs, player, ["x", "z"])
    if index is not None:
        steps = [0, 1, 4, 16, 64, 256, 512, 1024, 2048, 3000, 4000, 4900]
        print("  planar distance |nat-ref| after first pose difference: " + " ".join(
            f"+{s}:{distance(player(pairs[index + s][0]), player(pairs[index + s][1])):.4g}" for s in steps if index + s < len(pairs)))
        # Is the native player simply some rows early/late? Compare against neighbouring reference rows.
        for probe in (index, index + 16, index + 256, index + 1024):
            if probe >= len(pairs):
                continue
            b = player(pairs[probe][1])
            best = min(range(-12, 13), key=lambda k: distance(player(ref[base + probe + k]), b) if 0 <= base + probe + k < len(ref) else 1e30)
            print(f"      native row {probe}: nearest reference player within +-12 rows is row offset {best:+d} "
                  f"(distance {distance(player(ref[base + probe + best]), b):.4g}, same-row distance "
                  f"{distance(player(pairs[probe][0]), b):.4g})")
        if args.tolerance:
            for i, (a, b) in enumerate(pairs):
                if distance(player(a), player(b)) > args.tolerance:
                    print(f"  planar distance first exceeds {args.tolerance} at native row {i} (reference frame {a['T']['frame']})")
                    break

    print("\n== opponents (cars 1..) ==")
    summary = []
    for car in sorted(nat[0]["C"]):
        if not car:
            continue
        getter = lambda row, car=car: row["C"].get(car)
        d_index, d_keys = first_difference(pairs, getter, DISCRETE)
        c_index, c_keys = first_difference(pairs, getter, CONTINUOUS)
        e_index, e_keys = first_difference(pairs, getter, OPPONENT_EXTRA)
        summary.append((car, d_index, d_keys, c_index, c_keys, e_index, e_keys))
    never = [s[0] for s in summary if s[1] is None and s[3] is None]
    print(f"  opponents equal in prof/lap/done/rank/x/z/spd/yaw for every compared row: {len(never)} {never}")
    for label, slot in (("pose/speed", 3), ("decision", 1)):
        ordered = sorted((s for s in summary if s[slot] is not None), key=lambda s: s[slot])
        print(f"  first {label} difference by opponent (earliest first):")
        for s in ordered[:44]:
            car, index, keys = s[0], s[slot], s[slot + 1]
            a, b = pairs[index][0]["C"][car], pairs[index][1]["C"][car]
            last_a, last_b = pairs[-1][0]["C"].get(car), pairs[-1][1]["C"].get(car)
            tail = f" end-distance={distance(last_a, last_b):.4g}" if last_a and last_b else ""
            print(f"    car {car:2d}: native row {index} (frame {pairs[index][0]['T']['frame']}) "
                  + " ".join(f"{k}=ref:{show(k, a[k])}/nat:{show(k, b[k])}" for k in keys)
                  + f" ref-upd={a.get('upd')} nat-mode/roll={b.get('mode')}/{b.get('roll')}" + tail)
    if summary:
        earliest = min((s for s in summary if s[3] is not None), key=lambda s: s[3], default=None)
        if earliest:
            car, index = earliest[0], earliest[3]
            getter = lambda row: row["C"].get(car)
            for key in ("x", "z", "spd"):
                print(f"  earliest opponent car {car} {key} growth[nat-ref] {growth(pairs, getter, key, index)}")
            print("  earliest opponent extra fields at that row: " + " ".join(
                f"{k}=ref:{show(k, pairs[index][0]['C'][car][k])}/nat:{show(k, pairs[index][1]['C'][car][k])}"
                for k in OPPONENT_EXTRA if k in pairs[index][0]["C"][car] and k in pairs[index][1]["C"][car]))

    print("\n== decision milestones (each side on its own rows) ==")
    events([row for row, _ in pairs], "reference", args.events)
    events([row for _, row in pairs], "native   ", args.events)
    ref_tail = ref[base + len(nat):]
    if ref_tail:
        print(f"  reference continues {len(ref_tail)} rows beyond the native trace; its later transitions:")
        events(ref[base + len(nat) - 1:], "reference", args.events, base + len(nat) - 1)
    last_r, last_n = pairs[-1]
    print(f"\nfinal compared row: reference phase={last_r['T']['phase']} remaining={last_r['T']['remaining']} clock={last_r['T']['clock']}"
          f" lap={last_r['C'][0]['lap']} rank={last_r['C'][0]['rank']} prof={last_r['C'][0]['prof']} | native phase={last_n['T']['phase']}"
          f" remaining={last_n['T']['remaining']} clock={last_n['T']['clock']} lap={last_n['C'][0]['lap']} rank={last_n['C'][0]['rank']}"
          f" prof={last_n['C'][0]['prof']}")
    if args.require_equal:
        skip = {int(v) for v in args.skip_rows.split(",") if v}
        bad = []
        kinds = {"race": 0, "podium": 0, "timeline": 0}
        podium, view_worst = False, (0.0, None)
        race_view_rows, race_view_worst = 0, (0.0, None)
        explained = {"mid-pass": [], "caught-up stall": []}

        def differs(a, b, keys, timeline, kind):
            return any(a["T"].get(k) != b["T"].get(k) for k in timeline) or (kind != "timeline" and (set(a["C"]) != set(b["C"]) or any(
                a["C"][car].get(k) != b["C"][car].get(k) for car in a["C"] for k in keys)))

        def explain(i, keys, timeline, kind):
            """The kind of a reference row that holds no whole pass (see the module text), or None."""
            if i < 1 or kind == "timeline":
                return None
            a, b, before = pairs[i][0], pairs[i][1], pairs[i - 1][1]
            scene = lambda row: int(row["T"]["scene"]) if "scene" in row["T"] else None
            if i + 1 < len(pairs) and scene(a) is not None and scene(a) == scene(pairs[i - 1][0]) and \
                    scene(pairs[i + 1][0]) == scene(a) + 2 and not differs(a, before, keys, timeline, kind):
                return "caught-up stall"
            if any(a["T"].get(k) != b["T"].get(k) for k in timeline) or a["T"].get("ctick") != before["T"].get("ctick") or \
                    set(a["C"]) != set(b["C"]) or set(a["C"]) != set(before["C"]):
                return None
            same = lambda row, car: all(a["C"][car].get(k) == row["C"][car].get(k) for k in keys)
            cars = sorted(a["C"])
            split = next((n for n, car in enumerate(cars) if not same(b, car)), len(cars))
            if split == len(cars) or not all(same(before, car) for car in cars[split:]):
                return None
            return "mid-pass"
        for i, (a, b) in enumerate(pairs):
            if i in skip:
                continue
            kind, view = "race", None
            if args.podium:
                active = b["T"]["active"]
                podium = podium or active in ("27", "28")
                if active == "27" or (active == "28" and a["T"].get("ctick") == "0") or (podium and active in ("25", "26")):
                    kind = "timeline"
                elif active == "28":
                    kind = "podium"
                    if "view" in a["T"] and "view" in b["T"]:
                        view = max(abs(number("x", x) - number("x", y)) for x, y in zip(a["T"]["view"].split("."), b["T"]["view"].split(".")))
            kinds[kind] += 1
            keys, timeline = DISCRETE + CONTINUOUS, TIMELINE
            if kind != "race":
                # The podium scene's cars: the original's pool keeps the race's lap and rank words in them.
                keys = ["prof"] + CONTINUOUS
                # After the race the phase words switch one row apart on the two sides (the native trace sets the
                # owner's phase at once); the timers carry the phases instead.
                timeline = [k for k in TIMELINE if k not in ("phase", "active")] + ["ptimer", "notify"]
            why = None
            if differs(a, b, keys, timeline, kind):
                why = explain(i, keys, timeline, kind)
                if why:
                    explained[why].append(i)
                else:
                    bad.append(i)
            if view is not None and not why:   # a row without a whole pass shows the camera of the pass before
                view_worst = max(view_worst, (view, i), key=lambda v: v[0])
            if args.view_limit is not None and kind == "race" and not why and 20 <= int(b["T"]["active"]) <= 24 and \
                    "view" in a["T"] and "view" in b["T"] and (a["T"].get("camera") != "1" or a["T"].get("ctr") == "0"):
                worst = max(abs(number("x", x) - number("x", y)) for x, y in zip(a["T"]["view"].split("."), b["T"]["view"].split(".")))
                race_view_rows += 1
                race_view_worst = max(race_view_worst, (worst, i), key=lambda v: v[0])
                if worst > args.view_limit:
                    bad.append(i)
        for why, rows in explained.items():
            if rows:
                print(f"\n{why} reference rows (accepted on their evidence): {len(rows)}, native rows {rows}")
        verdict = "PASS" if not bad else f"FAIL: {len(bad)} rows differ, first native row {bad[0]}"
        if args.podium:
            if not kinds["podium"]:
                verdict = "FAIL: no podium rows"; bad = bad or [None]
            print(f"\npodium: {kinds['race']} race rows, {kinds['podium']} podium rows (cars compared), {kinds['timeline']} rows timeline"
                  f" only (entry, stage setups, results); podium camera: largest view difference {view_worst[0]:.4g}"
                  f" (native row {view_worst[1]})")
        if args.view_limit is not None:
            print(f"\nrace views: {race_view_rows} rows, largest view difference {race_view_worst[0]:.4g} (native row {race_view_worst[1]})")
        print(f"\nrequire-equal: {len(pairs) - len(skip & set(range(len(pairs))))} rows, {len(nat[0]['C'])} cars: {verdict}")
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
