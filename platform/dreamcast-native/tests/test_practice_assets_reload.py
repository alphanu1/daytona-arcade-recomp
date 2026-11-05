"""Actual asset load/release and pack ownership, with GPU/startup math stubbed."""
from pathlib import Path
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'code/render/scene/practice_assets.c').read_text()
# Since 17 October (courses) the reset is nm_practice_render_unload, which the load calls first.
functions = source[source.index('void nm_practice_render_unload('):]
reset = '''    /* Callers release queued rendering before replacing the game-owned CPU packs. */
    nm_practice_render_unload(scene);
'''


def fixture(path, world=False):
    # Minimal valid packs retain the two real intro-window source addresses.
    faces = 1 if world else 12
    meshes = 1 if world else 2
    vertices = meshes * 3
    data = b''
    for i in range(meshes):
        count = 1 if world else (7 if i == 0 else 5)
        data += struct.pack('<4I6f6I', i*3, 3, 0 if i == 0 else 7,
                            count, *([0.0]*6), 0, 0, int(world), 0, 0, 0)
    data += bytes(vertices*16)
    data += struct.pack('<5H2BI3fI', 0, 1, 2, 0, 0, 3, 0, 0x102, 0, 0, 0, 0)*faces
    data += bytes(32)
    for i in range(meshes):
        data += struct.pack('<4I12f5f6h4I', i, 0 if world else (0x1c8 if i == 0 else 0x241),
                            0, 0, *([0.0]*17), *([0]*10))
    if world:
        data += bytes(vertices*2)
        data += bytes((-len(data)) % 4)
        data += bytes(64)
    path.write_bytes(struct.pack('<8I4i4I', 0x344d4e44 if world else 0x314d4e44,
                                64+len(data), meshes, vertices, faces, 1, meshes,
                                int(world), *([0]*8)) + data)


