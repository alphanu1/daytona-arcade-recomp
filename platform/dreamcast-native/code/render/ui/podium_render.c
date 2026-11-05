/* The podium's 2D screen: include/render/ui/podium_render.h. */
#include "render/ui/podium_render.h"
#include "render/materials/palette_ram.h"
#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
#if __has_include("podium_screen_data.h")
#include "podium_screen_data.h" /* NATIVE_PODIUM_SCREEN_DIR (local.cfg.mk): tools/prepare_podium_screen.py */
#include "nm_mem.h"
#endif

#ifdef NM_PODIUM_SCREEN_DATA
/* b948/ba18 constants (prepare_podium_screen.py's docstring): */
#define PODIUM_SPAWN_TIMER 1598   /* k = 0: the first ba18 pass */
#define PHOTO_FROM 1213           /* befc: +34 >= 0x4bd */
#define PHOTO_STEPS 40            /* +44 at b9b8 */
#define LOGOS_FROM 1408           /* 233bac rows 47, 48 (0x580) */
#define TEXT_FROM 1409            /* ba7c: 0x581 */
#define LAYER_A_UNTIL 1570        /* 233bac rows 49..51 and ba7c (0x622): erased */
#define BACKGROUND_TIMER 383      /* bc00 row 0: 384 (see the header) */
#define WIPE_FROM 40              /* bc00 rows 1, 2: 42 set, 21 clear; 25 more a tick (bd4c): seen from 40 on */
#define BANNER_ROWS 49            /* +48 at b9c0; the banner rests on row 1 */

static enj_texture_info_t textures[NM_PODIUM_SCREEN_PARTS];
alignas(32) static pvr_poly_hdr_t headers[NM_PODIUM_SCREEN_PARTS];
static uint8_t *static_palette, *cinematic_palette;
static unsigned loaded, ready, static_written, cinematic_written;
static int32_t frame_k, frame_timer;
static uint32_t frames;

void nm_podium_render_unload(void) {
    ready = 0;
    if (loaded && (pvr_wait_ready() || pvr_wait_render_done())) { enj_state_flag_shutdown(NULL); return; }
    while (loaded) enj_texture_unload(textures + --loaded);
    free(static_palette); static_palette = NULL;
    free(cinematic_palette); cinematic_palette = NULL;
    static_written = cinematic_written = 0;
}

