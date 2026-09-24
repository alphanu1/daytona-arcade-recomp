#!/usr/bin/env sh
# Fetch MAME at the pinned commit into extern/mame (git-ignored).
# Only the paths the tools and tests read are checked out.
set -eu
MAME_COMMIT=dddd73680656e355bb2b5beecab1167c9f07bf81
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/extern/mame"

if [ ! -d "$DEST/.git" ]; then
    git clone --filter=blob:none --no-checkout https://github.com/mamedev/mame.git "$DEST"
fi
cd "$DEST"
git sparse-checkout set --no-cone \
    /src/devices/cpu/i960/ \
    /src/mame/sega/model2.cpp /src/mame/sega/model2.h \
    /src/mame/sega/model2_v.cpp /src/mame/sega/model2_m.cpp \
    /src/mame/shared/segam1audio.cpp /src/mame/shared/segam1audio.h
git fetch --depth 1 origin "$MAME_COMMIT"
git checkout --detach "$MAME_COMMIT"

# Our patches (patches/mame, recorded in THIRD_PARTY.md). Skipped if already applied.
for p in "$ROOT"/patches/mame/*.patch; do
    [ -e "$p" ] || continue
    if git apply --check "$p" 2>/dev/null; then
        git apply "$p"
        echo "applied $(basename "$p")"
    fi
done
