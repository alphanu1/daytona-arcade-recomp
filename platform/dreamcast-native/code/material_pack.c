#include "material_pack.h"
#include "heap.h"
#include "scene_loader.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "nm_mem.h"

static int compare_victims(const void *left, const void *right) {
    const nm_material_victim_t *a=left, *b=right;
    if (a->last_used!=b->last_used) return a->last_used<b->last_used ? -1 : 1;
    return (a->resource>b->resource)-(a->resource<b->resource);
}

unsigned nm_material_eviction_order(unsigned count, const uint32_t *last_used,
        const uint32_t *allocated, uint32_t frame, nm_material_victim_t *order) {
    if (!last_used || !allocated || !frame || !order) return 0;
    unsigned used=0;
    for (unsigned i=0;i<count;++i)
        if (allocated[i] && last_used[i]<frame)
            order[used++]=(nm_material_victim_t){last_used[i],i};
    qsort(order,used,sizeof(*order),compare_victims);
    return used;
}

_Static_assert(sizeof(nm_texture_record_t) == 16 && sizeof(nm_material_binding_t) == 16, "DNT1 records");
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error DNT1 reader requires a little-endian host
#endif

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static unsigned u16(const uint8_t *p) { return p[0] | (unsigned)p[1]<<8; }

/* zlib state + optional 32 KiB history window, allocated only at pack open.
 * Bounded allocator also covers malformed streams; no frame-time malloc.
 * A library needing more space fails closed instead of growing the heap. */
typedef struct {
    z_stream stream;
    size_t used;
    uint64_t arena[48*1024/8];
} nm_material_decoder_t;
static voidpf decoder_alloc(voidpf opaque, uInt items, uInt size) {
    nm_material_decoder_t *d=opaque;
    if (size && items>(sizeof(d->arena)-d->used)/size) return NULL;
    size_t bytes=(size_t)items*size;
    if (bytes>sizeof(d->arena)-d->used || bytes+7>sizeof(d->arena)-d->used) return NULL;
    void *p=(uint8_t *)d->arena+d->used;
    d->used+=(bytes+7)&~(size_t)7;
    nm_memset(p,0,bytes);
    return p;
}
static void decoder_free(voidpf opaque, voidpf address) { (void)opaque; (void)address; }

void nm_material_pack_close(nm_material_pack_t *pack) {
    if (pack->file) fclose(pack->file);
    if (pack->decoder) {
        nm_material_decoder_t *d=pack->decoder;
        if (d->stream.state) inflateEnd(&d->stream);
        free(d);
    }
    free(pack->data);
    free(pack->indexed_layouts);
    nm_memset(pack, 0, sizeof(*pack));
}

