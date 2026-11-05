"""ROM-free square/rectangular VQ16 and small-codebook byte checks."""
import importlib.util
from pathlib import Path
import struct
import sys

spec = importlib.util.spec_from_file_location("materials", Path(__file__).resolve().parents[1] / "tools/check_materials.py")
materials = importlib.util.module_from_spec(spec)
spec.loader.exec_module(materials)
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from prepare_material_crops import crop_axis, crop_vq, crop_texture16

assert crop_axis(80,152,64) == (8,16)
assert crop_axis(0,152,64) == (0,64)  # Keep wrap/edge behavior, not a guessed clamp.
assert crop_axis(8,512,64) == (0,64)
assert crop_axis(400,480,64) == (48,16)
for bad in ((8,0,64),(-1,8,64),(0,65536,64),(0,8,63)):
    try: crop_axis(*bad)
    except ValueError: pass
    else: raise AssertionError("bad UV interval accepted")
for size in (8,16,32,64,128,256,512,1024):
    for low in range(0,size*16,17):
        for high in (low,low+8,low+size*2):
            start,length = crop_axis(low,high,size)
            assert start % 2 == 0 and 8 <= length <= size and not length & (length-1)
            assert 0 <= start <= size-length
            if length < size:
                assert start <= low//8-1 <= high//8+1 < start+length
