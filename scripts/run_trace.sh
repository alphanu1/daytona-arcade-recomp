#!/usr/bin/env sh
# Run the Model-2 MAME headless with the m2trace plugin.
#
#   scripts/run_trace.sh OUT_DIR [extra MAME args...]
#
# Environment (see docs/trace-format.md): M2TRACE_FRAMES (default 600),
# M2TRACE_TRIGGER, M2TRACE_RECORD_INPUT, M2TRACE_REPLAY_INPUT. The trace goes
# to OUT_DIR/trace.m2tr and the branch harvest to OUT_DIR/branches.txt.
#
# Every run starts from empty NVRAM, cfg and state directories inside OUT_DIR,
# so two runs of the same command start from the same machine state.
# ROMs are read from roms/ (git-ignored). SYSTEM defaults to daytona93.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$1"; shift
SYSTEM="${SYSTEM:-daytona93}"
rm -rf "$OUT"
mkdir -p "$OUT/nvram" "$OUT/cfg" "$OUT/sta" "$OUT/diff" "$OUT/snap"
export M2TRACE_OUT="${M2TRACE_OUT:-$OUT/trace.m2tr}"
export M2TRACE_FRAMES="${M2TRACE_FRAMES:-600}"
export M2TRACE_BRANCHES="${M2TRACE_BRANCHES:-$OUT/branches.txt}"
exec "$ROOT/extern/mame/m2" "$SYSTEM" \
    -rompath "$ROOT/roms" \
    -nvram_directory "$OUT/nvram" -cfg_directory "$OUT/cfg" \
    -state_directory "$OUT/sta" -diff_directory "$OUT/diff" -snapshot_directory "$OUT/snap" \
    -video none -sound none -nothrottle -skip_gameinfo \
    -plugins -plugin m2trace \
    -pluginspath "$ROOT/extern/mame/plugins;$ROOT/tools/mame-plugins" \
    "$@"
