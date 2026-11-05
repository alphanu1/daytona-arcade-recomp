"""Run the actual native loader against a bounded fake VRAM allocator.

Tests ownership/upload policy, NOT PVR timing, rasterization or hardware fit.
"""
from pathlib import Path
import json
import os
import struct
import subprocess
import sys
import tempfile

code = Path(__file__).resolve().parents[1] / 'code'
with tempfile.TemporaryDirectory(prefix='native-residency-') as temp:
    root = Path(temp)
    (root/'enDjinn').mkdir()
    (root/'enDjinn/enj_texture.h').write_text('''#pragma once
#include <stddef.h>
#include <stdint.h>
typedef void *pvr_ptr_t;
typedef struct { uint8_t prefix[16]; uint32_t pvr_type; uint8_t tail[12]; } host_dt;
typedef struct {
    host_dt hdr; uint32_t pvrformat; unsigned width,height;
    struct { unsigned compressed,mipmapped,twiddled; } flags; pvr_ptr_t ptr;
} enj_texture_info_t;
#define fDtIsCompressed(h) (!!((h)->pvr_type & (1u<<30)))
#define fDtIsMipmapped(h) (!!((h)->pvr_type & (1u<<31)))
size_t pvr_mem_available(void);
void *pvr_mem_malloc(size_t);
void pvr_mem_free(void *);
void pvr_txr_load(const void *,void *,size_t);
int pvr_wait_ready(void);
int pvr_wait_render_done(void);
/* Palette RAM stand-in: materials.c writes DNT6 car palettes at load
 * (palette_ram.c); word 1024 is the format register PAL_RAM_CTRL. */
typedef enum { PVR_PAL_ARGB1555, PVR_PAL_RGB565, PVR_PAL_ARGB4444, PVR_PAL_ARGB8888 } pvr_palfmt_t;
#define PVR_PALETTE_TABLE_BASE 0
#define PVR_PALETTE_CFG 4096
extern uint32_t host_palette_ram[1025];
static inline void pvr_set_pal_format(pvr_palfmt_t format) { host_palette_ram[1024]=format; }
static inline void pvr_set_pal_entry(unsigned index,uint32_t value) { host_palette_ram[index]=value; }
#define PVR_GET(address) (host_palette_ram[(address)/4])
uint64_t timer_ns_gettime64(void);
void enj_state_flag_shutdown(void *);
''')
    (root/'enDjinn/enj_enDjinn.h').write_text('#include "enj_texture.h"\n')
    (root/'malloc.h').write_text('''#include <stdlib.h>
static inline void *memalign(size_t alignment,size_t bytes) {
    return aligned_alloc(alignment,(bytes+alignment-1)&~(alignment-1));
}
''')
    # Two different binding keys sharing no payload: exact budget is128 bytes,
    # including the measured32-byte allocation overhead for each32-byte blob.
    blob = bytearray(64); blob[:4] = b'DcTx'
    struct.pack_into('<I',blob,4,64)
    struct.pack_into('<HHI',blob,12,8,8,0x48000000)
    struct.pack_into('<4H',blob,32,0xf800,0xf800,0xf800,0xf800)
    blob[40:56] = bytes([255])*16
    pack = bytearray(struct.pack('<4s7I',b'DNT2',224,2,2,64,64,0,0))
    for offset in (96,160): pack += struct.pack('<IIHHI',offset,64,8,8,0)
    for i in range(2): pack += struct.pack('<6HI',0x4000,0,0,i,10,i,0)
    pack += blob*2
    struct.pack_into('<4H',pack,192,0x001f,0x001f,0x001f,0x001f)
    fixture = root/'fixture.dnt'; fixture.write_bytes(pack)
    # DNT6 (palette-shaded scenery): two 8x8 PAL4 textures, two bindings on
    # two palettes (entry 15 transparent: shared opaque/punch-through lists).
    texel = struct.pack('<4sIBBBBHHI12x',b'DcTx',64,0,0,0,15,8,8,0x28000000)
    palettes = [[0xff000000|(0x101010*c+0x010203*p) for c in range(15)]+[0x00f0f0f0] for p in range(2)]
    meta = b''.join(struct.pack('<IIHHI',0,64,8,8,0) for _ in range(2))
    meta += b''.join(struct.pack('<6HI',0x4000,0,0,i,10,i,0) for i in range(2))
    meta += struct.pack('<2I',0xff808080,0xff404040)+bytes([0,1,0,0])
    meta += b''.join(struct.pack('<16I',*row) for row in palettes)+bytes([0,0,0,0x80,1,1,1,0x80])
    start = (32+len(meta)+31)&~31
    meta = bytearray(meta)
    for i in range(2): struct.pack_into('<I',meta,16*i,start+64*i)
    palette_pack = struct.pack('<4s7I',b'DNT6',start+128,2,2,64,64,2,3<<8)+bytes(meta)+bytes(start-32-len(meta))
    palette_pack += texel+bytes(range(32))+texel+bytes(range(32,64))
    palette_fixture = root/'palette.dnt'; palette_fixture.write_bytes(palette_pack)
    program = '#define PALETTE_FIXTURE ' + json.dumps(str(palette_fixture)) + '\n' \
        '#include ' + json.dumps(str(code/'render/materials/materials.c')) + '\n' + \
        '#include ' + json.dumps(str(code/'render/materials/palette_ram.c')) + '\n' + r'''
#include <assert.h>
static size_t capacity,vram_used;
static unsigned allocations,uploads,releases,attempts,fail_at;
static int ready_failure;
size_t pvr_mem_available(void) { return capacity-vram_used; }
void *pvr_mem_malloc(size_t bytes) {
    ++attempts;
    if (attempts==fail_at || bytes+32>capacity-vram_used) return NULL;
    uint8_t *p=malloc(bytes+32); assert(p);
    memcpy(p,&bytes,sizeof(bytes)); vram_used+=bytes+32; ++allocations;
    return p+32;
}
void pvr_mem_free(void *ptr) {
    assert(ptr); uint8_t *p=(uint8_t *)ptr-32; size_t bytes;
    memcpy(&bytes,p,sizeof(bytes)); assert(vram_used>=bytes+32);
    vram_used-=bytes+32; ++releases; free(p);
}
void pvr_txr_load(const void *from,void *to,size_t bytes) {
    memcpy(to,from,bytes); ++uploads;
}
int pvr_wait_ready(void) { return ready_failure ? -1 : 0; }
int pvr_wait_render_done(void) { return 0; }
uint32_t host_palette_ram[1025];
uint64_t timer_ns_gettime64(void) { static uint64_t t; return ++t; }
void enj_state_flag_shutdown(void *unused) { (void)unused; assert(0); }
static void reset(size_t budget) {
    assert(!vram_used && !world.pack.data);
    capacity=512*1024+budget;
    allocations=uploads=releases=attempts=fail_at=0; ready_failure=0;
}
/* DNT6 world pack: palettes at load into the banks it shares with the cars,
 * then one VRAM block holding only the textures the renderer names. */
static void palettized(const char *path) {
    memset(host_palette_ram,0,1024*sizeof(*host_palette_ram));
    reset(64); assert(nm_world_materials_load(path));
    assert(nm_world_materials_palettized() && !world.resident && !allocations && !uploads);
    /* No car pack: the two palettes take the first two scene banks. */
    /* In the build's palette format: the source colour, or its 16-bit entry. */
    for(unsigned i=0;i<32;++i) assert(host_palette_ram[16*NM_CAR_PALETTE_BANK_FIRST+i]==nm_palette_encode(world.pack.palettes[i]));
    assert(host_palette_ram[1024]==NM_PALETTE_FORMAT);
    assert(!host_palette_ram[16*NM_CAR_PALETTE_BANK_FIRST-1] && !host_palette_ram[16*(NM_CAR_PALETTE_BANK_FIRST+2)]);
    assert(nm_world_materials_palette_bank(world.pack.bindings)==(int)NM_CAR_PALETTE_BANK_FIRST);
    assert(nm_world_materials_palette_bank(world.pack.bindings+1)==(int)NM_CAR_PALETTE_BANK_FIRST+1);
    assert(nm_world_materials_palette_bank(world.pack.bindings+2)==-1);
    assert(!nm_world_materials_texture(0) && !nm_world_materials_texture(1));
    const uint8_t used[2]={0,1};
    assert(nm_world_materials_upload(used) && world.resident && allocations==1 && uploads==1 && vram_used==64);
    assert(!nm_world_materials_upload(used) && allocations==1);       /* Once per load. */
    const enj_texture_info_t *texture=nm_world_materials_texture(1);
    assert(!nm_world_materials_texture(0) && texture && texture->width==8 && texture->height==8);
    for(unsigned i=0;i<32;++i) assert(((const uint8_t *)texture->ptr)[i]==32+i);
    nm_world_materials_unload(); nm_world_materials_unload();
    assert(!vram_used && releases==1 && !world.pack.data && !nm_world_materials_palettized());
    /* Every texture, and a budget one texture short. */
    reset(96); assert(nm_world_materials_load(path) && nm_world_materials_upload(NULL)); /* 64 + the allocator's 32. */
    assert(allocations==1 && uploads==2 && nm_world_materials_texture(0) && nm_world_materials_texture(1));
    nm_world_materials_unload(); assert(!vram_used);
    reset(32); assert(nm_world_materials_load(path) && !nm_world_materials_upload(NULL) && !allocations);
    nm_world_materials_unload(); assert(!vram_used && !world.pack.data);
    /* The same palettes already written by a car pack: both are shared and
     * nothing is added; a palette RAM that differs is refused. */
    reset(2*NM_MATERIAL_CACHE_BANKS*NM_MATERIAL_CACHE_BYTES);
    assert(nm_materials_load(path) && allocations==1 && uploads==2 && vram_used==64+32);
    for(unsigned i=0;i<2;++i) {
        /* Resident at load: one block, each texture at its place, bytes in VRAM. */
        const enj_texture_info_t *texture=nm_materials_texture(i);
        assert(nm_materials_require(i) && texture && texture->ptr==(uint8_t *)bank+32*i);
        for(unsigned b=0;b<32;++b) assert(((const uint8_t *)texture->ptr)[b]==32*i+b);
    }
    assert(!nm_materials_require(2) && !nm_materials_texture(2));
    assert(!nm_materials_load(path) && uploads==2); /* Do not overwrite live ownership. */
    assert(nm_world_materials_load(path));
    assert(world.palette_banks==2 && world.palette_slots[0]==0 && world.palette_slots[1]==1);
    nm_world_materials_unload();
    /* What the program wrote there differs (the CPU copy): always refused. */
    written[16*NM_CAR_PALETTE_BANK_FIRST+3]^=1;
    assert(!nm_world_materials_load(path) && !world.pack.data);
    written[16*NM_CAR_PALETTE_BANK_FIRST+3]^=1;
    /* What the PVR reads back differs: refused with ARGB8888 entries; with
     * 16-bit entries reported and not fatal (palette_ram.h). */
    host_palette_ram[16*NM_CAR_PALETTE_BANK_FIRST+3]^=1;
    assert(nm_world_materials_load(path)==NM_PALETTE_16BIT && !world.pack.data==!NM_PALETTE_16BIT);
    nm_world_materials_unload();
    host_palette_ram[16*NM_CAR_PALETTE_BANK_FIRST+3]^=1;
    /* High half of a 16-bit entry: not compared. */
    if(NM_PALETTE_16BIT) {
        host_palette_ram[16*NM_CAR_PALETTE_BANK_FIRST+3]|=0xabcd0000u;
        assert(nm_world_materials_load(path)); nm_world_materials_unload();
    }
    /* A format register changed since startup is refused. */
    host_palette_ram[1024]^=1; assert(!nm_world_materials_load(path)); host_palette_ram[1024]^=1;
    nm_materials_unload(); assert(!vram_used);
}
int main(int argc,char **argv) {
    assert(argc==2);
    /* Nothing is written before the format is set (main.c). */
    host_palette_ram[1024]=99; assert(!nm_world_materials_load(PALETTE_FIXTURE) && !world.pack.data);
    assert(nm_palette_ram_init() && host_palette_ram[1024]==NM_PALETTE_FORMAT);
    palettized(PALETTE_FIXTURE);
    /* A pack of an earlier format (here DNT2) is refused by both loaders and
     * leaves nothing behind. */
    reset(NM_MATERIAL_CACHE_BANKS*NM_MATERIAL_CACHE_BYTES+32);
    assert(!nm_materials_load(argv[1]) && !vram_used && !allocations && !index_pack.data && !textures);
    assert(!nm_world_materials_load(argv[1]) && !vram_used && !allocations && !world.pack.data);
    /* Allocation failure and a failed GPU fence at unload roll back. */
    reset(64); fail_at=1;
    assert(!nm_materials_load(PALETTE_FIXTURE) && !vram_used && !index_pack.data);
    reset(96); assert(nm_world_materials_load(PALETTE_FIXTURE)); fail_at=1;
    assert(!nm_world_materials_upload(NULL) && attempts==1 && !vram_used);
    nm_world_materials_unload(); assert(!world.pack.data);
    (void)ready_failure;
    printf("PASS: actual loader (palette RAM %s): resident DNT6 car pack, DNT6 world palettes, shared banks and selective upload, budget, rollback and unload; earlier formats refused (fake VRAM)\n",
        nm_palette_ram_format_name());
}
'''
    (root/'test.c').write_text(program)
    # Default build (ARGB8888 entries) and make NATIVE_PAL16=1555.
    for defines in ([], ['-DNM_PAL16=1555']):
        executable = root/('test'+''.join(defines).replace('=', ''))
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-g','-DNATIVE_MATERIALS','-DNATIVE_DRIVING_TEXTURES',*defines,
            '-I'+str(root),'-I'+str(code.parent/'include'),str(root/'test.c'),str(code/'material_pack.c'),
            '-lz','-o',str(executable)],check=True)
        subprocess.run([str(executable),str(fixture),*sys.argv[1:]],check=True)
