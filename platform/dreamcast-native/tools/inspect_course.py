"""Inventory daytona93's authored scenery cells, not sampled visibility alone.

Reads verified, user-owned importer output. Optional model-source directories
are cross-referenced by ROM address; ambiguous matches are never guessed.
--output writes a NEW local JSON file containing ROM-derived table data. Keep
it ignored; it is host evidence, not a target pack or complete course export.
Includes authored lateral/spawn profiles; collision meshes, moving entities,
materials and residency are separate work.
--cell-assets SOURCE_DIR PREPARED_VQ instead budgets captured per-cell materials
and adjacent transitions. Those costs do not cover changing camera/light state.
"""
import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import struct

from prepare_models import FACE, HEADER, MESH, VERTEX, floats, prepare, rows
from prepare_materials import validate_textures

# Analysis fingerprints, not ROM content. These addresses are daytona93-only.
IMAGES = {
    "program.bin": (0x200000, "f830f4a65e1a4cb0dc3520f4bea4184dcfbd1aa5a8b129c89977e8cabe012ef4"),
    "main_data.bin": (0x2000000, "bca85554517b86ecc3cbd3a725e0b217141b6516e5be2a84dabbe1dc9ad57a9e"),
}
MAIN_BASE = 0x02000000


def words(data, base, address, count):
    offset = address - base
    if address % 4 or offset < 0 or count < 1 or offset + 4*count > len(data):
        raise ValueError("unaligned or out-of-image course table read")
    return struct.unpack_from(f"<{count}I", data, offset)


def intro_camera(main, start=0x2864030):
    """Authored 48-byte keys selected by5b68; raw fields, not sampled poses.

    5cac advances by48; time=-1 follows the next word as a source link.
    Keep unresolved fields as words until the interpolation oracle names them.
    """
    records, visited, address = [], set(), start
    for _ in range(2048):
        if address in visited:
            return dict(start=start, loop=address, records=records,
                        source_bytes=sum(8 if r["words"][0]==0xffffffff else 48 for r in records))
        visited.add(address)
        tick, link = words(main, MAIN_BASE, address, 2)
        data = (tick, link) if tick==0xffffffff else words(main, MAIN_BASE, address, 12)
        records.append(dict(address=address, words=data))
        address = link if tick==0xffffffff else address+48
    raise ValueError("intro camera exceeds 2048-record guard")


def intro_stages(main, start):
    """226378/2264f4 authored spawn/event graph; never recorded frame state."""
    stages,visited,address=[],set(),start
    while address not in visited:
        if len(stages)>=64:
            raise ValueError("intro stage graph exceeds64 stages")
        visited.add(address)
        source=address
        events,count,camera=words(main,MAIN_BASE,address,3)
        if not 1<=count<=44:
            raise ValueError("invalid intro car count")
        spawn=[words(main,MAIN_BASE,address+12+16*i,4) for i in range(count)]
        timeline=[]
        for index in range(256):
            row=words(main,MAIN_BASE,events+16*index,4)
            timeline.append(row)
            if row[0]==0xffffffff:
                address=None
                break
            actor=row[2]&65535
            if actor&32768:
                address=row[3]
                break
            if actor>=count:
                raise ValueError("intro event references absent car")
        else:
            raise ValueError("intro event byte cursor would wrap")
        stages.append(dict(source=source,event_source=events,camera=camera,spawn=spawn,events=timeline))
        if address is None:
            break
    return dict(start=start,loop=address,stages=stages)


def model_list(main, pointer):
    # Only these exact RAM descriptors have canonical ROM geometry/UV proof.
    windows={0x2890564:(0x1c8,0x805168,0x80009c,11,0),
             0x2890578:(0x241,0x8051c8,0x8000b8,9,0)}
    if pointer in windows:
        if tuple(words(main,MAIN_BASE,pointer,5))!=windows[pointer]:
            raise ValueError('canonical intro window descriptor changed')
        return [windows[pointer][:4]]
    result=[]
    for i in range(128):
        at=pointer+16*i
        model=words(main,MAIN_BASE,at,1)[0]
        if not model:
            return result
        row=words(main,MAIN_BASE,at,4)
        if not 0x800000<=model<0xc00000 or row[1]>=0x800000 or row[2]>=0x800000:
            raise ValueError("intro body list is not ROM geometry/texture data")
        result.append(row)
    raise ValueError("intro model list exceeds128 objects")


