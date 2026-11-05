#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>
#include <malloc.h>
#include "render/scene/sky.h"
#include "scene_loader.h"
#include "nm_mem.h"

static struct {
    enj_texture_info_t textures[4];
    alignas(32) pvr_poly_hdr_t headers[6];
    nm_sky_rect_t rects[NM_SKY_RECTS];
    uint32_t fill[2];
    unsigned loaded,count,rows;
} sky;

void nm_sky_unload(void) {
    if(sky.loaded) {
        if(!pvr_wait_ready() && !pvr_wait_render_done())
            for(unsigned i=0;i<sky.loaded;++i) enj_texture_unload(sky.textures+i);
        else { puts("FAIL sky GPU fence; allocations left to PVR shutdown"); enj_state_flag_shutdown(NULL); }
    }
    nm_memset(&sky,0,sizeof(sky));
}
int nm_sky_load(const char *path) {
    if(sky.loaded) return 0;
    FILE *file=fopen(path,"rb");
    if(!file) return 0;
    unsigned char header[32];
    void *buffer=NULL;
    int ok=0;
    const size_t before=pvr_mem_available();
    if(fread(header,1,32,file)!=32 || !nm_sky_header_valid(header,32)) goto done;
    sky.fill[0]=nm_sky_word(header+8);sky.fill[1]=nm_sky_word(header+12);sky.rows=nm_sky_header_rows(header);
    buffer=nm_alloc(NM_SKY_TEXTURE_BYTES);
    if(!buffer) goto done;
    for(unsigned i=0;i<4;++i) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        if(fread(buffer,1,NM_SKY_TEXTURE_BYTES,file)!=NM_SKY_TEXTURE_BYTES ||
            !nm_sky_texture_valid(buffer,NM_SKY_TEXTURE_BYTES)) goto done;
        nm_loader_yield(0); /* then its upload */
        if(!enj_texture_load_blob(buffer,sky.textures+i)) goto done;
        ++sky.loaded;
        if(memcmp(sky.textures[i].ptr,(unsigned char *)buffer+32,NM_SKY_TEXTURE_BYTES-32)) goto done;
    }
    if(fgetc(file)!=EOF || ferror(file) || pvr_mem_available()<512*1024 ||
        before-pvr_mem_available()!=4u*NM_SKY_TEXTURE_BYTES) goto done;
    for(unsigned i=0;i<6;++i) {
        pvr_poly_cxt_t c;
        if(i<4) pvr_poly_cxt_txr(&c,PVR_LIST_OP_POLY,sky.textures[i].pvrformat,512,512,
            fDtAdjustPVRPointer(&sky.textures[i].hdr,sky.textures[i].ptr),
#ifdef NM_FILTER_ALL
            PVR_FILTER_BILINEAR);
#else
            PVR_FILTER_NONE);
#endif
        else pvr_poly_cxt_col(&c,PVR_LIST_OP_POLY);
        /* Clamped: a wrapping filter blends a page's edge texels with its opposite edge (seams at page joins and
         * the panorama's top row). */
        if(i<4) c.txr.uv_clamp=PVR_UVCLAMP_UV;
        c.gen.culling=PVR_CULLING_NONE;
        c.depth.comparison=PVR_DEPTHCMP_GEQUAL;
        pvr_poly_compile(sky.headers+i,&c);
    }
    printf("Native sky: 4 VQ16 pages, GPU=%u bytes incl KOS; staging=%u freed; max4 OP/SQ quads\n",
        4*NM_SKY_TEXTURE_BYTES,NM_SKY_TEXTURE_BYTES);
    ok=1;
done:
    free(buffer);
    if(fclose(file)) ok=0;
    if(!ok) nm_sky_unload();
    return ok;
}
static void sky_render(void *unused) {
    (void)unused;
    shz_xmtrx_init_scale(640.0f/496.0f,480.0f/384.0f,1);
    for(unsigned i=0;i<sky.count;++i) {
        const nm_sky_rect_t *r=sky.rects+i;
        pvr_poly_hdr_t *header=pvr_dr_target();*header=sky.headers[r->page];pvr_dr_commit(header);
        for(unsigned corner=0;corner<4;++corner) {
            const unsigned dx=corner>=2 ? r->width : 0,dy=corner&1 ? 0 : r->height;
            const shz_vec4_t p=shz_xmtrx_transform_vec4(shz_vec4_init(r->x+dx,r->y+dy,0,1));
            pvr_vertex_t *v=pvr_dr_target();
            *v=(pvr_vertex_t){.flags=corner==3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                .x=p.x*ENJ_XSCALE,.y=p.y,.z=NM_SKY_DEPTH,
                .u=(r->u+dx)*(1.0f/512),.v=(r->v+dy)*(1.0f/512),
                .argb=r->page<4 ? 0xffffffff : sky.fill[r->page-NM_SKY_BLUE]};
            pvr_dr_commit(v);
        }
    }
}
int nm_sky_enqueue(uint16_t heading,uint16_t scroll_y) {
    if(sky.loaded!=4 || !(sky.count=nm_sky_rects_rows(heading,scroll_y,sky.rows,sky.rects))) return 0;
    enj_render_list_add(PVR_LIST_OP_POLY,sky_render,NULL);
    return 1;
}
