/* Car and scenery textures: DNT6 palette packs, uploaded once per scene load
 * and resident until unload. Nothing is uploaded, decoded or moved while a
 * scene runs. */
#include "render/materials/materials.h"
#include "scene_loader.h"
#include "render/materials/palette_ram.h"
#include "heap.h"
#include <enDjinn/enj_enDjinn.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include "nm_mem.h"

static nm_material_pack_t index_pack;
static enj_texture_info_t *textures;
static unsigned uploaded;
static pvr_ptr_t bank;
#if NM_SMOKE_MODE
/* make NATIVE_SMOKE_MODE=pt-tex|pt-screen|tr-screen (material_pack.h, NM_SMOKE_MODE): the smoke textures, made at
 * load from each texture a translucent-class binding names (square PAL4, no mip chain), in a VRAM block of their own.
 *   pt-tex: the same texels with every other one, (x + y) odd, made index 15 (alpha 0). In the twiddled PAL4 layout
 *     (bit 0 of a texel's index is y & 1, bit 1 is x & 1; byte k holds texels 2k and 2k+1, the low nibble first)
 *     that is the high nibble of every even byte and the low nibble of every odd one.
 *   pt-screen: twice as wide, the texture's own square followed by a square of index 15 (a 2s x s twiddled texture is
 *     two s x s twiddled squares one after the other), so u + 1/2 always reads a transparent texel. tr-screen: the same.
 * smoke_textures[resource].ptr is NULL for a resource no translucent binding names. */
static enj_texture_info_t *smoke_textures;
static pvr_ptr_t smoke_bank;
static int materials_smoke_textures(const uint8_t *backing, uint32_t backing_start) {
    unsigned count=0; size_t bytes=0;
    smoke_textures=nm_calloc(index_pack.resources,sizeof(*smoke_textures));
    if(!smoke_textures) return 0;
    for(unsigned i=0;i<index_pack.count;++i) {
        if(!(i&4095u)) nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        const nm_material_binding_t *b=index_pack.bindings+i;
        if(b->resource==NM_MATERIAL_SOLID || !nm_material_translucent(b->header) || smoke_textures[b->resource].width) continue;
        const nm_texture_record_t *r=index_pack.textures+b->resource;
        const enj_texture_info_t *t=textures+b->resource;
        if(r->width!=r->height || t->flags.mipmapped || t->flags.compressed || r->size-32!=(uint32_t)r->width*r->height/2) {
            printf("FAIL: smoke texture %u is not a square PAL4 texture without mips\n",b->resource); return 0;
        }
        smoke_textures[b->resource]=*t;
        smoke_textures[b->resource].width=(uint16_t)(NM_SMOKE_DOOR ? 2u*r->width : r->width);
        bytes+=(size_t)smoke_textures[b->resource].width*r->height/2; ++count;
    }
    if(!count) return 1;
    smoke_bank=pvr_mem_malloc((bytes+31)&~(size_t)31);
    uint8_t *scratch=nm_alloc(bytes);
    if(!smoke_bank || !scratch) { free(scratch); return 0; }
    size_t used=0;
    for(unsigned res=0;res<index_pack.resources;++res) {
        enj_texture_info_t *t=smoke_textures+res;
        if(!t->width) continue;
        nm_loader_yield(0); /* one texture a step */
        const nm_texture_record_t *r=index_pack.textures+res;
        const uint8_t *in=backing+r->offset-backing_start+32;
        const size_t square=(size_t)r->width*r->height/2;
        uint8_t *out=scratch+used;
        for(size_t k=0;k<square;++k)
            out[k]=NM_SMOKE_MODE==1 ? (uint8_t)(in[k]|(k&1u ? 0x0fu : 0xf0u)) : in[k];
        if(NM_SMOKE_DOOR) nm_memset(out+square,0xff,square);
        t->ptr=(uint8_t *)smoke_bank+used;
        const size_t size=(size_t)t->width*r->height/2;
        pvr_txr_load(out,t->ptr,size);
        if(memcmp(t->ptr,out,size)) { free(scratch); return 0; }
        used+=size;
    }
    free(scratch);
    printf("Native smoke textures (%s): %u textures, %lu bytes VRAM, VRAM-byte-check PASS\n",
        NM_SMOKE_MODE==1 ? "pt-tex: every other texel transparent" : NM_SMOKE_MODE==2 ? "pt-screen: shape beside a transparent square" :
        "tr-screen: shape beside a transparent square",
        count,(unsigned long)used);
    return 1;
}
const enj_texture_info_t *nm_materials_smoke_texture(unsigned resource) {
    return smoke_textures && resource<index_pack.resources && smoke_textures[resource].ptr ? smoke_textures+resource : NULL;
}
#if NM_SMOKE_DOOR && NM_SMOKE_TIGHT
/* make NATIVE_SMOKE_TIGHT=1 (model_submission.h, sq_tr_tight_prepare): mask[y*side+x] = 1 where texel (x, y) of the
 * resource's smoke texture is not texel 15, read back from its VRAM copy (the first, twiddled side x side square). */
