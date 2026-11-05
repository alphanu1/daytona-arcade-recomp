"""ROM-free DNC3 extraction/reader roundtrip and malformed input checks."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from prepare_contact import prepare, HEADER, MAGIC, MAIN_BASE

program, main, copro = bytearray(0x3a58), bytearray(33024), bytearray(512)
struct.pack_into("<4I",program,0x3a48,*([MAIN_BASE]*4))
struct.pack_into("<8I",main,0,0,0,MAIN_BASE+32,MAIN_BASE+36,0,0,0,0)
struct.pack_into("<2I",main,32,MAIN_BASE+256,MAIN_BASE+40)
main[40:42] = bytes((1,0xf5))
struct.pack_into("<16384H",main,256,*([0,1]*8192))
struct.pack_into("<4I",copro,64,*([48]*4))
struct.pack_into("<4I",copro,128,*([64]*4))
struct.pack_into("<4I",copro,192,0,2,1,0)  # Preserve reverse order.
for i in range(2):
    struct.pack_into("<12f",copro,256+64*i,*[float(i*16+n) for n in range(12)])
    struct.pack_into("<I",copro,316+64*i,0xffffc000+i)
    struct.pack_into("<3f",copro,304+64*i,19+i,-0.0,21+i)
data = prepare(program,main,copro,2)
assert HEADER.unpack_from(data) == (MAGIC,32952,2,4,2,2,0,0)
assert struct.unpack_from("<4H",data,32800) == (0,2,1,0)
assert data[32808:32856] == copro[256:304]
assert struct.unpack_from("<I",data,32856)[0] == 1
assert struct.unpack_from("<I",data,32860)[0] == 0xffffc000
assert data[32864:32868] == copro[308:312]
assert struct.unpack_from("<3f",data,32868) == (4.5,5.5,6.5)
assert data[32880:32928] == copro[320:368]
assert struct.unpack_from("<2I",data,32928) == (0xf5,0xffffc001)
assert data[32936:32940] == copro[372:376]
assert struct.unpack_from("<3f",data,32940) == (20.5,21.5,22.5)
rounded = bytearray(copro)
for at, value in zip((256,268,280,292),(16777216,1,-16777216,1)):
    struct.pack_into("<f",rounded,at,value)
assert struct.unpack_from("<f",prepare(program,main,rounded,2),32868)[0] == 0.25
for at in (256,268,280,292):
    struct.pack_into("<f",rounded,at,2e38)
try:
    prepare(program,main,rounded,2)
except ValueError:
    pass
else:
    raise AssertionError("overflowing recovery center accepted")
bad_files = [data[:n] for n in (0,1,31,32,32799,len(data)-1)] + [data+b"\0"]
for at,fmt,value in ((0,"I",0),(0,"I",0x31434e44),(0,"I",0x32434e44),(4,"I",len(data)+1),(8,"I",4),(12,"I",0),
                     (12,"I",65537),(16,"I",0),(16,"I",8193),(20,"I",0),(20,"I",34),
                     (24,"I",1),(28,"I",1),(32,"H",2),(32,"H",65535),
                     (32802,"H",3),(32804,"H",2),(32808,"f",float("nan")),(32856,"I",256),
                     (32864,"f",float("inf")),(32868,"f",float("nan")),(32872,"f",-float("inf")),
                     (32876,"f",float("nan"))):
    bad = bytearray(data); struct.pack_into("<"+fmt,bad,at,value); bad_files.append(bad)
with tempfile.TemporaryDirectory(prefix="daytona-contact-") as directory:
    path = Path(directory)/"contact.dnc"
    path.write_bytes(data)
    subprocess.run([sys.argv[1],str(path)],check=True)
    # Odd list length pads with zero; that pad is validated by the reader.
    struct.pack_into("<4I",copro,192,0,1,0,0)
    odd = prepare(program,main,copro,0)
    path.write_bytes(odd); subprocess.run([sys.argv[1],str(path)],check=True)
    bad = bytearray(odd); bad[32806] = 1; bad_files.append(bad)
    for bad in bad_files:
        path.write_bytes(bad)
        assert subprocess.run([sys.argv[1],str(path)],capture_output=True).returncode == 1
    struct.pack_into("<4I",copro,192,0,2,1,0)
for target,at,fmt,value in ((program,0x3a50,"I",MAIN_BASE+len(main)),
                           (main,32,"I",MAIN_BASE+257),(main,36,"I",MAIN_BASE+len(main)),
                           (copro,196,"I",34),(copro,200,"I",8192),(copro,256,"f",float("inf")),
                           (copro,308,"f",float("inf"))):
    old = target[at:at+struct.calcsize(fmt)]
    struct.pack_into("<"+fmt,target,at,value)
    try:
        prepare(program,main,copro,2)
    except ValueError:
        pass
    else:
        raise AssertionError("malformed source accepted")
    target[at:at+len(old)] = old
for sources in ((program[:1],main,copro),(program,main[:1],copro),(program,main,copro[:1])):
    try:
        prepare(*sources,2)
    except ValueError:
        pass
    else:
        raise AssertionError("truncated source accepted")
print(f"PASS: DNC3 exact bits, ordered lists, surfaces/headings, rounded recovery centers, alignment, source bounds and {len(bad_files)} malformed files")
