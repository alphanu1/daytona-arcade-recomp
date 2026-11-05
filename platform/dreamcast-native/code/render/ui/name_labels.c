#include "render/ui/name_labels.h"
#include "render/materials/palette_ram.h"
#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>
#include "result_actor.h"
#include "nm_mem.h"

static enj_texture_info_t textures[64];
alignas(32) static pvr_poly_hdr_t headers[64];
static shz_vec2_t inverse_size[64];
static const nm_name_label_part_t *parts;
static unsigned loaded,ready,pending;
static int countdown;
static unsigned static_parts,record_count;
static struct { unsigned part,x,y; } record_draws[19];

int nm_name_labels_unload(void) {
    if(pending) return 0;
    ready=0;
    if(loaded && (pvr_wait_ready() || pvr_wait_render_done())) return 0;
    while(loaded) enj_texture_unload(textures+--loaded);
    parts=NULL;record_count=0;return 1;
}

int nm_name_labels_load(const char *root,const nm_name_label_part_t *source,unsigned count,unsigned fixed) {
    if(!root || !source || !fixed || count>64 || fixed>count || count-fixed!=36 || loaded || pending) {
        printf("FAIL name labels: root=%p parts=%p count=%u static=%u loaded=%u pending=%u\n",
            root,source,count,fixed,loaded,pending);return 0;
    }
    unsigned bytes=0;
    for(unsigned i=0;i<count;++i) {
        const nm_name_label_part_t *p=source+i;
        if(p->bank<60 || p->bank>63 || p->foreground>1 || !p->width || !p->height ||
           p->texture_width<8 || p->texture_width>128 || p->texture_height<8 || p->texture_height>128 ||
           (p->texture_width&(p->texture_width-1)) || (p->texture_height&(p->texture_height-1)) ||
           p->width>p->texture_width || p->height>p->texture_height ||
           p->x+p->width>496 || p->y+p->height>384) {
            printf("FAIL name label %u: bank=%u fg=%u rect=%u,%u %ux%u texture=%ux%u\n",
                i,p->bank,p->foreground,p->x,p->y,p->width,p->height,p->texture_width,p->texture_height);return 0;
        }
        if(i>=fixed) {
            const unsigned glyph=i-fixed;
            const unsigned width=glyph>=10 && glyph<22 ? 8 : 16;
            const unsigned height=glyph<10 ? 24 : glyph<22 ? 16 : 32;
            if(p->x+p->width>width || p->y+p->height>height || !p->foreground) {
                printf("FAIL name glyph %u: rect=%u,%u %ux%u cell=%ux%u fg=%u\n",
                    glyph,p->x,p->y,p->width,p->height,width,height,p->foreground);return 0;
            }
        }
        bytes+=p->texture_width*p->texture_height/2+32;
    }
    const unsigned before=pvr_mem_available();
    if(before<bytes+512*1024 || pvr_wait_ready() || pvr_wait_render_done()) {
        printf("FAIL name labels: VRAM=%u required=%u or PVR busy\n",before,bytes+512*1024);return 0;
    }
    char path[512];
    for(unsigned i=0;i<count;++i) {
        const nm_name_label_part_t *p=source+i;
        if(snprintf(path,sizeof(path),"%slabel-%03u.dt",root,i)>=(int)sizeof(path) ||
           !enj_texture_load_file(path,textures+i)) { printf("FAIL name label load: %s\n",path);goto fail; }
        ++loaded;
        if(textures[i].width!=p->texture_width || textures[i].height!=p->texture_height ||
           textures[i].pvrformat!=PVR_TXRFMT_PAL4BPP) {
            printf("FAIL name label texture %u: got=%ux%u fmt=%08lx expected=%ux%u PAL4\n",i,
                textures[i].width,textures[i].height,(unsigned long)textures[i].pvrformat,
                p->texture_width,p->texture_height);goto fail;
        }
        pvr_poly_cxt_t context;
        pvr_poly_cxt_txr(&context,PVR_LIST_PT_POLY,textures[i].pvrformat|PVR_TXRFMT_4BPP_PAL(p->bank),
            p->texture_width,p->texture_height,textures[i].ptr,PVR_FILTER_NONE);
        context.gen.culling=PVR_CULLING_NONE;
        context.depth.comparison=p->foreground ? PVR_DEPTHCMP_ALWAYS : PVR_DEPTHCMP_GEQUAL;
        context.depth.write=PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(headers+i,&context);nm_memset((char *)(headers+i)+16,0,16);
        inverse_size[i]=shz_vec2_init(shz_divf_fsrra(1.0f,p->texture_width),shz_divf_fsrra(1.0f,p->texture_height));
    }
    if(snprintf(path,sizeof(path),"%slabels.dp",root)>=(int)sizeof(path) ||
       !nm_palette_ram_write_dpal_file(path,960,"name label palette") || pvr_mem_available()<512*1024) {
        printf("FAIL name label palette: %s free=%u\n",path,pvr_mem_available());goto fail;
    }
    parts=source;static_parts=fixed;record_count=0;ready=1;
    printf("Native name labels: textures=%u allocated=%u free=%u; background depth/foreground overlay, resident PAL4\n",
        loaded,before-pvr_mem_available(),pvr_mem_available());
    return 1;
fail:
    nm_name_labels_unload();return 0;
}

