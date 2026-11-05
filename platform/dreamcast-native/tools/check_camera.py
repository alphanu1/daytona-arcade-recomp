"""Compare source instances in an explicitly chosen anchor's coordinate frame.

This is offline calibration evidence, not a camera replay or automatic static/
dynamic classification. The anchor is ASSUMED to be a fixed world object;
equal relative poses alone cannot prove that assumption (car parts also agree).
The optional --world-vehicles check resolves body identity via authored model
lists and gates normal-body matrices against before-frame state. It calibrates
authored coordinate units, not physical metres, crash poses, projection or light.
The original cross-frame residual report alone is not an acceptance gate.
Uses the validated model exporter/packer; no new dependency or target format.
"""
import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import re
import struct

from prepare_models import floats, prepare, rows


IDENTITY = [[1., 0., 0., 0.], [0., 1., 0., 0.], [0., 0., 1., 0.]]


def multiply(a, b):
    result = [[sum(a[r][k] * b[k][c] for k in range(3)) +
               (a[r][3] if c == 3 else 0) for c in range(4)] for r in range(3)]
    if not all(math.isfinite(v) for row in result for v in row):
        raise ValueError("non-finite affine product")
    return result


def inverse(matrix):
    if not all(math.isfinite(v) for row in matrix for v in row):
        raise ValueError("non-finite anchor matrix")
    # Partial-pivot Gauss-Jordan, only for this host-side 3x3 affine inverse.
    work = [matrix[r][:3] + IDENTITY[r][:3] for r in range(3)]
    for c in range(3):
        pivot = max(range(c, 3), key=lambda r: abs(work[r][c]))
        work[c], work[pivot] = work[pivot], work[c]
        divisor = work[c][c]
        if not divisor:
            raise ValueError("singular anchor matrix")
        work[c] = [v / divisor for v in work[c]]
        for r in range(3):
            if r != c:
                scale = work[r][c]
                work[r] = [v - scale * p for v, p in zip(work[r], work[c])]
    result = [row[3:] + [-sum(row[3+k] * matrix[k][3] for k in range(3))]
              for row in work]
    norm = lambda m: max(sum(abs(v) for v in row[:3]) for row in m)
    if norm(matrix) * norm(result) > 1e6:
        raise ValueError("ill-conditioned anchor; choose a different object")
    for product in (multiply(matrix, result), multiply(result, matrix)):
        if max(abs(product[r][c] - IDENTITY[r][c]) for r in range(3) for c in range(4)) > 1e-8:
            raise ValueError("anchor inverse reconstruction failed")
    return result


def load(directory, anchor):
    # Reuse all source bounds, indices, finite/size and manifest checks.
    prepare(directory)
    instances = []
    for fields in rows(directory / "instances.txt"):
        mesh, address, window, reverse = map(int, fields[:4])
        digest = hashlib.sha256((directory / f"mesh_{mesh}.txt").read_bytes()).hexdigest()
        values = floats(fields[4:16])
        matrix = [[values[c*3+r] for c in range(4)] for r in range(3)]
        instances.append(((address, window, reverse, digest), mesh, matrix))
    anchors = [item for item in instances if item[0][0] == anchor]
    if len(anchors) != 1:
        raise ValueError(f"{directory}: anchor must occur exactly once, found {len(anchors)}")
    key, _, camera = anchors[0]
    undo_camera = inverse(camera)
    groups = defaultdict(list)
    for identity, mesh, matrix in instances:
        if identity[1:3] == key[1:3]:
            groups[identity].append((mesh, multiply(undo_camera, matrix)))
    return key, groups


