"""ROM-free validation checks; optional full exported catalog verification."""
from pathlib import Path
import sys
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from inspect_intro_lights import read_lights, inspect

with tempfile.TemporaryDirectory() as temp:
    path = Path(temp)/'lights.txt'
    lines = [f'0 0 0 0 {i} 4278190080\n' for i in range(256)]
    transparent = [f'24576 0 0 0 {i} '+ ' '.join(['4278190080']*15+['0'])+'\n'
                   for i in range(256)]
    valid = ''.join(lines+transparent)
    path.write_text(valid)
    assert len(read_lights(path)) == 512
    for invalid in ('', ''.join(lines[:-1]), valid+lines[0],
                    valid.replace('4278190080','4294967296',1),
                    valid.replace('4278190080','0',1),
                    valid.replace('24576','65536',1),
                    valid.replace('0 0 0 0 0 ','0 0 0 0 256 ',1),
                    valid.replace(' 0\n',' 4278190080\n',1)):
        path.write_text(invalid)
        try: read_lights(path)
        except ValueError: continue
        raise AssertionError('invalid colour table accepted')
if len(sys.argv)>1:
    print(inspect(Path(sys.argv[1])))
print('PASS: full luma coverage, exact alpha, bounds and duplicate rejection')
