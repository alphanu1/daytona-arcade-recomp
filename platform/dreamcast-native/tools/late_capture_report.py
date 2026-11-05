#!/usr/bin/env python3
"""Pictures of a NATIVE_DIAG_LATE_CAPTURE run (include/late_capture.h, code/practice.c): one PNG per captured
frame with a caption bar, a contact sheet, an index (JSON and CSV), optionally beside the stream simulator's
picture of the same race tick.

usage: late_capture_report.py CAPTURE.bin [--out DIR] [--sim DIR] [--log CONSOLE.log]
  CAPTURE.bin  what the console wrote to the asset root (default name late-capture.bin; the .csv beside it is the
               same index as text and is not needed)
  --out DIR    output directory (default: CAPTURE without .bin, plus "-png")
  --sim DIR    the simulator's pictures, TICK.ppm or TICK.png (640x480; build them with the late-capture build
               directory's sim_pictures.sh): shown beside the console's picture of a race frame of that tick, with
               the PVR work model's figures of that frame from DIR/pvr.txt if present
  --log FILE   the console log: its LATE-CAPTURE and DROP-SUMMARY lines go into index.json

Output: NNN-KIND-seqS-tickT.png (the picture at 640x480 with its caption), contact.png (one row per late frame:
the frame shown before it, the late frame, the simulator's picture), index.json, index.csv.
Caption: kind (first: one of the first N late frames; worst: kept as one of the N worst after them; before: the
frame shown just before late frame PAIR), scene sequence number, run tick (simulation ticks of the whole run),
race tick, phase/scene, vblanks of its display interval, cause; updater us x simulation ticks, wait, registration,
PVR render, registration end after the previous flip, gap before the updater, previous render; camera mode/
transition; copy time, the scene whose registration held the copy, time from the frame's flip to the copy, buffer
(front: on display; back), TORN if the buffer may have been overwritten during the copy.
"""
import argparse
import csv
import json
import re
import struct
from pathlib import Path

HEADER = struct.Struct('<32I')
HEADER_FIELDS = ('magic', 'version', 'width', 'height', 'format', 'image_bytes', 'slots', 'first_n', 'images', 'names_bytes',
                 'period_ns', 'late', 'eligible', 'not_kept', 'missed', 'copies', 'copy_us_total', 'copy_us_max', 'affected',
                 'torn', 'before_images', 'run_ticks', 'evicted')
RECORD = struct.Struct('<14Ii2Ii10I4I')
RECORD_FIELDS = ('kind', 'seq', 'pair_seq', 'run', 'tick', 'phase', 'scene', 'vbl', 'cause', 'upd_us', 'ticks', 'wait_us',
                 'reg_us', 'rnd_us', 'late_us', 'other_us', 'prev_rnd_us', 'camera_mode', 'camera_transition', 'copy_us',
                 'copy_scene', 'since_flip_us', 'source', 'flags', 'fb_addr', 'score', 'image', 'flip_vbl')
MAGIC = 0x434c4d4e
RGB565, BGR888 = 1, 2
TORN, UNTIMED, COPIED = 1, 2, 4
NAME_GROUPS = ('kinds', 'phases', 'scenes', 'causes', 'sources')


def parse(path):
    """The capture file: header dict, name tables, records (dicts with names resolved) and raw pictures."""
    data = Path(path).read_bytes()
    words = HEADER.unpack_from(data, 0)
    header = dict(zip(HEADER_FIELDS, words))
    if header['magic'] != MAGIC or header['version'] != 1:
        raise ValueError('%s: not a late-capture file (magic %08x version %u)' % (path, header['magic'], header['version']))
    at = HEADER.size
    strings = data[at:at + header['names_bytes']].split(b'\0')
    names, group = {}, []
    for text in strings:
        if len(names) == len(NAME_GROUPS):
            break
        if text:
            group.append(text.decode())
        else:
            names[NAME_GROUPS[len(names)]] = group
            group = []
    at += header['names_bytes']
    records = []
    for i in range(header['images']):
        record = dict(zip(RECORD_FIELDS, RECORD.unpack_from(data, at + i * RECORD.size)))
        for field, group in (('kind', 'kinds'), ('phase', 'phases'), ('scene', 'scenes'), ('cause', 'causes'), ('source', 'sources')):
            table = names.get(group, [])
            record[field + '_name'] = table[record[field]] if record[field] < len(table) else str(record[field])
        record['torn'] = bool(record['flags'] & TORN)
        record['untimed'] = bool(record['flags'] & UNTIMED)
        records.append(record)
    at += header['images'] * RECORD.size
    size = header['image_bytes']
    pictures = [data[at + i * size:at + (i + 1) * size] for i in range(header['images'])]
    if any(len(p) != size for p in pictures):
        raise ValueError('%s: truncated (%u pictures of %u bytes expected)' % (path, header['images'], size))
    return header, names, records, pictures


