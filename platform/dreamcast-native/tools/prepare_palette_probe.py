"""Generate the data for tests/palette_probe (hardware shading comparison).

For a few real car and road textures and three polygon lumas each, emit:
(a) the 16 exact colours of that polygon as its own PAL4 palette,
(b) a colour-independent gain-bucket palette plus a vertex colour,
(c) the current pack's anchor texture and modulation colour for that binding,
all over the same 32x32 texel region, plus two VQ+PAL4 encodings of one index
texture. Colours follow tools/material.h using a reference video-table
dump. Output (ROM-derived) goes to a NEW directory under ignored build/:
palette_probe_data.h for the console program and probe.json for
tools/check_palette_probe.py.
"""
import argparse
import collections
import json
from pathlib import Path
import pickle
import struct

import numpy as np

from prepare_material_crops import block_index

BUCKETS = 4  # Saturating gain buckets per channel (plus the linear one).
ORIGIN_X, ORIGIN_Y, CELL = 32, 40, 64


def video(dump):
    pal = np.fromfile(dump/'palram.bin', '<u2')
    xlat = np.fromfile(dump/'colorxlat.bin', '<u2')
    luma = np.fromfile(dump/'lumaram.bin', np.uint8)
    sheets = [np.fromfile(dump/'tex0.bin', '<u4'), np.fromfile(dump/'tex1.bin', '<u4')]
    gamma = np.array([int(max((i-64.0)*255.0/191.0, 0.0)) for i in range(256)])
    curve = np.stack([gamma[xlat[(c << 8)+np.arange(64)] & 0xff] for c in range(32)])
    for channel in (1, 2):
        other = np.stack([gamma[xlat[channel*0x2000+(c << 8)+np.arange(64)] & 0xff] for c in range(32)])
        if not (other == curve).all():
            raise ValueError('colour translation differs per channel; the gain model assumes one curve')
    return pal, luma, sheets, curve


def ramp(luma, header):
    return luma[(((header[1] & 255) << 7)+np.arange(16)*8)*4].astype(np.int64)


