"""ROM-free sidecar checks across reordered scenery exports; optional C reader."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import prepare_models as models
from asset_rules import load_asset_rules
from optimize_flat_cutouts import collapse_flat_cutouts


def sources(root, addresses):
    (root/'manifest.json').write_text(json.dumps(dict(meshes=len(addresses),
        instances=len(addresses), unique_faces=4*len(addresses),
        unindexed_vertices=16*len(addresses), crtc_offset=[0, 0], render_offset=[0, 0])))
    instances = []
    for mid, address in enumerate(addresses):
        rows = ['mode 1', f'bounds 0 0 {mid} 4 1 {mid}']
        for x in range(4):
            rows.append('face 4 131585 24595 0 3600 41216 0 0 1 255 255 7 255')
            for px, py in ((x, 0), (x+1, 0), (x+1, 1), (x, 1)):
                rows.append(f'v {px} {py} {mid} {px*256} {py*256}')
        (root/f'mesh_{mid}.txt').write_text('\n'.join(rows)+'\n')
        instances.append(f'{mid} {address} 1 0 1 0 0 0 1 0 0 0 1 0 0 10 '
                         '256 256 0 0 1 -1 128 496 512 248 270')
    (root/'instances.txt').write_text('\n'.join(instances)+'\n')
    (root/'cells.txt').write_text('0 0 '+str(len(addresses))+'\n'+
                                ''.join(f'{i} {len(addresses)} 0\n' for i in range(1, 256)))


def rejects(sidecar, data, value):
    sidecar.write_text(json.dumps(value) if not isinstance(value, str) else value)
    try:
        load_asset_rules(sidecar, data)
    except ValueError:
        return
    raise AssertionError(f'accepted invalid sidecar: {value}')


with tempfile.TemporaryDirectory(prefix='asset-rules-') as temp:
    base = Path(temp)
    for course, addresses in enumerate(((0x800001, 0x800001, 0x800002),
        (0x900002, 0x900001, 0x900002), (0xa00002, 0xa00002, 0xa00001))):
        root = base/str(course)
        root.mkdir()
        sources(root, addresses)
        data, _ = models.prepare(root)
        data = models.world_cells(data, root)
        disabled = addresses[0]
        selected = {i for i, a in enumerate(addresses) if a != disabled}
        sizes = [1 if a == disabled else 4 for a in addresses]
        sidecar = root/'rules.json'
        rules = dict(version=1, defaults=dict(collapse_flat_cutouts=True, face_group_size=4),
                     geometry={hex(disabled): dict(collapse_flat_cutouts=False, face_group_size=1)})
        sidecar.write_text(json.dumps(rules))
        enabled, actual_sizes, report = load_asset_rules(sidecar, data, False, 2)
        assert enabled == selected and actual_sizes == sizes
        assert report['sha256'] == hashlib.sha256(sidecar.read_bytes()).hexdigest()
        assert [r['source_addresses'] for r in report['geometry']] == [[hex(a)] for a in addresses]
        collapsed, pairs = collapse_flat_cutouts(data, selected)
        assert pairs == 2
        expected = models.group_faces(collapsed, mesh_sizes=sizes)
        assert models.HEADER.unpack_from(expected)[4] == 10
        for i, address in enumerate(addresses):
            mesh = models.MESH.unpack_from(expected, 64+i*64)
            assert mesh[3] == (4 if address == disabled else 2)
            assert mesh[12] == (4 if address == disabled else 1)
        output = root/'world.dnm'
        command = [sys.executable, str(Path(models.__file__)), str(root), str(output),
                   '--world-cells', '--asset-rules', str(sidecar)]
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert output.read_bytes() == expected
        assert json.loads(result.stdout)['asset_rules'] == report
        if len(sys.argv) > 1:
            result = subprocess.run([sys.argv[1], str(output)], capture_output=True, text=True)
            assert result.returncode == 0, (result.stdout, result.stderr)
        # Existing files are never overwritten, and no-world use is rejected.
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode != 0 and output.read_bytes() == expected
        output.unlink()
        result = subprocess.run([arg for arg in command if arg != '--world-cells'],
                                capture_output=True, text=True)
        assert result.returncode != 0 and not output.exists()
        # CLI baseline, empty rules and all-disabled selection preserve geometry.
        sidecar.write_text('{"version":1}')
        assert load_asset_rules(sidecar, data)[:2] == (set(), None)
        assert load_asset_rules(sidecar, data, True, 2)[:2] == (set(range(3)), [2]*3)
        assert collapse_flat_cutouts(data, set()) == (data, 0)
        for bad in ([], {}, {'version': True}, {'version': 2}, {'version': 1, 'typo': {}},
            {'version': 1, 'defaults': {'collapse_flat_cutouts': 1}},
            {'version': 1, 'defaults': {'join_strips': 1}},
            {'version': 1, 'defaults': {'face_group_size': True}},
            {'version': 1, 'defaults': {'face_group_size': 257}},
            {'version': 1, 'defaults': {'face_group_size': 0}},
            {'version': 1, 'geometry': []},
            {'version': 1, 'geometry': {'0xbfffff': {}}},
            {'version': 1, 'geometry': {hex(disabled): {'sprite': True}}},
            {'version': 1, 'geometry': {hex(disabled): {'face_group_size': 1}}},
            {'version': 1, 'geometry': {hex(disabled): {}, str(disabled): {}}},
            '{"version":1,"geometry":{},"geometry":{}}'):
            rejects(sidecar, data, bad)
        # The exporter can deduplicate identical meshes from different addresses.
        h = models.HEADER.unpack_from(data)
        ib = 64+h[2]*64+h[3]*16+h[4]*32+h[5]*32
        shared = bytearray(data+data[ib:ib+models.INSTANCE.size])
        header = list(h)
        header[1], header[6] = len(shared), h[6]+1
        models.HEADER.pack_into(shared, 0, *header)
        other = next(a for a in addresses if a != disabled)
        instance = list(models.INSTANCE.unpack_from(data, ib))
        instance[1] = other
        models.INSTANCE.pack_into(shared, ib, *instance)
        sidecar.write_text('{"version":1,"defaults":{"collapse_flat_cutouts":true}}')
        assert load_asset_rules(sidecar, shared)[0] == set(range(3))
        rejects(sidecar, shared, dict(version=1, geometry={hex(disabled): dict(collapse_flat_cutouts=True)}))
        # Wrong IDs must fail before creating output, not silently use defaults.
        sidecar.write_text('{"version":1,"geometry":{"0xbfffff":{}}}')
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode != 0 and 'unknown geometry' in result.stderr and not output.exists()
print('PASS source-address rules, three reordered exports, validation, preservation and CLI'+
      (' and real C reader' if len(sys.argv) > 1 else ''))
