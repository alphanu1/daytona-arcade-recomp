"""Extract bounded daytona93 vehicle parameters/math tables into a NEW DND27 file.

ROM-derived data stays ignored/local. No sampled poses or generated program.
"""
import argparse
import hashlib
import math
from pathlib import Path
import struct

from inspect_course import IMAGES, MAIN_BASE, words


def f32(value):
    return struct.unpack("<f",struct.pack("<f",value))[0]


def tgp_reciprocal(tables, divisor_bits):
    """Offline command3 refinement, matching force_math.h's table semantics."""
    if len(tables) != 0x40000:
        raise ValueError("incorrect TGP table image size")
    def entry(second):
        index = 0x8000 | ((divisor_bits >> 9) & 0x3ffe) | second
        bits = struct.unpack_from("<I",tables,4*index)[0]
        exponent = ((bits >> 23) + 127 - ((divisor_bits >> 23) & 255)) & 255
        bits = (bits & 0x7fffff) | (exponent << 23) | ((divisor_bits & 0x80000000) if second else 0)
        return struct.unpack("<f",struct.pack("<I",bits))[0]
    normalized = struct.unpack("<f",struct.pack("<I",(divisor_bits & 0x807fffff) | 0x3f800000))[0]
    return f32(entry(1)-f32(normalized*entry(0)))


