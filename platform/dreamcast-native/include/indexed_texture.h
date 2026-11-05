#ifndef DAYTONA_NATIVE_INDEXED_TEXTURE_H
#define DAYTONA_NATIVE_INDEXED_TEXTURE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdalign.h>
#include "nm_mem.h"
#ifdef __DREAMCAST__
#include <sh4zam/shz_mem.h>
#endif

/* CPU staging only, not SQ/PVR ownership. Caller supplies disjoint32-byte
 * aligned buffers and an offline-quantized16-colour palette. Header/layout
 * are prepared offline; no runtime twiddle, colour conversion or allocation.
 * Failure leaves output untouched. Returned bytes are an ordinary DcTx.
 * Raw fallback indices are pre-twiddled one-byte palette symbols. */
typedef struct {
    alignas(32) const uint32_t *header;
    const uint16_t *patterns;
    const uint8_t *indices;
    size_t index_bytes, pixels, payload, size;
    unsigned entries;
} nm_indexed_texture_t;

/* Startup boundary. Borrowed buffers must stay alive and immutable until
 * the prepared descriptor is discarded. Re-prepare after any source change. */
static inline int nm_indexed_texture_prepare(nm_indexed_texture_t *prepared,
        const uint32_t header[8], const uint16_t *patterns, unsigned entries,
        const uint8_t *indices, size_t index_bytes) {
    if (!prepared || !header || !indices ||
            (((uintptr_t)prepared|(uintptr_t)header|(uintptr_t)indices)&31u) ||
            entries>256 || (entries&3) ||
            (entries && (!patterns || ((uintptr_t)patterns&31u)))) return 0;
    const unsigned width=header[3]&65535u, height=header[3]>>16, mode=header[4];
    if (header[0]!=0x78546344u || header[5] || header[6] || header[7] ||
            header[2]!=(entries ? (entries-1)<<16 : 0) || (mode&~0x4800003fu) ||
            width!=(8u<<((mode>>3)&7)) || height!=(8u<<(mode&7)) ||
            !!entries!=!!(mode&(1u<<30))) return 0;
    const size_t pixels=(size_t)width*height;
    if (index_bytes!=(entries ? pixels/4 : pixels)) return 0;
    const size_t payload=entries ? entries*8+index_bytes : pixels*2;
    const size_t size=(32+payload+31)&~(size_t)31;
    if (size>264224 || header[1]!=size) return 0;
    for (size_t i=0;i<index_bytes;++i)
        if (entries ? indices[i]<256-entries : indices[i]>15) return 0;
    *prepared=(nm_indexed_texture_t){header,patterns,indices,index_bytes,pixels,payload,size,entries};
    return 1;
}

/* Only descriptors successfully initialized by prepare(), never file bytes. */
static inline size_t nm_indexed_texture_expand_prepared(void *output,size_t capacity,
        const nm_indexed_texture_t *prepared,const uint16_t palette[16]) {
    if (!output || !prepared || !palette ||
            (((uintptr_t)output|(uintptr_t)palette)&31u) || capacity<prepared->size) return 0;
    const uint32_t *header=prepared->header;
    const uint16_t *patterns=prepared->patterns;
    const uint8_t *indices=prepared->indices;
    const unsigned entries=prepared->entries;
    const size_t pixels=prepared->pixels,index_bytes=prepared->index_bytes;
    const size_t size=prepared->size,payload=prepared->payload;
    uint32_t *out=output;
#ifdef __DREAMCAST__
    shz_memcpy32(out,header,32);
#else
    nm_memcpy(out,header,32);
#endif
    out+=8;
    if (entries) {
        for (unsigned i=0;i<entries;++i) {
            const unsigned q=patterns[i];
            out[2*i]=(uint32_t)palette[q&15]|(uint32_t)palette[(q>>8)&15]<<16;
            out[2*i+1]=(uint32_t)palette[(q>>4)&15]|(uint32_t)palette[q>>12]<<16;
        }
        void *target=(uint8_t *)out+entries*8;
#ifdef __DREAMCAST__
        shz_memcpy32(target,indices,index_bytes&~(size_t)31);
        if(index_bytes&31) shz_memcpy4((uint8_t *)target+(index_bytes&~(size_t)31),
            indices+(index_bytes&~(size_t)31),index_bytes&31);
#else
        nm_memcpy(target,indices,index_bytes);
#endif
    } else {
        const uint32_t *symbols=(const uint32_t *)indices;
        for (size_t i=0;i<pixels/4;++i) {
            const uint32_t q=symbols[i];
            out[2*i]=(uint32_t)palette[q&15]|(uint32_t)palette[(q>>8)&15]<<16;
            out[2*i+1]=(uint32_t)palette[(q>>16)&15]|(uint32_t)palette[q>>24]<<16;
        }
    }
    for (size_t at=32+payload;at<size;at+=4) ((uint32_t *)output)[at/4]=0;
    return size;
}

/* Checked one-shot entry retained for callers without immutable backing. */
static inline size_t nm_indexed_texture_expand(void *output,size_t capacity,
        const uint32_t header[8],const uint16_t *patterns,unsigned entries,
        const uint8_t *indices,size_t index_bytes,const uint16_t palette[16]) {
    nm_indexed_texture_t prepared;
    if (!nm_indexed_texture_prepare(&prepared,header,patterns,entries,indices,index_bytes)) return 0;
    return nm_indexed_texture_expand_prepared(output,capacity,&prepared,palette);
}
#endif
