#!/usr/bin/env python3
"""Compare NATIVE_FRAME_COMPARE RGB24 captures; nonzero exit means NOT pixel-identical.

Usage: python3 check_frame_pixels.py baseline-mount candidate-mount
No tolerance or image resampling; fixed ticks 600/1200/1800, 640x480 RGB24.
"""
import hashlib
from pathlib import Path
import sys

def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    different = False
    for tick in (600, 1200, 1800):
        name = f"frame-{tick:04d}.rgb"
        a, b = (Path(root, name).read_bytes() for root in sys.argv[1:])
        if len(a) != 640 * 480 * 3 or len(b) != len(a):
            raise SystemExit(f"FAIL: {name}: incomplete RGB24 frame")
        changed = [i for i in range(640 * 480) if a[i*3:i*3+3] != b[i*3:i*3+3]]
        delta = [abs(x-y) for x, y in zip(a, b)]
        bbox = None if not changed else (
            min(i % 640 for i in changed), min(i // 640 for i in changed),
            max(i % 640 for i in changed), max(i // 640 for i in changed))
        interior = sum(4 <= i % 640 < 636 and 4 <= i // 640 < 476 for i in changed)
        print(f"tick={tick} changed={len(changed)}/307200 interior={interior} "
              f"max_channel_delta={max(delta)} mean_channel_delta={sum(delta)/len(delta):.6f} bbox={bbox}")
        print(f"  baseline={hashlib.sha256(a).hexdigest()} candidate={hashlib.sha256(b).hexdigest()}")
        different |= bool(changed)
    return int(different)

if __name__ == "__main__":
    sys.exit(main())
