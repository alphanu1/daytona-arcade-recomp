#!/usr/bin/env python3
"""Cross-check our i960 decoder against Ghidra's SLEIGH decode, using the
third-party mumbel/ghidra_i960 processor module (Apache-2.0) through pypcode.

  ghidra_oracle.py <build-dir> <ghidra_i960-dir> [--random N] [--words FILE]

Only words our decoder marks executable (MAME's i960KB executor set) are
compared. For each, Ghidra must decode it, and these must agree:
mnemonic, length, direct branch target, and operand values (registers and
numbers, parsed out of both syntaxes). Differences are grouped by mnemonic
and kind, with an example of each. Exit 1 on any difference.

This is a second, independent reference: MAME's disassembler and executor
are written by the same project, SLEIGH is not.
"""

import argparse
import collections
import os
import random
import re
import subprocess
import sys
import tempfile

try:
    import pypcode
except ImportError:
    print("ghidra_oracle: SKIP (pip install pypcode)")
    sys.exit(77)

TOKEN = re.compile(r"-?0x[0-9a-f]+|[a-z]+[0-9]*|[0-9]+|\+[01]\.0")
# Ops whose src1 is an integer in MAME's executor and in SLEIGH, but which
# MAME's disassembler prints as an FP register or literal.
MAME_INT_SRC1_AS_FP = {"cvtir", "cvtilr", "scaler", "scalerl"}
# Register spellings that differ between the two syntaxes.
ALIAS = {"r0": "pfp", "r1": "sp", "r2": "rip", "g15": "fp"}


def operands(text, addr=0):
    """Operand tokens as a list of canonical strings: registers by name,
    numbers as integers."""
    out = []
    # Ghidra prints MEMB mode 5 as "<disp+8> (ip)"; MAME prints the address.
    text = re.sub(r"(-?0x[0-9a-f]+)\s*\(ip\)", lambda mo: hex((int(mo.group(1), 16) + addr) & 0xFFFFFFFF), text.lower())
    # Ghidra spells an unscaled index "[g12 * 0x1]"; MAME omits the scale.
    text = re.sub(r"\s*\*\s*0x1\s*\]", "]", text.lower())
    for t in TOKEN.findall(text):
        if t.lstrip("-").startswith("0x"):
            out.append(str(int(t, 16) & 0xFFFFFFFF))  # -0x8 == 0xfffffff8
        elif t.isdigit():
            out.append(str(int(t)))
        else:
            out.append(ALIAS.get(t, t))
    return out


