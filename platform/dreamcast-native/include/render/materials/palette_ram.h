#ifndef DAYTONA_NATIVE_PALETTE_RAM_H
#define DAYTONA_NATIVE_PALETTE_RAM_H
/* The one palette RAM format of the program (PAL_RAM_CTRL, KOS
 * pvr_set_pal_format). It is one register for all 1024 entries (KOS
 * dc/pvr/pvr_regs.h PVR_PALETTE_CFG, 0x0108; dc/pvr/pvr_pal.h: every entry is
 * 32 bits whatever the format), so no bank can hold a format of its own. The
 * format is chosen at build time, set once at startup (nm_palette_ram_init,
 * before the first entry is stored) and never changed, so no entry is ever
 * stored under one format and read under another; every writer of this program goes through
 * nm_palette_ram_write, which takes ARGB8888 source colours and stores them in
 * that format.
 *   default            ARGB8888: entries as the packs hold them.
 *   NM_PAL16=1555      (make NATIVE_PAL16=1555) ARGB1555: one alpha bit
 *                      (alpha >= 128), 5 bits a channel, nearest value under
 *                      the PVR's bit-repeat expansion (q << 3 | q >> 2; the
 *                      palette probe captures of 4 October 2026 matched that
 *                      expansion with 0 of 24,576 samples off). KOS's
 *                      pvr_set_pal_format (dc/pvr/pvr_pal.h) warns that a
 *                      filtered palette texture renders at about half speed
 *                      with ARGB8888 entries.
 *   NM_PAL16=565       RGB565: no alpha. Every texture of this program is a
 *                      palette texture and the punch-through list needs
 *                      transparent entries (HUD, finish actor, cutouts), so
 *                      local.cfg.mk refuses it; the encoding exists for the
 *                      host tests only.
 * In the 16-bit formats an entry is still one 32-bit word of which only the
 * low 16 bits are valid (PALETTE_RAM); KOS asks for zero high bits.
 * Constants and inline conversions only above the declarations: host tools and
 * tests include this header. */
#include <stdint.h>

#define NM_PALETTE_ARGB1555 0u /* = PVR_PAL_ARGB1555 */
#define NM_PALETTE_RGB565 1u   /* = PVR_PAL_RGB565 */
#define NM_PALETTE_ARGB8888 3u /* = PVR_PAL_ARGB8888 */
#ifndef NM_PAL16
#define NM_PALETTE_FORMAT NM_PALETTE_ARGB8888
#elif NM_PAL16 == 1555
#define NM_PALETTE_FORMAT NM_PALETTE_ARGB1555
#elif NM_PAL16 == 565
#define NM_PALETTE_FORMAT NM_PALETTE_RGB565
#else
#error "NM_PAL16 must be 1555 or 565"
#endif
#define NM_PALETTE_16BIT (NM_PALETTE_FORMAT != NM_PALETTE_ARGB8888)

/* The PVR's widening of a 5- or 6-bit channel to 8 bits (bit repeat). */
static inline unsigned nm_palette_expand5(unsigned q) { return (q << 3) | (q >> 2); }
static inline unsigned nm_palette_expand6(unsigned q) { return (q << 2) | (q >> 4); }
/* The 5-bit (bits=5) or 6-bit (bits=6) value whose expansion is nearest to the
 * 8-bit v; on a tie the lower one. */
static inline unsigned nm_palette_narrow(unsigned v, unsigned bits) {
    const unsigned top = (1u << bits) - 1u;
    unsigned q = (v * top + 127u) / 255u, best = q, error = 256u;
    for (unsigned k = q ? q - 1u : 0u; k <= q + 1u && k <= top; ++k) {
        const unsigned e = bits == 5 ? nm_palette_expand5(k) : nm_palette_expand6(k);
        const unsigned d = e > v ? e - v : v - e;
        if (d < error) { error = d; best = k; }
    }
    return best;
}
/* One ARGB8888 source colour as the entry word of format `format`. */
static inline uint32_t nm_palette_encode_as(uint32_t argb, unsigned format) {
    const unsigned r = (argb >> 16) & 255u, g = (argb >> 8) & 255u, b = argb & 255u;
    if (format == NM_PALETTE_ARGB1555)
        return (argb >> 31) << 15 | nm_palette_narrow(r, 5) << 10 | nm_palette_narrow(g, 5) << 5 | nm_palette_narrow(b, 5);
    if (format == NM_PALETTE_RGB565)
        return nm_palette_narrow(r, 5) << 11 | nm_palette_narrow(g, 6) << 5 | nm_palette_narrow(b, 5);
    return argb;
}
/* An entry word of format `format` as the ARGB8888 colour the PVR reads. */
static inline uint32_t nm_palette_decode_as(uint32_t entry, unsigned format) {
    if (format == NM_PALETTE_ARGB1555)
        return (entry & 0x8000u ? 0xff000000u : 0u) | nm_palette_expand5((entry >> 10) & 31u) << 16 |
               nm_palette_expand5((entry >> 5) & 31u) << 8 | nm_palette_expand5(entry & 31u);
    if (format == NM_PALETTE_RGB565)
        return 0xff000000u | nm_palette_expand5((entry >> 11) & 31u) << 16 |
               nm_palette_expand6((entry >> 5) & 63u) << 8 | nm_palette_expand5(entry & 31u);
    return entry;
}
static inline uint32_t nm_palette_encode(uint32_t argb) { return nm_palette_encode_as(argb, NM_PALETTE_FORMAT); }

/* Startup, before any entry is written: sets PAL_RAM_CTRL to the build's
 * format and prints it. 0 if the register does not read back as written. */
int nm_palette_ram_init(void);
/* count ARGB8888 colours to entries first.. in the build's format, then
 * nm_palette_ram_verify. owner names the log line (NULL: no line). */
int nm_palette_ram_write(unsigned first, const uint32_t *argb, unsigned count, const char *owner);
/* Entries first.. hold these ARGB8888 colours in the build's format?
 * Two checks: what this program wrote there (a copy kept on the CPU: always
 * fatal when different) and what the PVR reads back. A readback difference is
 * fatal with ARGB8888 entries, as before; with 16-bit entries the low 16 bits
 * are compared and a difference is counted and printed but not fatal: the
 * text describes no readback of the 16-bit formats (and lists unreliable
 * palette RAM reads on early Holly revisions). 0 also for a range outside
 * palette RAM or a format register that changed since startup. */
int nm_palette_ram_verify(unsigned first, const uint32_t *argb, unsigned count, const char *owner);
/* A DPAL blob ("DPAL", colours, then the ARGB8888 colours) to entries first.. */
int nm_palette_ram_write_dpal(const void *blob, unsigned first, const char *owner);
/* The same from a DPAL file (load time). */
int nm_palette_ram_write_dpal_file(const char *path, unsigned first, const char *owner);
/* Entry i as the PVR reads it back (16-bit formats: the low 16 bits), for
 * "left as it was" comparisons. */
uint32_t nm_palette_ram_read(unsigned index);
/* A scene loaded in the frames of another screen (scene_loader.h): from nm_palette_ram_defer on, writes go to the
 * program's copy only (verify and read see them there) until nm_palette_ram_flush stores them all, after the
 * screen that shows its own palettes has been released. The flush reads them back (16-bit formats: not fatal). */
void nm_palette_ram_defer(void);
int nm_palette_ram_flush(void);
/* Name of the build's format, for log lines. */
const char *nm_palette_ram_format_name(void);
#endif
