"""ROM-free extraction check; run directly with Python, no framework."""
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from prepare_race_intents import extract, extract_announcements, render, PREDICATES

program = bytearray(0x37000)
expected = []
for i in range(57):
    duration = (-2147483648, -1, 0, 2147483647)[i % 4]
    trigger = (-32768, -1, 0, 32767)[i % 4]
    row = (0x80000000+i, 0xffffffff-i, duration, trigger)
    struct.pack_into('<IIihH', program, 0x35ec0+16*i, *row, 0xa55a)
    expected.append(row)
struct.pack_into('<4I', program, 0x191c8, 11, 12, 21, 22)
struct.pack_into('<4I', program, 0x36250, 0, 1, 0x80000000, 0xffffffff)
assert extract(program) == (expected, [(11,21), (12,22)], [0,1,1,1])
addresses = (0x236300, 0x236400, 0x236500)
struct.pack_into('<4I', program, 0x36260, addresses[0], addresses[2], addresses[1], addresses[0])
tables = []
for t, address in enumerate(addresses):
    # Source advances a cursor, not a sorted lookup: descending/repeated thresholds are valid.
    rows = [(threshold, 0x12340000+i, t*3+i) for i, threshold in enumerate((12,10,10))] + [(100000,18,0)]
    for i, (threshold, signal, tag) in enumerate(rows):
        struct.pack_into('<iIII', program, address-0x200000+16*i, threshold, signal, PREDICATES[tag], 0)
    tables.append(rows)
struct.pack_into('<9I', program, 0x1956c, *range(48,57))
assert extract_announcements(program) == (tables, [0,2,1,0], tuple(range(48,57)))
header = render(program)
assert '},-2147483648,-32768,0},' in header
assert 'UINT32_C(0xffffffff)' in header
assert 'nm_race_intent_alternate[4] = {0,1,1,1};' in header
try:
    extract(program[:0x3625f])
except ValueError:
    pass
else:
    raise AssertionError('truncated selector table accepted')
for offset, value in ((0x36308,1), (0x3630c,1), (0x36330,100001), (0x36260,0x1ffff0)):
    broken = bytearray(program)
    struct.pack_into('<I', broken, offset, value)
    try:
        extract_announcements(broken)
    except ValueError:
        pass
    else:
        raise AssertionError(f'invalid creator data accepted at {offset:x}')
assert 'nm_race_announcement_offsets[4] = {0,8,4,0};' in header
assert 'nm_race_announcement_counts[4] = {4,4,4,4};' in header
broken = bytearray(program)
for i in range(128):
    struct.pack_into('<iIII', broken, 0x36300+16*i, 0, 18, 0, 0)
try:
    extract_announcements(broken)
except ValueError as error:
    assert str(error) == 'missing announcement sentinel'
else:
    raise AssertionError('unterminated creator table accepted')
print('PASS: 57 consumer rows; all 9 creator tags; source-order thresholds, selectors, widths and malformed data')
