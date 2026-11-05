#include "render/ui/name_render.h"
#include "lighting.h"
#include <enDjinn/enj_defs.h>
#include <enDjinn/enj_render.h>
#include "nm_mem.h"

static const nm_pack_t *pack;
static const nm_name_layout_t *layout;
static const uint32_t (*colors)[2][64];
static const nm_drive_params_t *math_params;
static nm_name_entry_t entry;
static nm_name_marker_t marker;
static nm_view_t view;
static unsigned display_mode,frame,pending,failed;
static unsigned submitted_vertices;
static int intro_remaining;
static unsigned exit_phase;
static uint16_t exit_tick;
alignas(32) static pvr_poly_hdr_t header;

/* Same512KiB TA allocation/64KiB UI reserve as the normal game. Reject a whole
 * strip before writing it; clipping must never overflow TA or leave it open. */
static int reserve_strip(unsigned count) {
    if(count>(512u*1024u-64u*1024u)/32u-1u-submitted_vertices) {
        failed=1;return 0;
    }
    submitted_vertices+=count;return 1;
}

static void emit(float x,float y,float q,uint32_t argb,int last) {
    pvr_vertex_t *v=pvr_dr_target();
    v->flags=last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v->x=x*(640.0f/496.0f)*ENJ_XSCALE;v->y=y*(480.0f/384.0f);v->z=q;
    v->u=v->v=0;v->argb=argb;v->oargb=0;
    pvr_dr_commit(v);
}

static void render(void *unused) {
    (void)unused;pending=0;submitted_vertices=0;
    pvr_poly_hdr_t *target=pvr_dr_target();*target=header;pvr_dr_commit(target);
    const float light[3]={0,0,1};
    const unsigned draws=exit_phase ? 29+entry.count : intro_remaining>=0 ? 29 : nm_name_draw_count(&entry);
    for(unsigned draw=0;draw<draws;++draw) {
        alignas(32) shz_mat3x4_t pose;
        unsigned binding;
        int ok;
        if(exit_phase && draw<29) {
            binding=draw;
            ok=nm_name_exit_pose(math_params,layout,draw,exit_phase,exit_tick,pose.elem);
        } else if(intro_remaining>=0) {
            binding=draw;
            const float radius=(float)intro_remaining*0.03f+layout->radius[draw];
            ok=nm_name_ring_pose(math_params,radius,layout->angle[draw],
                                (uint16_t)((unsigned)intro_remaining<<11),pose.elem);
        } else ok=nm_name_draw_pose(math_params,layout,&entry,&marker,frame,draw,&binding,pose.elem);
        if(!ok) {
            failed=1;return;
        }
        const nm_mesh_t *mesh=pack->meshes+pack->instances[binding].mesh;
        shz_xmtrx_load_3x4(&pose);
        for(unsigned f=0;f<mesh->face_count;++f) {
            const nm_face_t *face=pack->faces+mesh->face_first+f;
            alignas(32) nm_cached_vertex_t points[4];
            float raw[3][3];
            for(unsigned i=0;i<face->count;++i) {
                const nm_vertex_t *v=pack->vertices+mesh->vertex_first+face->indices[i];
                const shz_vec3_t p=shz_xmtrx_transform_point3(shz_vec3_init(v->position[0],v->position[1],v->position[2]));
                if(i<3) { raw[i][0]=p.x;raw[i][1]=p.y;raw[i][2]=p.z; }
                const nm_view_vertex_t focused={p.x*280.0f,p.y*280.0f,p.z,0,0};
                nm_cache_vertex(&view,&focused,points+i);
            }
            const shz_vec3_t n=shz_xmtrx_transform_vec3(shz_vec3_init(face->normal[0],face->normal[1],face->normal[2]));
            const float normal[3]={n.x,n.y,n.z};
            const int lit=nm_face_lighting_eval(mesh->mode&3,normal,raw,light,pack->materials+face->material,1);
            if(lit<0) { failed=1;return; }
            if(!(face->attribute&(1u<<17)) && (lit&256)) continue;
            const uint32_t color=colors[face->material][display_mode][(lit&255)>>2];
            unsigned outside=0;
            for(unsigned i=0;i<face->count;++i) outside|=nm_cached_outside(points+i);
            if(!outside) {
                static const unsigned quad[]={1,2,0,3};
                if(!reserve_strip(face->count)) return;
                for(unsigned i=0;i<face->count;++i) {
                    const nm_cached_vertex_t *p=points+(face->count==4 ? quad[i] : i);
                    emit(p->x,p->y,p->q,color,i+1==face->count);
                }
            } else for(unsigned t=1;t+1<face->count;++t) {
                const nm_cached_vertex_t *tri[]={points,points+t,points+t+1};
                nm_screen_vertex_t clipped[NM_CLIP_VERTICES];
                const unsigned count=nm_project_cached_triangle(&view,tri,clipped);
                for(unsigned i=1;i+1<count;++i) {
                    if(!reserve_strip(3)) return;
                    const unsigned indices[]={0,i,i+1};
                    for(unsigned j=0;j<3;++j) {
                        const nm_screen_vertex_t *p=clipped+indices[j];
                        emit(p->x,p->y,p->q,color,j==2);
                    }
                }
            }
        }
    }
}

