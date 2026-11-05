"""Generate a NEW local C header of authored course0 intro driving data.

No recorded poses, ROM pointers or runtime conversion. Includes camera keys;
mesh/material assets are separate, so this is not a complete cinematic scene.
"""
import argparse
import hashlib
import math
from pathlib import Path
import re
import struct

from inspect_course import IMAGES, MAIN_BASE, intro_stages, intro_camera, intro_car_models, intro_crashes, words


def literal(value):
    value = struct.unpack("<f", struct.pack("<f", value))[0]
    if not math.isfinite(value):
        raise ValueError("nonfinite intro parameter")
    return value.hex() + "f"


def stage_arrays(graph, speed, profiles, last_profile=False, noops=(0x2265c0,)):
    """noops: the event callbacks that are a bare ret (2265c0 of course 0's graphs; the attract graphs of courses
    1 and 2 name 2265c8 and 2265c4, which are the same one-word ret)."""
    stages = graph["stages"]
    indices = {s["source"]: i for i, s in enumerate(stages)}
    if not 1 <= len(stages) <= 64 or len(indices) != len(stages):
        raise ValueError("invalid intro stage graph")
    result = []
    kinds = {0xcf74: 0, 0xcfc0: 1, 0xd05c: 2}
    for stage in stages:
        spawn, events = [], []
        if not 1 <= len(stage["spawn"]) <= 44 or not 1 <= len(stage["events"]) <= 256:
            raise ValueError("invalid intro stage counts")
        if sum(row[3]==0xd05c for row in stage['spawn'])>1:
            raise ValueError('intro instance budget supports one special actor')
        #226378 starts at player501220, then advances by the entity stride.
        # Native cars[0] is that player, not RAM[501224] (first opponent).
        if stage['spawn'][0][3]!=0xcf74 or any(row[3]==0xcf74 for row in stage['spawn'][1:]):
            raise ValueError('intro player must be the unique first spawn')
        for route, index, packed, updater in stage["spawn"]:
            profile, lateral = packed & 65535, struct.unpack("<h", struct.pack("<H", packed >> 16))[0]
            # cffc adds one without wrapping: the last profile is only usable
            # when the caller has exported what the source reads beyond it.
            if route >= 8 or profile >= profiles - (0 if last_profile else 1) or updater not in kinds:
                raise ValueError("unsupported intro spawn")
            spawn.append(f"{{{route},{literal(speed(index))},{profile},{lateral},{kinds[updater]}}}")
        # 2264f4 (and nm_intro_events_step) fire the events in list order while their time is below the scene
        # tick, so a list need not be sorted: course 2's podium stage 2 lists ten at 160, then ten at 128, which
        # fire together on tick 161.
        for i, (tick, index, packed, callback) in enumerate(stage["events"]):
            actor, lateral = struct.unpack("<2h", struct.pack("<I", packed))
            if actor < 0:
                if callback not in indices or i != len(stage["events"]) - 1:
                    raise ValueError("unresolved intro stage transition")
                events.append(f"{{{tick},0.0f,-1,0,{indices[callback]}}}")
            else:
                if actor >= len(spawn) or (callback and callback not in noops):
                    raise ValueError("unsupported intro actor/callback")
                events.append(f"{{{tick},{literal(speed(index))},{actor},{lateral},UINT32_MAX}}")
        if not (stage["events"][-1][2] & 32768):
            raise ValueError("intro driving stage lacks transition")
        result.append((spawn, events))
    return result


def camera_arrays(main, cursor, cars, run_on=False, linked=False, span=None):
    """run_on: the track runs on into the next one (the pit tracks of views 0 and 2 run into those of views 1
    and 3): its keys end where a key's time is not above the last, which the pit never reaches (released
    before the last key's time; 5cd0 leaves a key only once the next time is not above the tick).
    linked: the sequence may end in a link to another sequence (the standing-start cameras 6474[1], 6474[2]
    link to 2864030 after their last key at 6336 ticks, which the start never reaches: c594 calls 62b4 at
    5010a8 == 192, 128 ticks in). The keys up to the link are exported; the linked sequence is not.
    span: a stage of SPAN ticks whose sequence links on into others and loops back into one of them, not to its
    own start (Expert's podium stages 2865898, 2865930, 28659c8 run on into 2865708, which loops on itself). 5cac
    follows a link to the next record, a time-0 key there starts a new shot as an internal zero-time key does; so
    the chain is exported without its links, its loop unrolled until the shots cover more than SPAN ticks: the
    stage's last event (its transition) comes before the native wrap to key 0 (nm_intro_camera_step)."""
    data = intro_camera(main, cursor + 48)
    records = data["records"]
    if span is not None and data["loop"] != cursor + 48:
        keys = [r for r in records if r["words"][0] != 0xffffffff]
        loop = [i for i, r in enumerate(keys) if r["address"] == data["loop"]]
        if len(loop) != 1 or any(len(r["words"]) != 12 for r in keys):
            raise ValueError("unsupported camera topology")
        def covered(rows):
            total, shot = 0, 0
            for r in rows:
                if not r["words"][0]:
                    total += shot
                shot = r["words"][0]
            return total + shot
        tail = keys[loop[0]:]
        if not covered(tail):
            raise ValueError("unsupported camera topology")
        while covered(keys) <= span + 1:
            keys = keys + tail
        records = keys + [records[-1]]
    if run_on:
        for i in range(1, len(records)):
            if records[i]["words"][0] != 0xffffffff and records[i]["words"][0] <= records[i - 1]["words"][0]:
                records = records[:i] + [records[-1]]
                break
    if linked:
        ends = [i for i, r in enumerate(records) if r["words"][0] == 0xffffffff]
        records = records[:ends[0] + 1] if ends else records
    elif not run_on and span is None and data["loop"] != cursor + 48:
        raise ValueError("unsupported camera topology")
    if len(records)<3 or records[-1]["words"][0]!=0xffffffff:
        raise ValueError("unsupported camera topology")
    keys, times = [], []
    for record in records[:-1]:
        row = record["words"]
        # 5cd0 leaves a key as soon as the next time is not above the tick, so a
        # callback-free key that does not advance its shot never bounds an interval.
        if times and row[0] and row[0]<=times[-1] and len(row)==12 and not row[11] and row[0]<=0x7fffffff:
            continue
        if len(row)!=12 or row[11] or row[0]>0x7fffffff:
            raise ValueError("unsupported camera event/time")
        anchors = [struct.unpack("<i", struct.pack("<I", row[i]))[0] for i in (1,5)]
        if any(a < -1 or a >= cars for a in anchors):
            raise ValueError("camera anchor outside stage")
        values = [literal(struct.unpack("<f",struct.pack("<I",row[i]))[0]) for i in (2,3,4,6,7,8,9,10)]
        keys.append('{' + f'{anchors[0]},{anchors[1]},' + '{'+','.join(values[:3])+'},{'+','.join(values[3:6])+'},'+','.join(values[6:])+'}')
        times.append(row[0])
    if times[0] or not times[-1] or any(b and b<=a for a,b in zip(times,times[1:])):
        raise ValueError("invalid camera shot intervals")
    return keys, times


