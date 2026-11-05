"""ROM-free native streaming material pack, exact C reader and malformed inputs.

CHECKER [REAL_PACK NEW_INVALID_PACK] additionally makes a local last-resource
corruption fixture for the hardware rollback test. Never overwrites a file.
"""
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import zlib
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import prepare_materials as materials
from inspect_course import budget_cells

# Offline storage selection must honor the ALIGNED budget, including a mixed
# compressed/stored result, and leave every byte unchanged after decoding.
samples=[bytes(2048),random.Random(0x56513136).randbytes(4096),bytes(range(256))*8]
compressed=[zlib.compress(blob,9) for blob in samples]
minimum=sum((len(s)+31)&~31 for s in compressed)
for budget in (minimum,minimum+32,minimum+512,16384):
    streams,count,raw_bytes=materials.compress_textures(samples,budget)
    assert sum((len(s)+31)&~31 for s in streams)<=budget
    assert [zlib.decompress(s) for s in streams]==samples
    assert count==sum(s==zlib.compress(blob,0) for s,blob in zip(streams,samples))
    assert raw_bytes==sum(len(blob) for s,blob in zip(streams,samples) if s==zlib.compress(blob,0))
    assert (streams,count,raw_bytes)==materials.compress_textures(samples,budget)
assert materials.compress_textures(samples)[0]==compressed
assert 0<materials.compress_textures(samples,minimum)[1]<len(samples)

