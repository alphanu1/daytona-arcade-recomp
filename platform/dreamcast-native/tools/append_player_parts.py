"""Append the race player's own parts to an existing car pack, into a NEW directory.

The player draw (6c40/6dc0 body, 7384..7664 wheels) submits a fixed list that
the opponent catalogue does not contain: glass 0x13b (polygon RAM, authored in
main_data.bin and validated against it) and the checker shadow 0x8d7ab7. They
become instances N and N+1; N+2..N+5 rebind the existing wheel meshes with the
authored render offsets (program 0x234af4) as instance translation, exactly as
prepare_models.player_car() does. Existing meshes/faces/materials/instances and
every texture resource stay byte-identical.

car.dnt (DNT4) gains bindings only: the glass header's missing low lumas reuse
the resource of its darkest existing binding, with that binding's modulation
scaled by the source palette ratio (per channel least squares over 16 entries,
video tables from a reference dump). The shadow is solid colour 0 (black) with
the checker bit; the mesh renderer never looks its material up.

The renderer's loader matches every pack instance against the generated
nm_intro_model_ids catalogue, so the six addresses are appended to a copy of
intro_drive_data.h in a NEW header directory (use it as RACE_ASSET_DIR).

--effects RACE_CARS (m2segment --export-native-race-cars, which exports the
effect lists too): the car-effect models (include/car_effects.h: the dust,
spark, smoke and spin smoke table entries the rules reach, 82 models in table
order: dust and sparks punch-through, smoke and spin smoke the checker,
translucent ones) become instances between the catalogue and the six player parts,
so the player parts stay the pack's last six instances. Their meshes follow
the glass and shadow meshes, their materials are appended like the glass's.
Their (header, luma) bindings are already in car.dnt: the race-car export's
material keys carry them. The model addresses are read from the ROM tables
and must equal include/car_effects.h's.

--crew RACE_CARS (the same export: it holds the crew's lists too): the pit
crew's models (include/pit_crew.h: every part of the model sets the crew
records 239848[0] and their scripts name, and the tool of list 288ddc8; 26
models in address order; the held wheel is the wheel 8d7c9a the pack has; not
the manual twins, tools/export_course.hpp) become instances after the effects,
before the six player parts. Their bindings are in car.dnt the same way.

--hoods RACE_CARS (the same export): the clean hoods of the hood view, the
automatic car's 94fff9 and the manual car's 9506a8 (lists 231eb0[0][0] and
231eb0[1][0], include/player_parts.h), become instances after the crew,
before the six player parts; bindings as above. The damaged hoods are
position/normal variants of the clean one (car-hoods.dnb,
tools/prepare_damage_bodies.py --hoods), which the scene writes into the
hood's mesh as it does the damaged bodies; --all-hoods appends all eight
as meshes instead (about 107 KB more heap a car).

ROM-derived output: write only below ignored build/ directories.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct

import numpy as np

import prepare_models as pm

GLASS, SHADOW = 0x13b, 0x8d7ab7
WHEELS = (0x8d7b13, 0x8d7c9a, 0x8d7b13, 0x8d7c9a)


def tables(data):
    h = pm.HEADER.unpack_from(data)
    off = pm.table_offsets(h)
    sizes = (pm.MESH.size, pm.VERTEX.size, pm.FACE.size, pm.MATERIAL.size, pm.INSTANCE.size)
    return h, [data[o:o+n*s] for o, n, s in zip(off, h[2:7], sizes)], off


# The punch-through effects (header 6409: dust, sparks), then the checker ones (header e409: smoke, spin smoke; the
# translucent class of prepare_palette_materials.py). Spin smoke: 27 entries by particle life, 4 models.
EFFECT_TABLES = (('dust', 0x231f06, 1, 31), ('spark', 0x232006, 9, 24), ('smoke', 0x231f86, 1, 31),
                 ('spin', 0x232206, 0, 26))


def effect_models(program, main_rom):
    """Model of every effect table entry the rules reach, in table order, each once; checked against
    include/car_effects.h (nm_fx_*_models)."""
    from inspect_course import MAIN_BASE, words
    header = (Path(__file__).resolve().parent.parent/'include'/'car_effects.h').read_text()
    out = []
    for name, table, first, last in EFFECT_TABLES:
        declared = re.search(r'nm_fx_%s_models\[[^]]*\]=\{([^}]*)\}' % name, header)
        if not declared:
            raise ValueError(f'include/car_effects.h has no nm_fx_{name}_models')
        declared = [int(v, 16) for v in declared.group(1).replace('\n', ' ').split(',')]
        for i in range(first, last+1):
            pointer = struct.unpack_from('<I', program, table-0x200000+4*i)[0]
            record = words(main_rom, MAIN_BASE, pointer, 5)
            if record[4] or not 0x800000 <= record[0] < 0xc00000:
                raise ValueError('effect model list changed')
            if declared[i] != record[0]:
                raise ValueError(f'include/car_effects.h nm_fx_{name}_models[{i}] is not the ROM table entry')
            if record[0] not in out:
                out.append(record[0])
    return out


def crew_models(program, main_rom):
    """Every model of the pit crew's draw (tools/prepare_intro.py crew_art reads the same tables), in address
    order: the 16 lists of each model set the six records 239848[0] and their scripts (action 5) name, their
    manual twins 64 bytes further (1ad3c with 501464 = 1: the manual car's crew colours), and the tool list
    288ddc8 of 1b33c; each list one model."""
    from inspect_course import MAIN_BASE, words
    def program_words(address, count):
        return struct.unpack_from(f'<{count}I', program, address-0x200000)
    base = program_words(0x239848, 1)[0]
    sets = set()
    for crew in range(6):
        record = program_words(base+32*crew, 3)
        sets.add(record[1])
        entry = record[2]
        for _ in range(64):
            frame, action, argument = program_words(entry, 3)
            if action == 5:
                sets.add(argument)
            if frame == 999999:
                break
            entry += 32
        else:
            raise ValueError('pit crew script without terminator')
    lists = {v for s in sets for v in program_words(s, 16)+program_words(s+64, 16)} | {0x288ddc8}
    out = set()
    for pointer in lists:
        record = words(main_rom, MAIN_BASE, pointer, 5)
        if record[4] or not 0x800000 <= record[0] < 0xc00000:
            raise ValueError('pit crew object list is not one model')
        out.add(record[0])
    return sorted(out)


def hood_models(program, main_rom):
    """The hoods 231eb0[v][0..3] (7328..7350) of the automatic (v 0) and the manual car (v 1), one model per list,
    in list order, checked against include/player_parts.h nm_player_hood_models and nm_player_manual_hood_models."""
    from inspect_course import MAIN_BASE, words
    header = (Path(__file__).resolve().parent.parent/'include'/'player_parts.h').read_text()
    out = []
    for v, name in enumerate(('nm_player_hood_models', 'nm_player_manual_hood_models')):
        declared = re.search(name+r'\[4\]=\{([^}]*)\}', header)
        if not declared:
            raise ValueError(f'include/player_parts.h has no {name}')
        declared = [int(x, 16) for x in declared.group(1).split(',')]
        for i in range(4):
            pointer = struct.unpack_from('<I', program, 0x231eb0-0x200000+16*v+4*i)[0]
            record = words(main_rom, MAIN_BASE, pointer, 5)
            if record[4] or record[0] != declared[i]:
                raise ValueError(f'hood list 231eb0[{v}] differs from include/player_parts.h {name}')
            out.append(record[0])
    return out


def append_models(car, extras, program, main_rom, effects=None, crew=None, hoods=None):
    """effects, crew, hoods: (local pack of the models, their addresses in instance order) or None."""
    h, (meshes, vertices, faces, materials, instances), off = tables(car)
    if h[0] != (0x324d4e44 | pm.ALIGNED) or h[7] or any(h[8:]):
        raise ValueError('expected the aligned DNM2 car pack without groups/strips')
    refs_at = (off[4]+h[6]*pm.INSTANCE.size+31) & ~31
    old_refs = car[refs_at:refs_at+2*h[3]]
    known = {materials[i:i+pm.MATERIAL.size]: i//pm.MATERIAL.size
             for i in range(0, len(materials), pm.MATERIAL.size)}
    added, new_faces, new_meshes, new_vertices = [], bytearray(), bytearray(), bytearray()
    sources = [(extra, [address]) for address, extra in zip((GLASS, SHADOW), extras)]
    sources.append(effects or (None, []))
    sources.append(crew or (None, []))
    sources.append(hoods or (None, []))
    effect_meshes, crew_meshes, hood_meshes = [], [], []
    for source, (extra, addresses) in enumerate(sources):
        if extra is None:
            continue
        eh, (em, ev, ef, emat, ei), _ = tables(extra)
        if (eh[0] != 0x314d4e44 or eh[6] != len(addresses) or (len(addresses) == 1 and eh[2] != 1) or
                [pm.INSTANCE.unpack_from(ei, i*pm.INSTANCE.size)[1] for i in range(eh[6])] != addresses):
            raise ValueError('expected one local model per source address')
        material_map = {}
        for i in range(eh[5]):
            record = emat[i*pm.MATERIAL.size:(i+1)*pm.MATERIAL.size]
            if record not in known:
                known[record] = h[5]+len(added)
                added.append(record)
            material_map[i] = known[record]
        first_mesh = h[2]+len(new_meshes)//pm.MESH.size
        vertex_shift, face_shift = h[3]+len(new_vertices)//pm.VERTEX.size, h[4]+len(new_faces)//pm.FACE.size
        for m in range(eh[2]):
            mesh = list(pm.MESH.unpack_from(em, m*pm.MESH.size))
            mesh[0] += vertex_shift
            mesh[2] += face_shift
            new_meshes += pm.MESH.pack(*mesh)
        new_vertices += ev
        for i in range(eh[4]):
            face = list(pm.FACE.unpack_from(ef, i*pm.FACE.size))
            face[4] = material_map[face[4]]
            new_faces += pm.FACE.pack(*face)
        if source >= 2:   # the effects (2), the crew (3), then the hoods (4), after the glass and the shadow
            (effect_meshes if source == 2 else crew_meshes if source == 3 else hood_meshes)[:] = [
                first_mesh+pm.INSTANCE.unpack_from(ei, i*pm.INSTANCE.size)[0] for i in range(eh[6])]
    owners = {}
    for i in range(h[6]):
        inst = pm.INSTANCE.unpack_from(instances, i*pm.INSTANCE.size)
        owners.setdefault(inst[1], inst[0])
    identity = (1, 0, 0, 0, 1, 0, 0, 0, 1)
    new_instances = bytearray()
    for mesh, address in zip(effect_meshes+crew_meshes+hood_meshes,
                             (effects[1] if effects else [])+(crew[1] if crew else [])+(hoods[1] if hoods else [])):
        new_instances += pm.INSTANCE.pack(mesh, address, 0, 0, *identity, 0, 0, 0,
                                          1, 1, 0, 0, 0, 0, 0, 640, 480, 320, 240, 0, 0, 0, 0)
    for i, address in enumerate((GLASS, SHADOW)):
        new_instances += pm.INSTANCE.pack(h[2]+i, address, 0, 0, *identity, 0, 0, 0,
                                          1, 1, 0, 0, 0, 0, 0, 640, 480, 320, 240, 0, 0, 0, 0)
    # 7384..7664: four authored part lists (program 232096), render offsets 234af4.
    from inspect_course import MAIN_BASE, words
    for wheel, pointer in enumerate(struct.unpack_from('<4I', program, 0x32096)):
        record = words(main_rom, MAIN_BASE, pointer, 5)
        if record[0] != WHEELS[wheel] or record[4]:
            raise ValueError('player wheel descriptor table changed')
    offsets = []
    for wheel, address in enumerate(WHEELS):
        xyz = struct.unpack_from('<3f', program, 0x34af4+12*wheel)
        if not all(np.isfinite(xyz)) or max(map(abs, xyz)) > 4:
            raise ValueError('invalid wheel render offset')
        offsets.append(xyz)
        new_instances += pm.INSTANCE.pack(owners[address], address, 0, 0, *identity, *xyz,
                                          1, 1, 0, 0, 0, 0, 0, 640, 480, 320, 240, 0, 0, 0, 0)
    counts = (h[2]+len(new_meshes)//pm.MESH.size, h[3]+len(new_vertices)//pm.VERTEX.size,
              h[4]+len(new_faces)//pm.FACE.size, h[5]+len(added), h[6]+6+len(effect_meshes)+len(crew_meshes)+len(hood_meshes))
    payload = b''.join((meshes, bytes(new_meshes), vertices, bytes(new_vertices), faces, bytes(new_faces),
                        materials, *added, instances, bytes(new_instances)))
    plain = pm.HEADER.pack(0x314d4e44, pm.HEADER.size+len(payload), *counts, 0, *([0]*8))+payload
    out = pm.align_tables(pm.share_positions(plain))
    nh, _, noff = tables(out)
    at = (noff[4]+nh[6]*pm.INSTANCE.size+31) & ~31
    if out[at:at+len(old_refs)] != old_refs:
        raise ValueError('existing position references changed')
    return out, {'instances': counts[4], 'meshes': counts[0], 'faces': counts[2],
                 'materials_added': len(added), 'wheel_offsets': offsets, 'effect_instances': len(effect_meshes),
                 'crew_instances': len(crew_meshes), 'hood_instances': len(hood_meshes)}


def video_tables(dump):
    pal = np.fromfile(dump/'palram.bin', '<u2')
    xlat = np.fromfile(dump/'colorxlat.bin', '<u2')
    luma = np.fromfile(dump/'lumaram.bin', np.uint8)
    if len(pal) != 0x2000 or len(xlat) != 0x6000 or len(luma) != 0x20000:
        raise ValueError('unexpected video table dump sizes')
    gamma = np.array([int(max((i-64.0)*255.0/191.0, 0.0)) for i in range(256)])

    def palette(header, light):
        colour = int(pal[((header[3] >> 6) & 0x3ff)+0x1000])
        ramp = luma[(((header[1] & 255) << 7)+np.arange(16)*8)*4].astype(np.int64)
        level = np.minimum(63, ramp*light//256)
        return np.stack([gamma[xlat[c*0x2000+(((colour >> (5*c)) & 31) << 8)+level] & 0xff]
                         for c in range(3)], 1).astype(float)
    return palette


def append_bindings(dnt, header, lights, palette):
    magic, size, resources, count, payload, staging, a, b = struct.unpack_from('<4s7I', dnt)
    if magic != b'DNT4' or size != len(dnt):
        raise ValueError('expected the DNT4 car material pack')
    records = [list(struct.unpack_from('<IIHHI', dnt, 32+16*i)) for i in range(resources)]
    base = 32+16*resources
    rows = [(dnt[base+16*i:base+16*i+16], dnt[base+16*count+4*i:base+16*count+4*i+4])
            for i in range(count)]
    start = (32+16*resources+20*count+31) & ~31
    family = {struct.unpack_from('<H', r, 8)[0]: (r, m) for r, m in rows
              if struct.unpack_from('<4H', r) == tuple(header)}
    if not family:
        raise ValueError('glass header has no existing bindings')
    anchor_light = min(family)
    anchor, modulation = family[anchor_light]
    resource, crop = struct.unpack_from('<HI', anchor, 10)
    blue, green, red, alpha = modulation
    reference = palette(header, anchor_light)
    added, worst = [], 0.0
    for light in sorted(set(lights)-set(family)):
        if light > anchor_light:
            raise ValueError('missing glass light above the darkest anchor')
        wanted = palette(header, light)
        scaled = []
        for channel, value in enumerate((red, green, blue)):
            g = reference[:, channel]
            ratio = float((g*wanted[:, channel]).sum()/max((g*g).sum(), 1e-9))
            if not 0 <= ratio <= 1:
                raise ValueError('glass modulation ratio outside the modulate range')
            scaled.append(int(round(value*ratio)))
            worst = max(worst, float(np.abs(g*ratio-wanted[:, channel]).max()))
        added.append((struct.pack('<6HI', *header, light, resource, crop),
                      bytes((scaled[2], scaled[1], scaled[0], alpha))))
    rows = sorted(rows+added, key=lambda row: row[0][:10])
    if len({row[0][:10] for row in rows}) != len(rows):
        raise ValueError('duplicate material binding')
    new_start = (32+16*resources+20*len(rows)+31) & ~31
    out = bytearray(struct.pack('<4s7I', magic, size+new_start-start, resources, len(rows),
                                payload, staging, a, b))
    for record in records:
        record[0] += new_start-start
        out += struct.pack('<IIHHI', *record)
    out += b''.join(row[0] for row in rows)+b''.join(row[1] for row in rows)
    out += bytes(new_start-len(out))+dnt[start:]
    return bytes(out), {'bindings_added': len(added), 'anchor_light': anchor_light,
                        'resource': resource, 'palette_ratio_max_error': worst}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('assets', type=Path, help='existing asset directory (read only)')
    parser.add_argument('windows', type=Path, help='m2segment --export-native-player-car (canonical glass)')
    parser.add_argument('sources', type=Path, help='m2segment --export-native-models of a race frame (shadow)')
    parser.add_argument('images', type=Path, help='daytona93 ROM image directory')
    parser.add_argument('video', type=Path, help='reference palram/colorxlat/lumaram dump directory')
    parser.add_argument('output', type=Path, help='NEW asset directory')
    parser.add_argument('race_header', type=Path, help='existing generated intro_drive_data.h')
    parser.add_argument('header_output', type=Path, help='NEW directory for the extended header')
    parser.add_argument('--effects', type=Path, metavar='RACE_CARS',
                        help='m2segment --export-native-race-cars directory: append the car-effect models')
    parser.add_argument('--crew', type=Path, metavar='RACE_CARS',
                        help='m2segment --export-native-race-cars directory: append the pit crew\'s models')
    parser.add_argument('--hoods', type=Path, metavar='RACE_CARS',
                        help='m2segment --export-native-race-cars directory: append the hood view\'s clean hoods')
    parser.add_argument('--all-hoods', action='store_true', help='with --hoods: every hood of both cars as a mesh')
    args = parser.parse_args()
    if args.output.exists() or args.header_output.exists():
        raise SystemExit('output directory already exists; choose a new one')
    header_text = args.race_header.read_text()
    catalogue = re.search(r'(nm_intro_model_ids\[\] = \{)([^}]*)(\})', header_text)
    if not catalogue or len(re.findall(r'nm_intro_model_ids\[\] = ', header_text)) != 1:
        raise ValueError('generated header has no single model catalogue')
    listed = [int(v) for v in catalogue.group(2).split(',')]
    program, main_rom = pm.car_images(args.images)
    extras = (pm.local_models(pm.prepare(args.windows)[0], [GLASS], windows_rom=main_rom),
              pm.local_models(pm.prepare(args.sources)[0], [SHADOW]))
    effects = crew = hoods = None
    prepared = {}
    def exported(directory):
        if directory not in prepared:
            prepared[directory] = pm.prepare(directory)[0]
        return prepared[directory]
    if args.effects:
        addresses = effect_models(program, main_rom)
        effects = (pm.local_models(exported(args.effects), addresses), addresses)
    if args.crew:
        addresses = crew_models(program, main_rom)
        crew = (pm.local_models(exported(args.crew), addresses), addresses)
    if args.hoods:
        listed_hoods = hood_models(program, main_rom)
        addresses = listed_hoods if args.all_hoods else [listed_hoods[0], listed_hoods[4]]
        hoods = (pm.local_models(exported(args.hoods), addresses), addresses)
    car = (args.assets/'car.dnm').read_bytes()
    models, report = append_models(car, extras, program, main_rom, effects, crew, hoods)
    if report['instances'] != len(listed)+6+sum(len(g[1]) for g in (effects, crew, hoods) if g):
        raise ValueError('generated catalogue does not describe this car pack')
    eh, (em, _, ef, emat, _), _ = tables(extras[0])
    glass_mesh = pm.MESH.unpack_from(em, 0)
    headers = set()
    ambient = 255.0
    for i in range(glass_mesh[2], glass_mesh[2]+glass_mesh[3]):
        material = pm.MATERIAL.unpack_from(emat, pm.FACE.unpack_from(ef, i*pm.FACE.size)[4]*pm.MATERIAL.size)
        headers.add(material[:4])
        ambient = min(ambient, material[5])
    if len(headers) != 1:
        raise ValueError('player glass uses more than one material header')
    header = headers.pop()
    # Luma = clamp(diffuse term + ambient + specular): never below the ambient.
    lights = range(int(ambient), 256)
    materials, binding_report = append_bindings((args.assets/'car.dnt').read_bytes(), header, lights,
                                                video_tables(args.video))
    shutil.copytree(args.assets, args.output)
    (args.output/'car.dnm').write_bytes(models)
    (args.output/'car.dnt').write_bytes(materials)
    args.header_output.mkdir()
    tail = ','.join(str(v) for v in ((*effects[1],) if effects else ())+((*crew[1],) if crew else ())+
                    ((*hoods[1],) if hoods else ())+(GLASS, SHADOW, *WHEELS))
    (args.header_output/'intro_drive_data.h').write_text(
        header_text[:catalogue.end(2)]+','+tail+header_text[catalogue.end(2):])
    report.update(binding_report)
    report.update(glass_header=list(header), glass_ambient=ambient,
                  car_dnm_bytes=len(models), car_dnt_bytes=len(materials),
                  car_dnm_sha256=hashlib.sha256(models).hexdigest(),
                  car_dnt_sha256=hashlib.sha256(materials).hexdigest())
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
