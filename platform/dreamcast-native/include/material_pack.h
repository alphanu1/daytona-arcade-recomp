#ifndef DAYTONA_NATIVE_MATERIAL_PACK_H
#define DAYTONA_NATIVE_MATERIAL_PACK_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "indexed_texture.h"
#include "nm_mem.h"

/* DNT1/2: 32-byte LE header, 16-byte resource records, 16-byte sorted bindings,
 * zero padding to 32 bytes, then consecutive unchanged DcTx files.
 * Default loader retains metadata only; the explicit CPU-backed car loader
 * keeps prepared texture blobs in RAM for a bounded per-frame VRAM bank.
 * DcTx accepts twiddled base-level RGB565/ARGB1555, VQ or uncompressed.
 * No source proofs/palettes/ROM parser. Full-course residency remains separate. */
#define NM_TEXTURE_PAYLOAD_MAX (3u * 1024u * 1024u)
#define NM_MATERIAL_FILE_MAX (4u * 1024u * 1024u)
#define NM_MATERIAL_RESOURCES_MAX 2048u
#define NM_MATERIAL_BINDINGS_MAX 8192u
#define NM_MATERIAL_SOLID 65535u
/* DNT3 is CPU-only: same records/cropped bindings, size remains decoded DcTx
 * size; stored is zlib stream length (including wrapper/checksum). Each stream
 * has zero padding to 32 bytes. Stored deflate blocks trade RAM for decode time;
 * CPU backing is capped at 9 MiB (10 MiB including metadata/file allowance).
 * DNT1/2 stored must be zero. No GPU format or VRAM budget change. */
typedef struct { uint32_t offset, size; uint16_t width, height; uint32_t stored; } nm_texture_record_t;
typedef struct { uint16_t header[4], luma, resource; uint32_t color; } nm_material_binding_t;
/* DNT4: uncompressed DNT2 layout, followed after bindings by count uint32_t
 * vertex ARGB values, then ordinary 32-byte padding and DcTx resources.
 * Textured values have opaque alpha (DNT6: but the translucent class, below); solid entries must be UINT32_MAX (their
 * binding.color remains authoritative). Separate lighting from crop origins.
 * DNT1/2/3 have no table and retain white textured vertex color. */
/* DNT2 textured color holds crop_x | crop_y<<16, in original texels.
 * DNT1 textured UINT32_MAX means origin0/full size. Solid ARGB is unchanged. */
/* DNT6 (tools/prepare_palette_materials.py): palette shading. DNT4 layout with
 * one twiddled PAL4 DcTx per arcade texture; after the vertex ARGB table come
 * count palette indices (uint8_t, 255 for solid, zero padded to 4 bytes), then
 * header[6] palettes of 16 ARGB8888 and header[6] 4-byte keys (bucket R, G, B,
 * lumabase | list<<7). The vertex ARGB is the fitted colour the PVR multiplies
 * with the palette entry. header[7]: bit 0 split opaque/punch-through
 * palettes, bits 8..15 saturating buckets, bit 16 mip chains. The renderer
 * owns the mapping of palette index to palette RAM bank. */
#define NM_MATERIAL_PALETTES_MAX 64u
/* DNT6 translucent class (tools/prepare_palette_materials.py): a textured header with texel 15 transparent (word 0
 * bits 13-14 = 3) and the screen-checker bit 15, the arcade's screen-door half coverage (the cars' smoke). Its
 * vertex ARGB carries alpha NM_MATERIAL_TRANSLUCENT_ALPHA; the renderer puts it on the translucent list. */
#define NM_MATERIAL_TRANSLUCENT_ALPHA 0x80u
static inline int nm_material_translucent(const uint16_t header[4]) {
    return (header[0]&0xc000u)==0xc000u && ((header[0]>>13)&3u)==3u;
}
/* How the renderer draws the translucent class (make NATIVE_SMOKE_MODE, local.cfg.mk; an experiment): 0 the
 * translucent list at alpha 0.5 (default), 1 punch-through with a checker of texels (pt-tex), 2 punch-through with a
 * checker of screen pixels (pt-screen: model_submission.h, "Translucent effects"), 3 the same checker of screen pixels
 * on the translucent list, pre-sorted back to front, each pixel the texel's or untouched (tr-screen). The pack is the
 * same for all. NM_SMOKE_PUNCH: on the punch-through list; NM_SMOKE_DOOR: the screen checker (u' and one depth per
 * face). NM_SMOKE_POINT (make NATIVE_SMOKE_POINT=1): the screen checker point sampled instead of bilinear. */
