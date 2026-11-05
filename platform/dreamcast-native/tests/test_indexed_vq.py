"""Exact post-quantization VQ/raw16 checks, optionally with owned intro assets."""
from pathlib import Path
import struct
import sys
import subprocess
import tempfile
import ctypes
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from indexed_vq import layout, texture
from check_materials import decode_texture16
from prepare_models import rows
from inspect_intro_lights import read_lights

temporary = tempfile.TemporaryDirectory() if len(sys.argv)>2 and sys.argv[2]!='-' else None
native = ctypes.CDLL(sys.argv[3]).nm_check_indexed_texture if len(sys.argv)>3 else None
if native:
    native.restype=ctypes.c_size_t
    native.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_void_p,
                     ctypes.c_uint,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p]


def native_check(prepared,colors,fmt,data):
    buffers=[]
    def aligned(blob):
        buffer=ctypes.create_string_buffer(len(blob)+31);buffers.append(buffer)
        address=(ctypes.addressof(buffer)+31)&~31
        ctypes.memmove(address,blob,len(blob))
        return address
    palette=[]
    for color in colors:
        b,g,r,a=struct.pack('<I',color)
        palette.append(((r>>3)<<11|(g>>2)<<5|(b>>3)) if fmt=='RGB565' else
                       ((a>>7)<<15|(r>>3)<<10|(g>>3)<<5|(b>>3)))
    _,_,patterns,indices=prepared
    expected=bytes([0xa5])*(len(data)+32)
    out=aligned(expected)
    header=aligned(data[:32])
    args=[out,len(data),header,aligned(struct.pack(f'<{len(patterns)}H',*patterns)),
          len(patterns),aligned(indices),len(indices),aligned(struct.pack('<16H',*palette))]
    for index,value in ((1,len(data)-1),(0,out+1),(6,len(indices)-1),(4,257),
                        (5,args[5]+1),(7,args[7]+1)):
        bad=args.copy();bad[index]=value
        assert native(*bad)==0 and ctypes.string_at(out,len(expected))==expected
    mutations=[(header,0)]
    if len(patterns)<256: mutations.append((args[5],0 if patterns else 16))
    for address,value in mutations:
        byte=ctypes.c_uint8.from_address(address);saved=byte.value;byte.value=value
        assert native(*args)==0 and ctypes.string_at(out,len(expected))==expected
        byte.value=saved
    assert native(*args)==len(data)
    assert ctypes.string_at(out,len(data))==data
    assert ctypes.string_at(out+len(data),32)==bytes([0xa5])*32

def check(indices,w,h,colors,fmt,prepared=None):
    prepared = prepared or layout(indices,w,h)
    data = texture(prepared,colors,fmt)
    dw,dh,pixels = decode_texture16(data)
    assert (dw,dh)==(w,h)
    palette=[]
    for c in colors:
        b,g,r,a=struct.pack('<I',c)
        b=(b>>3)*8+(b>>5);r=(r>>3)*8+(r>>5)
        g=(g>>2)*4+(g>>6) if fmt=='RGB565' else (g>>3)*8+(g>>5)
        palette.append(bytes((b,g,r,a)))
    quads=struct.unpack(f'<{len(indices)//2}H',indices)
    expected=bytearray(w*h*4)
    for y in range(h):
        for x in range(w):
            symbol=(quads[(y//2)*(w//2)+x//2]>>(4*((y&1)*2+(x&1))))&15
            expected[(y*w+x)*4:(y*w+x+1)*4]=palette[symbol]
    assert pixels==expected
    if native: native_check(prepared,colors,fmt,data)
    if temporary:
        # Feed the exact produced DcTx to the production C material reader.
        h0=(0x4000 if fmt=='RGB565' else 0x6000)|max(0,w.bit_length()-6)|(max(0,h.bit_length()-6)<<3)
        pack=(struct.pack('<4s7I',b'DNT2',64+len(data),1,1,len(data)-32,len(data),0,0)+
              struct.pack('<IIHHI',64,len(data),w,h,0)+
              struct.pack('<6HI',h0,0,0,0,0,0,0)+data)
        path=Path(temporary.name)/'texture.dnt';path.write_bytes(pack)
        result=subprocess.run([sys.argv[2],str(path)],capture_output=True,text=True)
        assert result.returncode==0,(result.stdout,result.stderr)
    return bool(prepared[2]),len(data)


colors=tuple(0xff000000|i*0x112233 for i in range(16))
for w,h in ((8,8),(16,8),(8,32),(64,64)):
    indices=struct.pack(f'<{w*h//4}H',*(i%400 for i in range(w*h//4)))
    for fmt in ('RGB565','ARGB1555'):
        assert check(indices,w,h,colors,fmt)[0]==(w*h//4<=256)
for luma in range(256):
    check(bytes.fromhex('1032')*16,8,8,tuple((0 if i==15 else 0xff000000)|luma*0x010101 for i in range(16)),'ARGB1555')
for args in ((b'',8,8),(bytes(32),7,8),(bytes(32),2048,8)):
    try: layout(*args)
    except ValueError: continue
    raise AssertionError('malformed layout accepted')
if len(sys.argv)>1:
    root=Path(sys.argv[1]);lights=read_lights(root/'material_lights.txt')
    checked=set();vq=raw=total=0
    for fields in rows(root/'materials/materials.txt'):
        name,fmt,w,h,*key=fields;w,h=int(w),int(h);header=tuple(map(int,key[:4]))
        if header in checked:continue
        checked.add(header)
        indices=(root/'materials'/Path(name).with_suffix('.m2t')).read_bytes()[64:]
        prepared=layout(indices,w,h)
        for luma in (0,47,127,255):
            compressed,size=check(indices,w,h,lights[(*header,luma)],fmt,prepared)
            vq+=compressed;raw+=not compressed;total+=size
    print(f'PASS: {len(checked)} real headers, {vq} VQ / {raw} raw light variants, exact16-bit pixels/alpha, {total} bytes')
print('PASS: VQ codebook order, rectangular twiddle,256-luma reuse,raw fallback and invalid dimensions')
if temporary: temporary.cleanup()
