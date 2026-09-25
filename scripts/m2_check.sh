#!/usr/bin/env sh
# Lockstep check (M1): record 600 frames of attract in MAME (trace + IRQ log),
# build the images from the user's ROM set, and replay them through the
# runtime. Game-derived files stay in git-ignored traces/ and build/.
#
#   scripts/m2_check.sh [FRAMES]
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FRAMES="${1:-600}"
cd "$ROOT"
[ -f build/rom_cache/daytona93/program.bin ] || python3 scripts/m2import.py roms/daytona93.zip build/rom_cache/daytona93
M2TRACE_FRAMES="$FRAMES" M2TRACE_IRQLOG="$ROOT/traces/m2check.irq" ./scripts/run_trace.sh traces/m2check >/dev/null 2>&1
./build/m2replay build/rom_cache/daytona93 traces/m2check/trace.m2tr traces/m2check.irq
[ -x build/m2native ] && ./build/m2native build/rom_cache/daytona93 traces/m2check/trace.m2tr traces/m2check.irq