int nm_smoke_texture_mask(unsigned resource, unsigned side, uint8_t *mask) {
    const enj_texture_info_t *t=nm_materials_smoke_texture(resource);
    if(!t || t->height!=side || t->width!=2*side) return 0;
    /* The square's bytes in one copy (word reads of VRAM, not a byte read a texel); then the texels from it. */
    uint8_t data[64u*64u/2u];
    if(side>64u) return 0;
    nm_memcpy(data,t->ptr,(size_t)side*side/2u);
    for(unsigned y=0;y<side;++y) for(unsigned x=0;x<side;++x) {
        unsigned index=0;
        for(unsigned bit=0;(1u<<bit)<side;++bit) index|=((y>>bit)&1u)<<(2*bit) | ((x>>bit)&1u)<<(2*bit+1);
        mask[y*side+x]=((data[index/2]>>(index&1u ? 4 : 0))&15u)!=15u;
    }
    return 1;
}
#endif
#endif

static void materials_texture_info(enj_texture_info_t *info, const nm_texture_record_t *r, const uint8_t *blob) {
    nm_memcpy(&info->hdr,blob,sizeof(info->hdr));
    info->pvrformat=info->hdr.pvr_type&0xFFC00000;
    info->width=r->width; info->height=r->height;
    info->flags.compressed=fDtIsCompressed(&info->hdr);
    info->flags.mipmapped=fDtIsMipmapped(&info->hdr);
    info->flags.twiddled=1;
}

static int materials_wait_idle(void) {
    /* TA readiness means the queued scene STARTED rendering, not finished.
     * Drain registration first, then wait for its render completion. */
    return pvr_wait_ready()==0 && pvr_wait_render_done()==0;
}

void nm_materials_unload(void) {
    if (bank) {
        if (materials_wait_idle()) pvr_mem_free(bank);
        else { puts("FAIL: GPU fence; bank reclaimed only by PVR shutdown"); enj_state_flag_shutdown(NULL); }
        bank=NULL;
    }
#if NM_SMOKE_MODE
    if (smoke_bank) { if (materials_wait_idle()) pvr_mem_free(smoke_bank); smoke_bank=NULL; }
    free(smoke_textures); smoke_textures=NULL;
#endif
    free(textures); textures = NULL; uploaded = 0;
    nm_material_pack_close(&index_pack);
}

int nm_materials_require(unsigned resource) { return bank && resource<uploaded; }

const nm_material_binding_t *nm_materials_find(const uint16_t header[4], unsigned luma) {
    return nm_material_find(&index_pack, header, luma);
}
const enj_texture_info_t *nm_materials_texture(unsigned resource) {
    return resource < uploaded ? &textures[resource] : NULL;
}
const nm_material_pack_t *nm_materials_index(void) { return &index_pack; }
int nm_materials_palette_bank(const nm_material_binding_t *binding) {
    if(!index_pack.palette_index || binding<index_pack.bindings ||
       binding>=index_pack.bindings+index_pack.count) return -1;
    const int palette=nm_material_palette(&index_pack,binding);
    return palette<0 ? -1 : (int)NM_CAR_PALETTE_BANK_FIRST+palette;
}

