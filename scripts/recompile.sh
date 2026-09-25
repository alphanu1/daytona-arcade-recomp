#!/usr/bin/env sh
# Recompile the game's i960 code to native C++ and build it.
#   scripts/recompile.sh
# Output goes to build/gen/daytona93 (git-ignored; derived from the game).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[ -f build/rom_cache/daytona93/program.bin ] || python3 scripts/m2import.py roms/daytona93.zip build/rom_cache/daytona93
ninja -C build m2recomp
rm -rf build/gen/daytona93 && mkdir -p build/gen/daytona93
./build/m2recomp build/rom_cache/daytona93/program.bin build/gen/daytona93 --seeds seeds/daytona93.txt
cmake -S . -B build >/dev/null
ninja -C build m2native m2replay
