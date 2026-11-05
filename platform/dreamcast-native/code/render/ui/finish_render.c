#include "render/ui/finish_render.h"
#include "render/materials/palette_ram.h"
#include "meshpack.h"
#include "geometry.h"
#include "scene_loader.h"
#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>
#include "nm_mem.h"

static nm_pack_t pack;
static enj_texture_info_t textures[4];
alignas(32) static pvr_poly_hdr_t headers[4];
alignas(32) static shz_mat3x4_t poses[4];
static nm_view_t view;
static unsigned ready, frames;
/* Authored draw order; exact offline PAL4 snapshot, not a material cache.
 * Banks0/1 are reserved here: race world/car/sky and qfont are non-palettized.
 * ponytail: four fixed assets; use a generated binding table if this UI grows. */
static const unsigned models[]={9263988,9264030,9263967,9264009};
static const unsigned files[]={2,5,1,4}, banks[]={1,0,0,1};
static const uint16_t material_headers[4][4]={
    {25600,0,7835,1216},{25600,0,8027,1152},
    {25600,0,7834,1152},{25600,0,7899,1216}};

void nm_finish_render_unload(void) {
    if(pack.data) {
        const int idle=!pvr_wait_ready() && !pvr_wait_render_done();
        if(idle) {
            for(unsigned i=0;i<4;++i)
                if(textures[i].ptr) enj_texture_unload(textures+i);
        } else enj_state_flag_shutdown(NULL);
        printf("Native finish PAL4: frames=%u; startup-only textures/palettes\n",frames);
    }
    nm_pack_unload(&pack);
    nm_memset(textures,0,sizeof(textures));ready=frames=0;
}

static void points(unsigned part,nm_cached_vertex_t out[4]) {
    const nm_mesh_t *mesh=pack.meshes+pack.instances[part].mesh;
    const nm_face_t *face=pack.faces+mesh->face_first;
    shz_xmtrx_load_3x4(poses+part);
    for(unsigned i=0;i<4;++i) {
        const nm_vertex_t *v=pack.vertices+mesh->vertex_first+face->indices[i];
        const shz_vec3_t p=shz_xmtrx_transform_point3(shz_vec3_init(
            v->position[0],v->position[1],v->position[2]));
        const nm_view_vertex_t focused={p.x*280.0f,p.y*280.0f,p.z,
                                       v->u*(1.0f/256.0f),v->v*(1.0f/256.0f)};
        nm_cache_vertex(&view,&focused,out+i);
    }
}

static void render(void *unused) {
    (void)unused;
    for(unsigned part=0;part<4;++part) {
        nm_cached_vertex_t vertices[4];points(part,vertices);
        pvr_poly_hdr_t *header=pvr_dr_target();*header=headers[part];pvr_dr_commit(header);
        for(unsigned t=1;t<3;++t) {
            const nm_cached_vertex_t *triangle[]={vertices,vertices+t,vertices+t+1};
            nm_screen_vertex_t polygon[NM_CLIP_VERTICES];
            const unsigned count=nm_project_cached_triangle(&view,triangle,polygon);
            for(unsigned k=1;k+1<count;++k) {
                const unsigned indices[]={0,k,k+1};
                for(unsigned j=0;j<3;++j) {
                    const nm_screen_vertex_t *p=polygon+indices[j];
                    pvr_vertex_t *v=pvr_dr_target();
                    v->flags=j==2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                    v->x=p->x*(640.0f/496.0f)*ENJ_XSCALE;v->y=p->y*(480.0f/384.0f);v->z=p->q;
                    v->u=shz_divf_fsrra(p->uq,p->q);v->v=shz_divf_fsrra(p->vq,p->q);
                    v->argb=0xffffffff;v->oargb=0;pvr_dr_commit(v);
                }
            }
        }
    }
    ++frames;
}

int nm_finish_render_enqueue(const nm_finish_actor_t *actor,int branch,const int16_t center[2]) {
    if(!ready || !actor || !center) return 0;
    /* Use the native camera's logical center, not a captured arcade viewport.
     * Finish poses already live in view space: do not apply the car camera twice. */
    view=(nm_view_t){{center[0],384-center[1]},{0,496,0,384},0.01f};
    if(!nm_view_valid(&view)) return 0;
    for(unsigned i=0;i<4;++i)
        if(!nm_finish_actor_pose(actor,i,branch,poses[i].elem)) return 0;
    enj_render_list_add(PVR_LIST_PT_POLY,render,NULL);return 1;
}

