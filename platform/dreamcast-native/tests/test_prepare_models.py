"""ROM-free packer/real C reader checks; optional source-dir roundtrip.

python3 platform/dreamcast-native/tests/test_prepare_models.py build/check_native_meshpack [SOURCE_DIR [INTRO_ROM_IMAGES]]
"""
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

spec = importlib.util.spec_from_file_location("models", Path(__file__).resolve().parents[1] / "tools/prepare_models.py")
models = importlib.util.module_from_spec(spec)
spec.loader.exec_module(models)


def roundtrip(source, data):
    h = models.HEADER.unpack_from(data)
    mesh_base = models.HEADER.size
    vertex_base = mesh_base + h[2] * models.MESH.size
    face_base = vertex_base + h[3] * models.VERTEX.size
    material_base = face_base + h[4] * models.FACE.size
    instance_base = material_base + h[5] * models.MATERIAL.size
    checked = 0
    for i in range(h[2]):
        mesh = models.MESH.unpack_from(data, mesh_base + i * models.MESH.size)
        rows = models.rows(source / f"mesh_{i}.txt")
        assert int(models.row(rows, "mode", 1)[0]) == mesh[10]
        assert models.floats(models.row(rows, "bounds", 6)) == mesh[4:10]
        for j in range(mesh[3]):
            src = models.row(rows, "face", 13)
            face = models.FACE.unpack_from(data, face_base + (mesh[2] + j) * models.FACE.size)
            assert (int(src[0]), int(src[1])) == (face[5], face[7])
            assert models.floats(src[6:9]) == face[8:11]
            material = models.MATERIAL.unpack_from(data, material_base + face[4] * models.MATERIAL.size)
            assert tuple(map(int, src[2:6])) == material[:4]
            assert models.floats([src[9], src[10], src[12]]) == material[4:7]
            assert int(src[11]) == material[7]
            for k in range(face[5]):
                values = models.row(rows, "v", 5)
                offset = vertex_base + (mesh[0] + face[k]) * models.VERTEX.size
                expected = models.VERTEX.pack(*models.floats(values[:3]), *map(int, values[3:]))
                assert data[offset:offset + models.VERTEX.size] == expected
                checked += 1
        assert next(rows, None) is None
    for i, values in enumerate(models.rows(source / "instances.txt")):
        expected = models.INSTANCE.pack(*map(int, values[:4]), *models.floats(values[4:21]),
                                        *map(int, values[21:]), 0, 0, 0, 0)
        offset = instance_base + i * models.INSTANCE.size
        assert data[offset:offset + models.INSTANCE.size] == expected
    return checked