def compare(baseline, current):
    anchor, left = baseline
    key, right = current
    if key != anchor:
        raise ValueError("anchor mesh/window/reverse changed; cannot compare this pair")
    result = []
    # Do not match repeated instances by draw order: it is not an entity ID.
    common = left.keys() & right.keys()
    ambiguous = sum(len(left[k]) != 1 or len(right[k]) != 1 for k in common)
    for k in sorted(common):
        if len(left[k]) != 1 or len(right[k]) != 1:
            continue
        (ma, a), (mb, b) = left[k][0], right[k][0]
        result.append(dict(source=hex(k[0]), baseline_mesh=ma, current_mesh=mb,
            linear_error=max(abs(a[r][c]-b[r][c]) for r in range(3) for c in range(3)),
            translation_error=max(abs(a[r][3]-b[r][3]) for r in range(3))))
    return dict(compared=len(result), ambiguous_keys=ambiguous,
                baseline_only_keys=len(left.keys()-right.keys()),
                current_only_keys=len(right.keys()-left.keys()), residuals=result)


def body_matrix(position, angles):
    """Normal body draw 6c40: translate, yaw, pitch, roll (no crash offsets)."""
    pitch, yaw, roll = [v * (2*math.pi/65536) for v in angles]
    cp, sp, cy, sy, cr, sr = (math.cos(pitch), math.sin(pitch), math.cos(yaw),
                             math.sin(yaw), math.cos(roll), math.sin(roll))
    y = [[cy, 0., -sy, position[0]], [0., 1., 0., position[1]], [sy, 0., cy, position[2]]]
    x = [[1., 0., 0., 0.], [0., cp, sp, 0.], [0., -sp, cp, 0.]]
    z = [[cr, sr, 0., 0.], [-sr, cr, 0., 0.], [0., 0., 1., 0.]]
    return multiply(multiply(y, x), z)


def match_bodies(groups, bodies):
    matched, ambiguous = [], 0
    for key, instances in groups.items():
        owners = [b for b in bodies if key[0] in b['models']]
        if len(owners) != 1 or len(instances) != 1:
            ambiguous += bool(owners)
            continue
        body = owners[0]
        actual = instances[0][1]
        expected = body_matrix(body['position'], body['angles'])
        linear = max(abs(actual[r][c]-expected[r][c]) for r in range(3) for c in range(3))
        translation = max(abs(actual[r][3]-expected[r][3]) for r in range(3))
        # Presentation-only gates, in authored world units (not physical metres).
        if linear > 1e-5 or translation > 1e-3:
            raise ValueError(f"body {body['address']:x} world mismatch: linear={linear:g}, translation={translation:g}")
        matched.append(dict(entity=hex(body['address']), model=hex(key[0]),
                            linear_error=linear, translation_error=translation))
    if not matched:
        raise ValueError('no unambiguous body models; zero coverage is not a pass')
    return dict(matched=matched, ambiguous=ambiguous)


def check_world(directory, anchor, log, images):
    from inspect_course import IMAGES, MAIN_BASE, courses, words
    data = {}
    for name, (size, digest) in IMAGES.items():
        path = images/name
        if path.stat().st_size != size:
            raise ValueError(f'incorrect daytona93 size: {name}')
        data[name] = path.read_bytes()
        if hashlib.sha256(data[name]).hexdigest() != digest:
            raise ValueError(f'unrecognized daytona93 image: {name}')
    scenery = courses(data['program.bin'], data['main_data.bin'])
    if not any(o['model'] == anchor for course in scenery for cell in course['cells'] for o in cell['objects']):
        raise ValueError('anchor is not an authored scenery model')
    bodies = []
    pattern = re.compile(r'native_vehicle_pose phase=before address=([0-9a-f]{8}) model=([0-9a-f]{8}) '
                         r'xyz_bits=([0-9a-f]{8}),([0-9a-f]{8}),([0-9a-f]{8}) '
                         r'render_angles=([0-9a-f]{4}),([0-9a-f]{4}),([0-9a-f]{4})')
    for line in log.read_text().splitlines():
        match = pattern.fullmatch(line)
        if not match:
            continue
        values = [int(v, 16) for v in match.groups()]
        address, pointer = values[:2]
        position = struct.unpack('<3f', struct.pack('<3I', *values[2:5]))
        if not all(math.isfinite(v) for v in position) or address%16 or not 0x500000 <= address <= 0x5ffd00:
            raise ValueError('invalid native vehicle evidence')
        models = set()
        for i in range(256):
            model = words(data['main_data.bin'], MAIN_BASE, pointer+16*i, 1)[0]
            if model == 0:
                break
            models.add(model)
        else:
            raise ValueError('unterminated body model list')
        bodies.append(dict(address=address, models=models, position=position, angles=values[5:]))
    if not 1 <= len(bodies) <= 64 or len({b['address'] for b in bodies}) != len(bodies):
        raise ValueError('missing/duplicate before-frame vehicle evidence')
    _, groups = load(directory, anchor)
    return dict(coverage='normal body matrices versus before-frame live vehicle state; not target pose assets',
                source=str(directory), **match_bodies(groups, bodies))


