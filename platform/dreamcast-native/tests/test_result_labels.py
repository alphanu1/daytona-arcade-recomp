"""Owned-ROM check: python3 test_result_labels.py ROM_CACHE.

Digests independently verified by original5454/18e2c/18414 in --check-native-race.
"""
from pathlib import Path
import struct
import sys
import re

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from prepare_result_labels import extract, pack, render_header, name_render_header


def fnv(data):
    h = 2166136261
    for byte in data:
        h = ((h ^ byte)*16777619)&0xffffffff
    return h


source = Path(sys.argv[1])
program = (source/'program.bin').read_bytes()
main = (source/'main_data.bin').read_bytes()
assets, chars, palette, metadata = extract(program, main)
assert fnv(chars) == 0xf195da14 and fnv(palette) == 0x4f6a7f5e
planes = [a for a in assets.values() if 'pens' in a]
assert len(planes) == 30 and len(metadata['ranks']) == 42
assert [name for name, a in assets.items() if 'missing_tiles' in a] == ['block-02872cae']
blob = b''.join(struct.pack(f'<{len(a["pens"])}H', *a['pens']) for a in planes)
assert len(blob) == 263296 and fnv(blob) == 0x59e1d3f5
for a in planes:
    assert len(a['pens']) == a['width']*a['height']
    assert a['raw_colors'] <= 16
    assert all(0 <= p < 4096 and (p == 0 or p&15) for p in a['pens'])
for bad_program, bad_main in ((program[:-1], main), (program, bytes(len(main)))):
    try:
        extract(bad_program, bad_main)
    except ValueError:
        pass
    else:
        raise AssertionError('unrecognized input accepted')
print('PASS 30 exact pen planes/42 ranks; unresolved penalty art retained; fingerprint rejection')
name_assets, name_chars, name_palette, name_metadata = extract(program, main, name_scene=True)
assert list(name_assets)[:4] == name_metadata['fixed'] == [
    'block-02883100', 'block-0288484c', 'block-02884a0a', 'block-02884a4a']
assert name_metadata['tilemap_destinations'] == [0x1004000, 0x100011e, 0x100150a, 0x1001542]
assert [(name_assets[n]['width'], name_assets[n]['height']) for n in name_metadata['fixed']] == [(496,384),(248,56),(104,16),(112,16)]
assert name_metadata['countdown']==['glyph-20']+[f'glyph-{i:02x}' for i in range(0x30,0x3a)]
assert name_metadata['time_glyphs']==[f'time-{i:02d}' for i in range(12)]
assert name_metadata['rank_glyphs']==[f'rank-{i:02d}' for i in (*range(10),11,12,13,14)]
for name in name_metadata['countdown']:
    assert name_assets[name]['pens']==assets[name]['pens']
assert not any(name_assets['glyph-20']['pens'])
# Independently check the four original scene-copy ranges, including untouched
# shared bytes/gaps, rather than accepting output from the new loader itself.
expected_chars = bytearray(chars)
root = struct.unpack_from('<I', main, 0x800014)[0]-0x2000000
char_list, palette_list = struct.unpack_from('<II', main, root)
for i, (destination, length) in enumerate(((0x10e0000,20352),(0x10e5000,4832),
                                        (0x10e7000,928),(0x10e8000,52896))):
    src, dst = struct.unpack_from('<II', main, char_list-0x2000000+8*i)
    assert dst == destination and struct.unpack_from('<I', main, src-0x2000000)[0]*32 == length
    expected_chars[dst-0x1080000:dst-0x1080000+length] = main[src-0x2000000+4:src-0x2000000+4+length]
assert name_chars == expected_chars
expected_palette = bytearray(palette)
at = palette_list-0x2000000
assert struct.unpack_from('<II', main, at) == (0x1800c00,336)
expected_palette[0xc00:0xea0] = main[at+8:at+680]
assert struct.unpack_from('<III', main, at+680) == (0x1801c00,0,0)  # Zero-length copy, then terminator.
assert name_palette == expected_palette
assert fnv(name_chars) == 0x40bb0775 and fnv(name_palette) == 0x178be191
name_blob = b''.join(struct.pack(f'<{len(name_assets[n]["pens"])}H', *name_assets[n]['pens']) for n in name_metadata['fixed'])
assert len(name_blob) == 415616 and fnv(name_blob) == 0xd845cabe
for a in name_assets.values():
    assert 'missing_tiles' not in a and len(a['pens']) == a['width']*a['height']
    assert a['raw_colors'] <= 16
    assert all(0 <= p < 4096 and (p == 0 or p&15) for p in a['pens'])
