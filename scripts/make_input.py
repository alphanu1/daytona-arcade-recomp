#!/usr/bin/env python3
"""Write a scripted M2IN input stream (docs/trace-format.md).

  make_input.py HEADER.m2in SCRIPT OUT.m2in

HEADER is any input stream recorded on the same machine
(M2TRACE_RECORD_INPUT); only its field layout is used, so the scripted stream
matches the machine's fields exactly. SCRIPT is a text file:

  frames N                  total frames to write
  # comment
  <from>-<to> <name>=<v>    hold a control over a frame range (inclusive)
  <at> <name>=<v>           one frame only

Names: coin, service, test, start, vr1..vr4 (1 = pressed), gear0..gear4
(1 = selected), steer, accel, brake (raw 0x20-0xe0 ADC values). Unscripted
controls rest at their recorded defaults. Later lines override earlier ones.
"""

import struct
import sys

# name -> (port, mask); digital unless listed in ANALOG.
CONTROLS = {
    "coin": (":IN0", 0x01), "service": (":IN0", 0x08), "test": (":IN0", 0x04),
    "start": (":IN0", 0x10),
    "vr1": (":IN0", 0x20), "vr2": (":IN0", 0x40), "vr3": (":IN0", 0x80), "vr4": (":IN1", 0x01),
    "gear0": (":GEARS", 0x01), "gear1": (":GEARS", 0x02), "gear2": (":GEARS", 0x04),
    "gear3": (":GEARS", 0x08), "gear4": (":GEARS", 0x10),
    "steer": (":STEER", 0xFF), "accel": (":ACCEL", 0xFF), "brake": (":BRAKE", 0xFF),
}
ANALOG = {"steer", "accel", "brake"}


def read_header(blob):
    assert blob[:4] == b"M2IN" and struct.unpack_from("<I", blob, 4)[0] == 1, "not an M2IN v1 stream"
    p = 8
    n = struct.unpack_from("<H", blob, p)[0]
    head_start = p
    p += 2 + n  # game
    nf = struct.unpack_from("<H", blob, p)[0]
    p += 2
    fields = []
    for _ in range(nf):
        ln = struct.unpack_from("<H", blob, p)[0]
        port = blob[p + 2:p + 2 + ln].decode()
        p += 2 + ln
        mask, defv, analog = struct.unpack_from("<IIB", blob, p)
        p += 9
        fields.append((port, mask, defv, bool(analog)))
    return blob[:p], fields


def main():
    header_path, script_path, out_path = sys.argv[1:4]
    header, fields = read_header(open(header_path, "rb").read())
    index = {(port, mask): i for i, (port, mask, _, _) in enumerate(fields)}

    total = 0
    events = []  # (from, to, field index, raw value)
    for lineno, line in enumerate(open(script_path), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if parts[0] == "frames":
            total = int(parts[1])
            continue
        rng, assign = parts
        lo, _, hi = rng.partition("-")
        lo, hi = int(lo), int(hi or lo)
        name, _, val = assign.partition("=")
        if name not in CONTROLS:
            sys.exit(f"{script_path}:{lineno}: unknown control {name}")
        port, mask = CONTROLS[name]
        fi = index[(port, mask)]
        _, _, defv, analog = fields[fi]
        v = int(val, 0)
        if analog:
            raw = v & mask
        else:  # digital: pressed flips the field away from its default
            raw = (defv ^ mask) if v else defv
        events.append((lo, hi, fi, raw))

    if total <= 0:
        sys.exit("script needs 'frames N'")
    frames = []
    for f in range(total):
        row = [defv for (_, _, defv, _) in fields]
        for lo, hi, fi, raw in events:
            if lo <= f <= hi:
                row[fi] = raw
        frames.append(struct.pack("<%dI" % len(row), *row))
    with open(out_path, "wb") as out:
        out.write(header)
        out.write(b"".join(frames))
    print(f"{out_path}: {total} frames, {len(fields)} fields, {len(events)} scripted ranges")


if __name__ == "__main__":
    main()
