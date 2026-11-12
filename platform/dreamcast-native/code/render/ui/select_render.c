#include "render/ui/select_render.h"
#include "render/materials/palette_ram.h"
#include <enDjinn/enj_defs.h>
#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include "nm_mem.h"

/* What the original draws in each select state (build/dc-native-frontend.PLAN.md, select.h):
 *   0-5   course select: layer 2, the maps (the cursor's lit: light block 502120, others 502210), the red
 *         frame ring (3D, under layer 0; at the frame centre +34/+38; in state 3 only on odd 5010b0), the
 *         three panels (the cursor's shows its second block while (5010b0 & 7) > 4), layer 0, the timer
 *         digits (+56 >> 6, states 0-4).
 *   6-13  transmission select: layer 2, the yellow ring (states 8-11, state 11 on odd 5010b0), the two
 *         panels, layer 0, the CAR SELECT arrow of the cursor, the digits (state 9).
 *   14-16 GENTLEMEN START YOUR ENGINES: its layer 2 (the text), from state 16 the course photo (layer 0).
 * The original's comb wipes, slide-in and palette fades (states 4-7, 12-13, 15-16) are cut: each screen
 * holds until the state that replaces it, with the same tick counts. The transmission screen's small map
 * (3D in the original: the chosen course's map at 0.644 of 0.9) is that course's lit map texture scaled into
 * the box the original draws it in. Not drawn yet: the transmission screen's two turning cars (3D).
 * Five courses (the PC build's, select.h NM_SELECT_COURSES; a pack with the Circuit Edition's panels and maps,
 * platform/pc-native/tools/prepare_select_cce.py): the five panels and maps in one row of 96-pixel columns, the
 * panels and the ring at 5/8 of their size and the maps at 90/112, filtered; the arcade's art otherwise as is. */

enum {
    ROLE_COURSE_BG = 1, ROLE_COURSE_FG, ROLE_TRANS_BG, ROLE_TRANS_FG, ROLE_GENT_TEXT, ROLE_GENT_PHOTO,
    ROLE_COURSE_RING = 8, ROLE_TRANS_RING = 9, ROLE_COURSE_PANEL = 16, ROLE_TRANS_PANEL = 24, ROLE_DIGIT = 32,
    ROLE_DIGIT_BLANK = 42, ROLE_ARROW = 48, ROLE_SMALL_MAP = 52, ROLE_MAP = 56,
    /* The Circuit Edition's slots 3 and 4: panels 64 + 2 (slot - 3) + variant, maps 68 + 2 (slot - 3) + lit, the
     * transmission screen's small maps 72 + slot - 3. */
    ROLE_CCE_PANEL = 64, ROLE_CCE_MAP = 68, ROLE_CCE_SMALL_MAP = 72, ROLE_COUNT = 80
};
/* The five-course row: column x, panel and map tops, their scales. */
#define FIVE_X0 11.0f
#define FIVE_PITCH 96.0f
#define FIVE_PANEL_Y 90.0f
#define FIVE_MAP_Y 152.0f
#define FIVE_PANEL_SCALE 0.625f
#define FIVE_MAP_SCALE (90.0f / 112.0f)
enum { SELECT_MAX_TEXTURES = 160, SELECT_MAX_QUADS = 256, SELECT_MAX_PALETTES = 64 };

typedef struct { uint16_t width, height, palette, opaque; uint32_t bytes; } select_texture_t;
typedef struct { uint16_t texture; int16_t x, y; uint16_t width, height, u, v, role; } select_quad_t;

static enj_texture_info_t textures[SELECT_MAX_TEXTURES];
alignas(32) static pvr_poly_hdr_t headers[SELECT_MAX_TEXTURES];
alignas(32) static pvr_poly_hdr_t filtered[SELECT_MAX_TEXTURES];   /* the same, bilinear: drawn scaled */
static select_texture_t texture_info[SELECT_MAX_TEXTURES];
static select_quad_t quads[SELECT_MAX_QUADS];
static unsigned texture_count, quad_count, loaded, five;
static int32_t anchors[4];
static int16_t panel0[2];   /* slot 0's panel in the arcade's layout (its frame anchor's panel) */
/* This frame's pose (set by nm_select_render_enqueue, read by the list callbacks). */
static struct {
    uint32_t roles[(ROLE_COUNT + 31) / 32]; /* bit set of the roles drawn (ROLE_COUNT bits) */
    int16_t ring_dx, ring_dy;
    unsigned tens, units, digits;
} pose;
static unsigned frames;

