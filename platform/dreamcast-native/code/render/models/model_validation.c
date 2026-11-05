/* Model startup validation: transform, lighting, material and projection checks. */
#include "render/models/model_math.h"
#include "render/materials/materials.h"
#include "scene_loader.h"
#include <kos.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

/* Presentation import check only. No claim about game/TGP arithmetic parity. */
int nm_pack_check_math(const nm_pack_t *pack) {
    float max_error = 0, max_scaled = 0;
    unsigned vertices = 0, quantized_differences = 0, failures = 0;
    for (uint32_t i = 0; i < pack->header->instances; ++i) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        const nm_instance_t *instance = &pack->instances[i];
        const nm_mesh_t *mesh = &pack->meshes[instance->mesh];
        const float *m = instance->matrix;
        load_matrix(m);
        for (uint32_t v = 0; v < mesh->vertex_count; ++v) {
            const float *p = pack->vertices[mesh->vertex_first + v].position;
            const shz_vec4_t transformed = shz_xmtrx_transform_vec4(shz_vec4_init(p[0], p[1], p[2], 1));
            const float actual[3] = {transformed.x, transformed.y, transformed.z};
            if (transformed.w != 1) ++failures;
            for (unsigned a = 0; a < 3; ++a) {
                const float reference = p[0] * m[a] + p[1] * m[3 + a] + p[2] * m[6 + a] + m[9 + a];
                const float magnitude = fabsf(p[0] * m[a]) + fabsf(p[1] * m[3 + a]) +
                                        fabsf(p[2] * m[6 + a]) + fabsf(m[9 + a]);
                const float error = fabsf(actual[a] - reference);
                /* Cancellation-aware guard, not a relative-to-result claim. */
                const float bound = 8 * FLT_EPSILON * magnitude + FLT_MIN;
                if (!isfinite(reference) || !isfinite(actual[a]) || !isfinite(magnitude) || error > bound) ++failures;
                if (error > max_error) max_error = error;
                if (magnitude && error / magnitude > max_scaled) max_scaled = error / magnitude;
                const float focus = a < 2 ? instance->focus[a] : 1;
                const float focused_ref = reference * focus, focused_actual = actual[a] * focus;
                if (!isfinite(focused_ref) || !isfinite(focused_actual)) ++failures;
                uint32_t ref_bits, actual_bits;
                memcpy(&ref_bits, &focused_ref, 4); memcpy(&actual_bits, &focused_actual, 4);
                quantized_differences += (ref_bits >> 8) != (actual_bits >> 8);
            }
            ++vertices;
        }
    }
    printf("Native model FTRV check: %u instance vertices; max abs=%g scaled=%g "
           "24-bit component differences=%u failures=%u (8*FLT_EPSILON magnitude bound)\n",
           vertices, (double)max_error, (double)max_scaled, quantized_differences, failures);
    return failures == 0;
}

/* Facing of a supplied-normal face, sum over view axes of (M n)(M p + t), on
 * the scalar path (fast=0) or through FTRV and FIPR (fast=1), the operations
 * face_light uses. *bound receives the cancellation-aware rounding bound of
 * either path: each transformed component is within 8*FLT_EPSILON of its
 * term magnitudes (the FTRV check above), the dot adds as much again, so the
 * value is within 24*FLT_EPSILON*sum(|M||n| * (|M||p|+|t|)) of the exact one. */
static float check_facing(const nm_pack_t *pack, const nm_mesh_t *mesh, const nm_instance_t *instance,
        const nm_face_t *face, int fast, float *bound) {
    const float *m = instance->matrix, *n = face->normal;
    const float *p = pack->vertices[mesh->vertex_first + face->indices[2]].position;
    float normal[3], point[3], magnitude = 0;
    for (unsigned a = 0; a < 3; ++a) {
        normal[a] = n[0]*m[a] + n[1]*m[3+a] + n[2]*m[6+a];
        point[a] = p[0]*m[a] + p[1]*m[3+a] + p[2]*m[6+a] + m[9+a];
        magnitude += (fabsf(n[0]*m[a]) + fabsf(n[1]*m[3+a]) + fabsf(n[2]*m[6+a])) *
                     (fabsf(p[0]*m[a]) + fabsf(p[1]*m[3+a]) + fabsf(p[2]*m[6+a]) + fabsf(m[9+a]));
    }
    *bound = 24 * FLT_EPSILON * magnitude + FLT_MIN;
    if (fast) {
        const shz_vec4_t tp = shz_xmtrx_transform_vec4(shz_vec4_init(p[0], p[1], p[2], 1));
        const shz_vec4_t tn = shz_xmtrx_transform_vec4(shz_vec4_init(n[0], n[1], n[2], 0));
        return shz_dot6f(tn.x, tn.y, tn.z, tp.x, tp.y, tp.z);
    }
    return normal[0]*point[0] + normal[1]*point[1] + normal[2]*point[2];
}

