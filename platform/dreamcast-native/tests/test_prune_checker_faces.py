"""ROM-free checker omission and real C reader check.

python3 platform/dreamcast-native/tests/test_prune_checker_faces.py build/dc-native-cone-fix.2WaB51/check_meshpack
"""
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

tool=Path(__file__).resolve().parents[1]/'tools/prepare_models.py'
spec=importlib.util.spec_from_file_location('models',tool)
models=importlib.util.module_from_spec(spec)
spec.loader.exec_module(models)


def tables(data):
    h=models.HEADER.unpack_from(data)
    vb=models.HEADER.size+h[2]*models.MESH.size
    fb=vb+h[3]*models.VERTEX.size
    mb=fb+h[4]*models.FACE.size
    ib=mb+h[5]*models.MATERIAL.size
    return h,vb,fb,mb,ib


vertices=[models.VERTEX.pack(*v) for v in (
    (9,9,1,901,902),(0,0,1,10,11),(1,0,1,20,21),(0,1,1,30,31),
    (1,0,1,40,41),(8,8,1,801,802),
    (20,0,1,0,0),(21,0,1,100,101),(21,1,1,200,201),(20,1,1,300,301))]
faces=[models.FACE.pack(*f) for f in (
    (4,2,3,0,1,3,0,0x20302,-0.0,0,1,0),
    (0,2,5,3,0,4,0,0x20101,0,0,1,0),
    (1,3,2,4,2,4,0,0x20101,0,1,0,0),
    (0,1,2,0,0,3,0,0x20202,1,0,0,0),
    (2,3,1,0,2,3,0,0x20302,0,0,-1,0))]
meshes=[models.MESH.pack(*m) for m in (
    (0,6,0,3,0,0,1,9,9,1,1,0,0,0,0,0),
    (6,4,3,2,20,0,1,21,1,1,0,0,0,0,0,0))]
materials=[models.MATERIAL.pack(header,1,2,3,127,64,32,7,0,0)
           for header in (0xc000,0,0x4000)]
instances=[models.INSTANCE.pack(mesh,0x800000+256*i,i,i%2,
    1,0,0,0,1,0,0,0,1,i,2*i,3*i, 288,288,0,0,1,
    -1,128,496,512,248,270,0,0,0,0)
    for i,mesh in enumerate((1,0,1))]
payload=b''.join([*meshes,*vertices,*faces,*materials,*instances])
data=models.HEADER.pack(0x314d4e44,64+len(payload),2,10,5,3,3,0,0,128,0,128,0,0,0,0)+payload
result=models.omit_checker_faces(data)
h,vb,fb,mb,ib=tables(result)
assert h[2:7]==(2,7,3,3,3) and h[1]==len(result)
assert len(data)-len(result)==2*models.FACE.size+3*models.VERTEX.size
assert result[8:12]==data[8:12] and result[20:64]==data[20:64]
assert result[vb:fb]==b''.join(vertices[v] for v in (1,2,3,4,7,8,9))
assert result[mb:]==b''.join([*materials,*instances])
for m,ranges in enumerate(((0,4,0,2),(4,3,2,1))):
    at=models.HEADER.size+m*models.MESH.size
    assert models.MESH.unpack_from(result,at)[:4]==ranges
    assert result[at+16:at+models.MESH.size]==data[at+16:at+models.MESH.size]
for f,original,indices in ((0,0,(3,1,2,0)),(1,2,(0,2,1,3)),(2,4,(1,2,0,0))):
    raw=result[fb+f*models.FACE.size:fb+(f+1)*models.FACE.size]
    assert struct.unpack_from('<4H',raw)==indices
    assert raw[8:]==faces[original][8:]  # Material/count/flags/normal stay byte-exact.
    original_mesh=0 if original<3 else 1
    old_face=models.FACE.unpack(faces[original])
    new_face=models.FACE.unpack(raw)
    new_mesh=models.MESH.unpack_from(result,64+64*original_mesh)
    old_mesh=models.MESH.unpack(meshes[original_mesh])
    for corner in range(old_face[5]):
        at=vb+(new_mesh[0]+new_face[corner])*models.VERTEX.size
        assert result[at:at+models.VERTEX.size]==vertices[old_mesh[0]+old_face[corner]]
assert models.omit_checker_faces(result)==result
no_checker=bytearray(data)
_,_,_,old_mb,_=tables(data)
struct.pack_into('<H',no_checker,old_mb,0x4000)
assert models.omit_checker_faces(bytes(no_checker))==bytes(no_checker)
for mesh_id in (0,1):
    empty=bytearray(data)
    _,_,old_fb,_,_=tables(data)
    mesh=models.MESH.unpack(meshes[mesh_id])
    for f in range(mesh[2],mesh[2]+mesh[3]):
        struct.pack_into('<H',empty,old_fb+f*models.FACE.size+8,0)
    try:
        models.omit_checker_faces(empty)
    except ValueError as error:
        assert f'empty mesh {mesh_id}' in str(error)
    else:
        raise AssertionError('checker-only mesh was silently removed')
