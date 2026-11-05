"""ROM-free regression for differing crop footprints in residency analysis."""
from pathlib import Path
import struct
import tempfile
from check_resident_lighting import union_crop, crop_weights, inspect

assert union_crop(32,32,{(0,0,8,8),(16,8,8,8)}) == (0,0,32,16)
assert union_crop(64,64,{(42,42,8,8),(54,54,8,8)}) == (32,32,32,32)
assert union_crop(32,32,{(24,24,8,8)}) == (24,24,8,8)
try:
    union_crop(32,32,{(26,0,8,8)})
except AssertionError:
    pass
else:
    raise AssertionError('out-of-source crop accepted')

with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    # Metadata-only fixture: this analyzer does not read compressed payloads.
    data=bytearray(struct.pack('<4s7I',b'DNT3',96,2,2,0,0,0,0))
    data+=struct.pack('<IIHHI',96,64,8,8,0)*2
    indices=bytearray(512)
    for y in range(32):
        for x in range(32):
            index=1 if x<8 and y<8 else 2 if 16<=x<24 and 8<=y<16 else 0
            at=2*((y//2)*16+x//2)
            word=struct.unpack_from('<H',indices,at)[0]
            word|=index<<(4*(2*(y&1)+(x&1)))
            struct.pack_into('<H',indices,at,word)
    for resource,(luma,x,y) in enumerate(((128,0,0),(255,16,8))):
        data+=struct.pack('<6HI',0x4000,0,0,0,luma,resource,x|(y<<16))
        palette=bytearray(bytes((0,0,0,255))*16)
        for i,color in ((1,5 if luma==128 else 10),(2,90 if luma==128 else 100)):
            palette[4*i:4*i+4]=bytes((color,color,color,255))
        state=palette+indices
        assert crop_weights(state,32,(0,0,8,8))=={1:64}
        assert crop_weights(state,32,(16,8,8,8))=={2:64}
        (root/f'{luma}.m2t').write_bytes(state)
    (root/'pack.dnt').write_bytes(data)
    (root/'materials.txt').write_text('128.tga RGB565 32 32 16384 0 0 0 128\n255.tga RGB565 32 32 16384 0 0 0 255\n')
    report=inspect(root/'pack.dnt',root)
    assert report['families_with_different_crops']==1
    assert report['proposed_full_codebook_vq_bytes_with_kos']==2208
    assert report['worst_families'][0]['crop']==[0,0,32,16]
    assert report['bindings']==2 and report['exact_bindings']==1
    assert report['ideal_modulation_max_error']==1
    assert report['ideal_modulation_mean_abs_error']==0.5
print('PASS resident-lighting union footprints, edge shifts, invalid crops, per-binding weights and floor256 model')