/* edge_on: a face whose facing lies within the rounding bound of zero on
 * both paths has the eye in its plane; either side is a correct answer (it
 * projects to a line) and the paths may disagree. Such faces are counted and
 * printed, not failed, and their luma (which follows the side) is not
 * compared. A side difference with either facing outside twice the bound
 * still fails. Cars keep the exact gate (edge_on=0). */
static int check_lighting(const nm_pack_t *pack, unsigned allowed_delta, int edge_on) {
    const nm_material_t test_material = {.diffuse = 80, .ambient = 3.75f,
        .specular_scale = 4, .specular_control = 1};
    const float test_points[3][3] = {{0,1,2}, {0,0,2}, {1,0,2}};
    const float test_normal[3] = {0,0,2}, test_light[3] = {0,0,0.5f};
    const int expected[4] = {83, 97, 43 | 256, 45 | 256};
    for (unsigned mode = 0; mode < 4; ++mode)
        if (nm_face_lighting(mode, test_normal, test_points, test_light, &test_material) != expected[mode]) return 0;
    uint32_t reference_hash = 2166136261u, actual_hash = 2166136261u;
    unsigned count = 0, differences = 0, rear_differences = 0, max_delta = 0, invalid = 0;
    unsigned ambiguous = 0, ambiguous_differences = 0;
    float ambiguous_bound = 0;
    for (unsigned i = 0; i < pack->header->instances; ++i) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        const nm_instance_t *instance = &pack->instances[i];
        const nm_mesh_t *mesh = &pack->meshes[instance->mesh];
        load_matrix(instance->matrix);
        for (unsigned f = 0; f < mesh->face_count; ++f) {
            const nm_face_t *face = &pack->faces[mesh->face_first + f];
            const int reference = face_light(pack, mesh, instance, face, 0);
            const int actual = face_light(pack, mesh, instance, face, 1);
            if (reference < 0 || actual < 0) ++invalid;
            differences += reference != actual;
            int excused = 0;
            if (mesh->mode < 2 && ((reference ^ actual) & 256) && differences <= 4) {
                /* Evidence for either outcome: both facings and the bound. */
                float bound, fast_bound;
                const float scalar_facing = check_facing(pack, mesh, instance, face, 0, &bound);
                const float fast_facing = check_facing(pack, mesh, instance, face, 1, &fast_bound);
                printf("Native lighting side difference: instance=%u face=%u scalar facing=%g FTRV facing=%g bound=%g\n",
                       i, f, (double)scalar_facing, (double)fast_facing, (double)bound);
            }
            if (edge_on && mesh->mode < 2) {
                float bound, fast_bound;
                const float scalar_facing = check_facing(pack, mesh, instance, face, 0, &bound);
                const float fast_facing = check_facing(pack, mesh, instance, face, 1, &fast_bound);
                /* Each path is within the bound of the exact value; if that
                 * is within the bound of zero, both are within twice it. */
                excused = isfinite(scalar_facing) && isfinite(fast_facing) && isfinite(bound) &&
                    fabsf(scalar_facing) <= 2*bound && fabsf(fast_facing) <= 2*bound;
                if (excused) {
                    ++ambiguous;
                    ambiguous_differences += ((reference ^ actual) & 256) != 0;
                    if (bound > ambiguous_bound) ambiguous_bound = bound;
                }
            }
            if (!excused) {
                rear_differences += ((reference ^ actual) & 256) != 0;
                const unsigned delta = (unsigned)abs((reference & 255) - (actual & 255));
                if (delta > max_delta) max_delta = delta;
            }
            reference_hash = (reference_hash ^ (uint32_t)reference) * 16777619u;
            actual_hash = (actual_hash ^ (uint32_t)actual) * 16777619u;
            ++count;
        }
    }
    uint64_t elapsed[5];
    for (unsigned pass = 0; pass < 5; ++pass) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        uint32_t hash = 2166136261u;
        const uint64_t started = timer_ns_gettime64();
        for (unsigned i = 0; i < pack->header->instances; ++i) {
            const nm_instance_t *instance = &pack->instances[i];
            const nm_mesh_t *mesh = &pack->meshes[instance->mesh];
            load_matrix(instance->matrix);
            for (unsigned f = 0; f < mesh->face_count; ++f)
                hash = (hash ^ (uint32_t)face_light(pack, mesh, instance,
                        &pack->faces[mesh->face_first + f], 1)) * 16777619u;
        }
        elapsed[pass] = (timer_ns_gettime64() - started) / 1000;
        if (hash != actual_hash) ++invalid;
    }
    for (unsigned a = 0; a < 5; ++a) for (unsigned b = a+1; b < 5; ++b)
        if (elapsed[a] > elapsed[b]) { uint64_t t = elapsed[a]; elapsed[a] = elapsed[b]; elapsed[b] = t; }
    printf("Native lighting: faces=%u scalar_hash=%08lx FTRV_hash=%08lx differences=%u "
           "rear_differences=%u max_luma_delta=%u invalid=%u; FTRV+light min/median/max=%llu/%llu/%llu us\n",
           count, (unsigned long)reference_hash, (unsigned long)actual_hash, differences,
           rear_differences, max_delta, invalid, (unsigned long long)elapsed[0],
           (unsigned long long)elapsed[2], (unsigned long long)elapsed[4]);
    if (edge_on) printf("Native lighting edge-on faces: %u with the eye in the face plane within the rounding bound "
        "(largest bound %g), %u of them on different sides in the two paths; excluded from side and luma parity\n",
        ambiguous, (double)ambiguous_bound, ambiguous_differences);
    return !rear_differences && max_delta<=allowed_delta && !invalid;
}