def model_arrays(catalog):
    """Resolve ROM list pointers once, to the sorted --intro-cars DNM bindings."""
    lists = {item['pointer']: item['objects'] for item in catalog['lists']}
    if len(lists)!=len(catalog['lists']) or any(len(v)!=1 for v in lists.values()):
        raise ValueError('intro part requires one unambiguous mesh per list')
    addresses = sorted({v[0][0] for v in lists.values()})
    canonical={0x2890564:(0x1c8,0x805168,0x80009c,11),0x2890578:(0x241,0x8051c8,0x8000b8,9)}
    if any(not 0x800000<=row[0][0]<0xc00000 and tuple(row[0])!=canonical.get(pointer)
           for pointer,row in lists.items()):
        raise ValueError('unverified intro RAM model identity')
    if not addresses:
        raise ValueError('invalid intro model identity')
    def binding(pointer):
        if pointer not in lists:
            raise ValueError('missing intro model list')
        return addresses.index(lists[pointer][0][0])
    bodies, special = [], []
    for car in catalog['cars']:
        if car['source_updater']==0xd05c:
            if special or len(car['lists'])!=1:
                raise ValueError('unsupported special intro model')
            special = [binding(car['lists'][0])]
        else:
            if car['id']!=len(bodies) or len(car['lists'])!=8:
                raise ValueError('unordered intro body choices')
            bodies.append([binding(p) for p in car['lists']])
    if not bodies or len(special)!=1 or len(catalog['wheels'])!=4 or len(catalog['selectors'])!=4:
        raise ValueError('incomplete intro model bindings')
    return addresses, bodies, special[0], [binding(p) for p in catalog['wheels']], [binding(p) for p in catalog['selectors']]


def crash_arrays(sequences):
    """Drop only the unused first word; preserve vertical-offset float bits."""
    arrays, choices = [], []
    for sequence in sequences:
        rows=[]
        for raw in sequence['rows']:
            model,marker,x,y,z=struct.unpack('<IH3h',struct.pack('<4I',*raw)[4:])
            rows.append(f'{{{model},{marker},{{{x},{y},{z}}}}}')
        if not 1<=len(rows)<=2048:
            raise ValueError('invalid crash row count')
        if rows not in arrays:
            arrays.append(rows)
        choices.append(arrays.index(rows))
    return arrays,choices


def stage_tables(out, prefix, arrays, cameras):
    for i, (spawn, events) in enumerate(arrays):
        for kind, values, typename in (("spawn", spawn, "nm_intro_spawn_t"), ("events", events, "nm_intro_event_t")):
            out.append(f'static const alignas(32) {typename} {prefix}_{kind}_{i}[] = {{')
            out.append(',\n'.join(values) + '};')
        keys,times=cameras[i]
        out.append(f'static const alignas(32) nm_intro_key_t {prefix}_keys_{i}[] = {{')
        out.append(',\n'.join(keys)+'};')
        out.append(f'static const alignas(32) uint32_t {prefix}_times_{i}[] = {{'+','.join(map(str,times))+'};')
    out.append(f'static const alignas(32) nm_intro_stage_data_t {prefix}_stages[] = {{')
    out.append(',\n'.join(f'{{{prefix}_spawn_{i},{prefix}_events_{i},{prefix}_keys_{i},{prefix}_times_{i},{len(s)},{len(e)},{len(cameras[i][0])},0}}'
                         for i, (s, e) in enumerate(arrays)) + '};')


def source_numbers(program, main, slot=0):
    def number(at):
        return struct.unpack("<f", struct.pack("<I", words(main, MAIN_BASE, at, 1)[0]))[0]
    def speed(index):
        if not 0 <= index < 512:
            raise ValueError("intro speed index outside table")
        return number(0x28b7520 + 4 * index) * number(0x28b573c)
    descriptor = words(program, 0, 0x3a48 + 4 * slot, 1)[0]   # 39c0: the course descriptor 3a48[501460]
    profile_table = words(main, MAIN_BASE, descriptor + 20, 1)[0]
    return number, speed, profile_table, words(main, MAIN_BASE, profile_table, 1)[0] + 1


def prepare_podium(program, main, slots=(0,)):
    """Phase27/28 graph of every course slot of SLOTS (2265a4 -> 2865a70[501460]), same layout as the intro:
    course 0's under the names nm_podium_*, another slot's under nm_podium_cN_*, and nm_podium_courses[3], the
    graphs by slot (stages NULL: not in this header). Also the attract graph (226360 -> 2865a60[501460]) of every
    slot but 0 (nm_attract_cN_*; course 0's is the intro header's nm_intro_stages) and nm_attract_courses[3].

    Stage data only: car models, crash rows and speed tables stay in the intro
    header. Cars spawned on the last profile read the next164 ROM bytes for
    one tick; they are exported as a zero profile only after checking them.
    """
    slots = sorted(set(slots))
    if 0 not in slots or any(slot not in (0, 1, 2) for slot in slots):
        raise ValueError("course slots must be 0, 1 or 2 and include 0")
    out = ['/* Local ROM-derived data: do not distribute. Podium driving and camera data. */',
           '#ifndef DAYTONA_PODIUM_DATA_H', '#define DAYTONA_PODIUM_DATA_H',
           '#include "intro_drive_data.h"', '#include "road.h"']
    entries, attract = {}, {0: '{nm_intro_stages,sizeof(nm_intro_stages)/sizeof(nm_intro_stages[0]),0}'}
    for slot in slots:
        _, speed, profile_table, profiles = source_numbers(program, main, slot)
        if slot:
            # 226360: the attract's graph 2865a60[501460] (course 0's is the intro header's nm_intro_stages). Its
            # event callbacks are checked to be a bare ret (i960 0a000000) in the program ROM.
            graph = intro_stages(main, words(main, MAIN_BASE, 0x2865a60 + 4 * slot, 1)[0])
            callbacks = {e[3] for s in graph["stages"] for e in s["events"] if e[3] and not e[2] & 32768}
            noops = tuple(c for c in callbacks if words(program, 0, c - 0x200000, 1)[0] == 0x0a000000)
            arrays = stage_arrays(graph, speed, profiles, noops=noops)
            cameras = [camera_arrays(main, s["camera"], len(s["spawn"])) for s in graph["stages"]]
            prefix = f'nm_attract_c{slot}'
            out.append(f'/* Course slot {slot}: its attract graph 2865a60[{slot}]. */')
            stage_tables(out, prefix, arrays, cameras)
            attract[slot] = f'{{{prefix}_stages,{len(arrays)},0}}'
        graph = intro_stages(main, words(main, MAIN_BASE, 0x2865a70 + 4 * slot, 1)[0])
        overrun = profile_table + 8 + 164 * profiles - MAIN_BASE
        if overrun + 164 > len(main) or any(main[overrun:overrun + 164]):
            raise ValueError("podium spawn overrun profile is not zero")
        arrays = stage_arrays(graph, speed, profiles, True)
        cameras = [camera_arrays(main,s["camera"],len(s["spawn"]),span=s["events"][-1][0]) for s in graph["stages"]]
        prefix = 'nm_podium' if not slot else f'nm_podium_c{slot}'
        if slot:
            out.append(f'/* Course slot {slot}: its podium graph 2865a70[{slot}]. */')
        stage_tables(out, prefix, arrays, cameras)
        out += ['/* Source bytes after the last authored profile, verified zero at export. */',
                f'static const alignas(32) nm_road_profile_t {prefix}_overrun_profile = {{0}};']
        entries[slot] = f'{{{prefix}_stages,{len(arrays)},&{prefix}_overrun_profile}}'
    out += ['/* The podium graphs by course slot 501460; stages NULL: not in this header. */',
            'typedef struct { const nm_intro_stage_data_t *stages; unsigned count; const nm_road_profile_t *overrun; } nm_podium_course_t;',
            f'#define NM_PODIUM_COURSES {sum(1 << slot for slot in slots)}u /* bit N: slot N is in this header */',
            'static const nm_podium_course_t nm_podium_courses[3] = {' + ','.join(entries.get(slot, '{0}') for slot in range(3)) + '};',
            '/* The attract graphs by course slot 501460 (226360: 2865a60[501460]; course 0: the intro header\'s). */',
            f'#define NM_ATTRACT_COURSES {sum(1 << slot for slot in slots)}u /* bit N: slot N is in this header */',
            'static const nm_podium_course_t nm_attract_courses[3] = {' + ','.join(attract.get(slot, '{0}') for slot in range(3)) + '};',
            '#endif', '']
    return '\n'.join(out)