invalid=[b'',data[:63],data[:-1],data+b'x',models.share_positions(data),models.group_faces(data,1)]
for magic in (0,0x334d4e44,0x344d4e44):
    bad=bytearray(data);struct.pack_into('<I',bad,0,magic);invalid.append(bad)
for bad in (bytearray(data[:-1]),bytearray(data+b'x')):
    struct.pack_into('<I',bad,4,len(bad));invalid.append(bad)
for bad in invalid:
    try:
        models.omit_checker_faces(bad)
    except ValueError:
        pass
    else:
        raise AssertionError('non-prepared DNM1 accepted')

with tempfile.TemporaryDirectory(prefix='daytona-checker-test-') as tmp:
    root=Path(tmp)
    pack=root/'test.dnm'
    for candidate in (data,result,bytes(no_checker),models.share_positions(result),models.group_faces(result,1)):
        pack.write_bytes(candidate)
        checked=subprocess.run([sys.argv[1],str(pack)],capture_output=True,text=True)
        assert checked.returncode==0,(checked.stdout,checked.stderr)
    shared=models.share_positions(result)
    assert struct.unpack_from('<7H',shared,len(result))==(0,1,2,1,0,1,2)
    assert models.group_faces(result,1)==models.group_faces(shared,1)
    # CLI uses the filter after mode conversion and before position/group tables.
    source=root/'source';source.mkdir()
    (source/'manifest.json').write_text(json.dumps(dict(meshes=2,instances=3,unique_faces=5,
        unindexed_vertices=17,crtc_offset=[0,128],render_offset=[0,128])))
    for i,raw in enumerate(meshes):
        mesh=models.MESH.unpack(raw)
        lines=[f'mode {mesh[10]}','bounds '+' '.join(map(str,mesh[4:10]))]
        for f in range(mesh[2],mesh[2]+mesh[3]):
            face=models.FACE.unpack(faces[f]);material=models.MATERIAL.unpack(materials[face[4]])
            values=[face[5],face[7],*material[:4],*face[8:11],material[4],material[5],material[7],material[6]]
            lines.append('face '+' '.join(map(str,values)))
            for v in face[:face[5]]:
                lines.append('v '+' '.join(map(str,models.VERTEX.unpack(vertices[mesh[0]+v]))))
        (source/f'mesh_{i}.txt').write_text('\n'.join(lines)+'\n')
    context=models.INSTANCE.unpack(instances[0])[2:27]
    (source/'instances.txt').write_text('\n'.join(' '.join(map(str,(*models.INSTANCE.unpack(raw)[:2],*context)))
                                                for raw in instances)+'\n')
    (source/'cells.txt').write_text('0 0 3\n'+''.join(f'{i} 3 0\n' for i in range(1,256)))
    for number,options in enumerate(([],['--omit-checker-faces'],['--omit-checker-faces','--share-positions'],
                    ['--omit-checker-faces','--face-group-size','1'],
                    ['--world-cells','--omit-checker-faces','--share-positions'])):
        output=root/f'cli{number}.dnm'
        run=subprocess.run([sys.executable,str(tool),str(source),str(output),*options],capture_output=True,text=True)
        assert run.returncode==0,(run.stdout,run.stderr)
        # As the CLI: world packs keep checker faces' vertices apart (prepare_face_order rewrites their u, v).
        models.SPLIT_CHECKER_VERTICES='--world-cells' in options
        prepared=models.prepare(source)[0]
        models.SPLIT_CHECKER_VERTICES=False
        expected=models.world_cells(prepared,source) if '--world-cells' in options else prepared
        before=models.HEADER.unpack_from(expected)
        if '--world-cells' in options:
            assert before[3]>models.HEADER.unpack_from(models.prepare(source)[0])[3]
        if '--omit-checker-faces' in options:
            expected=models.omit_checker_faces(expected)
            report=json.loads(run.stdout)
            after=models.HEADER.unpack_from(expected)
            assert report['checker_faces_removed']==2
            assert report['vertices_removed']==before[3]-after[3]
            assert report['faces']==3 and report['vertices']==after[3]
            if 'source_vertices' in report:
                assert report['vertex_bytes_saved']==(report['source_vertices']-after[3])*models.VERTEX.size
        if '--share-positions' in options:
            expected=models.share_positions(expected)
        if '--face-group-size' in options:
            expected=models.group_faces(expected,1)
        assert output.read_bytes()==expected
print('PASS: checker-only faces removed; exact retained corners/bounds/materials/instances; CLI/default/shared/grouped and rejection checks')
