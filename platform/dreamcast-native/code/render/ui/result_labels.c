#include "render/ui/result_labels.h"
#include "render/materials/palette_ram.h"
#include "result_actor.h"
#include "scene_loader.h"
#include "result_label_data.h"
#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
#include <string.h>
#include "nm_mem.h"

static enj_texture_info_t textures[NM_RESULT_LABEL_TEXTURE_COUNT];
alignas(32) static pvr_poly_hdr_t headers[NM_RESULT_LABEL_TEXTURE_COUNT];
static shz_vec2_t inverse_size[NM_RESULT_LABEL_TEXTURE_COUNT];
typedef struct { uint16_t asset,x,y,pad; } label_draw_t;
alignas(32) static label_draw_t draws[22];
static unsigned loaded,ready,count,frames;
static unsigned palette_applied;
/* Written at the result hand-off, with the glyph palettes (palette_banks.h). */
alignas(32) static uint32_t label_palette[2+NM_RESULT_LABEL_PALETTE_COLORS];
static int16_t current_scroll;
static int prepare(unsigned rank,uint32_t total,int16_t best,int16_t scroll);

void nm_result_labels_unload(void) {
    ready=palette_applied=0;
    if(!loaded) return;
    printf("Native result labels: frames=%u; cached PT headers, direct SQ, startup-only uploads\n",frames);
    if(pvr_wait_ready() || pvr_wait_render_done()) { enj_state_flag_shutdown(NULL);return; }
    while(loaded) enj_texture_unload(textures+--loaded);
    nm_memset(textures,0,sizeof(textures));count=frames=0;
}

static int read_exact(const char *path,void *data,size_t size) {
    FILE *file=fopen(path,"rb");if(!file) return 0;
    const int ok=fread(data,1,size,file)==size && fgetc(file)==EOF;
    fclose(file);return ok;
}

/* Result hand-off, after nm_result_materials_palettes_apply (which fences the
 * last car frame). Entries 960.. only; everything below is left as it is. */
int nm_result_labels_palettes_apply(void) {
    if(!ready) return 0;
    if(palette_applied) return 1;
    uint32_t preserved[960];
    for(unsigned i=0;i<960;++i) preserved[i]=nm_palette_ram_read(i);
    /* In the program's palette format (palette_ram.h), read back. */
    if(!nm_palette_ram_write_dpal(label_palette,NM_RESULT_LABEL_PALETTE_FIRST,"result label palette")) return 0;
    for(unsigned i=0;i<960;++i) if(nm_palette_ram_read(i)!=preserved[i]) return 0;
    palette_applied=1;
    printf("Native result label palettes: %u colours at %u written at the result hand-off; readback PASS, entries 0..959 preserved\n",
        NM_RESULT_LABEL_PALETTE_COLORS,NM_RESULT_LABEL_PALETTE_FIRST);
    return 1;
}