int nm_pack_check_lighting(const nm_pack_t *pack) { return check_lighting(pack,0,0); }
int nm_pack_check_scenery_lighting(const nm_pack_t *pack) {
    /* The owned race-scenery exporter explicitly prepares both facing
     * branches and adjacent +/-1 integer lumas for rigid FTRV view rotations.
     * Keep the original car gate exact. Side parity is required of every
     * scenery face except one seen edge-on within the rounding bound
     * (check_lighting). With baked scenery light (NM_PACK_BAKED_LIGHT) the
     * renderer evaluates no world luma: of this check it uses only the
     * facing sign, FTRV and FIPR, for the faces its plane-constant test
     * leaves undecided; the luma comparison then matters to unbaked packs. */
    for (unsigned i=0;i<pack->header->meshes;++i) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        const nm_mesh_t *mesh=pack->meshes+i;
        if (mesh->mode>1) return 0;
        for (unsigned f=0;f<mesh->face_count;++f) {
            const nm_face_t *face=pack->faces+mesh->face_first+f;
            const nm_material_t *m=pack->materials+face->material;
            /* Variable specular only where the renderer evaluates it (NM_PACK_LIVE_SPECULAR, sq_live_update). */
            if ((mesh->mode&1) && m->specular_control && m->specular_scale && m->ambient<255 &&
                !(nm_pack_flags(pack)&NM_PACK_LIVE_SPECULAR)) return 0;
            for (unsigned a=0;a<3;++a) if (fabsf(face->normal[a])>4) return 0;
        }
    }
    return check_lighting(pack,1,1);
}

int nm_pack_check_materials(const nm_pack_t *pack) {
    unsigned faces = 0, solids = 0, textured = 0, missing = 0;
    for (unsigned i = 0; i < pack->header->instances; ++i) {
        const nm_instance_t *instance = &pack->instances[i];
        const nm_mesh_t *mesh = &pack->meshes[instance->mesh];
        load_matrix(instance->matrix);
        for (unsigned f = 0; f < mesh->face_count; ++f) {
            const nm_face_t *face = &pack->faces[mesh->face_first + f];
            const int lighting = face_light(pack, mesh, instance, face, 1);
            const uint16_t *header = pack->materials[face->material].header;
            const nm_material_binding_t *binding = nm_materials_find(header, lighting & 255);
            ++faces;
            if (lighting < 0 || !binding) { ++missing; continue; }
            if (binding->resource == NM_MATERIAL_SOLID) ++solids;
            else {
                ++textured;
                if (!nm_materials_texture(binding->resource)) ++missing;
            }
        }
    }
    printf("Native material binding: faces=%u textured=%u solid=%u missing=%u (live computed face lights)\n",
           faces, textured, solids, missing);
    return missing == 0;
}