with tempfile.TemporaryDirectory(prefix="daytona-model-test-") as temp:
    root = Path(temp)
    (root / "manifest.json").write_text(json.dumps(dict(meshes=1, instances=1, unique_faces=2,
        unindexed_vertices=7, crtc_offset=[0, 128], render_offset=[0, 128])))
    verts = ["v 0 0 1 0 0", "v 1 0 1 256 0", "v 1 1 1 256 256", "v 0 1 1 0 256"]
    (root / "mesh_0.txt").write_text("\n".join([
        "mode 1", "bounds 0 0 1 1 1 1", "face 4 131329 16384 0 0 0 0 0 1 127 64 7 32",
        *verts, "face 3 131330 16384 0 0 0 0 0 1 127 64 7 32", *verts[:3]]) + "\n")
    (root / "instances.txt").write_text("0 8388608 1 0 1 0 0 0 1 0 0 0 1 0 0 10 288 288 0 0 1 -1 128 496 512 248 270\n")
    data, info = models.prepare(root)
    assert info["vertices"] == 4 and info["materials"] == 1 and len(data) == 400
    assert roundtrip(root, data) == 7
    pack = root / "test.dnm"
    def accepted(candidate):
        pack.write_bytes(candidate)
        result = subprocess.run([sys.argv[1], str(pack)], capture_output=True, text=True)
        assert result.returncode in (0, 1), result.stderr
        if result.returncode:
            assert result.stdout == "REJECT: invalid DNM1/2/3/4/5\n", result.stderr
        return result.returncode == 0
    assert accepted(data)
    # Independent authored-window strip: no ROM payload or recorded pose.
    window_root=root/'windows';window_root.mkdir()
    for address,uv,header,faces in ((0x1c8,0x5168,0x9c,7),(0x241,0x51c8,0xb8,5),(0x13b,0x50f8,0x78,9)):
        (window_root/'manifest.json').write_text(json.dumps(dict(meshes=1,instances=1,unique_faces=faces,
            unindexed_vertices=4*faces,crtc_offset=[0,128],render_offset=[0,128])))
        (window_root/'instances.txt').write_text((root/'instances.txt').read_text().replace('8388608',str(address)))
        lines=['mode 1',f'bounds 0 0 1 {faces} 1 1']
        main=bytearray(0xa00000)
        at=0x9c0000+4*address
        struct.pack_into('<6f',main,at,0,0,1,0,1,1);at+=24
        struct.pack_into('<4H',main,0x9e0000+2*header,0x4009,0,0,0)
        for i in range(faces):
            attr=0x20201
            struct.pack_into('<I9f',main,at,attr,0,0,1,i+1,0,1,i+1,1,1);at+=40
            struct.pack_into('<8H',main,0x9e0000+2*uv+i*16,0,0,0,512,512,512,512,0)
            lines += [f'face 4 {attr} 16393 0 0 0 0 0 1 127 47 7 239',
                      f'v {i} 1 1 0 0',f'v {i} 0 1 512 0',
                      f'v {i+1} 0 1 512 512',f'v {i+1} 1 1 0 512']
        (window_root/'mesh_0.txt').write_text('\n'.join(lines)+'\n')
        window_data=models.prepare(window_root)[0]
        models.check_player_windows(window_data,main,address)
        assert accepted(models.local_models(window_data,[address],windows_rom=main))
        try: models.local_models(window_data,[address])
        except ValueError: pass
        else: raise AssertionError('unverified RAM player windows accepted')
        wh=models.HEADER.unpack_from(window_data)
        wfb=128+wh[3]*models.VERTEX.size
        wmb=wfb+wh[4]*models.FACE.size
        for offset in (128,140,wfb+12,wfb+16,wmb):
            bad=bytearray(window_data);bad[offset]^=1
            try: models.check_player_windows(bad,main,address)
            except ValueError: pass
            else: raise AssertionError('noncanonical window position/UV/attribute/normal/material accepted')
    del main
    shared=models.share_positions(data)
    assert accepted(shared) and shared[8:len(data)]==data[8:]
    assert struct.unpack_from('<4H',shared,len(data))==(0,1,2,3)
    grouped=models.group_faces(shared,1)
    assert accepted(grouped) and grouped==models.group_faces(data,1)
    assert models.HEADER.unpack_from(grouped)[0]==0x344d4e44 and models.GROUP.size==64
    assert models.HEADER.unpack_from(grouped)[7]==2
    group_count=models.HEADER.unpack_from(grouped)[7]
    group_base=len(grouped)-group_count*models.GROUP.size
    # normal_length is the serialized-zero final word, filled only at runtime.
    for g in range(group_count):
        offset=group_base+(g+1)*models.GROUP.size-struct.calcsize('<I')
        for bits in (0x00000000,0x80000000,0x00000001,0x3f800000,0x7fc00000):
            bad=bytearray(grouped);struct.pack_into('<I',bad,offset,bits)
            assert accepted(bad)==(bits==0),(g,offset,hex(bits))
    assert roundtrip(root,grouped)==7  # Geometry/UV/material/instance order unchanged.
    assert models.MESH.unpack_from(grouped,64)[11:13]==(0,2)
    assert accepted(models.group_faces(data,256))
    for bad_size in (0,-1,257,1.5,True):
        try: models.group_faces(data,bad_size)
        except ValueError: pass
        else: raise AssertionError('invalid group size accepted')
    # DNM4 bounds/cones are untrusted input: reject undercoverage, NaN,
    # reversed bounds, invalid cone fields, range references and table tails.
    for offset,fmt,value in ((28,'I',0),(28,'I',3),(108,'I',1),(112,'I',0),
                             (112,'I',3),(116,'I',1),(220,'I',2),
                             (408,'f',0.5),(408,'f',float('nan')),
                             (420,'f',-1),(444,'f',-1),(464,'I',2),
                             (468,'I',1),(472,'f',-1)):
        bad=bytearray(grouped);struct.pack_into('<'+fmt,bad,offset,value)
        assert not accepted(bad),(offset,value)
    assert not accepted(grouped[:-1]) and not accepted(grouped+b'x')
    odd=root/'odd';odd.mkdir()
    (odd/'manifest.json').write_text(json.dumps(dict(meshes=1,instances=1,unique_faces=1,
        unindexed_vertices=3,crtc_offset=[0,128],render_offset=[0,128])))
    (odd/'mesh_0.txt').write_text('\n'.join(['mode 1','bounds 0 0 1 1 1 1',
        'face 3 131330 16384 0 0 0 0 0 1 127 64 7 32',*verts[:3]])+'\n')
    (odd/'instances.txt').write_text((root/'instances.txt').read_text())
    odd_shared=models.share_positions(models.prepare(odd)[0])
    odd_grouped=models.group_faces(odd_shared)
    assert len(odd_shared)%4==2 and accepted(odd_grouped)
    assert odd_grouped[len(odd_shared):len(odd_shared)+2]==bytes(2)
    bad=bytearray(odd_grouped);bad[len(odd_shared)]=1
    assert not accepted(bad)
    # Exercise actual recursive spatial partitioning, including a short tail.
    window_grouped=models.group_faces(window_data,4)
    assert accepted(window_grouped) and models.HEADER.unpack_from(window_grouped)[7]==3
    assert roundtrip(window_root,window_grouped)==36
    assert window_grouped==models.group_faces(window_data,4)
    # Exact XYZ with different UVs shares projection, never texture coordinates.
    aliases=bytearray(data)
    aliases[144:156]=aliases[128:140]
    aliases[160:172]=aliases[128:140]
    shared_aliases=models.share_positions(aliases)
    assert accepted(shared_aliases) and shared_aliases[64:len(data)]==aliases[64:]
    assert struct.unpack_from('<4H',shared_aliases,len(data))==(0,0,0,3)
    for offset,value in ((400,1),(402,65535),(404,1),(406,0)):
        bad=bytearray(shared_aliases); struct.pack_into('<H',bad,offset,value)
        assert not accepted(bad)  # forward/range/chain/wrong XYZ
    assert not accepted(shared[:-1]) and not accepted(shared+b'x')
    zeros=bytearray(aliases); struct.pack_into('<I',zeros,144,0x80000000)
    distinct=models.share_positions(zeros)
    assert accepted(distinct) and struct.unpack_from('<4H',distinct,len(data))==(0,1,0,3)
    bad=bytearray(distinct); struct.pack_into('<H',bad,len(data)+2,0)
    assert not accepted(bad)  # -0 cannot borrow +0's transform.
    (root/'cells.txt').write_text('0 0 1\n'+''.join(f'{i} 1 0\n' for i in range(1,256)))
    world=models.world_cells(data,root)
    assert accepted(world) and world[64:288]==data[64:288]
    assert models.HEADER.unpack_from(world)[8:12]==(0,0,0,0)
    assert models.INSTANCE.unpack_from(world,288)[4:16]==(1,0,0,0,1,0,0,0,1,0,0,0)
    for bad_cells in ('', '0 0 0\n'+''.join(f'{i} 0 0\n' for i in range(1,256)),
                      '0 1 1\n'+''.join(f'{i} 2 0\n' for i in range(1,256))):
        (root/'cells.txt').write_text(bad_cells)
        try: models.world_cells(data,root)
        except ValueError: pass
        else: raise AssertionError('incomplete/overlapping world cell ranges accepted')
    local = models.local_model(data, 0x800000)
    assert accepted(local)
    assert local[64:288] == data[64:288]  # Exact geometry and materials.
    assert models.INSTANCE.unpack_from(local,288)[4:16] == (1,0,0,0,1,0,0,0,1,0,0,0)
    for address in (0,0x800001,0xc00000):
        try:
            models.local_model(data,address)
        except ValueError:
            pass
        else:
            raise AssertionError('missing/non-ROM mesh accepted')
    original_manifest=(root/'manifest.json').read_text()
    original_instances=(root/'instances.txt').read_text()
    (root/'mesh_1.txt').write_text((root/'mesh_0.txt').read_text())
    manifest=json.loads(original_manifest)
    manifest.update(meshes=2,instances=2,unique_faces=4,unindexed_vertices=14)
    (root/'manifest.json').write_text(json.dumps(manifest))
    second=original_instances.replace('0 8388608','1 8388864',1)
    (root/'instances.txt').write_text(original_instances+second)
    (root/'cells.txt').write_text('0 0 2\n'+''.join(f'{i} 2 0\n' for i in range(1,256)))
    two=models.prepare(root)[0]
    grouped_two=models.group_faces(two,1)
    assert accepted(grouped_two) and roundtrip(root,grouped_two)==14
    assert models.MESH.unpack_from(grouped_two,128)[11:13]==(2,2)
    bad=bytearray(grouped_two)
    second_face=64+2*64+8*16+2*32
    struct.pack_into('<I',bad,second_face+28,0)  # Cross-mesh group forbidden.
    assert not accepted(bad)
    assert accepted(models.share_positions(two))
    assert struct.unpack_from('<8H',models.share_positions(two),len(two))==(0,1,2,3,0,1,2,3)
    assert accepted(models.world_cells(two,root))
    bad=bytearray(two); struct.pack_into('<f',bad,len(bad)-models.INSTANCE.size+52,11)
    try: models.world_cells(bad,root)
    except ValueError: pass
    else: raise AssertionError('different per-object world transform accepted')
    selected=models.local_model(models.prepare(root)[0],0x800100)
    assert accepted(selected) and selected[64:288]==data[64:288]  # Rebased second-mesh ranges.
    parts=models.local_models(two,[0x800100,0x800000,0x800100])
    assert accepted(parts) and accepted(models.share_positions(parts))
    h=models.HEADER.unpack_from(parts)
    assert h[2:7]==(2,8,4,1,3)  # Shared materials and repeated mesh, not repeated geometry.
    assert models.MESH.unpack_from(parts,64)[:4]==(0,4,0,2)
    assert models.MESH.unpack_from(parts,128)[:4]==(4,4,2,2)
    at=len(parts)-3*models.INSTANCE.size
    for i,(mesh,address) in enumerate(((0,0x800100),(1,0x800000),(0,0x800100))):
        instance=models.INSTANCE.unpack_from(parts,at+i*models.INSTANCE.size)
        assert instance[:4]==(mesh,address,0,0)
        assert instance[4:16]==(1,0,0,0,1,0,0,0,1,0,0,0)
    for addresses in ([],[0x800000,0x800001],[0x800000,0xc00000]):
        try: models.local_models(two,addresses)
        except ValueError: pass
        else: raise AssertionError('empty/partially missing/invalid parts accepted')
    (root/'instances.txt').write_text(original_instances+second.replace('8388864','8388608',1))
    try:
        models.local_model(models.prepare(root)[0],0x800000)
    except ValueError:
        pass
    else:
        raise AssertionError('ambiguous model accepted')
    (root/'manifest.json').write_text(original_manifest)
    (root/'instances.txt').write_text(original_instances)
    rejected = [b"", data[:63], data[:100], data[:-1], data + b"x"]
    # Explicit offsets independently check the C layout and every record family.
    for offset, fmt, value in [
        (0, "I", 0), (4, "I", models.MAX_BYTES + 1), (8, "I", 0xffffffff),
        (24, "I", 0), (28, "I", 1), (64, "I", 1), (68, "I", 65537),
        (80, "f", float("nan")), (108, "I", 1), (128, "f", 2),
        (128, "f", float("inf")), (192, "H", 65535), (200, "H", 1),
        (202, "B", 5), (203, "B", 1), (204, "I", 0), (208, "f", float("nan")),
        (230, "H", 1), (264, "f", 256), (276, "I", 256), (280, "I", 1),
        (288, "I", 1), (296, "I", 256), (300, "I", 2), (304, "f", float("nan")),
        (372, "h", 2048), (396, "I", 1),
    ]:
        bad = bytearray(data)
        struct.pack_into("<" + fmt, bad, offset, value)
        rejected.append(bad)
    assert all(not accepted(bad) for bad in rejected)
    # A failed prepare must not accept an inconsistent source bound.
    mesh_path = root / "mesh_0.txt"
    mesh_path.write_text(mesh_path.read_text().replace("bounds 0 0 1 1 1 1", "bounds 0 0 1 2 1 1"))
    try:
        models.prepare(root)
    except ValueError:
        pass
    else:
        raise AssertionError("inconsistent source bounds accepted")
    if len(sys.argv) > 2:
        source = Path(sys.argv[2])
        real_data, info = models.prepare(source)
        assert accepted(real_data)
        print(f"PASS: {roundtrip(source, real_data)} source face vertices roundtrip exactly; {info['bytes']} bytes")
        if len(sys.argv) > 3:
            sys.path.insert(0, str(Path(models.__file__).parent))
            local = models.intro_cars(real_data, Path(sys.argv[3]))
            assert accepted(local)
            h = models.HEADER.unpack_from(local)
            assert 92 <= h[2] <= 110 and h[6] == 110
            start = len(local)-h[6]*models.INSTANCE.size
            bindings = [models.INSTANCE.unpack_from(local, start+i*models.INSTANCE.size)
                        for i in range(h[6])]
            assert len({v[1] for v in bindings}) == 110
            from inspect_course import MAIN_BASE, words, intro_stages, intro_car_models
            from prepare_intro import model_arrays
            program, main = models.car_images(Path(sys.argv[3]))
            graph = intro_stages(main, words(main, MAIN_BASE, 0x2865a60, 1)[0])
            catalog = intro_car_models(program, main, graph)
            ids, bodies, special, wheels, selectors = model_arrays(catalog)
            assert ids == [v[1] for v in bindings]
            assert catalog['special_phases']==(0x288d670,0x288d670,0x288d684,0x288d684,
                                               0x288d698,0x288d698,0x288d6ac,0x288d6ac)
            lists = {item['pointer']: item['objects'][0][0] for item in catalog['lists']}
            assert catalog['shadow']==0x288b7a8 and lists[catalog['shadow']]==0x8d9e42
            assert catalog['crash_parts']==(0x288f600,0x288f5ec)
            assert [lists[p] for p in catalog['crash_parts']]==[0xa14c10,0xa14bd3]
            for address,faces in ((0xa14c10,1),(0xa14bd3,5)):
                attachment=bindings[ids.index(address)]
                mesh=models.MESH.unpack_from(local,models.HEADER.size+attachment[0]*models.MESH.size)
                assert mesh[3]==faces
            shadow=bindings[ids.index(0x8d9e42)]
            mesh=models.MESH.unpack_from(local,models.HEADER.size+shadow[0]*models.MESH.size)
            assert mesh[3]==4
            vb=models.HEADER.size+h[2]*models.MESH.size
            fb=vb+h[3]*models.VERTEX.size
            mb=fb+h[4]*models.FACE.size
            for i in range(mesh[1]):
                vertex=models.VERTEX.unpack_from(local,vb+(mesh[0]+i)*models.VERTEX.size)
                assert vertex[1]==0 # Source shadow lies on local ground plane.
            for i in range(mesh[3]):
                face=models.FACE.unpack_from(local,fb+(mesh[2]+i)*models.FACE.size)
                assert face[5]==4
                material=models.MATERIAL.unpack_from(local,mb+face[4]*models.MATERIAL.size)
                assert material[:4]==(0,0,0,0x40) # Solid, no checker or blending bits.
            for car in catalog['cars']:
                indices = [special] if car['source_updater']==0xd05c else bodies[car['id']]
                assert [ids[i] for i in indices] == [lists[p] for p in car['lists']]
            for pointers, indices in ((catalog['wheels'], wheels), (catalog['selectors'], selectors)):
                assert [ids[i] for i in indices] == [lists[p] for p in pointers]
            for v in bindings:
                assert v[2:4] == (0, 0)
                assert v[4:16] == (1,0,0,0,1,0,0,0,1,0,0,0)
                assert v[16:21] == (1,1,0,0,0)
            print(f"PASS: 110 intro bindings incl16wheel/8special phases/2canonical windows/shadow/2crash parts, {h[2]} shared meshes, no captured poses; {len(local)} bytes")
    print(f"PASS: exact indexed pack, shared material, lifecycle and {len(rejected)} malformed files rejected")