def podium_sources(main):
    """2265a4 selects by course slot, then reuses226378's spawn/event layout.

    Source inventory only: phase27/28 callbacks, lights and ownership are not
    implemented by parsing this graph. Keep slot aliases and raw camera words.
    """
    result = []
    for slot, start in enumerate(words(main, MAIN_BASE, 0x2865a70, 4)):
        graph = intro_stages(main, start)
        cameras = sorted({stage['camera'] for stage in graph['stages']})
        result.append(dict(slot=slot, graph=graph,
                           cameras=[intro_camera(main, camera) for camera in cameras]))
    return result


def finish_models(main):
    """4580's two render branches, in original submission order.

    4588 tests57 > signed timer BEFORE decrement; both branches draw the same
    four lists with different transforms. Initialization4578 sets256. Costs are authored strip
    budgets, not decoded face counts or VRAM sizes. No sampled matrices.
    """
    draws=(0x288b460,0x288b488,0x288b44c,0x288b474)
    return dict(initial_timer=256, branch_threshold=57, moving_ticks=200,
                settled_when="signed timer <57 before decrement",
                draw_lists=draws,
                lists=[dict(pointer=p,objects=model_list(main,p)) for p in sorted(set(draws))],
                coverage="authored finish geometry bindings/order only; animation, materials and VRAM unverified")


def intro_car_models(program, main, graph, race=False):
    """e594's per-ID body choices plus the separate d05c actor and wheel lists.

    This is a source binding catalog, NOT proof of mesh/material residency.
    Window geometry is canonical; animation/crash overlays remain separate.
    """
    cars={}
    for stage in graph["stages"]:
        for index,spawn in enumerate(stage["spawn"]):
            kind=spawn[3]
            if kind not in (0xcf74,0xcfc0,0xd05c):
                raise ValueError("unknown intro model initialization")
            pointers=(0x288d65c,) if kind==0xd05c else words(main,MAIN_BASE,0x286fac0+48*index,8)
            cars[(index,kind)]=pointers
    if race:
        # e594 indexes all40 course0 IDs directly, not ID%20. Keep the special
        # attract actor too so the enlarged pack remains usable by that scene.
        cars={key:value for key,value in cars.items() if key[1]==0xd05c}
        for index in range(40):
            cars[(index,0xcde0)]=words(main,MAIN_BASE,0x286fac0+48*index,8)
    selectors=words(main,MAIN_BASE,0x286fab0,4)
    wheels=struct.unpack_from('<4I',program,0x32096)
    phase_wheels=words(program,0,0x832c,16) # High/medium detail, eight authored phases each.
    special_phases=words(program,0,0x34d58,8) # d144: shared frame&7, four repeated extra models.
    windows=(0x2890564,0x2890578)
    shadow=0x288b7a8
    crash_parts=(0x288f600,0x288f5ec) # Detail1/2, raw5c!=0, same body matrix.
    pointers=set(selectors+wheels+phase_wheels+windows+special_phases+crash_parts+(shadow,))
    for values in cars.values():
        pointers.update(values)
    lists=[dict(pointer=p,objects=model_list(main,p)) for p in sorted(pointers)]
    return dict(cars=[dict(id=i,source_updater=k,lists=v) for (i,k),v in sorted(cars.items())],
                selectors=selectors,wheels=wheels,phase_wheels=phase_wheels,special_phases=special_phases,windows=windows,shadow=shadow,crash_parts=crash_parts,lists=lists,
                coverage="body/wheel/canonical window/d144/shadow/low-detail crash bindings; excludes animation and VRAM proof")


def intro_crashes(main, selectors=(0x2896e58,0x2896e5c,0x2896e60)):
    """15de0/15e84: three directories, always variant0; authored animation.

    Preserve the unused word in each16-byte row too. No sampled car poses.
    The first two also serve boundary crashes; the third is global flag28.
    """
    result=[]
    for selector in selectors:
        directory=words(main,MAIN_BASE,selector,1)[0]
        variants,sequence=words(main,MAIN_BASE,directory,2)
        if not variants:
            raise ValueError("empty intro crash directory")
        count=words(main,MAIN_BASE,sequence,1)[0]
        if not 1 <= count <= 2048:
            raise ValueError("intro crash exceeds bounded row count")
        data=words(main,MAIN_BASE,sequence+4,4*count)
        rows=[data[i:i+4] for i in range(0,len(data),4)]
        result.append(dict(selector=selector,directory=directory,variants=variants,
                           sequence=sequence,rows=rows,source_bytes=4+16*count))
    return result