/* DNT5 has separate bounded CPU storage; legacy format limits do not change. */
static int open_indexed(nm_material_pack_t *p,const uint8_t h[32]) {
    const uint32_t size=u32(h+4),nr=u32(h+8),nb=u32(h+12),nl=u32(h+24),np=u32(h+28);
    if(size<32 || size>2u*1024u*1024u || (size&31) || !nr || nr>=65535 ||
            !nb || nb>=65535 || !nl || nl>256 || !np || np>16384) return 0;
    const uint32_t metadata=16*(nr+nb),base=(32+metadata+31)&~31u;
    uint32_t end=base+64*nl+32*np;
    if(end>size || fseek(p->file,0,SEEK_END) || ftell(p->file)!=(long)size ||
            fseek(p->file,0,SEEK_SET)) return 0;
    p->data=nm_alloc(size);
    p->indexed_layouts=nm_alloc(nl*sizeof(*p->indexed_layouts));
    if(!p->data || !p->indexed_layouts || fread(p->data,1,size,p->file)!=size) return 0;
    const uint8_t *data=p->data;
    if(memcmp(data,h,32)) return 0;
    for(unsigned i=32+metadata;i<base;++i) if(data[i]) return 0;
    for(unsigned i=0;i<nl;++i) {
        const uint8_t *record=data+base+64*i;
        const unsigned po=u32(record+32),io=u32(record+36),bytes=u32(record+40),entries=u32(record+44);
        if(entries>256 || (entries&3) || po!=end || 2*entries>size-po ||
                io!=((po+2*entries+31)&~31u) || io>size || bytes>size-io ||
                u32(record+48) || u32(record+52) || u32(record+56) || u32(record+60)) return 0;
        for(unsigned j=po+2*entries;j<io;++j) if(data[j]) return 0;
        end=(io+bytes+31)&~31u;
        if(end>size) return 0;
        for(unsigned j=io+bytes;j<end;++j) if(data[j]) return 0;
        if(!nm_indexed_texture_prepare(p->indexed_layouts+i,(const uint32_t *)record,
                (const uint16_t *)(data+po),entries,data+io,bytes)) return 0;
    }
    if(end!=size) return 0;
    p->textures=(nm_texture_record_t *)(data+32);
    p->bindings=(nm_material_binding_t *)(data+32+16*nr);
    p->indexed_palettes=(const uint16_t *)(data+base+64*nl);
    uint64_t payload=0;unsigned maximum=0;
    for(unsigned i=0;i<nr;++i) {
        const nm_texture_record_t *r=p->textures+i;
        if(r->offset>=nl || r->stored>=np) return 0;
        const nm_indexed_texture_t *layout=p->indexed_layouts+r->offset;
        if(r->size!=layout->size || r->width!=(layout->header[3]&65535) ||
                r->height!=layout->header[3]>>16) return 0;
        payload+=r->size-32;if(r->size>maximum) maximum=r->size;
    }
    if(payload!=u32(h+16) || maximum!=u32(h+20)) return 0;
    for(unsigned i=0;i<nb;++i) {
        const nm_material_binding_t *b=p->bindings+i;
        if(b->luma>255 || (i && memcmp(b-1,b,10)>=0)) return 0;
        if(b->header[0]&0x4000) {
            if(b->resource>=nr || b->color) return 0;
            const nm_texture_record_t *r=p->textures+b->resource;
            const unsigned mode=p->indexed_layouts[r->offset].header[4];
            const int rgb=((b->header[0]>>13)&3)==2;
            if(rgb!=!!(mode&(1u<<27)) || r->width!=(32u<<(b->header[0]&7)) ||
                    r->height!=(32u<<((b->header[0]>>3)&7))) return 0;
            if(!rgb) for(unsigned c=0;c<16;++c)
                if(!!(p->indexed_palettes[r->stored*16+c]&0x8000)!=(c!=15)) return 0;
        } else if(b->resource!=NM_MATERIAL_SOLID || b->color>>24!=255) return 0;
    }
    p->resources=nr;p->count=nb;p->payload=(uint32_t)payload;p->staging=maximum;
    p->metadata=metadata;p->backing_bytes=size;p->decoder_bytes=nl*sizeof(*p->indexed_layouts);
    return 1;
}