with tempfile.TemporaryDirectory(prefix="native-materials-") as temp:
    root = Path(temp); source = root / "source"; prepared = root / "prepared"
    (source / "materials").mkdir(parents=True); prepared.mkdir()
    entries, rows = [], []
    for i, rgb in enumerate((True, False)):
        dt = bytearray(32); dt[:4] = b"DcTx"
        struct.pack_into("<HHI", dt, 12, 32, 32, 0x40000012 | (int(rgb) << 27))
        dt += struct.pack("<4H", *([0xf800 if rgb else 0x7c00]*4)) + bytes([255])*256
        dt += bytes(-len(dt) % 32); struct.pack_into("<I", dt, 4, len(dt))
        name = f"texture_{i}.dt"; (prepared / name).write_bytes(dt)
        color = bytes((0,0,255,255 if rgb else 0))
        tga = bytearray(18); tga[2] = 2; struct.pack_into("<HHBB", tga, 12, 32,32,32,0x28)
        tga += color*1024
        for luma in ((100,120) if rgb else (255,)):
            src = f"source_{i}_{luma}.tga"
            (source / "materials" / src).write_bytes(tga)
            (source / "materials" / Path(src).with_suffix(".m2t")).write_bytes(color*16 + bytes(512))
            fmt = "RGB565" if rgb else "ARGB1555"; header = [0x4000 if rgb else 0x6000,0,0,0]
            entries.append(dict(file=name, source_file=src, format=fmt, width=32, height=32,
                header=header, luma=luma, sha256=hashlib.sha256(dt).hexdigest(),
                source_sha256=hashlib.sha256(tga).hexdigest(), compressed=True, vram_payload_bytes=len(dt)-32))
            rows.append(f"{src} {fmt} 32 32 {' '.join(map(str,header))} {luma}")
    (source / "materials/materials.txt").write_text("\n".join(rows)+"\n")
    (source / "solid_materials.txt").write_text("0 0 0 1 7 4278190080\n0 0 0 2 8 4294967295\n")
    manifest = dict(textures=entries, vram_payload_bytes=576)
    (prepared / "manifest.json").write_text(json.dumps(manifest))
    data, report = materials.prepare(source, prepared)
    assert report["textures"] == 2 and report["bindings"] == 5 and report["metadata_bytes"] == 112
    assert report["staging_bytes"] == 320 and report["alpha_changed_pixels"] == 0 and len(data) == 800
    # Exercise the full cell budget reader with real source proofs/DT bytes,
    # including two light keys sharing one resource and texture-free solids.
    (source / "manifest.json").write_text(json.dumps(dict(meshes=1, instances=1,
        unique_faces=1, unindexed_vertices=3, crtc_offset=[0,0], render_offset=[0,0])))
    (source / "mesh_0.txt").write_text("mode 1\nbounds 0 0 1 1 1 1\n"
        "face 3 258 16384 0 0 0 0 0 1 127 64 7 32\n"
        "v 0 0 1 0 0\nv 1 0 1 256 0\nv 1 1 1 256 256\n")
    (source / "instances.txt").write_text("0 8388608 1 0 1 0 0 0 1 0 0 0 1 0 0 10 288 288 0 0 1 -1 128 496 512 248 270\n")
    cell_ranges = "0 0 1\n" + "".join(f"{i} 1 0\n" for i in range(1,256))
    cell_materials = "0 16384 0 0 0 100\n0 16384 0 0 0 120\n0 24576 0 0 0 255\n0 0 0 0 1 7\n0 0 0 0 2 8\n"
    (source / "cells.txt").write_text(cell_ranges)
    (source / "cell_materials.txt").write_text(cell_materials)
    budget = budget_cells(source, prepared)
    assert budget["full_course"] == dict(textures=2, payload_bytes=576, estimated_allocation_bytes=640)
    assert all(p["peak_adjacent_overlap"]["estimated_allocation_bytes"] == 640 for p in budget["neighborhoods"])
    for name, malformed in (
        ("cells.txt", cell_ranges.replace("1 1 0\n", "1 0 0\n", 1)),
        ("cells.txt", cell_ranges.replace("0 0 1\n", "0 0 -1\n", 1)),
        ("cells.txt", cell_ranges.rsplit("255 1 0\n", 1)[0]),
        ("cell_materials.txt", cell_materials + "0 16384 0 0 0 100\n"),
        ("cell_materials.txt", cell_materials.replace("0 16384", "1 16384", 1)),
        ("cell_materials.txt", cell_materials.replace("0 0 0 0 2 8\n", "")),
        ("cell_materials.txt", cell_materials.replace("0 0 0 0 2 8", "0 0 0 0 2 9")),
        ("cell_materials.txt", cell_materials.replace("0 16384 0 0 0 120", "0 16384 0 0 0 121")),
    ):
        path = source / name
        original = path.read_text(); path.write_text(malformed)
        try: budget_cells(source, prepared)
        except ValueError: pass
        else: raise AssertionError("malformed cell dependency/range accepted")
        finally: path.write_text(original)
    print("PASS: cell ranges, shared-resource transition costs and eight malformed dependencies/ranges")
    uv_bounds = "16384 0 0 0 100 80 80 88 88\n16384 0 0 0 120 80 80 88 88\n24576 0 0 0 255 80 80 88 88\n"
    (source / "material_uv.txt").write_text(uv_bounds)
    cropped = budget_cells(source, prepared, root / "crops")
    assert cropped["full_course"] == dict(textures=2, payload_bytes=64, estimated_allocation_bytes=128)
    assert cropped["crop_candidates"]["cropped_bindings"] == 3
    assert len(list((root / "crops").glob("*.dt"))) == 2
    (source / "material_uv.txt").write_text(uv_bounds.replace("120 80 80 88 88", "120 0 0 255 255"))
    aliases = budget_cells(source, prepared, root / "alias-crops")
    assert aliases["full_course"] == dict(textures=2, payload_bytes=320, estimated_allocation_bytes=384)
    assert aliases["crop_candidates"]["cropped_bindings"] == 1
    assert aliases["crop_candidates"]["retained_alias_groups"] == 1
    for bad in (uv_bounds.split("\n",1)[1], uv_bounds + uv_bounds.splitlines()[0]+"\n",
                uv_bounds.replace("80 80 88 88", "89 80 88 88", 1),
                uv_bounds.replace("80 80 88 88", "-1 80 88 88", 1)):
        (source / "material_uv.txt").write_text(bad)
        try: budget_cells(source, prepared, root / "bad-crops")
        except ValueError: pass
        else: raise AssertionError("malformed material UV bounds accepted")
        assert not (root / "bad-crops").exists()
    (source / "material_uv.txt").write_text(uv_bounds)
    try: budget_cells(source, prepared, root / "crops")
    except FileExistsError: pass
    else: raise AssertionError("existing crop assets overwritten")
    print("PASS: exact VQ crop cost/dedup, UV coverage/ranges and overwrite refusal")
    file = root / "materials.dnt"
    def accepted(candidate, cpu=False):
        file.write_bytes(candidate)
        result = subprocess.run([sys.argv[1], str(file)]+(["--cpu-backed"] if cpu else []), capture_output=True, text=True)
        assert result.returncode in (0,1), result.stderr
        if result.returncode: assert result.stdout == "REJECT: invalid native materials\n", result.stdout
        return result.returncode == 0
    assert accepted(data)
    crop_data, crop_info = materials.prepare(source, prepared, root / "crops")
    assert crop_data[:4] == b"DNT2" and crop_info["vram_payload_bytes"] == 64
    assert accepted(crop_data)
    # DNT4 keeps sorted bindings/crop origins intact, adding a separate ARGB
    # table. Legacy packs remain byte-identical and need no lighting table.
    _,_,nr,nb,payload,staging,_,_ = materials.HEADER.unpack_from(crop_data)
    table_at = 32 + (nr+nb)*16
    start = (table_at+nb*4+31)&~31
    old_start = materials.RESOURCE.unpack_from(crop_data,32)[0]
    lit = bytearray(crop_data[:table_at])
    lit[:4] = b'DNT4'
    solid_indices, textured_indices = [], []
    for i in range(nb):
        resource = materials.BINDING.unpack_from(crop_data,32+nr*16+i*16)[5]
        if resource == 65535: solid_indices.append(i)
        else: textured_indices.append(i)
        lit += struct.pack('<I',0xffffffff if resource == 65535 else 0xff123456+i)
    lit += bytes(start-len(lit)) + crop_data[old_start:]
    struct.pack_into('<I',lit,4,len(lit))
    for i in range(nr):
        offset = materials.RESOURCE.unpack_from(lit,32+i*16)[0]
        struct.pack_into('<I',lit,32+i*16,offset+start-old_start)
    assert accepted(lit) and accepted(lit,cpu=True)
    # Only CPU-backed DNT4 shares the car DNT5 binding domain. Keep the
    # ordinary resident and legacy ceilings; exercise the exact 16-bit edge.
    ro,rs,rw,rh,_=materials.RESOURCE.unpack_from(crop_data,32)
    for count in (8193,65534,65535):
        records=b''.join(materials.BINDING.pack(0,0,0,i//256,i%256,65535,0xff123456)
                         for i in range(count))
        for magic in (b'DNT4',b'DNT2'):
            colors=struct.pack(f'<{count}I',*([0xffffffff]*count)) if magic==b'DNT4' else b''
            begin=(48+len(records)+len(colors)+31)&~31
            large=bytearray(materials.HEADER.pack(magic,begin+rs,1,count,rs-32,rs,0,0))
            large+=materials.RESOURCE.pack(begin,rs,rw,rh,0)+records+colors
            large+=bytes(begin-len(large))+crop_data[ro:ro+rs]
            assert not accepted(large)
            assert accepted(large,cpu=True)==(magic==b'DNT4' and count<65535)
    assert lit[32+nr*16:table_at] == crop_data[32+nr*16:table_at]
    assert lit[start:] == crop_data[old_start:]
    for value in (0xff000000,0xffffffff):
        edge = bytearray(lit)
        struct.pack_into('<I',edge,table_at+textured_indices[0]*4,value)
        assert accepted(edge)
    for offset,value in ((table_at+textured_indices[0]*4,0x7f123456),
                         (table_at+solid_indices[0]*4,0xff123456),
                         (32,old_start),(44,1),(24,1),(28,1)):
        bad = bytearray(lit); struct.pack_into('<I',bad,offset,value)
        assert not accepted(bad) and not accepted(bad,cpu=True)
    for end in (table_at,table_at+nb*4-1,start-1,len(lit)-1):
        bad = bytearray(lit[:end]); struct.pack_into('<I',bad,4,len(bad))
        assert not accepted(bad)
    bad = bytearray(lit); bad[:4] = b'DNT2'
    assert not accepted(bad)
    assert accepted(crop_data) and accepted(data)
    print('PASS: DNT4 separate vertex color, unchanged crops/solids/payload, malformed tables and legacy compatibility')
    import prepare_resident_materials as resident
    crop_entries = json.loads((root/'crops/crops.json').read_text())['textures_metadata']
    resources = {e['file']:i for i,e in enumerate({e['file']:e for e in crop_entries}.values())}
    anchors = dict(source_pack_sha256=hashlib.sha256(crop_data).hexdigest(),entries=[
        dict(header=e['header'],luma=e['luma'],resource=resources[e['file']],
             crop=[*e['crop_origin'],e['width'],e['height']],
             source_file=e['source_file'],source_sha256=e['source_sha256']) for e in crop_entries])
    texture_rows = [dict(resource=i,file='crops/'+name,
        sha256=hashlib.sha256((root/'crops'/name).read_bytes()).hexdigest()) for name,i in resources.items()]
    fit_rows = [dict(header=e['header'],luma=e['luma'],resource=e['resource'],
        crop=e['crop'],bgr=[255,255,255]) for e in anchors['entries']]
    anchor_file=root/'resident-anchors.json'; texture_file=root/'resident-textures.json'; fit_file=root/'resident-fit.json'
    anchor_file.write_text(json.dumps(anchors)); texture_file.write_text(json.dumps(dict(textures=texture_rows)))
    fit_file.write_text(json.dumps(dict(bindings=fit_rows)))
    def resident_pack(budget=128):
        file.write_bytes(crop_data)
        return resident.prepare(file,source/'materials',anchor_file,texture_file,fit_file,Path(sys.argv[1]).resolve(),budget)
    resident_data,resident_info=resident_pack()
    assert accepted(resident_data) and resident_data[:4]==b'DNT4'
    assert resident_data[32:table_at]==lit[32:table_at] and resident_data[start:]==lit[start:]
    assert resident_info['allocation_bytes']==128 and resident_info['alpha_changed']==0
    assert (resident_info['rgb_error_sum'],resident_info['rgb_samples'],resident_info['rgb_max_error'])==(128,384,1)
    def rejected_resident(budget=128):
        try: resident_pack(budget)
        except (ValueError,KeyError,subprocess.CalledProcessError): pass
        else: raise AssertionError('invalid resident inputs accepted')
    rejected_resident(127)
    for malformed in (fit_rows+fit_rows[:1],fit_rows[1:],
                      [dict(fit_rows[0],bgr=[256,0,0]),*fit_rows[1:]],
                      [dict(fit_rows[0],crop=[0,0,8,8]),*fit_rows[1:]]):
        fit_file.write_text(json.dumps(dict(bindings=malformed))); rejected_resident()
    fit_file.write_text(json.dumps(dict(bindings=fit_rows)))
    proof=source/'materials'/Path(crop_entries[0]['source_file']).with_suffix('.m2t')
    proof_bytes=proof.read_bytes(); proof.write_bytes(bytes([1])+proof_bytes[1:])
    rejected_resident(); proof.write_bytes(proof_bytes)
    for malformed in ([dict(texture_rows[0],file='../outside.dt'),*texture_rows[1:]],
                      [dict(texture_rows[0],sha256='0'*64),*texture_rows[1:]]):
        texture_file.write_text(json.dumps(dict(textures=malformed))); rejected_resident()
    texture_file.write_text(json.dumps(dict(textures=texture_rows)))
    alpha_entry=next(e for e in crop_entries if e['header'][0]==0x6000)
    alpha_index=resources[alpha_entry['file']]; alpha_path=root/'crops'/alpha_entry['file']
    alpha_original=alpha_path.read_bytes(); alpha_bad=bytearray(alpha_original)
    struct.pack_into('<H',alpha_bad,32,struct.unpack_from('<H',alpha_bad,32)[0]|0x8000)
    alpha_path.write_bytes(alpha_bad)
    bad_rows=[dict(e) for e in texture_rows]
    bad_rows[alpha_index]['sha256']=hashlib.sha256(alpha_bad).hexdigest()
    texture_file.write_text(json.dumps(dict(textures=bad_rows)))
    rejected_resident()
    alpha_path.write_bytes(alpha_original)
    texture_file.write_text(json.dumps(dict(textures=texture_rows)))
    assert resident_pack()[0]==resident_data
    existing=root/'do-not-replace.dnt'; existing.write_bytes(b'preserve existing asset')
    command=[sys.executable,resident.__file__,str(file),str(source/'materials'),str(anchor_file),
        str(texture_file),str(fit_file),str(Path(sys.argv[1]).resolve()),str(existing)]
    assert subprocess.run(command,capture_output=True).returncode!=0
    assert existing.read_bytes()==b'preserve existing asset'
    print('PASS: reusable resident exporter, exact source proof, measured RGB/alpha, coverage/budget/path rejection and native roundtrip')
    # Mixed VQ/raw16 uses the SAME reader/cache/lifecycle. No new exporter
    # policy here: synthetic bytes isolate format admission from art quality.
    for rgb in (False, True):
        for w,h in ((8,16),(32,32),(64,128),(256,512),(512,512)):
            raw=bytearray(32);raw[:4]=b'DcTx'
            mode=(int(rgb)<<27)|((w.bit_length()-4)<<3)|(h.bit_length()-4)
            struct.pack_into('<HHI',raw,12,w,h,mode)
            raw+=struct.pack('<H',0xf800 if rgb else 0x7c00)*(w*h)
            struct.pack_into('<I',raw,4,len(raw))
            vq=(prepared/'texture_0.dt').read_bytes()
            raw_header=(0x4000 if rgb else 0x6000)|max(0,w.bit_length()-6)|(max(0,h.bit_length()-6)<<3)
            def mixed(candidate,deflate=False):
                blobs=[vq,candidate]
                encoded=[zlib.compress(blob) if deflate else blob for blob in blobs]
                bindings=sorted([materials.BINDING.pack(0x4000,0,0,0,100,0,0),
                    materials.BINDING.pack(raw_header,0,0,1,255,1,0)],key=lambda b:b[:10])
                size=96+sum((len(b)+31)&~31 for b in encoded)
                result=bytearray(materials.HEADER.pack(b'DNT3' if deflate else b'DNT2',
                    size,2,2,sum(len(b)-32 for b in blobs),max(map(len,blobs)),0,0))
                at=96
                for blob,stored,(rw,rh) in zip(blobs,encoded,((32,32),(w,h))):
                    result+=materials.RESOURCE.pack(at,len(blob),rw,rh,len(stored) if deflate else 0)
                    at+=(len(stored)+31)&~31
                result+=b''.join(bindings)
                for blob in encoded:result+=blob+bytes(-len(blob)%32)
                assert len(result)==size
                return result
            for deflate in (False,True):
                if len(raw)>264224:
                    assert not accepted(mixed(raw,deflate),deflate)
                    continue # Raw does not raise the existing staging ceiling.
                assert accepted(mixed(raw,deflate),deflate)
                for at,fmt,value in ((10,'B',1),(16,'I',mode|(1<<31)),
                    (16,'I',mode|(1<<30)),(16,'I',mode|(1<<26)),
                    (16,'I',mode|(1<<25)),(16,'I',0x28000000|(mode&63)),
                    (16,'I',0x30000000|(mode&63)),(16,'I',mode^(1<<27)),
                    (12,'H',w*2)):
                    bad=bytearray(raw);struct.pack_into('<'+fmt,bad,at,value)
                    assert not accepted(mixed(bad,deflate),deflate)
                for bad in (raw[:-32],raw+bytes(32)):
                    bad=bytearray(bad);struct.pack_into('<I',bad,4,len(bad))
                    assert not accepted(mixed(bad,deflate),deflate)
    print('PASS: mixed VQ/raw RGB565/ARGB1555, square/rectangular DNT2/DNT3 and malformed raw layouts')
    packed, packed_info = materials.prepare(source, prepared, root / "crops", cpu_backed=True, deflate=True)
    assert packed[:4] == b"DNT3" and packed_info["vram_payload_bytes"] == 64
    assert accepted(packed, True) and not accepted(packed)
    assert packed_info["backing_bytes"] < crop_info["backing_bytes"]
    for i in range(2):
        at,n,w,h,stored = materials.RESOURCE.unpack_from(packed,32+i*16)
        old,nold,*_ = materials.RESOURCE.unpack_from(crop_data,32+i*16)
        assert n == nold and zlib.decompress(packed[at:at+stored]) == crop_data[old:old+n]
    assert packed[64:144] == crop_data[64:144] # Every binding, crop origin, solid colour.
    fast, fast_info = materials.prepare(source, prepared, root / "crops", cpu_backed=True,
                                        deflate=True, fast_backing_bytes=1024)
    assert accepted(fast, True) and fast_info["stored_block_resources"] == 2
    assert fast_info["backing_bytes"] <= 1024 and fast[64:144] == packed[64:144]
    for i in range(2):
        at,n,w,h,stored = materials.RESOURCE.unpack_from(fast,32+i*16)
        old,nold,*_ = materials.RESOURCE.unpack_from(crop_data,32+i*16)
        assert n == nold and zlib.decompress(fast[at:at+stored]) == crop_data[old:old+n]
    for budget in (-1,0,1,9*1024*1024+1):
        try: materials.prepare(source,prepared,root / "crops",cpu_backed=True,
                               deflate=True,fast_backing_bytes=budget)
        except ValueError: pass
        else: raise AssertionError("invalid/insufficient fast backing budget accepted")
    try: materials.prepare(source,prepared,root / "crops",cpu_backed=True,fast_backing_bytes=1024)
    except ValueError: pass
    else: raise AssertionError("fast backing without DNT3 accepted")
    for cpu,crops in ((False,root / "crops"),(True,None)):
        try: materials.prepare(source,prepared,crops,cpu_backed=cpu,deflate=True)
        except ValueError: pass
        else: raise AssertionError("compressed format without CPU backing/crop semantics accepted")
    def streams(payloads):
        out=bytearray(packed[:160])
        for i,payload in enumerate(payloads):
            struct.pack_into("<I",out,32+i*16,len(out))
            struct.pack_into("<I",out,44+i*16,len(payload))
            out += payload; out += bytes(-len(out)%32)
        struct.pack_into("<I",out,4,len(out))
        return out
    payloads=[]
    for i in range(2):
        at,_,_,_,n=materials.RESOURCE.unpack_from(packed,32+i*16)
        payloads.append(packed[at:at+n])
    first=payloads[0]
    for payload in (b"x",first[:-1],first+b"x",first+first,
                    first[:-1]+bytes([first[-1]^1]),zlib.compress(b"x"),
                    zlib.compress(zlib.decompress(first)+bytes(32))):
        assert not accepted(streams([payload,payloads[1]]),True)
    wrong=bytearray(zlib.decompress(first)); wrong[32+8]=0 # Missing small codebook prefix.
    assert not accepted(streams([zlib.compress(wrong),payloads[1]]),True)
    for at,value in ((0,0),(16,0),(36,0xffffffe0),(44,0),(44,0xffffffff),(48,160)):
        bad=bytearray(packed); struct.pack_into("<I",bad,at,value)
        assert not accepted(bad,True)
    at,_,_,_,n=materials.RESOURCE.unpack_from(packed,32)
    assert (n&31)!=0
    bad=bytearray(packed); bad[at+n]=1
    assert not accepted(bad,True)
    print("PASS: lossless DNT3 bytes/bindings, CPU-only guard, repeated in-memory decode and corrupt/oversized/trailing streams")
    # Maximum admitted DcTx: bounded full-output inflate, repeatedly reset;
    # exercises more than zlib's 32 KiB sliding-window length.
    huge=bytearray(32); huge[:4]=b"DcTx"; huge[10]=255
    struct.pack_into("<HHI",huge,12,1024,1024,0x4800003f)
    huge += bytes(2048)+bytes(range(256))*1024
    struct.pack_into("<I",huge,4,len(huge))
    stored=zlib.compress(huge,9)
    large=materials.HEADER.pack(b"DNT3",64+((len(stored)+31)&~31),1,1,len(huge)-32,len(huge),0,0)
    large+=materials.RESOURCE.pack(64,len(huge),1024,1024,len(stored))
    large+=materials.BINDING.pack(0x402d,0,0,0,100,0,0)
    large+=stored+bytes(-len(stored)%32)
    assert len(huge)==264224 and accepted(large,True)
    # Exercise the actual 9 MiB CPU guard with valid stored streams on both
    # sides. GPU payload is not resident: these fixtures stay CPU-only.
    raw=zlib.compress(huge,0)
    span=(len(raw)+31)&~31
    for count in (35,36):
        start=(32+(count+1)*16+31)&~31
        repeated=bytearray(materials.HEADER.pack(b"DNT3",start+count*span,count,1,
                            count*(len(huge)-32),len(huge),0,0))
        for i in range(count):
            repeated+=materials.RESOURCE.pack(start+i*span,len(huge),1024,1024,len(raw))
        repeated+=materials.BINDING.pack(0x402d,0,0,0,100,0,0)
        repeated+=bytes(start-len(repeated))
        repeated+=(raw+bytes(span-len(raw)))*count
        assert 8*1024*1024 < count*span < 10*1024*1024
        assert accepted(repeated,True) == (count==35)
    print("PASS: stored-block byte parity, explicit backing budget and actual 9 MiB reader boundary")
    # Same texture header can have different lighting materials. Retain the
    # union (minimum ambient), fractional ambient truncates, and solids obey
    # the same rule. Bytes/UVs of surviving bindings must remain unchanged.
    mesh_path = source / "mesh_0.txt"
    saved_mesh = mesh_path.read_text()
    saved_info = (source / "manifest.json").read_text()
    faces = []
    for header, ambient in (([16384,0,0,0],110.75), ([16384,0,0,0],130),
                            ([24576,0,0,0],255), ([0,0,0,1],8), ([0,0,0,2],8)):
        faces.append(f"face 3 258 {' '.join(map(str,header))} 0 0 1 127 {ambient} 7 32\n"
                     "v 0 0 1 0 0\nv 1 0 1 256 0\nv 1 1 1 256 256\n")
    mesh_path.write_text("mode 1\nbounds 0 0 1 1 1 1\n"+"".join(faces))
    info = json.loads(saved_info); info.update(unique_faces=5, unindexed_vertices=15)
    (source / "manifest.json").write_text(json.dumps(info))
    assert materials.reachable_luma_bounds(source)[(16384,0,0,0)] == 110
    for directory, original in ((None,data), (root / "crops",crop_data)):
        filtered, result = materials.prepare(source, prepared, directory, reachable_lights=True)
        assert accepted(filtered) and result["unreachable_bindings_removed"] == 2
        h = materials.HEADER.unpack_from(filtered)
        assert h[2:4] == (2,3)
        def payloads(blob):
            h = materials.HEADER.unpack_from(blob)
            return [blob[o:o+n] for o,n,*_ in (materials.RESOURCE.unpack_from(blob,32+i*16) for i in range(h[2]))]
        assert payloads(filtered) == payloads(original)
        keys = [materials.BINDING.unpack_from(filtered,32+h[2]*16+i*16)[:5] for i in range(h[3])]
        assert set(keys) == {(16384,0,0,0,120),(24576,0,0,0,255),(0,0,0,2,8)}
    reachable_mesh = mesh_path.read_text()
    mesh_path.write_text(reachable_mesh.replace(" 110.75 ", " 121 "))
    pruned, result = materials.prepare(source, prepared, reachable_lights=True)
    assert accepted(pruned) and result["textures"] == 1 and result["bindings"] == 2
    assert result["vram_payload_bytes"] == 288 and result["unreachable_bindings_removed"] == 3
    mesh_path.write_text(reachable_mesh.replace(" 110.75 ", " -1 "))
    try: materials.prepare(source, prepared, reachable_lights=True)
    except ValueError: pass
    else: raise AssertionError("invalid ambient accepted")
    mesh_path.write_text(saved_mesh)
    (source / "manifest.json").write_text(saved_info)
    try: materials.prepare(source, prepared, reachable_lights=True)
    except ValueError: pass
    else: raise AssertionError("unowned material binding silently dropped")
    print("PASS: conservative ambient bounds, shared-header union, solids and unchanged VQ payloads")
    textured_at = next(at for at in range(64,144,16) if struct.unpack_from("<H",crop_data,at)[0] & 0x4000)
    for offset,value in ((0,0), (textured_at+12,1), (textured_at+12,32),
                         (textured_at+12,32<<16), (textured_at+12,0xffffffff)):
        bad = bytearray(crop_data); struct.pack_into("<I",bad,offset,value)
        assert not accepted(bad)
    bad = bytearray(crop_data); bad[:4] = b"DNT1"
    assert not accepted(bad) # DNT1 must not interpret crop coordinates as ARGB.
    crop_manifest = root / "crops/crops.json"
    saved = crop_manifest.read_text()
    corrupted = json.loads(saved)
    corrupted["textures_metadata"][0]["crop_origin"][0] = 10
    crop_manifest.write_text(json.dumps(corrupted))
    try: materials.prepare(source, prepared, root / "crops")
    except ValueError: pass
    else: raise AssertionError("crop origin lost its required UV halo")
    crop_manifest.write_text(saved)
    damaged_file = root / "crops" / json.loads(saved)["textures_metadata"][0]["file"]
    saved_dt = damaged_file.read_bytes(); damaged_dt = bytearray(saved_dt); damaged_dt[32] ^= 1
    damaged_file.write_bytes(damaged_dt)
    try: materials.prepare(source, prepared, root / "crops")
    except ValueError: pass
    else: raise AssertionError("changed cropped VQ accepted")
    damaged_file.write_bytes(saved_dt)
    invalid = [b"", data[:31], data[:80], data[:-1], data+b"x"]
    for offset, fmt, value in [
        (0,"I",0), (4,"I",0xffffffff), (8,"I",2049), (12,"I",8193), (16,"I",0),
        (20,"I",1), (24,"I",1), (28,"I",1), (32,"I",128), (36,"I",0xffffffff),
        (40,"H",16), (42,"H",3), (44,"I",1), (48,"I",160), (56,"H",2048),
        (64+8,"H",256), (64+10,"H",0), (64+12,"I",0), (144,"B",1),
        (160,"I",0), (164,"I",64), (168,"B",1), (169,"B",1), (170,"B",255),
        (172,"H",16), (176,"I",0x48000012 | (1<<31)), # Mips.
        (176,"I",0x68000012), # PAL4: intentionally excluded from this VQ16 pack.
        (176,"I",0x70000012), # PAL8.
        (176,"I",0x08000012), # Raw flag with a VQ-sized payload.
        (176,"I",0x4a000012), # Stride.
        (176,"I",0x40000012), # RGB resource contradicts binding renderer.
        (200,"B",254), # Index before the small codebook.
    ]:
        bad = bytearray(data); struct.pack_into("<"+fmt, bad, offset, value); invalid.append(bad)
    bad = bytearray(data); bad[80:96] = bad[64:80]; invalid.append(bad) # Duplicate binding key.
    assert all(not accepted(bad) for bad in invalid)
    # End-to-end source proof -> mixed full-image pack -> real native reader.
    for i in (0,1):
        name=f'texture_{i}.dt';target=prepared/name;saved=target.read_bytes()
        raw=bytearray(32);raw[:4]=b'DcTx'
        struct.pack_into('<HHI',raw,12,32,32,0x08000012 if i==0 else 0x12)
        raw+=struct.pack('<H',0xf800 if i==0 else 0x7c00)*1024
        struct.pack_into('<I',raw,4,len(raw));target.write_bytes(raw)
        mixed_entries=[dict(e,compressed=False,sha256=hashlib.sha256(raw).hexdigest(),
            vram_payload_bytes=len(raw)-32) if e['file']==name else e for e in entries]
        mixed_manifest=dict(textures=mixed_entries,vram_payload_bytes=2048+288)
        (prepared/'manifest.json').write_text(json.dumps(mixed_manifest))
        mixed_data,mixed_info=materials.prepare(source,prepared)
        assert accepted(mixed_data) and mixed_info['alpha_changed_pixels']==0
        assert mixed_info['vram_payload_bytes']==2336 and mixed_info['staging_bytes']==2080
        at,n,*_=materials.RESOURCE.unpack_from(mixed_data,32+i*16)
        assert mixed_data[at:at+n]==raw
        from prepare_material_crops import prepare_crops
        crop_path=root/f'mixed-crops-{i}'
        _,crop_report=prepare_crops(source,prepared,mixed_entries,crop_path)
        assert crop_report['cropped_bindings']==3
        for deflate in (False,True):
            cropped_pack,cropped_info=materials.prepare(source,prepared,crop_path,
                cpu_backed=deflate,deflate=deflate)
            assert accepted(cropped_pack,deflate) and cropped_info['vram_payload_bytes']==160
        cropped_manifest=crop_path/'crops.json';crop_records=json.loads(cropped_manifest.read_text())
        raw_crop=next(e for e in crop_records['textures_metadata'] if e['original_file']==name)
        cropped_file=crop_path/raw_crop['file'];saved_crop=cropped_file.read_bytes()
        corrupted=bytearray(saved_crop);corrupted[32]^=1;cropped_file.write_bytes(corrupted)
        try: materials.prepare(source,prepared,crop_path)
        except ValueError: pass
        else: raise AssertionError('modified raw crop pixels accepted')
        cropped_file.write_bytes(saved_crop)
        if i==0:
            mixed_entries[0]['compressed']=True # Only the earlier alias lies.
            (prepared/'manifest.json').write_text(json.dumps(mixed_manifest))
            try: materials.prepare(source,prepared)
            except ValueError: pass
            else: raise AssertionError('contradictory alias metadata accepted')
            mixed_entries[0]['compressed']=False
        # Manifest compression declaration must agree with the actual bytes.
        for e in mixed_entries:
            if e['file']==name:e['compressed']=True
        (prepared/'manifest.json').write_text(json.dumps(mixed_manifest))
        try: materials.prepare(source,prepared)
        except ValueError: pass
        else: raise AssertionError('raw/VQ manifest mismatch accepted')
        target.write_bytes(saved)
    (prepared/'manifest.json').write_text(json.dumps(manifest))
    assert materials.prepare(source,prepared)[0]==data
    print('PASS: mixed raw/VQ exporter, exact source/byte proof, native reader and original VQ pack byte parity')
    # A valid raw texture can fit the total budget but exceed one staging slot.
    large_source=root/'raw-large-source';large_prepared=root/'raw-large-prepared'
    (large_source/'materials').mkdir(parents=True);large_prepared.mkdir()
    tga=bytearray(18);tga[2]=2;struct.pack_into('<HHBB',tga,12,512,512,32,0x28)
    tga+=bytes((0,0,255,255))*(512*512)
    (large_source/'materials/red.tga').write_bytes(tga)
    (large_source/'materials/red.m2t').write_bytes(bytes((0,0,255,255))*16+bytes(512*512//2))
    (large_source/'materials/materials.txt').write_text('red.tga RGB565 512 512 16420 0 0 0 255\n')
    (large_source/'solid_materials.txt').write_text('')
    raw=bytearray(32);raw[:4]=b'DcTx';struct.pack_into('<HHI',raw,12,512,512,0x08000036)
    raw+=struct.pack('<H',0xf800)*(512*512);struct.pack_into('<I',raw,4,len(raw))
    (large_prepared/'red.dt').write_bytes(raw)
    large_entry=dict(file='red.dt',source_file='red.tga',format='RGB565',width=512,height=512,
        header=[16420,0,0,0],luma=255,compressed=False,
        sha256=hashlib.sha256(raw).hexdigest(),source_sha256=hashlib.sha256(tga).hexdigest())
    (large_prepared/'manifest.json').write_text(json.dumps(dict(textures=[large_entry],vram_payload_bytes=len(raw)-32)))
    try: materials.prepare(large_source,large_prepared)
    except ValueError as error: assert str(error)=='texture exceeds native staging limit'
    else: raise AssertionError('raw exporter exceeded native staging ceiling')
    print('PASS: valid oversized raw source rejected by unchanged per-resource staging ceiling')
    manifest["textures"] = entries[1:]
    (prepared / "manifest.json").write_text(json.dumps(manifest))
    try: materials.prepare(source, prepared)
    except ValueError: pass
    else: raise AssertionError("missing source variant accepted")
    print(f"PASS: DNT1/DNT2 coverage, exact crop proof, UV origin bounds, aliases/solids, lifecycle and {len(invalid)} malformed DNT1 files")

if len(sys.argv) == 4:
    original = Path(sys.argv[2])
    subprocess.run([sys.argv[1], str(original)], check=True)
    damaged = bytearray(original.read_bytes())
    resources = struct.unpack_from("<I", damaged, 8)[0]
    last = struct.unpack_from("<I", damaged, 32 + (resources-1)*16)[0]
    damaged[last+8] = 1 # Unsupported DcTx version; earlier resources remain valid.
    with Path(sys.argv[3]).open("xb") as out:
        out.write(damaged)
    assert subprocess.run([sys.argv[1], sys.argv[3]]).returncode == 1
    print(f"Hardware rollback fixture: final resource {resources-1} invalid; do not use as game assets")