int nm_pack_check_projection(const nm_pack_t *pack) {
    const nm_view_t test_view = {{0, 0}, {-10, 10, -10, 10}, 1};
    const nm_view_vertex_t test_input[3] = {{-1, -1, 0, 0, 0}, {2, 0, 2, 1, 0}, {0, 2, 2, 0, 1}};
    const nm_screen_vertex_t expected[4] = {{0.5f, 0.5f, 1, 0.5f, 0}, {1, 0, 0.5f, 0.5f, 0},
                                          {0, -1, 0.5f, 0, 0.5f}, {-0.5f, -0.5f, 1, 0, 0.5f}};
    nm_screen_vertex_t projected[NM_CLIP_VERTICES];
    if (nm_project_triangle(&test_view, test_input, projected) != 4) return 0;
    unsigned matched = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int found = 0;
        for (unsigned j = 0; j < 4; ++j) {
            const nm_screen_vertex_t a = projected[i], b = expected[j];
            if (fabsf(a.x-b.x) < 1e-6f && fabsf(a.y-b.y) < 1e-6f && fabsf(a.q-b.q) < 1e-6f &&
                fabsf(a.uq-b.uq) < 1e-6f && fabsf(a.vq-b.vq) < 1e-6f) {
                found = 1; matched |= 1u << j;
            }
        }
        if (!found) return 0;
    }
    if (matched != 15) return 0;
    unsigned capacity = 0;
    for (unsigned i = 0; i < pack->header->meshes; ++i)
        if (pack->meshes[i].vertex_count > capacity) capacity = pack->meshes[i].vertex_count;
    nm_cached_vertex_t *points = malloc(capacity * sizeof(*points));
    if (!points) return 0;
    uint64_t elapsed[5];
    uint64_t transform_ns = 0, projection_ns = 0;
    unsigned triangles = 0, visible = 0, emitted = 0;
    for (unsigned pass = 0; pass < 5; ++pass) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        triangles = visible = emitted = 0;
        transform_ns = projection_ns = 0;
        const uint64_t started = timer_ns_gettime64();
        for (unsigned i = 0; i < pack->header->instances; ++i) {
            const nm_instance_t *inst = &pack->instances[i];
            const nm_mesh_t *mesh = &pack->meshes[inst->mesh];
            const nm_view_t view = instance_view(pack, inst);
            if (!nm_view_valid(&view)) continue;
            const uint64_t transform_started = timer_ns_gettime64();
            load_matrix(inst->matrix);
            for (unsigned v = 0; v < mesh->vertex_count; ++v) {
                const nm_vertex_t *vertex = &pack->vertices[mesh->vertex_first + v];
                const float *p = vertex->position;
                const shz_vec4_t transformed = shz_xmtrx_transform_vec4(shz_vec4_init(p[0], p[1], p[2], 1));
                const nm_view_vertex_t point = {transformed.x * inst->focus[0], transformed.y * inst->focus[1],
                                                transformed.z, vertex->u, vertex->v};
                nm_cache_vertex(&view, &point, &points[v]);
            }
            const uint64_t projection_started = timer_ns_gettime64();
            transform_ns += projection_started - transform_started;
            for (unsigned f = 0; f < mesh->face_count; ++f) {
                const nm_face_t *face = &pack->faces[mesh->face_first + f];
                const unsigned th = pack->materials[face->material].header[0];
                const float uscale = 1.0f / (float)(256u << (th & 7));
                const float vscale = 1.0f / (float)(256u << ((th >> 3) & 7));
                for (unsigned t = 1; t + 1 < face->count; ++t) {
                    const unsigned corners[3] = {0, t, t + 1};
                    const nm_cached_vertex_t *input[3] = {&points[face->indices[corners[0]]],
                        &points[face->indices[corners[1]]], &points[face->indices[corners[2]]]};
                    const unsigned count = nm_project_cached_triangle(&view, input, projected);
                    for (unsigned v = 0; v < count; ++v) {
                        projected[v].uq *= uscale; projected[v].vq *= vscale;
                    }
                    ++triangles; visible += count != 0; emitted += count;
                }
            }
            projection_ns += timer_ns_gettime64() - projection_started;
        }
        elapsed[pass] = (timer_ns_gettime64() - started) / 1000;
    }
    free(points);
    for (unsigned a = 0; a < 5; ++a) for (unsigned b = a + 1; b < 5; ++b)
        if (elapsed[a] > elapsed[b]) { uint64_t swap = elapsed[a]; elapsed[a] = elapsed[b]; elapsed[b] = swap; }
    printf("Native projection self-check PASS; %u triangles, %u visible, %u output vertices; "
           "scratch=%u bytes; transform+clip min/median/max=%llu/%llu/%llu us (5 passes, no drawing)\n",
           triangles, visible, emitted, capacity * (unsigned)sizeof(*points),
           (unsigned long long)elapsed[0], (unsigned long long)elapsed[2], (unsigned long long)elapsed[4]);
    printf("Last projection pass: FTRV/vertex cache=%llu us, indexed faces/clipping=%llu us\n",
           (unsigned long long)(transform_ns / 1000), (unsigned long long)(projection_ns / 1000));
    return 1;
}

