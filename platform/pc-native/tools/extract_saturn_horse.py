#!/usr/bin/env python3
"""The Saturn horse's hoof sounds and torque curves, from your own Saturn Daytona USA disc, for the PC build.

    python3 extract_saturn_horse.py SATURN_FILES_DIR ASSET_SET

SATURN_FILES_DIR holds the disc's SOUNDS.BIN (the 68000 sound driver, loaded at sound RAM 0) and GAMED.BIN (the
race's sound bank, loaded at 0x3000). Writes ASSET_SET/sound/horse-hoof{0,1,2}.wav, the sounds the horse's gallop
plays (APROG 06034848: commands ae113bff, ae113cff, ae113dff), resolved the way the driver plays them:

  command ae 11 nn    sound effect nn of group 11 (driver 1242): table *(3000+1c) -> group -> effect -> a
                      track; effects 3b..3d are one note each, 9a kk ..: channel 10, key kk (21, 22, 23)
  note on (driver 57e) channel 10's program 0 (table *(3000+04), a drum map: first key, end key, then 8 bytes a
                      key: tone index, note, fine...); the tone (6e2: *(3000+00) + 16 * index): flags, the 8-bit
                      sample's sound RAM address (24 bits), its length in samples
  pitch (driver b98)  the note's high nibble is the SCSP octave (signed), its low nibble the semitone, whose
                      FNS is the driver's table at 1b10 (16 entries a semitone); rate = 44100 * 2^oct * (1 + FNS/1024)

The three keys play one sample (tone 24) at three pitches. Output: 16-bit mono WAV at each sound's own rate.

Also ASSET_SET/horse-drive.bin from APROG.BIN (loaded at 06003000): the horse's torque curves, automatic 0604633c and
manual 060461cc (the car table 0602ed18 gives them to horse 1; 16.16, the first 67 entries, the arcade curve's
length): 'HRS1', then 2 x 67 little-endian int32 (code/pc_horse.c).
Game-derived output: keep the asset set under build/.
"""
import struct
import sys
import wave
from pathlib import Path

DATA_BASE = 0x3000
APROG_BASE, HORSE_TORQUE = 0x06003000, (0x0604633C, 0x060461CC)   # automatic, manual
TORQUE_ENTRIES = 67


def horse_drive(src, asset_set):
    aprog = (src / 'APROG.BIN').read_bytes()
    curves = [struct.unpack_from(f'>{TORQUE_ENTRIES}i', aprog, at - APROG_BASE) for at in HORSE_TORQUE]
    if curves[0][0] != curves[1][0] or curves[0][-1] != 0 or not 0x1000000 < curves[0][0] < 0x2000000:
        raise SystemExit('APROG.BIN: no horse torque curves where the Saturn Daytona USA keeps them')
    path = asset_set / 'horse-drive.bin'
    path.unlink(missing_ok=True)
    path.write_bytes(b'HRS1' + struct.pack(f'<{2 * TORQUE_ENTRIES}i', *curves[0], *curves[1]))
    print(f'horse torque curves -> {path}')


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, out = Path(sys.argv[1]), Path(sys.argv[2]) / 'sound'
    horse_drive(src, Path(sys.argv[2]))
    driver = (src / 'SOUNDS.BIN').read_bytes()
    bank = (src / 'GAMED.BIN').read_bytes()
    W = lambda a: struct.unpack_from('>H', bank, a - DATA_BASE)[0]
    L = lambda a: struct.unpack_from('>I', bank, a - DATA_BASE)[0]
    B = lambda a: bank[a - DATA_BASE]
    if struct.unpack_from('>2I', driver, 0) != (0x3000, 0x120):
        raise SystemExit('SOUNDS.BIN is not the Daytona USA sound driver')
    fns = [struct.unpack_from('>H', driver, 0x1b10 + 2 * i)[0] & 0x3ff for i in range(192)]
    if fns[:3] != [0, 3, 7]:
        raise SystemExit('unexpected pitch table at driver 1b10')
    tones, programs, effects = L(DATA_BASE), L(DATA_BASE + 4), L(DATA_BASE + 0x1c)
    group = effects + W(effects + 2 + 2 * 0x11)
    out.mkdir(parents=True, exist_ok=True)
    for i, effect in enumerate((0x3b, 0x3c, 0x3d)):
        track = effects + W(group + 2 + 2 * effect)
        if B(track) & 0x80 == 0 or B(track + 1) != 0x9a:
            raise SystemExit(f'effect {effect:x}: not a single channel-10 note')
        key = B(track + 2)
        program = programs + W(programs + 2)          # channel 10's program 0
        if not B(program) & 0x80:
            raise SystemExit('program 0 is not a drum map')
        first, end = B(program + 2), B(program + 3)
        if not first <= key < end:
            raise SystemExit(f'key {key:x} outside the drum map')
        record = program + 4 + 8 * (key - first)
        tone_index, note = W(record) & 0x3ff, B(record + 2)
        tone = tones + 16 * tone_index
        start = L(tone) & 0xffffff
        length = L(tone + 4)
        if not (DATA_BASE <= start and start + length <= DATA_BASE + len(bank)):
            raise SystemExit('tone outside the bank')
        octave = (note >> 4) & 15
        octave = octave - 16 if octave & 8 else octave
        rate = 44100 * 2.0 ** octave * (1024 + fns[(note & 15) * 16]) / 1024
        pcm = bank[start - DATA_BASE:start - DATA_BASE + length]
        samples = struct.pack(f'<{length}h', *((b - 256 if b > 127 else b) << 8 for b in pcm))
        with wave.open(str(out / f'horse-hoof{i}.wav'), 'wb') as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(round(rate))
            w.writeframes(samples)
        print(f'effect {effect:x}: key {key:x}, tone {tone_index:x} at {start:06x} ({length} samples), note {note:02x}: '
              f'{rate:.0f} Hz -> {out / f"horse-hoof{i}.wav"}')


if __name__ == '__main__':
    main()