def race_start_block(program, main, slot):
    """The start camera (6474[slot], selected by 5b88, played by the keyed updater 5cac as the intro; authored keys,
    not sampled poses) and the pit tables of course slot SLOT (501460: 0 Beginner, 1 Expert, 2 Advanced)."""
    start = words(program, 0, 0x6474 + 4 * slot, 1)[0]
    count = words(program, 0, 0x3980 + 4 * slot, 1)[0]
    if not MAIN_BASE <= start < MAIN_BASE + len(main) or not 1 <= count <= 44:
        raise ValueError("invalid race-start camera table or car count")
    keys, times = camera_arrays(main, start - 48, count, linked=slot != 0)
    return ['static const alignas(32) nm_intro_key_t nm_race_start_keys[] = {', ',\n'.join(keys) + '};',
            'static const alignas(32) uint32_t nm_race_start_times[] = {' + ','.join(map(str, times)) + '};',
            f'enum {{ NM_RACE_START_KEYS = {len(keys)} }};'], pit_arrays(program, main, count, slot) + tunnel_lights(program, slot)


def tunnel_lights(program, slot):
    """The scene light's tunnel ranges of course SLOT (include/tunnel_light.h; 64bc..6508: count 232352[course],
    pointers 23235a + course * 232356): enter and exit profiles (count + 1 each), the tunnel's profiles, the
    point-light mask and the light positions."""
    def u16(address): return struct.unpack_from('<H', program, address - 0x200000)[0]
    def u32(address): return struct.unpack_from('<I', program, address - 0x200000)[0]
    def f32(address): return struct.unpack_from('<f', program, address - 0x200000)[0]
    count = program[0x232352 - 0x200000 + slot]
    stride = u32(0x232356)
    enter, exit_, ranges, mask, lights = (u32(0x23235a + slot * stride + 4 * i) for i in range(5))
    if count > 32 or stride != 20:
        raise ValueError("unexpected tunnel light table")
    if not count:
        return ['/* 232352[course] = 0: no tunnel light ranges (include/tunnel_light.h). */',
                'static const nm_tunnel_lights_t nm_race_tunnel = {0,0,0,0,0,0,0,nm_race_light_rates,nm_race_light_normal};']
    profiles = lambda base: [u16(base + 2 * i) for i in range(count + 1)]
    points = [[f32(lights + 12 * i + 4 * k) for k in range(3)] for i in range(count)]
    return ['/* 23235a[course]: the scene light\'s tunnel ranges (include/tunnel_light.h). */',
            'static const uint16_t nm_race_tunnel_enter[] = {' + ','.join(map(str, profiles(enter))) + '};',
            'static const uint16_t nm_race_tunnel_exit[] = {' + ','.join(map(str, profiles(exit_))) + '};',
            'static const float nm_race_tunnel_lights[][3] = {' + ','.join('{' + ','.join(map(literal, p)) + '}' for p in points) + '};',
            f'static const nm_tunnel_lights_t nm_race_tunnel = {{nm_race_tunnel_enter,nm_race_tunnel_exit,{u16(ranges)},'
            f'{u16(ranges + 2)},0x{u32(mask):x}u,nm_race_tunnel_lights,{count},nm_race_light_rates,nm_race_light_normal}};']


def light_tables(program):
    """The normal-race light 235db8 (+c..+14, as lighting.h) and the scene light's glide steps 2323aa (by speed
    index >> 6), shared by every course."""
    f32 = lambda address: struct.unpack_from('<f', program, address - 0x200000)[0]
    normal = [f32(0x235db8 + 0xc + 4 * k) for k in range(3)]
    if [round(v, 6) for v in normal] != [-0.45, -0.89, -0.45]:
        raise ValueError("unexpected normal-race light")
    rates = [f32(0x2323aa + 4 * i) for i in range(8)]
    if not all(r < 0 for r in rates):
        raise ValueError("unexpected scene light steps")
    return ['/* 235db8 + c: the normal-race light; 2323aa: the scene light\'s step by speed index >> 6 (tunnel_light.h). */',
            'static const float nm_race_light_normal[3] = {' + ','.join(map(literal, normal)) + '};',
            'static const float nm_race_light_rates[8] = {' + ','.join(map(literal, rates)) + '};']


def suffixed(lines, suffix):
    """The block of another course: every nm_race_* / NM_RACE_* name it defines or uses gets SUFFIX (types,
    *_t, and the shared crash keys are not renamed)."""
    def rename(match):
        name = match.group(0)
        if name.endswith('_t') or name.startswith(('nm_race_crash', 'NM_RACE_CRASH', 'nm_race_light_')):
            return name
        return name + (suffix.upper() if name.isupper() else suffix)
    return [re.sub(r'\b(?:nm_race|NM_RACE)_[A-Za-z0-9_]+', rename, line) for line in lines]


