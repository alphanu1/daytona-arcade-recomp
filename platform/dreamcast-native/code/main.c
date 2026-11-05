#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>
#include <malloc.h>
#include <stdio.h>
#ifdef NATIVE_DISC
#include <kos/dbgio.h>
#endif
#include "practice.h"
#include "render/materials/palette_ram.h"
#ifndef NM_ASSET_ROOT /* make NATIVE_ASSET_ROOT=..., /cd/ with NATIVE_DISC=1 (local.cfg.mk) */
#define NM_ASSET_ROOT "/pc/"
#endif
#include "../tests/startup_check.h"
#ifndef NM_PT_CHUNK
#define NM_PT_CHUNK 512
#endif
_Static_assert(NM_PT_CHUNK >= 32 && NM_PT_CHUNK <= 512, "ISP_FEED_CFG punch-through chunk, vertices");
#ifdef NM_PT_ALPHA_REF
_Static_assert(NM_PT_ALPHA_REF >= 1 && NM_PT_ALPHA_REF <= 255, "PT_ALPHA_REF: 1..255");
#ifndef PVR_PT_ALPHA_REF
#define PVR_PT_ALPHA_REF 0x011c
#endif
#endif
#ifdef NM_DIAG_LISTS
/* make NATIVE_DIAG_LISTS=3|2 (measurement of the PVR render time). KOS gives
 * every list an object pointer block per tile and points the region array at
 * it; a list that receives nothing still ends, per tile, in an "end of list"
 * word the core has to fetch. With PVR_BINSIZE_0 KOS writes "No List" to
 * TA_ALLOC_CTRL and the Empty PTR bit into that list's pointer of every
 * tile. enDjinn opens all five lists every frame (enj_render.c) and KOS
 * sends a blank header to each (pvr_list_finish and pvr_scene_finish call
 * pvr_blank_polyhdr, kernel/arch/dreamcast/hardware/pvr/pvr_scene.c), a list
 * entry for a list that has no object pointer blocks to file it into
 * (PVR_BINSIZE_0, pvr_buffers.c), and KOS starts the render only when
 * the lists that reported "done" are exactly the enabled ones. So the two
 * calls are wrapped at link time (-Wl,--wrap, local.cfg.mk): a closed list is
 * neither opened nor ended. Nothing may be queued on such a list: the race
 * scenes queue on the opaque, opaque modifier and punch-through lists only. */
_Static_assert(NM_DIAG_LISTS == 2 || NM_DIAG_LISTS == 3, "lists left open");
#if NM_DIAG_LISTS == 2 && !defined(NM_DIAG_NO_SHADOW_VOLUMES)
#error "NM_DIAG_LISTS=2 closes the opaque modifier list: build without shadow volumes"
#endif
static int diag_list_closed(pvr_list_t list) {
    return list == PVR_LIST_TR_POLY || list == PVR_LIST_TR_MOD || (NM_DIAG_LISTS == 2 && list == PVR_LIST_OP_MOD);
}
int __real_pvr_list_begin(pvr_list_t list);
int __real_pvr_list_finish(void);
static int diag_list_skipped;
int __wrap_pvr_list_begin(pvr_list_t list) {
    if (diag_list_closed(list)) { diag_list_skipped = 1; return 0; }
    return __real_pvr_list_begin(list);
}
int __wrap_pvr_list_finish(void) {
    if (diag_list_skipped) { diag_list_skipped = 0; return 0; }
    return __real_pvr_list_finish();
}
#endif

/* ROM-free hardware check, not a claim about TGP arithmetic equivalence. */
static int check_math(void) {
    shz_xmtrx_init_scale(2.0f, -3.0f, 4.0f);
    const shz_vec4_t v = shz_xmtrx_transform_vec4(
        shz_vec4_init(3.0f, -4.0f, 5.0f, 1.0f));
    return v.x == 6.0f && v.y == 12.0f && v.z == 20.0f && v.w == 1.0f;
}