/* The pack's palettes, once per scene load. Palette RAM is read while
 * the PVR renders; the previous scene's release already fenced its last frame
 * (nm_practice_render_release), and nothing of this scene is submitted yet. */
static int materials_write_palettes(void) {
    _Static_assert(NM_CAR_PALETTE_BANK_FIRST+NM_CAR_PALETTE_BANKS<=64,"car palette banks inside palette RAM");
    if(index_pack.palette_count>NM_CAR_PALETTE_BANKS) {
        printf("FAIL: car material pack needs %lu palette banks; banks %u..%u (%u) are reserved for cars\n",
            (unsigned long)index_pack.palette_count,NM_CAR_PALETTE_BANK_FIRST,
            NM_CAR_PALETTE_BANK_FIRST+NM_CAR_PALETTE_BANKS-1,NM_CAR_PALETTE_BANKS);
        return 0;
    }
    /* Written in the program's palette format (palette_ram.h), read back. */
    const unsigned first=16*NM_CAR_PALETTE_BANK_FIRST,entries=16*index_pack.palette_count;
    if(!nm_palette_ram_write(first,index_pack.palettes,entries,"car palettes")) {
        printf("FAIL: car palette RAM readback at entries %u..%u\n",first,first+entries-1);return 0;
    }
    printf("Native car palettes: %lu palettes in banks %u..%lu (%s, %s lists, %lu saturating buckets); readback PASS\n",
        (unsigned long)index_pack.palette_count,NM_CAR_PALETTE_BANK_FIRST,
        (unsigned long)(NM_CAR_PALETTE_BANK_FIRST+index_pack.palette_count-1),nm_palette_ram_format_name(),
        index_pack.palette_flags&1 ? "split" : "shared",(unsigned long)((index_pack.palette_flags>>8)&255));
    return 1;
}

int nm_materials_load(const char *path) {
    if (index_pack.data || textures) return 0;
    const uint64_t started = timer_ns_gettime64();
    const unsigned before = pvr_mem_available();
    uint8_t *backing = NULL;
    if (!nm_material_pack_open_cpu(&index_pack,path)) goto fail;
    if (!index_pack.palette_index) {
        puts("FAIL: car.dnt is not a DNT6 palette pack (tools/prepare_palette_materials.py); earlier formats are no longer drawn");
        goto fail;
    }
    if (!materials_write_palettes()) goto fail;
    if (index_pack.payload>NM_MATERIAL_CACHE_BANKS*NM_MATERIAL_CACHE_BYTES) goto fail;
    /* One PAL4 texture per arcade texture: VRAM is exactly the payload. */
    const size_t bank_bytes=(index_pack.payload+31)&~(size_t)31;
    if (bank_bytes+32+512*1024>before) goto fail;
    const uint32_t backing_start=index_pack.textures[0].offset, backing_bytes=index_pack.backing_bytes;
    backing=nm_heap_alloc("car texture upload scratch",32,backing_bytes);
    textures=nm_calloc(index_pack.resources,sizeof(*textures));
    bank=pvr_mem_malloc(bank_bytes);
    if (!backing || !textures || !bank) goto fail;
    if (fseek(index_pack.file,backing_start,SEEK_SET) ||
        fread(backing,1,backing_bytes,index_pack.file)!=backing_bytes) goto fail;
    /* Startup only: no GPU scene has been submitted yet. Every texture keeps
     * its address for the entire scene; actual VRAM is verified before the
     * CPU copy is released. */
    uint32_t used=0;
    for (unsigned i=0;i<index_pack.resources;++i) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one texture a step */
        const nm_texture_record_t *r=index_pack.textures+i;
        const uint8_t *blob=backing+r->offset-backing_start;
        if (!nm_material_texture_validate(&index_pack,i,blob,r->size) || r->size-32>bank_bytes-used) goto fail;
        materials_texture_info(textures+i,r,blob);
        textures[i].ptr=(uint8_t *)bank+used;
        pvr_txr_load(blob+32,textures[i].ptr,r->size-32);
        if (memcmp(textures[i].ptr,blob+32,r->size-32)) goto fail;
        used+=r->size-32; ++uploaded;
    }
#if NM_SMOKE_MODE
    if (!materials_smoke_textures(backing,backing_start)) { puts("FAIL: smoke textures (NATIVE_SMOKE_MODE)"); goto fail; }