def prepare_race_start(program, main, slots=(0,)):
    """Race-start header for the course slots SLOTS (0 must be one of them): course 0's tables under the names
    they always had, every other slot's with the suffix _cN, the shared crash camera and view presets once, and
    nm_race_course_starts[3], the tables by slot (NULL where a slot is not in the header)."""
    slots = sorted(set(slots))
    if 0 not in slots or any(slot not in (0, 1, 2) for slot in slots):
        raise ValueError("course slots must be 0, 1 or 2 and include 0")
    # Crash camera 5c14: car-relative table named by the lda immediate at 5c20.
    if words(program, 0, 0x5c20, 1)[0] != 0x8c183000:
        raise ValueError("unexpected crash camera selector")
    crash_keys, crash_times = camera_arrays(main, words(program, 0, 0x5c24, 1)[0] - 48, 1)
    if any(not key.startswith('{-1,-1,') for key in crash_keys):
        raise ValueError("crash camera keys are expected to be car-relative")
    names = ', '.join(str(slot) for slot in slots)
    out = [f'/* Local ROM-derived data: do not distribute. Rolling/standing-start, crash and pit camera keys, pit stop '
           f'tables of the course slots {names}. */',
           '#ifndef DAYTONA_RACE_START_DATA_H', '#define DAYTONA_RACE_START_DATA_H',
           '#include "intro_math.h"', '#include "pit.h"'] + light_tables(program)
    start, pit = race_start_block(program, main, 0)
    out += start + [
           'static const alignas(32) nm_intro_key_t nm_race_crash_keys[] = {', ',\n'.join(crash_keys) + '};',
           'static const alignas(32) uint32_t nm_race_crash_times[] = {' + ','.join(map(str, crash_times)) + '};',
           f'enum {{ NM_RACE_CRASH_KEYS = {len(crash_keys)} }};'] + pit + view_presets(program) + prop_arrays(program, main, slots)  # every course's props
    for slot in slots[1:]:
        start, pit = race_start_block(program, main, slot)
        out += [f'/* Course slot {slot}: the same tables with the suffix _c{slot}. */'] + suffixed(start + pit, f'_c{slot}')
    entries = []
    for slot in range(3):
        suffix = f'_c{slot}' if slot else ''
        entries.append(f'{{nm_race_keyed_tracks{suffix},nm_race_pit_view_tracks{suffix},&nm_race_pit{suffix},'
                       f'&nm_race_tunnel{suffix}}}' if slot in slots else '{0}')
    mask = sum(1 << slot for slot in slots)
    out += ['/* The tables by course slot 501460 (pit.h nm_race_course_start_t); {0}: not in this header. */',
            f'#define NM_RACE_START_COURSES {mask}u /* bit N: slot N is in this header */',
            'static const nm_race_course_start_t nm_race_course_starts[3] = {' + ',\n'.join(entries) + '};',
            '#endif', '']
    return '\n'.join(out)


PROP_KINDS = (0x223114, 0x223078, 0x2230ac, 0x2230e0)   # the init entries of the four prop kinds (include/props.h)


