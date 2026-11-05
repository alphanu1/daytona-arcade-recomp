/* The frame-counter animations' frames as the renderer decodes them (include/clock_actors.h: nm_clock_point,
 * nm_clock_normal, the state and u, v tables, as model_renderer.c nm_render_world_clock_frame writes them) against
 * every frame of the original's lists (tools/prepare_world_objects.py clock --expected: the export's positions,
 * normals, texture headers, lumas and u, v in the world pack's order).
 *
 * usage: test_clock WORLD.dnm ACTORS.dna EXPECTED.ckx
 *
 * Checked on every frame of every group: each vertex record's position within the group's bound (the int16 step;
 * plus the interpolation error 0.068 for a group stored every 8 frames); its u, v equal; each face's texture header
 * and lumas equal (a face of the state table its state, any other the pack's own material and baked lumas; a PLACED
 * group's header only, its lumas are evaluated at run time); the
 * normal of a face whose side a frame may test within 2e-4 (and the pack's own where the file stores none). Prints
 * the largest errors per group. Exits 1 on a difference, 77 without the files (ROM output, never committed). */
#include "clock_actors.h"
#include "meshpack.h"
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

int main(int argc,char **argv) {
    if(argc!=4) { fputs("usage: test_clock WORLD.dnm ACTORS.dna EXPECTED.ckx\n",stderr);return 2; }
    nm_pack_t pack={0};
    size_t dna_size=0,expected_size=0;
    void *dna=read_file(argv[2],&dna_size);
    uint8_t *expected=read_file(argv[3],&expected_size);
    if(!dna || !expected || !nm_pack_load(&pack,argv[1])) { puts("SKIP: files missing");return 77; }
    if(!nm_clock_validate(dna,dna_size,pack.meshes,pack.header->meshes)) { puts("FAIL: the DNA1 file does not fit the pack");return 1; }
    const nm_clock_header_t *h=dna;
    if(expected_size<8 || memcmp(expected,"CKX1",4) || *(uint32_t *)(expected+4)!=h->groups) { puts("FAIL: expected file");return 1; }
    size_t at=8;
    int failed=0;
    for(unsigned i=0;i<h->groups;++i) {
        const nm_clock_group_t *g=nm_clock_groups(dna)+i;
        const uint32_t *counts=(const uint32_t *)(expected+at);at+=12;
        if(counts[0]!=g->frames || counts[1]!=g->vertices || counts[2]!=g->faces) { printf("FAIL: group %u counts\n",i);return 1; }
        const nm_mesh_t *mesh=pack.meshes+g->mesh;
        const nm_vertex_t *vertices=pack.vertices+mesh->vertex_first;
        const nm_face_t *faces=pack.faces+mesh->face_first;
        const uint16_t *vertex_point=nm_clock_table(dna,g,NM_CLOCK_VERTEX_POINT);
        const int16_t *positions=nm_clock_table(dna,g,NM_CLOCK_POSITIONS);
        const nm_clock_state_t *states=nm_clock_table(dna,g,NM_CLOCK_STATES);
        const uint16_t *state_faces=nm_clock_table(dna,g,NM_CLOCK_STATE_FACES),*face_states=nm_clock_table(dna,g,NM_CLOCK_FACE_STATES);
        const uint16_t *normal_faces=nm_clock_table(dna,g,NM_CLOCK_NORMAL_FACES);
        const int16_t *normals=nm_clock_table(dna,g,NM_CLOCK_NORMALS);
        const uint16_t *uv_vertices=nm_clock_table(dna,g,NM_CLOCK_UV_VERTICES),*uv_sets=nm_clock_table(dna,g,NM_CLOCK_UV_SETS);
        const uint16_t *uv_frames=nm_clock_table(dna,g,NM_CLOCK_UV_FRAMES);
        float step=0;
        for(unsigned a=0;a<3;++a) if(g->step[a]>step) step=g->step[a];
        const float bound=(g->rate>1 ? 0.068f : 0.0f)+step;
        float worst_position=0,worst_normal=0;
        unsigned state_errors=0,uv_errors=0,normal_errors=0,position_errors=0,sided_static=0;
        for(unsigned k=0;k<g->frames;++k) {
            const float *want_position=(const float *)(expected+at);at+=12u*g->vertices;
            const uint16_t *want_uv=(const uint16_t *)(expected+at);at+=(4u*g->vertices+3u)&~3u;
            const float *want_normal=(const float *)(expected+at);at+=12u*g->faces;
            const nm_clock_state_t *want_state=(const nm_clock_state_t *)(expected+at);at+=12u*g->faces;
            unsigned a=0,b=0;float t=0;
            if(g->rate) nm_clock_between(g,k,&a,&b,&t);
            for(unsigned v=0;v<g->vertices;++v) {
                float p[3];
                if(vertex_point[v]==NM_CLOCK_STILL) memcpy(p,vertices[v].position,sizeof(p));
                else {
                    nm_clock_point(g,positions+3u*g->points*a,positions+3u*g->points*b,t,vertex_point[v],p);
                    if(g->rate==1) { /* what model_renderer.c writes for rate 1 */
                        float stored[3];
                        nm_clock_point_stored(g,positions+3u*g->points*a,vertex_point[v],stored);
                        if(memcmp(stored,p,sizeof(p))) ++position_errors;
                    }
                }
                for(unsigned c=0;c<3;++c) {
                    const float e=fabsf(p[c]-want_position[3u*v+c]);
                    /* A still point keeps the base position, which may lie float32 steps apart in x (clock_base). */
                    if(e>worst_position) worst_position=e;
                    if(!(e<=bound+1e-4f)) ++position_errors;
                }
                uint16_t uv[2]={vertices[v].u,vertices[v].v};
                for(unsigned s=0;s<g->uv_vertices;++s) if(uv_vertices[s]==v) {
                    uv[0]=uv_sets[2u*(g->uv_vertices*uv_frames[k]+s)];uv[1]=uv_sets[2u*(g->uv_vertices*uv_frames[k]+s)+1u];
                }
                if(uv[0]!=want_uv[2u*v] || uv[1]!=want_uv[2u*v+1u]) ++uv_errors;
            }
            for(unsigned f=0;f<g->faces;++f) {
                nm_clock_state_t got={{0},{faces[f].reserved,(faces[f].attribute&(1u<<17)) ? faces[f].padding : 0u},0};
                memcpy(got.header,pack.materials[faces[f].material].header,sizeof(got.header));
                for(unsigned j=0;j<g->state_faces;++j) if(state_faces[j]==f) got=states[face_states[k*g->state_faces+j]];
                /* A PLACED group's lumas are the renderer's (its placement's): the header only. */
                if(g->flags&NM_CLOCK_GROUP_PLACED ? memcmp(got.header,want_state[f].header,sizeof(got.header)) :
                   memcmp(&got,want_state+f,sizeof(got))) ++state_errors;
                const int sided=!(faces[f].attribute&(1u<<17)) || want_state[f].luma[0]!=want_state[f].luma[1];
                int stored=0;
                for(unsigned j=0;j<g->normal_faces;++j) if(normal_faces[j]==f) {
                    stored=1;
                    for(unsigned c=0;c<3;++c) {
                        const float n=nm_clock_normal(normals[4u*(g->normal_faces*a+j)+c],normals[4u*(g->normal_faces*b+j)+c],t);
                        if(g->rate==1) {
                            const float stored=nm_clock_normal_stored(normals[4u*(g->normal_faces*a+j)+c]);
                            if(memcmp(&stored,&n,sizeof(n))) ++normal_errors;
                        }
                        const float e=fabsf(n-want_normal[3u*f+c]);
                        if(e>worst_normal) worst_normal=e;
                        if(!(e<=(g->rate>1 ? 0.05f : 2e-4f))) ++normal_errors;
                    }
                }
                if(!stored && sided) {
                    ++sided_static;
                    if(memcmp(faces[f].normal,want_normal+3u*f,sizeof(faces[f].normal))) ++normal_errors;
                }
            }
        }
        printf("group %u mesh %u: %u frames, rate %u, %u points; position error %.5f (bound %.5f), normal error %.5f; "
            "errors: position %u, u,v %u, state %u, normal %u (%u side-tested faces with a still normal)\n",
            i,(unsigned)g->mesh,(unsigned)g->frames,(unsigned)g->rate,(unsigned)g->points,(double)worst_position,
            (double)bound,(double)worst_normal,position_errors,uv_errors,state_errors,normal_errors,sided_static);
        failed|=position_errors || uv_errors || state_errors || normal_errors;
    }
    if(at!=expected_size) { puts("FAIL: the expected file has more data");failed=1; }
    puts(failed ? "FAIL: clock frames" : "PASS: clock frames, every frame of every group");
    nm_pack_unload(&pack);
    return failed;
}