code = r'''
#include "meshpack.h"
#include "course_assets.h"
#include "clock_actors.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *owned[8];
static unsigned allocations, frees, live;
static void *allocate(size_t alignment,size_t size) {
    void *p=aligned_alloc(alignment,(size+alignment-1)&~(alignment-1));assert(p);
    unsigned i=0;while(i<8 && owned[i]) ++i;assert(i<8);owned[i]=p;
    ++allocations;++live;return p;
}
static void dispose(void *p) {
    if(!p) return;
    unsigned i=0;while(i<8 && owned[i]!=p) ++i;assert(i<8);
    owned[i]=NULL;++frees;--live;free(p);
}
#define aligned_alloc allocate
#define memalign allocate
#define free dispose
#include "meshpack.c"
static const unsigned nm_intro_model_ids[]={IDS};
static const unsigned nm_intro_window_models[]={0,1};
static const float nm_race_world_light[3]={0};
static const int nm_practice_render_view=0;
typedef struct {struct {int culling;} gen;} pvr_poly_cxt_t;
enum {PVR_LIST_OP_POLY=0,PVR_CULLING_NONE=0,NM_SKY_TEST_HASH=42};
typedef struct {void *data;unsigned classes,faces;} nm_damage_bodies_t;
typedef struct {
    nm_pack_t model,world;
    nm_header_t intro_header,world_header;
    const nm_instance_t *car_sources,*world_sources;
    nm_instance_t car_instances[99],instance,*world_instances;
    nm_vertex_t *intro_windows[2],*window_vertices;
    unsigned intro_window_count[2],window_vertex_count;
    uint16_t intro_window_uv[2][72],window_base_uv[72];
    void *math,*points;
    struct {struct {struct {struct {float position[3];} ground;} boundary;unsigned scenery_cell;} car;} state;
    struct {unsigned profile_count;struct {struct {float xyz[3];} lanes[8];} *profiles;} road;
    struct {unsigned slot,instances,profiles;const struct {uint16_t first,count;} *cells;} scenery;
    unsigned world_object_first,world_object_count; /* world objects of the pack, behind the cells */
    nm_damage_bodies_t damage_bodies,hood_variants; /* car-damage.dnb, car-hoods.dnb: the fixtures have none */
    unsigned player_body_variant,player_hood_variant;
    uint8_t transmission; /* 501464: the manual car reserves a light class a face */
    uint8_t course; /* 501460 of the scene's world: the fixtures are course 0's */
    uint16_t window_materials[2]; /* the windows' tunnel materials: the fixtures have none */
    void *clock_data; unsigned clock_count; uint16_t clock_source[NM_CLOCK_GROUPS_MAX]; /* the fixtures have no animation */
    float view[12];int header;
} nm_practice_scene_t;
static unsigned gpu_live,releases;
static int fail_at;
static int gpu_start(void) {assert(!gpu_live);gpu_live=1;return 1;}
static void gpu_stop(void) {gpu_live=0;++releases;}
#define nm_view_valid(v) ((void)(v),1)
#define nm_practice_render_pose(s) ((void)(s),1)
#define nm_body_pose(a,b,c,d) ((void)(a),(void)(b),(void)(c),memset(d,0,48),1)
#define nm_pose_compose(a,b,c) ((void)(a),(void)(b),(void)(c),1)
#define nm_practice_render_matrix(m) ((void)(m))
#define nm_practice_render_check_transform(a,b,c) ((void)(a),(void)(b),(void)(c),1)
#define nm_practice_render_lighting(s) ((void)(s),1)
#define nm_practice_render_parts(s) ((void)(s),1)
#define nm_materials_load(p) ((void)(p),fail_at!=1)
#define nm_pack_check_math(p) ((void)(p),1)
#define nm_pack_check_lighting(p) ((void)(p),1)
#define nm_render_init(p) ((void)(p),gpu_start())
#define nm_track_surface_select(p) ((void)(p),1)
#define nm_scenery_stage_select(p,stage) ((void)(p),1)
#define nm_practice_render_scenery(s) ((void)(s),1)
#define nm_pack_check_scenery_lighting(p) ((void)(p),1)
#define nm_sky_test_hash(p) ((void)(p),42u)
#define nm_sky_load(p) ((void)(p),1)
#define nm_world_materials_load(p) ((void)(p),fail_at!=2)
#define nm_render_world_attach(p) ((void)(p),1)
#define nm_render_world_clocks(d) ((void)(d),1)
#define nm_props_world_actors(d,c) ((void)(d),(void)(c))
#define nm_heap_alloc(owner,alignment,bytes) ((void)(owner),allocate((alignment) ? (alignment) : 8u,(bytes)))
#define nm_render_world_capacity(p,f) ((void)(p),(void)(f))
#define nm_render_world_streams(e) ((void)(e))
#define nm_finish_render_load(p) ((void)(p),1)
#define nm_result_materials_load(p) ((void)(p),1)
#define nm_result_render_load(p,m) ((void)(p),(void)(m),1)
#define nm_result_labels_load(p) ((void)(p),1)
#define pvr_poly_cxt_col(p,l) ((void)(p),(void)(l))
#define pvr_poly_compile(p,c) ((void)(p),(void)(c))
#define nm_result_labels_unload() ((void)0)
#define nm_result_render_unload() ((void)0)
#define nm_result_materials_unload() ((void)0)
#define nm_finish_render_unload() ((void)0)
#define nm_hud_render_unload() ((void)0)
#define nm_render_release() gpu_stop()
#define nm_world_materials_unload() ((void)0)
#define nm_sky_unload() ((void)0)
#define nm_materials_unload() ((void)0)
#define NM_DAMAGE_BODY_CLEAN 16
#define nm_damage_bodies_unload(b) ((void)(b))
#define nm_damage_bodies_load(b,p) ((void)(b),(void)(p),1)
#define NM_DAMAGE_HOOD_CLEAN 0
#define nm_damage_hoods_load(b,p) ((void)(b),(void)(p),1)
#define nm_render_car_light_classes_reserve(n) ((void)(n))
FUNCTIONS
static void borrowed(nm_practice_scene_t *s,int world) {
    assert(s->car_sources && s->model.instances==s->car_instances);
    assert(s->model.header==&s->intro_header);
    for(unsigned w=0;w<2;++w) {
        const nm_mesh_t *m=s->model.meshes+s->car_sources[w].mesh;
        assert(s->intro_windows[w]==s->model.vertices+m->vertex_first);
        assert(s->intro_window_count[w]==m->vertex_count);
        for(unsigned v=0;v<m->vertex_count;++v)
            assert(s->intro_windows[w][v].u==s->intro_window_uv[w][2*v]);
    }
    if(world) {
        assert(s->world_sources && s->world_instances);
        assert(s->world.instances==s->world_instances && s->world.header==&s->world_header);
        assert(s->world_sources[0].mesh<s->world.header->meshes);
    }
}
int main(int argc,char **argv) {
    assert(argc==2);
    static const struct {uint16_t first,count;} cells[256]; /* the fixture world's cells hold no instance */
    nm_practice_scene_t s={0};s.scenery.instances=WORLD_INSTANCES;s.scenery.cells=(const void *)cells;
    const char *stage=NULL;
    assert(nm_practice_render_load(&s,argv[1],&stage));borrowed(&s,1);assert(live==3);
    nm_practice_render_release();assert(!gpu_live && live==3);
    assert(nm_practice_render_load(&s,argv[1],&stage));borrowed(&s,1);assert(live==3 && frees==3);
    nm_practice_render_release();
    /* Failure before car load must clear every borrowed span and live allocation. */
    s.window_vertices=(nm_vertex_t *)s.model.vertices;s.window_vertex_count=1;
    assert(!nm_practice_render_load(&s,"/nonexistent-daytona-d43/",&stage));
    assert(!live && !s.car_sources && !s.world_sources && !s.world_instances);
    assert(!s.window_vertices && !s.window_vertex_count);
    for(unsigned w=0;w<2;++w) assert(!s.intro_windows[w] && !s.intro_window_count[w]);
    nm_practice_render_release();
    /* Car-only failure after source rebinding, then a successful retry. */
    fail_at=1;assert(!nm_practice_render_load(&s,argv[1],&stage));borrowed(&s,0);
    assert(live==1 && !s.world_sources && !s.world_instances);
    nm_practice_render_release();fail_at=0;
    assert(nm_practice_render_load(&s,argv[1],&stage));borrowed(&s,1);assert(live==3);
    nm_practice_render_release();
    /* Late partial failure owns both packs and live instances until retry/final exit. */
    fail_at=2;assert(!nm_practice_render_load(&s,argv[1],&stage));borrowed(&s,1);assert(live==3);
    nm_practice_render_release();fail_at=0;
    assert(nm_practice_render_load(&s,argv[1],&stage));borrowed(&s,1);
    nm_practice_render_release();nm_practice_render_release();
    nm_pack_unload(&s.model);free(s.world_instances);s.world_instances=NULL;nm_pack_unload(&s.world);
    nm_pack_unload(&s.model);free(s.world_instances);nm_pack_unload(&s.world);
    assert(!live && allocations==frees && releases==8);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='daytona-assets-reload-') as temp:
    directory = Path(temp)
    assets = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else directory
    if len(sys.argv) == 1:
        fixture(assets / 'car.dnm')
        fixture(assets / 'world.dnm', world=True)
    # Read only the catalog/count used by the startup stubs; actual C loader
    # independently validates all bytes and performs every owned allocation.
    packs = {}
    for name in ('car', 'world'):
        path = assets / (name + '.dnm')
        data = path.read_bytes()
        h = struct.unpack_from('<8I', data)
        alignment = 32 if h[0] & 0x80000000 else 1
        offset = 64
        for count, stride in zip(h[2:6], (64,16,32,32)):
            offset = (offset+alignment-1)//alignment*alignment + count*stride
        offset = (offset+alignment-1)//alignment*alignment
        packs[name] = (h[6], [struct.unpack_from('<I', data, offset+i*112+4)[0] for i in range(h[6])])
        print(f'{path}: sha256={hashlib.sha256(data).hexdigest()}', flush=True)
    harness = code.replace('IDS', ','.join(map(str,packs['car'][1])))
    harness = harness.replace('WORLD_INSTANCES', str(packs['world'][0]))
    compiler = os.environ.get('CC', 'cc')
    print(subprocess.check_output([compiler, '--version'], text=True).splitlines()[0], flush=True)
    for variant, body in [('fixed', functions), ('before', functions.replace(reset, '', 1))]:
        check = directory / (variant + '.c')
        check.write_text(harness.replace('FUNCTIONS', body))
        binary = directory / variant
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                        '-I'+str(root / 'include'), '-I'+str(root / 'code'),
                        str(check), '-lm', '-o', str(binary)], check=True)
        result = subprocess.run([str(binary), str(assets)+'/'], capture_output=True, text=True)
        if variant == 'fixed':
            assert result.returncode == 0, result.stdout + result.stderr
        else:
            assert result.returncode != 0 and 'nm_practice_render_load' in result.stderr, result.stderr
        print(f'{variant}: exit={result.returncode}', flush=True)
print('PASS: actual load/release/load, partial failure/retry, borrowed spans, single frees; ASan/UBSan; rejects pre-fix body')