int nm_result_labels_load(const char *root) {
    if(!root || loaded || ready) return 0;
    _Static_assert(NM_RESULT_LABEL_PALETTE_FIRST==960 && NM_RESULT_LABEL_PALETTE_COLORS<=64,"label palette ownership");
    const unsigned before=pvr_mem_available();
    unsigned bytes=0;
    for(unsigned i=0;i<NM_RESULT_LABEL_TEXTURE_COUNT;++i) {
        const nm_result_label_part_t *p=nm_result_label_parts+i;
        if(p->texture!=i || p->bank<60 || p->bank>=64 || p->texture_width<8 || p->texture_width>128 ||
           p->texture_height<8 || p->texture_height>128 || (p->texture_width&(p->texture_width-1)) ||
           (p->texture_height&(p->texture_height-1)) || !p->width || !p->height ||
           p->width>p->texture_width || p->height>p->texture_height) return 0;
        bytes+=32+p->texture_width*p->texture_height/2;
    }
    if(before<bytes+512*1024) return 0;
    alignas(32) uint32_t blob[8+128*128/8];char path[512];
    for(unsigned i=0;i<NM_RESULT_LABEL_TEXTURE_COUNT;++i) {
        const nm_result_label_part_t *p=nm_result_label_parts+i;
        const unsigned size=32+p->texture_width*p->texture_height/2;
        const uint32_t expected[8]={0x78546344,size,0x0f000000,p->texture_width|((uint32_t)p->texture_height<<16),
            0x28000000|((__builtin_ctz(p->texture_width)-3)<<3)|(__builtin_ctz(p->texture_height)-3),0,0,0};
        if(snprintf(path,sizeof(path),"%slabels/label-%03u.dt",root,i)>=(int)sizeof(path) ||
           !read_exact(path,blob,size) || memcmp(blob,expected,32) || !enj_texture_load_blob(blob,textures+i)) goto fail;
        ++loaded;
        const volatile uint32_t *vram=(const volatile uint32_t *)textures[i].ptr;
        for(unsigned w=0;w<(size-32)/4;++w) if(vram[w]!=blob[w+8]) goto fail;
        pvr_poly_cxt_t c;
        pvr_poly_cxt_txr(&c,PVR_LIST_PT_POLY,textures[i].pvrformat|PVR_TXRFMT_4BPP_PAL(p->bank),
            p->texture_width,p->texture_height,textures[i].ptr,PVR_FILTER_NONE);
        c.gen.culling=PVR_CULLING_NONE;c.depth.comparison=PVR_DEPTHCMP_ALWAYS;c.depth.write=PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(headers+i,&c);nm_memset((char *)(headers+i)+16,0,16);
        inverse_size[i]=shz_vec2_init(shz_divf_fsrra(1.0f,p->texture_width),shz_divf_fsrra(1.0f,p->texture_height));
        if(fabsf(inverse_size[i].x*p->texture_width-1.0f)>0.000001f ||
           fabsf(inverse_size[i].y*p->texture_height-1.0f)>0.000001f) goto fail;
    }
    if(snprintf(path,sizeof(path),"%slabels/labels.dp",root)>=(int)sizeof(path) ||
       !read_exact(path,label_palette,sizeof(label_palette)) || label_palette[0]!=0x4c415044 ||
       label_palette[1]!=NM_RESULT_LABEL_PALETTE_COLORS) goto fail;
    if(pvr_mem_available()<512*1024) goto fail;
    ready=1;
    unsigned checked=0;
    static int oracle_done; /* the preparation oracle once per power-on (the attract loop reloads the scene at each course) */
    if(oracle_done) {
        printf("Native result labels resident: textures=%u allocated=%u free=%u; preparation oracle passed at power-on\n",
            loaded,before-pvr_mem_available(),pvr_mem_available());
        return 1;
    }
    for(unsigned rank=0;rank<42;++rank) for(int scroll=0;scroll<=512;scroll+=16) {
        if(!scroll) nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        if(!prepare(rank,1237,4660,(int16_t)scroll) || count!=(rank<9 ? 21u : 22u)) goto fail;
        for(unsigned d=0;d<count;++d) {
            const uint16_t *span=nm_result_label_spans[draws[d].asset];
            for(unsigned i=0;i<span[1];++i) {
                const nm_result_label_part_t *p=nm_result_label_parts+span[0]+i;
                for(unsigned corner=0;corner<4;++corner) {
                    const float x=draws[d].x+p->x+scroll+((corner&2) ? p->width : 0);
                    const float y=draws[d].y+p->y+((corner&1) ? p->height : 0);
                    const shz_vec2_t xy=shz_vec2_mul(shz_vec2_init(x,y),shz_vec2_init(640.0f/496.0f,480.0f/384.0f));
                    if(xy.x!=x*(640.0f/496.0f) || xy.y!=y*(480.0f/384.0f)) goto fail;
                    ++checked;
                }
            }
        }
    }
    if(prepare(42,0,0,0) || prepare(0,0,0,-1) || prepare(0,0,0,513)) goto fail;
    count=0;oracle_done=1;
    printf("Native result label preparation PASS:42 ranks33scrolls vertices=%u, SH4ZAM scale oracle and bounds\n",checked);
    printf("Native result labels resident PASS: textures=%u allocated=%u free=%u palette-colors=%u; full VRAM readback, palette RAM written at the result hand-off\n",
        loaded,before-pvr_mem_available(),pvr_mem_available(),NM_RESULT_LABEL_PALETTE_COLORS);
    return 1;
fail:
    puts("FAIL result labels startup");nm_result_labels_unload();return 0;
}