def to_rgb(header, raw):
    """A picture as an RGB numpy array (height, width, 3)."""
    import numpy as np
    w, h = header['width'], header['height']
    if header['format'] == RGB565:
        p = np.frombuffer(raw, '<u2').reshape(h, w).astype(np.uint32)
        r, g, b = p >> 11, (p >> 5) & 63, p & 31
        return np.stack(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)), -1).astype(np.uint8)
    if header['format'] == BGR888:
        return np.frombuffer(raw, np.uint8).reshape(h, w, 3)[:, :, ::-1].copy()
    raise ValueError('unknown picture format %u' % header['format'])


def caption(r):
    head = '#%u %s seq %u%s  run %u  tick %u  %s/%s  vbl %u  %s' % (
        r['image'], r['kind_name'], r['seq'], ' (before late %u)' % r['pair_seq'] if r['kind_name'] == 'before' else '',
        r['run'], r['tick'], r['phase_name'], r['scene_name'], r['vbl'], r['cause_name'])
    times = 'upd %u us x%u  wait %u  reg %u  rnd %u  late %d  other %u  prev rnd %u' % (
        r['upd_us'], r['ticks'], r['wait_us'], r['reg_us'], r['rnd_us'], r['late_us'], r['other_us'], r['prev_rnd_us'])
    copy = 'camera %d/%u  copy %u us in scene %u, %u us after its flip, %s buffer%s%s' % (
        r['camera_mode'], r['camera_transition'], r['copy_us'], r['copy_scene'], r['since_flip_us'], r['source_name'],
        ', TORN' if r['torn'] else '', ', events incomplete' if r['untimed'] else '')
    return [head, times, copy]


def font(size):
    from PIL import ImageFont
    try:
        return ImageFont.load_default(size=size)
    except TypeError:   # Pillow before 10.1
        return ImageFont.load_default()


def wrap(lines, f, width):
    """Caption lines broken at their double spaces (and ", ") to fit the picture's width."""
    out = []
    for line in lines:
        parts = [p for p in re.split(r'(?<=,) |  ', line) if p]
        current = ''
        for part in parts:
            joined = current + ('  ' if current else '') + part
            if current and f.getlength(joined) > width - 8:
                out.append(current)
                current = part
            else:
                current = joined
        out.append(current)
    return out


def with_caption(image, lines, size):
    from PIL import Image, ImageDraw
    f = font(size)
    lines = wrap(lines, f, image.width)
    line_h = size + 4
    out = Image.new('RGB', (image.width, image.height + line_h * len(lines) + 6), (16, 16, 16))
    out.paste(image, (0, 0))
    draw = ImageDraw.Draw(out)
    for i, text in enumerate(lines):
        draw.text((4, image.height + 3 + i * line_h), text, fill=(235, 235, 235), font=f)
    return out


def sim_picture(directory, tick):
    from PIL import Image
    if not directory:
        return None
    for suffix in ('.ppm', '.png'):
        path = Path(directory) / ('%u%s' % (tick, suffix))
        if path.exists():
            return Image.open(path).convert('RGB')
    return None


def sim_work(directory):
    """The simulator's PVR work model lines (DIR/pvr.txt, "P tick=T key=value ..."), by tick."""
    path = Path(directory) / 'pvr.txt' if directory else None
    if not path or not path.exists():
        return {}
    out = {}
    for line in path.read_text().splitlines():
        if line.startswith('P tick='):
            row = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', line)}
            out[row['tick']] = row
    return out


def log_lines(path):
    if not path:
        return {}
    text = Path(path).read_text(errors='replace')
    out = {}
    for key in ('LATE-CAPTURE v1:', 'DROP-SUMMARY ', 'DROP-LOG '):
        lines = [l.strip() for l in text.splitlines() if l.startswith(key)]
        if lines:
            out[key.strip(' :')] = dict(re.findall(r'(\w[\w-]*)=(-?[\w./-]+)', lines[-1]))
    return out


