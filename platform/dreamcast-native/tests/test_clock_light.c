/* The lumas of the PLACED actor groups (include/render/models/clock_light.h, what model_renderer.c
 * nm_render_world_clock_light bakes) against the per-face reference: lighting.h's evaluation of every face for its
 * placement, the code the renderer ran before the key and the one-sided shortcut.
 *
 * usage: test_clock_light WORLD.dnm ACTORS.dna
 *
 * For every PLACED group, every stored frame (the normals of that frame written as nm_render_world_clock_frame
 * writes them) and 64 placements (rotations about the three axes, some with a scale, each with its own view): the
 * front and back luma of every face equal the reference's; a placement's key equals its own and, after a second
 * placement, differs when a result differs. Also: two placements with equal keys give equal lumas, so the renderer
 * may keep the lumas of the last key. Exits 1 on a difference, 77 without the files (ROM output, never committed). */
#include "clock_actors.h"
#include "meshpack.h"
#include "render/models/clock_light.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *read_file(const char *path,size_t *size) {
    FILE *f=fopen(path,"rb");
    if(!f) return NULL;
    fseek(f,0,SEEK_END);*size=(size_t)ftell(f);fseek(f,0,SEEK_SET);
    void *data=aligned_alloc(32,(*size+31u)&~(size_t)31u);
    if(data && fread(data,1,*size,f)!=*size) { free(data);data=NULL; }
    fclose(f);
    return data;
}
static const float light[3]={-0.45f,-0.89f,-0.45f};   /* lighting.h nm_race_world_light */

/* The reference: model_renderer.c nm_render_world_clock_light as it evaluated every face (host arithmetic). */
static void reference(const nm_face_t *faces,unsigned count,const nm_material_t *materials,unsigned mode,
        const float model[12],const float placed[12],const float view[12],const uint16_t *rows,const uint16_t *face_row,
        uint16_t (*out)[2]) {
    const float *l=light;
    float lm[3],az[3];
    for(unsigned c=0;c<3;++c) lm[c]=model[3*c]*l[0]+model[3*c+1]*l[1]+model[3*c+2]*l[2];
    az[0]=placed[2]; az[1]=placed[5]; az[2]=placed[8];
    const float light_z=view[2]*l[0]+view[5]*l[1]+view[8]*l[2];
    for(unsigned f=0;f<count;++f) {
        const nm_face_t *face=faces+f;
        const nm_material_t *material=materials+face->material;
        const float *n=face->normal;
        const float dotl=n[0]*lm[0]+n[1]*lm[1]+n[2]*lm[2];
        float specular=0;
        if((mode&1u) && material->specular_control) {
            const float z=n[0]*az[0]+n[1]*az[1]+n[2]*az[2];
            const unsigned control=material->specular_control;
            float term=(2*dotl)*z-light_z;
            if(term<0) term=0;
            if(control>>1) term*=term;
            if(control>>2) term*=term;
            if((control+1)>>3) term*=term;
            specular=term*material->specular_scale;
        }
        const uint16_t *row=rows+face_row[f]*256u;
        uint16_t side[2];
        for(unsigned rear=0;rear<2;++rear) {
            const float shown=rear ? -dotl : dotl;
            float luminance=(shown>0 ? shown : 0.0f)*material->diffuse+material->ambient+specular;
            if(luminance<0) luminance=0;
            if(luminance>255) luminance=255;
            side[rear]=row[(unsigned)(int)luminance];
        }
        out[f][0]=side[0]; out[f][1]=(face->attribute&(1u<<17)) ? side[1] : (uint16_t)UINT16_MAX;
    }
}
static uint32_t seed=0x2545f491u;
static float unit(void) { seed=seed*1664525u+1013904223u; return (float)(seed>>8)*(1.0f/16777216.0f); }
/* The TGP's rotations of a column-major 3x4 (practice.c practice_cone_rotate). */
static void rotate(float m[12],unsigned axis,float turn) {
    const float c=cosf(turn),sn=sinf(turn);
    float *const a=m+(axis==0 ? 3 : 0),*const b=m+(axis==2 ? 3 : 6);
    for(unsigned r=0;r<3;++r) {
        const float u=a[r],v=b[r];
        if(axis==1) { a[r]=u*c+v*sn; b[r]=v*c-u*sn; }
        else { a[r]=u*c-v*sn; b[r]=u*sn+v*c; }
    }
}
static void random_pose(float m[12],int scaled,float reach) {
    static const float identity[12]={1,0,0, 0,1,0, 0,0,1, 0,0,0};
    memcpy(m,identity,sizeof(identity));
    rotate(m,1,unit()*6.2831853f); rotate(m,0,unit()*6.2831853f); rotate(m,2,unit()*6.2831853f);
    if(scaled) { const float s=0.5f+unit()*2.0f,sz=0.69f; for(unsigned r=0;r<3;++r) { m[r]*=s; m[6+r]*=sz; } }
    for(unsigned r=0;r<3;++r) m[9+r]=(unit()-0.5f)*reach;
}
static void compose(const float a[12],const float b[12],float out[12]) {
    for(unsigned c=0;c<4;++c) for(unsigned r=0;r<3;++r)
        out[c*3+r]=a[r]*b[c*3]+a[3+r]*b[c*3+1]+a[6+r]*b[c*3+2]+(c==3 ? a[9+r] : 0.0f);
}