int main(void) {
#if defined(NATIVE_DISC) && !defined(NM_DISC_CONSOLE)
    /* From a disc nobody reads the console: KOS would write it to the serial port, waiting on the port's FIFO at
     * 115200 baud (a long report line about 10 ms). make NATIVE_DISC_CONSOLE=1 keeps it (a serial cable, Flycast). */
    dbgio_dev_select("null");
#endif
    if(nm_startup_check()) return 1;
    if (!check_math()) {
        puts("FAIL: SH4ZAM scale check");
        return 1;
    }
    enj_state_init_defaults();
    /* Bounded3500-triangle stream plus64KiB HUD/list reserve, two TA sets. */
    enj_state_get()->video.pvr_params.vertex_buf_size=512*1024;
    /* Keep all five enDjinn lists and overflow buffers. Object pointer blocks of
     * 16 words (default since 16 October 2026): a tile's list links to an
     * overflow block after 15 entries instead of 7, which the ISP walks
     * faster. Hardware, fast race, interleaved A/B with identical checkpoints:
     * render mean 9.56 -> 9.37 ms, frames over 12 ms 520 -> 345; about 100 KB
     * more VRAM. make NATIVE_OPB=8 restores the former size. */
#ifndef NM_OPB_WORDS
#define NM_OPB_WORDS PVR_BINSIZE_16
#endif
    for(unsigned list=0;list<5;++list)
        enj_state_get()->video.pvr_params.opb_sizes[list]=NM_OPB_WORDS;
#ifdef NM_DIAG_OPB
    /* make NATIVE_DIAG_OPB=16|32 (measurement): object pointer blocks of that size. */
    for(unsigned list=0;list<5;++list)
        enj_state_get()->video.pvr_params.opb_sizes[list]=NM_DIAG_OPB;
#endif
#ifdef NM_DIAG_LISTS
    for(unsigned list=0;list<5;++list)
        if(diag_list_closed((pvr_list_t)list)) enj_state_get()->video.pvr_params.opb_sizes[list]=PVR_BINSIZE_0;
#endif
#if !defined(NM_TR_AUTOSORT) || !NM_TR_AUTOSORT
    /* The translucent list in pre-sort mode (the pre-sort bit of every tile of
     * the region array, pvr_init_tile_matrices): polygons in submission order,
     * no per-pixel sort. Its polygons are the car effects' smoke, which the
     * renderer sorts back to front by each polygon's nearest depth, the
     * arcade's painter key (model_submission.h, "Translucent effects"), and the
     * HUD's (the CONDITION panel with NATIVE_HUD_RADAR_PANEL=1, registered after
     * the models: drawn over them). The probes of build/dc-native-gpu-split
     * measured 9.5 ns a sample pre-sorted against 9.7 auto-sorted at two layers,
     * and auto-sort repeats its passes for deeper stacks (a spin cloud: 12..37).
     * make NATIVE_TR_AUTOSORT=1 keeps the auto-sort mode (measurement). The
     * fill probe's presort|add forms (NATIVE_DIAG_FILL_PROBE) need this mode. */
    enj_state_get()->video.pvr_params.autosort_disabled=1;
#elif defined(NM_DIAG_FILL_PROBE) && NM_DIAG_FILL_PROBE >= 3
#error "NATIVE_DIAG_FILL_PROBE=presort|add needs the pre-sort mode (NATIVE_TR_AUTOSORT=0)"
#endif
    enj_state_get()->video.display_mode = DM_640x480;
    enj_state_get()->video.pixel_mode = PM_RGB888P;
#ifdef NM_DIAG_FB565
    /* make NATIVE_DIAG_FB565=1: ONLY a measurement of what the frame buffer's
     * pixel format costs in render time. The product keeps 24 bits. */
    enj_state_get()->video.pixel_mode = PM_RGB565;
#endif
    if (enj_state_startup() != 0) return 1;
    /* Palette RAM format (render/materials/palette_ram.h; make NATIVE_PAL16):
     * the register decides how each entry is stored, so it is set once here,
     * before any scene writes a palette entry, and never again. */
    if (!nm_palette_ram_init()) return 1;
#if defined(NM_DIAG_OPB) || defined(NM_DIAG_LISTS) || defined(NM_DIAG_FB565)
    printf("DIAG PVR setup: object pointer block words %d/%d/%d/%d/%d (opaque, opaque modifier, translucent, translucent modifier, punch-through), frame buffer %s\n",
           enj_state_get()->video.pvr_params.opb_sizes[0], enj_state_get()->video.pvr_params.opb_sizes[1],
           enj_state_get()->video.pvr_params.opb_sizes[2], enj_state_get()->video.pvr_params.opb_sizes[3],
           enj_state_get()->video.pvr_params.opb_sizes[4],
           enj_state_get()->video.pixel_mode == PM_RGB565 ? "RGB565 (measurement only)" : "RGB888 packed");
#endif
#ifdef NM_DIAG_NO_SKY
    /* make NATIVE_DIAG_NO_SKY=1 (measurement): the sky quads are not queued
     * (render/scene/practice.c); the background plane shows in one colour. */
    pvr_set_bg_color(0.31f, 0.50f, 0.75f);
    puts("DIAG no sky: flat background plane");
#endif
    /* Punch-through chunk size of the tile renderer: ISP_FEED_CFG (PVR
     * register 0x098, KOS name PVR_ISP_FEED_CFG, before KOS 368a2eb9 PVR_UNK_0098), bits 13..4, in vertices. Per
     * tile the ISP takes the punch-through polygons in chunks of this many
     * vertices and repeats its passes per chunk, so a larger chunk means
     * fewer passes. pvr_init writes 0x00800408: translucent cache 0x200
     * vertices (bits 23..14), chunk 0x40, discard mode (bit 3), autosort
     * (bit 0 clear). Only the chunk field changes here; it has to be
     * written after enj_state_startup, which runs pvr_init. Default 512
     * (make NATIVE_PT_CHUNK=32|64|128|256|512): on hardware, stage 5
     * scripted race of 13 October 2026, 512 rendered in 11.9 ms mean with
     * 1,161 late frames, KOS's 64 in 12.0 ms with 1,302, 32 in 12.3 ms. */
#ifndef PVR_ISP_FEED_CFG
#define PVR_ISP_FEED_CFG PVR_UNK_0098 /* the register's name before KOS 368a2eb9 */
#endif
    PVR_SET(PVR_ISP_FEED_CFG, 0x00800008u | ((unsigned)NM_PT_CHUNK << 4));
    printf("ISP_FEED_CFG=%08lx (punch-through chunk %u vertices)\n",
           (unsigned long)PVR_GET(PVR_ISP_FEED_CFG), (unsigned)NM_PT_CHUNK);
#ifdef NM_PT_ALPHA_REF
    /* Punch-through alpha reference: PT_ALPHA_REF (PVR register 0x11C, at
     * 0xA05F811C), the alpha a punch-through pixel needs to be drawn. Its
     * reset value 0xFF draws only fully opaque samples: with point sampling
     * every texel is 0 or 0xFF and the value changes nothing, but a bilinear
     * cutout (make NATIVE_FILTER_ALL=1) then loses every sample that mixes in
     * a transparent texel, so silhouettes erode by half a texel and thin
     * parts vanish. 0x80 (the default of NATIVE_FILTER_ALL=1; make
     * NATIVE_PT_ALPHA_REF=1..255) puts the edge where the texel edge is. One
     * register for every punch-through polygon: the point-sampled ones (HUD,
     * finish actor, title, the catch-fence stipple) draw the same pixels at
     * any value as long as their texels' alpha is 0 or 0xFF, which ARGB1555
     * palette RAM (make NATIVE_PAL16=1555, default) guarantees. pvr_init does
     * not write it; nothing else does. Without the option it is left alone. */
    PVR_SET(PVR_PT_ALPHA_REF, (uint32_t)NM_PT_ALPHA_REF);
    printf("PT_ALPHA_REF=%02lx\n", (unsigned long)(PVR_GET(PVR_PT_ALPHA_REF) & 0xffu));
#endif
    printf("Native FSAA: %d, horizontal vertex scale=%d\n",ENJ_FSAA,ENJ_XSCALE);
    vid_set_dithering(false);
    return nm_practice_run(NM_ASSET_ROOT);
}
