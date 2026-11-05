/* Palette RAM writes of the whole program in one format (palette_ram.h). */
#include "render/materials/palette_ram.h"
#include <enDjinn/enj_enDjinn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nm_mem.h"

_Static_assert(NM_PALETTE_ARGB1555 == PVR_PAL_ARGB1555 && NM_PALETTE_RGB565 == PVR_PAL_RGB565 &&
               NM_PALETTE_ARGB8888 == PVR_PAL_ARGB8888, "palette format numbers are PAL_RAM_CTRL's");

/* What this program wrote to each entry (the entry word), and whether it did. */
static uint32_t written[1024], valid[1024 / 32];
#define PALETTE_VALID(e) ((valid[(e) >> 5] >> ((e) & 31u)) & 1u)
static unsigned initialised;
/* nm_palette_ram_defer: entries written but held back from the PVR until nm_palette_ram_flush. */
static uint32_t pending[1024 / 32];
static unsigned deferring;
#define PALETTE_PENDING(e) ((pending[(e) >> 5] >> ((e) & 31u)) & 1u)

const char *nm_palette_ram_format_name(void) {
    return NM_PALETTE_FORMAT == NM_PALETTE_ARGB1555 ? "ARGB1555" :
           NM_PALETTE_FORMAT == NM_PALETTE_RGB565 ? "RGB565" : "ARGB8888";
}

static int palette_format_ok(void) {
    return (PVR_GET(PVR_PALETTE_CFG) & 3u) == NM_PALETTE_FORMAT;
}

int nm_palette_ram_init(void) {
    /* Before the first entry: the register decides how a write is stored. */
    pvr_set_pal_format((pvr_palfmt_t)NM_PALETTE_FORMAT);
    initialised = palette_format_ok();
    printf("Native palette RAM: %s (PAL_RAM_CTRL=%lu), set once before any entry is written%s\n",
           nm_palette_ram_format_name(), (unsigned long)(PVR_GET(PVR_PALETTE_CFG) & 3u),
           initialised ? "" : "; FAIL: the register does not read back");
    return initialised;
}

uint32_t nm_palette_ram_read(unsigned index) {
    if (PALETTE_PENDING(index & 1023u)) return written[index & 1023u]; /* what the flush will store */
    const uint32_t word = PVR_GET(PVR_PALETTE_TABLE_BASE + 4 * (index & 1023u));
    return NM_PALETTE_16BIT ? word & 0xffffu : word;
}

int nm_palette_ram_verify(unsigned first, const uint32_t *argb, unsigned count, const char *owner) {
    if (!initialised || first > 1024u || count > 1024u - first || !argb) return 0;
    if (!palette_format_ok()) {
        printf("FAIL: palette format register changed since startup (%s)\n", owner ? owner : "palette");
        return 0;
    }
    unsigned differences = 0, high = 0, at = 0;
    uint32_t read_at = 0;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned e = first + i;
        const uint32_t want = nm_palette_encode(argb[i]);
        if (!PALETTE_VALID(e) || written[e] != want) {
            printf("FAIL: palette entry %u was written as %08lx, not as the %s entry %08lx of %s\n", e,
                   PALETTE_VALID(e) ? (unsigned long)written[e] : 0ul, nm_palette_ram_format_name(),
                   (unsigned long)want, owner ? owner : "this palette");
            return 0;
        }
        if (PALETTE_PENDING(e)) continue; /* held back: the flush reads it back */
        const uint32_t word = PVR_GET(PVR_PALETTE_TABLE_BASE + 4 * e);
        if (NM_PALETTE_16BIT && (word >> 16)) ++high;
        if ((NM_PALETTE_16BIT ? word & 0xffffu : word) != want) {
            if (!differences++) { at = e; read_at = word; }
        }
    }
    if (differences) {
        printf("%s: palette RAM readback of %s differs in %u of %u entries (first %u: read %08lx, written %08lx)%s\n",
               NM_PALETTE_16BIT ? "Native palette RAM" : "FAIL", owner ? owner : "palette", differences, count, at,
               (unsigned long)read_at, (unsigned long)written[at],
               NM_PALETTE_16BIT ? "; not fatal with 16-bit entries" : "");
        if (!NM_PALETTE_16BIT) return 0;
    }
    if (NM_PALETTE_16BIT && high)
        printf("Native palette RAM: %u of %u entries of %s read back with high bits set (written as zero)\n", high,
               count, owner ? owner : "palette");
    return 1;
}

int nm_palette_ram_write(unsigned first, const uint32_t *argb, unsigned count, const char *owner) {
    if (!initialised || first > 1024u || count > 1024u - first || !argb) return 0;
    if (!palette_format_ok()) {
        printf("FAIL: palette format register changed since startup (%s)\n", owner ? owner : "palette");
        return 0;
    }
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t word = nm_palette_encode(argb[i]);
        if (deferring) pending[(first + i) >> 5] |= 1u << ((first + i) & 31u);
        else {
            pvr_set_pal_entry(first + i, word);
            pending[(first + i) >> 5] &= ~(1u << ((first + i) & 31u));
        }
        written[first + i] = word;
        valid[(first + i) >> 5] |= 1u << ((first + i) & 31u);
    }
    return nm_palette_ram_verify(first, argb, count, owner);
}

void nm_palette_ram_defer(void) { deferring = 1; }

int nm_palette_ram_flush(void) {
    deferring = 0;
    unsigned count = 0, differences = 0;
    for (unsigned e = 0; e < 1024u; ++e) {
        if (!PALETTE_PENDING(e)) continue;
        pvr_set_pal_entry(e, written[e]);
        pending[e >> 5] &= ~(1u << (e & 31u));
        ++count;
        const uint32_t word = PVR_GET(PVR_PALETTE_TABLE_BASE + 4 * e);
        differences += (NM_PALETTE_16BIT ? word & 0xffffu : word) != written[e];
    }
    if (count)
        printf("Native palette RAM: %u entries written after the screen before (%u read back differently%s)\n", count,
               differences, differences && NM_PALETTE_16BIT ? "; not fatal with 16-bit entries" : "");
    return !differences || NM_PALETTE_16BIT;
}

int nm_palette_ram_write_dpal(const void *blob, unsigned first, const char *owner) {
    if (!blob) return 0;
    uint32_t head[2];
    nm_memcpy(head, blob, sizeof(head));
    if (head[0] != 0x4c415044u || !head[1] || head[1] > 1024u) return 0; /* "DPAL" */
    return nm_palette_ram_write(first, (const uint32_t *)blob + 2, head[1], owner);
}

int nm_palette_ram_write_dpal_file(const char *path, unsigned first, const char *owner) {
    FILE *file = path ? fopen(path, "rb") : NULL;
    uint32_t *blob = file ? nm_alloc(4 * (2 + 1024)) : NULL; /* load time only */
    if (!blob) { if (file) fclose(file); return 0; }
    const size_t words = fread(blob, 4, 2 + 1024, file);
    const int end = feof(file) && !ferror(file);
    fclose(file);
    const int ok = end && words >= 2 && blob[1] <= 1024u && words == 2 + blob[1] &&
                   nm_palette_ram_write_dpal(blob, first, owner);
    free(blob);
    return ok;
}
