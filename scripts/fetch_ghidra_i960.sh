#!/usr/bin/env sh
# Fetch the third-party Ghidra i960 processor module (mumbel/ghidra_i960,
# Apache-2.0) at the pinned commit into extern/ghidra_i960 (git-ignored).
# Mainline Ghidra has no i960 module. Used by tests/ghidra_oracle.py through
# pypcode (pip install pypcode), and loadable into Ghidra itself by copying the
# directory into <ghidra>/Ghidra/Processors/.
set -eu
COMMIT=727ef7872c5b1cd6ceb5a81f5e474d1ced92945c
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/extern/ghidra_i960"
[ -d "$DEST/.git" ] || git clone https://github.com/mumbel/ghidra_i960.git "$DEST"
git -C "$DEST" fetch --depth 1 origin "$COMMIT" 2>/dev/null || true
git -C "$DEST" checkout --detach "$COMMIT"