static void role_on(unsigned role) { pose.roles[role >> 5] |= 1u << (role & 31u); }
static int role_is_on(unsigned role) { return role < ROLE_COUNT && (pose.roles[role >> 5] >> (role & 31u)) & 1u; }

/* Painter order inside the screen: layer 2, maps, ring, panels, layer 0, arrows, digits (depth grows). */
static float role_depth(unsigned role) {
    if (role == ROLE_COURSE_BG || role == ROLE_TRANS_BG || role == ROLE_GENT_TEXT) return 1.0f;
    if (role >= ROLE_CCE_PANEL && role < ROLE_CCE_MAP) return 4.0f;
    if (role >= ROLE_SMALL_MAP) return 2.0f;
    if (role == ROLE_COURSE_RING || role == ROLE_TRANS_RING) return 3.0f;
    if (role >= ROLE_COURSE_PANEL && role < ROLE_DIGIT) return 4.0f;
    if (role == ROLE_GENT_PHOTO) return 5.0f;
    if (role == ROLE_COURSE_FG || role == ROLE_TRANS_FG) return 5.0f;
    if (role >= ROLE_ARROW) return 6.0f;
    return 7.0f;
}

static int small_map(unsigned role) {
    return (role >= ROLE_SMALL_MAP && role < ROLE_MAP) || (role >= ROLE_CCE_SMALL_MAP && role < ROLE_COUNT);
}

/* k: the quad's size against its texels'. */
static void quad(const select_quad_t *q, float x, float y, float k) {
    const select_texture_t *t = texture_info + q->texture;
    const float z = role_depth(q->role);
    /* The small maps scale a map's texture: their u/v hold the source extent, drawn from texel 0. */
    const int scaled = small_map(q->role);
    for (unsigned corner = 0; corner < 4; ++corner) {
        const unsigned dx = corner >= 2 ? q->width : 0, dy = corner & 1 ? 0 : q->height;
        const unsigned su = scaled ? (corner >= 2 ? q->u : 0) : q->u + dx, sv = scaled ? (corner & 1 ? 0 : q->v) : q->v + dy;
        const shz_vec4_t p = shz_xmtrx_transform_vec4(shz_vec4_init(x + k * (float)dx, y + k * (float)dy, z, 1));
        pvr_vertex_t *v = pvr_dr_target();
        *v = (pvr_vertex_t){.flags = corner == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, .x = p.x, .y = p.y, .z = p.z,
            .u = (float)su / t->width, .v = (float)sv / t->height, .argb = 0xffffffff};
        pvr_dr_commit(v);
    }
}

/* Where a quad goes in the five-course row (and its scale); 0 for the quads it leaves where they are. */
static int five_place(const select_quad_t *q, float *x, float *y, float *k) {
    unsigned slot;
    if (q->role >= ROLE_COURSE_PANEL && q->role < ROLE_COURSE_PANEL + 6) slot = (q->role - ROLE_COURSE_PANEL) >> 1;
    else if (q->role >= ROLE_CCE_PANEL && q->role < ROLE_CCE_MAP) slot = 3 + ((q->role - ROLE_CCE_PANEL) >> 1);
    else if (q->role >= ROLE_MAP && q->role < ROLE_MAP + 6) slot = 8 + ((q->role - ROLE_MAP) >> 1);
    else if (q->role >= ROLE_CCE_MAP && q->role < ROLE_CCE_SMALL_MAP) slot = 11 + ((q->role - ROLE_CCE_MAP) >> 1);
    else if (q->role == ROLE_COURSE_RING) {
        /* The ring around the gliding panel: its top left the frame - anchor + slot 0's arcade panel. */
        const float px = (float)panel0[0] + (float)pose.ring_dx, py = (float)panel0[1] + (float)pose.ring_dy;
        *k = FIVE_PANEL_SCALE;
        *x = px + FIVE_PANEL_SCALE * (float)(q->x - panel0[0]);
        *y = py + FIVE_PANEL_SCALE * (float)(q->y - panel0[1]);
        return 1;
    } else return 0;
    const int map = slot >= 8;
    *x = FIVE_X0 + FIVE_PITCH * (float)(map ? slot - 8 : slot);
    *y = map ? FIVE_MAP_Y : FIVE_PANEL_Y;
    *k = map ? FIVE_MAP_SCALE : FIVE_PANEL_SCALE;
    return 1;
}

