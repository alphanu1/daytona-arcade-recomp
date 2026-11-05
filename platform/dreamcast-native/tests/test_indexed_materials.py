"""Verify every compact binding against owned source geometry/light/index data.

python3 test_indexed_materials.py SOURCE_DIR PACK.dnt
No runtime-format support is implied by this offline test.
"""
from pathlib import Path
import struct
import sys
import subprocess
import tempfile
import zlib
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from inspect_intro_lights import read_lights
from prepare_models import rows
from prepare_materials import HEADER, RESOURCE, BINDING, reachable_luma_bounds
from indexed_vq import layout, texture

source=Path(sys.argv[1]);data=Path(sys.argv[2]).read_bytes()
magic,size,nr,nb,payload,maximum,nl,np=HEADER.unpack_from(data)
assert magic==b'DNT5' and size==len(data)<=2*1024*1024
assert 0<nr<65535 and 0<nb<65535 and 0<nl<=256 and 0<np<=16384
base=(32+16*(nr+nb)+31)&~31
assert not any(data[32+16*(nr+nb):base])
palbase=base+64*nl;end=palbase+32*np;templates=[]
for i in range(nl):
    at=base+64*i;header=data[at:at+32]
    po,io,ib,entries,*reserved=struct.unpack_from('<8I',data,at+32)
    assert not any(reserved) and entries<=256 and entries%4==0
    assert po==end and io==(po+2*entries+31)&~31
    assert not any(data[po+2*entries:io]) and io+ib<=len(data)
    patterns=struct.unpack_from(f'<{entries}H',data,po)
    w,h=struct.unpack_from('<HH',header,12)
    end=(io+ib+31)&~31;assert not any(data[io+ib:end])
    templates.append((header,(w,h,patterns,data[io:io+ib])))
assert end==len(data)
resource=[RESOURCE.unpack_from(data,32+16*i) for i in range(nr)]
assert len({(r[0],r[4]) for r in resource})==nr
for li,sz,w,h,pi in resource:
    assert li<nl and pi<np
    header,prepared=templates[li]
    assert sz==struct.unpack_from('<I',header,4)[0] and (w,h)==prepared[:2]
assert payload==sum(r[1]-32 for r in resource) and maximum==max(r[1] for r in resource)
lights=read_lights(source/'material_lights.txt');bounds=reachable_luma_bounds(source)
images={}
for fields in rows(source/'materials/materials.txt'):
    name,fmt,w,h,*key=fields;header=tuple(map(int,key[:4]))
    if header in images:continue
    proof=(source/'materials'/Path(name).with_suffix('.m2t')).read_bytes()
    images[header]=(fmt,layout(proof[64:],int(w),int(h)))
seen=set();last=b'';used_resources=set();used_palettes=set();used_layouts=set()
for i in range(nb):
    at=32+nr*16+i*16;packed=data[at:at+16]
    assert packed[:10]>last;last=packed[:10]
    *key,ri,color=BINDING.unpack(packed);key=tuple(key);header=key[:4]
    assert key in lights and key[4]>=bounds[header];seen.add(key)
    colors=lights[key]
    if ri==65535:
        assert not header[0]&0x4000 and color==colors[0];continue
    assert ri<nr and color==0 and header in images
    li,sz,w,h,pi=resource[ri];dt,prepared=templates[li]
    fmt,original=images[header]
    assert prepared==original
    assert bool(struct.unpack_from('<I',dt,16)[0]&(1<<27))==(fmt=='RGB565')
    expected=[]
    for c in colors:
        if fmt=='RGB565': expected.append(((c>>19)&31)<<11|((c>>10)&63)<<5|((c>>3)&31))
        else: expected.append(((c>>31)&1)<<15|((c>>19)&31)<<10|((c>>11)&31)<<5|((c>>3)&31))
    assert data[palbase+32*pi:palbase+32*(pi+1)]==struct.pack('<16H',*expected)
    if li not in used_layouts: assert texture(prepared,colors,fmt)[:32]==dt
    used_resources.add(ri);used_palettes.add(pi);used_layouts.add(li)
assert seen=={key for key in lights if key[4]>=bounds[key[:4]]}
assert used_resources==set(range(nr)) and used_palettes==set(range(np)) and used_layouts==set(range(nl))
print(f'PASS: {nb} exact source bindings, {nr} resource pairs, {nl} layouts, {np} palettes; {len(data)} bytes')
if len(sys.argv)>3:
    checker=sys.argv[3]
    result=subprocess.run([checker,sys.argv[2],'--cpu-backed'],capture_output=True,text=True)
    assert result.returncode==0,(result.stdout,result.stderr)
    crc=0
    for li,sz,w,h,pi in resource:
        header,(_,_,patterns,indices)=templates[li]
        palette=struct.unpack_from('<16H',data,palbase+32*pi)
        if patterns:
            words=[palette[(q>>shift)&15] for q in patterns for shift in (0,8,4,12)]
            blob=header+struct.pack(f'<{len(words)}H',*words)+indices
        else:
            blob=header+struct.pack(f'<{len(indices)}H',*(palette[i] for i in indices))
        blob+=bytes((-len(blob))&31);assert len(blob)==sz
        crc=zlib.crc32(blob,crc)
    assert f'INDEXED decoded crc32={crc:08x}' in result.stdout,result.stdout
    # Mutate every pointer/count family; reject before publication or expansion.
    changes=[(4,len(data)-32),(8,65535),(12,65535),(24,257),(28,16385),
             (16,payload+1),(20,maximum+32),(32,nl),(36,1),(44,np),
             (base,0),(base+32,len(data)+32),(base+36,0),(base+40,0xffffffff),
             (base+44,257),(base+48,1)]
    with tempfile.TemporaryDirectory() as tmp:
        path=Path(tmp)/'bad.dnt'
        invalid=[data[:31],data[:-1],data+b'\0'*32]
        for at,value in changes:
            broken=bytearray(data);struct.pack_into('<I',broken,at,value);invalid.append(broken)
        for broken in invalid:
            path.write_bytes(broken)
            rejected=subprocess.run([checker,str(path),'--cpu-backed'],capture_output=True,text=True)
            assert rejected.returncode==1,(rejected.returncode,rejected.stdout,rejected.stderr)
        # CPU-only format must not accidentally enter a resident-world path.
        path.write_bytes(data)
        assert subprocess.run([checker,str(path)],capture_output=True).returncode==1
    print(f'PASS: native decoded CRC32 {crc:08x}, {len(invalid)} malformed files and resident load rejected')
