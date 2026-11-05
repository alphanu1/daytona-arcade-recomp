#include "render/scene/damage_bodies.h"
#include "heap.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nm_mem.h"

int nm_damage_variants_load(nm_damage_bodies_t *bodies, const char *path, const char *owner, unsigned count,
        unsigned clean) {
    nm_damage_bodies_unload(bodies);
    if(!count || clean>=count) return 0;
    FILE *file=fopen(path,"rb");
    if(!file) { printf("Native %s: %s absent; the player draws the clean model only\n",owner,path); return 1; }
    uint32_t header[8];
    int ok=0;
    uint8_t *data=NULL;
    if(fread(header,4,8,file)!=8 || memcmp(header,"DNB1",4) || header[5]!=count || !header[3] || !header[4] ||
       header[3]>65536 || header[4]>65536 || header[6]%32 || header[6]<32u+12u*(header[3]+header[4]) ||
       header[1]!=32u+count*header[6] || !header[7] || header[7]>header[4]) goto done;
    data=nm_heap_alloc(owner,32,header[1]);
    if(!data) goto done;
    nm_memcpy(data,header,32);
    if(fread(data+32,1,header[1]-32u,file)!=header[1]-32u || fgetc(file)!=EOF) goto done;
    nm_damage_bodies_t candidate={data,header[2],header[3],header[4],header[6],header[7],count,clean};
    for(unsigned v=0;v<count;++v) {
        const nm_damage_body_t *body=nm_damage_body(&candidate,v);
        if(body->index!=v || !body->model || (v==clean)!=(body->model==header[2])) goto done;
        for(unsigned a=0;a<3;++a)
            if(!isfinite(body->bounds[a]) || !isfinite(body->bounds[a+3]) || body->bounds[a]>body->bounds[a+3]) goto done;
        const float *values=(const float *)(body+1);
        for(unsigned i=0;i<3u*(header[3]+header[4]);++i) if(!isfinite(values[i])) goto done;
    }
    *bodies=candidate; data=NULL; ok=1;
    printf("Native %s: %u variants of model %06x (%u vertices, %u faces, up to %u light classes), %u bytes\n",
        owner,count,(unsigned)header[2],(unsigned)header[3],(unsigned)header[4],(unsigned)header[7],(unsigned)header[1]);
done:
    if(!ok) printf("FAIL: invalid %s %s\n",owner,path);
    free(data);
    fclose(file);
    return ok;
}

void nm_damage_bodies_unload(nm_damage_bodies_t *bodies) {
    free(bodies->data);
    nm_memset(bodies,0,sizeof(*bodies));
}

int nm_damage_body_apply(const nm_damage_bodies_t *bodies, unsigned variant, const nm_pack_t *pack, unsigned mesh) {
    return nm_damage_body_apply_mapped(bodies,variant,pack,mesh,NULL);
}

int nm_damage_body_apply_mapped(const nm_damage_bodies_t *bodies, unsigned variant, const nm_pack_t *pack,
        unsigned mesh, const uint16_t *face_of) {
    if(!bodies->data || variant>=bodies->count || !pack || mesh>=pack->header->meshes) return 0;
    /* nm_pack_load owns writable CPU memory (as the reflection UV rewrite of the windows). */
    nm_mesh_t *out=(nm_mesh_t *)pack->meshes+mesh;
    if(out->vertex_count!=bodies->vertices || out->face_count!=bodies->faces) return 0;
    for(unsigned i=0;face_of && i<bodies->faces;++i) if(face_of[i]>=bodies->faces) return 0;
    const nm_damage_body_t *body=nm_damage_body(bodies,variant);
    const float *positions=(const float *)(body+1),*normals=positions+3u*bodies->vertices;
    nm_vertex_t *vertices=(nm_vertex_t *)pack->vertices+out->vertex_first;
    nm_face_t *faces=(nm_face_t *)pack->faces+out->face_first;
    for(unsigned i=0;i<bodies->vertices;++i) nm_memcpy(vertices[i].position,positions+3u*i,sizeof(vertices[i].position));
    for(unsigned i=0;i<bodies->faces;++i) {
        const unsigned from=face_of ? face_of[i] : i;
        nm_memcpy(faces[i].normal,normals+3u*from,sizeof(faces[i].normal));
    }
    nm_memcpy(out->bounds,body->bounds,sizeof(out->bounds));
    return 1;
}

int nm_damage_body_face_map(const nm_pack_t *pack, unsigned clean, unsigned mesh, uint16_t *face_of, unsigned capacity) {
    if(!pack || !face_of || clean>=pack->header->meshes || mesh>=pack->header->meshes || clean==mesh) return 0;
    const nm_mesh_t *a=pack->meshes+clean,*m=pack->meshes+mesh;
    if(a->vertex_count!=m->vertex_count || a->face_count!=m->face_count || m->face_count>capacity || m->face_count>65536) return 0;
    const nm_vertex_t *av=pack->vertices+a->vertex_first,*mv=pack->vertices+m->vertex_first;
    for(unsigned i=0;i<m->vertex_count;++i)
        if(memcmp(av[i].position,mv[i].position,sizeof(av[i].position)) || av[i].u!=mv[i].u || av[i].v!=mv[i].v) return 0;
    const nm_face_t *af=pack->faces+a->face_first,*mf=pack->faces+m->face_first;
    for(unsigned i=0;i<m->face_count;++i) {
        unsigned j=0;
        while(j<a->face_count && (memcmp(af[j].indices,mf[i].indices,sizeof(af[j].indices)) || af[j].count!=mf[i].count)) ++j;
        if(j==a->face_count || memcmp(af[j].normal,mf[i].normal,sizeof(af[j].normal))) return 0;
        face_of[i]=(uint16_t)j;
    }
    return 1;
}
