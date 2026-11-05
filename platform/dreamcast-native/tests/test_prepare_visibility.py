"""ROM-free DNV1 reader/lifecycle rejection and placeholder strip checks."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from prepare_visibility import drawable_model

polygon=bytearray(4*24)
assert not drawable_model(polygon,0x800000)
struct.pack_into('<I',polygon,24,1) # Non-drawing quad link, skip ten words.
struct.pack_into('<I',polygon,64,2) # Non-drawing triangle, skip seven words.
assert not drawable_model(polygon,0x800000)
struct.pack_into('<I',polygon,64,0x102)
assert drawable_model(polygon,0x800000)
struct.pack_into('<I',polygon,64,1)
try: drawable_model(polygon,0x800000)
except ValueError: pass
else: raise AssertionError('unterminated placeholder accepted')
try: drawable_model(polygon,0x400000)
except ValueError: pass
else: raise AssertionError('non-ROM model accepted')

payload=bytes(range(25))+bytes(reversed(range(25)))+bytes(2)
payload+=struct.pack('<ffI',128,0,0x1000)*48
payload+=struct.pack('<2H',0,2)+struct.pack('<2H',2,0)*255
payload+=struct.pack('<18H',1,3,*range(16))+struct.pack('<18H',6,8,*range(16))
data=struct.pack('<8I',0x31564e44,32+len(payload),2,10,2,2,0,0)+payload
with tempfile.TemporaryDirectory(prefix='native-visibility-') as temp:
    path=Path(temp)/'scenery.dnv'
    def accepted(blob):
        path.write_bytes(blob)
        p=subprocess.run([sys.argv[1],str(path)],capture_output=True,text=True)
        assert p.returncode in (0,1), p.stderr
        if p.returncode: assert p.stdout=='REJECT: invalid scenery pack\n',p.stdout
        return p.returncode==0
    assert accepted(data)
    malformed=[b'',data[:31],data[:-1],data+b'x']
    for offset,fmt,value in (
        (0,'I',0),(4,'I',0),(8,'I',4),(12,'I',0),(12,'I',4098),(16,'I',65),
        (20,'I',65536),(24,'I',1),(28,'I',1),(32,'B',25),(32,'B',1),
        (57,'B',25),(82,'B',1),(83,'B',1),
        (84,'f',float('nan')),(88,'f',float('inf')),(84,'f',385),
        (92,'I',1<<25),(660,'H',1),(662,'H',3),(664,'H',1),
        (1684,'H',4),(1686,'H',10),(1720,'H',3)):
        bad=bytearray(data);struct.pack_into('<'+fmt,bad,offset,value);malformed.append(bad)
    assert all(not accepted(bad) for bad in malformed)
    # Legal zero-range/empty geometry still has complete 256-cell coverage.
    empty=bytearray(data[:1684]);struct.pack_into('<I',empty,4,len(empty))
    struct.pack_into('<I',empty,16,0);struct.pack_into('<I',empty,20,0)
    empty[660:1684]=bytes(1024)
    assert accepted(empty)
print(f'PASS DNV1 layout, load/unload ownership, {len(malformed)} malformed files and non-drawing placeholders')