int main(int argc,char **argv) {
    if(argc!=3) { fputs("usage: test_clock_light WORLD.dnm ACTORS.dna\n",stderr);return 2; }
    nm_pack_t pack={0};
    size_t dna_size=0;
    void *dna=read_file(argv[2],&dna_size);
    if(!dna || !nm_pack_load(&pack,argv[1])) { puts("SKIP: files missing");return 77; }
    if(!nm_clock_validate(dna,dna_size,pack.meshes,pack.header->meshes)) { puts("FAIL: the DNA1 file does not fit the pack");return 1; }
    const nm_clock_header_t *h=dna;
    /* A row per material: the luma index itself, tagged by the row (the bindings of the renderer's rows are a
     * lookup of the same index). */
    uint16_t *row_of=calloc(pack.header->materials,sizeof(*row_of));
    unsigned rows=0;
    for(unsigned m=0;m<pack.header->materials;++m) row_of[m]=(uint16_t)rows++;
    uint16_t *table=malloc(rows*256u*sizeof(*table));
    for(unsigned r=0;r<rows;++r) for(unsigned luma=0;luma<256;++luma) table[r*256u+luma]=(uint16_t)((r*256u+luma)&0xffffu);
    unsigned groups=0,placements=0,faces_checked=0,specular_faces=0,two_sided=0,keys_equal=0,keys_differ=0;
    int failed=0;
    for(unsigned i=0;i<h->groups && !failed;++i) {
        const nm_clock_group_t *g=nm_clock_groups(dna)+i;
        if(!(g->flags&NM_CLOCK_GROUP_PLACED)) continue;
        ++groups;
        const nm_mesh_t *mesh=pack.meshes+g->mesh;
        nm_face_t *faces=(nm_face_t *)pack.faces+mesh->face_first;
        uint16_t *face_row=malloc(mesh->face_count*sizeof(*face_row));
        int specular=0;
        for(unsigned f=0;f<mesh->face_count;++f) {
            face_row[f]=row_of[faces[f].material];
            if((mesh->mode&1u) && pack.materials[faces[f].material].specular_control) { specular=1; ++specular_faces; }
            if(faces[f].attribute&(1u<<17)) ++two_sided;
        }
        uint16_t (*expected)[2]=malloc(mesh->face_count*sizeof(*expected)),(*actual)[2]=malloc(mesh->face_count*sizeof(*actual));
        uint16_t (*previous)[2]=malloc(mesh->face_count*sizeof(*previous));
        const uint16_t *normal_faces=nm_clock_table(dna,g,NM_CLOCK_NORMAL_FACES);
        const int16_t *normals=nm_clock_table(dna,g,NM_CLOCK_NORMALS);
        for(unsigned frame=0;frame<g->frames && !failed;++frame) {
            if(g->rate) {
                unsigned a,b; float t;
                nm_clock_between(g,frame,&a,&b,&t);
                const int16_t *na=normals+4u*g->normal_faces*a,*nb=normals+4u*g->normal_faces*b;
                for(unsigned j=0;j<g->normal_faces;++j)
                    for(unsigned axis=0;axis<3;++axis)
                        faces[normal_faces[j]].normal[axis]=nm_clock_normal(na[4u*j+axis],nb[4u*j+axis],t);
            }
            nm_clock_light_key_t last_key; int have_last=0;
            for(unsigned k=0;k<64 && !failed;++k) {
                float model[12],view[12],placed[12];
                random_pose(model,k&4,200.0f);
                random_pose(view,0,60.0f);
                compose(view,model,placed);
                reference(faces,mesh->face_count,pack.materials,mesh->mode,model,placed,view,table,face_row,expected);
                nm_clock_light_key_t key;
                nm_clock_light_key(&key,model,placed,view,light,specular);
                nm_clock_light_faces(faces,mesh->face_count,pack.materials,mesh->mode&1u,&key,table,face_row,actual);
                ++placements;
                for(unsigned f=0;f<mesh->face_count && !failed;++f) {
                    ++faces_checked;
                    if(expected[f][0]!=actual[f][0] || expected[f][1]!=actual[f][1]) {
                        printf("FAIL: group %u frame %u placement %u face %u: reference lumas %u/%u, clock_light.h %u/%u\n",
                            i,frame,k,f,expected[f][0],expected[f][1],actual[f][0],actual[f][1]);
                        failed=1;
                    }
                }
                /* The same placement again: the key equals, so would its lumas. */
                nm_clock_light_key_t again;
                nm_clock_light_key(&again,model,placed,view,light,specular);
                if(memcmp(&key,&again,sizeof(key))) { printf("FAIL: group %u: a placement's key differs from its own\n",i); failed=1; }
                /* A different view of the same model: the key of a group without a specular material equals, and
                 * its lumas do; with one, the lumas may differ and then the key does. */
                float view2[12],placed2[12];
                random_pose(view2,0,60.0f);
                compose(view2,model,placed2);
                reference(faces,mesh->face_count,pack.materials,mesh->mode,model,placed2,view2,table,face_row,previous);
                nm_clock_light_key(&again,model,placed2,view2,light,specular);
                const int same_key=!memcmp(&key,&again,sizeof(key));
                const int same_lumas=!memcmp(expected,previous,mesh->face_count*sizeof(*expected));
                if(same_key) ++keys_equal; else ++keys_differ;
                if(same_key && !same_lumas) { printf("FAIL: group %u: equal keys, different lumas (view)\n",i); failed=1; }
                if(!specular && !same_key) { printf("FAIL: group %u: no specular material yet the key reads the view\n",i); failed=1; }
                if(have_last && !memcmp(&key,&last_key,sizeof(key))) { printf("FAIL: group %u: two placements with one key\n",i); failed=1; }
                last_key=key; have_last=1;
            }
        }
        free(face_row); free(expected); free(actual); free(previous);
    }
    free(row_of); free(table);
    if(failed) return 1;
    printf("PASS: clock light, %u placed groups, %u placements, %u faces (%u with a specular material, %u two-sided), "
        "keys of another view equal %u / differ %u\n",groups,placements,faces_checked,specular_faces,two_sided,keys_equal,keys_differ);
    return 0;
}