#endif
    if (fclose(index_pack.file)) { index_pack.file=NULL; goto fail; }
    index_pack.file=NULL;
    free(backing);
    printf("Resident car startup: resources=%u payload=%lu allocation=%lu VRAM-byte-check PASS; no gameplay uploads\n",
        uploaded,(unsigned long)used,(unsigned long)bank_bytes);
    printf("Native car materials: %u PAL4 resources/%lu bindings; metadata/descriptors=%lu; VRAM free=%lu; startup=%llu us\n",
        uploaded,(unsigned long)index_pack.count,
        (unsigned long)(index_pack.metadata+uploaded*sizeof(*textures)),
        (unsigned long)pvr_mem_available(),
        (unsigned long long)((timer_ns_gettime64()-started)/1000));
    return 1;
fail:
    printf("Native materials rollback: uploaded=%u textures\n", uploaded);
    free(backing);
    nm_materials_unload();
    puts("FAIL: native material load/validation/budget; partial uploads rolled back");
    return 0;
}

/* The course's scenery pack: palettes in the banks it shares with the cars,
 * and one VRAM block for the textures the selected scenery draws. */
static struct {
    nm_material_pack_t pack;
    enj_texture_info_t *textures;
    uint32_t used, loaded, budget;
    unsigned resident;
    pvr_ptr_t block;
    uint8_t *palette_slots; /* palette RAM slot of each pack palette */
    unsigned palette_banks;
    uint64_t bytes;
} world;

const nm_material_pack_t *nm_world_materials_index(void) { return &world.pack; }

void nm_world_materials_unload(void) {
    if (world.block) {
        if (materials_wait_idle()) pvr_mem_free(world.block);
        else { puts("FAIL world GPU fence; allocation left to PVR shutdown"); enj_state_flag_shutdown(NULL); }
    }
    free(world.palette_slots);
    free(world.textures);
    nm_material_pack_close(&world.pack);
    nm_memset(&world,0,sizeof(world));
}

/* Palettes now (nothing of this scene is submitted yet and the previous scene
 * was fenced at its release); textures when the renderer knows which of them
 * the selected scenery draws (nm_world_materials_upload). */
static int world_palette_load(void) {
    const size_t available=pvr_mem_available();
    if(available<=512*1024) return 0;
    world.budget=available-512*1024<NM_WORLD_MATERIAL_BYTES ?
        (uint32_t)(available-512*1024) : NM_WORLD_MATERIAL_BYTES;
    world.palette_slots=nm_alloc(world.pack.palette_count);
    world.textures=nm_calloc(world.pack.resources,sizeof(*world.textures));
    if(!world.palette_slots || !world.textures) return 0;
    const nm_material_pack_t *cars=index_pack.palette_index ? &index_pack : NULL;
    world.palette_banks=nm_material_palette_share(cars,&world.pack,world.palette_slots,NM_CAR_PALETTE_BANKS);
    if(!world.palette_banks) {
        printf("FAIL: car and world palettes need more than the %u banks %u..%u\n",NM_CAR_PALETTE_BANKS,
            NM_CAR_PALETTE_BANK_FIRST,NM_CAR_PALETTE_BANK_FIRST+NM_CAR_PALETTE_BANKS-1);
        return 0;
    }
    const unsigned shared=cars ? cars->palette_count : 0;
    unsigned added=0;
    for(unsigned i=0;i<world.pack.palette_count;++i) {
        const unsigned first=16*(NM_CAR_PALETTE_BANK_FIRST+world.palette_slots[i]);
        const uint32_t *colours=world.pack.palettes+16*i;
        /* Shared banks too: the car pack wrote them at its own load. */
        if(world.palette_slots[i]>=shared ? !nm_palette_ram_write(first,colours,16,"world palettes") :
           !nm_palette_ram_verify(first,colours,16,"world palettes in car banks")) {
            printf("FAIL: world palette RAM readback at entries %u..%u\n",first,first+15);return 0;
        }
        added+=world.palette_slots[i]>=shared;
    }
    printf("Native world palettes: %lu palettes, %u in car banks, %u added; banks %u..%u in use of %u..%u (%s); readback PASS\n",
        (unsigned long)world.pack.palette_count,(unsigned)world.pack.palette_count-added,added,
        NM_CAR_PALETTE_BANK_FIRST,NM_CAR_PALETTE_BANK_FIRST+world.palette_banks-1,
        NM_CAR_PALETTE_BANK_FIRST,NM_CAR_PALETTE_BANK_FIRST+NM_CAR_PALETTE_BANKS-1,nm_palette_ram_format_name());
    return 1;
}