def main():
    from PIL import Image, ImageDraw
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('capture', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--sim', type=Path)
    parser.add_argument('--log', type=Path)
    args = parser.parse_args()
    header, names, records, pictures = parse(args.capture)
    out = args.out or args.capture.with_name(args.capture.stem + '-png')
    out.mkdir(parents=True, exist_ok=True)
    full = {}
    for r, raw in zip(records, pictures):
        image = Image.fromarray(to_rgb(header, raw))
        if image.size != (640, 480):
            image = image.resize((640, 480), Image.NEAREST)
        full[r['image']] = image
        name = '%03u-%s-seq%u-tick%u.png' % (r['image'], r['kind_name'], r['seq'], r['tick'])
        r['png'] = name
        with_caption(image, caption(r), 14).save(out / name)
    # Contact sheet: one row per late picture (before | late | simulator), in run order.
    tile = (320, 240)
    late = [r for r in records if r['kind_name'] != 'before']
    before = {r['pair_seq']: r for r in records if r['kind_name'] == 'before'}
    columns = 3 if args.sim else 2
    work = sim_work(args.sim)
    cells = []
    for r in late:
        row = []
        b = before.get(r['seq'])
        row.append(with_caption(full[b['image']].resize(tile, Image.BILINEAR), [l for l in caption(b)], 10) if b else None)
        row.append(with_caption(full[r['image']].resize(tile, Image.BILINEAR), caption(r), 10))
        if args.sim:
            s = sim_picture(args.sim, r['tick']) if r['scene_name'] == 'race' else None
            r['sim'] = bool(s)
            lines = ['simulator (PVR model picture), race tick %u' % r['tick']]
            model = work.get(r['tick'])
            if model:
                r['sim_work'] = model
                lines.append('model: opaque tris %u tritiles %u px %u  punch-through tris %u tritiles %u tripasses %u tsp %u  '
                             'modifier tritiles %u' % tuple(model.get(k, 0) for k in (
                                 'op_tris', 'op_tritiles', 'op_px', 'pt_tris', 'pt_tritiles', 'pt_tripasses', 'pt_tsp', 'mod_tritiles')))
            row.append(with_caption(s.resize(tile, Image.BILINEAR), lines, 10) if s else None)
        cells.append(row)
    cell_h = max((c.height for row in cells for c in row if c), default=tile[1])
    title_h = 40
    sheet = Image.new('RGB', (columns * (tile[0] + 6) + 6, title_h + max(len(cells), 1) * (cell_h + 6)), (40, 40, 40))
    draw = ImageDraw.Draw(sheet)
    title = ['%s: %u pictures (%u before), %u late frames, %u eligible, %u not kept, %u missed' % (
                 args.capture.name, header['images'], header['before_images'], header['late'], header['eligible'],
                 header['not_kept'], header['missed']),
             'copies %u, mean %u us, max %u us; %u late frames caused by copies; columns: shown before | late frame%s' % (
                 header['copies'], header['copy_us_total'] // max(header['copies'], 1), header['copy_us_max'], header['affected'],
                 ' | simulator, same race tick' if args.sim else '')]
    for i, text in enumerate(title):
        draw.text((6, 4 + 16 * i), text, fill=(255, 255, 255) if i == 0 else (200, 200, 200), font=font(12))
    for y, row in enumerate(cells):
        for x, cell in enumerate(row):
            if cell:
                sheet.paste(cell, (6 + x * (tile[0] + 6), title_h + y * (cell_h + 6)))
    sheet.save(out / 'contact.png')
    plain = [{k: v for k, v in r.items()} for r in records]
    (out / 'index.json').write_text(json.dumps({'file': str(args.capture), 'header': header, 'names': names,
                                                'console': log_lines(args.log), 'pictures': plain}, indent=1))
    with open(out / 'index.csv', 'w', newline='') as f:
        fields = ['image', 'png', 'kind_name', 'seq', 'pair_seq', 'run', 'tick', 'phase_name', 'scene_name', 'vbl', 'cause_name',
                  'upd_us', 'ticks', 'wait_us', 'reg_us', 'rnd_us', 'late_us', 'other_us', 'prev_rnd_us', 'camera_mode',
                  'camera_transition', 'copy_us', 'copy_scene', 'since_flip_us', 'source_name', 'torn', 'untimed', 'flip_vbl', 'score']
        writer = csv.DictWriter(f, fieldnames=fields, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(plain)
    print('%u pictures (%u late, %u before) -> %s (contact.png, index.json, index.csv)' % (
        len(records), len(late), len(records) - len(late), out))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
