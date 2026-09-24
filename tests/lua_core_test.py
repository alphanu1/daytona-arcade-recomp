#!/usr/bin/env python3
"""Cross-language check of the MAME plugin's pure Lua core against the C++
trace library, without MAME or any ROM.

  lua_core_test.py <build-dir> <scratch-dir>

Needs a Lua 5.4 runtime through the `lupa` package (MAME bundles Lua 5.4).
1. The Lua region hash equals an independent Python FNV-1a-64 over words.
2. A trace encoded by core.lua is byte-identical to the one the C++ Writer
   produces (test_trace --reference), and tracediff reports them identical.
3. One changed register in the Lua trace makes tracediff report exactly it.
4. The input-stream codec round-trips, and field_value decodes digital
   (active-low) and analog fields.
"""

import os
import random
import subprocess
import sys

try:
    import lupa.lua54 as lupa
except ImportError:
    print("lua_core_test: SKIP (pip install lupa for a Lua 5.4 runtime)")
    sys.exit(77)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE = os.path.join(ROOT, "tools", "mame-plugins", "m2trace", "core.lua")

failures = 0
checks = 0


def check(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("FAIL:", what)


def fnv_words(data):
    h = 0xCBF29CE484222325
    for i in range(0, len(data), 4):
        h ^= int.from_bytes(data[i:i + 4], "little")
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def pattern(n, seed):
    # Must match tests/test_trace.cpp: bytes i*7+3+seed.
    return bytes((i * 7 + 3 + seed) & 0xFF for i in range(n))


def main():
    build, scratch = sys.argv[1], sys.argv[2]
    os.makedirs(scratch, exist_ok=True)
    lua = lupa.LuaRuntime(encoding=None)
    with open(CORE, "rb") as f:
        core = lua.execute(f.read())

    # 1. Hash against Python, across sizes either side of the 1 KiB block.
    rng = random.Random(960)
    for n in (0, 4, 8, 1020, 1024, 1028, 4096, 65536 + 12):
        data = bytes(rng.getrandbits(8) for _ in range(n))
        got = core.hash_words(data) & 0xFFFFFFFFFFFFFFFF
        check(got == fnv_words(data), f"hash of {n} bytes: lua {got:016x} python {fnv_words(data):016x}")

    # 2. The reference trace, encoded in Lua.
    def sample(epoch, seed, tweak_reg=None):
        mem = pattern(4096, seed)
        regions = lua.table_from([
            lua.table_from([0x00200000, 4096, core.hash_words(mem)]),
            lua.table_from([0x00500000, 16, core.hash_words(mem[:16])]),
        ])
        regs = [(i * 0x01010101 + epoch) & 0xFFFFFFFF for i in range(36)]
        if tweak_reg is not None:
            regs[tweak_reg] ^= 1
        return core.rec_sample(epoch, 1000 + epoch, regions, lua.table_from(regs))

    def lua_trace(tweak_reg=None):
        parts = [
            core.trace_header(b"daytona", b"reference", b"vblank-ack"),
            core.rec_access(True, 0x00884000, 0x3F800000, 0xFFFFFFFF),
            core.rec_access(False, 0x00884000, 0xDEADBEEF, 0xFFFFFFFF),
            core.rec_note(b"reset"),
            core.rec_access(True, 0x00E80000, 0xFFFFFFFE, 0xFFFFFFFF),
            sample(0, 0),
            core.rec_access(True, 0x01C80000, 0x00000041, 0x000000FF),
            sample(1, 5, tweak_reg),
        ]
        return b"".join(parts)

    lua_path = os.path.join(scratch, "lua.m2tr")
    ref_path = os.path.join(scratch, "ref.m2tr")
    with open(lua_path, "wb") as f:
        f.write(lua_trace())
    subprocess.run([os.path.join(build, "test_trace"), "--reference", ref_path], check=True)
    with open(ref_path, "rb") as f:
        ref = f.read()
    check(lua_trace() == ref, "Lua-encoded trace is byte-identical to the C++ reference")

    tracediff = os.path.join(build, "tracediff")
    r = subprocess.run([tracediff, "--compare-ip", "--compare-frame", lua_path, ref_path],
                       capture_output=True, text=True)
    check(r.returncode == 0 and "identical: 2 epochs, 4 events" in r.stdout,
          "tracediff: Lua vs C++ identical\n" + r.stdout)

    # 3. One register flipped (g1, index 17) at epoch 1.
    with open(lua_path, "wb") as f:
        f.write(lua_trace(tweak_reg=17))
    r = subprocess.run([tracediff, lua_path, ref_path], capture_output=True, text=True)
    check(r.returncode == 1 and "DIVERGED at epoch 1" in r.stdout and "g1:" in r.stdout,
          "tracediff reports the flipped register\n" + r.stdout)

    # 4. Input codec.
    fields = lua.table_from([
        lua.table_from({b"port": b":IN0", b"mask": 0x10, b"defvalue": 0x10, b"analog": False}),
        lua.table_from({b"port": b":STEER", b"mask": 0xFF, b"defvalue": 0x80, b"analog": True}),
        lua.table_from({b"port": b":X", b"mask": 0xFF00, b"defvalue": 0, b"analog": True}),
    ])
    frames = [[0x10, 0x80, 0x1200], [0x00, 0xE0, 0xFF00], [0x10, 0x20, 0]]
    blob = core.input_header(b"daytona", fields) + b"".join(
        core.input_frame(lua.table_from(fr)) for fr in frames)
    header, parsed = core.input_parse(blob)
    check(header is not None, "input stream parses")
    got = [[parsed[i + 1][j + 1] for j in range(3)] for i in range(len(parsed))]
    check(got == frames, f"input frames round-trip: {got}")
    hf = header[b"fields"]
    check(hf[2][b"port"] == b":STEER" and hf[2][b"analog"] is True, "field header")
    f_in0, f_steer, f_x = hf[1], hf[2], hf[3]
    check(core.field_value(f_in0, 0x10) == 0, "START released (active-low bit high)")
    check(core.field_value(f_in0, 0x00) == 1, "START pressed (active-low bit low)")
    check(core.field_value(f_steer, 0xE0) == 0xE0, "steer full right")
    check(core.field_value(f_x, 0x1200) == 0x12, "analog field above bit 0 is shifted down")
    bad, msg = core.input_parse(blob[:-1])
    check(bad is None and b"whole frames" in msg, "truncated input stream is rejected")

    print(f"lua_core_test: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
