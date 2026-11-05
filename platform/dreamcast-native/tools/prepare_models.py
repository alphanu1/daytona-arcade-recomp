"""Pack verified source models as DNM1/2/3/4/5/6; no machine state.

python3 platform/dreamcast-native/tools/prepare_models.py SOURCE_DIR NEW_PACK_FILE
ROM-derived outputs must remain in ignored local directories.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

HEADER = struct.Struct("<8I4i4I")
MESH = struct.Struct("<4I6f6I")
VERTEX = struct.Struct("<3f2H")
FACE = struct.Struct("<5HBBI3fI")
MATERIAL = struct.Struct("<4H3f3I")
INSTANCE = struct.Struct("<4I17f6h4I")
GROUP = struct.Struct("<14f2I")
MAX_BYTES = 3 * 1024 * 1024   # meshpack.h NM_PACK_MAX_BYTES
ALIGNED = 0x80000000


def table_offsets(header):
    """First five table bases for validated legacy or aligned DNM packs."""
    offset, bases = HEADER.size, []
    for count, record in zip(header[2:7], (MESH, VERTEX, FACE, MATERIAL, INSTANCE)):
        if header[0] & ALIGNED:
            offset = (offset+31)&~31
        bases.append(offset)
        offset += count*record.size
    return tuple(bases)


def align_tables(data):
    """Final offline layout pass for legacy DNM1..6; keep all table bytes/order."""
    if len(data) < HEADER.size:
        raise ValueError('truncated DNM header')
    h = list(HEADER.unpack_from(data))
    magic = h[0]
    if (magic not in (0x314d4e44, 0x324d4e44, 0x334d4e44, 0x344d4e44, 0x354d4e44, 0x364d4e44)
            or h[1] != len(data) or len(data) > MAX_BYTES):
        raise ValueError('table alignment requires a prepared legacy DNM1..6 pack')
    # DNM6 carries the group and strip tables only when their counts are nonzero.
    layered = magic == 0x364d4e44
    tables = [(h[2], MESH.size, 1), (h[3], VERTEX.size, 1),
              (h[4], FACE.size, 1), (h[5], MATERIAL.size, 1),
              (h[6], INSTANCE.size, 1)]
    if magic != 0x314d4e44:
        tables.append((h[3], 2, 1))
    if magic in (0x334d4e44, 0x344d4e44, 0x354d4e44) or (layered and h[7]):
        tables.append((h[7], 24 if magic == 0x334d4e44 else GROUP.size, 4))
    if magic == 0x354d4e44 or (layered and h[12]):
        tables.extend(((h[12], 32, 32), (h[13], 2, 1)))
    if layered and h[15] & 4:   # NM_PACK_LOD_GROUPS: the count is the flag word's high half
        tables.append((h[15] >> 16, 32, 4))
    out = bytearray(data[:HEADER.size])
    offset = HEADER.size
    for count, stride, legacy_alignment in tables:
        start = (offset+legacy_alignment-1) & -legacy_alignment
        end = start+count*stride
        if not count or end > len(data) or any(data[offset:start]):
            raise ValueError('invalid DNM table extent or padding')
        out.extend(bytes((-len(out))&31))
        out.extend(data[start:end])
        offset = end
    legacy_end = (offset+3)&~3 if magic == 0x354d4e44 or layered else offset
    if legacy_end != len(data) or any(data[offset:]):
        raise ValueError('invalid DNM tail')
    out.extend(bytes((-len(out))&31))
    if len(out) > MAX_BYTES:
        raise ValueError('aligned tables exceed resident geometry ceiling; split offline')
    h[0], h[1] = magic | ALIGNED, len(out)
    HEADER.pack_into(out, 0, *h)
    return bytes(out)


def rows(path):
    return iter(line.split() for line in path.read_text().splitlines()
                if line.strip() and not line.lstrip().startswith("#"))


def row(lines, tag, count):
    values = next(lines, [])
    if len(values) != count + 1 or values[0] != tag:
        raise ValueError(f"expected {tag} with {count} fields")
    return values[1:]


def floats(values):
    # Canonicalize to the actual target representation before dedup/bounds.
    packed = struct.pack(f"<{len(values)}f", *map(float, values))
    result = struct.unpack(f"<{len(values)}f", packed)
    if not all(map(math.isfinite, result)):
        raise ValueError("non-finite source number")
    return result


def outward_float(value):
    """The next finite float32, for a conservative serialized sphere radius."""
    packed=struct.pack("<f", value)
    bits=struct.unpack("<I",packed)[0]
    if bits<0x7f7fffff:
        packed=struct.pack("<I",bits+1)
    return struct.unpack("<f",packed)[0]


def name_solid_colors(raw_color):
    """Original a74..b3c colour translation and raster gamma, both boot modes.

    Solid luma uses its upper six bits, so 64 colours cover all 256 shades.
    This is offline integer table construction, not Dreamcast frame work.
    """
    palettes=[]
    for step,bias in ((12,0x1160),(9,29<<8)):
        colors=[]
        for luma in range(64):
            argb=0xff000000
            for shift,output_shift in ((0,16),(5,8),(10,0)):
                product=((raw_color>>shift)&31)*luma*step
                translated=min(255,(product+bias)>>6) if product else 0
                gamma=max(0,(translated-64)*255//191)
                argb|=gamma<<output_shift
            colors.append(argb)
        palettes.append(colors)
    return palettes


def name_data_header(report):
    layout=report['name_layout']
    values=lambda rows: ','.join(str(v) for v in rows)
    floats=lambda rows: ','.join(float(v).hex()+'f' for v in rows)
    materials=report['name_solid_palette']
    lines=['/* Generated from owned daytona93; do not commit ROM-derived data. */',
           '#ifndef DAYTONA_NAME_DATA_H', '#define DAYTONA_NAME_DATA_H',
           '#include "render/ui/name_draw.h"',
           '_Alignas(32) static const nm_name_layout_t nm_name_layout_data={',
           '.radius={'+floats(r['radius'] for r in layout['ring'])+'},',
           '.initial_x={'+floats(layout['initial_x'])+'},',
           '.angle={'+values(r['angle'] for r in layout['ring'])+'},',
           '.exit={']
    lines += ['{.velocity={'+floats(r['exit_velocity'])+'},.scale_rate='+float(r['exit_scale']).hex()+
              'f,.spin={'+values(r['exit_spin'])+'}},' for r in layout['ring']]
    lines += ['}};',
           'static const uint32_t nm_name_model_addresses[30]={'+values(report['name_models'])+'};',
           f'static const uint16_t nm_name_material_headers[{len(materials)}][4]={{']
    lines += ['{'+values(m['header'])+'},' for m in materials]
    lines += ['};',f'_Alignas(32) static const uint32_t nm_name_colors[{len(materials)}][2][64]={{']
    for material in materials:
        lines.append('{')
        lines += ['{'+','.join(f'0x{c:08x}u' for c in colors)+'},'
                  for colors in material['argb_by_display_mode']]
        lines.append('},')
    return '\n'.join(lines+['};','#endif',''])


# --world-cells: a screen-checker face (header word 0 bit 15) and another face never share a vertex record, even with
# equal position and u, v (prepare_face_order.py --stipple-panels rewrites the panels' u, v; Expert has such pairs).
# Course 0 and 2 have none, so their packs are unchanged.
SPLIT_CHECKER_VERTICES = False


def prepare(sources):
    source_info = json.loads((sources / "manifest.json").read_text())
    mesh_count = source_info["meshes"]
    if type(mesh_count) is not int or not 1 <= mesh_count <= MAX_BYTES // MESH.size:
        raise ValueError("invalid mesh count")
    meshes, vertices, faces, materials, instances = [], [], [], {}, []
    source_vertices = 0
    for mesh_id in range(mesh_count):
        lines = rows(sources / f"mesh_{mesh_id}.txt")
        mode = int(row(lines, "mode", 1)[0])
        bounds = floats(row(lines, "bounds", 6))
        if mode not in range(4):
            raise ValueError("invalid model mode")
        first_vertex, first_face = len(vertices), len(faces)
        unique, positions = {}, []
        for fields in lines:
            if len(fields) != 14 or fields[0] != "face":
                raise ValueError("invalid face record")
            count, attribute = map(int, fields[1:3])
            if (count not in (3, 4) or count != (4 if attribute & 1 else 3)
                    or not attribute & 3 or not (attribute >> 8) & 3):
                raise ValueError("invalid face shape/link")
            header = tuple(map(int, fields[3:7]))
            normal = floats(fields[7:10])
            diffuse, ambient, scale = floats([fields[10], fields[11], fields[13]])
            control = int(fields[12])
            if not all(0 <= v <= 255 for v in (diffuse, ambient, scale, control)):
                raise ValueError("invalid lighting parameters")
            material = MATERIAL.pack(*header, diffuse, ambient, scale, control, 0, 0)
            material_id = materials.setdefault(material, len(materials))
            indices = []
            for _ in range(count):
                values = row(lines, "v", 5)
                xyz = floats(values[:3])
                vertex = VERTEX.pack(*xyz, *map(int, values[3:]))
                key = (vertex, SPLIT_CHECKER_VERTICES and bool(header[0] & 0x8000))
                if key not in unique:
                    unique[key] = len(unique)
                    vertices.append(vertex)
                    positions.append(xyz)
                indices.append(unique[key])
                source_vertices += 1
            indices += [0] * (4 - count)
            faces.append(FACE.pack(*indices, material_id, count, 0, attribute, *normal, 0))
        if not positions or len(faces) == first_face or len(unique) > 65536:
            raise ValueError("empty or oversized mesh")
        actual_bounds = tuple(fn(p[a] for p in positions) for fn in (min, max) for a in range(3))
        if bounds != actual_bounds:
            raise ValueError("source bounds do not enclose exactly its vertices")
        meshes.append(MESH.pack(first_vertex, len(unique), first_face, len(faces) - first_face,
                                *bounds, mode, 0, 0, 0, 0, 0))
    for values in rows(sources / "instances.txt"):
        if len(values) != 27:
            raise ValueError("invalid instance record")
        ids = tuple(map(int, values[:4]))
        if not 0 <= ids[0] < mesh_count or not 0 <= ids[2] <= 255 or ids[3] not in (0, 1):
            raise ValueError("invalid instance reference")
        coordinates = tuple(map(int, values[21:]))
        if not all(-2048 <= v <= 2047 for v in coordinates):
            raise ValueError("invalid reference viewport/center")
        instances.append(INSTANCE.pack(*ids, *floats(values[4:21]), *coordinates, 0, 0, 0, 0))
    if (not instances or len(instances) != source_info["instances"]
            or len(faces) != source_info["unique_faces"]
            or source_vertices != source_info["unindexed_vertices"]):
        raise ValueError("source manifest disagrees with model records")
    offsets = source_info["crtc_offset"] + source_info["render_offset"]
    if len(offsets) != 4 or not all(type(v) is int and -65536 <= v <= 65536 for v in offsets):
        raise ValueError("invalid projection offsets")
    size = HEADER.size + sum(map(len, meshes + vertices + faces + list(materials) + instances))
    if size > MAX_BYTES:
        raise ValueError("pack exceeds the 3 MiB resident geometry ceiling (NM_PACK_MAX_BYTES); split it offline")
    header = HEADER.pack(0x314d4e44, size, mesh_count, len(vertices), len(faces),
                         len(materials), len(instances), 0, *offsets, 0, 0, 0, 0)
    data = b"".join([header] + meshes + vertices + faces + list(materials) + instances)
    return data, dict(bytes=size, meshes=mesh_count, vertices=len(vertices),
                      source_vertices=source_vertices, faces=len(faces), materials=len(materials),
                      instances=len(instances), vertex_bytes_saved=(source_vertices - len(vertices)) * VERTEX.size,
                      sha256=hashlib.sha256(data).hexdigest())


def local_model(data, address):
    return local_models(data, [address])


def check_player_windows(data, main, address=0x13b):
    """Independently compare the RAM-tagged mesh with authored ROM strip/UVs.

    Called only after the caller SHA-validates main_data.bin. No captured
    coordinates or provenance flag can authorize arbitrary RAM as static.
    """
    layouts={0x13b:(0x50f8,0x78,13,9),0x1c8:(0x5168,0x9c,11,7),0x241:(0x51c8,0xb8,9,5)}
    if address not in layouts:
        raise ValueError('unsupported canonical window model')
    uv,header,strips,faces=layouts[address]
    h=HEADER.unpack_from(data)
    vb=HEADER.size+h[2]*MESH.size
    fb=vb+h[3]*VERTEX.size
    mb=fb+h[4]*FACE.size
    ib=mb+h[5]*MATERIAL.size
    owners={v[0] for i in range(h[6]) if (v:=INSTANCE.unpack_from(data,ib+i*INSTANCE.size))[1]==address}
    if len(owners)!=1:
        raise ValueError('missing or ambiguous canonical player windows')
    mesh=MESH.unpack_from(data,HEADER.size+owners.pop()*MESH.size)
    if mesh[3]!=faces or mesh[10]!=1:
        raise ValueError('unsupported player window shape/mode')
    at=0x9c0000+4*address
    p0,p1=main[at:at+12],main[at+12:at+24]
    at+=24
    index=0
    for _ in range(strips+1):
        attr=struct.unpack_from('<I',main,at)[0]
        if not attr&3:
            break
        normal=main[at+4:at+16]
        p2,p3=main[at+16:at+28],main[at+28:at+40]
        at+=40
        count=4 if attr&1 else 3
        pairs=struct.unpack_from(f'<{count*2}H',main,0x9e0000+2*uv)
        expected_header=struct.unpack_from('<4H',main,0x9e0000+2*header)
        link=(attr>>8)&3
        if link:
            if index>=mesh[3]:
                raise ValueError('extra authored player window face')
            f=FACE.unpack_from(data,fb+(mesh[2]+index)*FACE.size)
            material=MATERIAL.unpack_from(data,mb+f[4]*MATERIAL.size)
            if (f[5],f[7],struct.pack('<3f',*f[8:11]),material[:4])!=(count,attr,normal,expected_header):
                raise ValueError('player window topology/normal/material differs from ROM')
            for v,position in enumerate((p1,p0,p2,p3)[:count]):
                expected=position+struct.pack('<2H',pairs[v*2+1],pairs[v*2])
                start=vb+(mesh[0]+f[v])*VERTEX.size
                if data[start:start+VERTEX.size]!=expected:
                    raise ValueError('player window position/base UV differs from ROM')
            index+=1
        uv+=count*2
        delta=(attr>>12)&31
        header+=4*(delta-32 if delta&16 else delta)
        if link in (0,2): p0,p1=p2,p3
        elif link==1: p1=p2
        else: p0=p3
    else:
        raise ValueError('unterminated authored player windows')
    if index!=faces:
        raise ValueError('incomplete authored player windows')


def local_models(data, addresses, *, windows_rom=None):
    """Select unambiguous ROM parts, sharing repeated geometry/materials.

    Drop ALL observed transforms/lights/viewports: native state places the car.
    The identity instance is only a DNM1 mesh binding, never a captured pose.
    Input is the validated result of prepare(), not an arbitrary binary file.
    """
    if not addresses or any(not 0x800000 <= address < 0xc00000 and
                           not (address in (0x13b,0x1c8,0x241) and windows_rom is not None) for address in addresses):
        raise ValueError('local model must be ROM-backed')
    for address in set(addresses)&{0x13b,0x1c8,0x241}:
        check_player_windows(data,windows_rom,address)
    h = HEADER.unpack_from(data)
    vb = HEADER.size+h[2]*MESH.size
    fb = vb+h[3]*VERTEX.size
    mb = fb+h[4]*FACE.size
    ib = mb+h[5]*MATERIAL.size
    meshes, vertices, faces, materials, instances, selected = [], [], [], {}, [], {}
    vertex_count = 0
    for address in addresses:
        owners = {v[0] for i in range(h[6])
                  if (v := INSTANCE.unpack_from(data, ib+i*INSTANCE.size))[1] == address}
        if len(owners) != 1:
            raise ValueError('missing or ambiguous source model')
        owner = owners.pop()
        if owner not in selected:
            selected[owner] = len(meshes)
            mesh = list(MESH.unpack_from(data, HEADER.size+owner*MESH.size))
            vertices.append(data[vb+mesh[0]*VERTEX.size:vb+(mesh[0]+mesh[1])*VERTEX.size])
            first_face = len(faces)
            for i in range(mesh[2], mesh[2]+mesh[3]):
                face = list(FACE.unpack_from(data, fb+i*FACE.size))
                material = data[mb+face[4]*MATERIAL.size:mb+(face[4]+1)*MATERIAL.size]
                face[4] = materials.setdefault(material, len(materials))
                faces.append(FACE.pack(*face))
            mesh[0], mesh[2] = vertex_count, first_face
            vertex_count += mesh[1]
            meshes.append(MESH.pack(*mesh))
        instances.append(INSTANCE.pack(selected[owner],address,0,0, 1,0,0,0,1,0,0,0,1,0,0,0,
                                       1,1,0,0,0, 0,0,640,480,320,240, 0,0,0,0))
    payload = b''.join([*meshes,*vertices,*faces,*materials,*instances])
    if HEADER.size+len(payload)>MAX_BYTES:
        raise ValueError('local parts exceed geometry ceiling')
    return HEADER.pack(0x314d4e44,HEADER.size+len(payload),len(meshes),vertex_count,len(faces),
                       len(materials),len(instances),0, *([0]*8))+payload


def car_images(images):
    """Fingerprint the owned source images before following car pointers."""
    from inspect_course import IMAGES
    rom = {}
    for name in ('program.bin', 'main_data.bin'):
        size, digest = IMAGES[name]
        if (images/name).stat().st_size != size:
            raise ValueError('incorrect daytona93 image size')
        rom[name] = (images/name).read_bytes()
        if hashlib.sha256(rom[name]).hexdigest() != digest:
            raise ValueError('unrecognized daytona93 image')
    return rom['program.bin'], rom['main_data.bin']


def intro_cars(data, images, race=False):
    """All authored intro body choices/wheels/canonical windows as local bindings.

    Includes d144 phase models, ground shadow and low-detail crash parts; no captured poses.
    """
    from inspect_course import MAIN_BASE, words, intro_stages, intro_car_models
    program, main = car_images(images)
    graph = intro_stages(main, words(main, MAIN_BASE, 0x2865a60, 1)[0])
    catalog = intro_car_models(program, main, graph, race)
    addresses = sorted({obj[0] for item in catalog['lists'] for obj in item['objects']})
    return local_models(data, addresses, windows_rom=main)


def player_car(data, images, complete=False):
    """Normal player body + wheels, optionally canonical windows and shell.

    Source6484/7300/7384; not crash variants or shadows.
    No recorded matrix survives. Existing DNM instance translations hold the
    four fixed wheel offsets; native steering/spin replaces them each frame.
    """
    from inspect_course import MAIN_BASE, words
    program, main = car_images(images)
    body_list = words(main,MAIN_BASE,0x286fac0,1)[0] # Normal undamaged body0 table base.
    # i960 permits this halfword-aligned pointer table; course words() does not.
    pointers = [body_list,*struct.unpack_from('<4I',program,0x32096)]
    if complete:
        pointers += [0x2890550,words(main,MAIN_BASE,0x286fab8,1)[0]]
    addresses = []
    for pointer in pointers:
        records = words(main,MAIN_BASE,pointer,5)
        if records[4] != 0:
            raise ValueError('expected single-mesh authored car part list')
        addresses.append(records[0])
    if addresses != [0x95ff9d,0x8d7b13,0x8d7c9a,0x8d7b13,0x8d7c9a]+([0x13b,0x961bbf] if complete else []):
        raise ValueError('normal player part identity changed')
    out = bytearray(local_models(data,addresses,windows_rom=main if complete else None))
    at = len(out)-(len(addresses)-1)*INSTANCE.size
    for wheel in range(4):
        xyz = struct.pack('<3I',*words(program,0,0x34af4+12*wheel,3))
        if not all(math.isfinite(v) for v in struct.unpack('<3f',xyz)):
            raise ValueError('invalid wheel render offset')
        out[at+wheel*INSTANCE.size+52:at+wheel*INSTANCE.size+64] = xyz
    return bytes(out)


OBJECT_INSTANCE = 0x80000000  # meshpack.h, NM_OBJECT_INSTANCE


OBJECT_FLAGS = 0x200 | 0x400 | 0x800  # meshpack.h, NM_OBJECT_PLACED, NM_OBJECT_SPIN, NM_OBJECT_MESHES


def world_objects(source, first):
    """objects.txt of prepare_world_objects.py: {instance: (instance.padding[0],
    placement or None)} for the instances behind the 256 cell ranges
    (meshpack.h, NM_OBJECT_*). Older files have no flags and no placement."""
    words = {}
    for fields in rows(source / "objects.txt"):
        head = fields[:fields.index(";")] if ";" in fields else fields
        instance, cell, near, kind, variant = map(int, head[:5])
        flags = int(head[5]) if len(head) > 5 else 0
        parts = " ".join(fields).split(";")
        matrix = parts[3].split() if len(parts) > 3 else ["-"]
        matrix = None if matrix == ["-"] else [float(v) for v in matrix]
        if (instance != first+len(words) or not 0 <= cell <= 255 or near not in (0, 1)
                or not 0 <= kind <= 15 or not 0 <= variant <= 255 or flags & ~OBJECT_FLAGS
                or (matrix is not None) != bool(flags & 0x200) or (matrix is not None and len(matrix) != 12)):
            raise ValueError("invalid world object record")
        words[instance] = (OBJECT_INSTANCE | variant << 16 | kind << 12 | flags | near << 8 | cell, matrix)
    return words


def world_cells(data, source, objects=False):
    """Authored scenery uses one shared world-to-view matrix, not per-object poses.

    Retain every mesh/cell binding, but discard captured camera/light state.
    Requires complete cell-export provenance and identical observed contexts;
    never use this on an arbitrary sampled scene containing cars/other objects.
    """
    h=list(HEADER.unpack_from(data))
    end=0
    cells=list(rows(source / "cells.txt"))
    if len(cells)!=256:
        raise ValueError("world pack requires all256 authored cells")
    for i, fields in enumerate(cells):
        if len(fields)!=3:
            raise ValueError("invalid world cell range")
        cell,first,count=map(int,fields)
        if cell!=i or first!=end or count<0:
            raise ValueError("unordered or overlapping world cells")
        end+=count
    object_words = world_objects(source, end) if objects else {}
    if end+len(object_words)!=h[6]:
        raise ValueError("world cells do not cover every instance")
    ib=HEADER.size+h[2]*MESH.size+h[3]*VERTEX.size+h[4]*FACE.size+h[5]*MATERIAL.size
    instances=[]; context=None
    for i in range(h[6]):
        v=INSTANCE.unpack_from(data,ib+i*INSTANCE.size)
        if not 0x800000<=v[1]<0xc00000:
            raise ValueError("world scenery must be ROM-backed")
        if context is None:
            context=v[2:]
        elif context!=v[2:]:
            raise ValueError("scenery does not share one world-to-view context")
        word, matrix = object_words.get(i, (0, None))
        instances.append(INSTANCE.pack(v[0],v[1],0,0,*(matrix or (1,0,0,0,1,0,0,0,1,0,0,0)),
            1,1,0,0,0,0,0,640,480,320,240,word,0,0,0))
    h[8:12]=[0]*4
    return HEADER.pack(*h)+data[HEADER.size:ib]+b"".join(instances)


def omit_checker_faces(data):
    """Remove runtime-omitted screen-space checker faces from prepared DNM1.

    Retain mesh bounds, material IDs/table and instance bytes; compact surviving
    vertices in their original order. Input is trusted prepare/local/world output.
    """
    if len(data)<HEADER.size:
        raise ValueError("checker omission requires prepared DNM1")
    h=list(HEADER.unpack_from(data))
    vb=HEADER.size+h[2]*MESH.size
    fb=vb+h[3]*VERTEX.size
    mb=fb+h[4]*FACE.size
    end=mb+h[5]*MATERIAL.size+h[6]*INSTANCE.size
    if h[0]!=0x314d4e44 or h[1]!=len(data) or end!=len(data):
        raise ValueError("checker omission requires prepared DNM1")
    checker={m for m in range(h[5]) if struct.unpack_from('<H',data,mb+m*MATERIAL.size)[0]&0x8000}
    meshes,vertices,faces=[],[],[]
    for m in range(h[2]):
        at=HEADER.size+m*MESH.size
        mesh=MESH.unpack_from(data,at)
        retained=[]
        used=set()
        for f in range(mesh[2],mesh[2]+mesh[3]):
            raw=data[fb+f*FACE.size:fb+(f+1)*FACE.size]
            face=FACE.unpack(raw)
            if face[4] not in checker:
                retained.append(raw)
                used.update(face[:face[5]])
        if not retained:
            raise ValueError(f'checker omission would empty mesh {m}; no automatic instance removal')
        first_vertex,first_face=len(vertices),len(faces)
        remap={}
        for v in range(mesh[1]):
            if v in used:
                remap[v]=len(remap)
                start=vb+(mesh[0]+v)*VERTEX.size
                vertices.append(data[start:start+VERTEX.size])
        for raw in retained:
            face=FACE.unpack(raw)
            out=bytearray(raw)
            struct.pack_into(f'<{face[5]}H',out,0,*(remap[v] for v in face[:face[5]]))
            faces.append(bytes(out))
        out=bytearray(data[at:at+MESH.size])
        struct.pack_into('<4I',out,0,first_vertex,len(remap),first_face,len(retained))
        meshes.append(bytes(out))
    if len(faces)==h[4]:
        return data
    h[3],h[4]=len(vertices),len(faces)
    payload=b''.join([*meshes,*vertices,*faces])+data[mb:]
    h[1]=HEADER.size+len(payload)
    return HEADER.pack(*h)+payload


def share_positions(data):
    """Append DNM2 position references to validated prepare/local/world output.

    Keep all existing vertices, faces, UVs and materials byte-exact. Different
    signed zeros remain distinct. No runtime hashing or extra transform scratch.
    """
    header=list(HEADER.unpack_from(data))
    if header[0]!=0x314d4e44 or header[1]!=len(data):
        raise ValueError("position sharing requires prepared DNM1")
    vertex_base=HEADER.size+header[2]*MESH.size
    refs=[]
    for i in range(header[2]):
        mesh=MESH.unpack_from(data,HEADER.size+i*MESH.size)
        positions={}
        for v in range(mesh[1]):
            at=vertex_base+(mesh[0]+v)*VERTEX.size
            key=bytes(data[at:at+12])
            refs.append(positions.setdefault(key,v))
    header[0]=0x324d4e44
    header[1]+=2*len(refs)
    if len(refs)!=header[3] or header[1]>MAX_BYTES:
        raise ValueError("shared position table exceeds pack bounds")
    return HEADER.pack(*header)+data[HEADER.size:]+struct.pack(f"<{len(refs)}H",*refs)


def group_faces(data, size=32, mesh_sizes=None):
    """Spatial/normal-cone bounds over prepared geometry; retain face order.

    Reuses check_face_groups.c's longest-centroid-axis median partition, splitting
    on group boundaries. Only padding metadata changes in the existing tables.
    Input must be validated prepare/local/world output, not arbitrary binary.
    """
    if type(size) is not int or not 1 <= size <= 256:
        raise ValueError("invalid face group size")
    h=list(HEADER.unpack_from(data))
    if h[0]==0x314d4e44:
        data=share_positions(data)
        h=list(HEADER.unpack_from(data))
    if h[0]!=0x324d4e44 or h[1]!=len(data):
        raise ValueError("face grouping requires prepared DNM1/2")
    if mesh_sizes is not None and (len(mesh_sizes)!=h[2] or
            any(type(s) is not int or not 1<=s<=256 for s in mesh_sizes)):
        raise ValueError("invalid per-mesh face group sizes")
    out=bytearray(data)
    vb=HEADER.size+h[2]*MESH.size
    fb=vb+h[3]*VERTEX.size
    groups=[]
    cone_cos=math.cos(math.radians(30))
    cone_min_faces=8
    for m in range(h[2]):
        group_size=size if mesh_sizes is None else mesh_sizes[m]
        mesh=list(MESH.unpack_from(data,HEADER.size+m*MESH.size))
        vertices=[VERTEX.unpack_from(data,vb+(mesh[0]+v)*VERTEX.size)[:3] for v in range(mesh[1])]
        faces=[FACE.unpack_from(data,fb+(mesh[2]+f)*FACE.size) for f in range(mesh[3])]
        centers=[tuple(sum(vertices[f[v]][a]/f[5] for v in range(f[5])) for a in range(3)) for f in faces]

        def partition(order):
            if len(order)<=group_size:
                return [order]
            spans=[max(centers[f][a] for f in order)-min(centers[f][a] for f in order) for a in range(3)]
            axis=max(range(3),key=spans.__getitem__)
            order=sorted(order,key=lambda f:(centers[f][axis],f))
            half=((len(order)//group_size+1)//2)*group_size
            return partition(order[:half])+partition(order[half:])

        def cone_ready(members):
            if mesh[10]>1 or any(faces[f][7]&(1<<17) for f in members):
                return False
            axis=[sum(faces[f][8+a] for f in members) for a in range(3)]
            length=math.sqrt(sum(v*v for v in axis))
            return length and all(sum(axis[a]*faces[f][8+a] for a in range(3))/length>=cone_cos
                               for f in members)

        def cone_partition(members):
            if len(members)<2*cone_min_faces or cone_ready(members):
                return [members]
            spans=[max(faces[f][8+a] for f in members)-min(faces[f][8+a] for f in members)
                   for a in range(3)]
            axis=max(range(3),key=spans.__getitem__)
            members=sorted(members,key=lambda f:(faces[f][8+axis],f))
            half=len(members)//2
            return cone_partition(members[:half])+cone_partition(members[half:])

        mesh[11]=len(groups)
        spatial_groups=partition(list(range(mesh[3])))
        for spatial in spatial_groups:
            members_list=cone_partition(spatial) if len(spatial)>=2*cone_min_faces and \
                mesh[10]<=1 and not any(faces[f][7]&(1<<17) for f in spatial) else [spatial]
            for members in members_list:
                points=[vertices[faces[f][v]] for f in members for v in range(faces[f][5])]
                bounds=tuple(fn(p[a] for p in points) for fn in (min,max) for a in range(3))
                normal_center=floats(tuple(sum(faces[f][8+a] for f in members)/len(members) for a in range(3)))
                normal_radius=outward_float(max(math.dist(faces[f][8:11],normal_center) for f in members))
                facing_points=[vertices[faces[f][2]] for f in members]
                point_center=floats(tuple(sum(p[a] for p in facing_points)/len(facing_points) for a in range(3)))
                point_radius=outward_float(max(math.dist(p,point_center) for p in facing_points))
                flags=1 if len(members)>=cone_min_faces and cone_ready(members) else 0
                for f in members:
                    struct.pack_into('<I',out,fb+(mesh[2]+f)*FACE.size+28,len(groups))
                groups.append(GROUP.pack(*bounds,*normal_center,normal_radius,*point_center,point_radius,flags,0))
        mesh[12]=len(groups)-mesh[11]
        MESH.pack_into(out,HEADER.size+m*MESH.size,*mesh)
    out.extend(bytes((-len(out))&3))
    out.extend(b''.join(groups))
    if len(out)>MAX_BYTES:
        raise ValueError("face groups exceed resident geometry ceiling")
    h[0],h[1],h[7]=0x344d4e44,len(out),len(groups)
    HEADER.pack_into(out,0,*h)
    return bytes(out)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", type=Path)
    parser.add_argument("output", type=Path)
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument("--local-model", type=lambda s: int(s, 0),
                        help="ROM model address; one local mesh, no captured pose/view")
    mode.add_argument("--world-cells", action="store_true",
                      help="complete authored scenery; drop common captured camera/light context")
    mode.add_argument("--player-car", type=Path, metavar="ROM_IMAGES",
                      help="normal body/four wheels with authored render offsets; no captured pose")
    mode.add_argument("--player-car-complete", type=Path, metavar="ROM_IMAGES",
                      help="also include ROM-verified base-UV windows and normal shell")
    mode.add_argument("--intro-cars", type=Path, metavar="ROM_IMAGES",
                      help="complete intro body/wheel catalog; local coordinates, no captured poses")
    mode.add_argument("--race-cars", type=Path, metavar="ROM_IMAGES",
                      help="complete40-car course0 catalog, retaining intro parts")
    mode.add_argument("--finish-models", type=Path, metavar="ROM_IMAGES",
                      help="four authored finish parts in draw order; discard captured transforms")
    mode.add_argument("--result-models", type=Path, metavar="ROM_IMAGES",
                      help="eight authored result parts in draw order; discard validation transforms")
    mode.add_argument("--name-models", type=Path, metavar="ROM_IMAGES",
                      help="29 name glyphs in selection order plus marker; discard validation transforms")
    parser.add_argument("--world-objects", action="store_true",
                        help="with --world-cells: SOURCES/objects.txt (prepare_world_objects.py) names the instances "
                             "behind the cells: world objects with their cell, bitmap and selector")
    parser.add_argument("--omit-checker-faces", action="store_true",
                        help="offline: drop runtime-omitted screen-space checker faces and unused vertices")
    parser.add_argument("--collapse-flat-cutouts", action="store_true",
                        help="offline: merge exact planar, constant-lit adjacent cutout quads")
    parser.add_argument("--join-strips", action="store_true",
                        help="DNM5: ordered cross-face strip candidates; requires DNM4 face groups")
    parser.add_argument("--sort-faces", action="store_true",
                        help="DNM6: Dreamcast draw order (list, material, layer), coplanar layers and strips; "
                             "see prepare_face_order.py. Replaces the arcade face order and --join-strips records")
    parser.add_argument("--part-header", type=Path, metavar="INTRO_DRIVE_DATA_H",
                        help="with --sort-faces: car parts drawn with one matrix, for layers across meshes")
    parser.add_argument("--sort-materials", type=Path, metavar="DNT",
                        help="with --sort-faces: this pack's DNT2/4 materials, to keep one texture's materials adjacent")
    parser.add_argument("--align-tables", action="store_true",
                        help="final offline pass: 32-byte table bases, unchanged record strides/contents")
    parser.add_argument("--share-positions", action="store_true",
                        help="DNM2: share exact XYZ projection across distinct UV vertices")
    parser.add_argument("--name-header", type=Path,
                        help="also emit native name layout/colour constants; requires --name-models")
    parser.add_argument("--face-group-size", type=int,
                        help="DNM4: offline spatial/normal-cone face groups (1..256), implies position sharing")
    parser.add_argument("--asset-rules", type=Path,
                        help="JSON sidecar: defaults and source-address geometry overrides; requires --world-cells")
    args = parser.parse_args()
    if args.world_objects and not args.world_cells:
        parser.error('--world-objects requires --world-cells')
    if args.asset_rules and not args.world_cells:
        parser.error('--asset-rules requires --world-cells')
    if (args.part_header or args.sort_materials) and not args.sort_faces:
        parser.error('--part-header and --sort-materials require --sort-faces')
    if args.face_group_size is not None and not 1<=args.face_group_size<=256:
        parser.error('--face-group-size must be in 1..256')
    if args.name_header:
        if not args.name_models:
            parser.error('--name-header requires --name-models')
        if args.name_header.resolve()==args.output.resolve() or args.name_header.exists() or args.output.exists():
            parser.error('name header and model outputs must be distinct new files')
    SPLIT_CHECKER_VERTICES = bool(args.world_cells)
    data, report = prepare(args.sources)
    if args.local_model is not None:
        data = local_model(data, args.local_model)
        report = dict(bytes=len(data), local_model=hex(args.local_model),
                      sha256=hashlib.sha256(data).hexdigest())
    elif args.result_models or args.name_models:
        from inspect_course import words, model_list
        program,main=car_images(args.result_models or args.name_models)
        #226c24: descriptor at+4 in unaligned32-byte selection records.
        lists=([struct.unpack_from('<I',program,0x3b8f6+32*i)[0] for i in range(29)]
               if args.name_models else words(program,0x200000,0x23267c,8))
        if args.name_models:
            lists.append(0x288ac90)  #22750c: marker, binding29 after the glyphs.
        objects=[model_list(main,p) for p in lists]
        if any(len(rows)!=1 or not 1<=rows[0][3]<=128 for rows in objects):
            raise ValueError('presentation list must have one bounded part')
        addresses=[rows[0][0] for rows in objects]
        if len(set(addresses))!=len(lists):
            raise ValueError('presentation parts must be distinct')
        data=local_models(data,addresses)
        report=dict(bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
        report['name_models' if args.name_models else 'result_models']=addresses
        if args.name_models:
            # Original ring and initial-slot tables, not captured screen poses.
            ring=[dict(radius=struct.unpack_from('<f',program,0x3b8f2+32*i)[0],
                       angle=struct.unpack_from('<h',program,0x3b90a+32*i)[0],
                       exit_velocity=list(struct.unpack_from('<3f',program,0x3b8fa+32*i)),
                       exit_scale=struct.unpack_from('<f',program,0x3b906+32*i)[0],
                       exit_spin=list(struct.unpack_from('<3h',program,0x3b90c+32*i)))
                  for i in range(29)]
            initial_x=list(struct.unpack_from('<3f',program,0x3b8e2))
            if not all(math.isfinite(v) for v in initial_x+
                       [v for r in ring for v in [r['radius'],r['exit_scale'],*r['exit_velocity']]]):
                raise ValueError('nonfinite name layout')
            report['name_layout']=dict(ring=ring,initial_x=initial_x)
            #5418 copies the shared 3D palette, separate from the 5454/5460 UI
            #palettes. a74 builds the boot-mode colour translation, not RGB555.
            h=HEADER.unpack_from(data)
            at=HEADER.size+h[2]*MESH.size+h[3]*VERTEX.size+h[4]*FACE.size
            palette=[]
            for i in range(h[5]):
                header=MATERIAL.unpack_from(data,at+i*MATERIAL.size)[:4]
                index=(header[3]>>6)&1023
                if header[0]&0x4000 or 2*index>=0x7de:
                    raise ValueError('name material outside shared solid palette')
                raw_color=struct.unpack_from('<H',main,0x8955a0+2*index)[0]
                palette.append(dict(header=header,raw_color=raw_color,
                    argb_by_display_mode=name_solid_colors(raw_color)))
            report['name_solid_palette']=palette
    elif args.finish_models:
        from inspect_course import finish_models
        _,main=car_images(args.finish_models)
        catalog=finish_models(main)
        lists={r['pointer']:r['objects'] for r in catalog['lists']}
        addresses=[r[0] for p in catalog['draw_lists'] for r in lists[p]]
        data=local_models(data,addresses)
        report=dict(bytes=len(data),finish_models=addresses,sha256=hashlib.sha256(data).hexdigest())
    elif args.intro_cars or args.race_cars:
        data=intro_cars(data,args.intro_cars or args.race_cars,bool(args.race_cars))
        report=dict(bytes=len(data),intro_cars=True,race_cars=bool(args.race_cars),sha256=hashlib.sha256(data).hexdigest())
    elif args.player_car or args.player_car_complete:
        data=player_car(data,args.player_car or args.player_car_complete,bool(args.player_car_complete))
        report=dict(bytes=len(data),player_car=True,complete=bool(args.player_car_complete),sha256=hashlib.sha256(data).hexdigest())
    elif args.world_cells:
        data=world_cells(data,args.sources,args.world_objects)
        report.update(world_cells=True,world_objects=args.world_objects,sha256=hashlib.sha256(data).hexdigest())
    collapse_meshes=group_sizes=None
    if args.asset_rules:
        from asset_rules import load_asset_rules
        collapse_meshes,group_sizes,report['asset_rules']=load_asset_rules(
            args.asset_rules,data,args.collapse_flat_cutouts,args.face_group_size,args.join_strips)
    if args.omit_checker_faces:
        before=HEADER.unpack_from(data)
        data=omit_checker_faces(data)
        after=HEADER.unpack_from(data)
        report.update(omit_checker_faces=True,checker_faces_removed=before[4]-after[4],
                      vertices_removed=before[3]-after[3],vertices=after[3],faces=after[4],
                      bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
        if 'source_vertices' in report:
            report['vertex_bytes_saved']=(report['source_vertices']-after[3])*VERTEX.size
    if args.collapse_flat_cutouts or collapse_meshes:
        from optimize_flat_cutouts import collapse_flat_cutouts
        before=HEADER.unpack_from(data)
        data,pairs=collapse_flat_cutouts(data,collapse_meshes)
        after=HEADER.unpack_from(data)
        report.update(flat_cutout_pairs_collapsed=pairs,vertices=after[3],faces=after[4],
                      flat_cutout_vertices_removed=before[3]-after[3],
                      bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
        if 'source_vertices' in report:
            report['vertex_bytes_saved']=(report['source_vertices']-after[3])*VERTEX.size
    if args.share_positions:
        data=share_positions(data)
        report.update(shared_positions=True,bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
    if args.face_group_size is not None or group_sizes is not None:
        data=group_faces(data,args.face_group_size or 32,group_sizes)
        reported_size=args.face_group_size if group_sizes is None else (
            group_sizes[0] if len(set(group_sizes))==1 else None)
        report.update(face_group_size=reported_size,groups=HEADER.unpack_from(data)[7],
                      shared_positions=True,bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
    strip_meshes=({rule['mesh'] for rule in report['asset_rules']['geometry'] if rule['join_strips']}
                  if args.asset_rules else None)
    if (strip_meshes is None and args.join_strips) or strip_meshes:
        from prepare_strips import prepare_strips
        data,strip_report=prepare_strips(data,strip_meshes)
        report.update(strip_report,bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
    if args.sort_faces:
        from prepare_face_order import sort_pack, intro_part_lists
        if HEADER.unpack_from(data)[0]==0x314d4e44:
            data=share_positions(data)
        data,order_report,_=sort_pack(
            data,intro_part_lists(args.part_header.read_text()) if args.part_header else (),
            materials=args.sort_materials.read_bytes() if args.sort_materials else None)
        report.update(face_order=order_report,shared_positions=True,bytes=len(data),
                      sha256=hashlib.sha256(data).hexdigest())
    if args.align_tables:
        data=align_tables(data)
        report.update(aligned_tables=True,bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
    with args.output.open("xb") as out:
        out.write(data)
    if args.name_header:
        with args.name_header.open('x') as out:
            out.write(name_data_header(report))
    print(json.dumps(report, indent=2))
