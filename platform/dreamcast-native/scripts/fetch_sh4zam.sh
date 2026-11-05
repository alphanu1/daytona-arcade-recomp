#!/usr/bin/env sh
# Fetch SH4ZAM (SH4 math library) at the pinned commit into extern/sh4zam (git-ignored) and build its static
# library there with KOS's toolchain (environ.sh; extern/sh4zam/lib/libsh4zam.a), so the build does not depend on the copy installed in KOS's addons.
# v0.10.0 (70dbb17): the apply_* matrix names are deprecated (rotate_*, add_translation, scale_3x3), the vector and
# quaternion magnitude forms check for zero (the *_fsrra forms are the unchecked ones), shz_memcpy sends short or
# source-misaligned copies to the C library, and the SH4 dot products use scalar FPU code unless SHZ_SH4_DOT_FIPR
# is defined (platform/dreamcast-native/local.cfg.mk defines it: the renderer's bounds and face checks mirror FIPR).
# v0.9.0 (0fd3a1e) was the release the renderer was written and measured with.
set -eu
SH4ZAM_COMMIT=70dbb177b20c1734fbe4f97df0f8c6ec87a8e8d3
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd -P)"
DEST="$ROOT/extern/sh4zam"
[ -d "$DEST/.git" ] || git clone https://github.com/gyrovorbis/sh4zam.git "$DEST"
git -C "$DEST" fetch --depth 1 origin "$SH4ZAM_COMMIT" 2>/dev/null || true
git -C "$DEST" checkout --detach "$SH4ZAM_COMMIT"
kos-cmake -S "$DEST" -B "$DEST/build" -DSHZ_ENABLE_TESTS=off -DSHZ_SAVE_TEMPS=off >/dev/null
cmake --build "$DEST/build" --target sh4zam
mkdir -p "$DEST/lib"
cp "$DEST/build/libsh4zam.a" "$DEST/lib/libsh4zam.a"
