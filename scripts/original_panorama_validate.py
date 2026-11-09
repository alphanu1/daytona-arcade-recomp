#!/usr/bin/env python3
"""Validate a course's original sky, retaining hashes and a small gallery.

Needs NumPy. Raw captures/frame logs are discarded after measuring each job.
Use a fresh ignored output directory when executables or inputs change.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import time
import struct
import zlib

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
ASPECTS = {"16:9": 682, "32:9": 1366}


def png(data, width, height=384):
    """BGRA dump to RGB PNG; slice assignment avoids a Python loop per pixel."""
    if len(data) != width * height * 4:
        raise ValueError("frame size does not match dimensions")
    rgb = bytearray(width * height * 3)
    rgb[0::3], rgb[1::3], rgb[2::3] = data[2::4], data[1::4], data[0::4]
    stride = width * 3
    rows = b"".join(b"\0" + rgb[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def executable(build, name):
    for path in (build / "Release" / (name + ".exe"), build / (name + ".exe"), build / name):
        if path.is_file():
            return path.resolve()
    raise ValueError(f"Build {name} first: cmake --build {build} --config Release --target {name}")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--nvram', type=Path, required=True)
    parser.add_argument('--set', choices=['daytona', 'daytona93'], default='daytona')
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--baseline-tools', type=Path)
    parser.add_argument('--bench', action='store_true')
    parser.add_argument('--course', choices=['beginner', 'advanced', 'expert'], default='beginner')
    args = parser.parse_args()
    out = args.output.resolve()
    build = (args.build_dir or ROOT / f'build-{args.set}').resolve()
    cache = (build / 'CMakeCache.txt').read_text()
    if not re.search(r'^M2_ROMSET:[^=]+=' + re.escape(args.set) + r'$', cache, re.MULTILINE):
        raise SystemExit('Build ROM set does not match --set')
    rom_dir = build / 'rom_cache' / args.set
    # Deletion is limited to generated files inside this dedicated ignored run.
    if not out.is_relative_to(ROOT / 'traces') or out == ROOT / 'traces' or out.is_symlink():
        raise SystemExit('Output must be a dedicated directory beneath traces/')
    tools = {r: executable(build, name) for r, name in [('software', 'm2run'), ('hardware', 'm2gpushot')]}
    input_file = 'race_to_end.txt' if args.course == 'beginner' else f'widescreen_{args.course}.txt'
    course_id = {'beginner': 0, 'advanced': 2, 'expert': 1}[args.course]
    inputs = (ROOT / 'scripts/inputs' / input_file).read_text() + '\n3200-3209 vr1=1\n3800-3809 vr2=1\n4400-4409 vr3=1\n5000-5009 vr4=1\n'
    signature = {'set': args.set, 'tools': {r: digest(p.read_bytes()) for r, p in tools.items()},
                 'inputs': digest(inputs.encode()), 'runner': digest(Path(__file__).read_bytes()),
                 'nvram': {n: digest((args.nvram / n).read_bytes()) for n in ['ioboard_eeprom.bin', 'backup_ram.bin']},
                 'rom': {p.name: digest(p.read_bytes()) for p in sorted(rom_dir.glob('*.bin'))}}
    out.mkdir(parents=True, exist_ok=True)
    manifest = out / 'manifest.json'
    if manifest.exists():
        if json.loads(manifest.read_text()) != signature:
            raise SystemExit('Run inputs changed: use a fresh output directory')
    else:
        manifest.write_text(json.dumps(signature, indent=2))
        (out / 'nvram').mkdir()
        for name in signature['nvram']:
            shutil.copyfile(args.nvram / name, out / 'nvram' / name)
        (out / 'inputs.txt').write_text(inputs)
    checks = []

    def remove_generated(path):
        if path.is_symlink() or not path.resolve().is_relative_to(out):
            raise RuntimeError(f'Unsafe generated path: {path}')
        path.unlink()

    def run(name, renderer, aspect='32:9', frames=6000, every=300, first=0, flags=(), baseline=False, inputs_override=None):
        folder = out / name
        folder.mkdir(parents=True, exist_ok=True)
        exe = tools[renderer] if not baseline else args.baseline_tools.resolve() / tools[renderer].name
        script = inputs_override or out / 'inputs.txt'
        command = [str(exe), str(rom_dir), str(frames), '--nvram', str(out / 'nvram'),
                   '--inputs', str(script), '--dump', str(folder), '--dump-from', str(first), '--every', str(every),
                   '--sky-log', str(folder / 'sky.jsonl')]
        if aspect:
            command += ['--aspect', aspect]
        command += list(flags)
        key = {'command': command, 'executable': digest(exe.read_bytes()), 'inputs': digest(script.read_bytes())}
        result_file = folder / 'result.json'
        if result_file.exists():
            record = json.loads(result_file.read_text())
            if record['key'] != key:
                raise RuntimeError(f'Changed job: {name}')
            return record
        start = time.perf_counter()
        completed = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
        (folder / 'log.txt').write_text(completed.stdout + completed.stderr)
        if completed.returncode:
            raise RuntimeError(f'{name}: {completed.stderr}')
        rows = [json.loads(line) for line in (folder / 'sky.jsonl').read_text().splitlines()]
        state = {r['frame']: r for r in rows}
        width = ASPECTS[aspect] if aspect else 496
        scale = int(flags[list(flags).index('--scale') + 1]) if '--scale' in flags else 1
        expected = list(range(max(every, ((first + every - 1) // every) * every), frames + 1, every))
        record = {'key': key, 'seconds': time.perf_counter() - start, 'images': {},
                  'active_frames': sum(r['panorama'] for r in rows),
                  'race_inactive': [r['frame'] for r in rows if 3000 <= r['frame'] <= 6000 and not r['panorama']],
                  # These race replays finish their menu confirmations at 2400;
                  # exclude the earlier attract activation from the loading test.
                  'first_course_active': next((r['frame'] for r in rows if r['frame'] >= 2400 and r['panorama'] and r['course'] == course_id), None),
                  'natural_wraps': [b['frame'] for a, b in zip(rows, rows[1:])
                                    if a['panorama'] and b['panorama'] and a['course'] == b['course'] == course_id
                                    and abs(a['panorama_phase'] - b['panorama_phase']) > 64000],
                  'active_sky_colours': sorted({r['sky_colour'] for r in rows if r['panorama'] and 'sky_colour' in r})}
        uploads = re.search(r'panorama uploads (\d+)', completed.stdout)
        if uploads:
            record['uploads'] = int(uploads[1])
        for frame in expected:
            path = folder / f'run_{frame:05d}.rgb'
            raw = path.read_bytes()
            assert len(raw) == width * 384 * 4 * scale * scale, name
            pixels = np.frombuffer(raw, dtype='<u4').reshape(384 * scale, width * scale)
            margin = (width - 496) // 2 * scale
            item = {'hash': digest(raw), 'centre': digest(pixels[:, margin:margin + 496 * scale].tobytes()),
                    'state': state[frame]}
            if width >= 682:
                start16 = (width - 682) // 2 * scale
                item['crop16'] = digest(pixels[:, start16:start16 + 682 * scale].tobytes())
            if scale > 1:
                item['nearest'] = np.array_equal(pixels, pixels[::scale, ::scale].repeat(scale, 0).repeat(scale, 1))
                item['unscaled'] = digest(pixels[::scale, ::scale].tobytes())
            record['images'][str(frame)] = item
            if frame in [3000, 3600, 3693, 3694, 4200, 4800, 5100, 5400] and renderer == 'hardware' and not baseline:
                path.with_suffix('.png').write_bytes(png(raw, width * scale, 384 * scale))
            remove_generated(path)
        remove_generated(folder / 'sky.jsonl')
        if renderer == 'hardware':
            previous, expected_uploads = None, 0
            for item in record['images'].values():
                if item['state']['panorama']:
                    course = item['state']['panorama_course']
                    if course != previous:
                        expected_uploads += 1
                        previous = course
            assert record['uploads'] == expected_uploads, f'{name}: stale cache or redundant upload'
        result_file.write_text(json.dumps(record, indent=2))
        print(f'{name}: {len(expected)} frames, {record["active_frames"]} active, {record.get("uploads", "CPU")} uploads', flush=True)
        return record

    def compare(label, a, b, field='hash', other=None, predicate=lambda _: True):
        keys = [f for f, row in a['images'].items() if f in b['images'] and predicate(row)]
        assert keys, label
        bad = [f for f in keys if a['images'][f][field] != b['images'][f][other or field]]
        checks.append({'check': label, 'frames': len(keys), 'mismatches': bad})
        (out / 'checks.json').write_text(json.dumps(checks, indent=2))
        if bad:
            raise RuntimeError(f'{label}: mismatched frames {bad[:20]}')

    original = ['--panorama-original']
    isolated = ['--panorama-only']
    skies = {}
    for aspect in ['16:9', '32:9']:
        slug = aspect.replace(':', 'x')
        off = run(f'{slug}/sky-off', 'hardware', aspect, every=60, flags=isolated)
        on = run(f'{slug}/sky-original', 'hardware', aspect, every=60, flags=original + isolated)
        compare(f'{aspect}: background centre unchanged, including fades and overlays', on, off, 'centre')
        assert not on['race_inactive'], f'{args.course}: inactive race frames {on["race_inactive"][:20]}'
        assert on['images']['3600']['state']['panorama_course'] == course_id
        skies[aspect] = on
    # Independent widths can change guest camera state. Frozen-frame aspect
    # crops are checked by m2panoramacheck instead of comparing separate replays.
    cpu = run('32x9/sky-software', 'software', every=60, flags=original + isolated)
    compare('Software/GPU complete background', cpu, skies['32:9'])
    wraps = [f for f in skies['32:9']['natural_wraps'] if f >= 3000]
    wrap_script = None
    if not wraps and args.set == 'daytona93' and args.course == 'expert':
        wrap_script = ROOT / 'scripts/inputs/panorama_daytona93_expert_wrap.txt'
        probe = run('wrap/probe', 'hardware', first=3000, every=6000,
                    flags=original + isolated, inputs_override=wrap_script)
        assert not probe['race_inactive'], 'Wrap steering caused panorama fallback'
        wraps = probe['natural_wraps']
    assert wraps, f'{args.course}: replay needs a natural wrap to validate'
    wrap = wraps[0]
    detail_off = run('wrap/off', 'hardware', frames=wrap + 10, first=wrap - 7, every=1,
                     flags=isolated, inputs_override=wrap_script)
    detail_on = run('wrap/original', 'hardware', frames=wrap + 10, first=wrap - 7, every=1,
                    flags=original + isolated, inputs_override=wrap_script)
    assert detail_on['active_frames'] == 18, 'Panorama must stay active through the full-phase wrap'
    compare('Consecutive natural full-phase wrap: centre unchanged', detail_on, detail_off, 'centre')
    loading = skies['32:9']['first_course_active']
    loading_off = run('loading/off', 'hardware', frames=loading + 25, first=max(1, loading - 20), every=1, flags=isolated)
    loading_on = run('loading/original', 'hardware', frames=loading + 25, first=max(1, loading - 20), every=1, flags=original + isolated)
    compare('Consecutive sky-loading transition: centre unchanged', loading_on, loading_off, 'centre')
    for renderer, frames in [('hardware', 18000), ('software', 6000)]:
        off = run(f'full/{renderer}/off', renderer, frames=frames)
        on = run(f'full/{renderer}/original', renderer, frames=frames, flags=original)
        compare(f'{renderer}: complete frame centre unchanged', on, off, 'centre')
        compare(f'{renderer}: inactive/menu fallback unchanged', on, off, predicate=lambda r: not r['state']['panorama'])
        if args.baseline_tools:
            old = run(f'baseline/{renderer}', renderer, frames=frames, baseline=True)
            compare(f'{renderer}: enhancement off versus previous executable', off, old)
    native_off = run('native/off', 'hardware', aspect=None)
    native_on = run('native/original', 'hardware', aspect=None, flags=original)
    compare('Native view unchanged', native_on, native_off)
    assert native_on['uploads'] == 0 and native_on['active_frames'] == 0
    large = run('scale2', 'hardware', frames=3600, every=3600, first=3600, flags=original + isolated + ['--scale', '2'])
    compare('2x supersampling retains source pixels', large, skies['32:9'], 'unscaled', 'hash')
    assert large['images']['3600']['nearest']
    hud_off = run('hud/off', 'hardware', flags=['--hud-edges'])
    hud_on = run('hud/original', 'hardware', flags=original + ['--hud-edges'])
    compare('HUD at edges: centre/overlays unchanged', hud_on, hud_off, 'centre')
    stretch = run('stretch', 'hardware', flags=original + ['--stretch-backdrop'])
    compare('Original panorama takes precedence over stretch in supported scenes', stretch,
            json.loads((out / 'full/hardware/original/result.json').read_text()),
            predicate=lambda r: r['state']['panorama'])
    if args.bench:
        benchmarks = []
        for mode in ['off', 'original', 'original', 'off']:
            command = [str(tools['hardware']), str(rom_dir), '6000', '--inputs', str(out / 'inputs.txt'),
                       '--nvram', str(out / 'nvram'), '--aspect', '32:9', '--bench', '--bench-from', '3000']
            if mode == 'original':
                command += original
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=True)
            benchmarks.append({'mode': mode, 'command': command, 'output': result.stdout + result.stderr})
        (out / 'benchmarks.json').write_text(json.dumps(benchmarks, indent=2))
    print(f'PASS: {len(checks)} comparisons; raw captures removed after hashing.', flush=True)


if __name__ == '__main__':
    main()
