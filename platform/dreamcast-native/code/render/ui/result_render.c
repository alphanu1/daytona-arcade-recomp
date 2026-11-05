#include "render/ui/result_render.h"
#include "render/materials/result_materials.h"
#include "result_actor_data.h"
#include "lighting.h"
#include "scene_loader.h"
#include <enDjinn/enj_enDjinn.h>
#include "nm_mem.h"

static nm_pack_t pack;
alignas(32) static pvr_poly_hdr_t headers[8][256];
alignas(32) static nm_cached_vertex_t vertices[8][4];
static unsigned shades[8],visible[8],ready,frames;
static nm_view_t view;
static const uint32_t models[]={9263862,9263799,9263883,9263820,9263904,9263946,9263841,9263925};

void nm_result_render_unload(void) {
    if(pack.data) printf("Native result draw: frames=%u; cached PT headers, direct SQ, resident PAL4\n",frames);
    nm_pack_unload(&pack);ready=frames=0;
}

static int prepare(const nm_result_actor_t *actor,const nm_drive_params_t *math,int verify) {
    const float light[3]={0,0,1};
    for(unsigned part=0;part<8;++part) {
        const nm_mesh_t *mesh=pack.meshes+pack.instances[part].mesh;
        const nm_face_t *face=pack.faces+mesh->face_first;
        alignas(32) shz_mat3x4_t pose;
        if(!nm_result_actor_pose(math,actor,part,pose.elem)) return 0;
        shz_xmtrx_load_3x4(&pose);
        shz_vec3_t normal=shz_xmtrx_transform_vec3(shz_vec3_init(face->normal[0],face->normal[1],face->normal[2]));
        float points[3][3],reference[3][3],ref_normal[3];
        for(unsigned i=0;i<4;++i) {
            const nm_vertex_t *v=pack.vertices+mesh->vertex_first+face->indices[i];
            const shz_vec3_t p=shz_xmtrx_transform_point3(shz_vec3_init(v->position[0],v->position[1],v->position[2]));
            const float xyz[3]={p.x,p.y,p.z};
            if(i<3) nm_memcpy(points[i],xyz,sizeof(xyz));
            if(verify) for(unsigned a=0;a<3;++a) {
                const float *m=pose.elem,*q=v->position;
                const float expected=m[a]*q[0]+m[a+3]*q[1]+m[a+6]*q[2]+m[a+9];
                if(!isfinite(xyz[a]) || fabsf(xyz[a]-expected)>0.00001f) return 0;
                if(i<3) reference[i][a]=expected;
                ref_normal[a]=m[a]*face->normal[0]+m[a+3]*face->normal[1]+m[a+6]*face->normal[2];
            }
            const nm_view_vertex_t focused={p.x*280.0f,p.y*280.0f,p.z,v->u*(1.0f/256.0f),v->v*(1.0f/256.0f)};
            nm_cache_vertex(&view,&focused,&vertices[part][i]);
        }
        const float n[3]={normal.x,normal.y,normal.z};
        const int lit=nm_face_lighting_eval(mesh->mode&3,n,points,light,pack.materials+face->material,1);
        if(lit<0 || (verify && lit!=nm_face_lighting(mesh->mode&3,ref_normal,reference,light,pack.materials+face->material))) return 0;
        shades[part]=(unsigned)lit&255;
        visible[part]=(face->attribute&(1u<<17)) || !(lit&256);
        if(!headers[part][shades[part]].cmd) return 0;
    }
    return 1;
}

static void render(void *unused) {
    (void)unused;
    for(unsigned part=0;part<8;++part) if(visible[part]) {
        pvr_poly_hdr_t *header=pvr_dr_target();*header=headers[part][shades[part]];pvr_dr_commit(header);
        for(unsigned t=1;t<3;++t) {
            const nm_cached_vertex_t *tri[]={vertices[part],vertices[part]+t,vertices[part]+t+1};
            nm_screen_vertex_t polygon[NM_CLIP_VERTICES];
            const unsigned count=nm_project_cached_triangle(&view,tri,polygon);
            for(unsigned k=1;k+1<count;++k) {
                const unsigned indices[]={0,k,k+1};
                for(unsigned j=0;j<3;++j) {
                    const nm_screen_vertex_t *p=polygon+indices[j];pvr_vertex_t *v=pvr_dr_target();
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

int nm_result_render_enqueue(const nm_result_actor_t *actor,const nm_drive_params_t *math,const int16_t center[2]) {
    if(!ready || !center) return 0;
    view=(nm_view_t){{center[0],384-center[1]},{0,496,0,384},0.01f};
    if(!nm_view_valid(&view) || !prepare(actor,math,0)) return 0;
    enj_render_list_add(PVR_LIST_PT_POLY,render,NULL);return 1;
}

int nm_result_render_load(const char *root,const nm_drive_params_t *math) {
    if(!root || !math || pack.data) return 0;
    char path[512];
    if(snprintf(path,sizeof(path),"%sresult.dnm",root)>=(int)sizeof(path) || !nm_pack_load(&pack,path)) return 0;
    if(pack.header->instances!=8 || pack.header->meshes!=8 || pack.header->faces!=8 || pack.header->vertices!=32) goto fail;
    nm_memset(headers,0,sizeof(headers));
    for(unsigned part=0;part<8;++part) {
        const nm_instance_t *instance=pack.instances+part;const nm_mesh_t *mesh=pack.meshes+instance->mesh;
        const nm_face_t *face=pack.faces+mesh->face_first;
        if(instance->source_address!=models[part] || mesh->vertex_count!=4 || mesh->face_count!=1 ||
           face->count!=4 || !(face->attribute&(3u<<8))) goto fail;
        for(unsigned luma=0;luma<256;++luma) {
            unsigned bank;const enj_texture_info_t *tex=nm_result_material(pack.materials[face->material].header,luma,&bank);
            if(!tex) continue;
            pvr_poly_cxt_t context;
            pvr_poly_cxt_txr(&context,PVR_LIST_PT_POLY,tex->pvrformat|PVR_TXRFMT_4BPP_PAL(bank),32,32,tex->ptr,PVR_FILTER_NONE);
            context.gen.culling=PVR_CULLING_NONE;
            context.depth.comparison=PVR_DEPTHCMP_ALWAYS;context.depth.write=PVR_DEPTHWRITE_DISABLE;
            pvr_poly_compile(&headers[part][luma],&context);nm_memset((char *)&headers[part][luma]+16,0,16);
        }
    }
    view=(nm_view_t){{248,176},{0,496,0,384},0.01f};
    static int oracle_done; /* the playback oracle once per power-on (the attract loop reloads the scene at each course) */
    if(oracle_done) { ready=1;puts("Native result renderer: resident; its oracle passed at power-on");return 1; }
    for(unsigned variant=0;variant<2;++variant) {
        nm_result_playback_t state={0};int32_t timer=576;
        for(unsigned i=0;i<8;++i) state.actor.other_angle[i]=(int16_t)(i*8191u);
        if(!nm_result_playback_init(&state,nm_result_initial,0,0,(int16_t)variant)) goto fail;
        for(unsigned tick=0;tick<450;++tick) {
            if(!(tick&15u)) nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
            if(!nm_result_playback_step(&state,nm_result_keys[variant],21,nm_result_targets,37,&timer,0) ||
               !prepare(&state.actor,math,1)) goto fail;
        }
    }
    ready=oracle_done=1;puts("Native result renderer PASS:900 authored ticks,28800 FTRV vertices, exact lighting/rear oracle, resident light bindings");return 1;
fail:
    puts("FAIL result renderer startup");nm_result_render_unload();return 0;
}
