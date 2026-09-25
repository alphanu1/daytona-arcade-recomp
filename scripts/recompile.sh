#!/usr/bin/env sh
# Recompile the game's i960 code and its TGP program to native C++ and build them.
#   scripts/recompile.sh
# Output goes to build/gen/daytona93 and build/gen/daytona93_tgp (git-ignored;
# derived from the game).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[ -f build/rom_cache/daytona93/tgp_program.bin ] || python3 scripts/m2import.py roms/daytona93.zip build/rom_cache/daytona93
ninja -C build m2recomp m2tgprecomp
rm -rf build/gen/daytona93 && mkdir -p build/gen/daytona93
./build/m2recomp build/rom_cache/daytona93/program.bin build/gen/daytona93 --seeds seeds/daytona93.txt
mkdir -p build/gen/daytona93_tgp
./build/m2tgprecomp build/rom_cache/daytona93/tgp_program.bin build/gen/daytona93_tgp/tgp_gen.cpp
cmake -S . -B build >/dev/null
ninja -C build m2native m2replay m2tgpcheck
