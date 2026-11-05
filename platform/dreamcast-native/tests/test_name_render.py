"""Run the actual name callback with a captured SQ stream and host transforms.

Checks queue ownership, strips, headers and failure propagation, not SH4ZAM.
"""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    tmp=Path(directory)
    (tmp/'enDjinn').mkdir()
    for name in ('enj_defs.h','enj_render.h'):
        (tmp/'enDjinn'/name).write_text('/* Test backend declared by harness. */\n')
    harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdalign.h>
#include <string.h>
#define ENJ_XSCALE 1
#define PVR_LIST_OP_POLY 0
#define PVR_CULLING_NONE 0
#define PVR_DEPTHCMP_GEQUAL 6
#define PVR_CMD_VERTEX 0xe0000000u
#define PVR_CMD_VERTEX_EOL 0xf0000000u
typedef struct { uint32_t cmd,padding[7]; } pvr_poly_hdr_t;
typedef struct { uint32_t flags;float x,y,z,u,v;uint32_t argb,oargb; } pvr_vertex_t;
typedef struct { struct { int culling; } gen;struct { int comparison; } depth; } pvr_poly_cxt_t;
typedef struct { float elem[12]; } shz_mat3x4_t;
typedef struct { float x,y,z; } shz_vec3_t;
static shz_mat3x4_t xmtrx;
static void shz_xmtrx_load_3x4(const shz_mat3x4_t *m) { xmtrx=*m; }
static shz_vec3_t shz_vec3_init(float x,float y,float z) { return (shz_vec3_t){x,y,z}; }
static shz_vec3_t shz_xmtrx_transform_vec3(shz_vec3_t v) {
    const float *m=xmtrx.elem;
    return shz_vec3_init(m[0]*v.x+m[3]*v.y+m[6]*v.z,
        m[1]*v.x+m[4]*v.y+m[7]*v.z,m[2]*v.x+m[5]*v.y+m[8]*v.z);
}
static shz_vec3_t shz_xmtrx_transform_point3(shz_vec3_t v) {
    v=shz_xmtrx_transform_vec3(v);
    v.x+=xmtrx.elem[9];v.y+=xmtrx.elem[10];v.z+=xmtrx.elem[11];return v;
}
static union { pvr_vertex_t vertex;pvr_poly_hdr_t header; } stream[4096];
static unsigned writes,compiles,queued;
static void (*callback)(void *);
static void *pvr_dr_target(void) { assert(writes<4096);return stream+writes; }
static void pvr_dr_commit(void *p) { assert(p==stream+writes);++writes; }
static void pvr_poly_cxt_col(pvr_poly_cxt_t *p,int list) { assert(list==0);memset(p,0,sizeof(*p)); }
static void pvr_poly_compile(pvr_poly_hdr_t *h,const pvr_poly_cxt_t *p) {
    assert(p->depth.comparison==PVR_DEPTHCMP_GEQUAL);h->cmd=0x80840000;++compiles;
}
static void enj_render_list_add(int list,void (*fn)(void *),void *data) {
    assert(list==0 && !data);callback=fn;++queued;
}
#include "render/ui/name_render.c"
int main(void) {
    static nm_drive_params_t math;
    math.sine[16383]=0x3f800000;
    nm_header_t h={.instances=30,.meshes=30,.materials=2};
    nm_mesh_t meshes[30]={0};nm_instance_t instances[30]={0};
    nm_vertex_t vertices[4]={{{-1,-1,0},0,0},{{1,-1,0},0,0},{{1,1,0},0,0},{{-1,1,0},0,0}};
    nm_face_t face={.indices={0,1,2,3},.count=4,.attribute=1u<<17};
    nm_material_t materials[2]={{.ambient=255},{.ambient=255}};
    for(unsigned i=0;i<30;++i) { meshes[i].face_count=1;meshes[i].vertex_count=4;instances[i].mesh=i; }
    nm_pack_t asset={.header=&h,.meshes=meshes,.instances=instances,.vertices=vertices,.faces=&face,.materials=materials};
    nm_name_layout_t positions={0};uint16_t headers[2][4]={{0}};
    uint32_t palette[2][2][64];
    for(unsigned i=0;i<256;++i) ((uint32_t *)palette)[i]=0xff123456;
    nm_name_entry_t state={0};nm_name_marker_t cursor={0};const int16_t center[]={248,208};
    assert(nm_name_render_init(&asset,&positions,headers,palette,0));
    assert(compiles==1);
    assert(!nm_name_render_enqueue_intro(&math,center,32));
    assert(!nm_name_render_enqueue_intro(NULL,center,0));
    for(unsigned remaining=0;remaining<32;++remaining) {
        writes=0;
        assert(nm_name_render_enqueue_intro(&math,center,remaining));
        assert(writes==0 && !nm_name_render_enqueue_intro(&math,center,0));
        callback(NULL);
        assert(!failed);
        if(!remaining) assert(writes==1+29*4); /* No marker/initial preview. */
    }
    writes=0;queued=0;
    assert(nm_name_render_enqueue(&state,&cursor,&math,center,0));
    assert(queued==1 && writes==0); /* No geometry or SQ work in updater. */
    assert(!nm_name_render_release());
    assert(!nm_name_render_enqueue(&state,&cursor,&math,center,0));
    state.count=99; /* Queued entry/marker are snapshots. */
    callback(NULL);
    assert(writes==1+31*4 && stream[0].header.cmd==0x80840000);
    for(unsigned i=1;i<writes;++i) {
        assert(stream[i].vertex.flags==((i%4)==0 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX));
        assert(stream[i].vertex.argb==0xff123456 && stream[i].vertex.z>0);
    }
    assert(stream[1].vertex.x>stream[3].vertex.x); /* Strip starts1,2,0,3. */
    assert(stream[1].vertex.y>stream[2].vertex.y);
    state.count=0;positions.radius[0]=0.6f+176.0f/280.0f;writes=0;
    assert(nm_name_render_enqueue(&state,&cursor,&math,center,0));callback(NULL);
    assert(!failed && writes>1 && stream[writes-1].vertex.flags==PVR_CMD_VERTEX_EOL);
    for(unsigned i=1;i<writes;++i) assert(stream[i].vertex.y>=0 && stream[i].vertex.y<=480);
    positions.radius[0]=0;face.count=3;writes=0;
    assert(nm_name_render_enqueue(&state,&cursor,&math,center,0));callback(NULL);
    assert(writes==1+31*3 && !failed);
    face.count=4;
    state.count=1;state.name[0]='A';writes=0;
    assert(!nm_name_render_enqueue_exit(&state,&math,center,7,21));
    assert(nm_name_render_enqueue_exit(&state,&math,center,5,1));
    assert(!writes && !nm_name_render_release());callback(NULL);
    assert(!failed && writes==1+30*4); /* Ring + initials, no marker/preview. */
    writes=0;
    assert(nm_name_render_enqueue_exit(&state,&math,center,7,10));callback(NULL);
    assert(!failed && writes==1+30*4);
    state.count=0;cursor.radius=NAN;
    assert(nm_name_render_enqueue(&state,&cursor,&math,center,0));callback(NULL);
    assert(!nm_name_render_enqueue(&state,&cursor,&math,center,0));
    assert(nm_name_render_release());
    assert(!nm_name_render_enqueue(&state,&cursor,&math,center,0));
    assert(compiles==1);
    submitted_vertices=(512u*1024u-64u*1024u)/32u-4u;
    assert(!reserve_strip(4));
    assert(reserve_strip(3));
    assert(!reserve_strip(1));
    return 0;
}
'''
    source=tmp/'check.c';source.write_text(harness)
    binary=tmp/'check'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-fsanitize=address,undefined',
        '-I',str(tmp),'-I',str(root/'include'),'-I',str(root/'code'),str(source),
        str(root/'code/geometry.c'),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: actual name callback, deferred SQ, one cached header, quad strips, snapshots, failure/release')
