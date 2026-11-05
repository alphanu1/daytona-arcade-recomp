#include "render/ui/title_render.h"
#include "render/materials/palette_ram.h"
#include <enDjinn/enj_defs.h>
#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>
#include <string.h>
#include "nm_mem.h"

typedef struct {
    const void *texture, *palette;
    unsigned bank, opaque;
} scene_asset_t;
typedef struct {
    uint16_t texture, x, y, width, height, u, v, depth, role;
} scene_quad_t;

/* Offline ROM-derived data selected by TITLE_SCENE_DIR at build time. */
#include "native_scene.inc"

#define TITLE_ASSET_COUNT (sizeof(scene_assets) / sizeof(scene_assets[0]))

alignas(32) static enj_texture_info_t textures[TITLE_ASSET_COUNT];
alignas(32) static pvr_poly_hdr_t headers[TITLE_ASSET_COUNT];
static title_pose_t current_pose;
static unsigned loaded;

static void title_render(void *data) {
    const unsigned opaque=(unsigned)(uintptr_t)data;
    unsigned previous=TITLE_ASSET_COUNT;
    shz_xmtrx_init_scale((640.0f/496.0f)*ENJ_XSCALE,480.0f/384.0f,1.0f);
    for(unsigned i=0;i<sizeof(scene_quads)/sizeof(scene_quads[0]);++i) {
        const scene_quad_t *q=scene_quads+i;
        if(scene_assets[q->texture].opaque!=opaque ||
           (q->role==TITLE_PROMPT && !current_pose.prompt) ||
           (q->role==TITLE_FOOTER && !current_pose.footer)) continue;
        const float x=(float)q->x+(q->role==TITLE_LOGO ? current_pose.logo_x : 0);
        if(q->texture!=previous) {
            pvr_poly_hdr_t *header=pvr_dr_target();*header=headers[q->texture];pvr_dr_commit(header);
            previous=q->texture;
        }
        for(unsigned corner=0;corner<4;++corner) {
            const unsigned dx=corner>=2 ? q->width : 0,dy=corner&1 ? 0 : q->height;
            const shz_vec4_t p=shz_xmtrx_transform_vec4(shz_vec4_init(x+dx,q->y+dy,q->depth,1));
            pvr_vertex_t *v=pvr_dr_target();
            *v=(pvr_vertex_t){.flags=corner==3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                .x=p.x,.y=p.y,.z=p.z,.u=(float)(q->u+dx)/textures[q->texture].width,
                .v=(float)(q->v+dy)/textures[q->texture].height,.argb=0xffffffff};
            pvr_dr_commit(v);
        }
    }
}

int nm_title_render_load(void) {
    if(loaded) return 0;
    for(unsigned i=0;i<TITLE_ASSET_COUNT;++i) {
        const scene_asset_t *asset=scene_assets+i;
        if(!enj_texture_load_blob(asset->texture,textures+i) ||
           (asset->palette && !nm_palette_ram_write_dpal(asset->palette,asset->bank*16,"title palette"))) goto fail;
        if(asset->palette) textures[i].pvrformat|=PVR_TXRFMT_4BPP_PAL(asset->bank);
        pvr_poly_cxt_t context;
        pvr_poly_cxt_txr(&context,asset->opaque ? PVR_LIST_OP_POLY : PVR_LIST_PT_POLY,
            textures[i].pvrformat,textures[i].width,textures[i].height,
            fDtAdjustPVRPointer(&textures[i].hdr,textures[i].ptr),PVR_FILTER_NONE);
        context.gen.culling=PVR_CULLING_NONE;context.depth.comparison=PVR_DEPTHCMP_GEQUAL;
        pvr_poly_compile(headers+i,&context);
    }
    loaded=1;return 1;
fail:
    for(unsigned i=0;i<TITLE_ASSET_COUNT;++i) if(textures[i].flags.loaded) enj_texture_unload(textures+i);
    nm_memset(textures,0,sizeof(textures));return 0;
}

int nm_title_render_enqueue(uint64_t tick) {
    if(!loaded) return 0;
    current_pose=title_pose(title_motion,sizeof(title_motion)/sizeof(title_motion[0]),tick);
    enj_render_list_add(PVR_LIST_OP_POLY,title_render,(void *)(uintptr_t)1);
    enj_render_list_add(PVR_LIST_PT_POLY,title_render,NULL);
    return 1;
}

int nm_title_render_release(void) {
    if(!loaded) return 1;
    if(pvr_wait_ready() || pvr_wait_render_done()) return 0;
    for(unsigned i=0;i<TITLE_ASSET_COUNT;++i) if(textures[i].flags.loaded) enj_texture_unload(textures+i);
    nm_memset(textures,0,sizeof(textures));loaded=0;return 1;
}
