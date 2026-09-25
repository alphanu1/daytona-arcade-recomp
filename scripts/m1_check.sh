#!/usr/bin/env sh
# M1 lockstep check: record 600 frames of attract in MAME (trace + IRQ log),
# build the images from the user's ROM set, and replay them through the
# runtime. Game-derived files stay in git-ignored traces/ and build/.
#
#   scripts/m1_check.sh [FRAMES]
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FRAMES="${1:-600}"
cd "$ROOT"
[ -f build/rom_cache/daytona93/program.bin ] || python3 scripts/m2import.py roms/daytona93.zip build/rom_cache/daytona93
M2TRACE_FRAMES="$FRAMES" M2TRACE_IRQLOG="$ROOT/traces/m1check.irq" ./scripts/run_trace.sh traces/m1check >/dev/null 2>&1
./build/m1replay build/rom_cache/daytona93 traces/m1check/trace.m2tr traces/m1check.irq