int nm_podium_render_load(const char *root) {
    if (!root) return 0;
    if (loaded || ready) nm_podium_render_unload();
    char path[512];
    unsigned bytes = 0;
    for (unsigned i = 0; i < NM_PODIUM_SCREEN_PARTS; ++i)
        bytes += nm_podium_screen_parts[i].texture_width * nm_podium_screen_parts[i].texture_height / 2 + 32;
    const unsigned before = pvr_mem_available();
    if (snprintf(path, sizeof(path), "%spodium/podium-000.dt", root) >= (int)sizeof(path)) return 0;
    FILE *probe = fopen(path, "rb");
    if (!probe) { printf("Native podium screen: no %s: not drawn\n", path); return 1; }
    fclose(probe);
    if (before < bytes + 512 * 1024) { printf("FAIL podium screen: VRAM %u for %u bytes\n", before, bytes); return 0; }
    for (unsigned i = 0; i < NM_PODIUM_SCREEN_PARTS; ++i) {
        const nm_podium_part_t *p = nm_podium_screen_parts + i;
        if (snprintf(path, sizeof(path), "%spodium/podium-%03u.dt", root, i) >= (int)sizeof(path) ||
            !enj_texture_load_file(path, textures + i)) { printf("FAIL podium screen: %s\n", path); goto fail; }
        ++loaded;
        if (textures[i].width != p->texture_width || textures[i].height != p->texture_height ||
            textures[i].pvrformat != PVR_TXRFMT_PAL4BPP || p->bank > 63) {
            printf("FAIL podium screen part %u: %ux%u format %08lx\n", i, textures[i].width, textures[i].height,
                   (unsigned long)textures[i].pvrformat);
            goto fail;
        }
        const int photo = p->role == NM_PODIUM_PART_PHOTO, back = p->role == NM_PODIUM_PART_BACKGROUND;
        pvr_poly_cxt_t context;
        pvr_poly_cxt_txr(&context, photo || back ? PVR_LIST_OP_POLY : PVR_LIST_PT_POLY,
            textures[i].pvrformat | PVR_TXRFMT_4BPP_PAL(p->bank), p->texture_width, p->texture_height, textures[i].ptr,
            photo ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
        context.gen.culling = PVR_CULLING_NONE;
        context.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        /* Every part writes its depth: the photo and layer A in front of every scene polygon (the photo over
         * the one cinematic tick it overlaps, the banner over the whole cinematic: a scene polygon queued after
         * them fails its depth test), the background behind. */
        context.depth.write = PVR_DEPTHWRITE_ENABLE;
        pvr_poly_compile(headers + i, &context);
    }
    {
        /* cinematic.dp waits in RAM for the cinematic: its banks hold the HUD's (61..63 with another course's map,
         * palette_banks.h) while the race is drawn. */
        if (snprintf(path, sizeof(path), "%spodium/cinematic.dp", root) >= (int)sizeof(path)) goto fail;
        FILE *file = fopen(path, "rb");
        const size_t size = 8 + 4 * 16 * NM_PODIUM_CINEMATIC_BANKS;
        cinematic_palette = file ? nm_alloc(size + 1) : NULL;
        const int ok = cinematic_palette && fread(cinematic_palette, 1, size + 1, file) == size;
        if (file) fclose(file);
        if (!ok) { printf("FAIL podium screen: %s\n", path); goto fail; }
    }
    {
        /* static.dp waits in RAM for the cleanup (its banks are the cars' until then). */
        if (snprintf(path, sizeof(path), "%spodium/static.dp", root) >= (int)sizeof(path)) goto fail;
        FILE *file = fopen(path, "rb");
        const size_t size = 8 + 4 * 16 * NM_PODIUM_STATIC_BANKS;
        static_palette = file ? nm_alloc(size + 1) : NULL;
        const int ok = static_palette && fread(static_palette, 1, size + 1, file) == size;
        if (file) fclose(file);
        if (!ok) { printf("FAIL podium screen: %s\n", path); goto fail; }
    }
    ready = 1;
    printf("Native podium screen: %u parts resident, VRAM %u bytes (%u free); banks %u..%u from the cinematic, %u..%u after the cleanup\n",
           loaded, before - pvr_mem_available(), pvr_mem_available(), NM_PODIUM_CINEMATIC_BANK,
           NM_PODIUM_CINEMATIC_BANK + NM_PODIUM_CINEMATIC_BANKS - 1, NM_PODIUM_STATIC_BANK,
           NM_PODIUM_STATIC_BANK + NM_PODIUM_STATIC_BANKS - 1);
    return 1;
fail:
    nm_podium_render_unload();
    return 0;
}

static void vertex(float x, float y, float z, float u, float v, int last) {
    pvr_vertex_t *out = pvr_dr_target();
    out->flags = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    out->x = x * (640.0f / 496.0f) * ENJ_XSCALE; out->y = y * (480.0f / 384.0f); out->z = z;
    out->u = u; out->v = v; out->argb = 0xffffffff; out->oargb = 0;
    pvr_dr_commit(out);
}

/* A part's rectangle: whole, or line by line with the row scroll: even lines `scroll` pixels to the right,
 * odd lines to the left, nothing wrapping around (the arcade's frames 14344..14356, every layer A pixel). */
static void part(unsigned i, float y_offset, float z, int scroll) {
    const nm_podium_part_t *p = nm_podium_screen_parts + i;
    pvr_poly_hdr_t *h = pvr_dr_target(); *h = headers[i]; pvr_dr_commit(h);
    const float iu = 1.0f / p->texture_width, iv = 1.0f / p->texture_height;
    const float x = p->x, y = p->y + y_offset;
    if (!scroll) {
        for (unsigned c = 0; c < 4; ++c) {
            const float dx = c & 2 ? p->width : 0, dy = c & 1 ? p->height : 0;
            vertex(x + dx, y + dy, z, dx * iu, dy * iv, c == 3);
        }
        return;
    }
    for (unsigned line = 0; line < p->height; ++line) {
        const int screen = (int)y + (int)line;
        const float left = x + (float)(screen & 1 ? -scroll : scroll);
        if (left >= 496.0f || left + p->width <= 0.0f) continue;
        for (unsigned c = 0; c < 4; ++c) {
            const float dx = c & 2 ? p->width : 0, dy = (float)line + (c & 1 ? 1.0f : 0.0f);
            vertex(left + dx, y + dy, z, dx * iu, dy * iv, c == 3);
        }
    }
}

static void render_opaque(void *unused) {
    (void)unused;
    const int32_t k = frame_k;
    if (frame_timer <= BACKGROUND_TIMER)
        for (unsigned i = 0; i < NM_PODIUM_SCREEN_PARTS; ++i)
            if (nm_podium_screen_parts[i].role == NM_PODIUM_PART_BACKGROUND) part(i, 0.0f, 0.0001f, 0);
    if (k < PHOTO_FROM) return;
    /* befc: s = fadd(0.29, fmul(0.05, cvtir(min(n, 26)))), then the 8 x 8 quad at depth 2, focus 280. */
    const int32_t n = PHOTO_STEPS - (k - PHOTO_FROM) > 0 ? PHOTO_STEPS - (k - PHOTO_FROM) : 0;
    const float scaled = 0.05f * (float)(n < 26 ? n : 26), s = 0.29f + scaled;
    const float half = 280.0f * 4.0f * s / 2.0f, cx = 248.0f, cy = 196.0f;
    for (unsigned i = 0; i < NM_PODIUM_SCREEN_PARTS; ++i) {
        if (nm_podium_screen_parts[i].role != NM_PODIUM_PART_PHOTO) continue;
        pvr_poly_hdr_t *h = pvr_dr_target(); *h = headers[i]; pvr_dr_commit(h);
        /* The texture is stored turned: screen x runs along its rows (v), screen y along its columns (u). */
        vertex(cx - half, cy - half, 1000.0f, 0.0f, 0.0f, 0);
        vertex(cx - half, cy + half, 1000.0f, 1.0f, 0.0f, 0);
        vertex(cx + half, cy - half, 1000.0f, 0.0f, 1.0f, 0);
        vertex(cx + half, cy + half, 1000.0f, 1.0f, 1.0f, 1);
    }
    ++frames;
}

static void render_layer_a(void *unused) {
    (void)unused;
    const int32_t k = frame_k, t = frame_timer;
    if (k >= LAYER_A_UNTIL) return;
    const int scroll = t <= WIPE_FROM ? 25 * (WIPE_FROM - t) : 0;
    const float banner = (float)(8 * ((BANNER_ROWS - k > 1 ? BANNER_ROWS - k : 1) - 1));
    for (unsigned i = 0; i < NM_PODIUM_SCREEN_PARTS; ++i) {
        const unsigned role = nm_podium_screen_parts[i].role;
        if (role == NM_PODIUM_PART_BANNER) {
            if (nm_podium_screen_parts[i].y + banner < 384.0f) part(i, banner, 1000.0f, scroll);
        } else if ((role == NM_PODIUM_PART_SEGA || role == NM_PODIUM_PART_AM2) ? k >= LOGOS_FROM :
                   role == NM_PODIUM_PART_TEXT ? k >= TEXT_FROM : 0)
            part(i, 0.0f, 1000.0f, scroll);
    }
}

int nm_podium_render_enqueue(const nm_podium_t *podium) {
    if (!ready || !podium || podium->phase != 28 || podium->timer > PODIUM_SPAWN_TIMER || podium->timer < 0) return 1;
    frame_timer = podium->timer;
    frame_k = PODIUM_SPAWN_TIMER - podium->timer;
    if (frame_k < PHOTO_FROM) static_written = 0;   /* a new podium */
    if (!cinematic_written) {
        /* The cinematic's first frame: the HUD (phases 20..24) is no longer drawn, the result labels come later. */
        if (!nm_palette_ram_write_dpal(cinematic_palette, NM_PODIUM_CINEMATIC_BANK * 16, "podium cinematic palette")) return 0;
        cinematic_written = 1;
    }
    if (frame_timer <= BACKGROUND_TIMER && !static_written) {
        /* After the cleanup no car or scenery is drawn: their banks take the static screen's palettes. */
        if (!nm_palette_ram_write_dpal(static_palette, NM_PODIUM_STATIC_BANK * 16, "podium static palette")) return 0;
        static_written = 1;
    }
    if (frame_timer <= BACKGROUND_TIMER || frame_k >= PHOTO_FROM) enj_render_list_add(PVR_LIST_OP_POLY, render_opaque, NULL);
    if (frame_k < LAYER_A_UNTIL) enj_render_list_add(PVR_LIST_PT_POLY, render_layer_a, NULL);
    return 1;
}
#else
int nm_podium_render_load(const char *root) {
    (void)root;
    puts("Native podium screen: no podium_screen_data.h (NATIVE_PODIUM_SCREEN_DIR): not drawn");
    return 1;
}
void nm_podium_render_unload(void) {}
int nm_podium_render_enqueue(const nm_podium_t *podium) { (void)podium; return 1; }
#endif
