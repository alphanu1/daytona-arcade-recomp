#!/usr/bin/env sh
# Fetch AICAflow (Dreamcast AICA audio runtime) and its authoring toolchain
# AICAforge at the pinned commits into extern/aicaflow (git-ignored). MIT.
# AICAforge is AICAflow's own pinned submodule (dependencies/AICAforge);
# AICAflow's authoring-dependencies target initialises it and AICAforge's
# pinned AICAflow SDK non-recursively, as AICAflow's docs/dependencies.md asks.
# AICAflow f448121 (firmware ABI 8: status reported by interrupt, no polling) was origin main on 16 October 2026;
# AICAforge e77de852 (origin main on 15 October) is still its pin. The host library (driver/sh4, built by the
# platform/dreamcast-native Makefile) and firmware/aicaflow.drv come from the same checkout and must match.
set -eu
AICAFLOW_COMMIT=f4481210a8a9fa8248a74b6e6e56da38c0df5c61
AICAFORGE_COMMIT=e77de85282035a679887f3166cf7caf6a0d61b16
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd -P)"
DEST="$ROOT/extern/aicaflow"
[ -d "$DEST/.git" ] || git clone https://github.com/dfchil/aicaflow.git "$DEST"
git -C "$DEST" fetch --depth 1 origin "$AICAFLOW_COMMIT" 2>/dev/null || true
git -C "$DEST" checkout --detach "$AICAFLOW_COMMIT"
make -C "$DEST" authoring-dependencies
FORGE="$DEST/dependencies/AICAforge"
if [ "$(git -C "$FORGE" rev-parse HEAD)" != "$AICAFORGE_COMMIT" ]; then
    git -C "$FORGE" fetch origin "$AICAFORGE_COMMIT"
    git -C "$FORGE" checkout --detach "$AICAFORGE_COMMIT"
    make -C "$FORGE" dependencies
fi