def exact_palette(pal, luma, curve, header, light):
    colour = int(pal[((header[3] >> 6) & 0x3ff)+0x1000])
    level = np.minimum(63, ramp(luma, header)*light//256)
    return np.stack([curve[(colour >> (5*c)) & 31][level] for c in range(3)], 1)


def gain_palettes(luma, curve, header):
    """Colour-independent ramps: the linear one, then BUCKETS saturating gains."""
    base = ramp(luma, header)

    def channel(gain):
        return curve[31][np.minimum(63, base*gain//(31*256))]
    linear = 0
    for gain in range(1, 31*255+1, 8):
        if (channel(gain) >= 255).sum() <= 1:
            linear = gain
    gains = [linear]+[int(linear+(31*255-linear)*(i+1)/BUCKETS) for i in range(BUCKETS)]
    return [channel(g).astype(float) for g in gains]


def fit_bucket(target, ramps):
    best = None
    for bucket, g in enumerate(ramps):
        v = min(255, max(0, int(round(float((g*target).sum()/max((g*g).sum(), 1e-9))*255))))
        error = float(np.abs(np.round(g*v/255)-target).max())
        if best is None or error < best[0]:
            best = (error, bucket, v)
    return best


def texels(sheets, header):
    sheet = sheets[1 if header[2] & 0x1000 else 0]
    bx, by = (header[2] & 63)*32, ((header[2] >> 6) & 31)*32
    out = np.zeros((32, 32), np.uint8)
    for v in range(32):
        for u in range(32):
            x, y = bx+u, by+v
            fold = 512 if x >= 1024 else 0
            if fold:
                x -= 1024
            off = (((y >> 1) ^ fold) << 9)+(x >> 1)
            bit = ((off & 1) << 4) | ((1 ^ (v & 1)) << 3) | ((1 ^ (u & 1)) << 2)
            out[v, u] = (int(sheet[(off >> 1) & 0x7ffff]) >> bit) & 15
    return out


def pal4_twiddled(index):
    payload = bytearray(512)
    for y in range(32):
        for x in range(32):
            at = block_index(x, y, 32, 32)
            payload[at//2] |= int(index[y, x]) << ((at & 1)*4)
    return bytes(payload)


def vq_variants_2x2(index):
    """First guesses (2x2 blocks); hardware showed neither decodes. Unused."""
    blocks = {}
    codes = np.zeros((16, 16), np.uint8)
    for by in range(16):
        for bx in range(16):
            block = (int(index[2*by, 2*bx]), int(index[2*by+1, 2*bx]), int(index[2*by, 2*bx+1]), int(index[2*by+1, 2*bx+1]))
            codes[by, bx] = blocks.setdefault(block, len(blocks))
    if len(blocks) > 256:
        return None
    order = sorted(blocks, key=blocks.get)
    body = bytearray(256)
    for by in range(16):
        for bx in range(16):
            body[block_index(bx, by, 16, 16)] = codes[by, bx]
    wide = bytearray(2048)   # A: four 16-bit texels per entry, index in the low bits.
    narrow = bytearray(512)  # B: four 4-bit texels per entry.
    for i, block in enumerate(order):
        struct.pack_into('<4H', wide, 8*i, *block)
        struct.pack_into('<H', narrow, 2*i, block[0] | block[1] << 4 | block[2] << 8 | block[3] << 12)
    return bytes(wide)+bytes(body), bytes(narrow)+bytes(body), len(blocks)


def vq_variants(index):
    """VQ+PAL4 as 4x4 blocks: an 8-byte codebook entry holds 16 four-bit
    texels in twiddled order (64 bits either way), so the index map of a 32x32
    texture is 8x8 bytes. Two nibble orders: low nibble first (as raw PAL4),
    and high nibble first."""
    blocks, codes = {}, np.zeros((8, 8), np.uint8)
    for by in range(8):
        for bx in range(8):
            block = tuple(int(v) for v in index[4*by:4*by+4, 4*bx:4*bx+4].reshape(-1))
            codes[by, bx] = blocks.setdefault(block, len(blocks))
    order = sorted(blocks, key=blocks.get)
    body = bytearray(64)
    for by in range(8):
        for bx in range(8):
            body[block_index(bx, by, 8, 8)] = codes[by, bx]
    low, high = bytearray(2048), bytearray(2048)
    for i, block in enumerate(order):
        for y in range(4):
            for x in range(4):
                at = block_index(x, y, 4, 4)
                low[8*i+at//2] |= block[y*4+x] << ((at & 1)*4)
                high[8*i+at//2] |= block[y*4+x] << ((1-(at & 1))*4)
    return bytes(low)+bytes(body), bytes(high)+bytes(body), len(blocks)


def load_pack(path):
    data = path.read_bytes()
    magic, size, resources, count, *_ = struct.unpack_from('<4s7I', data)
    if magic != b'DNT4':
        raise ValueError('expected a DNT4 material pack')
    records = [struct.unpack_from('<IIHHI', data, 32+16*i) for i in range(resources)]
    base = 32+16*resources
    bindings = {}
    for i in range(count):
        row = struct.unpack_from('<6HI', data, base+16*i)
        bindings[row[:5]] = (row[5], row[6], struct.unpack_from('<I', data, base+16*count+4*i)[0])
    return data, records, bindings


def choose(usage, bindings, pal, wanted, fixed=()):
    """Most drawn opaque, unmirrored, uncropped headers with distinct colours."""
    by_header = collections.defaultdict(dict)
    for key, count in usage.items():
        header = key[:4]
        if (header[0] >> 13) & 3 != 2 or header[0] & 0x8300 or key not in bindings:
            continue
        resource, crop, _ = bindings[key]
        if resource == 65535 or crop:
            continue
        by_header[header][key[4]] = count
    chosen, colours = [], set()
    ranked = [h for h in fixed if h in by_header]+sorted(by_header, key=lambda h: -sum(by_header[h].values()))
    for header in ranked:
        colour = int(pal[((header[3] >> 6) & 0x3ff)+0x1000])
        lights = sorted(by_header[header])
        if header in chosen or colour in colours or len(lights) < 3:
            continue
        colours.add(colour)
        chosen.append((header, (lights[0], lights[len(lights)//2], lights[-1])))
        if len(chosen) == wanted:
            break
    if len(chosen) != wanted:
        raise ValueError('not enough suitable textures')
    return chosen


def c_bytes(name, data, align=32):
    rows = ','.join(str(b) for b in data)
    return f'static const unsigned char {name}[{len(data)}] __attribute__((aligned({align}))) = {{{rows}}};\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('video', type=Path, help='reference dump: palram/colorxlat/lumaram/tex0/tex1 .bin')
    parser.add_argument('usage', type=Path, help='materials.pkl of a logged race (header, luma) usage')
    parser.add_argument('assets', type=Path, help='asset directory with car.dnt and world.dnt')
    parser.add_argument('output', type=Path, help='NEW directory')
    args = parser.parse_args()
    args.output.mkdir()
    pal, luma, sheets, curve = video(args.video)
    usage = pickle.load(args.usage.open('rb'))['total']
    pages, source, palettes, report = [], [], [], {'cell': CELL, 'origin': [ORIGIN_X, ORIGIN_Y], 'pages': []}
    road = [(17435, 0, 7168, 40000), (17609, 0, 2988, 35200), (17424, 0, 3591, 40000)]
    for page, (name, kind, fixed) in enumerate((('car', 'car', ()), ('road', 'world', road))):
        data, records, bindings = load_pack(args.assets/f'{name if name == "car" else "world"}.dnt')
        cells = []
        for t, (header, lights) in enumerate(choose(usage[kind], bindings, pal, 3, fixed)):
            index = texels(sheets, header)
            source.append(c_bytes(f'probe_index_{page}_{t}', pal4_twiddled(index)))
            if t == 0:
                vq = vq_variants(index)
                if vq:
                    source.append(c_bytes(f'probe_vq_wide_{page}', vq[0]))
                    source.append(c_bytes(f'probe_vq_narrow_{page}', vq[1]))
                report.setdefault('vq_blocks', []).append(vq[2] if vq else None)
            ramps = gain_palettes(luma, curve, header)
            for l, light in enumerate(lights):
                exact = exact_palette(pal, luma, curve, header, light)
                fits = [fit_bucket(exact[:, c].astype(float), ramps) for c in range(3)]
                gain = np.stack([ramps[f[1]] for f in fits], 1)
                vertex = [f[2] for f in fits]
                resource, crop, modulation = bindings[(*header, light)]
                offset, size, width, height, _ = records[resource]
                blob = data[offset:offset+size]
                source.append(c_bytes(f'probe_anchor_{page}_{t}_{l}', blob))
                bank = len(palettes)//2
                palettes.append([int(v) for row in exact for v in row])
                palettes.append([int(round(v)) for row in gain for v in row])
                model = np.round(gain*np.array(vertex)/255)
                cells.append({'texture': t, 'light_index': l, 'header': list(header), 'light': light,
                              'bank_exact': 2*bank, 'bank_gain': 2*bank+1, 'vertex': vertex,
                              'buckets': [f[1] for f in fits], 'modulation': modulation,
                              'anchor': [width, height], 'model_max_error': float(np.abs(model-exact).max()),
                              'expected': exact[index].reshape(-1, 3).tolist(),
                              'gain_expected': model[index].reshape(-1, 3).astype(int).tolist()})
        report['pages'].append({'name': name, 'cells': cells})
    lines = ['/* Generated by tools/prepare_palette_probe.py; ROM-derived, do not commit. */\n',
             '#ifndef PALETTE_PROBE_DATA_H\n#define PALETTE_PROBE_DATA_H\n', *source,
             f'static const unsigned char probe_palettes[{len(palettes)}][48] = {{\n']
    lines += ['{'+','.join(map(str, p))+'},\n' for p in palettes]
    lines.append('};\ntypedef struct { const unsigned char *anchor; unsigned anchor_bytes; unsigned short width, height;'
                 ' unsigned char vertex[3], texture, light; unsigned modulation; } probe_cell_t;\n')
    lines.append('static const probe_cell_t probe_cells[2][9] = {\n')
    for page, entry in enumerate(report['pages']):
        lines.append('{')
        for cell in entry['cells']:
            name = f'probe_anchor_{page}_{cell["texture"]}_{cell["light_index"]}'
            lines.append(f'{{{name},sizeof({name}),{cell["anchor"][0]},{cell["anchor"][1]},'
                         f'{{{cell["vertex"][0]},{cell["vertex"][1]},{cell["vertex"][2]}}},{cell["texture"]},'
                         f'{cell["light"]},0x{cell["modulation"]:08x}u}},')
        lines.append('},\n')
    lines.append('};\nstatic const unsigned char *const probe_index[2][3] = {{probe_index_0_0,probe_index_0_1,probe_index_0_2},'
                 '{probe_index_1_0,probe_index_1_1,probe_index_1_2}};\n')
    have_vq = all(report['vq_blocks'])
    lines.append(f'#define PROBE_HAVE_VQ {1 if have_vq else 0}\n')
    if have_vq:
        lines.append('static const unsigned char *const probe_vq_wide[2] = {probe_vq_wide_0,probe_vq_wide_1};\n'
                     'static const unsigned char *const probe_vq_narrow[2] = {probe_vq_narrow_0,probe_vq_narrow_1};\n')
    lines.append(f'#define PROBE_ORIGIN_X {ORIGIN_X}\n#define PROBE_ORIGIN_Y {ORIGIN_Y}\n#define PROBE_CELL {CELL}\n#endif\n')
    (args.output/'palette_probe_data.h').write_text(''.join(lines))
    (args.output/'probe.json').write_text(json.dumps(report))
    for entry in report['pages']:
        for cell in entry['cells']:
            print(entry['name'], 'texture', cell['texture'], 'header', cell['header'], 'luma', cell['light'],
                  'buckets', cell['buckets'], 'vertex', cell['vertex'], 'gain model max error', cell['model_max_error'],
                  'anchor', cell['anchor'], 'modulation %08x' % cell['modulation'])
    print('vq distinct 2x2 blocks per page', report['vq_blocks'])


if __name__ == '__main__':
    main()
