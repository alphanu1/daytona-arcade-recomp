/* ROM-free idle-PVR transfer microbenchmark. Not a PAL4 fill-rate test.
 * Build the foundation with NATIVE_TRANSFER_BENCH=1; exits via enDjinn.
 * Seven batches, alternating two sources. No printf/readback in timed loops.
 * GPU fences are outside measurement; palettes restored before shutdown. */
#include <string.h>
#include "../include/indexed_texture.h"
#ifdef NATIVE_INDEXED_PACK_BENCH
#include "../include/material_pack.h"
#include <zlib.h>
static int nm_indexed_pack_bench(void) {
    nm_material_pack_t pack={0};
    const uint64_t start=timer_ns_gettime64();
    if(!nm_material_pack_open_cpu(&pack,"/pc/intro-indexed.dnt")) {
        puts("INDEXED PACK load FAIL");return 0;
    }
    const uint64_t loaded=timer_ns_gettime64();
    void *scratch=memalign(32,pack.staging);
    int ok=scratch && pack.indexed_layouts;
    uLong crc=crc32(0,NULL,0);
    for(unsigned i=0;ok && i<pack.resources;++i) {
        ok=nm_material_texture_generate(&pack,i,scratch,pack.staging);
        if(ok) crc=crc32(crc,scratch,pack.textures[i].size);
    }
    ok &= crc==0x79f935b0u;
    printf("INDEXED PACK resources=%lu bindings=%lu owned-file=%lu descriptors=%lu staging=%lu load-us=%llu expand+crc-us=%llu crc=%08lx %s\n",
        (unsigned long)pack.resources,(unsigned long)pack.count,(unsigned long)pack.backing_bytes,
        (unsigned long)pack.decoder_bytes,(unsigned long)pack.staging,
        (unsigned long long)((loaded-start)/1000),
        (unsigned long long)((timer_ns_gettime64()-loaded)/1000),crc,ok ? "PASS" : "FAIL");
    free(scratch);nm_material_pack_close(&pack);return ok;
}
#endif
static int nm_indexed_texture_bench(void) {
    enum { PIXELS=64*128, REPEATS=256 };
    static const unsigned counts[]={4,64,256,0};
    alignas(32) uint32_t header[8]={0};
    alignas(32) uint16_t patterns[256], palette[16];
    uint8_t *indices=memalign(32,PIXELS), *out=memalign(32,32+2*PIXELS);
    pvr_ptr_t target=pvr_mem_malloc(2*PIXELS);
    if(!indices || !out || !target) {
        free(indices);free(out);if(target) pvr_mem_free(target);return 0;
    }
    int ok=!pvr_wait_ready() && !pvr_wait_render_done();
    for(unsigned i=0;i<256;++i) patterns[i]=(uint16_t)(i*197+0x3210);
    for(unsigned i=0;i<16;++i) palette[i]=(uint16_t)(0x8000+i*177);
    puts("Indexed texture bench: validated-once expansion; optional SQ upload; idle GPU, not frame timing");
    for(unsigned c=0;ok && c<4;++c) {
        const unsigned entries=counts[c], bytes=entries ? PIXELS/4 : PIXELS;
        const unsigned size=32+(entries ? entries*8+bytes : 2*PIXELS);
        for(unsigned i=0;i<bytes;++i) indices[i]=(uint8_t)(entries ? 256-entries+i%entries : i&15);
        header[0]=0x78546344;header[1]=size;header[2]=entries ? (entries-1)<<16 : 0;
        header[3]=64|(128u<<16);header[4]=28|(entries ? 1u<<30 : 0);
        nm_indexed_texture_t prepared;
        ok=nm_indexed_texture_prepare(&prepared,header,patterns,entries,indices,bytes);
        if(!ok) break;
        ok=nm_indexed_texture_expand_prepared(out,32+2*PIXELS,&prepared,palette)==size;
        const uint16_t *words=(const uint16_t *)(out+32);
        if(entries) {
            static const unsigned shifts[]={0,8,4,12};
            for(unsigned i=0;i<entries*4;++i) ok &= words[i]==palette[(patterns[i/4]>>shifts[i&3])&15];
            ok &= !memcmp(out+32+entries*8,indices,bytes);
        } else for(unsigned i=0;i<PIXELS;++i) ok &= words[i]==palette[indices[i]];
        for(unsigned round=0;ok && round<7;++round) for(unsigned upload=0;upload<2;++upload) {
            const uint64_t started=timer_ns_gettime64();
            size_t produced=0;
            for(unsigned repeat=0;repeat<REPEATS;++repeat) {
                produced+=nm_indexed_texture_expand_prepared(out,32+2*PIXELS,&prepared,palette);
                if(upload) pvr_txr_load(out+32,target,size-32);
            }
            const uint64_t elapsed=timer_ns_gettime64()-started;
            ok &= produced==(size_t)size*REPEATS;
            if(upload) ok &= !memcmp(target,out+32,size-32);
            printf("INDEXED round=%u entries=%u upload=%u bytes=%u ns/op=%llu check=%d\n",
                round,entries,upload,size-32,(unsigned long long)(elapsed/REPEATS),ok);
        }
    }
    pvr_mem_free(target);free(out);free(indices);
    printf("Indexed texture bench %s\n",ok ? "PASS" : "FAIL");
    return ok;
}
static int nm_transfer_bench(void) {
    const unsigned sizes[]={0,3072,6144,18432,131072};
    enum { REPEATS=1024, MAX_BYTES=131072 };
    uint32_t saved[16], colors[2][16];
    uint8_t *source=memalign(32,2*MAX_BYTES);
    pvr_ptr_t target=pvr_mem_malloc(MAX_BYTES);
    if (!source || !target) {
        free(source); if(target) pvr_mem_free(target); return 0;
    }
    if(pvr_wait_ready() || pvr_wait_render_done()) {
        free(source); pvr_mem_free(target); return 0;
    }
    for(unsigned i=0;i<2*MAX_BYTES;++i) source[i]=(uint8_t)(i*13+(i/MAX_BYTES)*47);
    for(unsigned i=0;i<16;++i) {
        saved[i]=PVR_GET(PVR_PALETTE_TABLE_BASE+4*(1008+i));
        colors[0][i]=0x8000+i*701; colors[1][i]=0x8000+i*911;
    }
    int ok=1;
    puts("Transfer bench: idle PVR, ns/operation; palette=16 MMIO writes, texture=SQ bytes; no GPU sampling/fence cost");
    for(unsigned round=0;round<7;++round) for(unsigned k=0;k<5;++k) {
        const unsigned kind=(k+round)%5, bytes=sizes[kind];
        if(bytes) pvr_txr_load(source,target,bytes);
        const uint64_t start=timer_ns_gettime64();
        for(unsigned n=0;n<REPEATS;++n) {
            if(bytes) pvr_txr_load(source+(n&1)*MAX_BYTES,target,bytes);
            else for(unsigned i=0;i<16;++i) pvr_set_pal_entry(1008+i,colors[n&1][i]);
        }
        const uint64_t elapsed=timer_ns_gettime64()-start;
        if(bytes) ok &= !memcmp(target,source+MAX_BYTES,bytes);
        else for(unsigned i=0;i<16;++i)
            ok &= PVR_GET(PVR_PALETTE_TABLE_BASE+4*(1008+i))==colors[1][i];
        printf("TRANSFER round=%u bytes=%u ns/op=%llu check=%d\n",round,bytes,
            (unsigned long long)(elapsed/REPEATS),ok);
    }
    for(unsigned i=0;i<16;++i) pvr_set_pal_entry(1008+i,saved[i]);
    pvr_mem_free(target); free(source);
    printf("Transfer bench %s; palette restored\n",ok ? "PASS" : "FAIL");
    ok=nm_indexed_texture_bench() && ok;
#ifdef NATIVE_INDEXED_PACK_BENCH
    ok=nm_indexed_pack_bench() && ok;
#endif
    return ok;
}
