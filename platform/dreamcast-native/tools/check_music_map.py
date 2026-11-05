"""Check include/audio/music_map.h: each bank-0x10 command plays the recording the map gives it.

    python3 platform/dreamcast-native/tools/check_music_map.py ROMS.zip ARCHIVE.zip WORK_DIR

ROMS.zip: your own daytona93 set; Revision A's sound program (epr-16720.7, epr-16721.8) is read from it
(CRC-checked, assembled like src/runtime/rom_import.cpp) into WORK_DIR, never elsewhere.
ARCHIVE.zip: vgmrips' Daytona USA pack (recipes/build_music.sh's input).
For ae1000..ae102f the native sequencer (tools/music_notes.cpp over src/runtime/native_sound_sequencer.cpp,
built into WORK_DIR) plays the command after the game's boot bytes; its music-chip note-ons (sample number,
pitch) over 60 s are compared with each VGM log's MultiPCM key-ons, decoded as the reference board's
MultiPCM sees them (tests/test_native_sound_oracle_rom.cpp). A mapped command must match its recording
(multiset similarity >= 0.99, first 400 notes in order >= 0.99) with every other recording below 0.6, and its
loop flag must equal the log's loop offset being set; an unmapped command must play no music note.
Output: the table and PASS/FAIL; WORK_DIR keeps the ROM-derived program and the binary (keep it in build/)."""
import collections, difflib, gzip, math, os, re, struct, subprocess, sys, zipfile, zlib

if len(sys.argv) != 4:
    sys.exit(__doc__)
roms, archive, work = sys.argv[1:]
here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.dirname(os.path.dirname(os.path.dirname(here)))
SECONDS = 60
os.makedirs(work, exist_ok=True)

program = bytearray()
with zipfile.ZipFile(roms) as z:
    for name, crc in (("epr-16720.7", 0x8E73CFFD), ("epr-16721.8", 0x1BB3B7B7)):
        data = z.read(name)
        if len(data) != 0x20000 or zlib.crc32(data) != crc:
            sys.exit(f"{name}: not Revision A's sound program")
        for i in range(0, len(data), 2):
            program += bytes((data[i + 1], data[i]))
program_path = os.path.join(work, "sound_program_revA.bin")
open(program_path, "wb").write(program)
binary = os.path.join(work, "music_notes")
subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-O2", "-I", os.path.join(repo, "src"),
                os.path.join(here, "music_notes.cpp"), os.path.join(repo, "src/runtime/native_sound_sequencer.cpp"),
                "-o", binary], check=True)

LEN = {**{c: 1 for c in range(0x30, 0x40)}, 0x4f: 1, 0x50: 1, **{c: 2 for c in range(0x40, 0x4f)},
       **{c: 2 for c in range(0x51, 0x60)}, 0x61: 2, 0x62: 0, 0x63: 0, 0x66: 0, **{c: 0 for c in range(0x70, 0x90)},
       **{c: 2 for c in range(0xa0, 0xc0)}, **{c: 3 for c in range(0xc0, 0xe0)}, **{c: 4 for c in range(0xe0, 0x100)}}


def rate(r2, r3):
    fraction = ((r3 & 15) << 6) | (r2 >> 2)
    octave = ((r3 >> 4) - 1) & 15
    return math.ldexp((10000000 / 224) * (1024 + fraction) / 1024, octave - 16 if octave >= 8 else octave)


def recording(data):
    """MultiPCM key-ons (seconds, sample, rate) and whether the log loops."""
    off = 0x34 + struct.unpack_from("<I", data, 0x34)[0]
    regs = [[0] * 11 for _ in range(28)]
    loaded, slot, address, t, notes, i = [0] * 28, 28, 0, 0, [], off
    while i < len(data) and data[i] != 0x66:
        c = data[i]
        if c == 0x67:
            i += 7 + (struct.unpack_from("<I", data, i + 3)[0] & 0x7fffffff)
            continue
        if c == 0x61: t += struct.unpack_from("<H", data, i + 1)[0]
        elif c == 0x62: t += 735
        elif c == 0x63: t += 882
        elif 0x70 <= c < 0x80: t += (c & 15) + 1
        elif c == 0xb5:
            port, d = data[i + 1], data[i + 2]
            if port & 0x80:
                raise ValueError("second MultiPCM in a music log")
            if port == 1: slot = 28 if (d & 7) == 7 else (d & 31) - ((d & 31) >> 3)
            elif port == 2: address = d
            elif port == 0 and slot < 28 and address < 11:
                r = regs[slot]
                r[address] = d
                if address == 1: loaded[slot] = d | ((r[2] & 1) << 8)
                if address == 4 and d & 128: notes.append((t / 44100, loaded[slot], round(rate(r[2], r[3]), 1)))
        elif c not in LEN:
            raise ValueError(f"VGM command {c:02x}")
        i += 1 + LEN[c]
    if notes:
        notes = [n for n in notes if n[0] - notes[0][0] < SECONDS]
    return notes, struct.unpack_from("<I", data, 0x1c)[0] != 0


def sequenced(index):
    out = subprocess.run([binary, program_path, "ae", "10", f"{index:x}", str(SECONDS)], capture_output=True,
                         text=True, check=True).stdout.splitlines()
    return [(float(t), int(s), round(float(h), 1)) for t, rom, bank, s, h in (l.split() for l in out[1:]) if rom == "0"]


def similarity(a, b):
    x = collections.Counter((s, h) for _, s, h in a)
    y = collections.Counter((s, h) for _, s, h in b)
    return sum((x & y).values()) / max(sum(x.values()), sum(y.values()), 1)


def ordered(a, b, n=400):
    return difflib.SequenceMatcher(None, [n_[1:] for n_ in a[:n]], [n_[1:] for n_ in b[:n]], autojunk=False).ratio()


header = open(os.path.join(repo, "platform/dreamcast-native/include/audio/music_map.h")).read()
mapped = {int(c, 16): (int(t), int(l)) for c, t, l in re.findall(r"\{0x(ae10[0-9a-f]{2}), (\d+), ([01])\}", header)}
with zipfile.ZipFile(archive) as z:
    logs = {int(n[:2]): (n[3:-4], *recording(gzip.decompress(z.read(n)))) for n in sorted(z.namelist()) if n.endswith(".vgz")}
if len(logs) != 45 or len(mapped) != 45:
    sys.exit(f"expected 45 recordings and 45 map entries, found {len(logs)} and {len(mapped)}")

failures = 0
for index in range(0x30):
    command = 0xae1000 | index
    notes = sequenced(index)
    if not notes:
        ok = command not in mapped
        print(f"ae10{index:02x}: no music notes{'' if ok else ' but mapped'}")
        failures += not ok
        continue
    scores = sorted(((similarity(notes, log[1]), track) for track, log in logs.items()), reverse=True)
    (best, track), runner = scores[0], scores[1][0]
    order = ordered(notes, logs[track][1])
    want = mapped.get(command)
    ok = want == (track, int(logs[track][2])) and best >= 0.99 and order >= 0.99 and runner < 0.6
    failures += not ok
    print(f"ae10{index:02x}: t{track:02d} {logs[track][0]!r} similarity {best:.3f} (next {runner:.3f}) ordered {order:.3f} "
          f"loops {int(logs[track][2])} notes {len(notes)}/{len(logs[track][1])}{'' if ok else f' MISMATCH (map {want})'}")
print(f"{'PASS' if not failures else 'FAIL'}: {48 - failures} of 48 bank 0x10 commands agree with audio/music_map.h")
sys.exit(1 if failures else 0)
