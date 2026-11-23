#!/usr/bin/env python3
"""Benchmark headless m2run: several runs per build, median and spread.

Single runs on a desktop vary by 10-15% (other load, clocks, the first run
after a build), so one number proves nothing. This runs each build N times,
interleaved so drift hits all builds alike, and fails if any run's results
differ: frame, i960/TGP instruction, interrupt and sound-byte counts, and the
screen hash (or the all-frames hash with --hash-frames).

  python scripts/bench_m2run.py [--builds build build-other] [--runs 5]
         [--set daytona93] [--frames 6000] [--inputs scripts/inputs/race_basic.txt]
         [-- extra m2run options, e.g. --no-render or --profile]

The ROM images come from BUILD/rom_cache/SET of the first build (made by
setup). Extra options after "--" go to every run; setting
M2_RASTER_THREADS in the environment pins the rasterizer's thread count.
"""

import argparse
import os
import re
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = ".exe" if os.name == "nt" else ""
SUMMARY = re.compile(r"^m2run: (.*?); ([0-9.]+) s \(")
HASH = re.compile(r"^\s+(last screen hash \S+|all \d+ drawn frames hash \S+)")


def m2run_path(build):
    for sub in ("Release", ""):
        path = os.path.join(ROOT, build, sub, "m2run" + EXE)
        if os.path.exists(path):
            return path
    sys.exit(f"bench: no m2run in {build} (build it: cmake --build {build} --config Release --target m2run)")


def run_once(exe, images, frames, inputs, extra):
    cmd = [exe, images, str(frames)] + (["--inputs", inputs] if inputs else []) + extra
    out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if out.returncode:
        sys.exit(f"bench: {' '.join(cmd)} failed:\n{out.stdout}{out.stderr}")
    counts = seconds = None
    hashes = []
    for line in out.stdout.splitlines():
        if m := SUMMARY.match(line):
            counts, seconds = m.group(1), float(m.group(2))
        elif m := HASH.match(line):
            hashes.append(m.group(1))
    if seconds is None:
        sys.exit(f"bench: no summary line from {exe}:\n{out.stdout}")
    return seconds, (counts, tuple(hashes)), out.stdout


def main():
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--builds", nargs="+", default=["build"])
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--set", default="daytona93")
    ap.add_argument("--frames", type=int, default=6000)
    ap.add_argument("--inputs", default=os.path.join("scripts", "inputs", "race_basic.txt"))
    args = ap.parse_args(argv)

    images = os.path.join(ROOT, args.builds[0], "rom_cache", args.set)
    if not os.path.isdir(images):
        sys.exit(f"bench: no ROM images at {images} (run setup first)")
    exes = {b: m2run_path(b) for b in args.builds}
    times = {b: [] for b in args.builds}
    reference = None
    for r in range(args.runs):
        for b in args.builds:
            seconds, result, stdout = run_once(exes[b], images, args.frames, args.inputs, extra)
            if reference is None:
                reference = result
                print(f"results: {result[0]}" + "".join(f"; {h}" for h in result[1]))
            elif result != reference:
                sys.exit(f"bench: {b} run {r + 1} differs from the first run:\n{stdout}")
            times[b].append(seconds)
            print(f"  run {r + 1} {b}: {seconds:.2f} s ({args.frames / seconds:.0f} frames/s)", flush=True)

    print(f"\n{args.frames} frames x {args.runs} runs, {args.set}, {os.path.basename(args.inputs) if args.inputs else 'no inputs'}"
          + (f", {' '.join(extra)}" if extra else "") + "; all results identical")
    base = statistics.median(times[args.builds[0]])
    for b in args.builds:
        t = times[b]
        med = statistics.median(t)
        line = (f"  {b:<16} median {med:6.2f} s ({args.frames / med:5.0f} frames/s), "
                f"min {min(t):.2f}, max {max(t):.2f}, spread {100 * (max(t) - min(t)) / med:4.1f}%")
        if b != args.builds[0]:
            line += f", {100 * (base - med) / base:+.1f}% vs {args.builds[0]}"
        print(line)


if __name__ == "__main__":
    main()
