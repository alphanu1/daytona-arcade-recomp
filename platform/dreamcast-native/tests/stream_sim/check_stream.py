"""Independent check of the model renderer's PVR stream over a recorded script.

Builds the host submission simulator (build_sim.py) and compares every frame,
triangle by triangle, with the reference renderer tests/reference_stream.h,
at scenery stages 3 and 5 (and 6 for a world pack with world objects). Needs prepared assets (car.dnm/.dnt, world.dnm/.dnt)
and a frame table; both are generated files outside the repository.

usage: check_stream.py [--fence-sweep] [--course N] ASSETS_DIR FRAMES.frm [more FRAMES.frm ...]
           [--source UNCUT_ASSETS_DIR UNCUT_ORDER.json ORDER.json [SPEC.json]]

gen_frames.py writes the frame tables (FRM5, with the scale of the road class each frame was drawn at).
--course N takes ASSETS_DIR/courseN-world.dnm for world.dnm (1 Expert, 2 Advanced); Expert's frames run on the record
cache, as its race does (NM_SQ_STREAM_POOL).

The road class at another scale (model_submission.h sq_road_depth_set), per table at the last stage: the scale changed
in the middle of the run must give, word for word, what a world built at that scale from the start gives and what the
queue path gives, and every mesh on the stream path must hold the records of a build at the frame's scale
(SIM_ROAD_DEPTH_CHECK); then the same with the scale changing in the middle of the record cache's builds of meshes. A
table on which no such change falls on a road record, or no mesh so started again reaches the path, fails.

--fence-sweep builds the simulator with -DNM_FENCE_SWEEP=1 (make NATIVE_FENCE_SWEEP=1; the default is 0):
the catch-fence sweep, which acts only on a world pack with a fence rule's stipple groups (assets-ssfar4cf3 ..
assets-ssfar4eu; the recommended assets-ssfar4f has none).

The reference renders the pack it is given, so by itself it says nothing
about a world pack whose punch-through faces were cut offline into opaque
pieces (tools/opaque_cores.py): their triangles are legitimately not the
arcade's. --source adds the missing half, check_cutout_sources.py: every
changed face of ASSETS_DIR/world.dnm against its own source face in the
uncut pack (texels covered, colours, plane, layer), from the shipped files.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

if len(sys.argv) < 3:
    raise SystemExit(__doc__)
here = Path(__file__).resolve().parent
arguments, source = sys.argv[1:], None
defines = []
course = 0
if '--course' in arguments:
    at = arguments.index('--course')
    course = int(arguments[at+1])
    arguments = arguments[:at]+arguments[at+2:]
if '--fence-sweep' in arguments:
    arguments = [a for a in arguments if a != '--fence-sweep']
    defines.append('-DNM_FENCE_SWEEP=1')
if '--source' in arguments:
    at = arguments.index('--source')
    arguments, source = arguments[:at], arguments[at+1:]
    if len(source) not in (3, 4) or len(arguments) < 2:
        raise SystemExit(__doc__)
assets, frames = arguments[0], arguments[1:]
if source:
    run = subprocess.run([sys.executable, str(here/'check_cutout_sources.py'), source[0], assets, *source[1:]],
                         capture_output=True, text=True)
    last = (run.stdout.strip().splitlines() or [run.stderr[-2000:]])[-1]
    if run.returncode or not last.startswith('PASS'):
        print(run.stdout[-3000:] or run.stderr[-2000:])
        raise SystemExit('FAIL: a cut face does not draw what its source face drew')
    print('PASS: cut faces against their sources: ' + last[6:])
# A world pack with world objects (meshpack.h, NM_PACK_WORLD_OBJECTS) is also checked at stage 6;
# its frame tables must be FRM2 (cell bitmaps and race state for the object selection).
import struct
world = f'course{course}-world' if course else 'world'
header = struct.unpack_from('<16I', (Path(assets)/f'{world}.dnm').read_bytes())
stages = ('3', '5', '6') if (header[0] & 0x7fffffff) == 0x364d4e44 and header[15] & 8 else ('3', '5')
# The record cache of a course without the whole tables (model_submission.h NM_SQ_STREAM_POOL, course_assets.h).
POOL = str(448*1024)
production = dict(SIM_WORLD=world)
if course == 1:
    production['SIM_STREAM_POOL'] = POOL
SCALES = ('0.5', '0.755102', '0.861751', '0.636364')  # NM_ROAD_DEPTH_SCALE, VR3, VR4, VR1

def frame_words(path):
    """Per frame what the stream's words give: tick, triangles and the digests of sim_main.c's F lines."""
    rows = []
    for line in Path(path).read_text().splitlines():
        if line.startswith('F '):
            fields = dict(item.split('=', 1) for item in line.split(' car:')[0].split()[1:])
            rows.append((fields['tick'], fields['triangles'], fields['digest'], fields['geometry'], fields['colour'], fields['wrapped']))
    return rows

def road_run(sim, tmp, table, stage, label, **settings):
    """One simulator run under the record check; its frames and its road depth line."""
    out = str(Path(tmp)/f'road-{label}.txt')
    run = subprocess.run([sim, assets, table, out], capture_output=True, text=True,
                         env=dict(os.environ, SIM_ORACLE='1', SIM_STAGE=stage, SIM_ROAD_DEPTH_CHECK='1', SIM_WORLD=world, **settings))
    lines = [line for line in run.stdout.splitlines() if 'reference' in line or line.startswith('road depth')]
    summary = [line for line in lines if line.startswith('road depth:')]
    if run.returncode or not summary or not any('independent reference' in line and ' 0 mismatches' in line for line in lines):
        print('\n'.join(lines[:12]) or run.stderr[-2000:])
        raise SystemExit(f'FAIL: road depth, {label}, {table}')
    return frame_words(out), summary[0]

def road_same(what, table, rows, other):
    if len(rows) != len(other):
        raise SystemExit(f'FAIL: road depth, {Path(table).name}: {what}: {len(rows)} frames against {len(other)}')
    for row, expected in zip(rows, other):
        if row != expected:
            raise SystemExit(f'FAIL: road depth, {Path(table).name}: {what}, tick {row[0]}: {row} against {expected}')

def road_depth(sim, tmp, table, stage):
    # A change in the middle of the run: frames at each scale against the world built at it from the start.
    count = len(road_run(sim, tmp, table, stage, 'table')[0])
    if count < 8:
        raise SystemExit(f'FAIL: {table}: {count} frames are too few for the road depth runs')
    starts = [count*k//len(SCALES) for k in range(len(SCALES))]
    steps = ','.join(f'{first}:{scale}' for first, scale in zip(starts, SCALES))
    rows, summary = road_run(sim, tmp, table, stage, 'steps', SIM_ROAD_DEPTH_STEPS=steps)
    queue, _ = road_run(sim, tmp, table, stage, 'steps-queue', SIM_ROAD_DEPTH_STEPS=steps, SIM_NO_STREAM='1')
    road_same('the rewritten streams against the queue path', table, rows, queue)
    for k, scale in enumerate(SCALES):
        built, _ = road_run(sim, tmp, table, stage, f'built-{k}', SIM_ROAD_DEPTH=scale)
        end = starts[k+1] if k+1 < len(SCALES) else count
        road_same(f'rewritten to {scale} against built at it', table, rows[starts[k]:end], built[starts[k]:end])
    print(f'PASS: stage {stage}, {Path(table).name}: {summary}; every frame word for word the world built at its scale and the queue path')
    # The record cache: builds of several frames, and enough frames between two changes for a mesh started again to finish.
    cycle = dict(SIM_ROAD_DEPTH_CYCLE=str(max(count//16, 8)))
    cached, summary = road_run(sim, tmp, table, stage, 'cache', SIM_STREAM_POOL=POOL, SIM_STREAM_BUDGET='300', **cycle)
    queue, _ = road_run(sim, tmp, table, stage, 'cache-queue', SIM_NO_STREAM='1', **cycle)
    road_same('the record cache against the queue path', table, cached, queue)
    match = re.search(r'; (\d+) came in a mesh.s build \((\d+) road records already written\), (\d+) of those', summary)
    if not match or not int(match[1]) or not int(match[2]) or not int(match[3]):
        raise SystemExit(f'FAIL: road depth, {Path(table).name}: no change caught a mesh half built and saw it on the path: {summary}')
    print(f'PASS: stage {stage}, {Path(table).name}: record cache: {summary}; every frame word for word the queue path')

with tempfile.TemporaryDirectory(prefix='daytona-stream-') as tmp:
    subprocess.run([sys.executable, str(here/'build_sim.py'), tmp, 'sim', *defines], check=True, stdout=subprocess.DEVNULL)
    for table in frames:
        for stage in stages:
            run = subprocess.run([str(Path(tmp)/'sim'), assets, table, os.devnull],
                                 env=dict(os.environ, SIM_ORACLE='1', SIM_STAGE=stage, SIM_ROAD_DEPTH_CHECK='1', **production), capture_output=True, text=True)
            lines = [line for line in run.stdout.splitlines() if 'reference' in line or 'road depth check' in line]
            if run.returncode or not any('independent reference' in line and ' 0 mismatches' in line for line in lines):
                print('\n'.join(lines[:12]) or run.stderr[-2000:])
                raise SystemExit(f'FAIL: stage {stage}, {table}: the submitted stream differs from the reference renderer')
            print(f'PASS: stage {stage}, {Path(table).name}: ' + lines[-1])
            translucent = [line for line in run.stdout.splitlines() if line.startswith('translucent list')]
            if translucent:
                print(f'PASS: stage {stage}, {Path(table).name}: ' + translucent[0])
        road_depth(str(Path(tmp)/'sim'), tmp, table, stages[-1])
    # The player's damaged bodies (car-damage.dnb): each written into the clean body's mesh and its tables refreshed must
    # give the tables a rebuild gives (SIM_DAMAGE_CHECK), and the wrecked body (variant 0) drawn all along the first
    # table must still be the reference renderer's stream.
    if (Path(assets)/'car-damage.dnb').is_file():
        run = subprocess.run([str(Path(tmp)/'sim'), assets, frames[0], os.devnull], capture_output=True, text=True,
                             env=dict(os.environ, SIM_ORACLE='1', SIM_STAGE=stages[-1], SIM_DAMAGE_CHECK='1', SIM_DAMAGE_VARIANT='0', **production))
        checks = [line for line in run.stdout.splitlines() if line.startswith('damage check:')]
        lines = [line for line in run.stdout.splitlines() if 'reference' in line]
        if (run.returncode or len(checks) != 17 or any('equal the rebuilt' not in line for line in checks) or
                not any('independent reference' in line and ' 0 mismatches' in line for line in lines)):
            print('\n'.join(checks+lines[:12]) or run.stderr[-2000:])
            raise SystemExit(f'FAIL: damaged bodies, {frames[0]}')
        print(f'PASS: damaged bodies: 17 variants refreshed as rebuilt; wrecked body, stage {stages[-1]}, '
              f'{Path(frames[0]).name}: ' + lines[-1])
