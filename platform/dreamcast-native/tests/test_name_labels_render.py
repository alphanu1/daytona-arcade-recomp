"""Execute production name UI loading/queue/render/release with a fake PVR."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    tmp=Path(directory)
    for path in ('enDjinn/enj_enDjinn.h','sh4zam/shz_sh4zam.h'):
        p=tmp/path;p.parent.mkdir(exist_ok=True);p.write_text('/* Mock backend in harness. */\n')
    harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdalign.h>
#define ENJ_XSCALE 1
#define PVR_LIST_PT_POLY 4
#define PVR_CULLING_NONE 0
#define PVR_DEPTHCMP_ALWAYS 7
#define PVR_DEPTHCMP_GEQUAL 6
#define PVR_DEPTHWRITE_DISABLE 1
#define PVR_FILTER_NONE 0
#define PVR_PAL_ARGB8888 3
#define PVR_TXRFMT_PAL4BPP 0x28000000u
#define PVR_TXRFMT_4BPP_PAL(bank) ((bank)<<21)
#define PVR_CMD_VERTEX 0xe0000000u
#define PVR_CMD_VERTEX_EOL 0xf0000000u
typedef struct { unsigned pvrformat,width,height;void *ptr; } enj_texture_info_t;
typedef struct { uint32_t cmd,comparison,padding[6]; } pvr_poly_hdr_t;
typedef struct { struct { int culling; } gen;struct { int comparison,write; } depth;unsigned texture; } pvr_poly_cxt_t;
typedef struct { uint32_t flags;float x,y,z,u,v;uint32_t argb,oargb; } pvr_vertex_t;
typedef struct { float x,y; } shz_vec2_t;
static shz_vec2_t shz_vec2_init(float x,float y) { return (shz_vec2_t){x,y}; }
static shz_vec2_t shz_vec2_mul(shz_vec2_t a,shz_vec2_t b) { return shz_vec2_init(a.x*b.x,a.y*b.y); }
static float shz_divf_fsrra(float a,float b) { return a/b; }
static unsigned allocations,frees,writes,compiles,queues;
static int palette_ok=1;
static int pvr_wait_ready(void) { return 0; }
static int pvr_wait_render_done(void) { return 0; }
static unsigned pvr_mem_available(void) { return 1024*1024; }
static int enj_texture_load_file(const char *path,enj_texture_info_t *t) {
    unsigned index=99;const char *name=strstr(path,"label-");assert(name);
    assert(sscanf(name,"label-%u.dt",&index)==1 && index<37);
    const unsigned relative=index-1;
    *t=(enj_texture_info_t){PVR_TXRFMT_PAL4BPP,index ? (relative>=10 && relative<22 ? 8u : 16u) : 8u,
        index ? (relative<10 ? 32u : relative<22 ? 16u : 32u) : 8u,(void *)(uintptr_t)(index+1)};
    ++allocations;return 1;
}
static void enj_texture_unload(enj_texture_info_t *t) { assert(t->ptr);t->ptr=NULL;++frees; }
/* render/materials/palette_ram.c: the program's palette format is its business. */
int nm_palette_ram_write_dpal_file(const char *path,unsigned offset,const char *owner) {
    assert(strstr(path,"labels.dp") && offset==960 && owner);return palette_ok;
}
static void pvr_poly_cxt_txr(pvr_poly_cxt_t *c,int list,unsigned fmt,unsigned w,unsigned h,void *ptr,int filter) {
    assert(list==4 && ((w==8 && h==8) || (w==8 && h==16) || (w==16 && h==32)) && ptr && !filter && fmt);
    memset(c,0,sizeof(*c));c->texture=(unsigned)(uintptr_t)ptr;
}
static void pvr_poly_compile(pvr_poly_hdr_t *h,const pvr_poly_cxt_t *c) {
    assert(c->depth.write==1);h->cmd=0x80840000;h->comparison=c->depth.comparison;h->padding[0]=c->texture;++compiles;
}
static union { pvr_vertex_t vertex;pvr_poly_hdr_t header; } stream[128];
static void *pvr_dr_target(void) { assert(writes<128);return stream+writes; }
static void pvr_dr_commit(void *p) { assert(p==stream+writes);++writes; }
static void (*callback)(void *);
static void enj_render_list_add(int list,void (*fn)(void *),void *data) { assert(list==4 && !data);callback=fn;++queues; }
#include "render/ui/name_labels.c"
int main(void) {
    nm_name_label_part_t parts[37]={{60,16,24,8,8,8,8,0}};
    for(unsigned i=1;i<37;++i) {
        const unsigned glyph=i-1;
        parts[i]=(nm_name_label_part_t){60,0,0,glyph>=10 && glyph<22 ? 8 : 16,
            glyph<10 ? 24 : glyph<22 ? 16 : 32,glyph>=10 && glyph<22 ? 8 : 16,
            glyph<10 ? 32 : glyph<22 ? 16 : 32,1};
    }
    parts[1]=(nm_name_label_part_t){60,2,1,14,23,16,32,1}; /* Cropped zero glyph. */
    nm_name_label_part_t *p=parts;
    assert(!nm_name_labels_load("/labels/",p,33,1));
    assert(!allocations);
    for(unsigned foreground=0;foreground<2;++foreground) {
        p->foreground=foreground;writes=0;
        assert(nm_name_labels_load("/labels/",p,37,1));
        assert(nm_name_labels_enqueue(-1) && !writes);
        assert(!nm_name_labels_enqueue(-1) && !nm_name_labels_unload());
        callback(NULL);
        assert(writes==5 && stream[0].header.comparison==(foreground ? 7u : 6u));
        assert(stream[4].vertex.flags==PVR_CMD_VERTEX_EOL);
        assert(stream[1].vertex.z==(foreground ? 1.0f : 0.000001f));
        assert(stream[1].vertex.u==0 && stream[4].vertex.u==1);
        assert(stream[1].vertex.v==0 && stream[4].vertex.v==1);
        assert(!nm_name_labels_enqueue(21) && !nm_name_labels_enqueue(-2));
        for(int seconds=0;seconds<=20;++seconds) {
            writes=0;assert(nm_name_labels_enqueue(seconds));assert(!writes);callback(NULL);
            const unsigned units=seconds<10 ? 5 : 10;
            assert(writes==units+5);
            if(seconds>=10) {
                assert(stream[5].header.padding[0]==(unsigned)(2+seconds/10));
                assert(stream[6].vertex.x==432.0f*(640.0f/496.0f));
            }
            assert(stream[units].header.padding[0]==(unsigned)(2+seconds%10));
            const int cropped=seconds%10==0;
            assert(stream[units+1].vertex.x==(448.0f+(cropped ? 2 : 0))*(640.0f/496.0f));
            assert(stream[units+1].vertex.y==(40.0f+(cropped ? 1 : 0))*1.25f);
            assert(stream[units+4].vertex.v==(cropped ? 23.0f : 24.0f)/32.0f);
        }
        writes=0;assert(nm_name_labels_record(12,6400,3200,1));
        assert(nm_name_labels_enqueue(-1));callback(NULL);
        assert(writes>5 && stream[5].header.padding[0]==25); /* static + rank tens part24. */
        assert(nm_name_labels_unload());
    }
    palette_ok=0;assert(!nm_name_labels_load("/labels/",p,37,1));
    assert(allocations==frees && !nm_name_labels_enqueue(-1));
    p->texture_width=16;assert(!nm_name_labels_load("/labels/",p,37,1));
    assert(allocations==frees && queues==46 && compiles==111);
    return 0;
}
'''
    source=tmp/'check.c';source.write_text(harness)
    binary=tmp/'check'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-fsanitize=address,undefined',
        '-I',str(tmp),'-I',str(root/'include'),'-I',str(root/'code'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: name UI background/foreground depth, deferred SQ, UVs, queue guards and failed-load cleanup')