def prop_arrays(program, main, slots=(0,)):
    """The props of the course slots SLOTS (include/props.h, spec-cones.md): per course the prop actors (handlers
    PROP_KINDS, actor table of prepare_world_objects; the collision manager 222d30 first, its argument the footprint,
    the sound manager 22302c last), and per kind what its init entry stores (the lda immediates of 223114, 223078,
    2230ac, 2230e0: knock sets, rest record, hit limit, sound). One record pool for every course: records are stored
    once, tables by their first record. Course 0's data keeps its name nm_race_props; another slot's is
    nm_race_props_cN; nm_race_prop_courses[3] holds them by slot (NULL: no props in this header)."""
    import prepare_world_objects as pwo

    def u32(at):
        return words(main, MAIN_BASE, at, 1)[0] if at >= MAIN_BASE else words(program, 0, at & 0x1fffff, 1)[0]

    def s16(v):
        v &= 0xffff
        return v-65536 if v & 0x8000 else v

    if u32(0x2896d80) != 0x8c:
        raise ValueError("prop version word 2896d80 is not 0x8c")
    # Each init entry: lda knock,r3 / st r3,+64; lda rest,r3 / st r3,+5c; lda limit,r4 / st r4,+68; lda sound,r3 /
    # st r3,+70; then b 223144 (223114 falls through to it): the immediates at +4, +10, +1c, +28.
    kinds = []
    for entry in PROP_KINDS:
        if any(u32(entry+off) != op for off, op in ((0, 0x8c183000), (8, 0x921f6064), (12, 0x8c183000), (20, 0x921f605c),
                                                    (24, 0x8c203000), (32, 0x92276068), (36, 0x8c183000), (44, 0x921f6070))):
            raise ValueError(f"prop init entry {entry:x} changed")
        kinds.append(dict(knock=u32(entry+4), rest=u32(entry+0x10), limit=u32(entry+0x1c), sound=u32(entry+0x28)))
    if (kinds[0]['knock'], kinds[0]['rest'], kinds[0]['limit'], kinds[0]['sound']) != (0x2896dc4, 0x2896d84, 0x3f99999a, 0x9e1d7f):
        raise ValueError("cone init entry 223114 changed")

    class R:
        def __init__(self):
            self.program, self.data = program, main

        def u32(self, at):
            return u32(at)

        def f32(self, at):
            return struct.unpack('<f', struct.pack('<I', u32(at)))[0]
    records = []

    def record(at):
        w8, wc = u32(at+8), u32(at+12)
        return (u32(at), u32(at+4), s16(w8), s16(w8 >> 16), s16(wc), s16(wc >> 16))
    tables, rests = {}, {}

    def table_index(table):
        if table not in tables:
            count = u32(table)
            if not 2 <= count <= 1024:
                raise ValueError("prop motion table length")
            tables[table] = (len(records), count, len(tables), table)
            records.extend(record(table+4+16*i) for i in range(count))
        return tables[table][2]

    def kind_entry(k):
        """The kind's rest record and knock sets in the pool (made on first use)."""
        if k not in rests:
            kind = kinds[k]
            rests[k] = len(records)
            records.append(record(kind['rest']))
            sets = []
            for f in range(2):
                at = u32(kind['knock']+4*f)
                n = u32(at)
                if not 1 <= n <= 4:
                    raise ValueError("prop knock set size")
                sets.append([table_index(u32(at+4+4*j)) for j in range(n)])
            kind['sets'] = sets
        return rests[k]
    courses = {}
    for slot in sorted(set(slots)):
        actors = [a for a in pwo.actors(R(), slot) if a['handler'] in PROP_KINDS + (0x222d30, 0x22302c)]
        if not actors:
            continue
        # 2002c makes one task per actor of a cell list in order: the manager, the props (50f000 + 80 (id-1)), the
        # sound manager; the prop ids are 1..n in that order, the manager's id is n.
        props = actors[1:-1]
        if (actors[0]['handler'] != 0x222d30 or actors[-1]['handler'] != 0x22302c or
                any(a['handler'] not in PROP_KINDS for a in props) or len({a['list_cell'] for a in actors}) != 1 or
                [a['id'] for a in props] != list(range(1, len(props)+1)) or actors[0]['id'] != len(props) or
                len(props) > 35 or any(a['y'] for a in props)):
            raise ValueError(f"course {slot}: prop actors are not manager, props 1..n, sound manager")
        if slot == 0 and (len(props) != 10 or any(a['argument'] != 0x288ead4 or a['heading'] for a in props)):
            raise ValueError("course 0 cone actors changed")
        for a in props:
            kind_entry(PROP_KINDS.index(a['handler']))
        courses[slot] = (actors[0]['argument'], props)
    if 0 not in courses:
        raise ValueError("course 0 has no props")
    for k in range(len(kinds)):
        kind_entry(k)   # every kind in the pool, so each course's table names a rest record
    order = sorted(tables.values(), key=lambda t: t[2])

    def bits(v):
        return f'{v:#010x}u'

    def flt(v):   # the word's float exactly, as a hexadecimal literal
        f = struct.unpack('<f', struct.pack('<I', v))[0]
        return ('-' if math.copysign(1.0, f) < 0 else '') + float.hex(abs(f)) + 'f'
    kind_rows = ','.join('{%d,{%d,%d},{%s,%s},%s,%s,%s}' % (
        rests[k], len(kinds[k]['sets'][0]), len(kinds[k]['sets'][1]),
        '{'+','.join(map(str, kinds[k]['sets'][0]))+'}', '{'+','.join(map(str, kinds[k]['sets'][1]))+'}',
        bits(kinds[k]['limit']), bits(kinds[k]['sound']), bits(kinds[k]['rest'])) for k in range(len(kinds)))
    out = ['/* 222d30, the prop kinds 223114/223078/2230ac/2230e0, 22302c: the props of every course in this header',
           ' * (include/props.h): rest records, motion tables, knock sets, footprints, actors. */',
           '#define NM_PROP_DATA 1', '#include "props.h"',
           'static const alignas(32) nm_prop_record_t nm_race_prop_records[] = {',
           ',\n'.join('{%s,%s,%d,%d,%d,%d}' % (bits(r[0]), bits(r[1]), *r[2:]) for r in records) + '};',
           'static const nm_prop_table_t nm_race_prop_tables[] = {' +
           ','.join('{%d,%d,%s}' % (t[0], t[1], bits(t[3])) for t in order) + '};']
    for slot, (corners, props) in sorted(courses.items()):
        name = 'nm_race_props' + (f'_c{slot}' if slot else '')
        out += [f'static const nm_prop_data_t {name} = {{nm_race_prop_records,nm_race_prop_tables,{len(records)},{len(order)},',
                '    {' + kind_rows + '},',
                '    {' + ','.join('{%s}' % ','.join(flt(u32(corners+12*k+4*j)) for j in range(3)) for k in range(4)) + '},',
                f'    {len(props)},{{',
                ',\n'.join('    {%s,%s,%s,%d,%d,%d}' % (
                    flt(u32(a['record'])), flt(u32(a['record']+4)), flt(u32(a['record']+8)), a['heading'], a['id'],
                    PROP_KINDS.index(a['handler'])) for a in props) + '}};']
    out += [f'#define NM_PROP_COURSES {sum(1 << slot for slot in courses)}u /* bit N: slot N has props in this header */',
            'static const nm_prop_data_t *const nm_race_prop_courses[3] = {' +
            ','.join(('&nm_race_props' + (f'_c{slot}' if slot else '')) if slot in courses else '0' for slot in range(3)) + '};']
    return out


def view_presets(program):
    """The race's view presets 0..3 (VR1..VR4): 5248..5300 copies program 2320b6 (16 entries of 20 bytes) to the
    backup RAM table 1d00220 that 62b4/8814 read by 501710. Words +0/+4/+8/+c (eye height, eye forward, window
    centre offset, focus), halfword +10 (pitch), bytes +12 (adjustment flag) and +13 (camera mode)."""
    if list(words(program, 0, 0x52a8, 4)) != [0x8c803000, 0x01d00220, 0x8c883000, 0x002320b6]:
        raise ValueError("unexpected view preset table copy")
    base = 0x20000 + (0x2320b6 & 0x1ffff)
    rows = []
    for view in range(4):
        entry = program[base + 20 * view:base + 20 * view + 20]
        values = struct.unpack("<4Ih2B", entry)
        floats = [struct.unpack("<f", struct.pack("<I", v))[0] for v in values[:4]]
        if not all(math.isfinite(f) for f in floats) or values[5] or values[6] not in (0, 2):
            raise ValueError("unsupported view preset")
        rows.append('{{' + ','.join(f'{v:#010x}u' for v in values[:4]) + f'}},{values[4]},{values[5]},{values[6]}}}')
    return ['/* 1d00220 (copy of 2320b6): the views VR1..VR4 (include/race_view.h). */',
            '#define NM_RACE_VIEW_DATA 1',
            'static const nm_race_view_preset_t nm_race_view_presets[4] = {' + ',\n'.join(rows) + '};']


