"""Export a source-checked DNT4 candidate from explicit anchor/lighting choices.

Does not select anchors or approve visual quality. Recomputes source-relative
RGB errors (floor256 hypothesis) and requires exact alpha. The native checker
validates both input and output layouts. ROM-derived output stays local.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from check_materials import decode_texture16, material_blob
from prepare_materials import HEADER, RESOURCE, BINDING
from prepare_models import rows


def prepare(original_path, sources, anchor_path, texture_path, fit_path, checker, budget):
    if not 0 < budget <= 3308584:
        raise ValueError('resident budget exceeds existing world ceiling')
    subprocess.run([str(checker),str(original_path),'--cpu-backed'],check=True,capture_output=True)
    original = original_path.read_bytes()
    manifest = json.loads(anchor_path.read_text())
    textures = json.loads(texture_path.read_text())['textures']
    choices = json.loads(fit_path.read_text())['bindings']
    if hashlib.sha256(original).hexdigest() != manifest['source_pack_sha256']:
        raise ValueError('source pack changed')
    magic,_,nr,nb,*_ = HEADER.unpack_from(original)
    if magic not in (b'DNT2',b'DNT3'):
        raise ValueError('expected validated cropped source pack')
    if not 1 <= len(textures) <= 2048:
        raise ValueError('invalid resident resource count')
    root = texture_path.parent.resolve()
    blobs, decoded = [], []
    allocation = 0
    for i,row in enumerate(textures):
        path = (root/row['file']).resolve()
        if not path.is_relative_to(root) or row['resource'] != i:
            raise ValueError('invalid resource path/index')
        blob = path.read_bytes()
        if not 64 <= len(blob) <= 264224 or blob[9] or hashlib.sha256(blob).hexdigest() != row['sha256']:
            raise ValueError('changed or oversized texture')
        allocation += len(blob)
        if allocation > budget:
            raise ValueError('resident allocation exceeds budget')
        decoded.append(decode_texture16(blob)); blobs.append(blob)
    source_keys = {}
    for fields in rows(sources/'materials.txt'):
        if len(fields)!=9:
            raise ValueError('invalid source material row')
        name,fmt,w,h,*key = fields
        key = tuple(map(int,key))
        if key in source_keys or Path(name).name != name:
            raise ValueError('duplicate source key or invalid filename')
        source_keys[key] = name,fmt,int(w),int(h)
    # Each proof is checked once. No retained full-image pixel cache: large
    # source inventories need not all coexist in memory during export.
    proven = set()
    def source_pixels(key, expected_sha=None):
        name,fmt,w,h = source_keys[key]
        if (len(key)!=5 or not key[0]&0x4000 or
                fmt != ('RGB565' if ((key[0]>>13)&3)==2 else 'ARGB1555') or
                (w,h) != (32 << (key[0]&7),32 << ((key[0]>>3)&7))):
            raise ValueError('source dimensions disagree with material')
        tga = (sources/name).read_bytes()
        digest = hashlib.sha256(tga).hexdigest()
        if (expected_sha is not None and digest != expected_sha) or (
                len(tga)!=18+w*h*4 or tga[:12]!=b'\0\0\2'+bytes(9) or
                tga[12:18]!=struct.pack('<HHBB',w,h,32,0x28)):
            raise ValueError('source TGA metadata/hash mismatch')
        identity = key,digest
        if identity not in proven:
            material_blob([dict(header=key[:4],luma=key[4],width=w,height=h,
                source_file=name,source_sha256=digest,pack_index=0)],sources,1)
            proven.add(identity)
        return w,h,tga[18:]
    anchors = {}
    for entry in manifest['entries']:
        header = tuple(entry['header']); resource = entry['resource']
        if len(header)!=4 or type(resource) is not int or not 0<=resource<len(blobs):
            raise ValueError('invalid anchor identity')
        x,y,w,h = entry['crop']
        sw,sh,pixels = source_pixels((*header,entry['luma']),entry['source_sha256'])
        if source_keys[(*header,entry['luma'])][0] != entry['source_file']:
            raise ValueError('anchor source identity mismatch')
        if (any(type(v) is not int for v in (x,y,w,h)) or min(x,y)<0 or x%2 or y%2 or
                x+w>sw or y+h>sh or decoded[resource][:2]!=(w,h)):
            raise ValueError('invalid anchor crop')
        mode = struct.unpack_from('<I',blobs[resource],16)[0]
        if (((header[0]>>13)&3)==2) != bool(mode&(1<<27)):
            raise ValueError('anchor format disagrees with material')
        cropped = b''.join(pixels[((y+r)*sw+x)*4:((y+r)*sw+x+w)*4] for r in range(h))
        if cropped[3::4] != decoded[resource][2][3::4]:
            raise ValueError('anchor changed source alpha')
        key = header,resource
        if key in anchors and anchors[key]!=entry['crop']:
            raise ValueError('aliased anchor crop disagrees')
        anchors[key] = entry['crop']
    if {r for _,r in anchors} != set(range(len(blobs))):
        raise ValueError('unproven anchor resources')
    mapping = {(tuple(e['header']),e['luma']): e for e in choices}
    if len(mapping)!=len(choices):
        raise ValueError('duplicate fitted binding')
    bindings,colors,seen = [],[],set()
    error=samples=maximum=sse=0
    for i in range(nb):
        record = original[32+nr*16+i*16:32+nr*16+(i+1)*16]
        *header,luma,resource,crop = BINDING.unpack(record)
        if resource==65535:
            bindings.append(record); colors.append(0xffffffff); continue
        key = tuple(header),luma
        entry = mapping[key]; seen.add(key)
        selected = entry['resource']
        if type(selected) is not int or not 0<=selected<len(blobs):
            raise ValueError('invalid selected resource')
        ax,ay,aw,ah = anchors[tuple(header),selected]
        _,_,w,h,_ = RESOURCE.unpack_from(original,32+resource*16)
        x,y = crop&65535,crop>>16
        if entry['crop']!=[x,y,w,h] or not (ax<=x and ay<=y and x+w<=ax+aw and y+h<=ay+ah):
            raise ValueError('anchor does not cover original binding crop')
        factors = entry['bgr']
        if len(factors)!=3 or any(type(c) is not int or not 0<=c<=255 for c in factors):
            raise ValueError('invalid modulation factors')
        sw,sh,source = source_pixels((*header,luma))
        base = decoded[selected][2]
        for row in range(h):
            for col in range(w):
                a=((y-ay+row)*aw+x-ax+col)*4; b=((y+row)*sw+x+col)*4
                if base[a+3]!=source[b+3]:
                    raise ValueError('binding changed source alpha')
                if not source[b+3]: continue
                for c in range(3):
                    delta=abs(base[a+c]*factors[c]//256-source[b+c])
                    error+=delta; samples+=1; maximum=max(maximum,delta); sse+=delta*delta
        bindings.append(BINDING.pack(*header,luma,selected,ax|(ay<<16)))
        b,g,r=factors; colors.append(0xff000000|r<<16|g<<8|b)
    if seen!=mapping.keys():
        raise ValueError('fitted bindings do not match source pack exactly')
    start=(32+(len(blobs)+nb)*16+nb*4+31)&~31
    data=bytearray(HEADER.pack(b'DNT4',start+sum(map(len,blobs)),len(blobs),nb,
        sum(len(b)-32 for b in blobs),max(map(len,blobs)),0,0))
    offset=start
    for blob,(w,h,_) in zip(blobs,decoded):
        data+=RESOURCE.pack(offset,len(blob),w,h,0); offset+=len(blob)
    data+=b''.join(bindings)+struct.pack(f'<{nb}I',*colors)
    data+=bytes(start-len(data))+b''.join(blobs)
    if len(data)>4*1024*1024:
        raise ValueError('resident file exceeds native limit')
    with tempfile.NamedTemporaryFile(dir=original_path.parent) as temp:
        temp.write(data); temp.flush()
        subprocess.run([str(checker),temp.name],check=True,capture_output=True)
    return data,dict(resources=len(blobs),bindings=nb,solid_bindings=nb-len(seen),
        allocation_bytes=sum(map(len,blobs)),budget_bytes=budget,bytes=len(data),
        rgb_error_sum=error,rgb_samples=samples,rgb_max_error=maximum,rgb_sse=sse,
        rgb_mean_abs_error=error/max(1,samples),alpha_changed=0,
        sha256=hashlib.sha256(data).hexdigest(),
        caveat='Supplied bindings only; floor256 hypothesis, not raster/continuity/hardware approval.')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('original','sources','anchors','textures','fit','checker','output'):
        parser.add_argument(name,type=Path)
    parser.add_argument('--budget',type=int,default=3308584)
    args=parser.parse_args()
    if args.output.exists(): raise FileExistsError(args.output)
    data,report=prepare(args.original,args.sources,args.anchors,args.textures,args.fit,args.checker.resolve(),args.budget)
    with args.output.open('xb') as output: output.write(data)
    print(json.dumps(report,indent=2))