static int append(unsigned asset,unsigned offset) {
    if(asset==UINT16_MAX) return 1; /* Optional tens-of-rank block. */
    if(asset>=NM_RESULT_LABEL_ASSET_COUNT || count>=22 || nm_result_label_spans[asset][0]==UINT16_MAX) return 0;
    const unsigned first=nm_result_label_spans[asset][0],length=nm_result_label_spans[asset][1];
    if(first+length>NM_RESULT_LABEL_TEXTURE_COUNT) return 0;
    draws[count++]=(label_draw_t){asset,((offset>>1)&63)*8,(offset>>7)*8,0};return 1;
}

static void render(void *unused) {
    (void)unused;
    if(!ready) return;
    const shz_vec2_t scale=shz_vec2_init(640.0f/496.0f,480.0f/384.0f);
    for(unsigned d=0;d<count;++d) {
        const label_draw_t *draw=draws+d;
        const uint16_t *span=nm_result_label_spans[draw->asset];
        for(unsigned i=0;i<span[1];++i) {
            const nm_result_label_part_t *p=nm_result_label_parts+span[0]+i;
            pvr_poly_hdr_t *hdr=pvr_dr_target();*hdr=headers[p->texture];pvr_dr_commit(hdr);
            for(unsigned corner=0;corner<4;++corner) {
                const int dx=(corner&2) ? p->width : 0,dy=(corner&1) ? p->height : 0;
                const shz_vec2_t xy=shz_vec2_mul(shz_vec2_init(draw->x+p->x+current_scroll+dx,draw->y+p->y+dy),scale);
                const shz_vec2_t uv=shz_vec2_mul(shz_vec2_init(dx,dy),inverse_size[p->texture]);
                pvr_vertex_t *v=pvr_dr_target();v->flags=corner==3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                v->x=xy.x*ENJ_XSCALE;v->y=xy.y;v->z=1.0f;v->u=uv.x;v->v=uv.y;v->argb=0xffffffff;v->oargb=0;pvr_dr_commit(v);
            }
        }
    }
    ++frames;
}

static int prepare(unsigned rank,uint32_t total,int16_t best,int16_t scroll) {
    if(!ready || rank>=42 || scroll<0 || scroll>512) return 0;
    count=0;current_scroll=scroll;
    for(unsigned i=0;i<3;++i) if(!append(nm_result_label_fixed[i],nm_result_label_normal_offsets[i])) return 0;
    for(unsigned i=0;i<3;++i) if(!append(nm_result_label_ranks[rank][i],nm_result_label_normal_offsets[i+3])) return 0;
    const uint32_t times[]={total,(uint32_t)(int32_t)best};
    for(unsigned t=0;t<2;++t) {
        uint16_t glyphs[8];nm_result_time_glyphs(times[t],glyphs);
        for(unsigned i=0;i<8;++i) {
            const unsigned code=glyphs[i]&255;
            if(code>=58 || nm_result_label_glyphs[code]==UINT16_MAX ||
               !append(nm_result_label_glyphs[code],nm_result_label_normal_offsets[6+t]+4*i)) return 0;
        }
    }
    return 1;
}

int nm_result_labels_enqueue(unsigned rank,uint32_t total,int16_t best,int16_t scroll) {
    if(!prepare(rank,total,best,scroll)) return 0;
    enj_render_list_add(PVR_LIST_PT_POLY,render,NULL);return 1;
}