def courses(program, main):
    # 0x38b0..0x38b8 selects this table into RAM 0x501420. 0x17828
    # selects cell pointers; 0x17a8c..0x17aa0 reads four-word records
    # terminated by model=0. The fourth word is cost, NOT a face count.
    result, total = [], 0
    for slot, table in enumerate(words(program, 0, 0x39b0, 4)):
        cells = []
        for index, pointer in enumerate(words(main, MAIN_BASE, table, 256)):
            objects = []
            address = pointer
            while words(main, MAIN_BASE, address, 1)[0]:
                model, uv, headers, cost = words(main, MAIN_BASE, address, 4)
                if not 0x800000 <= model < 0xc00000 or uv >= 0x800000 or headers >= 0x800000:
                    raise ValueError("expected ROM-only scenery geometry/texture addresses")
                total += 1
                if total > 65536:
                    raise ValueError("course inventory exceeds 65536 object-record guard")
                objects.append(dict(model=model, texture_points=uv, texture_headers=headers, cost=cost))
                address += 16
            x, z = index & 15, index >> 4
            # Nominal grid coordinates from 0x16e10..0x16e50: round X/Z,
            # add1024, shift7, mask15. NOT mesh bounds or exact FP edge rules.
            cells.append(dict(index=index, pointer=pointer,
                nominal_xz=[x*128-1024, z*128-1024, (x+1)*128-1024, (z+1)*128-1024],
                objects=objects))
        result.append(dict(slot=slot, table=table, cells=cells))
    return result


def road_profiles(program, main):
    # 0x39c0 loads descriptors; 0x380c selects the spatial->profile map and
    # 0x38f8 the profiles. 0x13b28 indexes 164-byte profiles / 20-byte lanes.
    result = []
    for slot, descriptor in enumerate(words(program, 0, 0x3a48, 4)):
        pointers = words(main, MAIN_BASE, descriptor, 8)
        logical, map_count = words(main, MAIN_BASE, pointers[1], 2)
        count, lanes = words(main, MAIN_BASE, pointers[5], 2)
        if (not 0 < logical <= 4096 or logical != count or lanes != 8 or
                not 0 < map_count <= 65536 or pointers[2] != pointers[1]+8+4*map_count or
                pointers[6] != pointers[5]+8+164*(count+1)):
            raise ValueError("unexpected authored road table layout")
        mapping = words(main, MAIN_BASE, pointers[1]+8, map_count)
        if any((v & 65535) > count for v in mapping):
            raise ValueError("road map points outside authored profiles")
        words(main, MAIN_BASE, pointers[5]+8, 41*(count+1))  # Bounds before decoding.
        records = []
        for i in range(count+1):
            offset = pointers[5]+8+i*164-MAIN_BASE
            first, last, control = struct.unpack_from("<bbh", main, offset)
            if not 0 <= first <= last < 8:
                raise ValueError("invalid authored road lane limits")
            samples = [struct.unpack_from("<3f4h", main, offset+4+lane*20) for lane in range(8)]
            if any(not math.isfinite(v) for sample in samples for v in sample[:3]):
                raise ValueError("nonfinite road position")
            records.append(dict(first_lane=first, last_lane=last, profile_control=control,
                lanes=[dict(xyz=s[:3], angles=s[3:6], extra=s[6]) for s in samples]))
        result.append(dict(slot=slot, descriptor=descriptor, logical_count=logical,
            profile_address=pointers[5]+8, profile_bytes=164*len(records),
            mapping_address=pointers[1]+8, mapping_bytes=4*map_count,
            mapping=[dict(profile=v & 65535, flags=v >> 16) for v in mapping], profiles=records))
    return result


