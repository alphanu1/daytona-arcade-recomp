"""ROM-free authored path extraction, exact guards and reader rejection."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from prepare_path import prepare, HEADER, MAGIC, MAIN_BASE

p, m = bytearray(0x3a58), bytearray(4096)
struct.pack_into("<4I",p,0x3a48,MAIN_BASE,MAIN_BASE+32,MAIN_BASE+64,MAIN_BASE+96)
struct.pack_into("<I",m,16,MAIN_BASE+128)
for route in range(8):
    struct.pack_into("<2I",m,128+8*route,MAIN_BASE+256+280*route,2)
    for row in range(10):
        struct.pack_into("<4f5hH",m,256+280*route+28*row,
                         -0.0,1.5,float(row),2.5,-1,-32768,32767,2,3,0xabcd)
struct.pack_into("<I",m,48,MAIN_BASE+256+280*8)
with tempfile.TemporaryDirectory(prefix="daytona-path-") as temp:
    out = Path(temp)/"path.dnp"
    for route in range(8):
        data = prepare(p,m,0,route)
        assert HEADER.unpack_from(data) == (MAGIC,312,0,route,2,10,0,0)
        assert data[32:] == m[256+280*route:536+280*route]
        out.write_bytes(data); subprocess.run([sys.argv[1],str(out)],check=True)
    bad_files = [data[:n] for n in (0,1,31,32,len(data)-1)]+[data+b"\0"]
    for at,value in ((0,0),(4,len(data)+1),(8,3),(12,8),(16,0),(16,1163),(20,9),(24,1),(28,1),
                     (32,0x7f800000),(36,0x7fc00000),(40,0xff800000),(44,0x7fffffff),(284,0x7f800000)):
        bad = bytearray(data); struct.pack_into("<I",bad,at,value); bad_files.append(bad)
    for bad in bad_files:
        out.write_bytes(bad)
        assert subprocess.run([sys.argv[1],str(out)],capture_output=True).returncode == 1
for slot,route in ((3,0),(-1,0),(0,8),(0,-1)):
    try: prepare(p,m,slot,route)
    except ValueError: pass
    else: raise AssertionError("invalid course/route accepted")
for at,value in ((128,MAIN_BASE+257),(132,0),(132,1163),(136,MAIN_BASE+999),(256,0x7fc00000)):
    old = m[at:at+4]; struct.pack_into("<I",m,at,value)
    try: prepare(p,m,0,0)
    except ValueError: pass
    else: raise AssertionError("invalid source accepted")
    m[at:at+4] = old
print(f"PASS: eight routes, exact28-byte rows/guards, bounded sources and {len(bad_files)} malformed DNP1 files")
