"""ROM-free exporter/target-reader check, using the existing C drive checker."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from prepare_drive import prepare

main = bytearray(0x8f7000)
tables = bytearray(0x40000)
program = bytearray(0x37000)
regions = [(0xb57bc,2),(0xb5788,1),(0xb57b0,2),(0xb5820,4),(0xb8360,67),
           (0xb8120,67),(0xb5d20,512),(0xb6520,512),(0xb6d20,512)]
expected = []
for region,(offset,count) in enumerate(regions):
    values = [float(10*region+i) for i in range(count)]
    struct.pack_into(f"<{count}f",main,0x800000+offset,*values)
    expected.extend(values)
# Synthetic reciprocal table entries chosen so exponent adjustment produces
# 0.25 and1.0: refinement = 1 - (2000/1024)*0.25 = 0.51171875.
index = 0x8000 | ((0x44fa0000 >> 9) & 0x3ffe)
struct.pack_into("<2f",tables,4*index,256,1024)
struct.pack_into("<f",tables,4,0.125)
axle_expected = []
for region,(offset,count) in enumerate(((0xb5790,2),(0xb5770,1),(0xb5784,1),
                                       (0xb57e0,2),(0xb57c4,2),(0xb57cc,2))):
    values = [float(10*region+i) for i in range(count)]
    struct.pack_into(f"<{count}f",main,0x800000+offset,*values)
    axle_expected.extend(values)
struct.pack_into("<f",main,0x8b5760,0.125)
struct.pack_into("<f",main,0x8b57fc,0.75)
struct.pack_into("<f",main,0x8b5774,2.5)
for offset,value in ((0x8b57a8,3.5),(0x8b57ac,4.5),(0x8b5780,5.5),(0x8b576c,6.5),(0x8b578c,7.5),
                     (0x8b57e8,8.5),(0x8b57ec,9.5),(0x8b57d4,10.5)):
    struct.pack_into("<f",main,offset,value)
struct.pack_into("<4f",program,0xf9c0,1,2,3,4)
struct.pack_into("<4f",main,0x8b5850,0.5,0.75,1,1.25)
struct.pack_into("<5f",main,0x8b5800,10,20,30,40,50)
struct.pack_into("<32f",program,0xebc4,*range(32))
struct.pack_into("<f",main,0x8b5758,3.5)
struct.pack_into("<8i",main,0x8b5830,-10,20,30,40,100,200,300,400)
struct.pack_into("<8i",program,0x34bc8,-4,4,-3,3,-2,2,-1,1)
struct.pack_into("<8b",program,0x34abc,-1,0,1,2,3,-1,2,1)
struct.pack_into("<12f",program,0x34ac4,*range(12))
struct.pack_into("<f",main,0x8b5b00,0.125)
struct.pack_into("<128f",main,0x8b5b10,*range(128))
struct.pack_into("<I",main,0x8b5d10,512)
# Negative speed indices (include/drive.h nm_drive_drag_bits): erased words below
# the drag table and at the 16-bit wrap of the two surface tables.
main[0x8b5d14:0x8b5d20] = b"\xff"*12
for base in (0x8b6520,0x8b6d20):
    main[base+4*(65536-131):base+4*65536] = b"\xff"*(4*131)
struct.pack_into("<32I",program,0x35b5c,*range(100,132))
for table in range(2):
    for curve in range(2):
        offset = 0x35000+64*(2*table+curve)
        struct.pack_into("<I",program,0x16d7c+8*table+4*curve,0x200000+offset)
        struct.pack_into("<16i",program,offset,*range(16))
struct.pack_into("<2I",main,0x896e58,0x2001000,0x2001000)
struct.pack_into("<2I",main,0x1000,4,0x2002000)
struct.pack_into("<I",main,0x2000,257)
for i in range(257):
    struct.pack_into("<2IH3h",main,0x2004+16*i,0,1000+i,i,-i,i,i//2)
struct.pack_into("<3I4h4I",main,0x8b5710,11,12,13,-1,-32768,32767,99,14,15,999,16)
struct.pack_into("<4I",program,0x35cfc,0x236000,0x236400,0x236800,0x236000)
for slot in range(3):
    struct.pack_into("<256I",program,0x36000+1024*slot,*[slot*256+i for i in range(256)])
program[0x35a34:0x35a38] = bytes((0,1,1,0))
struct.pack_into("<2f",main,0x8b573c,3.25,7.5)
struct.pack_into("<512h",program,0x35200,*range(-256,256))
struct.pack_into("<20h",program,0x35b18,*[(16384 if i%2 else -32768)+128+i for i in range(20)])
struct.pack_into("<4I",program,0x35b08,0x118,0x10c,0x100,0xf4)
struct.pack_into("<4I",program,0x35af8,0,1,2,3)
struct.pack_into("<4I",program,0x35b44,0x10001,0xffffffff,0x12345678,0)
struct.pack_into("<12H",program,0x14fb4,*range(65524,65536))
schedule = [(i+row*3)%14-5 for row in range(4) for i in range(14)]
struct.pack_into("<56b",program,0x13ec4,*schedule)
struct.pack_into("<2h",program,0x35280,-32768,32767)
struct.pack_into("<4h",program,0x35b54,-32768,-1,0,32767)
struct.pack_into("<44f",program,0xcd30,*range(44))
for broken in (0x8b5d18,0x8b6d20+4*(65536-131)):
    damaged = bytearray(main); damaged[broken] = 0
    try: prepare(damaged,tables,program)
    except ValueError as error: assert "negative speed index" in str(error)
    else: raise AssertionError("moved negative speed index words accepted")
data = prepare(main,tables,program)
assert len(data) == 284468 and struct.unpack_from("<4I",data) == (0x52444e44,284468,0,0)
assert data[16+281204:16+282228] == program[0x3543c-512:0x3543c+512]
for mode in range(256):
    assert data[16+282228+4*mode:16+282232+4*mode] == program[0x35290+4*(mode if mode<128 else mode-256):0x35294+4*(mode if mode<128 else mode-256)]
assert data[16+283252:16+284276] == program[0x35284:0x35684]
assert data[16+284276:] == program[0xcd30:0xcde0]
assert struct.unpack_from("<127f",data,16+280096) == tuple(range(1,128))
assert data[16+280604:16+281116] == program[0x34b88:0x34d88]
assert data[16+281116:16+281203] == program[0x34b56-43:0x34b56+44]
assert data[16+281203] == 0
assert data[16+280016:16+280020] == bytes((3,2,1,0))
assert struct.unpack_from("<4H",data,16+280020) == (0,0x5678,65535,1)
assert data[16+280028:16+280084] == bytes(v+5 for v in schedule)
assert struct.unpack_from("<6h",data,16+280084) == (-32768,32767,-32768,-1,0,32767)
assert struct.unpack_from("<f",data,16+280012) == (3.25,)
# A subnormal raw step must survive even when its old quarter-step rounds to0.
struct.pack_into("<I",main,0x8b573c,1)
assert struct.unpack_from("<I",prepare(main,tables,program),16+280012) == (1,)
struct.pack_into("<f",main,0x8b573c,3.25)
assert data[16+279936:16+279956] == bytes(range(128,148))
assert struct.unpack_from("<12H8I",data,16+279956) == tuple(range(65524,65536))+tuple(1<<i for i in range(8))
saved_indices = program[0x35000:0x35020]
struct.pack_into("<8I",program,0x35000,31,32,63,0xffffffff,0,1,30,0x80000002)
assert struct.unpack_from("<8I",prepare(main,tables,program),16+279980) == (1<<31,1,1<<31,1<<31,1,2,1<<30,4)
program[0x35000:0x35020] = saved_indices
for slot in range(4):
    assert struct.unpack_from("<256I",data,16+273780+1024*slot) == tuple(range((slot%3)*256,(slot%3+1)*256))
assert data[16+277876:16+277880] == bytes((0,1,1,0))
assert struct.unpack_from("<2f",data,16+277880) == (7.5,0.8125)
for table,base in enumerate((0x35140,0x35180,0x3573c,0x3561c)):
    for timer in range(256):
        index = (timer if timer < 128 else timer-256) if table < 2 else 144-timer
        assert data[16+277888+512*table+2*timer:16+277890+512*table+2*timer] == program[base+2*index:base+2*index+2]
assert struct.unpack_from("<6I3hH",data,16+273748) == (11,12,13,14,15,16,-1,-32768,32767,0)
assert struct.unpack_from(f"<{len(expected)}f",data,16) == tuple(expected)
assert struct.unpack_from("<f",data,16+4*len(expected))[0] == 0.51171875
assert struct.unpack_from("<10f",data,20+4*len(expected)) == tuple(axle_expected)
assert struct.unpack_from("<15f",data,60+4*len(expected)) == (0.125,0.75,1,2,3,4,2.5,3.5,4.5,5.5,6.5,7.5,8.5,9.5,10.5)
assert struct.unpack_from("<4f",data,16+6820) == (0.5,0.75,1,1.25)
assert struct.unpack_from("<5f",data,16+6836) == (10,20,30,40,50)
assert struct.unpack_from("<33f",data,16+6856) == tuple(range(32))+(3.5,)
assert struct.unpack_from("<12f",data,16+6988) == tuple(range(12))
assert struct.unpack_from("<f",data,16+7036)[0] == 0.125
for mode in range(256):
    low,high = struct.unpack_from("<2b",program,0x34b88+2*mode)
    rank = low if low > 0 else high if high < 0 else 0
    assert struct.unpack_from("<f",data,16+7040+4*mode)[0] == max(1,min(127,64+rank))
assert struct.unpack_from("<16i8b",data,16+8064) == (-10,20,30,40,100,200,300,400,-4,4,-3,3,-2,2,-1,1,-1,0,1,2,3,-1,2,1)
assert data[16+8136:16+270280] == tables
assert struct.unpack_from("<32I",data,16+270280) == tuple(range(100,132,2))+tuple(range(101,132,2))
assert struct.unpack_from("<64i",data,16+270280+128) == tuple(range(16))*4
for i in range(257):
    assert struct.unpack_from("<IH3h",data,16+270280+384+12*i) == (1000+i,i,-i,i,i//2)
with tempfile.TemporaryDirectory(prefix="daytona-drive-") as folder:
    path = Path(folder)/"drive.dnd"
    path.write_bytes(data)
    subprocess.run([sys.argv[1],str(path)],check=True)
for bad_main,bad_tables,bad_program in ((main[:16],tables,program),(main,tables[:-1],program),(main,tables,program[:16])):
    try:
        prepare(bad_main,bad_tables,bad_program)
    except ValueError:
        pass
    else:
        raise AssertionError("truncated source accepted")
for target,offset,fmt,value in ((program,0,"<I",1),(main,0x8b5850,"<f",float("nan")),
                               (main,0x8b573c,"<f",float("inf")),(main,0x8b5740,"<f",float("nan")),
                               (main,0x8b5b00,"<f",-1),
                               (main,0x8b5c10,"<f",float("nan")),
                               (main,0x8b5800,"<f",float("nan")),
                               (main,0x8b5758,"<f",float("inf")),
                               (program,0xebc4+31*4,"<f",float("nan")),
                               (program,0x34ac4+11*4,"<f",float("inf")),
                               (main,0x8b5830,"<i",101),
                               (program,0x34bc8,"<i",5),
                               (program,0x34abc,"<b",-2),
                               (program,0x34abc,"<b",4),
                               (main,0x1000,"<I",0),(main,0x2000,"<I",256),
                               (program,0x16d7c,"<I",0),
                               (program,0x35b08,"<I",0xf3),(program,0x35b0c,"<I",0xf5),
                               (program,0x35b10,"<I",0x119),(program,0x35b14,"<I",0xffffffff),
                               (program,0x35b08,"<I",0x10c),(program,0x35af8,"<I",1),
                               (program,0x13ec4,"<b",-6),(program,0x13ec4+55,"<b",9),
                               (program,0x13ec4,"<b",-4),
                               (program,0x35cfc,"<I",0),(program,0x35cfc,"<I",0x236002),
                               (program,0x35cfc,"<I",0x236ffc),(program,0x35a34,"<B",2)):
    previous = struct.unpack_from(fmt,target,offset)[0]
    struct.pack_into(fmt,target,offset,value)
    try:
        prepare(main,tables,program)
    except ValueError:
        pass
    else:
        raise AssertionError("invalid drive source accepted")
    struct.pack_into(fmt,target,offset,previous)
# Different source pointers are acceptable only when all consumed rows agree.
struct.pack_into("<I",main,0x896e5c,0x2001010)
struct.pack_into("<2I",main,0x1010,4,0x2004000)
main[0x4000:0x5014] = main[0x2000:0x3014]
assert prepare(main,tables,program) == data
struct.pack_into("<I",main,0x4008,123456)
try:
    prepare(main,tables,program)
except ValueError:
    pass
else:
    raise AssertionError("different crash sequences accepted")
struct.pack_into("<I",main,0x896e5c,0x2001000)
struct.pack_into("<f",tables,0,float("nan"))
try:
    prepare(main,tables,program)
except ValueError:
    pass
else:
    raise AssertionError("invalid sine accepted")
print("PASS: drive extraction layout, fixed reciprocal, target reader and invalid sources")
