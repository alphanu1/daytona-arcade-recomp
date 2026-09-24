#!/usr/bin/env sh
# Fetch Berkeley SoftFloat 3e at the pinned commit into extern/softfloat
# (git-ignored). BSD-3-Clause. Built by CMakeLists.txt from
# cmake/softfloat_sources.cmake.
set -eu
COMMIT=a0c6494cdc11865811dec815d5c0049fba9d82a8
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/extern/softfloat"
[ -d "$DEST/.git" ] || git clone https://github.com/ucb-bar/berkeley-softfloat-3.git "$DEST"
git -C "$DEST" fetch --depth 1 origin "$COMMIT" 2>/dev/null || true
git -C "$DEST" checkout --detach "$COMMIT"
