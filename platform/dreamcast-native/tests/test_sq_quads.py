"""Run the actual SQ emitter on host; capture writes instead of touching PVR."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "include/render/models/model_submission.h").read_text()
batch = source[source.index("typedef struct {"):source.index("static int sq_failed;")]
emitter = source[source.index("/* Flat-shaded strips (palette-shaded cars, DNT6)."):]
facing_marker = "#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)\n/* |value| <= 2^20, hence finite"
prepare = source[source.index(facing_marker):source.index("static int sq_prepare(void)")]
# Since 17 October the world's frame bound (nm_render_world_capacity) precedes sq_allocate: taken with it.
allocate = source[source.index("/* The world's frame bound (nm_render_world_capacity)"):source.index(facing_marker)]
renderer = (root / "code/render/models/model_renderer.c").read_text()
enqueue = renderer[renderer.index("int nm_render_enqueue_objects("):renderer.index("int nm_render_enqueue(void)")]
first_frame_reset = enqueue[enqueue.index("#ifdef NATIVE_CACHE_PROFILE"):enqueue.index("    const uint64_t started")]
release_reset = renderer[renderer.index("    draw_pack = NULL; draw_count = draw_samples = 0;"):]
types = renderer
types = types[types.index("static const nm_pack_t *draw_pack;"):types.index("static uint16_t *draw_bindings;")]
types = "\n".join(line for line in types.splitlines() if not line.startswith("_Static_assert"))
harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include "geometry.h"
#include "meshpack.h"
#include "material_pack.h"
#include "render/models/scenery_camera.h"
#include "heap.h"
#include "nm_mem.h"
#include "scene_loader.h" /* nm_loader_yield: a no-op on the host */
#define PVR_LIST_OP_POLY 0
#define PVR_LIST_PT_POLY 4
#define PVR_LIST_TR_POLY 2
#define PVR_CMD_VERTEX 0xe0000000u
#define PVR_CMD_VERTEX_EOL 0xf0000000u
#define NM_DRAW_TRIANGLES 512
#define NM_SKY_DEPTH 0.000001f
#define DRAW_PHASE_START(x)
#define DRAW_PHASE_NEXT(a,b,c)
#define DRAW_PHASE_END(a,b)
#define DRAW_PROFILE_SOURCE(s)
/* The host fixture has a deliberately smaller triangle-only stream. Target
 * builds supply the byte-accurate TA guard from model_renderer.c. */
static int draw_ta_fits(unsigned triangles,unsigned headers) {
    (void)headers;
    return triangles<=NM_DRAW_TRIANGLES;
}
typedef struct { uint32_t flags; float x,y,z,u,v; uint32_t argb,oargb; } pvr_vertex_t;
typedef struct { uint32_t cmd,mode1,mode2,mode3,padding[4]; } pvr_poly_hdr_t;
typedef void *pvr_ptr_t;
static union { pvr_vertex_t vertex; pvr_poly_hdr_t header; } stream[2048];
static unsigned writes;
static void *pvr_dr_target(void) { assert(writes<2048); return stream+writes; }
static void pvr_dr_commit(void *p) { assert(p==stream+writes); ++writes; }
static float shz_divf_fsrra(float a,float b) { return a/b; }
/* The renderer counts nothing itself; the fixture owns these statistics. */
#define SQ_COUNT(counter,amount) ((counter)+=(amount))
#ifdef NATIVE_PROFILE
static uint64_t draw_phases[10],draw_clip_paths[5],draw_frame_begin_wall;
static unsigned draw_texture_header_reuse;
#endif
static int draw_inspect_strips;
static int draw_inspect_stipple; /* the console's stipple texture of the inspection view: off */
static int draw_untextured;
static pvr_poly_hdr_t draw_inspect_headers[2]={
    {.cmd=0x80840000,.mode1=1},{.cmd=0x80840000,.mode1=4}};
static const unsigned draw_lists[2]={0,4};
static int sq_failed;
static struct { unsigned list; void (*call)(void *); void *data; } queue[128];
static unsigned queued;
static void enj_render_list_add(unsigned list,void (*call)(void *),void *data) {
    assert(queued<128); queue[queued].list=list;
    queue[queued].call=call; queue[queued++].data=data;
}
'''
state = r'''
static struct { const nm_pack_t *pack; const nm_material_pack_t *materials;
    nm_draw_material_t *cache; uint16_t *bindings; } world_draw;
static const pvr_poly_hdr_t **sq_shared_headers[2];
static unsigned draw_count,draw_header_count,draw_opaque,draw_cutout,draw_rear,draw_checker,draw_instances_rejected;
static uint64_t sq_joined_strips,sq_strip_vertices_saved,sq_flat_runs[2],sq_flat_run_faces[2],sq_flat_run_vertices[2];
static uint16_t *draw_bindings;
static uint32_t *draw_runs; /* NULL: the dense table (model_submission.h sq_lookup) */
static unsigned light_calls,vertex_calls;
static const nm_face_t *hidden_face;
static float view_shift;
static void *memalign(size_t alignment,size_t size) {
    return aligned_alloc(alignment,(size+alignment-1)&~(alignment-1));
}
/* Target-only matrix load; host adapter below supplies transformed points. */
#define shz_xmtrx_load_3x4(matrix) ((void)0)
static int face_light(const nm_pack_t *pack,const nm_mesh_t *mesh,const nm_instance_t *inst,
                      const nm_face_t *face,int optimized) { ++light_calls; return 255; }
static unsigned reject_draw_face(const nm_face_t *face,unsigned header,int light) { return face==hidden_face; }
static nm_view_t instance_view(const nm_pack_t *pack,const nm_instance_t *inst) {
    return (nm_view_t){{248+view_shift,192},{0,496,0,384},0.01f};
}
static void cache_draw_vertex(const nm_pack_t *pack,const nm_mesh_t *mesh,const nm_instance_t *inst,
        const nm_view_t *view,unsigned v,nm_cached_vertex_t *out,int inside) {
    const nm_vertex_t *p=pack->vertices+mesh->vertex_first+v;
    const nm_view_vertex_t input={p->position[0]+inst->matrix[9],p->position[1],p->position[2],p->u,p->v};
    ++vertex_calls; nm_cache_vertex(view,&input,out);
}
'''
cache_checks = r'''
static void reset(void);
static void check_point_cache(void) {
    nm_header_t original={.materials=2,.instances=2},live=original;
    struct { nm_material_t materials[2]; nm_instance_t instances[2]; } tables={0};
    assert((const void *)(tables.materials+2)==(const void *)tables.instances);
    nm_vertex_t vertices[7]={
        {{-16,16,1},8,16},{{-16,16,1},72,80},{{16,16,1},24,32},
        {{16,-16,1},40,48},{{-16,-16,1},56,64},{{0,0,1},88,96},{{0,0,1},104,112}};
    const uint16_t refs[7]={0,0,2,3,4,5,6};
    nm_face_t faces[2]={ {.indices={1,2,3},.count=3},
        {.indices={4,2,3},.count=3,.material=1} };
    nm_mesh_t mesh={.vertex_count=7,.face_count=2,.bounds={-16,-16,1,16,16,1}};
    nm_pack_t pack={.data=&original,.header=&live,.materials=tables.materials,
        .instances=tables.instances,.vertices=vertices,.faces=faces,.meshes=&mesh,.position_refs=refs};
    nm_material_binding_t bindings[2]={
        {.resource=NM_MATERIAL_SOLID,.color=0xff123456},
        {.header={3u<<13},.resource=NM_MATERIAL_SOLID,.color=0xffabcdef}};
    nm_material_pack_t materials={.bindings=bindings};
    nm_draw_material_t cache[2]={0};
    uint16_t lookup[512];
    for(unsigned i=0;i<512;++i) lookup[i]=i/256;
    for(unsigned i=0;i<2;++i) {
        cache[i].header.cmd=0x80840000; cache[i].header.mode1=i;
        cache[i].uv=(nm_material_uv_t){.origin={4,8},.scale={1.0f/256,1.0f/128}};
        tables.instances[i]=(nm_instance_t){.matrix={1,0,0,0,1,0,0,0,1,0,0,0},.focus={1,1}};
        tables.instances[i].matrix[9]=i*10;
    }
    memset(sq_sources+1,0,sizeof(sq_sources[1]));
    assert(sq_allocate(1,&pack));
    assert(sq_sources[1].point_capacity==14 && sq_sources[1].face_capacity==4);
    for(unsigned i=0;i<14;++i) assert(nm_cached_outside(sq_sources[1].points+i)==64);
    nm_cached_vertex_t *points=sq_sources[1].points;
    nm_cached_set_outside(points,7);
    assert(sq_allocate(1,&pack) && nm_cached_outside(points)==7); /* No reinitialization. */
    world_draw.pack=&pack; world_draw.materials=&materials; world_draw.cache=cache;
    world_draw.bindings=lookup;
    sq_sources[0].count=0;
    live.instances=1;
    for(unsigned frame=0;frame<6;++frame) {
        tables.instances[0].matrix[9]=frame*3;
        view_shift=frame*2;
        const nm_view_t view=instance_view(&pack,tables.instances);
        nm_view_vertex_t stale={frame==1 ? -1000 : 100,0,frame==2 ? 0 : 2,8,16};
        nm_cache_vertex(&view,&stale,points);
        nm_cached_vertex_t canary;
        memset(&canary,0xa5,sizeof(canary)); nm_cached_set_outside(&canary,32);
        points[6]=canary;
        hidden_face=frame==3 ? faces+1 : frame==4 ? faces : NULL;
        reset(); vertex_calls=light_calls=0;
        assert(sq_prepare_source(1,&pack) && sq_sources[1].count==1);
        assert(!vertex_calls);
        sq_sources[1].batches[0].object_batches=1;
        sq_submit_object(sq_sources[1].batches,PVR_LIST_OP_POLY);
        assert(!sq_failed);
        const unsigned prepared_calls=vertex_calls;
        sq_submit_object(sq_sources[1].batches,PVR_LIST_PT_POLY);
        assert(!sq_failed && vertex_calls==prepared_calls);
        assert(writes==(hidden_face ? 4 : 8));
        assert(!memcmp(points+6,&canary,sizeof(canary)));
        for(unsigned f=0;f<2;++f) if(faces+f!=hidden_face) for(unsigned v=0;v<3;++v) {
            const unsigned index=faces[f].indices[v];
            const nm_vertex_t *vertex=vertices+index;
            const nm_view_vertex_t fresh={vertex->position[0]+tables.instances[0].matrix[9],
                vertex->position[1],vertex->position[2],vertex->u,vertex->v};
            nm_cached_vertex_t expected;
            nm_cache_vertex(&view,&fresh,&expected);
            assert(!memcmp(points+index,&expected,sizeof(expected)));
            assert(nm_cached_outside(points+index)<=32);
            const unsigned slot=(f && hidden_face!=faces ? 4 : 0)+1+v;
            const pvr_vertex_t *emitted=&stream[slot].vertex;
            assert(emitted->x==expected.x*(640.0f/496.0f)*ENJ_XSCALE);
            assert(emitted->y==expected.y*(480.0f/384.0f) && emitted->z==expected.q);
            assert(emitted->u==(vertex->u-4)*(1.0f/256));
            assert(emitted->v==(vertex->v-8)*(1.0f/128));
            assert(emitted->argb==bindings[f].color);
        }
        assert(prepared_calls==(frame==3 || frame==4 ? 3 : 4));
        for(unsigned i=0;i<14;++i) assert(nm_cached_outside(points+i)<=64);
    }
    /* Culled instances compact the next frame's batches onto another object's old points. */
    hidden_face=NULL; view_shift=0; live.instances=2;
    tables.instances[0].matrix[9]=0;
    reset(); assert(sq_prepare_source(1,&pack) && sq_sources[1].count==2);
    assert(sq_sources[1].batches[1].points==points+7);
    tables.instances[0].matrix[11]=-200; /* Behind the eye. */
    reset(); assert(sq_prepare_source(1,&pack) && sq_sources[1].count==1);
    assert(sq_sources[1].batches[0].instance_index==1 && sq_sources[1].batches[0].points==points);
    sq_sources[1].batches[0].object_batches=1;
    sq_submit_object(sq_sources[1].batches,PVR_LIST_OP_POLY); assert(!sq_failed);
    const nm_view_t view=instance_view(&pack,tables.instances+1);
    const nm_view_vertex_t fresh={vertices[1].position[0]+10,16,1,72,80};
    nm_cached_vertex_t expected; nm_cache_vertex(&view,&fresh,&expected);
    assert(!memcmp(points+1,&expected,sizeof(expected)));
    for(unsigned i=0;i<14;++i) assert(nm_cached_outside(points+i)<=64);
    free(sq_sources[1].batches); free(points); free(sq_sources[1].bindings);
    memset(sq_sources+1,0,sizeof(sq_sources[1])); world_draw.pack=NULL;
    puts("PASS: actual allocation initializes status once; stale representative q/outside/invalid, changed pose/view, selective OP/PT, own alias UV, untouched canary, consumed demands and reassigned batch segments");
}
'''
checks = r'''
static void reset(void) { writes=draw_count=draw_header_count=draw_opaque=draw_cutout=0; }
/* Every batch of every source on one list: the renderer itself emits per object. */
static int sq_emit(unsigned list) { return sq_emit_range(list,0,world_draw.pack ? 2 : 1,0,UINT_MAX); }
#include "sq_strips.h"
int main(int argc,char **argv) {
    assert(argc==2);
    FILE *output=fopen(argv[1],"wb"); assert(output);
    for(unsigned session=0;session<2;++session) {
        draw_samples=1; draw_prepare_sum=5; draw_interval_max=7;
        first_frame_reset();
        assert(draw_samples==1 && draw_prepare_sum==5);
        release_reset();
        assert(!draw_samples && !draw_prepare_sum && !draw_interval_max);
    }
    puts("PASS: actual first-frame/release resets, two sessions");
    {
        nm_header_t header={.instances=2};
        nm_instance_t instances[2]={0};
        instances[0].matrix[0]=instances[0].matrix[4]=instances[0].matrix[8]=1;
        instances[0].focus[0]=instances[0].focus[1]=280;
        instances[1]=instances[0];
        const nm_view_t view={{248,192},{0,496,0,384},0.01f};
        nm_pack_t scenery={.header=&header,.instances=instances};
        nm_scenery_camera_t camera;
        nm_bounds_planes_t reference;
        for(unsigned change=0;change<5;++change) {
            instances[1]=instances[0];
            if(change==1) instances[1].matrix[9]=1;
            if(change==2) instances[1].focus[0]=281;
            if(change==3) instances[1].viewport[0]=1;
            if(change==4) instances[1].center[0]=1;
            nm_scenery_camera_prepare(&camera,&scenery,&view);
            assert(camera.shared==!change);
            if(camera.shared) {
                nm_bounds_planes_prepare(&reference,&view,instances[0].matrix,instances[0].focus);
                assert(!memcmp(&camera.planes,&reference,sizeof(reference)));
            }
        }
        instances[0].matrix[9]=12;instances[1]=instances[0];
        nm_scenery_camera_prepare(&camera,&scenery,&view);
        nm_bounds_planes_prepare(&reference,&view,instances[0].matrix,instances[0].focus);
        assert(camera.shared && !memcmp(&camera.planes,&reference,sizeof(reference)));
        header.instances=0;nm_scenery_camera_prepare(&camera,&scenery,&view);assert(!camera.shared);
        puts("PASS: scenery shares camera planes only for identical transforms/focus/viewport/center; next-frame refresh");
    }
    nm_vertex_t vertices[4]={{{0},8,16},{{0},24,32},{{0},40,48},{{0},56,64}};
    nm_face_t faces[2]={ {.indices={0,1,2,3},.count=4} };
    nm_mesh_t mesh={.vertex_count=4,.face_count=1};
    nm_pack_t pack={.vertices=vertices,.faces=faces,.meshes=&mesh};
    nm_material_binding_t binding={.resource=NM_MATERIAL_SOLID,.color=0xff123456};
    nm_material_pack_t materials={.bindings=&binding};
    nm_draw_material_t cache={.header={.cmd=0x80840000}};
    cache.uv=nm_material_uv(&materials,&binding);
    nm_cached_vertex_t points[4];
    uint32_t entries[2]={0,1u<<16};
    nm_sq_batch_t batch={.mesh=&mesh,.view={{248,192},{0,496,0,384},0.01f},
        .points=points,.bindings=entries,.prepared=1};
    sq_sources[0].batches=&batch; sq_sources[0].count=1;
    draw_pack=&pack; draw_materials=&materials; draw_material_cache=&cache;
    const unsigned masks[5][4]={{0},{0},{2,2,2,2},{0,0,1,0},{0,0,3,0}};
    for(unsigned profile_case=0;profile_case<5;++profile_case) {
        for(unsigned i=0;i<4;++i) {
            nm_view_vertex_t p={(i&1)?0.1f:-0.1f,(i&2)?-0.1f:0.1f,1,8,16};
            if(profile_case==2) p.x=-1000;
            if(profile_case>=3 && i==2) { p.x=profile_case==4?-1000:0; p.y=0; p.z=0.001f; }
            nm_cache_vertex(&batch.view,&p,points+i);
            assert(nm_cached_outside(points+i)==masks[profile_case][i]);
        }
        faces[0].count=profile_case==0?4:3;
        batch.opaque_count=1; batch.cutout_count=0;
        reset(); memset(stream,0,sizeof(stream));
#ifdef NATIVE_PROFILE
        const uint64_t expected[5][5]={{2,0,0,0,0},{1,0,0,0,0},{0,1,0,1,0},
                                      {0,0,1,0,1},{0,0,1,1,1}};
        memset(draw_clip_paths,0,sizeof(draw_clip_paths));
#endif
        assert(sq_emit(PVR_LIST_OP_POLY));
#ifdef NATIVE_PROFILE
        printf("clip case=%u observed=%llu/%llu/%llu/%llu/%llu\n",profile_case,
            (unsigned long long)draw_clip_paths[0],(unsigned long long)draw_clip_paths[1],
            (unsigned long long)draw_clip_paths[2],(unsigned long long)draw_clip_paths[3],
            (unsigned long long)draw_clip_paths[4]);
        assert(!memcmp(draw_clip_paths,expected[profile_case],sizeof(draw_clip_paths)));
#endif
        assert(fwrite(&writes,sizeof(writes),1,output)==1);
        assert(fwrite(stream,sizeof(stream[0]),writes,output)==writes);
    }
    {
        /* DNM6 layers: the written depth is q times the layer's table entry,
         * composed with the road-surface scale; nothing else moves. */
        draw_layer_scale_init();
        assert(draw_layer_scale[0]==1.0f && draw_layer_scale[1]>1.0f && draw_layer_scale[255]<1.5f);
        for(unsigned i=0;i<4;++i) {
            nm_view_vertex_t p={(i&1)?32:-32,(i&2)?-24:24,1+i*0.25f,vertices[i].u,vertices[i].v};
            nm_cache_vertex(&batch.view,&p,points+i);
        }
        faces[0].count=4; mesh.face_count=1; entries[0]=0;
        batch.opaque_count=1; batch.cutout_count=0;
        pvr_vertex_t plain[5];
        reset(); assert(sq_emit(PVR_LIST_OP_POLY) && writes==5);
        for(unsigned i=0;i<5;++i) plain[i]=stream[i].vertex;
        uint8_t surface=1; unsigned checked=0;
        for(unsigned layer=0;layer<4;++layer) for(unsigned road=0;road<2;++road) {
            faces[0].layer=(uint8_t)layer;
            /* The road class exists for the world source only. */
            if(road) {
                sq_sources[1].batches=&batch; sq_sources[1].count=1; batch.source=1;
                world_draw.pack=&pack; world_draw.materials=&materials; world_draw.cache=&cache;
                sq_surface_bindings=&surface;
            }
            reset(); assert(sq_emit_range(PVR_LIST_OP_POLY,road,road+1,0,UINT_MAX) && writes==5);
            const float scale=(road ? NM_ROAD_DEPTH_SCALE : 1.0f)*(1.0f+(float)layer*NM_LAYER_DEPTH_STEP);
            for(unsigned i=1;i<5;++i) {
                pvr_vertex_t got=stream[i].vertex;
                assert(got.z==points[(i==1)?1:(i==2)?2:(i==3)?0:3].q*scale);
                if(layer) assert(got.z>plain[i].z*(road ? NM_ROAD_DEPTH_SCALE : 1.0f));
                got.z=plain[i].z;
                assert(!memcmp(&got,plain+i,sizeof(got)));
            }
            if(road) {
                /* Another camera's scale (sq_road_depth_set): the road class of the emission table, and so the
                 * emitter, take it; 1 is refused; then the build's scale again. */
                materials.count=1;
                assert(sq_emit_records_prepare(1) && sq_emit_records[1][0].group->depth==NM_ROAD_DEPTH_SCALE);
                sq_road_depth_set(1.0f); assert(sq_road_depth==NM_ROAD_DEPTH_SCALE);
                sq_road_depth_set(0.755f); assert(sq_road_depth==0.755f && sq_emit_records[1][0].group->depth==0.755f);
                reset(); assert(sq_emit_range(PVR_LIST_OP_POLY,1,2,0,UINT_MAX) && writes==5);
                for(unsigned i=1;i<5;++i)
                    assert(stream[i].vertex.z==points[(i==1)?1:(i==2)?2:(i==3)?0:3].q*(0.755f*(1.0f+(float)layer*NM_LAYER_DEPTH_STEP)));
                sq_road_depth_set(NM_ROAD_DEPTH_SCALE); assert(sq_emit_records[1][0].group->depth==NM_ROAD_DEPTH_SCALE);
                sq_emit_records_release(1); materials.count=0;
            }
            if(road) {
                sq_sources[1].batches=NULL; sq_sources[1].count=0; batch.source=0;
                world_draw.pack=NULL; world_draw.materials=NULL; world_draw.cache=NULL;
                sq_surface_bindings=NULL;
            }
            ++checked;
        }
        faces[0].layer=0;
        printf("PASS: face layers, %u layer/road-surface cases; depth only, exact table product; road class rewritten to another scale\n",checked);
    }
    faces[0].count=4;
    binding.color=0x7f123456; /* Exercise opaque-alpha forcing, including PT. */
    unsigned cases=0;
    for(unsigned list=0;list<=4;list+=4) for(unsigned clipped=0;clipped<7;++clipped) {
        for(unsigned i=0;i<4;++i) {
            nm_view_vertex_t p={(i&1)?32:-32,(i&2)?-24:24,1,vertices[i].u,vertices[i].v};
            if(i==3) {
                if(clipped==1) p.z=-1;
                if(clipped==2) p.x=-1000;
                if(clipped==3) p.x=1000;
                if(clipped==4) p.y=-1000;
                if(clipped==5) p.y=1000;
                if(clipped==6) { p.z=0.01f; p.x=0.32f; p.y=-0.24f; }
            }
            nm_cache_vertex(&batch.view,&p,points+i);
        }
        /* Independent old fan: two ordinary triangle faces. */
        faces[0].count=3; faces[1]=(nm_face_t){.indices={0,2,3},.count=3};
        mesh.face_count=2; batch.opaque_count=list==0?2:0; batch.cutout_count=list==4?2:0;
        if(list==4) { entries[0]=1u<<16; entries[1]=0; }
        else { entries[0]=0; entries[1]=1u<<16; }
        reset(); assert(sq_emit(list));
        const unsigned triangles=draw_count,old_writes=writes;
        /* The same picture as two separate triangles: what the quad must expand to. */
        struct { pvr_vertex_t vertices[3]; } reference[32]={0};
        assert(triangles<32 && writes==1+triangles*3);
        for(unsigned t=0;t<triangles;++t)
            for(unsigned v=0;v<3;++v) reference[t].vertices[v]=stream[1+t*3+v].vertex;
        faces[0].count=4; mesh.face_count=1; entries[0]=0;
        batch.opaque_count=list==0; batch.cutout_count=list==4;
        reset(); assert(sq_emit(list)); assert(draw_count==triangles && draw_header_count==1);
        if(!clipped || clipped==6) {
            assert(writes==5 && old_writes==7);
            /* Expand the actual emitted strip back to the two original triangles. */
            const unsigned fan[2][3]={{3,1,2},{3,2,4}};
            for(unsigned t=0;t<2;++t) for(unsigned v=0;v<3;++v) {
                pvr_vertex_t got=stream[fan[t][v]].vertex;
                assert(got.flags==(fan[t][v]==4?PVR_CMD_VERTEX_EOL:PVR_CMD_VERTEX));
                got.flags=reference[t].vertices[v].flags;
                assert(!memcmp(&got,&reference[t].vertices[v],sizeof(got)));
            }
        } else {
            assert(writes==old_writes);
            for(unsigned t=0;t<triangles;++t) for(unsigned v=0;v<3;++v)
                assert(!memcmp(&stream[1+t*3+v].vertex,&reference[t].vertices[v],sizeof(pvr_vertex_t)));
        }
        pvr_vertex_t normal[128];
        const unsigned normal_writes=writes,normal_triangles=draw_count;
        assert(writes<128);
        for(unsigned i=0;i<writes;++i) normal[i]=stream[i].vertex;
        reset(); draw_inspect_strips=1;
        assert(sq_emit(list) && writes==normal_writes && draw_count==normal_triangles);
        assert(!memcmp(&stream[0].header,draw_inspect_headers+(list==4),32));
        uint32_t strip_color=0;
        for(unsigned i=1;i<writes;++i) {
            pvr_vertex_t actual=stream[i].vertex,expected=normal[i];
            if(!strip_color) strip_color=actual.argb;
            assert(actual.argb==strip_color && (actual.argb>>24)==255);
            if(actual.flags==PVR_CMD_VERTEX_EOL) strip_color=0;
            actual.argb=expected.argb=0;
            assert(!memcmp(&actual,&expected,sizeof(actual)));
        }
        assert(!strip_color);
        draw_inspect_strips=0; reset();
        assert(sq_emit(list) && writes==normal_writes);
        for(unsigned i=0;i<writes;++i) assert(!memcmp(&stream[i].vertex,normal+i,32));
        draw_untextured=1; reset();
        assert(sq_emit(list) && writes==normal_writes &&
               draw_count==normal_triangles && draw_header_count==1);
        assert(!memcmp(&stream[0].header,draw_inspect_headers+(list==4),32));
        for(unsigned i=1;i<writes;++i) {
            pvr_vertex_t expected=normal[i];
            expected.u=expected.v=0;
            expected.argb|=0xff000000u;
            assert(!memcmp(&stream[i].vertex,&expected,sizeof(expected)));
        }
        draw_untextured=0; reset();
        assert(sq_emit(list) && writes==normal_writes && draw_count==normal_triangles);
        for(unsigned i=0;i<writes;++i) assert(!memcmp(&stream[i].vertex,normal+i,32));
        ++cases;
    }
    binding.color=0xff123456;
    faces[1]=faces[0]; mesh.face_count=2; batch.cutout_count=2;
    entries[0]=1u<<16; entries[1]=0;
    reset(); assert(sq_emit(PVR_LIST_PT_POLY));
    assert(writes==9 && draw_count==4 && draw_header_count==1);
    for(unsigned i=1;i<9;++i)
        assert(stream[i].vertex.flags==(i%4==0?PVR_CMD_VERTEX_EOL:PVR_CMD_VERTEX));
    reset(); draw_count=NM_DRAW_TRIANGLES-1;
    assert(!sq_emit(PVR_LIST_PT_POLY) && writes==0);
    /* Three car slots: two parts, a culled car, then one part. One scenery
     * instance. Group by object, not part, preserving OP/PT membership. */
    nm_header_t header={.instances=4}; pack.header=&header;
    const unsigned boundaries[]={0,2,3,4};
    nm_sq_batch_t car_batches[3]={batch,batch,batch},world_batch=batch;
    car_batches[0].instance_index=0; car_batches[1].instance_index=1;
    car_batches[2].instance_index=3;
    for(unsigned i=0;i<3;++i) { car_batches[i].opaque_count=1; car_batches[i].cutout_count=0; }
    car_batches[1].cutout_count=1;
    world_batch.source=1; world_batch.opaque_count=1; world_batch.cutout_count=0;
    sq_sources[0].batches=car_batches; sq_sources[0].count=3;
    sq_sources[1].batches=&world_batch; sq_sources[1].count=1;
    world_draw.pack=&pack; world_draw.materials=&materials; world_draw.cache=&cache;
    assert(sq_queue_objects(boundaries,3,0) && queued==4);
    assert(queue[0].data==car_batches && queue[0].list==0);
    assert(queue[1].data==car_batches && queue[1].list==4);
    assert(queue[2].call==sq_submit_opponents && queue[2].list==0);
    assert(sq_opponents[0].count==1 && sq_opponents[0].objects[0]==car_batches+2);
    assert(queue[3].data==&world_batch && queue[3].list==0);
    assert(car_batches[0].object_batches==2 && car_batches[2].object_batches==1);
    reset(); queue[2].call(queue[2].data);
    assert(!sq_failed && writes==5 && draw_count==2); /* Only this car, no other batches. */
    reset(); queue[0].call(queue[0].data);
    assert(!sq_failed && writes==9 && draw_count==4); /* Both parts under one callback/header. */
    /* Same mesh, distinct scene material/header: both scenery callbacks
     * must use the world source, never the car's material cache. */
    nm_material_binding_t scenery_binding=binding;
    scenery_binding.color=0xff987654;
    nm_material_pack_t scenery_materials=materials;
    scenery_materials.bindings=&scenery_binding;
    nm_draw_material_t scenery_cache=cache;
    scenery_cache.header.mode1=0x1234;
    world_draw.materials=&scenery_materials; world_draw.cache=&scenery_cache;
    world_batch.cutout_count=1; queued=0;
    assert(sq_queue_objects(boundaries,3,0) && queued==5);
    for(unsigned q=3;q<5;++q) {
        reset(); queue[q].call(queue[q].data);
        assert(!sq_failed && writes==5 && draw_count==2);
        assert(stream[0].header.mode1==scenery_cache.header.mode1);
        for(unsigned v=1;v<5;++v) assert(stream[v].vertex.argb==scenery_binding.color);
    }
    world_draw.materials=&materials; world_draw.cache=&cache;
    /* Real preparation is lazy and executes once for OP+PT. Host math
     * adapters do not validate SH4 FTRV. */
    nm_instance_t instance={.matrix={1,0,0,0,1,0,0,0,1,0,0,0},.focus={1,1}};
    nm_material_t material={0}; pack.materials=&material; pack.instances=&instance;
    for(unsigned i=0;i<4;++i) {
        vertices[i].position[0]=(i&1)?32:-32;
        vertices[i].position[1]=(i&2)?-24:24;
        vertices[i].position[2]=1;
    }
    mesh.bounds[0]=-32; mesh.bounds[1]=-24; mesh.bounds[2]=1;
    mesh.bounds[3]=32; mesh.bounds[4]=24; mesh.bounds[5]=1;
    uint16_t lookup[256]={0};
    draw_bindings=lookup;
    sq_sources[0].points=points; sq_sources[0].bindings=entries;
    sq_sources[0].batch_capacity=3; sq_sources[0].point_capacity=4; sq_sources[0].face_capacity=2;
    header.instances=1; world_draw.pack=NULL; queued=0; reset();
    assert(sq_prepare_source(0,&pack));
    assert(!light_calls && !vertex_calls && !car_batches[0].prepared);
    assert(sq_queue_objects(NULL,0,0) && queued==2);
    assert(!light_calls && !vertex_calls && writes==0);
    queue[0].call(queue[0].data);
    assert(car_batches[0].prepared && light_calls==2 && vertex_calls==4);
    assert(writes==9);
    queue[1].call(queue[1].data);
    assert(light_calls==2 && vertex_calls==4 && writes==9);
    /* Forty mixed-list objects: player plus one opponent call per list. Changing
     * poses next frame must invalidate caches, not the borrowed queue data. */
    nm_instance_t fleet[40]; nm_sq_batch_t fleet_batches[40];
    nm_cached_vertex_t fleet_points[160]; uint32_t fleet_faces[80];
    for(unsigned i=0;i<160;++i) nm_cached_set_outside(fleet_points+i,64);
    unsigned fleet_boundaries[41];
    nm_material_t fleet_materials[2]={{0},{0}};
    nm_material_binding_t fleet_bindings[2]={
        {.resource=NM_MATERIAL_SOLID,.color=0xff123456},
        {.header={3u<<13},.resource=NM_MATERIAL_SOLID,.color=0xffabcdef}};
    nm_draw_material_t fleet_cache[2]={cache,cache};
    uint16_t fleet_lookup[512];
    for(unsigned i=0;i<512;++i) fleet_lookup[i]=i/256;
    fleet_cache[1].header.mode1=4; /* Different state, not just different colour. */
    materials.bindings=fleet_bindings; draw_material_cache=fleet_cache;
    pack.materials=fleet_materials; pack.instances=fleet; header.instances=40;
    faces[1].material=1; world_draw.pack=NULL;
    draw_bindings=fleet_lookup;
    sq_sources[0].batches=fleet_batches; sq_sources[0].points=fleet_points;
    sq_sources[0].bindings=fleet_faces; sq_sources[0].batch_capacity=40;
    sq_sources[0].point_capacity=160; sq_sources[0].face_capacity=80;
    for(unsigned i=0;i<=40;++i) fleet_boundaries[i]=i;
    for(unsigned frame=0;frame<2;++frame) {
        for(unsigned i=0;i<40;++i) { fleet[i]=instance; fleet[i].matrix[9]=(float)(i+frame*40); }
        reset(); queued=light_calls=vertex_calls=0;
        assert(sq_prepare_source(0,&pack) && sq_sources[0].count==40);
        const unsigned player=frame ? 17 : 0;
        assert(sq_queue_objects(fleet_boundaries,40,player) && queued==4);
        assert(sq_opponents[0].count==39 && sq_opponents[1].count==39);
        assert(!light_calls && !vertex_calls && !writes);
        for(unsigned list=0;list<=4;list+=4) {
            for(unsigned q=0;q<queued;++q) if(queue[q].list==list) queue[q].call(queue[q].data);
            assert(!sq_failed && light_calls==80 && vertex_calls==160);
        }
        assert(writes==400 && draw_count==160 && draw_header_count==80);
        for(unsigned list=0;list<2;++list) for(unsigned slot=0;slot<40;++slot) {
            const unsigned i=slot==0 ? player : slot<=player ? slot-1 : slot;
            const pvr_vertex_t *v=&stream[list*200+slot*5+1].vertex;
            assert(v->x==(248.0f+32.0f+(float)(i+frame*40))*(640.0f/496.0f)*ENJ_XSCALE);
            assert(v->argb==fleet_bindings[list].color);
        }
    }
    queued=0;
    assert(sq_queue_objects(fleet_boundaries,40,40) && queued==2);
    assert(sq_opponents[0].count==40 && sq_opponents[1].count==40);
    queued=0; assert(!sq_queue_objects(fleet_boundaries,45,0) && !queued);
    assert(!sq_queue_objects(fleet_boundaries,40,41) && !queued);
    sq_sources[0].count=0;
    assert(sq_queue_objects(fleet_boundaries,40,17) && !queued);
    sq_sources[0].count=1; header.instances=1;
    assert(sq_queue_objects(fleet_boundaries,1,0) && queued==2);
    assert(!sq_opponents[0].count && !sq_opponents[1].count);
    queued=0; sq_sources[0].count=1; header.instances=40;
    assert(sq_queue_objects(fleet_boundaries,40,17) && queued==2);
    assert(sq_opponents[0].count==1 && sq_opponents[1].count==1); /* Player culled. */
    puts("PASS: 40 cars, 4 OP/PT callbacks; player ordinal0/17, no-player batch, lazy preparation");
    printf("PASS: actual SQ emitter, %u OP/PT cases, quad/clipping/capacity; strip colours and untextured output preserve geometry and material-mode restoration; object queues, lazy preparation once\n",cases);
    check_point_cache();
    check_offline_strips(output);
    assert(!fclose(output));
}
'''
lifecycle = r'''
static unsigned draw_samples;
static uint64_t draw_prepare_sum,draw_prepare_max;
static uint64_t draw_previous_update,draw_interval_sum,draw_interval_max;
static void first_frame_reset(void) {
''' + first_frame_reset + '}\nstatic void release_reset(void) {\n' + release_reset
with tempfile.TemporaryDirectory(prefix="daytona-quads-") as tmp:
    c = Path(tmp) / "check.c"
    c.write_text(harness + types + batch + state + allocate + prepare + emitter + lifecycle + cache_checks + checks)
    exe = Path(tmp) / "check"
    for scale in (1, 2):
        outputs=[]
        for profile in (False,True):
            command=[os.environ.get("HOST_CC", "cc"), "-std=c11", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-fast-math", "-ffp-contract=off",
                    "-DNATIVE_DIRECT_SQ", f"-DENJ_XSCALE={scale}",
                    "-I", str(root / "include"), "-I", str(root / "tests"), str(c), str(root / "code/geometry.c"),
                    str(root / "code/render/models/scenery_camera.c"),
                    "-lm", "-o", str(exe)]+(["-DNATIVE_PROFILE"] if profile else [])
            subprocess.run(command,check=True)
            output=Path(tmp)/f"stream-{scale}-{profile}.bin"
            subprocess.run([str(exe),str(output)], check=True)
            outputs.append(output.read_bytes())
        assert outputs[0]==outputs[1],f"profile changed SQ bytes at scale={scale}"
        print(f"PASS: profiled/unprofiled SQ headers/vertices byte-identical, {len(outputs[0])} bytes")
        print(f"PASS: SQ horizontal scale={scale}")
    # A representative no face asked for must be transformed again, not
    # trusted with an earlier frame's projection (formerly a status reset in
    # the face loop; now the wanted/fresh marks of the vertex pass).
    invalidation="const int fresh=(marks[representative]&(SQ_MARK_WANTED|SQ_MARK_FRESH))!=0;"
    assert prepare.count(invalidation)==1
    broken=prepare.replace(invalidation,"const int fresh=1;")
    c.write_text(harness + types + batch + state + allocate + broken + emitter + lifecycle + cache_checks + checks)
    subprocess.run(command,check=True)
    rejected=subprocess.run([str(exe),str(Path(tmp)/"mutant.bin")],capture_output=True,text=True)
    assert rejected.returncode!=0 and "memcmp(points+index" in rejected.stderr, rejected.stderr
    print("PASS: omitted representative invalidation rejected by independent fresh-point comparison")
    layering="if(face->layer) depth_scale*=draw_layer_scale[face->layer];"
    assert emitter.count(layering)==1
    c.write_text(harness + types + batch + state + allocate + prepare + emitter.replace(layering,"") + lifecycle + cache_checks + checks)
    subprocess.run(command,check=True)
    rejected=subprocess.run([str(exe),str(Path(tmp)/"mutant.bin")],capture_output=True,text=True)
    assert rejected.returncode!=0 and "points[(i==1)?1:(i==2)?2:(i==3)?0:3].q*scale" in rejected.stderr, rejected.stderr
    print("PASS: emitter without the layer factor rejected by the exact depth comparison")