static void render(void *data) {
    const unsigned opaque = (unsigned)(uintptr_t)data;
    unsigned previous = ~0u;
    shz_xmtrx_init_scale((640.0f / 496.0f) * ENJ_XSCALE, 480.0f / 384.0f, 1.0f);
    for (unsigned i = 0; i < quad_count; ++i) {
        const select_quad_t *q = quads + i;
        if (texture_info[q->texture].opaque != opaque) continue;
        float x = q->x, y = q->y;
        if (q->role >= ROLE_DIGIT && q->role <= ROLE_DIGIT_BLANK) {
            /* One sprite per glyph, drawn at the tens and units places (18b44: 10001ec right-aligned). */
            for (unsigned place = 0; place < 2; ++place) {
                const unsigned glyph = place ? pose.units : pose.tens;
                if (!pose.digits || q->role != ROLE_DIGIT + glyph) continue;
                if (q->texture != previous) {
                    pvr_poly_hdr_t *h = pvr_dr_target(); *h = headers[q->texture]; pvr_dr_commit(h);
                    previous = q->texture;
                }
                quad(q, 432.0f + 24.0f * place, 24.0f, 1.0f);
            }
            continue;
        }
        if (!role_is_on(q->role)) continue;
        float k = 1.0f;
        if (five && five_place(q, &x, &y, &k)) {
        } else if (q->role == ROLE_COURSE_RING || q->role == ROLE_TRANS_RING) { x += pose.ring_dx; y += pose.ring_dy; }
        const unsigned header = q->texture * 2u + (k != 1.0f);
        if (header != previous) {
            pvr_poly_hdr_t *h = pvr_dr_target(); *h = k != 1.0f ? filtered[q->texture] : headers[q->texture]; pvr_dr_commit(h);
            previous = header;
        }
        quad(q, x, y, k);
    }
}

