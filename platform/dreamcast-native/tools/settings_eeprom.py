"""Write the I/O board's settings EEPROM with chosen operator settings (test mode > GAME ASSIGNMENTS).

usage: settings_eeprom.py PROGRAM.bin NEW_DIR [--difficulty easy|normal|hard|hardest] [--advertise on|off]
                          [--country usa|jpn|export] [--cabinet deluxe|uplight]
       settings_eeprom.py --show EEPROM.bin

Writes NEW_DIR/ioboard_eeprom.bin (128 bytes) for `m2segment --nvram NEW_DIR` (tools/common/nvram.h): the game's
own factory image (program 23c150, ROM data: the file stays in ignored build/ directories) with the chosen fields
and the checksum the game checks at boot, so the game takes the settings as if set in test mode.

The original (daytona93):
  - 228230 copies the EEPROM (dual-port RAM 0x100-0x17f, I/O command 3) to backup RAM 1d00000-1d0007f. 227cf0
    then checks "SEGA" at +0, the game id +4 (8240) and the layout version +6 (0001); a mismatch rewrites the
    factory image (from +0, +0x10 or +0x20 on) and saves it (228360). Otherwise it checks the CRC +8 over
    +0x0a..+0x7f (CRC-16/XMODEM: polynomial 1021, start 0, table 23c1d0); a bad copy falls back to the copy in
    backup RAM 1d00080, then to the factory image.
  - Then 53e588 = +0b, 53e5fe = +19, 53e5d0 = +1a, 53e59a = +1b, 53e59c = +20, and 53e110 (credits to start) =
    +15 unless +10 (coin/credit setting) is 26 (free play): 0.
  - The fields of the GAME ASSIGNMENTS screen (229640..229cc4; names from the tables 23c6e0..23c710):
      +19 ADVERTISE SOUND  0 ON (factory), 1 OFF     -> 53e5fe, read by 1ca8 (phase 2: ae1008 or be1a10)
      +1b COUNTRY          0 USA, 1 JPN (factory), 2 EXPORT -> 53e59a (texts, voices, speed units)
      +1a CABINET          0 DELUXE (factory), 1 UPLIGHT -> 53e5d0 (0a84: colour translation tables)
      +20 DIFFICULTY       0 EASY, 1 NORMAL (factory), 2 HARD, 3 HARDEST -> 53e59c (3860, 13574, 135a4)
    and +15 CREDIT TO START, +10 COIN/CREDIT SETTING (coins; the native game has none).
  - +7c counts the saves (228360).
"""
import argparse
import hashlib
import sys
from pathlib import Path

PROGRAM_BASE = 0x200000
PROGRAM_SHA256 = 'f830f4a65e1a4cb0dc3520f4bea4184dcfbd1aa5a8b129c89977e8cabe012ef4'
FACTORY = 0x23c150
CRC_TABLE = 0x23c1d0
FIELDS = {  # option: (offset, names in the order of the game's values)
    'advertise': (0x19, ('on', 'off')),
    'cabinet': (0x1a, ('deluxe', 'uplight')),
    'country': (0x1b, ('usa', 'jpn', 'export')),
    'difficulty': (0x20, ('easy', 'normal', 'hard', 'hardest')),
}


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc


def factory_image(program):
    if hashlib.sha256(program).hexdigest() != PROGRAM_SHA256:
        raise ValueError('unrecognized daytona93 program image')
    image = bytearray(program[FACTORY - PROGRAM_BASE:FACTORY - PROGRAM_BASE + 128])
    if image[:4] != b'SEGA':
        raise ValueError('factory settings image not found')
    # The game's own table must be the XMODEM table this tool computes with.
    table = program[CRC_TABLE - PROGRAM_BASE:CRC_TABLE - PROGRAM_BASE + 512]
    if any(int.from_bytes(table[2*i:2*i+2], 'little') != crc16_xmodem(bytes([i])) for i in range(256)):
        raise ValueError('checksum table differs from CRC-16/XMODEM')
    return image


def seal(image):
    image[8:10] = crc16_xmodem(bytes(image[0x0a:0x80])).to_bytes(2, 'little')
    return image


def describe(image):
    lines = []
    crc = int.from_bytes(image[8:10], 'little')
    lines.append(f'signature={bytes(image[:4])!r} id={image[4:6].hex()} version={image[6:8].hex()} '
                 f'crc={crc:04x} ({"good" if crc == crc16_xmodem(bytes(image[0x0a:0x80])) else "BAD"}) '
                 f'saves={int.from_bytes(image[0x7c:0x80], "little")}')
    for name, (offset, names) in FIELDS.items():
        v = image[offset]
        lines.append(f'{name}=+{offset:02x}:{v} ({names[v] if v < len(names) else "invalid"})')
    lines.append(f'credit_to_start=+15:{image[0x15]} coin_setting=+10:{image[0x10]}')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('program', type=Path, nargs='?')
    parser.add_argument('output', type=Path, nargs='?')
    parser.add_argument('--show', type=Path, help='decode an EEPROM image instead')
    for name, (_, names) in FIELDS.items():
        parser.add_argument('--' + name, choices=names)
    args = parser.parse_args()
    if args.show:
        image = args.show.read_bytes()
        if len(image) != 128:
            print('not a 128-byte EEPROM image', file=sys.stderr); return 2
        print(describe(image)); return 0
    if not args.program or not args.output:
        parser.print_usage(); return 2
    image = factory_image(args.program.read_bytes())
    for name, (offset, names) in FIELDS.items():
        choice = getattr(args, name)
        if choice:
            image[offset] = names.index(choice)
    seal(image)
    args.output.mkdir(parents=True, exist_ok=False)
    with (args.output / 'ioboard_eeprom.bin').open('xb') as out:
        out.write(image)
    print(describe(image))
    return 0


if __name__ == '__main__':
    sys.exit(main())
