#ifndef DAYTONA_NATIVE_RENDER_MODEL_MATH_H
#define DAYTONA_NATIVE_RENDER_MODEL_MATH_H
/* Shared presentation math; XMTRX is owned by the calling renderer/check. */
#include "model_renderer.h"
#include "geometry.h"
#include "lighting.h"
#include <sh4zam/shz_sh4zam.h>
#include <math.h>
#include <stdio.h>

static inline void load_matrix(const float *m) {
    alignas(32) shz_mat3x4_t matrix;
    shz_memcpy4(matrix.elem, m, sizeof(matrix));
    shz_xmtrx_load_3x4(&matrix);
}

static inline nm_view_t instance_view(const nm_pack_t *pack, const nm_instance_t *inst) {
    return (nm_view_t){
        {(float)(pack->header->crtc[0] + inst->center[0]),
         (float)(384 - inst->center[1] + pack->header->crtc[1])},
        {fmaxf(inst->viewport[0] + pack->header->render[0], 0),
         fminf(inst->viewport[2] + pack->header->render[0] + 1, 496),
         fmaxf(384 - inst->viewport[3] + pack->header->render[1], 0),
         fminf(384 - inst->viewport[1] + pack->header->render[1] + 1, 384)}, 0.01f};
}

static inline __attribute__((always_inline)) int face_light(const nm_pack_t *pack, const nm_mesh_t *mesh,
    const nm_instance_t *instance, const nm_face_t *face, int fast) {
    float points[3][3], normal[3] = {0};
    const float *m = instance->matrix;
    // Supplied normals need only p2 for facing; computed normals need all three.
    for (unsigned v = (mesh->mode & 2) ? 0 : 2; v < 4; ++v) {
        if (v == 3 && (mesh->mode & 2)) break;
        const float *p = v == 3 ? face->normal :
            pack->vertices[mesh->vertex_first + face->indices[v]].position;
        float *out = v == 3 ? normal : points[v];
        if (fast) {
            const shz_vec4_t t = shz_xmtrx_transform_vec4(shz_vec4_init(p[0], p[1], p[2], v == 3 ? 0 : 1));
            out[0] = t.x; out[1] = t.y; out[2] = t.z;
        } else {
            for (unsigned a = 0; a < 3; ++a) {
                out[a] = p[0]*m[a] + p[1]*m[3+a] + p[2]*m[6+a];
                if (v != 3) out[a] += m[9+a];
            }
        }
    }
#ifdef __DREAMCAST__
    if(fast) {
        /* Compare the dot-product change on IDENTICAL transformed inputs,
         * separately from existing scalar-vs-FTRV transform tolerance. */
        const int accelerated=nm_face_lighting_eval(mesh->mode,normal,points,
            instance->light,&pack->materials[face->material],1);
#ifdef NATIVE_LIGHT_VERIFY
        const int scalar=nm_face_lighting(mesh->mode,normal,points,
            instance->light,&pack->materials[face->material]);
        if(accelerated!=scalar) {
            printf("FAIL FIPR light: mode=%u material=%u scalar=%d accelerated=%d\n",
                (unsigned)mesh->mode,(unsigned)face->material,scalar,accelerated);
            return -1;
        }
#endif
        return accelerated;
    }
#endif
    return nm_face_lighting(mesh->mode,normal,points,
        instance->light,&pack->materials[face->material]);
}

#endif