int nm_finish_render_load(const char *root) {
    if(ready || pack.data || !root) return 0;
    const char *stage="mesh";unsigned part=0;
    char path[512];
    if(snprintf(path,sizeof(path),"%sfinish.dnm",root)>=(int)sizeof(path) ||
       !nm_pack_load(&pack,path)) return 0;
    if(pack.header->instances!=4 || pack.header->meshes!=4 || pack.header->vertices!=16 ||
       pack.header->faces!=4) goto fail;
    for(unsigned i=0;i<4;++i) {
        part=i;stage="mesh binding";
        const nm_instance_t *instance=pack.instances+i;
        const nm_mesh_t *mesh=pack.meshes+instance->mesh;
        const nm_face_t *face=pack.faces+mesh->face_first;
        if(instance->source_address!=models[i] || mesh->vertex_count!=4 ||
           mesh->face_count!=1 || face->count!=4 ||
           memcmp(pack.materials[face->material].header,material_headers[i],8)) goto fail;
        for(unsigned v=0;v<4;++v) {
            const nm_vertex_t *p=pack.vertices+mesh->vertex_first+v;
            if(p->u>256 || p->v>256) goto fail;
        }
        stage="texture load";
        if(snprintf(path,sizeof(path),"%sfinish/texture_%u.dt",root,files[i])>=(int)sizeof(path) ||
           !enj_texture_load_file(path,textures+i)) goto fail;
        stage="texture format";
        if(textures[i].width!=32 || textures[i].height!=32 ||
           textures[i].pvrformat!=PVR_TXRFMT_PAL4BPP || textures[i].flags.compressed ||
           textures[i].flags.mipmapped || !textures[i].flags.twiddled) goto fail;
        pvr_poly_cxt_t context;
        pvr_poly_cxt_txr(&context,PVR_LIST_PT_POLY,
            textures[i].pvrformat|PVR_TXRFMT_4BPP_PAL(banks[i]),32,32,textures[i].ptr,PVR_FILTER_NONE);
        context.gen.culling=PVR_CULLING_NONE;
        /* Presentation overlay, after world PT, without changing world depth. */
        context.depth.comparison=PVR_DEPTHCMP_ALWAYS;context.depth.write=PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(headers+i,&context);nm_memset((char *)(headers+i)+16,0,16);
    }
    for(unsigned bank=0;bank<2;++bank) {
        part=bank;stage="palette";
        alignas(32) uint8_t palette[24];
        if(snprintf(path,sizeof(path),"%sfinish/texture_%u.dt.pal",root,bank ? 2 : 1)>=(int)sizeof(path)) goto fail;
        FILE *file=fopen(path,"rb");if(!file) goto fail;
        const int valid=fread(palette,1,sizeof(palette),file)==sizeof(palette) && fgetc(file)==EOF;
        fclose(file);
        if(!valid) goto fail;
        uint32_t colors;nm_memcpy(&colors,palette+4,sizeof(colors));
        if(colors!=4 || !nm_palette_ram_write_dpal(palette,bank*16,"finish actor palette")) goto fail;
    }
    /* Actual mesh/386 actor ticks: FTRV versus independent scalar affine math.
     * No per-frame oracle; the target's FSRRA/clipping path is already tested. */
    nm_finish_actor_t actor;nm_finish_actor_init(&actor,0);
    stage="vertex oracle";
    view=(nm_view_t){{248,176},{0,496,0,384},0.01f};
    static int oracle_done; /* once per power-on (the attract loop reloads the scene at each course) */
    if(oracle_done) { ready=1;puts("Native finish PAL4: resident; its vertex oracle passed at power-on");return 1; }
    for(unsigned tick=0;tick<386;++tick) {
        if(!(tick&15u)) nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        const int branch=nm_finish_actor_step(&actor);
        for(unsigned part=0;part<4;++part) {
            if(!nm_finish_actor_pose(&actor,part,branch,poses[part].elem)) goto fail;
            nm_cached_vertex_t actual[4];points(part,actual);
            const nm_mesh_t *mesh=pack.meshes+pack.instances[part].mesh;
            const nm_face_t *face=pack.faces+mesh->face_first;
            for(unsigned i=0;i<4;++i) for(unsigned a=0;a<3;++a) {
                const float *p=pack.vertices[mesh->vertex_first+face->indices[i]].position;
                const float *m=poses[part].elem;
                const float reference=(m[a]*p[0]+m[a+3]*p[1]+m[a+6]*p[2]+m[a+9])*(a<2 ? 280.0f : 1.0f);
                const float result=a==0 ? actual[i].view.x : a==1 ? actual[i].view.y : actual[i].view.z;
                if(!isfinite(result) || fabsf(result-reference)>0.001f) goto fail;
            }
        }
    }
    ready=oracle_done=1;puts("Native finish PAL4 PASS: 6176 vertex transforms; 2048 VRAM bytes, banks0/1; native camera center");
    return 1;
fail:
    printf("FAIL finish asset stage=%s part=%u format=%08lx size=%ux%u twiddled=%u\n",
        stage,part,(unsigned long)textures[part].pvrformat,textures[part].width,
        textures[part].height,textures[part].flags.twiddled);
    nm_finish_render_unload();return 0;
}