def prepare(main, tables, program):
    # Scalars, four gears, two67-point curves, three512-point drag tables.
    # Preserve source bits, including zeros; division/force math remains live.
    values = []
    for address, count in ((0x28b57bc,2),(0x28b5788,1),(0x28b57b0,2),(0x28b5820,4),
                           (0x28b8360,67),(0x28b8120,67),(0x28b5d20,512),
                           (0x28b6520,512),(0x28b6d20,512)):
        values.extend(words(main,MAIN_BASE,address,count))
    if len(tables) != 0x40000:
        raise ValueError("incorrect TGP table image size")
    # MAME reciprocal table-port exponent adjustment (same BSD-3 provenance
    # as runtime/tgp.cpp; R. Belmont, Olivier Galibert, ElSemi, Angelo Salese).
    # The actual TGP command refines two table entries then multiplies by the
    # numerator. Here its denominator is always2000: fold only that static work.
    reciprocal = tgp_reciprocal(tables,0x44fa0000)
    axle = []
    for address,count in ((0x28b5790,2),(0x28b5770,1),(0x28b5784,1),
                          (0x28b57e0,2),(0x28b57c4,2),(0x28b57cc,2)):
        axle.extend(words(main,MAIN_BASE,address,count))
    force = words(main,MAIN_BASE,0x28b5760,1) + words(main,MAIN_BASE,0x28b57fc,1)
    force += words(program,0,0xf9c0,4)
    force += words(main,MAIN_BASE,0x28b5774,1)
    force += words(main,MAIN_BASE,0x28b57a8,2) + words(main,MAIN_BASE,0x28b5780,1)
    force += words(main,MAIN_BASE,0x28b576c,1) + words(main,MAIN_BASE,0x28b578c,1)
    force += words(main,MAIN_BASE,0x28b57e8,2) + words(main,MAIN_BASE,0x28b57d4,1)
    force += words(main,MAIN_BASE,0x28b5850,4)
    force += words(main,MAIN_BASE,0x28b5800,5)
    # Spin starts at32 (16cf4);15fc4 counts down. Not a captured motion curve.
    force += words(program,0,0xebc4,32) + words(main,MAIN_BASE,0x28b5758,1)
    force += words(program,0,0x34ac4,12)  # Four local wheel XYZs, not world poses.
    force += words(main,MAIN_BASE,0x28b5b00,1)
    # Actual player compares rank/lap with itself. Fold the byte-indexed
    # clamp/table lookup offline; opponents do NOT use this identity shortcut.
    bounds = struct.pack("<128I",*words(program,0,0x34b88,128))
    lap_word = words(program,0,0x34b54,1)[0]
    lap = struct.unpack("<b",bytes(((lap_word>>16)&255,)))[0]
    middle = 0x28b5b10+(words(main,MAIN_BASE,0x28b5d10,1)[0]>>1)
    # Negative speed indices (include/drive.h nm_drive_drag_bits): ef08 reads the
    # words before the drag table 0x28b5d20 and 10464/10474 the erased words at
    # 0x28b6520/0x28b6d20 + 4*(65536+index). The native code takes indices
    # -131..-5 from guided_targets below, -4 as 0x200 and -3..-1 as 0xffffffff;
    # nothing is added to the pack, so check that the ROM agrees.
    if (middle-63*4 != 0x28b5d20-131*4 or words(main,MAIN_BASE,0x28b5d10,1)[0] != 0x200 or
            words(main,MAIN_BASE,0x28b5d14,3) != (0xffffffff,)*3 or
            any(w != 0xffffffff for base in (0x28b6520,0x28b6d20)
                for w in words(main,MAIN_BASE,base+4*(65536-131),131))):
        raise ValueError("negative speed index drag words moved")
    for mode in range(256):
        low, high = struct.unpack_from("<2b",bounds,2*mode)
        rank = low if 0 < low else high if 0 > high else 0
        rank = max(-63,min(63,rank+lap))
        force += words(main,MAIN_BASE,middle+4*rank,1)
    thresholds = words(main,MAIN_BASE,0x28b5830,8)
    # Program alias00234xxx maps to file offset00034xxx, not work RAM.
    limits = words(program,0,0x34bc8,8)
    selectors = struct.pack("<2I",*words(program,0,0x34abc,2))
    signed_thresholds = struct.unpack("<8i",struct.pack("<8I",*thresholds))
    signed_limits = struct.unpack("<8i",struct.pack("<8I",*limits))
    if (any(signed_thresholds[i] > signed_thresholds[i+4] for i in range(4)) or
            any(signed_limits[2*i] > signed_limits[2*i+1] for i in range(4)) or
            any(v < -1 or v > 3 for v in struct.unpack("<8b",selectors))):
        raise ValueError("invalid gear thresholds, limits or selector mapping")
    payload = (struct.pack(f"<{len(values)}I",*values) + struct.pack("<f",reciprocal) +
               struct.pack("<10I",*axle) + struct.pack("<326I",*force) +
               struct.pack("<16I",*(thresholds+limits)) + selectors + tables)
    assert len(payload) == 270280
    coefficients = struct.unpack("<2016f",payload[:8064])
    sine = struct.unpack("<16384f",tables[:65536])
    inverse = struct.unpack("<32768f",tables[131072:])
    if (not all(math.isfinite(v) for v in coefficients + inverse) or
            not all(math.isfinite(v) and 0 <= v <= 1 for v in sine)):
        raise ValueError("invalid drive coefficients or sine table")
    if struct.unpack_from("<f",payload,7036)[0] < 0:
        raise ValueError("negative guided adjustment limit")
    models = [words(program,0,0x35b5c+4*curve+8*damage,1)[0]
              for curve in range(2) for damage in range(16)]
    thresholds = []
    for curve in range(2):
        for table in range(2):
            pointer = words(program,0,0x16d7c+8*table+4*curve,1)[0]
            thresholds.extend(words(program,0,pointer-0x200000,16))
    sequences = []
    for at in (0x2896e58,0x2896e5c):
        directory = words(main,MAIN_BASE,at,1)[0]
        if not words(main,MAIN_BASE,directory,1)[0]:
            raise ValueError("empty crash sequence directory")
        #15e84 explicitly selects variant0. Both entry paths share that sequence.
        sequence = words(main,MAIN_BASE,directory+4,1)[0]
        if words(main,MAIN_BASE,sequence,1)[0] != 257:
            raise ValueError("unexpected crash sequence length")
        rows = [words(main,MAIN_BASE,sequence+8+16*i,3) for i in range(257)]
        sequences.append(b"".join(struct.pack("<3I",*row) for row in rows))
    if sequences[0] != sequences[1]:
        raise ValueError("crash entry sequences are no longer identical")
    payload += struct.pack("<32I64I",*models,*thresholds) + sequences[0]
    assert len(payload) == 273748
    reset = words(main,MAIN_BASE,0x28b5710,3) + words(main,MAIN_BASE,0x28b5724,2) + words(main,MAIN_BASE,0x28b5730,1)
    angles = struct.pack("<2I",*words(main,MAIN_BASE,0x28b571c,2))[:6]
    payload += struct.pack("<6I",*reset) + angles + b"\0\0"
    assert len(payload) == 273780
    #16718 uses LDOB, so the signed lower-bound check cannot narrow0..255.
    #Keep the complete numeric lookup, including zero entries; runtime rejects
    #an actually attempted zero division rather than using the oracle's hack.
    for pointer in words(program,0,0x35cfc,4):
        payload += struct.pack("<256I",*words(program,0,pointer-0x200000,256))
    sides = struct.pack("<I",*words(program,0,0x35a34,1))
    if any(v > 1 for v in sides):
        raise ValueError("invalid pivot side directions")
    payload += sides
    assert len(payload) == 277880
    speed_scale, drop = struct.unpack("<2f",struct.pack("<2I",
        *words(main,MAIN_BASE,0x28b5740,1),*words(main,MAIN_BASE,0x28b573c,1)))
    drop = f32(drop*0.25)
    if not math.isfinite(speed_scale) or not math.isfinite(drop):
        raise ValueError("invalid early-player speed coefficients")
    payload += struct.pack("<2f",speed_scale,drop)
    # Original halfword tables may be read before their nominal starts. Keep
    # every byte-indexed value, not a guessed reachable positive timer range.
    for base in (0x35140,0x35180,0x3573c,0x3561c):
        for timer in range(256):
            index = (timer if timer < 128 else timer-256) if base < 0x35600 else 144-timer
            at = base+2*index
            if at < 0 or at+2 > len(program):
                raise ValueError("early-player table outside program image")
            payload += program[at:at+2]
    assert len(payload) == 279936
    if words(program,0,0,1)[0] != 0:
        raise ValueError("collision default selector is no longer zero")
    #15238 stores only the low byte of each authored halfword, second then first.
    for base in (0x35b18,0x35b2c):
        payload += struct.pack("<5I",*words(program,0,base,5))[::2]
    assert len(payload) == 279956
    payload += struct.pack("<6I",*words(program,0,0x14fb4,6))
    # SETBIT uses low5 index bits. Fold immutable shifts; retain every uint16
    # threshold without imposing a new ordering on the authored branches.
    payload += struct.pack("<8I",*[1 << (v&31) for v in words(program,0,0x35000,8)])
    assert len(payload) == 280012
    raw_step = words(main,MAIN_BASE,0x28b573c,1)[0]
    if not math.isfinite(struct.unpack("<f",struct.pack("<I",raw_step))[0]):
        raise ValueError("invalid collision speed step")
    payload += struct.pack("<I",raw_step)
    corners = words(program,0,0x35b08,4)
    if any(offset < 0xf4 or offset > 0x118 or (offset-0xf4)%12 for offset in corners):
        raise ValueError("collision corner outside native wheel points")
    indices = [(offset-0xf4)//12 for offset in corners]
    if sorted(indices) != list(range(4)):
        raise ValueError("collision corners are not a permutation")
    #1469c/147c0 countdown table must be the inverse corner map, not an
    #independent runtime copy. Verify all four entries before folding it.
    if words(program,0,0x35af8,4)[::-1] != tuple(indices.index(v) for v in range(4)):
        raise ValueError("collision point/corner maps disagree")
    payload += bytes(indices)
    payload += struct.pack("<4H",*[v&65535 for v in words(program,0,0x35b44,4)[::-1]])
    schedule = struct.unpack_from("<56b",program,0x13ec4)
    for row in range(4):
        values = schedule[14*row:14*row+14]
        if sorted(values) != list(range(-5,9)):
            raise ValueError("collision schedule is not a fourteen-car permutation")
        payload += bytes(v+5 for v in values)
    #1550c reuses the existing signed-byte shake table. Only its last two
    #positive-index neighbours and the four-step boost cycle are missing.
    payload += struct.pack("<3I",*words(program,0,0x35280,1),*words(program,0,0x35b54,2))
    assert len(payload) == 280096
    payload += struct.pack("<127I",*words(main,MAIN_BASE,middle-63*4,127))
    payload += bounds + program[0x34b56-43:0x34b56+44] + b"\0"
    assert len(payload) == 281204
    payload += program[0x3543c-512:0x3543c+512]
    assert len(payload) == 282228
    for mode in range(256):
        payload += struct.pack("<I",words(program,0,0x35290+4*(mode if mode<128 else mode-256),1)[0])
    payload += struct.pack("<256I",*words(program,0,0x35284,256))
    assert len(payload) == 284276
    payload += program[0xcd30:0xcde0]
    assert len(payload) == 284452
    return struct.pack("<4I",0x52444e44,len(payload)+16,0,0) + payload


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images",type=Path)
    parser.add_argument("output",type=Path)
    args = parser.parse_args()
    expected = dict(IMAGES, **{"copro_tables.bin": (0x40000,
        "3163343896150d46e3280f71eb276cfeabd9190602c0bfdd53a6cb18d91f0b04")})
    images = {}
    for name,(size,digest) in expected.items():
        data = (args.images/name).read_bytes()
        if len(data) != size or hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"unrecognized daytona93 image: {name}")
        images[name] = data
    data = prepare(images["main_data.bin"],images["copro_tables.bin"],images["program.bin"])
    with args.output.open("xb") as output:
        output.write(data)
    print(f"DND27 {len(data)} bytes, payload284452 (math262144 + parameters22308), no VRAM; SHA256 {hashlib.sha256(data).hexdigest()}")
