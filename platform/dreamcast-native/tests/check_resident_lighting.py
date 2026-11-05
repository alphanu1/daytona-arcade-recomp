"""Read-only feasibility check: fixed VQ bases plus ideal RGB modulation.

python3 platform/dreamcast-native/tests/check_resident_lighting.py WORLD.dnt SOURCE/materials
Uses exact pre-VQ M2T palettes/indices, weighted over each binding's crop.
Shared bases cover the union of all variant crops. Full-codebook VQ costs are
prospective upper estimates, not encoded assets. floor(base*factor/256) matches
the constant nearest PAL1555 hardware probe; applying it before VQ/quantization
does NOT prove final pixels. No quality threshold or renderer change is approved.
"""
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import struct
import sys


def union_crop(width, height, rectangles):
    """Smallest power-of-two bounding dimensions, even origins, >=8 texels."""
    assert rectangles and width >= 8 and height >= 8
    assert not width & (width-1) and not height & (height-1)
    for x, y, w, h in rectangles:
        assert min(x, y) >= 0 and w >= 8 and h >= 8
        assert not (x | y | w | h) & 1 and x+w <= width and y+h <= height
    origin, extent = [], []
    for axis, full in enumerate((width, height)):
        low = min(r[axis] for r in rectangles)
        high = max(r[axis]+r[axis+2] for r in rectangles)
        size = max(8, 1 << (high-low-1).bit_length())
        assert size <= full
        start = min(low, full-size) & ~1
        assert start <= low and start+size >= high
        origin.append(start); extent.append(size)
    return (*origin, *extent)


def crop_weights(base, width, rectangle):
    x0, y0, w, h = rectangle
    weights = Counter()
    for y in range(y0, y0+h):
        for x in range(x0, x0+w):
            at = 64+2*((y//2)*(width//2)+x//2)
            cell = base[at] | base[at+1] << 8
            weights[(cell >> (4*(2*(y&1)+(x&1)))) & 15] += 1
    return weights


def inspect(pack, sources):
    data = pack.read_bytes()
    magic, size, resources, bindings, *_ = struct.unpack_from("<4s7I", data)
    assert magic == b"DNT3" and size == len(data)
    records = [struct.unpack_from("<IIHHI", data, 32+i*16) for i in range(resources)]
    source = {}
    for line in (sources / "materials.txt").read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        name, fmt, width, height, *key = line.split()
        assert Path(name).name == name and fmt in ("RGB565", "ARGB1555")
        key = tuple(map(int, key))
        assert len(key) == 5 and key not in source
        source[key] = (name, int(width), int(height))
    families = defaultdict(list)
    for i in range(bindings):
        h0, h1, h2, h3, luma, resource, crop = struct.unpack_from("<6HI", data, 32+resources*16+i*16)
        if resource != 65535:
            assert resource < resources
            families[h0, h1, h2, h3].append((luma, resource, crop))
    report = []
    estimated_vram = split_families = varying_crops = 0
    total_error = total_samples = exact = count = maximum = 0
    for header, variants in sorted(families.items()):
        base_luma, _, _ = max(variants)
        name, width, height = source[header+(base_luma,)]
        base = (sources / Path(name).with_suffix(".m2t")).read_bytes()
        assert len(base) == 64+width*height//2
        colors = [tuple(base[i:i+4]) for i in range(0, 64, 4)]  # BGRA
        indices_hash = hashlib.sha256(base[64:]).digest()
        rectangles = {(crop & 65535, crop >> 16, *records[resource][2:4])
                      for _, resource, crop in variants}
        x0, y0, w, h = union_crop(width, height, rectangles)
        varying_crops += len(rectangles) > 1
        split_families += max(w, h) > 1024
        # Worst-case 256-entry VQ book, aligned payload and one KOS allocation.
        resident_bytes = ((2048+w*h//4+31) & ~31)+32
        estimated_vram += resident_bytes
        footprints = {r: crop_weights(base, width, r) for r in rectangles}
        worst = 0
        family_error = family_samples = 0
        for luma, resource, variant_crop in variants:
            name, sw, sh = source[header+(luma,)]
            state = (sources / Path(name).with_suffix(".m2t")).read_bytes()
            assert (sw, sh) == (width, height) and len(state) == len(base)
            assert hashlib.sha256(state[64:]).digest() == indices_hash
            rectangle = (variant_crop & 65535, variant_crop >> 16, *records[resource][2:4])
            weights = footprints[rectangle]
            target = [tuple(state[i:i+4]) for i in range(0, 64, 4)]
            assert all(a[3] == b[3] for a, b in zip(colors, target))
            error = samples = peak = 0
            for channel in range(3):
                # Permit separate RGB factors: more expressive than gray light.
                # A failure here cannot be repaired by a single scalar factor.
                factor = min(range(256), key=lambda f: sum(weight *
                    ((colors[index][channel]*f//256)-target[index][channel])**2
                    for index, weight in weights.items() if colors[index][3]))
                for index, weight in weights.items():
                    if not colors[index][3]:
                        continue
                    delta = abs(colors[index][channel]*factor//256-target[index][channel])
                    error += delta*weight
                    samples += weight
                    peak = max(peak, delta)
            exact += peak == 0
            count += 1
            total_error += error
            total_samples += samples
            maximum = max(maximum, peak)
            family_error += error
            family_samples += samples
            if peak > worst:
                worst, worst_luma = peak, luma
        report.append(dict(header=header, variants=len(variants), base_luma=base_luma,
            crop=[x0, y0, w, h], variant_footprints=len(rectangles),
            estimated_vq_bytes_with_kos=resident_bytes,
            mean_abs_error=family_error/max(1, family_samples),
            max_error=worst, worst_luma=worst_luma if worst else None))
    return dict(families=len(families), bindings=count, exact_bindings=exact,
        current_vram_with_kos=sum(r[1] for r in records),
        families_with_different_crops=varying_crops,
        families_requiring_further_split=split_families,
        proposed_full_codebook_vq_bytes_with_kos=estimated_vram,
        ideal_modulation_mean_abs_error=total_error/max(1, total_samples),
        ideal_modulation_max_error=maximum,
        caveat="Prospective union-cropped full-codebook estimate, no dedup. Pre-VQ source RGB times factor//256, no base quantization/VQ/filtering errors; highest AVAILABLE luma, not guaranteed255; no visual approval or final course coverage proof",
        worst_families=sorted(report, key=lambda r: r["max_error"], reverse=True))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    print(json.dumps(inspect(Path(sys.argv[1]), Path(sys.argv[2])), indent=2))