print('PASS four name-screen pen planes; scene-copy ranges, palette overlay/zero-length entry and source-oracle digests; RGB separate')
if len(sys.argv)>2:
    color_text=Path(sys.argv[2]).read_text()
    textures,pal,bindings,previews,checked=pack(assets,palette,color_text)
    header=render_header(assets,metadata,bindings)
    def table(name):
        body=re.search(r'\b'+name+r'\[[^;]+?=\s*\{(.*?)\};',header,re.S).group(1)
        return list(map(int,re.findall(r'\d+',body)))
    names=list(assets);spans=table('nm_result_label_spans');parts=table('nm_result_label_parts')
    assert table('nm_result_label_fixed')==[names.index(n) for n in metadata['fixed']]
    assert table('nm_result_label_ranks')==[names.index(n) if n else 65535 for row in metadata['ranks'] for n in row]
    for i,name in enumerate(names):
        first,count=spans[2*i:2*i+2]
        if name not in bindings:
            assert (first,count)==(65535,0)
        else:
            expected=[p[k] for p in bindings[name] for k in ('texture','bank','x','y','width','height','texture_width','texture_height')]
            assert parts[first*8:(first+count)*8]==expected
    assert [names[i] if i!=65535 else None for i in table('nm_result_label_glyphs')]==[
        f'glyph-{i:02x}' if f'glyph-{i:02x}' in assets else None for i in range(58)]
    colors=[int(line.split()[2]) for line in color_text.splitlines() if line and not line.startswith('#')]
    assert len(pal)==136  # Two banks,60/61; result glyph banks2..59 untouched.
    for name,parts in bindings.items():
        a=assets[name];canvas=[0]*len(a['pens'])
        for part in parts:
            tw,th=part['texture_width'],part['texture_height']
            side=min(tw,th);assert max(tw,th)<=128 and part['bank'] in (60,61)
            data=textures[part['texture']]
            assert struct.unpack_from('<HHI',data,12)[:2]==(tw,th)
            assert struct.unpack_from('<I',data,16)[0] == 0x28000000|((tw.bit_length()-4)<<3)|(th.bit_length()-4)
            for y in range(part['height']):
                for x in range(part['width']):
                    # Independent Morton address, then actual bank lookup.
                    at=(x//side+y//side)*side*side+sum(((y>>b)&1)<<(2*b) | ((x>>b)&1)<<(2*b+1) for b in range(side.bit_length()-1))
                    index=(data[32+at//2]>>((at&1)*4))&15
                    rgba=struct.unpack_from('<I',pal,8+((part['bank']-60)*16+index)*4)[0]
                    canvas[(part['y']+y)*a['width']+part['x']+x]=rgba
        assert canvas==[colors[p] if p else 0 for p in a['pens']]
    try:
        pack(assets,palette,color_text.replace('0 46602 ', '0 46603 ',1))
    except ValueError:
        pass
    else:
        raise AssertionError('mismatched raw palette accepted')
    print(f'PASS PAL4 decoded atlas placement/alpha for all30 planes; {len(textures)} textures, {checked} padded texels')
if len(sys.argv)>3:
    name_colors=Path(sys.argv[3]).read_text()
    assert name_colors.startswith('# name5454+5460 palette,')
    textures,pal,bindings,previews,checked=pack(name_assets,name_palette,name_colors)
    header=name_render_header(name_assets,name_metadata,bindings)
    body=header.split('nm_name_label_parts[]={',1)[1].split('};',1)[0]
    parts=[tuple(map(int,re.findall(r'\d+',row))) for row in body.splitlines() if row.startswith('{')]
    assert len(parts)==len(textures)==52
    expected=[]
    for i,name in enumerate(name_metadata['fixed']):
        # Original page1 background at0,0; three page0 foreground blocks.
        x,y=((0,0),(120,16),(40,336),(264,336))[i]
        for p in bindings[name]:
            expected.append((p['bank'],x+p['x'],y+p['y'],p['width'],p['height'],
                             p['texture_width'],p['texture_height'],int(i!=0)))
    assert bindings['glyph-20']==[]
    for name in name_metadata['countdown'][1:]+name_metadata['time_glyphs']+name_metadata['rank_glyphs']:
        assert len(bindings[name])==1
        p=bindings[name][0]
        expected.append((p['bank'],p['x'],p['y'],p['width'],p['height'],p['texture_width'],p['texture_height'],1))
    assert parts==expected
    first=name_metadata['fixed'][0]
    name_assets[first]['tiles'][0]^=0x8000
    try:
        name_render_header(name_assets,name_metadata,bindings)
    except ValueError:
        pass
    else:
        raise AssertionError('mixed pre/post3D plane accepted')
    finally:
        name_assets[first]['tiles'][0]^=0x8000
    colors=[int(line.split()[2]) for line in name_colors.splitlines() if line and not line.startswith('#')]
    assert set(bindings)==set(name_assets) and len(pal)<=8+4*16*4
    for name,parts in bindings.items():
        a=name_assets[name];canvas=[0]*len(a['pens'])
        for part in parts:
            tw,th=part['texture_width'],part['texture_height'];side=min(tw,th)
            assert max(tw,th)<=128 and 60<=part['bank']<=63
            data=textures[part['texture']]
            assert struct.unpack_from('<HHI',data,12)[:2]==(tw,th)
            assert struct.unpack_from('<I',data,16)[0] == 0x28000000|((tw.bit_length()-4)<<3)|(th.bit_length()-4)
            for y in range(part['height']):
                for x in range(part['width']):
                    at=(x//side+y//side)*side*side+sum(((y>>b)&1)<<(2*b) | ((x>>b)&1)<<(2*b+1) for b in range(side.bit_length()-1))
                    index=(data[32+at//2]>>((at&1)*4))&15
                    rgba=struct.unpack_from('<I',pal,8+((part['bank']-60)*16+index)*4)[0]
                    canvas[(part['y']+y)*a['width']+part['x']+x]=rgba
        assert canvas==[colors[p] if p else 0 for p in a['pens']]
    try:
        pack(name_assets,name_palette,Path(sys.argv[2]).read_text())
    except ValueError:
        pass
    else:
        raise AssertionError('result-scene palette accepted for name art')
    print(f'PASS four name PAL4 planes reconstructed exactly; {len(textures)} textures, {checked} padded texels; wrong scene palette rejected')