def pit_arrays(program, main, cars, slot=0):
    """Course0 pit stop (spec-pitstop.md): the waypoints 234bf8[0] read by e0a8 (seven and the row after them,
    which the abort path's fall-through reads), the clean bodies 234bf0[501464] of the repair dedc, the six crew
    records 239848[0] of 1abfc with their scripts (32-byte entries up to the 999999 terminator), the sound cues
    23ad4c of 1ad74, and the keyed camera tracks 234a9c[2] (5af8, entry) and 234aac[2] (5b20, stop) of view
    preset 2. Motion and model-set words stay identities for the presentation's binding."""
    def program_words(address, count):
        return words(program, 0, address - 0x200000, count)
    def number(bits):
        return struct.unpack("<f", struct.pack("<I", bits))[0]
    waypoints = program_words(program_words(0x234bf8 + 4 * slot, 1)[0], 32)
    rows = []
    for i in range(8):
        x, z, speed, reserved = waypoints[4 * i:4 * i + 4]
        # The row after the seven is whatever follows the table: course 0's runs into course 2's table, course 1's
        # (234ce8, the last) into list pointers (reserved 288d684). e0a8 reads only its x, z and km/h words (the
        # abort path's fall-through; nm_pit_waypoint_step), exported as their bits; the fourth word as it is.
        if (i < 7 and (reserved or not (number(speed) >= 0))):
            raise ValueError("unexpected pit waypoint row")
        rows.append('{' + ','.join(literal(number(v)) for v in (x, z, speed)) + f',{reserved:#x}u}}' if reserved else
                    '{' + ','.join(literal(number(v)) for v in (x, z, speed)) + ',0}')
    if not slot and [number(waypoints[4 * i + 2]) for i in range(7)] != [150.0, 100.0, 60.0, 60.0, 20.0, 0.0, 0.0]:
        raise ValueError("pit waypoint speeds moved (spec-pitstop.md section 2)")
    bodies = program_words(0x234bf0, 2)
    if list(bodies) != list(words(main, MAIN_BASE, 0x286fab0, 2)):
        raise ValueError("pit clean bodies differ from the grid reset's bodies")
    base = program_words(0x239848 + 4 * slot, 1)[0]
    records, scripts = [], []
    for crew in range(6):
        record = program_words(base + 32 * crew, 8)
        motion, models, script = record[:3]
        headings = struct.unpack("<2h", struct.pack("<I", record[6]))
        start, previous = len(scripts), -1
        for entry in range(32):
            frame, action, *arguments = program_words(script + 32 * entry, 5)
            if frame < previous or action > 11:
                raise ValueError("unsupported pit crew script")
            previous = frame
            scripts.append(f'{{{frame},{action},{{{",".join(str(v) for v in arguments)}}}}}')
            if frame == 999999:
                break
        else:
            raise ValueError("pit crew script without terminator")
        records.append(f'{{{motion},{models},{{{",".join(literal(number(v)) for v in record[3:6])}}},'
                       f'{headings[0]},{headings[1]},{record[7]},{start},{len(scripts) - start}}}')
    cues = [program_words(0x23ad4c + 8 * i, 2) for i in range(8)]
    if [t for t, _ in cues] != [71, 77, 161, 174, 274, 445, 451, 99999]:
        raise ValueError("pit crew sound cues moved")
    if len(scripts) > 128:
        raise ValueError("pit crew scripts exceed NM_PIT_SCRIPT_ENTRIES")
    out = crew_art(program, main, base)
    out += ['/* 234bf8/234bf0/239848/23ad4c: the pit stop (include/pit.h). */',
           'static const alignas(32) nm_pit_data_t nm_race_pit = {{' + ',\n'.join(rows) + '},',
           '{' + ','.join(str(v) for v in bodies) + '},{' + ',\n'.join(records) + '},',
           '{' + ','.join(f'{{{t},{c}}}' for t, c in cues) + f'}},{len(scripts)},{{' + ',\n'.join(scripts) + '},',
           '&nm_race_pit_crew};']
    for name, table in (('entry', 0x234a9c), ('stop', 0x234aac)):
        keys, times = camera_arrays(main, program_words(table + 8, 1)[0] - 48, cars)
        out += [f'static const alignas(32) nm_intro_key_t nm_race_pit_{name}_keys[] = {{' + ',\n'.join(keys) + '};',
                f'static const alignas(32) uint32_t nm_race_pit_{name}_times[] = {{' + ','.join(map(str, times)) + '};',
                f'enum {{ NM_RACE_PIT_{name.upper()}_KEYS = {len(keys)} }};']
    # Every view's tracks (5af8/5b20 index the tables by the view 501710 when the camera is armed).
    view_tracks = []
    for name, table in (('entry', 0x234a9c), ('stop', 0x234aac)):
        row = []
        for view in range(4):
            if view == 2:
                row.append(f'{{nm_race_pit_{name}_keys,nm_race_pit_{name}_times,NM_RACE_PIT_{name.upper()}_KEYS}}')
                continue
            start = program_words(table + 4 * view, 1)[0]
            keys, times = camera_arrays(main, start - 48, cars, run_on=intro_camera(main, start)["loop"] != start)
            out += [f'static const alignas(32) nm_intro_key_t nm_race_pit_{name}{view}_keys[] = {{' + ',\n'.join(keys) + '};',
                    f'static const alignas(32) uint32_t nm_race_pit_{name}{view}_times[] = {{' + ','.join(map(str, times)) + '};']
            row.append(f'{{nm_race_pit_{name}{view}_keys,nm_race_pit_{name}{view}_times,{len(keys)}}}')
        view_tracks.append('{' + ','.join(row) + '}')
    out += ['/* The pit entry and stop tracks by view 501710 (234a9c[view], 234aac[view]; race_camera.h). */',
            'static const nm_race_keyed_track_t nm_race_pit_view_tracks[2][4] = {' + ',\n'.join(view_tracks) + '};']
    out += ['/* The keyed tracks by nm_pit_t.camera: the start (6474), the pit entry (5af8) and stop (5b20). */',
            'static const nm_race_keyed_track_t nm_race_keyed_tracks[3] = {'
            '{nm_race_start_keys,nm_race_start_times,NM_RACE_START_KEYS},'
            '{nm_race_pit_entry_keys,nm_race_pit_entry_times,NM_RACE_PIT_ENTRY_KEYS},'
            '{nm_race_pit_stop_keys,nm_race_pit_stop_times,NM_RACE_PIT_STOP_KEYS}};']
    return out


