"""Validated offline scenery overrides keyed by original model address."""
import hashlib
import json


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate sidecar key: {key}')
        result[key] = value
    return result


def _rules(value):
    if not isinstance(value, dict) or set(value)-{'collapse_flat_cutouts', 'face_group_size', 'join_strips'}:
        raise ValueError('unknown geometry rule; expected collapse_flat_cutouts, face_group_size or join_strips')
    if 'collapse_flat_cutouts' in value and type(value['collapse_flat_cutouts']) is not bool:
        raise ValueError('collapse_flat_cutouts must be boolean')
    if 'join_strips' in value and type(value['join_strips']) is not bool:
        raise ValueError('join_strips must be boolean')
    if 'face_group_size' in value and (type(value['face_group_size']) is not int
                                     or not 1 <= value['face_group_size'] <= 256):
        raise ValueError('face_group_size must be an integer in 1..256')
    return value


def load_asset_rules(path, data, collapse=False, group_size=None, join=False):
    """Resolve sidecar against validated world_cells DNM1, before compaction.

    CLI baseline < sidecar defaults < per-address rules. One address can own
    several UV/material meshes; all receive the same rule. Unknown IDs fail.
    """
    from prepare_models import HEADER, MESH, VERTEX, FACE, MATERIAL, INSTANCE
    raw = path.read_bytes()
    sidecar = json.loads(raw, object_pairs_hook=_object)
    if (not isinstance(sidecar, dict) or set(sidecar)-{'version', 'defaults', 'geometry'}
            or type(sidecar.get('version')) is not int or sidecar['version'] != 1):
        raise ValueError('expected asset sidecar version 1')
    defaults = dict(collapse_flat_cutouts=collapse, face_group_size=group_size, join_strips=join)
    defaults.update(_rules(sidecar.get('defaults', {})))
    geometry = sidecar.get('geometry', {})
    if not isinstance(geometry, dict):
        raise ValueError('geometry must be an object keyed by source model address')
    overrides = {}
    for key, rule in geometry.items():
        # Resolve numeric aliases before checking for duplicate IDs.
        address = int(key, 16 if key.startswith('0x') else 10)
        if not 0x800000 <= address < 0xc00000 or address in overrides:
            raise ValueError(f'invalid or duplicate geometry address: {key}')
        overrides[address] = _rules(rule)
    h = HEADER.unpack_from(data)
    if h[0] != 0x314d4e44 or h[1] != len(data):
        raise ValueError('asset sidecar requires prepared DNM1')
    ib = HEADER.size+h[2]*MESH.size+h[3]*VERTEX.size+h[4]*FACE.size+h[5]*MATERIAL.size
    owners = [set() for _ in range(h[2])]
    for i in range(h[6]):
        instance = INSTANCE.unpack_from(data, ib+i*INSTANCE.size)
        owners[instance[0]].add(instance[1])
    if any(not addresses for addresses in owners):
        raise ValueError('sidecar requires source addresses for every mesh')
    known = set.union(*owners)
    if set(overrides)-known:
        raise ValueError('unknown geometry addresses: '+', '.join(hex(a) for a in sorted(set(overrides)-known)))
    resolved = []
    for addresses in owners:
        rules = [dict(defaults, **overrides.get(address, {})) for address in sorted(addresses)]
        if any(rule != rules[0] for rule in rules):
            raise ValueError('conflicting rules for shared mesh addresses: '+
                             ', '.join(hex(a) for a in sorted(addresses)))
        resolved.append(rules[0])
    sizes = [rule['face_group_size'] for rule in resolved]
    if any(size is not None for size in sizes) and any(size is None for size in sizes):
        raise ValueError('face_group_size needs a default for every mesh')
    report = dict(sha256=hashlib.sha256(raw).hexdigest(), geometry=[
        dict(mesh=i, source_addresses=[hex(a) for a in sorted(addresses)], **rule)
        for i, (addresses, rule) in enumerate(zip(owners, resolved))])
    enabled = {i for i, rule in enumerate(resolved) if rule['collapse_flat_cutouts']}
    return enabled, sizes if sizes[0] is not None else None, report
