"""Host selector check; optional argv are actual beginner DNM packs."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'code/render/scene/track_surface.c').read_text()
headers = re.findall(r'\{(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\}', source.split('int nm_track_surface_select', 1)[0])
assert len(headers) == 40 and len(set(headers)) == 40
# Independently audited export IDs, not inferred from production selection.
ids = [0,1,2,3,5,6,7,30,38,39,40,41,63,83,84,85,86,87,88,124,125,126,
       127,153,175,193,196,205,211,215,216,217,218,225,228,229,232,233,234,241]
code = r'''
#include "render/scene/track_surface.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static const unsigned ids[]={IDS};
static const uint16_t headers[][4]={HEADERS};
static int chosen(unsigned material) {
    for(unsigned i=0;i<sizeof(ids)/sizeof(*ids);++i) if(ids[i]==material) return 1;
    return 0;
}
static void verify(nm_pack_t *p,int real) {
    nm_pack_t pointers=*p;
    unsigned size=p->header->size;
    unsigned char *copy=malloc(size);assert(copy);memcpy(copy,p->data,size);
    nm_mesh_t *meshes=malloc(p->header->meshes*sizeof(*meshes));assert(meshes);
    memcpy(meshes,p->meshes,p->header->meshes*sizeof(*meshes));
    unsigned total=0,empty=0;
    nm_face_t *original=malloc(p->header->faces*sizeof(*original));assert(original);
    memcpy(original,p->faces,p->header->faces*sizeof(*original));
    /* Strip records (DNM5/6) survive the selection: independent expectation
     * from the original face table, then restored like meshes and faces. */
    unsigned strip_total=0;
    if(p->strips) for(unsigned m=0;m<p->header->meshes;++m) strip_total+=meshes[m].padding[3];
    nm_strip_t *strips=malloc((strip_total+1)*sizeof(*strips));assert(strips);
    const nm_strip_t *strip_base=p->strips ? p->strips+meshes[0].padding[2] : NULL;
    if(strip_total) memcpy(strips,strip_base,strip_total*sizeof(*strips));
    assert(nm_track_surface_select(p));
    /* Tables stay where they are, strips included. */
    assert(!memcmp(&pointers,p,sizeof(*p)));
    unsigned strips_kept=0;
    for(unsigned m=0;m<p->header->meshes;++m) {
        nm_mesh_t expected=meshes[m];unsigned count=0,kept=0;
        const nm_strip_t *source=p->strips ? strips+(meshes[m].padding[2]-meshes[0].padding[2]) : NULL;
        for(unsigned f=0;f<expected.face_count;++f) {
            nm_face_t *face=original+expected.face_first+f;
            int keep=real?chosen(face->material):face->material<40;
            for(unsigned s=0;source && s<meshes[m].padding[3];++s) if(source[s].face_first==expected.face_first+f) {
                /* One material per strip: all of its faces share this face's fate. */
                for(unsigned j=0;j<source[s].face_count;++j) assert(face[j].material==face->material);
                if(!keep) continue;
                nm_strip_t moved=source[s];moved.face_first=expected.face_first+count;
                assert(!memcmp(&moved,p->strips+expected.padding[2]+kept,sizeof(moved)));
                ++kept;
            }
            if(keep) {
                assert(!memcmp(face,p->faces+expected.face_first+count,sizeof(*face)));
                ++count;
            }
        }
        expected.face_count=count;
        if(p->strips) expected.padding[3]=kept;
        assert(!memcmp(&expected,p->meshes+m,sizeof(expected)));
        total+=count;empty+=count==0;strips_kept+=kept;
        memcpy((unsigned char *)p->data+((const unsigned char *)(p->meshes+m)-(unsigned char *)p->data),meshes+m,sizeof(expected));
        memcpy((unsigned char *)p->data+((const unsigned char *)(p->faces+expected.face_first)-(unsigned char *)p->data),original+expected.face_first,meshes[m].face_count*sizeof(*original));
    }
    if(strip_total) memcpy((unsigned char *)p->data+((const unsigned char *)strip_base-(unsigned char *)p->data),strips,strip_total*sizeof(*strips));
    free(strips);
    assert(total==(real?1373:40));assert(empty==(real?4:1));
    if(!real) assert(strips_kept==(pointers.strips ? 2u : 0u));
    /* Restoring only mutable mesh/face spans leaves all other bytes identical. */
    assert(!memcmp(copy,p->data,size));
    free(original);free(meshes);free(copy);
}
int main(int argc,char **argv) {
    struct fixture {nm_header_t h;nm_mesh_t meshes[2];nm_face_t faces[81];nm_material_t materials[41];nm_strip_t strips[4];} data={0};
    data.h.size=sizeof(data);data.h.meshes=2;data.h.faces=81;data.h.materials=41;
    data.meshes[0].face_count=80;data.meshes[1].face_first=80;data.meshes[1].face_count=1;
    for(unsigned i=0;i<40;++i) {
        memcpy(data.materials[i].header,headers[i],sizeof(headers[i]));
        data.faces[2*i].material=40;data.faces[2*i+1].material=i;
        data.faces[2*i+1].attribute=i+7;data.faces[2*i+1].group=i+11;data.faces[2*i+1].layer=i&3;
    }
    data.faces[80].material=40;
    /* Mesh 0: a dropped strip, two kept ones (faces 3 and 79 move to 1 and
     * 39) and another dropped one; mesh 1: its only strip is dropped. The
     * selector reads face ranges only, so one-face fixtures suffice. */
    const unsigned strip_faces[4]={2,3,79,80};
    for(unsigned i=0;i<4;++i) data.strips[i]=(nm_strip_t){.face_first=strip_faces[i],.face_count=1,.vertex_count=4,.vertex_first=4*i};
    data.strips[0].face_first=2;data.meshes[0].padding[2]=0;data.meshes[0].padding[3]=3;
    data.meshes[1].padding[2]=3;data.meshes[1].padding[3]=1;
    nm_pack_t p={.data=&data,.header=&data.h,.meshes=data.meshes,.faces=data.faces,.materials=data.materials,
        .strips=data.strips,.strip_indices=(const uint16_t *)&data};
    struct fixture before;
    data.materials[0].header[0]^=1;before=data;
    assert(!nm_track_surface_select(&p));assert(!memcmp(&before,&data,sizeof(data)));
    data.materials[0].header[0]^=1;memcpy(data.materials[40].header,headers[0],sizeof(headers[0]));before=data;
    assert(!nm_track_surface_select(&p));assert(!memcmp(&before,&data,sizeof(data)));
    memset(data.materials[40].header,0,sizeof(headers[0]));verify(&p,0);
    /* Packs without strip records (DNM4) keep the mesh strip fields untouched. */
    p.strips=NULL;verify(&p,0);
    /* Scenery stages (2026-10-12). Without scenery classes: stage 0 is the
     * audited selection above, any other stage keeps the pack whole. */
    p.strips=data.strips;
    before=data;
    assert(nm_scenery_stage_select(&p,0));
    { struct fixture reduced=data; data=before; assert(nm_track_surface_select(&p));
      assert(!memcmp(&reduced,&data,sizeof(data))); data=before; }
    assert(nm_scenery_stage_select(&p,3) && !memcmp(&before,&data,sizeof(data)));
    assert(!nm_scenery_stage_select(&p,NM_SCENERY_STAGE_LAST+1) && !memcmp(&before,&data,sizeof(data)));
    assert(!nm_scenery_stage_select(NULL,0));
    /* With classes the material's own stage decides: material i is stage
     * i%5 here (the shared material 40 is stage 4), surface bit ignored.
     * No material of this fixture has the world objects' stage 5
     * (NM_SCENERY_STAGE_OBJECTS, the last): stage 5 keeps what stage 4 keeps. */
    data.h.magic=NM_PACK_MAGIC_LAYERED;data.h.padding[2]=1;data.h.padding[3]=NM_PACK_SCENERY_CLASSES;
    for(unsigned i=0;i<41;++i) data.materials[i].padding[0]=(i<40 ? i%5 : 4)|(i&1 ? NM_SCENERY_SURFACE : 0);
    before=data;
    for(unsigned stage=0;stage<=NM_SCENERY_STAGE_LAST;++stage) {
        assert(nm_scenery_stage_select(&p,stage));
        unsigned kept=0;
        for(unsigned m=0;m<2;++m) {
            /* Kept faces stay in pack order, each an unchanged copy. */
            unsigned at=0;
            for(unsigned f=0;f<before.meshes[m].face_count;++f) {
                const nm_face_t *face=before.faces+before.meshes[m].face_first+f;
                if((before.materials[face->material].padding[0]&NM_SCENERY_STAGE_MASK)>stage) continue;
                assert(!memcmp(face,data.faces+before.meshes[m].face_first+at,sizeof(*face)));
                ++at;
            }
            assert(data.meshes[m].face_count==at && data.meshes[m].face_first==before.meshes[m].face_first);
            kept+=at;
        }
        assert(kept==(stage>=4 ? 81u : 8u*(stage+1)));
        assert(!memcmp(before.materials,data.materials,sizeof(data.materials)) && !memcmp(&before.h,&data.h,sizeof(data.h)));
        data=before;
    }
    assert(!nm_track_surface_select(NULL));
    for(int i=1;i<argc;++i) {nm_pack_t actual={0};assert(nm_pack_load(&actual,argv[i]));verify(&actual,1);nm_pack_unload(&actual);}
    return 0;
}
'''.replace('IDS', ','.join(map(str, ids))).replace('HEADERS', ','.join('{'+','.join(h)+'}' for h in headers))
with tempfile.TemporaryDirectory(prefix='daytona-track-surface-') as temp:
    check = Path(temp) / 'check.c'
    check.write_text(code)
    binary = Path(temp) / 'check'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                    '-I'+str(root / 'include'), str(check), str(root / 'code/meshpack.c'),
                    str(root / 'code/render/scene/track_surface.c'), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary), *sys.argv[1:]], check=True)
print('PASS: selector failure atomicity, face order/bytes, retained strips moved with their faces, pointers and conservative metadata; scenery stages with and without classes; ASan/UBSan')

practice_source = (root / 'code/practice.c').read_text()
boot = practice_source.split('#ifdef DAYTONA_AUTOSTART\n', 1)[1].split('    printf("Native practice:', 1)[0]
boot = '#ifdef DAYTONA_AUTOSTART\n' + boot
finite = practice_source.split('static __attribute__((noinline)) int practice_attract_update(uint32_t elapsed_us) {\n', 1)[1].split('    uint32_t dt=elapsed_us;', 1)[0]
with tempfile.TemporaryDirectory(prefix='daytona-attract-dispatch-') as temp:
    check = Path(temp) / 'check.c'
    check.write_text('''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
static unsigned attract,race,title,released,shutdowns,loops,demos;
static int title_ok=1,demo_ok;
static struct {struct {unsigned ticks,object_clock;} intro;unsigned course;int failed;} practice;
/* The V.R. demo (practice_demo_start): 1 when it runs (its mode then owns the updater, its end the title), 0 in a
 * build that counts it. */
static int practice_demo_start(void) {++demos;return demo_ok;}
static const uint32_t attract_tick_limit=1922;
enum { NM_ATTRACT_DEMO_ITERATIONS=836 }; /* include/attract.h */
static unsigned nm_attract_next_course(unsigned course) { static const unsigned next[4]={2,0,1,3}; return next[course&3u]%4u; }
static unsigned practice_attract_course;
static uint32_t counter;
static void nm_frontend_counter_set(uint32_t value) {counter=value;}
#define nm_frontend_pool_from_intro(pool,intro,road) ((void)0) /* frontend.h: the pool, not this check's */
static int nm_practice_attract_start(void) {++attract;return 1;}
static int nm_attract_mode_start(void) {++loops;return nm_practice_attract_start();}
static int nm_practice_restart_from_title(void) {++race;return 1;}
static int nm_title_mode_start(void) {++title;return title_ok;}
static void nm_practice_render_release(void) {++released;}
static void enj_state_flag_shutdown(void *unused) {(void)unused;++shutdowns;}
static int boot(void) {const char *stage=NULL;
''' + boot + '''
    (void)stage;return 1;
done: (void)stage;return 0;
}
static int finite(void) {
''' + finite + '''
    return 1;
}
int main(void) {
    /* Keep all dispatch stubs referenced in every preprocessor variant. */
    (void)nm_practice_attract_start;(void)nm_practice_restart_from_title;(void)nm_attract_mode_start;
    assert(boot());
#if defined(DAYTONA_AUTOSTART) && DAYTONA_AUTOSTART == 2
    assert(attract==1 && !race && !title);
#elif defined(DAYTONA_AUTOSTART)
    assert(race==1 && !attract && !title);
#else
    assert(attract==1 && loops==1 && counter==1 && !race && !title); /* power-on: phase 2, the attract loop */
#endif
    title=0;
    const unsigned limit=DAYTONA_RACE_TICK_LIMIT ? DAYTONA_RACE_TICK_LIMIT : attract_tick_limit;
    practice.intro.ticks=limit-1;finite();assert(!released && !shutdowns && !title);
    practice.intro.ticks=limit;finite();assert(released==1);
    if(DAYTONA_RACE_TICK_LIMIT) {assert(shutdowns==1 && !title);}
    else {
        assert(!shutdowns && title==1 && counter==practice.intro.object_clock+limit+836u);
        assert(practice_attract_course==2); /* the demo's end steps the course: 0 -> 2 */
        practice.course=2;practice.intro.ticks=limit;finite();assert(practice_attract_course==1 && title==2);
        practice.course=1;finite();assert(practice_attract_course==0 && title==3);
        title_ok=0;finite();assert(released==4 && shutdowns==1 && title==4);
        demo_ok=1;title_ok=1;const unsigned before=demos;
        finite();assert(demos==before+1 && released==4 && title==4); /* the demo's mode follows, nothing released */
    }
}
''')
    binary = Path(temp) / 'check'
    for flags in ([], ['-DDAYTONA_AUTOSTART=1'], ['-DDAYTONA_AUTOSTART=2']):
        for limit in (0, 1922):
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', *flags, f'-DDAYTONA_RACE_TICK_LIMIT={limit}',
                            str(check), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
print('PASS: actual attract (phase 2)/race/attract boot dispatch and finite attract cleanup/title failure, the V.R. demo started or counted, the course cycle 0, 2, 1')