static void render_part(unsigned i,unsigned x,unsigned y) {
    const shz_vec2_t scale=shz_vec2_init(640.0f/496.0f,480.0f/384.0f);
    const nm_name_label_part_t *p=parts+i;
    pvr_poly_hdr_t *header=pvr_dr_target();*header=headers[i];pvr_dr_commit(header);
    for(unsigned corner=0;corner<4;++corner) {
        const float dx=corner&2 ? p->width : 0,dy=corner&1 ? p->height : 0;
        const shz_vec2_t xy=shz_vec2_mul(shz_vec2_init(x+dx,y+dy),scale);
        const shz_vec2_t uv=shz_vec2_mul(shz_vec2_init(dx,dy),inverse_size[i]);
        pvr_vertex_t *v=pvr_dr_target();v->flags=corner==3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v->x=xy.x*ENJ_XSCALE;v->y=xy.y;v->z=p->foreground ? 1.0f : 0.000001f;
        v->u=uv.x;v->v=uv.y;v->argb=0xffffffff;v->oargb=0;pvr_dr_commit(v);
    }
}

static void record_part(unsigned part,unsigned x,unsigned y) {
    record_draws[record_count].part=part;
    record_draws[record_count].x=x+parts[part].x;
    record_draws[record_count++].y=y+parts[part].y;
}

int nm_name_labels_record(unsigned rank,uint32_t total,uint16_t best,int show_best) {
    if(!ready || pending || !rank || rank>24) return 0;
    record_count=0;
    const unsigned ranks=static_parts+22;
    if(rank>=10) record_part(ranks+rank/10,24,32);
    record_part(ranks+rank%10,40,32);
    const unsigned last=rank%10;
    const unsigned suffix=rank/10==1 || last<1 || last>3 ? 13 : 9+last;
    record_part(ranks+suffix,56,32);
    const uint32_t times[]={total,best};
    for(unsigned t=0;t<(show_best ? 2u : 1u);++t) {
        uint16_t glyphs[8];nm_result_time_glyphs(times[t],glyphs);
        for(unsigned i=0;i<8;++i) {
            const unsigned code=glyphs[i]&255;
            if(code==0x20) continue;
            const unsigned glyph=code==0x27 ? 10 : code==0x22 ? 11 : code-0x30;
            record_part(static_parts+10+glyph,(t ? 376 : 152)+8*i,336);
        }
    }
    return 1;
}

static void render(void *unused) {
    (void)unused;pending=0;
    const unsigned glyphs=static_parts;
    for(unsigned i=0;i<glyphs;++i) render_part(i,parts[i].x,parts[i].y);
    for(unsigned i=0;i<record_count;++i)
        render_part(record_draws[i].part,record_draws[i].x,record_draws[i].y);
    if(countdown>=0) {
        /* Original227660 ->18478: offset02ec, two16px glyphs, blank leading tens. */
        if(countdown>=10) {
            const unsigned tens=glyphs+(unsigned)countdown/10;
            render_part(tens,432+parts[tens].x,40+parts[tens].y);
        }
        const unsigned units=glyphs+(unsigned)countdown%10;
        render_part(units,448+parts[units].x,40+parts[units].y);
    }
}

int nm_name_labels_enqueue(int seconds) {
    if(!ready || pending || seconds < -1 || seconds>20) return 0;
    countdown=seconds;
    pending=1;enj_render_list_add(PVR_LIST_PT_POLY,render,NULL);return 1;
}