block_order = [[0, 2, 8, 10], [1, 3, 9, 11], [4, 6, 12, 14], [5, 7, 13, 15]]
cases = 0
for w, h in [(8, 8), (8, 16), (16, 8), (16,16), (16,32), (32,16)]:
    for rgb in (0, 1):
        for entries in (32, 256):
            data = bytearray(32)
            data[:4] = b"DcTx"; data[10] = entries - 1
            # The stride hint is ignored when the actual stride mode bit is off.
            mode = 0x40000040 | (rgb << 27) | ((w.bit_length()-4) << 3) | (h.bit_length()-4)
            struct.pack_into("<HHI", data, 12, w, h, mode)
            for i in range(entries):
                for pixel in range(4):
                    word = ((i & 31) << (11 if rgb else 10)) | (i >> 5) | (pixel << 5) | (0x8000 if not rgb and pixel < 3 else 0)
                    data += struct.pack("<H", word)
            data += bytes(256-entries + i % entries for i in range(w*h//4))
            data += bytes(-len(data) % 32)
            struct.pack_into("<I", data, 4, len(data))
            rw, rh, decoded = materials.decode_vq16(data)
            assert materials.decode_texture16(data) == (rw, rh, decoded)
            assert (rw, rh) == (w, h)
            for y in range(h):
                for x in range(w):
                    pen = (x//8+y//8)*16 + block_order[(y%8)//2][(x%8)//2]
                    if min(w,h) == 16:
                        pen = (x//16+y//16)*64 + ((x%16)//8)*32 + ((y%16)//8)*16 + block_order[(y%8)//2][(x%8)//2]
                    local = (y & 1) + 2*(x & 1)
                    pen %= entries
                    blue, red = pen >> 5, pen & 31
                    expected = bytes(((blue << 3) | (blue >> 2), local*4 if rgb else local*8,
                                      (red << 3) | (red >> 2), 255 if rgb or local < 3 else 0))
                    assert decoded[(y*w+x)*4:(y*w+x+1)*4] == expected
            cases += 1
            for x,y in {(0,0),(w-8,h-8),(2 if w>8 else 0,2 if h>8 else 0)}:
                cw,ch,pixels = materials.decode_vq16(crop_vq(bytes(data),(x,y,8,8)))
                assert (cw,ch) == (8,8)
                assert pixels == b"".join(decoded[((y+r)*w+x)*4:((y+r)*w+x+8)*4] for r in range(8))
            assert crop_vq(bytes(data),(0,0,w,h)) == bytes(data)
            for box in ((1,0,8,8),(0,0,7,8),(w,0,8,8),(-2,0,8,8)):
                try: crop_vq(bytes(data),box)
                except ValueError: pass
                else: raise AssertionError("bad VQ crop accepted")
            invalid = [data[:31], data[:-1], data + bytes(32)]
            bad = bytearray(data); bad[16] ^= 1; invalid.append(bad)
            bad = bytearray(data); bad[19] |= 128; invalid.append(bad)
            if entries < 256:
                bad = bytearray(data); bad[32+entries*8] = 0; invalid.append(bad)
            for bad in invalid:
                try:
                    materials.decode_vq16(bad)
                except ValueError:
                    pass
                else:
                    raise AssertionError("invalid VQ accepted")
print(f"PASS: {cases} rectangular/square VQ16 layouts, exact crops, UV halos, alpha, codebook offsets and malformed files")

def quadrant_order(x, y, side):
    # Independent recursive quadrant walk, not the decoder's bit spreading.
    if side == 1:
        yield x,y
    else:
        half=side//2
        for dx,dy in ((0,0),(0,half),(half,0),(half,half)):
            yield from quadrant_order(x+dx,y+dy,half)

raw_cases=0
for w,h in ((8,8),(8,16),(16,8),(64,128)):
    for rgb in (False,True):
        data=bytearray(32);data[:4]=b'DcTx'
        mode=(int(rgb)<<27)|((w.bit_length()-4)<<3)|(h.bit_length()-4)
        struct.pack_into('<HHI',data,12,w,h,mode)
        expected=bytearray(w*h*4);side=min(w,h);seen=set()
        for y0 in range(0,h,side):
            for x0 in range(0,w,side):
                for x,y in quadrant_order(x0,y0,side):
                    assert (x,y) not in seen;seen.add((x,y))
                    word=(x*419+y*1307)&65535
                    data+=struct.pack('<H',word)
                    b=word&31;g=(word>>5)&(63 if rgb else 31);r=(word>>(11 if rgb else 10))&31
                    expected[(y*w+x)*4:(y*w+x+1)*4]=bytes((b*8+b//4,
                        g*4+g//16 if rgb else g*8+g//4,r*8+r//4,255 if rgb or word&32768 else 0))
        assert len(seen)==w*h
        struct.pack_into('<I',data,4,len(data))
        assert materials.decode_texture16(data)==(w,h,bytes(expected))
        for x,y in {(0,0),(w-8,h-8),(2 if w>8 else 0,2 if h>8 else 0)}:
            cropped=crop_texture16(data,(x,y,8,8))
            assert materials.decode_texture16(cropped)==(8,8,b''.join(
                expected[((y+r)*w+x)*4:((y+r)*w+x+8)*4] for r in range(8)))
            assert not struct.unpack_from('<I',cropped,16)[0] & (1<<30)
        assert crop_texture16(data,(0,0,w,h))==data
        for box in ((1,0,8,8),(0,0,7,8),(w,0,8,8),(-2,0,8,8)):
            try: crop_texture16(data,box)
            except ValueError: pass
            else: raise AssertionError('invalid raw crop accepted')
        try: crop_vq(data,(0,0,w,h))
        except ValueError: pass
        else: raise AssertionError('VQ-only crop accepted raw16')
        try: materials.decode_vq16(data)
        except ValueError: pass
        else: raise AssertionError('VQ-only caller accepted raw16')
        invalid=[data[:31],data[:-1],data+bytes(32)]
        for at,fmt,value in ((10,'B',1),(16,'I',mode|0x40000000),
            (16,'I',mode|0x80000000),(16,'I',mode|0x04000000),
            (16,'I',mode|0x02000000),(16,'I',mode|0x30000000),
            (16,'I',mode|0x28000000),(12,'H',w*2)):
            bad=bytearray(data);struct.pack_into('<'+fmt,bad,at,value);invalid.append(bad)
        for bad in invalid:
            try: materials.decode_texture16(bad)
            except ValueError: pass
            else: raise AssertionError('invalid raw16 accepted')
        raw_cases+=1
print(f'PASS: {raw_cases} raw16 formats/layouts, recursive twiddle oracle, exact RGB/alpha and malformed inputs')
