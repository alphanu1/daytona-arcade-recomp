/* Host build of code/render/ui/select_render.c with the PVR, enDjinn and SH4ZAM calls it makes stubbed: loads
 * ROOT/select/ (tools/prepare_select.py), enqueues one pose and runs the list callbacks, printing every quad it
 * submits: `Q list texture x0 y0 u0 v0 x1 y1 u1 v1 x2 y2 u2 v2 x3 y3 u3 v3 z` in 640x480 screen space, and
 * `T index file width height` per texture. tests/test_select_render.py rasterizes them and compares the
 * picture with the original's screen.
 * usage: select_preview ROOT/ STATE SLOT TIMER FRAME_X FRAME_Y COUNTER COURSE */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- stubs: enDjinn, KOS PVR, SH4ZAM (only what select_render.c uses) --- */
#define ENJ_XSCALE 1
typedef enum { PVR_LIST_OP_POLY, PVR_LIST_TR_POLY, PVR_LIST_OP_MOD, PVR_LIST_TR_MOD, PVR_LIST_PT_POLY } pvr_list_t;
#define PVR_CMD_VERTEX 0xe0000000u
#define PVR_CMD_VERTEX_EOL 0xf0000000u
#define PVR_TXRFMT_4BPP_PAL(x) ((uint32_t)(5u<<27) | ((uint32_t)(x)<<21))
#define PVR_FILTER_NONE 0
#define PVR_CULLING_NONE 0
#define PVR_DEPTHCMP_GEQUAL 6
typedef struct { uint32_t cmd, mode1, mode2, mode3, d1, d2, d3, d4; } pvr_poly_hdr_t;
typedef struct { uint32_t flags; float x, y, z, u, v; uint32_t argb, oargb; } pvr_vertex_t;
typedef struct { struct { int culling; } gen; struct { int comparison; } depth; uint32_t texture; } pvr_poly_cxt_t;
typedef struct { uint32_t hdr[8]; void *ptr; uint32_t pvrformat; unsigned width, height; struct { unsigned loaded; } flags; } enj_texture_info_t;
typedef struct { float x, y, z, w; } shz_vec4_t;
static float scale_x = 1, scale_y = 1, scale_z = 1;
static void shz_xmtrx_init_scale(float x, float y, float z) { scale_x = x; scale_y = y; scale_z = z; }
static shz_vec4_t shz_vec4_init(float x, float y, float z, float w) { return (shz_vec4_t){x, y, z, w}; }
static shz_vec4_t shz_xmtrx_transform_vec4(shz_vec4_t v) { return (shz_vec4_t){v.x * scale_x, v.y * scale_y, v.z * scale_z, v.w}; }
static int list_now;
static int texture_now = -1;
static int vertex_n;
static pvr_vertex_t quad_vertices[4];
static union { pvr_poly_hdr_t h; pvr_vertex_t v; } target;
static void *pvr_dr_target(void) { return &target; }
static unsigned headers_compiled;
static void pvr_dr_commit(void *p) {
    const uint32_t first = *(const uint32_t *)p;
    if (first == PVR_CMD_VERTEX || first == PVR_CMD_VERTEX_EOL) {
        quad_vertices[vertex_n++] = *(const pvr_vertex_t *)p;
        if (first == PVR_CMD_VERTEX_EOL) {
            assert(vertex_n == 4);
            printf("Q %d %d", list_now, texture_now);
            for (unsigned i = 0; i < 4; ++i)
                printf(" %.3f %.3f %.6f %.6f", quad_vertices[i].x, quad_vertices[i].y, quad_vertices[i].u, quad_vertices[i].v);
            printf(" %.1f\n", quad_vertices[0].z);
            vertex_n = 0;
        }
    } else { texture_now = (int)((const pvr_poly_hdr_t *)p)->d4; vertex_n = 0; }
}
static unsigned texture_loads;
static int enj_texture_load_blob(const void *blob, enj_texture_info_t *t) {
    const uint8_t *b = blob;
    if (memcmp(b, "DcTx", 4)) return 0;
    memset(t, 0, sizeof(*t));
    t->width = b[12] | b[13] << 8; t->height = b[14] | b[15] << 8; t->flags.loaded = 1; t->ptr = (void *)(uintptr_t)(++texture_loads);
    return 1;
}
static void enj_texture_unload(enj_texture_info_t *t) { t->flags.loaded = 0; }
static void *fDtAdjustPVRPointer(void *hdr, void *ptr) { (void)hdr; return ptr; }
static void pvr_poly_cxt_txr(pvr_poly_cxt_t *c, pvr_list_t list, uint32_t format, unsigned w, unsigned h, void *ptr, int filter) {
    (void)list; (void)format; (void)w; (void)h; (void)filter; memset(c, 0, sizeof(*c)); c->texture = (uint32_t)(uintptr_t)ptr;
}
static void pvr_poly_compile(pvr_poly_hdr_t *h, const pvr_poly_cxt_t *c) {
    memset(h, 0, sizeof(*h)); h->cmd = 0x80000000u; h->d4 = c->texture - 1; ++headers_compiled;
}
static unsigned pvr_mem_available(void) { return 8u << 20; }
static int pvr_wait_ready(void) { return 0; }
static int pvr_wait_render_done(void) { return 0; }
typedef void (*callback_t)(void *);
static struct { pvr_list_t list; callback_t f; void *data; } lists[8];
static unsigned list_count;
static void enj_render_list_add(pvr_list_t list, callback_t f, void *data) { lists[list_count++] = (typeof(lists[0])){list, f, data}; }
static int nm_palette_ram_write(unsigned first, const uint32_t *argb, unsigned count, const char *owner) {
    (void)owner;
    for (unsigned i = 0; i < count; ++i) printf("P %u %08x\n", first + i, argb[i]);
    return 1;
}
/* The renderer's other includes: empty stand-ins in the test's include directory (tests/test_select_render.py);
 * palette_ram.h skipped by its guard (its writer is stubbed above); select.h is the real one. */
#define DAYTONA_NATIVE_PALETTE_RAM_H
#include <stdalign.h>
#include "../code/render/ui/select_render.c"

int main(int argc, char **argv) {
    if (argc != 9) { fputs("usage: select_preview ROOT/ STATE SLOT TIMER FRAME_X FRAME_Y COUNTER COURSE\n", stderr); return 2; }
    if (!nm_select_render_load(argv[1])) return 1;
    for (unsigned i = 0; i < texture_count; ++i)
        printf("T %u tex-%u.dt %u %u %u %u\n", i, i, texture_info[i].width, texture_info[i].height, texture_info[i].palette, texture_info[i].opaque);
    nm_select_t s; memset(&s, 0, sizeof(s));
    s.phase = 18; s.state = (uint8_t)atoi(argv[2]); s.slot = (uint16_t)atoi(argv[3]); s.timer = (uint16_t)atoi(argv[4]);
    s.frame[0] = atoi(argv[5]); s.frame[1] = atoi(argv[6]);
    const unsigned course = (unsigned)atoi(argv[8]);
    s.course = (uint8_t)course; s.chosen = (uint8_t)(course == 2 ? 1 : course == 1 ? 2 : 0); /* 23addc's slots */
    if (!nm_select_render_enqueue(&s, (uint32_t)strtoul(argv[7], NULL, 10))) return 1;
    for (unsigned i = 0; i < list_count; ++i) { list_now = lists[i].list; lists[i].f(lists[i].data); }
    return nm_select_render_release() ? 0 : 1;
}