#ifndef NM_SMOKE_MODE
#define NM_SMOKE_MODE 0
#endif
#if NM_SMOKE_MODE < 0 || NM_SMOKE_MODE > 3
#error "NM_SMOKE_MODE: 0 (tr), 1 (pt-tex), 2 (pt-screen) or 3 (tr-screen)"
#endif
#define NM_SMOKE_PUNCH (NM_SMOKE_MODE == 1 || NM_SMOKE_MODE == 2)
#define NM_SMOKE_DOOR (NM_SMOKE_MODE >= 2)
#ifndef NM_SMOKE_POINT
#define NM_SMOKE_POINT 0
#endif
#if NM_SMOKE_POINT && !NM_SMOKE_DOOR
#error "NM_SMOKE_POINT needs the screen checker (NM_SMOKE_MODE 2 or 3)"
#endif
typedef struct {
    FILE *file;
    void *data;
    nm_texture_record_t *textures;
    nm_material_binding_t *bindings;
    uint32_t *modulation;
    const uint8_t *palette_index; /* DNT6: per binding. NULL for every other format. */
    const uint32_t *palettes;     /* DNT6: palette_count*16 ARGB8888. */
    uint32_t palette_count, palette_flags;
    uint32_t resources, count, payload, staging, metadata;
    uint32_t backing_bytes;
    uint32_t decoder_bytes;
    void *decoder; /* DNT3 only: one reusable, bounded zlib workspace. */
    /* DNT5: data owns the entire aligned immutable file; descriptors borrow it.
     * Resource offset/stored are layout/palette IDs, NOT file offsets. */
    nm_indexed_texture_t *indexed_layouts;
    const uint16_t *indexed_palettes;
} nm_material_pack_t;

/* Zero initialize. Failure leaves it unchanged; close is idempotent.
 * After upload, fclose(pack.file); pack.file=NULL releases the stream buffer
 * while retaining the index for lookup. Close frees the remaining index. */
int nm_material_pack_open(nm_material_pack_t *pack, const char *path);
/* Explicit CPU-backed car variant set, NOT a larger VRAM allowance. */
int nm_material_pack_open_cpu(nm_material_pack_t *pack, const char *path);
int nm_material_texture_generate(const nm_material_pack_t *pack,unsigned resource,
                                void *buffer,size_t capacity);
/* Body/wheel/window6000-tick peak534560. Two544KiB banks add64KiB VRAM;
 * world loader still enforces the independent512KiB global reserve. */
#define NM_INTRO_MATERIAL_CACHE_BYTES (544u * 1024u)
#define NM_MATERIAL_CACHE_BYTES NM_INTRO_MATERIAL_CACHE_BYTES
#define NM_MATERIAL_CACHE_BANKS 2u
#ifndef NM_WORLD_MATERIAL_BYTES
#define NM_WORLD_MATERIAL_BYTES (109u * 32768u)
#endif
/* Build once after all current resources are pinned, only when eviction is
 * needed. Caller supplies count entries; SH4 newlib qsort is heap-free.
 * Stable oldest/index
 * order matches the original linear scan. GPU completion is still required
 * before consuming an entry whose last_used exceeds completed. */
typedef struct { uint32_t last_used, resource; } nm_material_victim_t;
unsigned nm_material_eviction_order(unsigned count, const uint32_t *last_used,
    const uint32_t *allocated, uint32_t frame, nm_material_victim_t *order);
#define NM_MATERIAL_DECODE_CACHE_BYTES (512u * 1024u)
/* CPU-only FIFO ring. Offsets/cursor are logical byte positions; physical
 * address is offset & (capacity-1). Age detects overwritten entries in O(1).
 * Return 1=hit, 2=decode required, 0=invalid (unchanged). Offsets start ~0.
 * GPU receives a copy; no CPU pointer survives the immediate upload.
 * ponytail: FIFO, not LRU; revisit only if measured gameplay thrashes. */
static inline int nm_material_decode_reserve(const nm_material_pack_t *pack, unsigned resource,
        uint32_t *offsets, uint32_t *used, uint32_t capacity) {
    if (!pack || !offsets || !used || resource>=pack->resources || (*used&31) ||
        capacity<64 || capacity>UINT32_MAX/2 || (capacity&(capacity-1))) return 0;
    const uint32_t size=pack->textures[resource].size;
    if (size<64 || (size&31) || size>capacity) return 0;
    /* Prevent stale hits when a long-running session exhausts the logical
     * counter. This rare reset is explicit, not modulo-age aliasing. */
    if (*used>UINT32_MAX-2*capacity) {
        nm_memset(offsets,255,pack->resources*sizeof(*offsets)); *used=0;
    }
    const uint32_t at=offsets[resource];
    if (at!=UINT32_MAX && at<=*used && *used-at>=size && *used-at<=capacity) return 1;
    const uint32_t physical=*used&(capacity-1);
    if (size>capacity-physical) *used+=capacity-physical;
    offsets[resource]=*used; *used+=size; return 2;
}
/* offsets[] starts at UINT32_MAX each frame. Reserve each resource once,
 * 32-byte aligned from validated DNT records; failure changes nothing. */