static int read_file(const char *path, void **data, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    int ok = !fseek(file, 0, SEEK_END);
    const long length = ok ? ftell(file) : -1;
    ok = ok && length > 0 && !fseek(file, 0, SEEK_SET);
    void *blob = ok ? nm_alloc((size_t)length) : NULL;
    ok = blob && fread(blob, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if (!ok) { free(blob); return 0; }
    *data = blob; *size = (size_t)length; return 1;
}

int nm_select_render_load(const char *root) {
    if (loaded || !root) return 0;
    char path[512];
    void *layout = NULL; size_t size = 0;
    if (snprintf(path, sizeof(path), "%sselect/select.dsl", root) >= (int)sizeof(path) || !read_file(path, &layout, &size)) {
        printf("FAIL: select screens: %s missing (asset set without the select art)\n", path);
        return 0;
    }
    const uint8_t *p = layout;
    uint32_t head[5];
    if (size < 36) goto fail;
    nm_memcpy(head, p, sizeof(head)); nm_memcpy(anchors, p + 20, sizeof(anchors));
    const uint32_t textures_n = head[2], palettes_n = head[3], quads_n = head[4];
    if (head[0] != 0x314c5344u || head[1] != 1 || !textures_n || textures_n > SELECT_MAX_TEXTURES ||
        palettes_n > SELECT_MAX_PALETTES || !quads_n || quads_n > SELECT_MAX_QUADS ||
        size != 36 + 12 * textures_n + 64 * palettes_n + 16 * quads_n) goto fail;
    p += 36;
    nm_memcpy(texture_info, p, 12 * textures_n); p += 12 * textures_n;
    for (unsigned b = 0; b < palettes_n; ++b, p += 64) {
        uint32_t colours[16]; nm_memcpy(colours, p, sizeof(colours));
        if (!nm_palette_ram_write(16 * b, colours, 16, "select palette")) goto fail;
    }
    nm_memcpy(quads, p, 16 * quads_n);
    five = 0; panel0[0] = 24; panel0[1] = 72;
    for (unsigned i = 0; i < quads_n; ++i) {
        if (quads[i].texture >= textures_n || quads[i].role >= ROLE_COUNT) goto fail;
        if (quads[i].role >= ROLE_CCE_PANEL) five = NM_SELECT_COURSES == 5u;
        if (quads[i].role == ROLE_COURSE_PANEL) { panel0[0] = quads[i].x; panel0[1] = quads[i].y; }
    }
    for (unsigned i = 0; i < textures_n; ++i) {
        const select_texture_t *t = texture_info + i;
        void *blob = NULL; size_t bytes = 0;
        if (t->palette >= palettes_n || snprintf(path, sizeof(path), "%sselect/tex-%u.dt", root, i) >= (int)sizeof(path) ||
            !read_file(path, &blob, &bytes) || bytes != t->bytes) { free(blob); goto fail; }
        const int ok = enj_texture_load_blob(blob, textures + i);
        free(blob);
        if (!ok) goto fail;
        ++texture_count;
        textures[i].pvrformat |= PVR_TXRFMT_4BPP_PAL(t->palette);
        pvr_poly_cxt_t context;
        pvr_poly_cxt_txr(&context, t->opaque ? PVR_LIST_OP_POLY : PVR_LIST_PT_POLY, textures[i].pvrformat,
            textures[i].width, textures[i].height, fDtAdjustPVRPointer(&textures[i].hdr, textures[i].ptr), PVR_FILTER_NONE);
        context.gen.culling = PVR_CULLING_NONE; context.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        pvr_poly_compile(headers + i, &context);
        context.txr.filter = PVR_FILTER_BILINEAR;
        pvr_poly_compile(filtered + i, &context);
    }
    quad_count = quads_n; free(layout); loaded = 1; frames = 0;
    printf("Native select screens: %u PAL4 textures, %u palette banks, %u quads%s; PVR free=%lu\n",
        texture_count, (unsigned)palettes_n, quad_count, five ? ", five courses" : "", (unsigned long)pvr_mem_available());
    return 1;
fail:
    free(layout);
    puts("FAIL: select screens: malformed select.dsl or texture");
    while (texture_count) enj_texture_unload(textures + --texture_count);
    return 0;
}

int nm_select_render_enqueue(const nm_select_t *s, uint32_t counter) {
    if (!loaded || !s) return 0;
    nm_memset(&pose, 0, sizeof(pose));
    const unsigned st = s->state, blink = (counter & 7u) > 4u, odd = counter & 1u;
    if (st <= 5) {
        role_on(ROLE_COURSE_BG); role_on(ROLE_COURSE_FG);
        for (unsigned slot = 0; slot < (five ? 5u : 3u); ++slot) {
            const unsigned panel = slot < 3 ? ROLE_COURSE_PANEL + 2 * slot : ROLE_CCE_PANEL + 2 * (slot - 3);
            const unsigned map = slot < 3 ? ROLE_MAP + 2 * slot : ROLE_CCE_MAP + 2 * (slot - 3);
            role_on(map + (slot == s->slot));
            role_on(panel + (slot == s->slot && blink && st <= 3));
        }
        if (st <= 2 || (st == 3 && odd)) role_on(ROLE_COURSE_RING);
        pose.ring_dx = (int16_t)(s->frame[0] - anchors[0]); pose.ring_dy = (int16_t)(s->frame[1] - anchors[1]);
        pose.digits = st <= 4;
    } else if (st <= 13) {
        role_on(ROLE_TRANS_BG); role_on(ROLE_TRANS_FG);
        const unsigned cursor = st >= 8 ? s->slot : 0;
        for (unsigned slot = 0; slot < 2; ++slot)
            role_on(ROLE_TRANS_PANEL + 2 * slot + (st >= 8 && slot == cursor && blink && st <= 11));
        role_on(ROLE_ARROW + (cursor & 1u));
        role_on(s->chosen < 3 ? ROLE_SMALL_MAP + s->chosen : five && s->chosen < 5 ? ROLE_CCE_SMALL_MAP + s->chosen - 3u : ROLE_SMALL_MAP);
        if ((st >= 8 && st <= 10) || (st == 11 && odd)) role_on(ROLE_TRANS_RING);
        pose.ring_dx = (int16_t)(s->frame[0] - anchors[2]); pose.ring_dy = (int16_t)(s->frame[1] - anchors[3]);
        pose.digits = st == 9;
    } else {
        role_on(ROLE_GENT_TEXT);
        if (st == 16) role_on(ROLE_GENT_PHOTO);
    }
    if (pose.digits) {
        const unsigned value = s->timer >> 6;
        pose.units = value % 10u;
        pose.tens = value >= 10u ? (value / 10u) % 10u : 10u;
    }
    ++frames;
    enj_render_list_add(PVR_LIST_OP_POLY, render, (void *)(uintptr_t)1);
    enj_render_list_add(PVR_LIST_PT_POLY, render, NULL);
    return 1;
}

unsigned nm_select_render_courses(void) { return loaded && five ? 5u : 3u; }

int nm_select_render_release(void) {
    if (!loaded) return 1;
    if (pvr_wait_ready() || pvr_wait_render_done()) return 0;
    while (texture_count) enj_texture_unload(textures + --texture_count);
    nm_memset(textures, 0, sizeof(textures));
    printf("Native select screens: %u frames drawn\n", frames);
    loaded = quad_count = 0; return 1;
}
