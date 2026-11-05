#include "../include/material_pack.h"
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* Original policy: independent linear reference for the sorted queue. */
static unsigned nm_material_evict(unsigned count, const uint32_t *last_used,
        const uint32_t *allocated, uint32_t frame, uint32_t completed) {
    unsigned victim=UINT32_MAX;
    if (!last_used || !allocated || !frame || completed>=frame) return victim;
    for (unsigned i=0;i<count;++i)
        if (allocated[i] && last_used[i]<=completed &&
            (victim==UINT32_MAX || last_used[i]<last_used[victim])) victim=i;
    return victim;
}

int main(int argc, char **argv) {
    if (argc != 2 && (argc!=3 || strcmp(argv[2],"--cpu-backed"))) return 2;
    /* Independent mip sizes from summing levels, not the reader's formula. */
    for(unsigned exponent=0;exponent<8;++exponent) {
        const unsigned side=8u<<exponent;
        unsigned indices=1;
        for(unsigned level=side;level>=2;level/=2) indices+=level*level/4;
        const uint32_t size=(32+2048+indices+31)&~31u;
        uint8_t *blob=calloc(1,size); assert(blob);
        memcpy(blob,"DcTx",4);memcpy(blob+4,&size,4);blob[10]=255;
        const uint16_t dimension=side;
        memcpy(blob+12,&dimension,2);memcpy(blob+14,&dimension,2);
        uint32_t mode=0xc8000000u|exponent<<3|exponent;
        memcpy(blob+16,&mode,4);
        nm_texture_record_t record={.width=side,.height=side,.size=size};
        nm_material_pack_t pack={.textures=&record,.resources=1};
        assert(nm_material_texture_validate(&pack,0,blob,size));
        assert(!nm_material_texture_validate(&pack,0,blob,size-1));
        blob[10]=254;assert(!nm_material_texture_validate(&pack,0,blob,size));blob[10]=255;
        blob[9]=1;assert(!nm_material_texture_validate(&pack,0,blob,size));blob[9]=0;
        mode^=1u<<30;memcpy(blob+16,&mode,4);
        assert(!nm_material_texture_validate(&pack,0,blob,size));
        mode^=1u<<30;mode^=1;memcpy(blob+16,&mode,4);
        record.height=8u<<(exponent^1);memcpy(blob+14,&record.height,2);
        assert(!nm_material_texture_validate(&pack,0,blob,size));
        free(blob);
    }
    {
        const uint32_t allocated[]={64,0,96,128}, stamps[]={2,0,4,3};
        assert(nm_material_evict(4,stamps,allocated,4,1)==UINT32_MAX);
        assert(nm_material_evict(4,stamps,allocated,4,2)==0); // N-2 safe.
        assert(nm_material_evict(4,stamps,allocated,4,3)==0); // Oldest first.
        assert(nm_material_evict(4,stamps,allocated,4,4)==UINT32_MAX); // Never current.
        const uint32_t only_previous[]={0,0,96,128};
        assert(nm_material_evict(4,stamps,only_previous,4,2)==UINT32_MAX);
        assert(nm_material_evict(4,stamps,only_previous,4,3)==3); // Requires render_done.
        assert(nm_material_evict(4,stamps,only_previous,0,0)==UINT32_MAX);
    }
    {
        uint32_t stamps[257], allocated[257], random=11;
        nm_material_victim_t order[257];
        assert(!nm_material_eviction_order(257,NULL,allocated,1,order));
        assert(!nm_material_eviction_order(257,stamps,NULL,1,order));
        assert(!nm_material_eviction_order(257,stamps,allocated,0,order));
        assert(!nm_material_eviction_order(257,stamps,allocated,1,NULL));
        assert(!nm_material_eviction_order(0,stamps,allocated,1,order));
        for (unsigned trial=0;trial<1024;++trial) {
            const uint32_t frame=trial%3==0 ? UINT32_MAX : trial%3==1 ? 1 : 17;
            uint32_t completed=frame>1 ? frame-2 : 0;
            for (unsigned i=0;i<257;++i) {
                random=random*1664525u+1013904223u;
                allocated[i]=(random&3) ? 64 : 0;
                const uint32_t age=(random>>8)%8;
                stamps[i]=age>frame ? 0 : frame-age;
            }
            const unsigned count=nm_material_eviction_order(257,stamps,allocated,frame,order);
            unsigned at=0;
            for (;;) {
                const unsigned expected=nm_material_evict(257,stamps,allocated,frame,completed);
                const unsigned actual=at<count && order[at].last_used<=completed ? order[at].resource : UINT32_MAX;
                assert(expected==actual);
                if (actual==UINT32_MAX) {
                    if (completed<frame-1) { completed=frame-1; continue; } // Render-done fence.
                    break;
                }
                assert(actual<257 && allocated[actual] && stamps[actual]<frame);
                ++at;
                // Some freed blocks are immediately reused by current pins.
                if (at&1) stamps[actual]=frame; else allocated[actual]=0;
            }
            assert(at==count);
        }
    }
    nm_texture_record_t texture = {.width=8,.height=16};
    nm_material_pack_t fixture = {.textures=&texture};
    nm_material_binding_t binding = {.header={0x4000,0,0,0}, .resource=0, .color=8u | (2u<<16)};
    fixture.bindings = &binding;
    assert(nm_material_argb(&fixture, &binding) == UINT32_MAX);
    uint32_t modulation = 0xff123456;
    fixture.modulation = &modulation;
    assert(nm_material_argb(&fixture, &binding) == modulation);
    nm_material_uv_t uv = nm_material_uv(&fixture, &binding);
    assert(uv.cropped == 3 && uv.origin[0] == 64 && uv.origin[1] == 16);
    assert(nm_material_uv_contains(&uv,72,24) && nm_material_uv_contains(&uv,119,135));
    assert(!nm_material_uv_contains(&uv,71,24) && !nm_material_uv_contains(&uv,120,24));
    assert(!nm_material_uv_contains(&uv,72,23) && !nm_material_uv_contains(&uv,72,136));
    for (unsigned u=72; u<120; ++u) for (unsigned v=24; v<136; ++v) {
        assert(((float)u-uv.origin[0])*uv.scale[0] == (u/8.0f-8)/8);
        assert(((float)v-uv.origin[1])*uv.scale[1] == (v/8.0f-2)/16);
    }
    texture.width = texture.height = 32;
    binding.color = UINT32_MAX;
    uv = nm_material_uv(&fixture, &binding);
    assert(!uv.cropped && uv.origin[0] == 0 && uv.scale[0] == 1.0f/256);
    assert(nm_material_uv_contains(&uv,65535,65535)); // Preserve full-axis wrapping.
    binding.resource = NM_MATERIAL_SOLID; binding.color = 0xff123456;
    modulation = UINT32_MAX;
    assert(nm_material_argb(&fixture, &binding) == binding.color);
    uv = nm_material_uv(&fixture, &binding);
    assert(!uv.cropped && uv.origin[0] == 0 && uv.origin[1] == 0);
    nm_material_pack_t pack = {0};
    {
        nm_texture_record_t records[3]={{.size=64},{.size=96},{.size=320}};
        nm_material_pack_t p={.textures=records,.resources=3};
        uint32_t slots[3]={UINT32_MAX,UINT32_MAX,UINT32_MAX}, used=0;
        assert(nm_material_decode_reserve(&p,0,slots,&used,256)==2 && used==64 && slots[0]==0);
        assert(nm_material_decode_reserve(&p,0,slots,&used,256)==1 && used==64);
        assert(nm_material_decode_reserve(&p,1,slots,&used,256)==2 && used==160 && slots[1]==64);
        assert(!nm_material_decode_reserve(&p,2,slots,&used,256) && used==160 && slots[1]==64);
        slots[0]=UINT32_MAX;
        used=256;
        assert(nm_material_decode_reserve(&p,0,slots,&used,256)==2 && used==320 && slots[0]==256);
        assert(nm_material_decode_reserve(&p,1,slots,&used,256)==1 && used==320); // Other entry survived wrap.
        used=352; // First 32 bytes of the old entry have now been overwritten.
        assert(nm_material_decode_reserve(&p,1,slots,&used,256)==2 && slots[1]==352 && used==448);
        slots[1]=UINT32_MAX;
        assert(nm_material_decode_reserve(&p,1,slots,&used,256)==2 && slots[1]==512 && used==608); // No split resource.
        used=UINT32_MAX&~31u;
        assert(nm_material_decode_reserve(&p,1,slots,&used,256)==2 && slots[1]==0 && used==96 && slots[0]==UINT32_MAX);
        used=1; assert(!nm_material_decode_reserve(&p,0,slots,&used,256) && used==1);
        used=192; assert(!nm_material_decode_reserve(&p,0,slots,&used,160) && used==192);
        assert(!nm_material_decode_reserve(&p,3,slots,&used,256));
        /* Independent byte ownership catches false hits after partial overwrite,
         * wrap/padding and thousands of evictions, not just reserve arithmetic. */
        records[2].size=128; used=0; memset(slots,255,sizeof(slots));
        uint8_t arena[256]={0}; uint32_t random=7;
        for (unsigned step=0;step<10000;++step) {
            random=random*1664525u+1013904223u;
            const unsigned i=random%3;
            const int result=nm_material_decode_reserve(&p,i,slots,&used,256);
            const unsigned at=slots[i]&255;
            assert(result && at+records[i].size<=256);
            if (result==1) for (unsigned j=0;j<records[i].size;++j) assert(arena[at+j]==i+1);
            else memset(arena+at,i+1,records[i].size);
        }
    }
    int valid = argc==3 ? nm_material_pack_open_cpu(&pack,argv[1]) : nm_material_pack_open(&pack, argv[1]);
    if (valid) {
        void *original = pack.data;
        assert(!nm_material_pack_open(&pack, argv[1]) && original == pack.data);
        void *staging = aligned_alloc(32,pack.staging);
        assert(staging);
        uLong indexed_crc=crc32(0,NULL,0);
        for (unsigned i = 0; i < pack.resources; ++i) {
            assert(!nm_material_texture_read(&pack, i, staging, pack.textures[i].size - 1));
            if (!nm_material_texture_read(&pack, i, staging, pack.staging)) valid = 0;
            if(valid && pack.indexed_layouts) indexed_crc=crc32(indexed_crc,staging,pack.textures[i].size);
        }
        if (valid && pack.decoder) {
            void *backing=malloc(pack.backing_bytes), *decoded=malloc(pack.staging);
            assert(backing && decoded);
            const uint32_t start=pack.textures[0].offset;
            assert(!fseek(pack.file,start,SEEK_SET));
            assert(fread(backing,1,pack.backing_bytes,pack.file)==pack.backing_bytes);
            for (unsigned pass=0;pass<8;++pass) for (unsigned i=0;i<pack.resources;++i) {
                const nm_texture_record_t *r=pack.textures+i;
                const uint32_t span=(r->stored+31)&~31u;
                const uint8_t *source=(const uint8_t *)backing+r->offset-start;
                assert(nm_material_texture_read(&pack,i,staging,pack.staging));
                assert(!nm_material_texture_decode(&pack,i,source,span-1,decoded,pack.staging));
                assert(!nm_material_texture_decode(&pack,i,source,span,decoded,r->size-1));
                assert(nm_material_texture_decode(&pack,i,source,span,decoded,pack.staging));
                assert(!memcmp(decoded,staging,r->size));
                assert(nm_material_texture_validate(&pack,i,decoded,r->size));
            }
            free(decoded); free(backing);
        }
        assert(!nm_material_texture_read(&pack, pack.resources, staging, pack.staging));
        for (unsigned i = 0; i < pack.count; ++i) {
            const nm_material_binding_t *b = &pack.bindings[i];
            assert(nm_material_find(&pack, b->header, b->luma) == b);
            assert(!nm_material_find(&pack, b->header, 256));
            if (!i || memcmp(b[-1].header, b->header, 8)) { /* a header's first binding: the row equals the lookups */
                const nm_material_binding_t *row[256];
                nm_material_find_row(&pack, b->header, row);
                for (unsigned l = 0; l < 256; ++l) assert(row[l] == nm_material_find(&pack, b->header, l));
            }
            assert(nm_material_argb(&pack, b) == (b->resource == NM_MATERIAL_SOLID ?
                b->color : pack.modulation ? pack.modulation[i] : UINT32_MAX));
        }
        uint32_t *offsets=malloc(pack.resources*sizeof(*offsets)); assert(offsets);
        memset(offsets,255,pack.resources*sizeof(*offsets));
        uint32_t used=1;
        assert(!nm_material_cache_reserve(&pack,0,offsets,&used,NM_MATERIAL_CACHE_BYTES));
        assert(used==1 && offsets[0]==UINT32_MAX);
        used=0;
        for (unsigned i=0;i<pack.resources;++i) {
            const uint32_t before=used;
            if (pack.textures[i].size-32>NM_MATERIAL_CACHE_BYTES-used) {
                assert(!nm_material_cache_reserve(&pack,i,offsets,&used,NM_MATERIAL_CACHE_BYTES));
                assert(used==before && offsets[i]==UINT32_MAX);
                used=0; memset(offsets,255,pack.resources*sizeof(*offsets));
            }
            const uint32_t start=used;
            assert(nm_material_cache_reserve(&pack,i,offsets,&used,NM_MATERIAL_CACHE_BYTES));
            assert(offsets[i]==start && used==start+pack.textures[i].size-32 && !(used&31));
            const uint32_t end=used;
            assert(nm_material_cache_reserve(&pack,i,offsets,&used,NM_MATERIAL_CACHE_BYTES) && used==end);
        }
        assert(!nm_material_cache_reserve(&pack,pack.resources,offsets,&used,NM_MATERIAL_CACHE_BYTES));
        free(offsets);
        const uint16_t missing[4] = {65535,65535,65535,65535};
        assert(!nm_material_find(&pack, missing, 255));
        { const nm_material_binding_t *row[256]; nm_material_find_row(&pack, missing, row);
          for (unsigned l = 0; l < 256; ++l) assert(!row[l]); }
        free(staging);
        if(valid && pack.indexed_layouts) printf("INDEXED decoded crc32=%08lx\n",indexed_crc);
        if (valid) printf("PASS: native materials/UV resources=%u bindings=%u metadata=%u staging=%u payload=%u\n",
            (unsigned)pack.resources, (unsigned)pack.count, (unsigned)pack.metadata,
            (unsigned)pack.staging, (unsigned)pack.payload);
    } else assert(!pack.data && !pack.file);
    nm_material_pack_close(&pack);
    assert(!pack.data && !pack.file && !pack.decoder && !pack.modulation);
    nm_material_pack_close(&pack);
    if (!valid) puts("REJECT: invalid native materials");
    return valid ? 0 : 1;
}
