"""NATIVE_DIAG_LATE_CAPTURE (code/late_capture.c) on the host, ASan/UBSan: the copy from a synthetic 640x480 packed
24-bit frame buffer (random pixels, B,G,R bytes as PM_RGB888P stores them) against a numpy reference of the 2x2 box
and RGB565 conversion, and byte for byte at full resolution; the slot policy (first N kept, then the N worst while
slots allow, "before" pictures only in free slots and given up to late frames); the file (header, name tables,
records, pictures) and its index read by tools/late_capture_report.py, which must make the PNGs and contact sheet.
Needs numpy and Pillow (the report tool's own dependencies)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

try:
    import numpy as np
    import PIL  # noqa: F401  (the report tool's)
except ImportError:
    print('SKIP: numpy or Pillow is not installed for this interpreter')
    sys.exit(77)

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root/'tools'))
import late_capture_report as report

harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "late_capture.h"
enum { FB=0x2000 };
int main(int argc,char **argv) {
    if(argc<3) return 2;
    FILE *in=fopen(argv[1],"rb"); if(!in) return 2;
    uint8_t *vram=calloc(1,FB+640*480*3);
    if(fread(vram+FB,1,640*480*3,in)!=640*480*3) return 2;
    fclose(in);
    nm_late_host_vram=vram;
    if(!nm_late_reserve()) return 3;
    /* Policy (4 slots, first 2): late frames 10 (score 5), 20 (3), 30 (9), 40 (1), 50 (7), 60 (8), 70 (10). */
    int s10=nm_late_take_late(10,5),s20=nm_late_take_late(20,3);   /* the first 2 */
    int b29=nm_late_take_before(29,30);       /* a free slot */
    int s30=nm_late_take_late(30,9);          /* among the 2 worst (9, 5): the last free slot */
    int b39=nm_late_take_before(39,40);       /* no free slot: none */
    int s40=nm_late_take_late(40,1);          /* not among the 2 worst: not kept */
    int s50=nm_late_take_late(50,7);          /* among the 2 worst (9, 7): takes the "before" picture's slot */
    int s60=nm_late_take_late(60,8);          /* among (9, 8): evicts the least bad beyond the first 2, 50 */
    int s70=nm_late_take_late(70,10);         /* among (10, 9): evicts 60 */
    printf("slots %d %d %d %d %d %d %d %d %d\n",s10,s20,b29,s30,b39,s40,s50,s60,s70);
    int failures=0;
    failures+=s10<0 || s20<0 || b29<0 || s30<0 || b39>=0 || s40>=0 || s50!=b29 || s60!=s50 || s70!=s60;
    failures+=nm_late_find(29,1)>=0 || nm_late_find(50,0)>=0 || nm_late_find(60,0)>=0 || nm_late_evicted()!=3;
    failures+=nm_late_record(s10)->kind!=NM_LATE_FIRST || nm_late_record(s70)->kind!=NM_LATE_WORST;
    /* Copy the frame into every kept slot, then write. */
    for(int s=0;s<NM_LATE_CAPTURE_SLOTS;++s) if(nm_late_record(s)->kind!=NM_LATE_FREE) {
        nm_late_record(s)->tick=1000+nm_late_record(s)->seq;nm_late_record(s)->vbl=2;nm_late_record(s)->cause=1;
        nm_late_copy(s,FB);
    }
    static const char *const kinds[]={"none","first","worst","before"},*const phases[]={"other","start","race"},
        *const scenes[]={"race"},*const causes[]={"pvr","cpu-update"},*const sources[]={"front","back"};
    const nm_late_names_t names[5]={{kinds,4},{phases,3},{scenes,1},{causes,2},{sources,2}};
    nm_late_header_t header={0};header.late=7;header.eligible=7;
    if(!nm_late_write(argv[2],"lc",&header,names)) ++failures;
    printf("failures %d evicted %u\n",failures,(unsigned)nm_late_evicted());
    free(vram);
    return failures ? 1 : 0;
}
'''

rng = np.random.default_rng(1014)
frame = rng.integers(0, 256, (480, 640, 3), dtype=np.uint8)          # B, G, R per pixel
box = frame.reshape(240, 2, 320, 2, 3).astype(np.uint32).sum((1, 3))
mean = (box+2) >> 2
reference = ((mean[:, :, 2] >> 3) << 11) | ((mean[:, :, 1] >> 2) << 5) | (mean[:, :, 0] >> 3)

with tempfile.TemporaryDirectory(prefix='late-capture-') as tmp:
    tmp = Path(tmp)
    (tmp/'frame.bgr').write_bytes(frame.tobytes())
    (tmp/'check.c').write_text(harness)
    for label, defines in (('320x240', ()), ('full', ('-DNM_LATE_CAPTURE_FULL_RES=1',))):
        binary = tmp/('check-'+label)
        subprocess.run([os.environ.get('HOST_CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1',
                        '-fsanitize=address,undefined', '-I', str(root/'include'), '-DNM_DIAG_LATE_CAPTURE=2',
                        '-DNM_LATE_CAPTURE_FIRST=2', '-DNM_LATE_CAPTURE_SLOTS=4', *defines, str(tmp/'check.c'),
                        str(root/'code/late_capture.c'), '-o', str(binary)], check=True)
        out_dir = tmp/label
        out_dir.mkdir()
        run = subprocess.run([str(binary), str(tmp/'frame.bgr'), str(out_dir)+'/'], capture_output=True, text=True)
        assert run.returncode == 0, run.stdout+run.stderr
        header, names, records, pictures = report.parse(out_dir/'lc.bin')
        assert [r['seq'] for r in records] == [10, 20, 30, 70], records
        assert [r['kind_name'] for r in records] == ['first', 'first', 'worst', 'worst'], records
        assert header['images'] == 4 and header['first_n'] == 2 and header['slots'] == 4 and header['evicted'] == 3, header
        assert names['causes'] == ['pvr', 'cpu-update'] and records[0]['cause_name'] == 'cpu-update', names
        for r, raw in zip(records, pictures):
            if label == 'full':
                assert raw == frame.tobytes(), 'full-resolution copy differs'
                assert (report.to_rgb(header, raw) == frame[:, :, ::-1]).all()
            else:
                got = np.frombuffer(raw, '<u2').reshape(240, 320)
                assert (got == reference).all(), 'downscale differs at %d pixels' % int((got != reference).sum())
                rgb = report.to_rgb(header, raw)
                assert rgb.shape == (240, 320, 3) and (rgb[:, :, 0] >> 3 == reference >> 11).all()
        csv_rows = (out_dir/'lc.csv').read_text().splitlines()
        assert len(csv_rows) == 5 and csv_rows[0].startswith('image,kind,seq') and ',worst,70,' in csv_rows[4], csv_rows
        offset = int(csv_rows[1].rsplit(',', 1)[1])
        assert (out_dir/'lc.bin').read_bytes()[offset:offset+header['image_bytes']] == pictures[0]
        made = subprocess.run([sys.executable, str(root/'tools/late_capture_report.py'), str(out_dir/'lc.bin'), '--out',
                               str(out_dir/'png')], capture_output=True, text=True, check=True).stdout
        from PIL import Image
        pngs = sorted((out_dir/'png').glob('0*.png'))
        assert len(pngs) == 4 and Image.open(pngs[0]).size[0] == 640 and Image.open(pngs[0]).size[1] > 480, made
        assert Image.open(out_dir/'png'/'contact.png').size[0] > 640
print('PASS: late capture: 2x2 box to RGB565 equal to the numpy reference, full resolution byte for byte; slots: first 2 '
      'kept, worst beyond them, "before" pictures given up; file, CSV offsets and the report tool\'s PNGs (ASan/UBSan)')