def crew_art(program, main, base):
    """The pit crew's presentation (include/pit_crew.h, spec-pitstop.md section 3): every motion the records and
    scripts name (action 4) as its 76-byte keyframes, (count >> 1) + 2 of them: the draw takes the keyframe of the
    frame before the script and its successor when the frame after it is odd, up to the motion's count; every
    model set the records and scripts name (action 5) with its manual twin 64 bytes further (1ad3c), each of the
    16 lists one model; the hierarchy offsets 23ac98 (g7 of 1ad30); the held items of 1b33c (translations and
    turns are code immediates, the lists 288b730 and 288ddc8). Model words are polygon-ROM addresses: the car
    pack's catalogue (tools/append_player_parts.py --crew)."""
    def program_words(address, count):
        return words(program, 0, address - 0x200000, count)
    def code(address, count):
        return words(program, 0, address, count)
    def number(bits):
        return struct.unpack("<f", struct.pack("<I", bits))[0]
    def single(entry):
        record = words(main, MAIN_BASE, entry, 5)
        if not 0x800000 <= record[0] < 0xc00000 or record[4]:
            raise ValueError("pit crew object list is not one model")
        return record[0]
    motions, sets = {}, []
    for crew in range(6):
        record = program_words(base + 32 * crew, 8)
        motions[record[0]] = max(motions.get(record[0], 0), record[7])
        if record[1] not in sets:
            sets.append(record[1])
        entry = record[2]
        while True:
            frame, action, *arguments = program_words(entry, 5)
            entry += 32
            if action == 4:
                if arguments[2] > arguments[1]:
                    raise ValueError("pit crew motion starts beyond its count")
                motions[arguments[0]] = max(motions.get(arguments[0], 0), arguments[1])
            elif action == 5 and arguments[0] not in sets:
                sets.append(arguments[0])
            if frame == 999999:
                break
    if code(0x1ad30, 2) != (0x8cb83000, 0x23ac98) or code(0x1ad3c, 2) != (code(0x1ad3c, 1)[0], 0x501464) or code(0x1ad4c, 1) != (0x59185e06,):
        raise ValueError("pit crew hierarchy table or manual-set offset moved")
    item_code = ((0x1b370, 0x1b3b0, 0x1b3c8), (0x1b3e4, 0x1b424, 0x1b43c), (0x1b458, None, 0x1b498))
    items = []
    for translation, roll, lst in item_code:
        loads = [code(translation + 8 * i, 2) for i in range(3)]
        if [w for w, _ in loads] != [0x8c183000, 0x8c203000, 0x8c283000] or code(lst, 1) != (0x8c803000,):
            raise ValueError("pit crew item draw moved")
        if roll is not None and code(roll, 1) != (0x5930de0e,):
            raise ValueError("pit crew item turn moved")
        items.append(([v for _, v in loads], 0xc000 if roll is not None else 0, single(code(lst + 4, 1)[0])))
    keys, motion_rows = [], []
    for address in sorted(motions):
        count = (motions[address] >> 1) + 2
        motion_rows.append(f'{{{address},{len(keys)},{count}}}')
        for k in range(count):
            raw = words(main, MAIN_BASE, address + 76 * k, 19)
            angles = struct.unpack("<32h", struct.pack("<16I", *raw[3:]))
            keys.append('{{' + ','.join(literal(number(v)) for v in raw[:3]) + '},{' + ','.join(map(str, angles)) + '}}')
    set_rows = []
    for address in sets:
        models = [[single(v) for v in program_words(address + 64 * twin, 16)] for twin in (0, 1)]
        set_rows.append(f'{{{address},{{' + ','.join('{' + ','.join(map(str, m)) + '}' for m in models) + '}}')
    if len(motion_rows) > 8 or len(set_rows) > 12:
        raise ValueError("pit crew motions or model sets exceed include/pit.h")
    offsets = program_words(0x23ac98, 45)
    return ['/* 1ada8/1b33c/1b4c0: the pit crew\'s motions, model sets and hierarchy (include/pit_crew.h). */',
            f'static const alignas(32) nm_pit_crew_key_t nm_race_pit_crew_keys[{len(keys)}] = {{' + ',\n'.join(keys) + '};',
            'static const alignas(32) nm_pit_crew_art_t nm_race_pit_crew = {{' +
            ','.join('{' + ','.join(literal(number(v)) for v in offsets[3 * i:3 * i + 3]) + '}' for i in range(15)) + '},',
            '{' + ','.join('{' + ','.join(literal(number(v)) for v in t) + '}' for t, _, _ in items) + '},{' +
            ','.join(str(struct.unpack("<h", struct.pack("<H", r))[0]) for _, r, _ in items) + '},{' +
            ','.join(str(m) for _, _, m in items) + '},',
            f'{len(motion_rows)},{len(set_rows)},{len(keys)},{{' + ','.join(motion_rows) + '},{' + ',\n'.join(set_rows) +
            '},nm_race_pit_crew_keys};']


def prepare(program, main, race=False):
    number, speed, _, profiles = source_numbers(program, main)
    graph = intro_stages(main, words(main, MAIN_BASE, 0x2865a60, 1)[0])
    arrays = stage_arrays(graph, speed, profiles)
    cameras = [camera_arrays(main,s["camera"],len(s["spawn"])) for s in graph["stages"]]
    catalog=intro_car_models(program,main,graph,race)
    addresses,bodies,special,wheels,selectors = model_arrays(catalog)
    objects={item['pointer']:item['objects'][0][0] for item in catalog['lists']}
    phases=[addresses.index(objects[p]) for p in catalog['phase_wheels']]
    special_phases=[addresses.index(objects[p]) for p in catalog['special_phases']]
    crash_parts=[addresses.index(objects[p]) for p in catalog['crash_parts']]
    windows=[addresses.index(objects[p]) for p in catalog['windows']]
    crashes,crash_choices=crash_arrays(intro_crashes(main))
    out = ['/* Local ROM-derived data: do not distribute. Driving and camera data. */',
           '#ifndef DAYTONA_INTRO_DRIVE_DATA_H', '#define DAYTONA_INTRO_DRIVE_DATA_H',
           '#include "intro_math.h"',
           'typedef struct { alignas(32) const nm_intro_spawn_t *spawn; const nm_intro_event_t *events;',
           '    const nm_intro_key_t *keys; const uint32_t *times;',
           '    uint32_t cars, event_count, key_count, reserved; } nm_intro_stage_data_t;',
           'static const alignas(32) nm_intro_speed_params_t nm_intro_speeds = {']
    out.append(','.join(literal(number(at)) for at in (0x28b57a4, 0x28b5814, 0x28b57b8)) + ',{')
    out.append(','.join(literal(number(0x28b85a0 + 4 * i)) for i in range(512)) + '}};')
    stage_tables(out, 'nm_intro', arrays, cameras)
    out.append('/* DNM identity checks and binding indices, not runtime ROM addresses. */')
    for name,values in (('model_ids',addresses),('wheel_models',wheels),('selector_models',selectors),('wheel_phase_models',phases),('special_phase_models',special_phases),('window_models',windows),('crash_models',crash_parts)):
        out.append(f'static const alignas(32) uint32_t nm_intro_{name}[] = {{'+','.join(map(str,values))+'};')
    out.append(f'static const uint32_t nm_intro_special_model = {special};')
    out.append(f'static const uint32_t nm_intro_shadow_model = {addresses.index(objects[catalog["shadow"]])};')
    out.append('static const alignas(32) uint32_t nm_intro_body_models[][8] = {')
    out.append(',\n'.join('{'+','.join(map(str,row))+'}' for row in bodies)+'};')
    out.append('/* Authored crash animation; legacy model_key holds vertical-offset float bits, not a pointer. */')
    for i,rows in enumerate(crashes):
        out.append(f'static const alignas(32) nm_crash_row_t nm_intro_crash_rows_{i}[] = {{')
        out.append(',\n'.join(rows)+'};')
    out.append('static const struct { const nm_crash_row_t *rows; unsigned count; } nm_intro_crash_sequences[] = {')
    out.append(',\n'.join(f'{{nm_intro_crash_rows_{i},{len(crashes[i])}}}' for i in crash_choices)+'};')
    out += ['#endif', '']
    return '\n'.join(out)