def self_test():
    world = [[2., .5, 0., 7.], [0., 3., 0., -5.], [0., 0., -1., 11.]]
    cameras = [IDENTITY, [[0., 0., 1., 13.], [0., 1., 0., -3.], [-1., 0., 0., 19.]]]
    key = (123, 1, 0, "synthetic")
    frames = [(key, {key: [(0, multiply(inverse(c), multiply(c, world)))]}) for c in cameras]
    row = compare(*frames)["residuals"][0]
    assert row["linear_error"] < 1e-12 and row["translation_error"] < 1e-12
    frames[1][1][key][0][1][0][3] += 2
    assert compare(*frames)["residuals"][0]["translation_error"] == 2
    frames[1][1][key].append(frames[1][1][key][0])
    assert compare(*frames)["compared"] == 0 and compare(*frames)["ambiguous_keys"] == 1
    try:
        compare(frames[0], ((123, 1, 0, "changed mesh"), frames[1][1]))
    except ValueError:
        pass
    else:
        raise AssertionError("changed anchor accepted")
    for bad in ([[0., 0., 0., 0.]] * 3,
                [[math.nan, 0., 0., 0.], IDENTITY[1], IDENTITY[2]],
                [[1e-9, 0., 0., 0.], IDENTITY[1], IDENTITY[2]]):
        try:
            inverse(bad)
        except ValueError:
            continue
        raise AssertionError("invalid anchor accepted")
    inverse(world)  # Nonuniform scale, shear and reflection are supported.
    body = dict(address=0x504e00, models={123}, position=[17., -3., 25.], angles=[128, 16384, 256])
    groups = {key: [(0, body_matrix(body['position'], body['angles']))]}
    assert len(match_bodies(groups, [body])['matched']) == 1
    for bad in ([body, body], [], [dict(body, position=[18., -3., 25.])],
                [dict(body, angles=[128, 0, 256])]):
        try:
            match_bodies(groups, bad)
        except ValueError:
            continue
        raise AssertionError('ambiguous/missing/moved/rotated vehicle accepted')
    print("PASS: anchor-relative affine reconstruction, moving/ambiguous instances and invalid anchors")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--anchor", type=lambda s: int(s, 0))
    parser.add_argument("--world-vehicles", type=Path, help="m2segment --inspect-native-vehicles log, before-frame poses")
    parser.add_argument("--images", type=Path, help="owned daytona93 images, only for --world-vehicles")
    parser.add_argument("sources", nargs="*", type=Path)
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.world_vehicles:
        if args.anchor is None or not args.images or len(args.sources) != 1:
            parser.error('world check requires --anchor, --images and one model source directory')
        print(json.dumps(check_world(args.sources[0], args.anchor, args.world_vehicles, args.images), indent=2))
    else:
        if args.anchor is None or len(args.sources) < 2:
            parser.error("provide --anchor SOURCE_ADDRESS and at least two source directories")
        baseline = load(args.sources[0], args.anchor)
        for directory in args.sources[1:]:
            print(json.dumps(dict(source=str(directory), **compare(baseline, load(directory, args.anchor))), indent=2))