def race_grid(program, main, roads):
    """Static01700/e594/e20c inputs, not a capture of already moving cars."""
    task_count = words(program, 0, 0x2f1f0, 1)[0]
    if not 1 <= task_count <= 1024:
        raise ValueError("invalid task initializer count")
    tasks = [words(program, 0, 0x2f1f4+20*i, 5) for i in range(task_count)]
    player = [r for r in tasks if r[4] == 0x501220]
    if len(player) != 1 or player[0][1] > 255:
        raise ValueError("ambiguous player initializer")
    player_id = player[0][1]
    first = tasks.index(player[0])
    result = []
    for slot, road in enumerate(roads):
        count = words(program, 0, 0x3980+4*slot, 1)[0]
        if not 1 <= count <= 44 or player_id >= count:
            raise ValueError("invalid race vehicle count/player ID")
        pool = tasks[first:first+count]
        if len(pool) != count or any(row[1] != i or row[2] != 768 for i, row in enumerate(pool)):
            raise ValueError("vehicle initializer pool is not the expected ordered grid")
        # Signed16 course offset, extracted through an aligned word read.
        raw = words(program, 0, 0x34be8+4*(slot//2), 1)[0] >> (16*(slot%2)) & 65535
        offset = raw if raw < 32768 else raw-65536
        profiles = road["logical_count"]
        if not 1 <= profiles <= 4096:
            raise ValueError("invalid grid profile count")
        cars = []
        for car_id in range(count):
            row = words(main, MAIN_BASE, 0x286fac0+48*car_id, 12)
            speed = struct.unpack("<f", struct.pack("<I", row[10]))[0]
            if row[8] >= 8 or not math.isfinite(speed):
                raise ValueError("invalid grid route or speed")
            profile = ((row[9]+offset)&0xffffffff) % profiles
            cars.append(dict(id=car_id, route=row[8], authored_profile=row[9],
                profile=profile, previous=row[9]&65535, initial_lap=-int(profile >= profiles//2),
                lateral=384 if car_id&1 else 1408, speed_parameter_bits=row[10], style=row[11]&255))
        result.append(dict(slot=slot, player_id=player_id, profile_offset=offset, cars=cars))
    return result


def model_index(inventory):
    result = defaultdict(list)
    for course in inventory:
        for cell in course["cells"]:
            for obj in cell["objects"]:
                result[obj["model"]].append((course["slot"], cell["index"]))
    return result


def match_models(inventory, source):
    packed, _ = prepare(source)  # Existing finite/topology/bounds/manifest checks.
    index = model_index(inventory)
    matched, ambiguous, unmatched, groups = [], 0, 0, defaultdict(int)
    for fields in rows(source / "instances.txt"):
        mesh, address, window, reverse = map(int, fields[:4])
        candidates = index.get(address, [])
        if not candidates:
            unmatched += 1
            continue
        if len(candidates) != 1:
            ambiguous += 1
            continue
        slot, cell = candidates[0]
        record = MESH.unpack_from(packed, HEADER.size + mesh*MESH.size)
        bounds = record[4:10]
        nominal = inventory[slot]["cells"][cell]["nominal_xz"]
        matrix = floats(fields[4:16])
        groups[(window, reverse, matrix)] += 1
        matched.append(dict(mesh=mesh, model=address, slot=slot, cell=cell, bounds=bounds,
            vertices=record[1], faces=record[3],
            geometry_bytes=MESH.size + record[1]*VERTEX.size + record[3]*FACE.size,
            extends_beyond_nominal_cell=(bounds[0] < nominal[0] or bounds[2] < nominal[1] or
                                         bounds[3] > nominal[2] or bounds[5] > nominal[3])))
    return dict(source=str(source), matched=matched, ambiguous_instances=ambiguous,
        unmatched_instances=unmatched,
        shared_matrix_group_sizes=sorted(groups.values(), reverse=True))


def match_car_models(catalog, source):
    prepare(source)  # Same topology/material/provenance validation as scenery matching.
    required={obj[0] for group in catalog["lists"] for obj in group["objects"]}
    owners=defaultdict(set)
    for fields in rows(source / "instances.txt"):
        mesh,address=map(int,fields[:2])
        if address in required:
            owners[address].add(mesh)
    return dict(source=str(source),required_models=len(required),
                matched=[dict(model=address,mesh=next(iter(meshes))) for address,meshes in sorted(owners.items()) if len(meshes)==1],
                ambiguous=[address for address,meshes in sorted(owners.items()) if len(meshes)!=1],
                missing=sorted(required-owners.keys()),
                coverage="source model identity only; not complete materials, live animation or VRAM residency")


def summarize(inventory):
    result = []
    for course in inventory:
        objects = [obj for cell in course["cells"] for obj in cell["objects"]]
        result.append(dict(slot=course["slot"], table=hex(course["table"]),
            cells=len(course["cells"]), records=len(objects),
            unique_models=len({obj["model"] for obj in objects}),
            nonzero_cost_cells=sum(any(obj["cost"] for obj in cell["objects"]) for cell in course["cells"]),
            total_reference_cost=sum(obj["cost"] for obj in objects),
            max_cell_reference_cost=max(sum(obj["cost"] for obj in cell["objects"]) for cell in course["cells"])))
    return result


def neighborhood(cell, radius):
    x, z = cell & 15, cell >> 4
    # The authored lookup masks both coordinates with 15. Include wraparound
    # residency too; clipping at the array edge undercounts possible overlap.
    return {((x+a) & 15) + 16*((z+b) & 15)
            for b in range(-radius,radius+1) for a in range(-radius,radius+1)}


def usage(resources, payloads):
    payload = sum(payloads[name] for name in resources)
    return dict(textures=len(resources), payload_bytes=payload,
                estimated_allocation_bytes=payload + 32*len(resources))


def budget_cells(source, prepared, crop_output=None):
    _, geometry = prepare(source)
    quality, entries = validate_textures(source, prepared)
    crop_report = None
    if crop_output is not None:
        from prepare_material_crops import prepare_crops
        entries, crop_report = prepare_crops(source, prepared, entries, crop_output)
        prepared = crop_output
    bindings = {tuple(e["header"]) + (e["luma"],): e["file"] for e in entries}
    if len(bindings) != len(entries):
        raise ValueError("duplicate material keys")
    solid_keys = set()
    for fields in rows(source / "solid_materials.txt"):
        values = tuple(map(int, fields))
        if (len(values) != 6 or values[0] & 0x4000 or
                not all(0 <= v <= 65535 for v in values[:4]) or
                not 0 <= values[4] <= 255 or values[5] >> 24 != 255 or
                values[:5] in solid_keys):
            raise ValueError("invalid or duplicate solid material")
        solid_keys.add(values[:5])
    # Charge actual validated DcTx payloads, not unverified per-entry estimates.
    payloads = {}
    for name in set(bindings.values()):
        data = (prepared / name).read_bytes()
        payloads[name] = len(data) - (data[9]+1)*32
    counts, next_instance = [], 0
    for fields in rows(source / "cells.txt"):
        if len(fields) != 3:
            raise ValueError("invalid cell range")
        cell, first, count = map(int, fields)
        if cell != len(counts) or first != next_instance or count < 0:
            raise ValueError("unordered or overlapping cell ranges")
        counts.append(count)
        next_instance += count
    if len(counts) != 256 or next_instance != geometry["instances"]:
        raise ValueError("cell ranges do not cover geometry")
    cells, seen, keys = [set() for _ in range(256)], set(), set()
    for fields in rows(source / "cell_materials.txt"):
        if len(fields) != 6:
            raise ValueError("invalid cell material")
        cell, *values = map(int, fields)
        key = tuple(values)
        if (not 0 <= cell < 256 or not counts[cell] or
                not all(0 <= v <= 65535 for v in values[:4]) or not 0 <= values[4] <= 255 or
                (cell,key) in seen):
            raise ValueError("invalid or duplicate cell dependency")
        seen.add((cell,key))
        if key not in bindings and key not in solid_keys:
            raise ValueError("missing cell material binding")
        keys.add(key)
        if key[0] & 0x4000:
            cells[cell].add(bindings[key])
    if keys != bindings.keys() | solid_keys:
        raise ValueError("cell dependencies do not cover material bindings")
    peaks = []
    for radius in (0,1,2):
        current, transition = [], []
        for cell in range(256):
            window = neighborhood(cell, radius)
            current.append(dict(center=cell, **usage(set().union(*(cells[c] for c in window)), payloads)))
            for to in sorted(neighborhood(cell,1) - {cell}):
                overlap = window | neighborhood(to, radius)
                transition.append(dict(start=cell, end=to,
                    **usage(set().union(*(cells[c] for c in overlap)), payloads)))
        peaks.append(dict(radius=radius,
            peak_current=max(current, key=lambda x:x["estimated_allocation_bytes"]),
            peak_adjacent_overlap=max(transition, key=lambda x:x["estimated_allocation_bytes"])))
    return dict(coverage="scenery snapshot dependencies only; excludes road/cars/HUD, changed lighting, allocator fragmentation and safety reserve",
        adjacency="eight directions on the authored wrapping 16x16 grid; not a camera/draw-distance visibility proof",
        allocation_assumption="validated VQ payload plus measured 32-byte overhead per resource; estimates, not new hardware measurements",
        geometry=geometry, full_course=usage(set(payloads), payloads),
        alpha_changed_pixels=quality["alpha_changed_pixels"],
        alpha_quality_scope="original uncropped VQ against source; crop texel identity is checked separately",
        crop_candidates=crop_report,
        neighborhoods=peaks)


def self_test():
    model=struct.pack('<5I',0x800001,12,16,3,0)
    assert model_list(model,MAIN_BASE)==[(0x800001,12,16,3)]
    for bad in (model[:-1],struct.pack('<I',0x13b)+model[4:]):
        try:
            model_list(bad,MAIN_BASE)
        except ValueError:
            continue
        raise AssertionError('invalid model list accepted')
    print('PASS: bounded ROM model-list records and malformed inputs')
    finish=bytearray(0x88b488+20)
    for i,p in enumerate((0x288b44c,0x288b460,0x288b474,0x288b488)):
        struct.pack_into('<5I',finish,p-MAIN_BASE,0x800001+i,12,16,3,0)
    catalog=finish_models(finish)
    assert catalog['draw_lists']==(0x288b460,0x288b488,0x288b44c,0x288b474)
    assert [r['objects'][0][0] for r in catalog['lists']]==list(range(0x800001,0x800005))
    try:
        finish_models(finish[:-1])
    except ValueError:
        pass
    else:
        raise AssertionError('truncated finish model list accepted')
    print('PASS: ordered finish model branches and truncated descriptor rejection')
    stage=bytearray(60)
    struct.pack_into("<3I",stage,0,MAIN_BASE+28,1,MAIN_BASE+100)
    struct.pack_into("<4I",stage,12,0,100,384<<16,0xcf74)
    struct.pack_into("<4I",stage,28,10,120,384<<16,0)
    struct.pack_into("<4I",stage,44,20,0,65535,MAIN_BASE)
    graph=intro_stages(stage,MAIN_BASE)
    assert graph["loop"]==MAIN_BASE and len(graph["stages"])==1 and graph["stages"][0]["source"]==MAIN_BASE
    absent=bytearray(stage);struct.pack_into("<I",absent,36,1)
    unaligned=bytearray(stage);struct.pack_into("<I",unaligned,56,MAIN_BASE+1)
    for bad in (stage[:-1],stage[:4]+struct.pack("<I",0)+stage[8:],
                stage[:4]+struct.pack("<I",45)+stage[8:],absent,unaligned):
        try:
            intro_stages(bad,MAIN_BASE)
        except ValueError:
            continue
        raise AssertionError("invalid intro stage accepted")
    terminal=bytearray(stage);struct.pack_into("<I",terminal,28,0xffffffff)
    assert intro_stages(terminal,MAIN_BASE)["loop"] is None
    print("PASS: authored spawn/event stage loop and malformed inputs")
    crash=struct.pack("<8I",MAIN_BASE+4,1,MAIN_BASE+12,1,0xaaaa,0xbbbb,0xcccc,0xdddd)
    parsed=intro_crashes(crash,(MAIN_BASE,))
    assert parsed[0]["rows"]==[(0xaaaa,0xbbbb,0xcccc,0xdddd)] and parsed[0]["source_bytes"]==20
    for bad in (crash[:-1],crash[:4]+struct.pack("<I",0)+crash[8:],
                crash[:12]+struct.pack("<I",2049)+crash[16:],
                struct.pack("<I",MAIN_BASE+1)+crash[4:]):
        try:
            intro_crashes(bad,(MAIN_BASE,))
        except ValueError:
            continue
        raise AssertionError("malformed intro crash source accepted")
    print("PASS: authored crash rows, variant0, malformed count/pointer/truncation")
    camera = bytearray(104)
    struct.pack_into("<12I",camera,0,0,*range(1,12))
    struct.pack_into("<12I",camera,48,100,*range(11,22))
    struct.pack_into("<2I",camera,96,0xffffffff,MAIN_BASE)
    parsed = intro_camera(camera,MAIN_BASE)
    assert parsed["source_bytes"]==104 and parsed["loop"]==MAIN_BASE
    assert len(parsed["records"])==3 and parsed["records"][1]["words"]==tuple([100]+list(range(11,22)))
    for bad in (camera[:95],camera[:100]):
        try:
            intro_camera(bad,MAIN_BASE)
        except ValueError:
            continue
        raise AssertionError("truncated camera source accepted")
    for link in (MAIN_BASE+1,MAIN_BASE+104,MAIN_BASE-4):
        bad=bytearray(camera);struct.pack_into("<I",bad,100,link)
        try:
            intro_camera(bad,MAIN_BASE)
        except ValueError:
            continue
        raise AssertionError("invalid camera link accepted")
    print("PASS: authored camera keys, loop link, exact raw fields and malformed bounds")
    podium = bytearray(0x865a80)
    podium[:60] = stage
    struct.pack_into('<I', podium, 8, MAIN_BASE+64)
    struct.pack_into('<12I', podium, 64, 0, *range(1, 12))
    struct.pack_into('<2I', podium, 112, 0xffffffff, MAIN_BASE+64)
    podium[128:188] = stage
    struct.pack_into('<3I', podium, 128, MAIN_BASE+156, 1, MAIN_BASE+64)
    struct.pack_into('<I', podium, 184, MAIN_BASE+128)
    struct.pack_into('<4I', podium, 0x865a70, MAIN_BASE, MAIN_BASE+128, MAIN_BASE, MAIN_BASE+128)
    parsed = podium_sources(podium)
    assert [entry['slot'] for entry in parsed] == [0, 1, 2, 3]
    assert [entry['graph']['start'] for entry in parsed] == [MAIN_BASE, MAIN_BASE+128]*2
    assert parsed[0]['graph']['stages'][0]['spawn'] == graph['stages'][0]['spawn']
    assert parsed[1]['graph']['loop'] == MAIN_BASE+128
    assert parsed[0]['cameras'][0]['records'][0]['words'] == tuple([0]+list(range(1, 12)))
    for at, value in ((0x865a70, MAIN_BASE+1), (4, 45), (116, MAIN_BASE+len(podium))):
        bad = bytearray(podium)
        struct.pack_into('<I', bad, at, value)
        try:
            podium_sources(bad)
        except ValueError:
            continue
        raise AssertionError('malformed podium source accepted')
    try:
        podium_sources(podium[:-1])
    except ValueError:
        pass
    else:
        raise AssertionError('truncated podium selector table accepted')
    print('PASS: podium slot aliases, exact spawn/camera fields and malformed sources')
    assert neighborhood(0,1) == {255,240,241,15,0,1,31,16,17}
    assert neighborhood(255,1) == {238,239,224,254,255,240,14,15,0}
    assert len(neighborhood(119,1)) == 9 and len(neighborhood(119,2)) == 25
    assert neighborhood(119,0) == {119}
    assert usage({"shared"} | {"shared","new"}, {"shared":64,"new":128}) == dict(
        textures=2, payload_bytes=192, estimated_allocation_bytes=256)
    program, main = bytearray(0x39c0), bytearray(256*4+36)
    struct.pack_into("<4I", program, 0x39b0, *([MAIN_BASE]*4))
    struct.pack_into("<256I", main, 0, *([MAIN_BASE+1024]*256))
    struct.pack_into("<9I", main, 1024, 0x800004, 10, 20, 7, 0x800025, 30, 40, 11, 0)
    parsed = courses(program, main)
    assert len(parsed) == 4 and all(len(c["cells"]) == 256 for c in parsed)
    assert parsed[0]["cells"][0]["nominal_xz"] == [-1024,-1024,-896,-896]
    assert parsed[0]["cells"][255]["nominal_xz"] == [896,896,1024,1024]
    assert summarize(parsed)[0]["total_reference_cost"] == 256*18
    assert len(model_index(parsed)[0x800004]) == 1024  # Ambiguous, not first-match wins.
    for data, base, at, n in ((main, MAIN_BASE, MAIN_BASE+1, 1),
                              (main, MAIN_BASE, MAIN_BASE-4, 1),
                              (main, MAIN_BASE, MAIN_BASE+len(main), 1)):
        try:
            words(data, base, at, n)
        except ValueError:
            continue
        raise AssertionError("bad read accepted")
    for at, value in ((0, MAIN_BASE+len(main)), (1024, 0x1000000),
                      (1028, 0x800000), (1032, 0x800000), (1056, 0x800004)):
        bad = bytearray(main)
        struct.pack_into("<I", bad, at, value)
        try:
            courses(program, bad)
        except ValueError:
            continue
        raise AssertionError("malformed course accepted")
    empty = bytearray(main)
    struct.pack_into("<I", empty, 1024, 0)
    assert summarize(courses(program, empty))[0]["records"] == 0
    program, main = bytearray(0x3a58), bytearray(1024)
    struct.pack_into("<4I", program, 0x3a48, *([MAIN_BASE]*4))
    struct.pack_into("<8I", main, 0, 0, MAIN_BASE+32, MAIN_BASE+52, 0, 0,
                     MAIN_BASE+64, MAIN_BASE+72+164*3, 0)
    struct.pack_into("<5I", main, 32, 2, 3, 0, 0x12340001, 2)
    struct.pack_into("<2I", main, 64, 2, 8)
    for i in range(3):
        struct.pack_into("<bbh", main, 72+i*164, 1, 6, -i)
        for lane in range(8):
            struct.pack_into("<3f4h", main, 76+i*164+lane*20, i, lane, i+lane, 1, -2, 3, 4)
    parsed = road_profiles(program, main)
    assert len(parsed) == 4 and parsed[0]["profile_bytes"] == 492
    assert parsed[0]["mapping"][1] == dict(profile=1, flags=0x1234)
    assert parsed[0]["profiles"][2]["lanes"][7]["xyz"] == (2,7,9)
    for at, fmt, value in ((32,"I",0), (36,"I",65537), (48,"I",3), (68,"I",7),
                            (72,"b",-1), (73,"b",8), (76,"f",float("nan")), (24,"I",MAIN_BASE+1000)):
        bad = bytearray(main)
        struct.pack_into("<"+fmt, bad, at, value)
        try:
            road_profiles(program, bad)
        except ValueError:
            continue
        raise AssertionError("malformed road profile accepted")
    print("PASS: road descriptors, spatial map/flags, distinct terminal profile, lane bounds and malformed data")
    print("PASS: four course tables, multi-object/empty cells, ambiguity, bounds, malformed records and wrapped residency neighborhoods")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", nargs="?", type=Path)
    parser.add_argument("--models", nargs="+", type=Path, default=[])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--cell-assets", nargs=2, type=Path, metavar=("SOURCE_DIR", "PREPARED_VQ"))
    parser.add_argument("--crop-output", type=Path, help="NEW candidate directory; requires --cell-assets and exported material_uv.txt; not target-ready")
    args = parser.parse_args()
    if args.crop_output and (not args.cell_assets or args.self_test):
        parser.error("--crop-output requires --cell-assets")
    if args.self_test:
        self_test()
    elif args.cell_assets:
        if args.images or args.models:
            parser.error("choose ROM inventory or prepared-cell budgeting, not both")
        report = budget_cells(*args.cell_assets, crop_output=args.crop_output)
        if args.output:
            with args.output.open("x") as out:
                json.dump(report, out, indent=2)
                out.write("\n")
        print(json.dumps(report, indent=2))
    else:
        if args.images is None:
            parser.error("provide the verified daytona93 importer directory")
        images = {}
        for name, (size, digest) in IMAGES.items():
            path = args.images / name
            if path.stat().st_size != size:
                raise ValueError(f"incorrect daytona93 image size: {name}")
            data = path.read_bytes()
            if hashlib.sha256(data).hexdigest() != digest:
                raise ValueError(f"unrecognized daytona93 image: {name}")
            images[name] = data
        inventory = courses(images["program.bin"], images["main_data.bin"])
        roads = road_profiles(images["program.bin"], images["main_data.bin"])
        camera = intro_camera(images["main_data.bin"])
        crashes = intro_crashes(images["main_data.bin"])
        stages=intro_stages(images["main_data.bin"],words(images["main_data.bin"],MAIN_BASE,0x2865a60,1)[0])
        car_models=intro_car_models(images["program.bin"],images["main_data.bin"],stages)
        report = dict(coverage="authored scenery cells and lateral/spawn profiles; no road collision mesh/entity/material/VRAM coverage",
            images={name: digest for name, (_, digest) in IMAGES.items()},
            summary=summarize(inventory),
            intro_camera=dict(start=camera["start"], loop=camera["loop"],
                              records=len(camera["records"]), source_bytes=camera["source_bytes"]),
            intro_stages=stages,
            podium_sources=podium_sources(images["main_data.bin"]),
            intro_car_models=car_models,
            finish_models=finish_models(images["main_data.bin"]),
            intro_crashes=[dict(selector=c["selector"],sequence=c["sequence"],
                                rows=len(c["rows"]),source_bytes=c["source_bytes"],
                                sha256=hashlib.sha256(b"".join(struct.pack("<4I",*r) for r in c["rows"])).hexdigest()) for c in crashes],
            race_grid=race_grid(images["program.bin"], images["main_data.bin"], roads),
            roads=[dict(slot=r["slot"], logical_count=r["logical_count"], profiles=len(r["profiles"]),
                        map_entries=len(r["mapping"]), source_bytes=r["profile_bytes"]+r["mapping_bytes"])
                   for r in roads],
            model_matches=[match_models(inventory, source) for source in args.models],
            intro_model_matches=[match_car_models(car_models,source) for source in args.models])
        if args.output:
            with args.output.open("x") as out:
                json.dump(dict(**report, courses=inventory, road_profiles=roads,
                               intro_camera_source=camera,intro_crash_sources=crashes), out, indent=2)
                out.write("\n")
        print(json.dumps(report, indent=2))