def ours(build, words):
    with tempfile.NamedTemporaryFile("w", delete=False, suffix=".txt") as f:
        for a, w0, w1 in words:
            f.write(f"{a:x} {w0:x} {w1:x}\n")
        path = f.name
    out = subprocess.run([os.path.join(build, "i960dis"), "--words", path],
                         capture_output=True, text=True, check=True).stdout
    os.unlink(path)
    res = []
    for line in out.splitlines():
        addr, length, exe, tgt, canon, mnem, text = line.split("\t")
        # MAME pads the mnemonic to 8 columns with no separator after a longer
        # one ("scanbyteg9,5"), so cut by the known mnemonic, not by a space.
        ops = text[len(mnem):] if text.startswith(mnem) else text
        res.append(dict(length=int(length), exe=exe == "1", canon=canon == "1",
                        target=None if tgt == "-" else int(tgt, 16),
                        mnem=mnem, ops=ops.strip()))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("ghidra_i960")
    ap.add_argument("--random", type=int, default=200000)
    ap.add_argument("--words", help="file of 'addr word0 word1' hex lines (e.g. real code)")
    ap.add_argument("--seed", type=int, default=960)
    args = ap.parse_args()

    ldefs = os.path.join(args.ghidra_i960, "data", "languages", "i960.ldefs")
    sla = os.path.join(os.path.dirname(ldefs), "i960.sla")
    if not os.path.exists(sla):
        sleigh = os.path.join(os.path.dirname(pypcode.__file__), "bin", "sleigh")
        subprocess.run([sleigh, sla.replace(".sla", ".slaspec"), sla], check=True, capture_output=True)
    lang = [l for l in pypcode.Arch("i960", ldefs).languages if l.id == "i960:LE:32:default"][0]
    ctx = pypcode.Context(lang)

    rng = random.Random(args.seed)
    words = [(rng.getrandbits(30) << 2, rng.getrandbits(32), rng.getrandbits(32)) for _ in range(args.random)]
    label = f"{args.random} random"
    if args.words:
        real = []
        for line in open(args.words):
            p = line.split()
            if len(p) >= 3:
                real.append((int(p[0], 16), int(p[1], 16), int(p[2], 16)))
        words += real
        label += f" + {len(real)} from {os.path.basename(args.words)}"

    mine = ours(args.build, words)
    diffs = collections.Counter()
    example = {}
    compared = ghidra_extra = 0
    reserved = collections.Counter()
    for (addr, w0, w1), m in zip(words, mine):
        blob = w0.to_bytes(4, "little") + w1.to_bytes(4, "little")
        try:
            dx = ctx.disassemble(blob, base_address=addr, max_instructions=1)
            g = dx.instructions[0] if dx.instructions else None
        except Exception:
            g = None
        if not m["exe"]:
            if g is not None and m["length"] == 0:
                ghidra_extra += 1
            continue
        if not m["canon"]:
            # Bits the KB reserves: MAME's executor ignores them, Ghidra's
            # module (which also models later parts) may not. Counted apart.
            reserved[g is not None] += 1
            continue
        compared += 1
        kinds = []
        if g is None:
            # Known reference conflict: MAME executes movre at 0x6e9 (and the
            # undocumented 0x6e1); the SLEIGH module decodes only 0x6e1.
            kinds.append("movre-0x6e9-sleigh-lacks" if m["mnem"] == "movre" and ((w0 >> 7) & 0xf) == 9
                         else "ghidra-rejects")
        else:
            if g.mnem.lower() != m["mnem"]:
                kinds.append(f"mnemonic:{g.mnem.lower()}")
            if g.length != m["length"]:
                kinds.append(f"length:{g.length}")
            if not kinds:
                gops = operands(g.body, addr)
                mops = operands(m["ops"])
                if m["target"] is not None and str(m["target"]) not in gops:
                    kinds.append("target")
                elif gops != mops:
                    if m["mnem"] in MAME_INT_SRC1_AS_FP and re.search(r"fp[0-3]|\+[01]\.0|\?", m["ops"]):
                        # Known: MAME's disassembler names src1 as an FP
                        # register/literal, but its executor (get_1_ri) and
                        # SLEIGH read an integer. Text only, not decode.
                        kinds.append("mame-text-int-src1-as-fp")
                    else:
                        kinds.append("operands")
        for k in kinds:
            key = (m["mnem"], k)
            diffs[key] += 1
            example.setdefault(key, f"@{addr:08x} {w0:08x} {w1:08x}  ours [{m['mnem']} {m['ops']}]  "
                                    f"ghidra [{'' if g is None else g.mnem + ' ' + g.body}]")

    print(f"ghidra_oracle: {label} words; {compared} executable compared; "
          f"{sum(diffs.values())} differences in {len(diffs)} groups; "
          f"{ghidra_extra} words Ghidra decodes that have no mnemonic in our table; "
          f"{sum(reserved.values())} executable words set KB-reserved bits "
          f"(Ghidra decodes {reserved[True]}, rejects {reserved[False]}; not compared)")
    for key, n in diffs.most_common():
        print(f"  {n:8d}  {key[0]:10s} {key[1]:22s} e.g. {example[key]}")
    known = {k for k in diffs if k[1] in ("mame-text-int-src1-as-fp", "movre-0x6e9-sleigh-lacks")}
    return 1 if set(diffs) - known else 0


if __name__ == "__main__":
    sys.exit(main())
