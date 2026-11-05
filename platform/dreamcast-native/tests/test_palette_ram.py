"""Palette RAM format (include/render/materials/palette_ram.h, code/render/materials/palette_ram.c).

python3 platform/dreamcast-native/tests/test_palette_ram.py [ASSET_DIR [HUD_PACK]]

Host only. Builds palette_ram.c against a fake palette RAM (1024 words and the
PAL_RAM_CTRL register) for the default build (ARGB8888) and for make
NATIVE_PAL16=1555, and checks:
  - the 5- and 6-bit narrowing is the nearest value under the PVR's bit-repeat
    expansion for all 256 inputs, and decode(encode(c)) == c for every colour
    a 16-bit entry can hold;
  - nothing is written before the format register is set, and the register is
    set once;
  - with ASSET_DIR (a race asset directory: car.dnt, world.dnt, hud/hud.dp,
    finish/*.pal, result/palettes.dpal, labels/labels.dp, name-labels/labels.dp)
    and optionally HUD_PACK (tools/prepare_hud.py output, hud/hud.dp): every
    palette the program writes, through the program's own write functions,
    lands in palette RAM as exactly the conversion of its ARGB8888 source
    colour (reference conversion below, written independently of the C code);
    every source alpha is 0 or 255, so the 1-bit alpha keeps punch-through
    coverage exactly; a 16-bit entry has zero high bits.
Prints, per palette source, the largest and mean channel change of the
conversion (ARGB8888 -> ARGB1555 -> what the PVR reads).
"""
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile

native = Path(__file__).resolve().parents[1]


def expand5(q):
    return (q << 3) | (q >> 2)


def expand6(q):
    return (q << 2) | (q >> 4)


def nearest(v, bits):
    """Reference: the q whose bit-repeat expansion is nearest v, lower on a tie."""
    expand = expand5 if bits == 5 else expand6
    return min(range(1 << bits), key=lambda q: (abs(expand(q)-v), q))


def encode1555(argb):
    return (argb >> 31) << 15 | nearest((argb >> 16) & 255, 5) << 10 | nearest((argb >> 8) & 255, 5) << 5 | nearest(argb & 255, 5)


def decode1555(word):
    return (0xff000000 if word & 0x8000 else 0) | expand5((word >> 10) & 31) << 16 | expand5((word >> 5) & 31) << 8 | expand5(word & 31)


def dnt6_palettes(path):
    data = path.read_bytes()
    magic, size, resources, count, _, _, palettes, _ = struct.unpack_from('<4s7I', data)
    assert magic == b'DNT6' and size == len(data), path
    table = 32+16*resources+20*count+((count+3) & ~3)
    return list(struct.unpack_from('<%dI' % (16*palettes), data, table))


def dpal(path):
    data = path.read_bytes()
    magic, colours = struct.unpack_from('<4sI', data)
    assert magic == b'DPAL' and len(data) == 8+4*colours, path
    return list(struct.unpack_from('<%dI' % colours, data, 8))


HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "render/materials/palette_ram.c"
uint32_t host_palette_ram[1025];
unsigned host_format_writes;
int main(int argc,char **argv) {
    /* Format register first: a write before it is refused. */
    static const uint32_t probe[1]={0xff102030u};
    host_palette_ram[1024]=99;
    assert(!nm_palette_ram_write(0,probe,1,NULL) && !host_palette_ram[0]);
    assert(nm_palette_ram_init() && host_palette_ram[1024]==NM_PALETTE_FORMAT && host_format_writes==1);
    /* Narrowing against the PVR's expansion, all inputs. */
    for(unsigned v=0;v<256;++v) for(unsigned bits=5;bits<=6;++bits) {
        const unsigned q=nm_palette_narrow(v,bits),top=(1u<<bits)-1;
        const int best=(int)(bits==5 ? nm_palette_expand5(q) : nm_palette_expand6(q))-(int)v;
        for(unsigned k=0;k<=top;++k) {
            const int other=(int)(bits==5 ? nm_palette_expand5(k) : nm_palette_expand6(k))-(int)v;
            assert(abs(best)<abs(other) || (abs(best)==abs(other) && q<=k));
        }
    }
    /* Every 16-bit colour survives decode then encode. */
    for(uint32_t w=0;w<65536;++w) {
        assert(nm_palette_encode_as(nm_palette_decode_as(w,NM_PALETTE_ARGB1555),NM_PALETTE_ARGB1555)==w);
        assert(nm_palette_encode_as(nm_palette_decode_as(w,NM_PALETTE_RGB565),NM_PALETTE_RGB565)==w);
    }
    /* Items: "dnt FIRST COUNT HEXFILE" (colours as hex words, one per line,
     * through nm_palette_ram_write) or "dpal FIRST PATH" (a DPAL file through
     * nm_palette_ram_write_dpal_file). Then all 1024 entries as hex. */
    for(int a=1;a+2<argc;a+=3) {
        const unsigned first=(unsigned)atoi(argv[a+1]);
        if(!strcmp(argv[a],"dpal")) { assert(nm_palette_ram_write_dpal_file(argv[a+2],first,argv[a+2])); continue; }
        assert(!strcmp(argv[a],"words"));
        FILE *in=fopen(argv[a+2],"r"); assert(in);
        static uint32_t colours[1024]; unsigned count=0; unsigned long word;
        while(count<1024 && fscanf(in,"%lx",&word)==1) colours[count++]=(uint32_t)word;
        fclose(in);
        assert(nm_palette_ram_write(first,colours,count,argv[a+2]));
    }
    assert(host_format_writes==1 && host_palette_ram[1024]==NM_PALETTE_FORMAT);
    for(unsigned i=0;i<1024;++i) printf("%08lx\n",(unsigned long)host_palette_ram[i]);
    return 0;
}
'''

FAKE_PVR = '''#pragma once
#include <stdint.h>
typedef enum { PVR_PAL_ARGB1555, PVR_PAL_RGB565, PVR_PAL_ARGB4444, PVR_PAL_ARGB8888 } pvr_palfmt_t;
#define PVR_PALETTE_TABLE_BASE 0
#define PVR_PALETTE_CFG 4096
extern uint32_t host_palette_ram[1025];
extern unsigned host_format_writes;
static inline void pvr_set_pal_format(pvr_palfmt_t format) { host_palette_ram[1024]=format; ++host_format_writes; }
static inline void pvr_set_pal_entry(unsigned index,uint32_t value) { host_palette_ram[index]=value; }
#define PVR_GET(address) (host_palette_ram[(address)/4])
'''


def sources(assets, hud):
    """(name, first entry, ARGB8888 colours, how the program writes them)."""
    out = []
    if assets is None:
        return out
    car = dnt6_palettes(assets/'car.dnt')
    world = dnt6_palettes(assets/'world.dnt')
    out.append(('car.dnt', 32, car, 'words'))
    # The world pack's palettes after the car's (the loader shares equal ones; the
    # conversion of each colour is what is checked here).
    out.append(('world.dnt', 32, world, 'words'))
    for name, first in (('finish/texture_1.dt.pal', 0), ('finish/texture_2.dt.pal', 16), ('hud/hud.dp', 992),
                        ('result/palettes.dpal', 32), ('labels/labels.dp', 960), ('name-labels/labels.dp', 960)):
        out.append((name, first, dpal(assets/name), assets/name))
    if hud is not None:
        out.append(('HUD pack hud/hud.dp', 992, dpal(hud/'hud/hud.dp'), hud/'hud/hud.dp'))
    return out


def main():
    assets = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    hud = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    with tempfile.TemporaryDirectory(prefix='native-palette-ram-') as temp:
        root = Path(temp)
        (root/'enDjinn').mkdir()
        (root/'enDjinn/enj_enDjinn.h').write_text(FAKE_PVR)
        (root/'harness.c').write_text(HARNESS)
        items = sources(assets, hud)
        for defines, name in (([], 'ARGB8888'), (['-DNM_PAL16=1555'], 'ARGB1555')):
            binary = root/('harness-'+name)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', *defines, '-I'+str(root), '-I'+str(native/'include'),
                            '-I'+str(native/'code'), str(root/'harness.c'), '-o', str(binary)], check=True)
            for label, first, colours, how in items:
                if how == 'words':
                    listing = root/(label.replace('/', '_')+'.txt')
                    listing.write_text(''.join('%08x\n' % c for c in colours[:1024-first]))
                    arguments = ['words', str(first), str(listing)]
                    colours = colours[:1024-first]
                else:
                    arguments = ['dpal', str(first), str(how)]
                run = subprocess.run([str(binary), *arguments], capture_output=True, text=True, check=True)
                ram = [int(w, 16) for w in run.stdout.split()[-1024:]]
                assert all((c >> 24) in (0, 255) for c in colours), f'{label}: an alpha other than 0 or 255'
                for i, c in enumerate(colours):
                    want = c if name == 'ARGB8888' else encode1555(c)
                    assert ram[first+i] == want, f'{name} {label} entry {first+i}: {ram[first+i]:08x} != {want:08x}'
                    if name == 'ARGB1555':
                        assert ram[first+i] >> 16 == 0
                        # Punch-through coverage: the alpha bit is the source's alpha test.
                        assert bool(ram[first+i] & 0x8000) == ((c >> 24) >= 128)
                if name == 'ARGB1555':
                    shown = [decode1555(encode1555(c)) for c in colours]
                    change = [abs(((a >> s) & 255)-((b >> s) & 255)) for a, b in zip(colours, shown) for s in (16, 8, 0)]
                    print(f'  {label:26s} {len(colours):4d} entries at {first:4d}: ARGB1555 as the PVR reads it, '
                          f'channel change max {max(change)} mean {sum(change)/len(change):.2f}; alpha kept')
            print(f'PASS: palette RAM {name}: format set once before any entry; nearest 5/6-bit narrowing under the '
                  f'bit-repeat expansion (all 256 inputs); 16-bit round trip (all 65,536 words); '
                  f'{len(items)} palette sources written through the program\'s functions equal their reference conversion')
    if assets is None:
        print('(no asset directory: the palette sources of a race were not checked)')


if __name__ == '__main__':
    main()