static inline int nm_material_cache_reserve(const nm_material_pack_t *pack, unsigned resource,
        uint32_t *offsets, uint32_t *used, uint32_t capacity) {
    if (!pack || !offsets || !used || resource>=pack->resources || (*used&31) || *used>capacity) return 0;
    if (offsets[resource]!=UINT32_MAX) return 1;
    const uint32_t size=pack->textures[resource].size;
    if (size<64 || (size&31) || size-32>capacity-*used) return 0;
    offsets[resource]=*used; *used+=size-32; return 1;
}
void nm_material_pack_close(nm_material_pack_t *pack);
int nm_material_texture_read(nm_material_pack_t *pack, unsigned resource, void *buffer, size_t capacity);
/* DNT3 decode from immutable CPU backing. No files/heap/texture conversion.
 * Validate decoded DcTx once at startup before admitting it to the cache. */
int nm_material_texture_decode(nm_material_pack_t *pack, unsigned resource,
    const void *source, size_t bytes, void *buffer, size_t capacity);
int nm_material_texture_validate(const nm_material_pack_t *pack, unsigned resource,
    const void *buffer, size_t bytes);
const nm_material_binding_t *nm_material_find(const nm_material_pack_t *pack, const uint16_t header[4], unsigned luma);
/* row[l] = nm_material_find(pack, header, l) for every luma 0..255, in one search: the bindings are strictly ordered
 * (checked at open, luma <= 255), so one header's bindings are one run in luma order. */
void nm_material_find_row(const nm_material_pack_t *pack, const uint16_t header[4], const nm_material_binding_t *row[256]);

/* Requires a validated binding belonging to this pack, like nm_material_uv. */
static inline uint32_t nm_material_argb(const nm_material_pack_t *pack, const nm_material_binding_t *b) {
    if (b->resource == NM_MATERIAL_SOLID) return b->color;
    return pack->modulation ? pack->modulation[b - pack->bindings] : UINT32_MAX;
}

/* Palette index of a binding of this pack, or -1 (solid, or not a DNT6 pack). */
static inline int nm_material_palette(const nm_material_pack_t *pack, const nm_material_binding_t *b) {
    if (!pack->palette_index || b->resource == NM_MATERIAL_SOLID) return -1;
    return pack->palette_index[b - pack->bindings];
}

/* Palette RAM is shared between the DNT6 packs of one scene: a palette of
 * `pack` whose 16 entries equal a palette of `base` (the car pack, or NULL)
 * uses that palette's slot; every other one takes the next free slot after
 * base's. slots[] receives one slot per palette of pack; the return value is
 * the number of slots in use by both packs, or 0 if it exceeds `capacity`.
 * Slot + first bank of the scene = palette RAM bank. Pure table work, shared
 * by the renderer and the host tools. */
static inline unsigned nm_material_palette_share(const nm_material_pack_t *base,
        const nm_material_pack_t *pack, uint8_t *slots, unsigned capacity) {
    unsigned used = base && base->palette_index ? base->palette_count : 0;
    const unsigned shared = used;
    if (!pack->palette_index || used > capacity) return 0;
    for (unsigned i = 0; i < pack->palette_count; ++i) {
        unsigned slot = 0;
        while (slot < shared && memcmp(base->palettes + 16*slot, pack->palettes + 16*i, 64)) ++slot;
        if (slot == shared) {
            /* Equal palettes inside pack share their new slot too. */
            unsigned earlier = 0;
            while (earlier < i && memcmp(pack->palettes + 16*earlier, pack->palettes + 16*i, 64)) ++earlier;
            if (earlier < i) slot = slots[earlier];
            else { if (used == capacity) return 0; slot = used++; }
        }
        slots[i] = (uint8_t)slot;
    }
    return used;
}

typedef struct {
    float scale[2], origin[2]; /* source UV units -> normalized cropped UV */
    uint16_t low[2], high[2]; /* one-texel halo, high exclusive */
    unsigned cropped;
} nm_material_uv_t;

/* Requires a validated binding from this pack. Shared by target and host check. */
static inline nm_material_uv_t nm_material_uv(const nm_material_pack_t *pack, const nm_material_binding_t *b) {
    nm_material_uv_t out = {0};
    for (unsigned a = 0; a < 2; ++a) {
        const unsigned original = 32u << ((b->header[0] >> (a*3)) & 7);
        unsigned extent = original, origin = 0;
        if (b->resource != NM_MATERIAL_SOLID) {
            const nm_texture_record_t *r = &pack->textures[b->resource];
            extent = a ? r->height : r->width;
            if (b->color != UINT32_MAX) origin = (b->color >> (a*16)) & 65535;
        }
        out.scale[a] = 1.0f / (8u*extent);
        out.origin[a] = (float)(origin*8);
        if (extent != original) {
            out.cropped |= 1u << a;
            out.low[a] = (uint16_t)((origin+1)*8);
            out.high[a] = (uint16_t)((origin+extent-1)*8);
        }
    }
    return out;
}

static inline int nm_material_uv_contains(const nm_material_uv_t *map, unsigned u, unsigned v) {
    return (!(map->cropped & 1) || (u >= map->low[0] && u < map->high[0])) &&
           (!(map->cropped & 2) || (v >= map->low[1] && v < map->high[1]));
}
#endif