int nm_name_render_init(const nm_pack_t *source,const nm_name_layout_t *positions,
        const uint16_t headers[2][4],const uint32_t palette[2][2][64],unsigned mode) {
    if(pack || !source || !source->header || !positions || !headers || !palette || mode>1 ||
       source->header->instances!=30 || source->header->meshes!=30 || source->header->materials!=2) return 0;
    for(unsigned i=0;i<2;++i)
        if((source->materials[i].header[0]&0x4000) || memcmp(source->materials[i].header,headers[i],8)) return 0;
    pvr_poly_cxt_t context;
    pvr_poly_cxt_col(&context,PVR_LIST_OP_POLY);
    context.gen.culling=PVR_CULLING_NONE;
    context.depth.comparison=PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(&header,&context);nm_memset((char *)&header+16,0,16);
    pack=source;layout=positions;colors=palette;display_mode=mode;failed=pending=0;
    return 1;
}

static int prepare_enqueue(const nm_drive_params_t *math,const int16_t center[2]) {
    if(!pack || pending || failed || !math || !center) return 0;
    view=(nm_view_t){{center[0],384-center[1]},{0,496,0,384},0.01f};
    if(!nm_view_valid(&view)) return 0;
    math_params=math;pending=1;return 1;
}

int nm_name_render_enqueue(const nm_name_entry_t *state,const nm_name_marker_t *cursor,
        const nm_drive_params_t *math,const int16_t center[2],uint32_t tick) {
    if(!nm_name_draw_count(state) || !cursor || !prepare_enqueue(math,center)) return 0;
    entry=*state;marker=*cursor;frame=tick;intro_remaining=-1;exit_phase=0;
    enj_render_list_add(PVR_LIST_OP_POLY,render,NULL);
    return 1;
}

int nm_name_render_enqueue_intro(const nm_drive_params_t *math,const int16_t center[2],unsigned remaining) {
    if(remaining>31 || !prepare_enqueue(math,center)) return 0;
    intro_remaining=(int)remaining;exit_phase=0;
    enj_render_list_add(PVR_LIST_OP_POLY,render,NULL);
    return 1;
}

int nm_name_render_enqueue_exit(const nm_name_entry_t *state,const nm_drive_params_t *math,
        const int16_t center[2],unsigned phase,uint16_t tick) {
    if(!nm_name_draw_count(state) || (phase!=5 && phase!=7) || (phase==7 && tick>20) ||
       !prepare_enqueue(math,center)) return 0;
    entry=*state;intro_remaining=-1;exit_phase=phase;exit_tick=tick;frame=0;
    enj_render_list_add(PVR_LIST_OP_POLY,render,NULL);return 1;
}

int nm_name_render_release(void) {
    if(pending) return 0;
    pack=NULL;layout=NULL;colors=NULL;math_params=NULL;failed=0;
    return 1;
}