int nm_world_materials_palette_bank(const nm_material_binding_t *binding) {
    if(!world.palette_slots || binding<world.pack.bindings ||
       binding>=world.pack.bindings+world.pack.count) return -1;
    const int palette=nm_material_palette(&world.pack,binding);
    return palette<0 ? -1 : (int)NM_CAR_PALETTE_BANK_FIRST+world.palette_slots[palette];
}

int nm_world_materials_palettized(void) { return world.palette_slots!=NULL; }

/* used: one byte per resource (NULL: all). Once per load, before any header
 * of this pack is compiled. Unused resources stay absent for the whole scene. */
int nm_world_materials_upload(const uint8_t *used) {
    if(!world.palette_slots || world.block || world.resident || !world.pack.file) return 0;
    uint32_t total=0,count=0;
    for(unsigned i=0;i<world.pack.resources;++i) if(!used || used[i]) {
        total+=world.pack.textures[i].size-32; ++count;
    }
    const size_t before=pvr_mem_available();
    if(!count || total>world.budget || total+32+512*1024>before) {
        printf("FAIL: world PAL4 textures need %lu bytes; budget %lu, VRAM free %lu\n",
            (unsigned long)total,(unsigned long)world.budget,(unsigned long)before);
        return 0;
    }
    uint8_t *staging=nm_heap_alloc("world texture upload scratch",32,world.pack.staging);
    world.block=pvr_mem_malloc(total);
    int ok=staging && world.block;
    uint32_t at=0;
    for(unsigned i=0;ok && i<world.pack.resources;++i) if(!used || used[i]) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one texture a step */
        const nm_texture_record_t *r=world.pack.textures+i;
        ok=!fseek(world.pack.file,(long)r->offset,SEEK_SET) && fread(staging,1,r->size,world.pack.file)==r->size &&
            nm_material_texture_validate(&world.pack,i,staging,r->size);
        if(!ok) break;
        enj_texture_info_t *info=world.textures+i;
        materials_texture_info(info,r,staging);
        info->ptr=(uint8_t *)world.block+at;
        pvr_txr_load(staging+32,info->ptr,r->size-32);
        ok=!memcmp(info->ptr,staging+32,r->size-32);
        at+=r->size-32; world.bytes+=r->size-32; ++world.loaded;
    }
    free(staging);
    if(!ok || fclose(world.pack.file)) { world.pack.file=NULL; puts("FAIL: world PAL4 texture upload"); return 0; }
    world.pack.file=NULL;
    world.used=(uint32_t)(before-pvr_mem_available());
    world.resident=1;
    printf("Native world PAL4 resident: %lu of %lu textures, payload=%lu VRAM=%lu (one block) budget=%lu free=%lu; byte check PASS; no gameplay uploads\n",
        (unsigned long)count,(unsigned long)world.pack.resources,(unsigned long)total,(unsigned long)world.used,
        (unsigned long)world.budget,(unsigned long)pvr_mem_available());
    return 1;
}

int nm_world_materials_load(const char *path) {
    if (world.pack.data || !nm_material_pack_open_cpu(&world.pack,path)) return 0;
    if (!world.pack.palette_index) {
        puts("FAIL: world.dnt is not a DNT6 palette pack (tools/prepare_palette_materials.py); earlier formats are no longer drawn");
        nm_material_pack_close(&world.pack); return 0;
    }
    nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
    if (world_palette_load()) return 1;
    nm_world_materials_unload(); return 0;
}

const enj_texture_info_t *nm_world_materials_texture(unsigned resource) {
    return resource<world.pack.resources && world.textures[resource].ptr ? world.textures+resource : NULL;
}
