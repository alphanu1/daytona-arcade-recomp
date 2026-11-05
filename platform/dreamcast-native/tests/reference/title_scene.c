/* Historical embedded-title renderer; not part of the gameplay build.
 * Requires native_scene.inc from tools/prepare_scene.py for standalone use. */
#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>
#include "title_scene.h"
#include "title.h"

typedef struct {
    const void *texture, *palette;
    unsigned bank, opaque;
} scene_asset_t;
typedef struct {
    uint16_t texture, x, y, width, height, u, v, depth, role;
} scene_quad_t;

/* ponytail: embed this first bounded 2D scene. Course residency needs file
 * packs so source blobs do not stay in system RAM after upload. */
#include "native_scene.inc"
#define ASSET_COUNT (sizeof(scene_assets) / sizeof(scene_assets[0]))
static enj_texture_info_t textures[ASSET_COUNT];
static pvr_poly_hdr_t headers[ASSET_COUNT];
static const unsigned opaque_pass = 1, cutout_pass = 0;
#ifdef NATIVE_TITLE
static uint64_t title_started;
static title_pose_t pose;
#endif

int scene_load(void) {
    for (unsigned i = 0; i < ASSET_COUNT; ++i) {
        const scene_asset_t *asset = &scene_assets[i];
        if (!enj_texture_load_blob(asset->texture, &textures[i])) goto fail;
        if (asset->palette) {
            if (!enj_texture_load_palette_blob(asset->palette, PVR_PAL_ARGB8888,
                                               asset->bank * 16)) goto fail;
            /* Bank width is determined by PAL4, not by its 32-bit colours. */
            textures[i].pvrformat |= PVR_TXRFMT_4BPP_PAL(asset->bank);
        }
        pvr_poly_cxt_t context;
        pvr_poly_cxt_txr(&context, asset->opaque ? PVR_LIST_OP_POLY : PVR_LIST_PT_POLY,
                        textures[i].pvrformat, textures[i].width, textures[i].height,
                        fDtAdjustPVRPointer(&textures[i].hdr, textures[i].ptr),
                        PVR_FILTER_NONE);
        context.gen.culling = PVR_CULLING_NONE;
        context.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        pvr_poly_compile(&headers[i], &context);
    }
    printf("Native 2D scene: %u resources, %u quads; PVR free=%lu bytes\n",
           (unsigned)ASSET_COUNT, (unsigned)(sizeof(scene_quads) / sizeof(scene_quads[0])),
           (unsigned long)pvr_mem_available());
    return 1;
fail:
    for (unsigned i = 0; i < ASSET_COUNT; ++i)
        if (textures[i].flags.loaded) enj_texture_unload(&textures[i]);
    return 0;
}

static void render(void *data) {
    const unsigned opaque = *(const unsigned *)data;
    unsigned previous = ASSET_COUNT;
    shz_xmtrx_init_scale(640.0f / 496.0f, 480.0f / 384.0f, 1.0f);
    for (unsigned i = 0; i < sizeof(scene_quads) / sizeof(scene_quads[0]); ++i) {
        const scene_quad_t *q = &scene_quads[i];
        if (scene_assets[q->texture].opaque != opaque) continue;
        float x = q->x;
#ifdef NATIVE_TITLE
        if ((q->role == TITLE_PROMPT && !pose.prompt) ||
            (q->role == TITLE_FOOTER && !pose.footer)) continue;
        if (q->role == TITLE_LOGO) x += pose.logo_x;
#endif
        if (q->texture != previous) {
            pvr_poly_hdr_t *header = pvr_dr_target();
            *header = headers[q->texture];
            pvr_dr_commit(header);
            previous = q->texture;
        }
        for (unsigned corner = 0; corner < 4; ++corner) {
            const unsigned dx = corner >= 2 ? q->width : 0;
            const unsigned dy = corner & 1 ? 0 : q->height;
            const shz_vec4_t pos = shz_xmtrx_transform_vec4(
                shz_vec4_init(x + dx, q->y + dy, q->depth, 1));
            pvr_vertex_t *vertex = pvr_dr_target();
            *vertex = (pvr_vertex_t){
                .flags = corner == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                .x = pos.x, .y = pos.y, .z = pos.z,
                .u = (float)(q->u + dx) / textures[q->texture].width,
                .v = (float)(q->v + dy) / textures[q->texture].height,
                .argb = 0xffffffff,
            };
            pvr_dr_commit(vertex);
        }
    }
}

void scene_enqueue(void) {
#ifdef NATIVE_TITLE
    const uint64_t now = timer_ns_gettime64() / 1000;
    if (!title_started) title_started = now;
    pose = title_pose(title_motion, sizeof(title_motion) / sizeof(title_motion[0]),
                      title_tick(now - title_started));
#endif
    enj_render_list_add(PVR_LIST_OP_POLY, render, (void *)&opaque_pass);
    enj_render_list_add(PVR_LIST_PT_POLY, render, (void *)&cutout_pass);
}