def self_test():
    raw=struct.unpack('<4I',struct.pack('<IIH3h',0xdeadbeef,123,65535,-32768,32767,-1))
    assert crash_arrays([dict(rows=[raw]),dict(rows=[raw])])==([['{123,65535,{-32768,32767,-1}}']],[0,0])
    for count in (0,2049):
        try: crash_arrays([dict(rows=[raw]*count)])
        except ValueError: continue
        raise AssertionError('invalid crash count accepted')
    catalog=dict(lists=[dict(pointer=1,objects=[(0x800100,0,0,0)]),
                        dict(pointer=2,objects=[(0x800000,0,0,0)])],
                 cars=[dict(id=0,source_updater=0xcf74,lists=[1]*8),
                       dict(id=1,source_updater=0xd05c,lists=[2])],
                 wheels=[1,2,1,2],selectors=[2]*4)
    assert model_arrays(catalog)==([0x800000,0x800100],[[1]*8],0,[1,0,1,0],[0]*4)
    race_catalog=dict(catalog,cars=[dict(id=i,source_updater=0xcde0,lists=[1]*8)
                                  for i in range(40)]+[catalog['cars'][1]])
    assert model_arrays(race_catalog)[1]==[[1]*8 for _ in range(40)]
    window=dict(pointer=0x2890564,objects=[(0x1c8,0x805168,0x80009c,11)])
    assert model_arrays(dict(catalog,lists=catalog['lists']+[window]))[0]==[0x1c8,0x800000,0x800100]
    for bad in (dict(catalog,lists=catalog['lists'][:1]),dict(catalog,wheels=[]),
                dict(catalog,cars=[]),dict(catalog,lists=catalog['lists']*2),
                dict(catalog,lists=catalog['lists']+[dict(window,pointer=3)]),
                dict(catalog,lists=catalog['lists']+[dict(window,objects=[(0x1c8,0x805169,0x80009c,11)])])):
        try: model_arrays(bad)
        except ValueError: continue
        raise AssertionError('invalid model binding accepted')
    camera=bytearray(104)
    struct.pack_into('<I',camera,48,100)
    struct.pack_into('<2I',camera,96,0xffffffff,MAIN_BASE)
    keys,times=camera_arrays(camera,MAIN_BASE-48,1)
    assert len(keys)==2 and times==[0,100]
    stale=bytearray(152);stale[:96]=camera[:96]
    struct.pack_into('<I',stale,96,10)
    struct.pack_into('<2I',stale,144,0xffffffff,MAIN_BASE)
    assert camera_arrays(stale,MAIN_BASE-48,1)[1]==[0,100]
    struct.pack_into('<I',stale,140,1) # A passed key's callback still fires: never drop it.
    try: camera_arrays(stale,MAIN_BASE-48,1)
    except ValueError: pass
    else: raise AssertionError('stale camera key with callback accepted')
    for offset,value in ((44,1),(20,1),(8,0x7f800000),(48,0)):
        invalid=bytearray(camera);struct.pack_into('<I',invalid,offset,value)
        try:
            camera_arrays(invalid,MAIN_BASE-48,1)
        except ValueError:
            continue
        raise AssertionError('invalid camera accepted')
    stage = dict(source=100, spawn=[(0, 2, 384 << 16, 0xcf74)],
                 events=[(1, 3, 384 << 16, 0x2265c0), (2, 0, 65535, 100)])
    speed = lambda index: index * 0.25
    result = stage_arrays(dict(stages=[stage]), speed, 196)
    assert result[0][0] == ['{0,0x1.0000000000000p-1f,0,384,0}']
    last = dict(stage, spawn=[(0, 2, 195, 0xcf74)])
    assert stage_arrays(dict(stages=[last]), speed, 196, True)[0][0] == ['{0,0x1.0000000000000p-1f,195,0,0}']
    for limit, allowed in ((195, False), (196, True)):
        try: stage_arrays(dict(stages=[dict(stage, spawn=[(0, 2, limit, 0xcf74)])]), speed, 196, allowed)
        except ValueError: continue
        raise AssertionError("spawn beyond exported profiles accepted")
    assert result[0][1][-1] == '{2,0.0f,-1,0,0}'
    # Unsorted times are the original's (2264f4 fires in list order): accepted as listed.
    unsorted = stage_arrays(dict(stages=[dict(stage, events=[(2, 3, 0, 0), (1, 0, 65535, 100)])]), speed, 196)
    assert [row.split(',')[0] for row in unsorted[0][1]] == ['{2', '{1']
    for broken in (
            dict(stage, spawn=[(8, 2, 0, 0xcf74)]),
            dict(stage, spawn=[(0, 2, 0, 0xdeadbeef)]),
            dict(stage, spawn=[(0, 2, 0, 0xcfc0)]),
            dict(stage, spawn=stage['spawn']*2),
            dict(stage, spawn=stage['spawn']+[(0,2,0,0xd05c)]*2),
            dict(stage, events=[(1, 3, 1, 0), (2, 0, 65535, 100)]),
            dict(stage, events=[(1, 3, 0, 0xdeadbeef), (2, 0, 65535, 100)]),
            dict(stage, events=[(2, 0, 65535, 200)])):
        try:
            stage_arrays(dict(stages=[broken]), speed, 196)
        except ValueError:
            continue
        raise AssertionError("invalid intro data accepted")
    try:
        literal(float('inf'))
    except ValueError:
        pass
    else:
        raise AssertionError("nonfinite literal accepted")
    print("PASS: native intro/camera records, float32 literals, resolved transitions and rejection checks")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('images', type=Path, nargs='?')
    parser.add_argument('output', type=Path, nargs='?')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--race-cars', action='store_true', help='include all40 course0 body bindings (requires matching mesh/material pack)')
    parser.add_argument('--podium', action='store_true', help='write podium_data.h stage tables for phase27/28 instead of the intro header')
    parser.add_argument('--race-start', action='store_true', help='write race_start_data.h (course0 rolling-start camera keys) instead of the intro header')
    parser.add_argument('--courses', default='0',
                        help='with --race-start or --podium: comma-separated course slots 501460 (0 Beginner, 1 Expert, 2 Advanced); 0 is always one')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    else:
        if args.images is None or args.output is None:
            parser.error('provide verified image directory and NEW header path')
        images = {}
        for name, (size, digest) in IMAGES.items():
            data = (args.images / name).read_bytes()
            if len(data) != size or hashlib.sha256(data).hexdigest() != digest:
                raise ValueError(f'unrecognized daytona93 image: {name}')
            images[name] = data
        slots = [int(v) for v in args.courses.split(',')]
        if slots != [0] and not (args.race_start or args.podium):
            parser.error('--courses needs --race-start or --podium')
        output = (prepare_race_start(images['program.bin'], images['main_data.bin'], slots) if args.race_start else
                  prepare_podium(images['program.bin'], images['main_data.bin'], slots) if args.podium else
                  prepare(images['program.bin'], images['main_data.bin'],args.race_cars))
        with args.output.open('x') as file:
            file.write(output)
        print(f'Native intro driving/camera header: {len(output)} source bytes; no texture/mesh data')
