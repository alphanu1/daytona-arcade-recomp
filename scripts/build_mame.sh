#!/usr/bin/env sh
# Build a Model-2-only MAME from extern/mame (fetch_mame.sh first).
# Output: extern/mame/m2 (the SUBTARGET name). Several tens of minutes on 4 cores.
set -eu
cd "$(dirname "$0")/../extern/mame"
make SUBTARGET=m2 SOURCES=src/mame/sega/model2.cpp TOOLS=0 USE_QTDEBUG=0 NOWERROR=1 SYMBOLS=0 OPTIMIZE=2 \
    -j"$(nproc)" "$@"
