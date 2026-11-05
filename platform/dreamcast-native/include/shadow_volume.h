#ifndef DAYTONA_NATIVE_SHADOW_VOLUME_H
#define DAYTONA_NATIVE_SHADOW_VOLUME_H
#include <math.h>
#include <string.h>
#include "meshpack.h"
#include "nm_mem.h"

/* Car ground shadows are closed prisms over the authored footprint outline
 * (see code/render/scene/practice.c). A prism over n outline points has 2n
 * wall triangles and two (n-2)-triangle caps. */
enum { NM_SHADOW_OUTLINE_MAX=16, NM_SHADOW_VOLUMES_MAX=45 };
static inline unsigned nm_shadow_volume_triangles(unsigned outline) { return outline>=3 ? 4*outline-4 : 0; }

/* Boundary loop of a flat quad mesh: edges used by exactly one face, chained,
 * with collinear points dropped. Returns the vertex count or 0. */
static inline unsigned nm_shadow_outline(const nm_pack_t *pack,const nm_mesh_t *mesh,float outline[][3]) {
    enum { EDGES=64 };
    const float *from[EDGES],*to[EDGES];
    unsigned edges=0;
    for(unsigned f=0;f<mesh->face_count;++f) {
        const nm_face_t *face=pack->faces+mesh->face_first+f;
        if(face->count<3 || face->count>4) return 0;
        for(unsigned c=0;c<face->count;++c) {
            const float *a=pack->vertices[mesh->vertex_first+face->indices[c]].position;
            const float *b=pack->vertices[mesh->vertex_first+face->indices[(c+1)%face->count]].position;
            unsigned twin=edges;
            for(unsigned e=0;e<edges && twin==edges;++e)
                if(!memcmp(from[e],b,12) && !memcmp(to[e],a,12)) twin=e;
            if(twin<edges) { from[twin]=from[edges-1];to[twin]=to[edges-1];--edges; }
            else { if(edges==EDGES) return 0; from[edges]=a;to[edges]=b;++edges; }
        }
    }
    if(edges<3) return 0;
    const float *loop[EDGES];
    const float *at=from[0],*next=to[0];
    unsigned count=0;
    for(unsigned step=0;step<edges;++step) {
        loop[count++]=at;
        unsigned e=0;
        while(e<edges && memcmp(from[e],next,12)) ++e;
        if(e==edges) return 0;
        at=from[e];next=to[e];
    }
    if(memcmp(at,from[0],12)) return 0; /* Not one closed loop. */
    unsigned kept=0;
    for(unsigned i=0;i<count;++i) {
        const float *p=loop[(i+count-1)%count],*q=loop[i],*r=loop[(i+1)%count];
        const float cross=(q[0]-p[0])*(r[2]-q[2])-(q[2]-p[2])*(r[0]-q[0]);
        if(fabsf(cross)<1e-4f) continue;
        if(kept==NM_SHADOW_OUTLINE_MAX) return 0;
        nm_memcpy(outline[kept++],q,12);
    }
    return kept>=3 ? kept : 0;
}
#endif
