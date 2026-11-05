"""Owned-ROM exporter regression: python3 test_prepare_race_records.py PROGRAM.bin."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from prepare_race_records import extract, extract_names, render, render_names

source = Path(sys.argv[1]).resolve()
program = source.read_bytes()
tables = extract(program)
data = extract_names(program)
rules, replacements, default = data
assert len(tables) == 3 and all(len(t) == 24 for t in tables)
assert len(rules) == 40 and len(replacements) == 2 and all(len(r) == 4 for r in replacements)
for i, (name, timer, command) in enumerate(rules):
    assert name.to_bytes(3, 'little') == program[0x3bda8+3*i:0x3bdab+3*i]
    assert timer == struct.unpack_from('<I', program, 0x3bd04+4*i)[0]
    assert command == struct.unpack_from('<I', program, 0x3be24+4*i)[0]
for i, row in enumerate(replacements):
    pointer = struct.unpack_from('<I', program, 0x3bec8+4*i)[0]-0x200000
    assert b''.join(n.to_bytes(3, 'little') for n in row) == program[pointer:pointer+12]
assert default == struct.unpack_from('<I', program, 0x3bec4)[0]
header = render_names(data)
values = [int(v, 16) for v in re.findall(r'0x([0-9a-f]{8})u', header)]
assert values == [v for row in rules for v in row]+[v for row in replacements for v in row]+[default]
assert len(values)*4 == 516 and 'alignas(32)' in header
for bad in (program[:-1], bytes(len(program))):
    for reader in (extract, extract_names):
        try:
            reader(bad)
        except ValueError:
            pass
        else:
            raise AssertionError('unrecognized program accepted')

script = Path(__file__).resolve().parents[1]/'tools/prepare_race_records.py'
with tempfile.TemporaryDirectory() as directory:
    out = Path(directory)
    records, names = out/'records.h', out/'names.h'
    command = [sys.executable, str(script), str(source), str(records), '--name-output', str(names)]
    subprocess.run(command, check=True, capture_output=True)
    assert records.read_text() == render(tables) and names.read_text() == header
    before = records.read_bytes(), names.read_bytes()
    assert subprocess.run(command, capture_output=True).returncode != 0
    assert (records.read_bytes(), names.read_bytes()) == before
    # An existing second output must prevent creation of the first output.
    fresh = out/'fresh.h'
    command[3] = str(fresh)
    assert subprocess.run(command, capture_output=True).returncode != 0 and not fresh.exists()
    command[-1] = str(fresh)
    assert subprocess.run(command, capture_output=True).returncode != 0 and not fresh.exists()
print('PASS record/name export: source rows/header layout516 bytes, fingerprint rejection, existing/duplicate output preservation')