static int open_pack(nm_material_pack_t *pack, const char *path, int cpu_backed) {
    if (!pack || pack->data || pack->file || pack->decoder || pack->indexed_layouts || !path) return 0;
    nm_material_pack_t next = {0};
    uint8_t h[32];
    next.file = fopen(path, "rb");
    if (!next.file) return 0;
    if (setvbuf(next.file, NULL, _IOFBF, 64*1024) || fread(h, 1, 32, next.file) != 32) goto fail;
    if(!memcmp(h,"DNT5",4)) {
        if(!cpu_backed || !open_indexed(&next,h)) goto fail;
        *pack=next;return 1;
    }
    if (
        (memcmp(h, "DNT1", 4) && memcmp(h, "DNT2", 4) && memcmp(h, "DNT3", 4) &&
         memcmp(h, "DNT4", 4) && memcmp(h, "DNT6", 4)) ||
        (h[3] != '6' && (u32(h+24) || u32(h+28)))) goto fail;
    const int compressed = h[3] == '3', crops = h[3] != '1';
    const int palettized = h[3] == '6', modulated = h[3] == '4' || palettized;
    if ((compressed || palettized) && !cpu_backed) goto fail;
    if (palettized) {
        next.palette_count = u32(h+24); next.palette_flags = u32(h+28);
        if (!next.palette_count || next.palette_count > NM_MATERIAL_PALETTES_MAX ||
            (next.palette_flags & ~0x1ff01u)) goto fail;
    }
    const uint32_t size = u32(h+4);
    next.resources = u32(h+8); next.count = u32(h+12);
    next.payload = u32(h+16); next.staging = u32(h+20);
    if (size > (cpu_backed ? 10u*1024u*1024u : NM_MATERIAL_FILE_MAX) || !next.resources ||
        next.resources > (cpu_backed ? 4096u : NM_MATERIAL_RESOURCES_MAX) ||
        /* Offline resident car anchors retain the DNT5 16-bit binding domain.
         * Keep ordinary resident and legacy DNT1..3 limits unchanged. */
        !next.count || next.count > (cpu_backed && modulated ? 65534u : NM_MATERIAL_BINDINGS_MAX) ||
        (!compressed && next.payload > (cpu_backed ? 8u*1024u*1024u : NM_TEXTURE_PAYLOAD_MAX)) ||
        fseek(next.file, 0, SEEK_END) || ftell(next.file) != (long)size || fseek(next.file, 32, SEEK_SET)) goto fail;
    next.metadata = (next.resources + next.count) * 16 + (modulated ? next.count*4 : 0) +
        (palettized ? ((next.count+3)&~3u) + next.palette_count*68 : 0);
    const uint32_t start = (32 + next.metadata + 31) & ~31u;
    if (size < start) goto fail;
    next.backing_bytes=size-start;
    if (compressed && next.backing_bytes>9u*1024u*1024u) goto fail;
    next.data = nm_heap_alloc(path, 0, next.metadata);
    if (!next.data || fread(next.data, 1, next.metadata, next.file) != next.metadata) goto fail;
    next.textures = next.data;
    next.bindings = (nm_material_binding_t *)(next.textures + next.resources);
    if (modulated) next.modulation = (uint32_t *)(next.bindings + next.count);
    if (palettized) {
        const uint8_t *indices = (const uint8_t *)(next.modulation + next.count);
        const uint8_t *table = indices + ((next.count+3)&~3u);
        const uint8_t *keys = table + next.palette_count*64;
        for (unsigned i = next.count; i < ((next.count+3)&~3u); ++i) if (indices[i]) goto fail;
        next.palette_index = indices; next.palettes = (const uint32_t *)table;
        for (unsigned i = 0; i < next.palette_count; ++i) {
            /* Entry 15 is transparent exactly in palettes serving the
             * punch-through list; every other entry is opaque. */
            const unsigned list = keys[4*i+3] >> 7;
            if ((keys[4*i+3] & 126) || (!list && !(next.palette_flags & 1))) goto fail;
            for (unsigned b = 0; b < 3; ++b) if (keys[4*i+b] > ((next.palette_flags >> 8) & 255)) goto fail;
            for (unsigned c = 0; c < 16; ++c)
                if ((next.palettes[16*i+c] >> 24) != (c == 15 && list ? 0u : 255u)) goto fail;
        }
    }
    for (unsigned i = 32 + next.metadata; i < start; ++i) if (fgetc(next.file) != 0) goto fail;
    uint32_t offset = start, payload = 0, maximum = 0;
    for (unsigned i = 0; i < next.resources; ++i) {
        const nm_texture_record_t *r = &next.textures[i];
        if ((compressed ? !r->stored || r->stored>r->size+1024 : r->stored!=0) ||
            r->offset != offset || r->size < 64 || r->size>264224 || (r->size & 31) ||
            r->width < 8 || r->width > 1024 || (r->width & (r->width-1)) ||
            r->height < 8 || r->height > 1024 || (r->height & (r->height-1))) goto fail;
        const uint32_t span=compressed ? (r->stored+31)&~31u : r->size;
        if (span>size-offset) goto fail;
        offset += span; payload += r->size - 32;
        if (r->size > maximum) maximum = r->size;
    }
    if (offset != size || payload != next.payload || maximum != next.staging || maximum > 264224) goto fail;
    for (unsigned i = 0; i < next.count; ++i) {
        if (!(i & 1023u)) nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        const nm_material_binding_t *b = &next.bindings[i];
        if (b->luma > 255 || (i && memcmp(b-1, b, 10) >= 0)) goto fail;
        /* Vertex alpha 0xff, except the translucent class of DNT6 (textured, texel 15 transparent, screen
         * checker: the cars' smoke), whose 0x80 is the half coverage of the arcade's screen door. */
        if (modulated && ((next.modulation[i] >> 24) != (palettized && b->resource != NM_MATERIAL_SOLID &&
                nm_material_translucent(b->header) ? NM_MATERIAL_TRANSLUCENT_ALPHA : 255u) ||
            (b->resource == NM_MATERIAL_SOLID && next.modulation[i] != UINT32_MAX))) goto fail;
        if (palettized && (b->resource == NM_MATERIAL_SOLID ? next.palette_index[i] != 255 :
                           next.palette_index[i] >= next.palette_count)) goto fail;
        if (b->header[0] & 0x4000) {
            if (b->resource >= next.resources || (!crops && b->color != UINT32_MAX)) goto fail;
            if (palettized) {
                /* A punch-through binding needs a transparent entry 15. */
                const uint8_t *keys = (const uint8_t *)next.palettes + next.palette_count*64;
                if (((b->header[0] >> 13) & 3) == 3 && !(keys[4*next.palette_index[i]+3] >> 7)) goto fail;
            }
            const nm_texture_record_t *r = &next.textures[b->resource];
            for (unsigned a = 0; a < 2; ++a) {
                const unsigned original = 32u << ((b->header[0] >> (a*3)) & 7);
                const unsigned extent = a ? r->height : r->width;
                const unsigned origin = crops ? (b->color >> (a*16)) & 65535 : 0;
                if (origin & 1 || origin + extent > original || (!crops && extent != original)) goto fail;
            }
        } else if (b->resource != NM_MATERIAL_SOLID || (b->color >> 24) != 255) goto fail;
    }
    if (compressed) {
        nm_material_decoder_t *d=nm_calloc(1,sizeof(*d));
        if (!d) goto fail;
        next.decoder=d;
        next.decoder_bytes=sizeof(*d);
        d->stream.zalloc=decoder_alloc; d->stream.zfree=decoder_free; d->stream.opaque=d;
        if (inflateInit(&d->stream)!=Z_OK) goto fail;
    }
    *pack = next;
    return 1;
fail:
    nm_material_pack_close(&next);
    return 0;
}

