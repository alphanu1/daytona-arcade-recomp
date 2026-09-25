#!/usr/bin/env sh
# Lockstep check: record a run in MAME (trace + IRQ log), build the images
# from the user's ROM set, and replay the run through the recompiled native
# code (m2native) and the reference interpreter (m2replay), and MAME's TGP
# log through the recompiled TGP program (m2tgpcheck); m2native also holds
# the native geometrizer to MAME's display lists at every vblank.
# Game-derived files stay in git-ignored traces/ and build/.
#
#   scripts/m2_check.sh                 600 frames of attract, no input
#   scripts/m2_check.sh SCENARIO        scripts/inputs/SCENARIO.txt (coin up,
#                                       race, test mode...), its own frame count
#
# A scenario's input stream is built from the script with make_input.py; it
# needs one recorded stream for the field layout (traces/hdr.m2in, recorded
# once with M2TRACE_RECORD_INPUT). The trace is deleted after the check
# unless M2_CHECK_KEEP is set.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="${1:-attract}"
cd "$ROOT"
[ -f build/rom_cache/daytona93/tgp_program.bin ] || python3 scripts/m2import.py roms/daytona93.zip build/rom_cache/daytona93
OUT="traces/check_$NAME"
if [ "$NAME" = attract ]; then
    M2TRACE_FRAMES=600 M2TRACE_IRQLOG="$ROOT/$OUT.irq" M2TRACE_TGPLOG="$ROOT/$OUT.tgp" M2TRACE_GEOLOG="$ROOT/$OUT.geo" ./scripts/run_trace.sh "$OUT" >/dev/null 2>&1
else
    python3 scripts/make_input.py traces/hdr.m2in "scripts/inputs/$NAME.txt" "$OUT.m2in"
    FRAMES="$(awk '/^frames/{print $2}' "scripts/inputs/$NAME.txt")"
    M2TRACE_FRAMES="$FRAMES" M2TRACE_REPLAY_INPUT="$OUT.m2in" M2TRACE_IRQLOG="$ROOT/$OUT.irq" M2TRACE_TGPLOG="$ROOT/$OUT.tgp" M2TRACE_GEOLOG="$ROOT/$OUT.geo" \
        ./scripts/run_trace.sh "$OUT" >/dev/null 2>&1
fi
STATUS=0
[ -x build/m2native ] && { ./build/m2native build/rom_cache/daytona93 "$OUT/trace.m2tr" "$OUT.irq" "$OUT.geo" || STATUS=1; }
[ -n "${M2_CHECK_REPLAY:-}" ] || [ "$NAME" = attract ] && { ./build/m2replay build/rom_cache/daytona93 "$OUT/trace.m2tr" "$OUT.irq" || STATUS=1; }
[ -x build/m2tgpcheck ] && { ./build/m2tgpcheck build/rom_cache/daytona93 "$OUT.tgp" || STATUS=1; }
[ -n "${M2_CHECK_KEEP:-}" ] || rm -f "$OUT/trace.m2tr" "$OUT.tgp" "$OUT.geo"
exit $STATUS
