"""ROM-free DNR7 source/reader checks, plus legacy DNR3/4/5/6 compatibility."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from prepare_road import prepare as prepare_pack, HEADER, MAGIC, MAIN_BASE

tables=bytearray(0x40000)
struct.pack_into("<I",tables,4*(0x8000|((0x3fc00000>>9)&0x3ffe)|1),0x3f800000)
def prepare(program,main,slot):
    return prepare_pack(program,main,slot,tables)

p, m = bytearray(0x36000), bytearray(0x8b57a8)
struct.pack_into("<4I",m,0x86fab0,100,104,108,112)
for at,value in ((0x8b573c,0.125),(0x8b57a4,1.5),(0x8b574c,2.5)):
    struct.pack_into("<f",m,at,value)
struct.pack_into("<4I",p,0x3950,8,2,4,8)
struct.pack_into("<I",p,0x2f1f0,2)
for i in range(2):
    struct.pack_into("<5I",p,0x2f1f4+20*i,0,i,768,0,0x501220+4*i)
    struct.pack_into("<4I",m,0x86fac0+48*i+32,i,i,0x438e8000,2)
struct.pack_into("<4I",p,0x3980,2,2,2,2)
struct.pack_into("<4I",p,0x3a48,*[MAIN_BASE+32*i for i in range(4)])
for slot in range(4):
    struct.pack_into("<I",m,0x870304+4*slot,MAIN_BASE+1024)
    struct.pack_into("<8I",m,32*slot,0,MAIN_BASE+128,MAIN_BASE+144,0,0,MAIN_BASE+256,MAIN_BASE+592,0)
    struct.pack_into("<I",m,0x862444+4*slot,MAIN_BASE+598)
    struct.pack_into("<I",m,0x872974+4*slot,MAIN_BASE+640+32*slot)
    struct.pack_into("<I",m,640+32*slot,slot)
    for i in range(slot):
        struct.pack_into("<I",m,644+32*slot+4*i,0xabcd0000+(i%2))
struct.pack_into("<4I",m,128,1,2,0x00400000,0x00020001)
struct.pack_into("<2I",m,256,1,8)
for profile in range(2):
    struct.pack_into("<bbh",m,264+164*profile,0,7,-32768)
    for lane in range(8):
        struct.pack_into("<3f4h",m,268+164*profile+20*lane,-0.0,1.5,float(lane),-32768,32767,-1,2)
struct.pack_into("<2H",m,598,0x8000,0x1000)
struct.pack_into("<4I",m,1024,*[MAIN_BASE+2048+1028*d for d in range(4)])
for difficulty in range(4):
    struct.pack_into("<257I",m,2048+1028*difficulty,*[difficulty*1000+i for i in range(257)])
for table in (0x34fb0,0x34f70,0x34e38,0x34e28):
    struct.pack_into("<4I",p,table,*[0x235100]*4)
struct.pack_into("<16I",p,0x35100,*range(16))
with tempfile.TemporaryDirectory(prefix="daytona-road-") as temp:
    out = Path(temp)/"road.dnr"
    for slot in range(4):
        data = prepare(p,m,slot)
        laps = (8,2,4,8)[slot]
        base=4592+2*slot
        grid_at=(base+31)&~31
        assert HEADER.unpack_from(data) == (MAGIC,grid_at+160,slot,2,2,slot,laps,2)
        assert data[32:360] == m[264:592]
        assert struct.unpack_from("<4H",data,360) == (0,0x8040,1,0x1002)
        assert data[368:4480] == m[2048:6160]
        assert struct.unpack_from("<24I",data,4480) == tuple(range(4))*2+tuple(range(16))
        assert data[4576:4592] == bytes(p[0x35100+2*i] for i in range(16))
        assert data[4592:base] == struct.pack(f"<{slot}H",*[i%2 for i in range(slot)])
        assert data[base:grid_at] == bytes(grid_at-base)
        assert struct.unpack_from("<5Ii2I",data,grid_at)==(0x438e8000,0,0,0,1408,-1,0,2)
        assert struct.unpack_from("<2f",data,grid_at+64)==(35.625,35.625)
        assert data[grid_at+72:grid_at+96]==bytes(24)
        assert struct.unpack_from("<2f",data,grid_at+96)==(35.625,35.625)
        assert data[grid_at+104:grid_at+128]==bytes(24)
        assert struct.unpack_from("<4I2f2I",data,grid_at+128)==(100,104,108,112,1.5,2.5,0,0)
        out.write_bytes(data); subprocess.run([sys.argv[1],str(out),str(laps)],check=True)
        legacy=bytearray(data[:grid_at+96]+data[grid_at+128:]);struct.pack_into("<2I",legacy,0,0x36524e44,len(legacy))
        out.write_bytes(legacy);subprocess.run([sys.argv[1],str(out),str(laps)],check=True)
        legacy=bytearray(data[:grid_at+64]);struct.pack_into("<2I",legacy,0,0x35524e44,len(legacy))
        out.write_bytes(legacy);subprocess.run([sys.argv[1],str(out),str(laps)],check=True)
        for magic,legacy_laps in ((0x33524e44,0),(0x34524e44,laps)):
            legacy = bytearray(data[:base])
            struct.pack_into("<2I",legacy,0,magic,base)
            struct.pack_into("<2I",legacy,24,legacy_laps,0)
            out.write_bytes(legacy); subprocess.run([sys.argv[1],str(out),str(legacy_laps)],check=True)
    bad_files = [data[:n] for n in (0,1,31,32,len(data)-1)]+[data+b"\0"]
    for at,value in ((0,0),(0,0x31524e44),(0,0x32524e44),(4,369),(8,4),(12,1),(12,4098),(16,0),(16,65537),
                     (20,1),(20,32),(24,0),(24,256),(28,0),(28,45),(36,0x7f800000),(40,0x7fc00000),
                     (grid_at,0x7fc00000),(grid_at+4,1),(grid_at+8,65536),(grid_at+12,28),
                     (grid_at+16,384),(grid_at+20,0),(grid_at+24,8),(grid_at+28,256),(base,1),
                     (360,2),(364,0xffff),(4592,2),(grid_at+64,0x7fc00000),(grid_at+72,1),
                     (grid_at+96,0x7fc00000),(grid_at+104,1),
                     (grid_at+128,0),(grid_at+144,0x7f800000),(grid_at+148,0x7fc00000),(grid_at+152,1)):
        bad = bytearray(data); struct.pack_into("<I",bad,at,value); bad_files.append(bad)
    for first,last in ((-1,7),(0,8),(7,6)):
        bad = bytearray(data); struct.pack_into("<bb",bad,32,first,last); bad_files.append(bad)
    for bad in bad_files:
        out.write_bytes(bad)
        assert subprocess.run([sys.argv[1],str(out)],capture_output=True).returncode == 1
for slot in (-1,4):
    try: prepare(p,m,slot)
    except ValueError: pass
    else: raise AssertionError("invalid slot accepted")
for at,value in ((0x862444,MAIN_BASE+599),(0x862444,MAIN_BASE-2),
                 (0x862444,MAIN_BASE+len(m)-2),(136,2),(268,0x7fc00000),
                 (0x872974,MAIN_BASE+641),(0x872974,MAIN_BASE-4),(640,32),
                 (0x870304,MAIN_BASE+1025),(1024,MAIN_BASE+len(m)-1024),(1024,MAIN_BASE-4),
                 (0x86fab0,0),(0x8b573c,0x7fc00000),(0x8b57a4,0x7f800000),(0x8b574c,0x7fc00000)):
    old = m[at:at+4]; struct.pack_into("<I",m,at,value)
    try: prepare(p,m,0)
    except ValueError: pass
    else: raise AssertionError("invalid source accepted")
    m[at:at+4] = old
for at,value in ((0x3950,0),(0x3950,256),(0x34fb0,0x235101),(0x34f70,0x200000+len(p)),(0x34e38,0x1ffffc),
                 (0x34e28,0x235101),(0x34e28,0x200000+len(p)-32)):
    old = p[at:at+4]; struct.pack_into("<I",p,at,value)
    try: prepare(p,m,0)
    except ValueError: pass
    else: raise AssertionError("invalid award source accepted")
    p[at:at+4] = old
struct.pack_into("<2I",m,640,1,2)
try: prepare(p,m,0)
except ValueError: pass
else: raise AssertionError("out-of-course checkpoint accepted")
print(f"PASS: four road slots, DNR7 guided/start/grid/laps, legacy DNR3/4/5/6, exact profiles/awards and {len(bad_files)} malformed files")