int nm_material_pack_open(nm_material_pack_t *pack, const char *path) { return open_pack(pack,path,0); }
int nm_material_pack_open_cpu(nm_material_pack_t *pack, const char *path) { return open_pack(pack,path,1); }

int nm_material_texture_generate(const nm_material_pack_t *pack,unsigned resource,
        void *buffer,size_t capacity) {
    if(!pack || !pack->indexed_layouts || resource>=pack->resources) return 0;
    const nm_texture_record_t *r=pack->textures+resource;
    return nm_indexed_texture_expand_prepared(buffer,capacity,pack->indexed_layouts+r->offset,
            pack->indexed_palettes+r->stored*16)==r->size;
}

int nm_material_texture_read(nm_material_pack_t *pack, unsigned resource, void *buffer, size_t capacity) {
    if (!pack || !pack->file || !buffer || resource >= pack->resources) return 0;
    const nm_texture_record_t *r = &pack->textures[resource];
    if(pack->indexed_layouts) return nm_material_texture_generate(pack,resource,buffer,capacity);
    if (capacity < r->size || fseek(pack->file, r->offset, SEEK_SET)) return 0;
    if (pack->decoder) {
        const uint32_t span=(r->stored+31)&~31u;
        void *stored=nm_alloc(span); // File reader is startup/host only.
        if (!stored) return 0;
        const int ok=fread(stored,1,span,pack->file)==span &&
            nm_material_texture_decode(pack,resource,stored,span,buffer,capacity);
        free(stored);
        if (!ok) return 0;
    } else if (fread(buffer,1,r->size,pack->file)!=r->size) return 0;
    return nm_material_texture_validate(pack,resource,buffer,r->size);
}

int nm_material_texture_decode(nm_material_pack_t *pack, unsigned resource,
        const void *source, size_t bytes, void *buffer, size_t capacity) {
    if (!pack || !pack->decoder || resource>=pack->resources || !source || !buffer) return 0;
    const nm_texture_record_t *r=pack->textures+resource;
    if (bytes!=((r->stored+31)&~31u) || capacity<r->size) return 0;
    for (size_t i=r->stored;i<bytes;++i) if (((const uint8_t *)source)[i]) return 0;
    nm_material_decoder_t *d=pack->decoder;
    if (inflateReset(&d->stream)!=Z_OK) return 0;
    d->stream.next_in=(Bytef *)source; d->stream.avail_in=r->stored;
    d->stream.next_out=buffer; d->stream.avail_out=r->size;
    return inflate(&d->stream,Z_FINISH)==Z_STREAM_END &&
        !d->stream.avail_in && d->stream.total_out==r->size;
}

int nm_material_texture_validate(const nm_material_pack_t *pack, unsigned resource,
        const void *buffer, size_t bytes) {
    if (!pack || resource>=pack->resources || !buffer) return 0;
    const nm_texture_record_t *r=pack->textures+resource;
    if (bytes!=r->size || bytes<32) return 0;
    const uint8_t *dt=buffer;
    const uint32_t mode = u32(dt+16), entries = (uint32_t)dt[10] + 1;
    const uint32_t pixels = (uint32_t)r->width * r->height;
    if (memcmp(dt, "DcTx", 4) || u32(dt+4) != r->size || dt[8] || dt[9] ||
        u16(dt+12) != r->width || u16(dt+14) != r->height ||
        r->width != (8u << ((mode >> 3) & 7)) || r->height != (8u << (mode & 7))) return 0;
    const int mipmapped=!!(mode & (1u<<31));
    if (pack->palette_index) {
        /* DNT6: twiddled PAL4 only, 16 colours, whole 32-byte rows. A square
         * mip chain is one byte of lead-in, then 1x1 (one byte) up to the base. */
        if ((mode & ~0x80000000u) != (0x28000000u | (mode & 0x3f)) || dt[10] || dt[11] != 15 ||
            (mipmapped && r->width != r->height)) return 0;
        uint32_t payload = pixels/2;
        if (mipmapped) { payload = 2; for (uint32_t side = 2; side <= r->width; side *= 2) payload += side*side/2; }
        return 32 + ((payload + 31) & ~31u) == r->size;
    }
    if (mode & ~0xc80007ffu) return 0;
    /* Offline square VQ mip chains only. Keep raw/small-codebook mip layouts
     * out until their distinct offsets have independent encoder checks. */
    if(mipmapped && (!(mode & (1u<<30)) || entries!=256 || r->width!=r->height)) return 0;
    if (mode & (1u<<30)) {
        const uint32_t indices=mipmapped ? (pixels-1)/3+1 : pixels/4;
        if (((32 + entries*8 + indices + 31) & ~31u) != r->size) return 0;
        for (unsigned i = 0; i < indices; ++i) if (dt[32 + entries*8 + i] < 256-entries) return 0;
    } else {
        /* Exact twiddled RGB565/ARGB1555 fallback; no palettes, mips or stride.
         * Keep the existing pack/staging/VRAM ceilings, even for raw images. */
        if (dt[10] || 32 + pixels*2 != r->size) return 0;
    }
    if(pack->indexed_layouts) return !memcmp(dt,pack->indexed_layouts[r->offset].header,32);
    /* ponytail: startup-only resources*bindings scan; pre-index formats if
     * measured sector loading makes this material at larger inventories. */
    for (unsigned i = 0; i < pack->count; ++i) {
        const nm_material_binding_t *b = &pack->bindings[i];
        if (b->resource == resource && (((b->header[0] >> 13) & 3) == 2) != !!(mode & (1u<<27))) return 0;
    }
    return 1;
}

const nm_material_binding_t *nm_material_find(const nm_material_pack_t *pack, const uint16_t header[4], unsigned luma) {
    if (!pack || !pack->data || luma > 255) return NULL;
    uint16_t key[5];
    nm_memcpy(key, header, 8); key[4] = (uint16_t)luma;
    unsigned low = 0, high = pack->count;
    while (low < high) {
        const unsigned mid = low + (high-low)/2;
        const int cmp = memcmp(key, &pack->bindings[mid], 10);
        if (!cmp) return &pack->bindings[mid];
        if (cmp < 0) high = mid; else low = mid+1;
    }
    return NULL; // Missing lights are explicit, never nearest-variant guesses.
}

void nm_material_find_row(const nm_material_pack_t *pack, const uint16_t header[4], const nm_material_binding_t *row[256]) {
    for (unsigned l = 0; l < 256; ++l) row[l] = NULL;
    if (!pack || !pack->data) return;
    uint16_t key[5];
    nm_memcpy(key, header, 8); key[4] = 0;
    unsigned low = 0, high = pack->count; /* the first binding not below (header, luma 0) */
    while (low < high) {
        const unsigned mid = low + (high-low)/2;
        if (memcmp(&pack->bindings[mid], key, 10) < 0) low = mid+1; else high = mid;
    }
    for (; low < pack->count && !memcmp(pack->bindings[low].header, key, 8); ++low)
        if (pack->bindings[low].luma < 256) row[pack->bindings[low].luma] = &pack->bindings[low];
}
