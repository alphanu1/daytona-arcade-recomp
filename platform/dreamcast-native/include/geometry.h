#ifndef DAYTONA_NATIVE_GEOMETRY_H
#define DAYTONA_NATIVE_GEOMETRY_H
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "finite.h"
#include "nm_mem.h"
#ifdef __DREAMCAST__
#include <sh4zam/shz_sh4zam.h>
#endif

/* IEEE binary32, as required by the model pack and geometry kernel. Strict
 * SH4 isfinite emits unordered-FP helper calls; inspecting the exponent
 * preserves NaN/Inf rejection without arithmetic or those calls. */
static inline int nm_finite_float(float value) {
    uint32_t bits;
    nm_memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

/* A triangle gains at most one vertex per clipping plane (near + 4 sides). */
#define NM_CLIP_VERTICES 8
typedef struct { float x, y, z, u, v; } nm_view_vertex_t;
typedef struct { float x, y, q, uq, vq; } nm_screen_vertex_t;
typedef struct {
    float center[2], bounds[4], near_z; /* left, right, top, bottom */
} nm_view_t;
typedef struct {
    nm_view_vertex_t view;
    float x, y, q; /* Derive uq/vq from view.u/v when emitting, not extra storage. */
} nm_cached_vertex_t;

/* q is positive only for projected inside points. Otherwise its storage holds
 * a sign-tagged integer: five plane bits, invalid=32, unused/demanded=64/65,
 * demanded and proven inside=66.
 * SH4ZAM's alias type permits direct aligned integer access to float storage;
 * host memcpy preserves the same bits without float/int conversions. */
static inline unsigned nm_cached_outside(const nm_cached_vertex_t *point) {
#ifdef __DREAMCAST__
    const uint32_t bits = *(const shz_alias_uint32_t *)&point->q;
#else
    uint32_t bits;
    nm_memcpy(&bits, &point->q, sizeof(bits));
#endif
    return bits >> 31 ? bits & 127u : 0;
}
/* Nonzero status only: an inside point must instead receive its positive q. */
static inline void nm_cached_set_outside(nm_cached_vertex_t *point, unsigned outside) {
    const uint32_t bits = 0x80000000u | outside;
#ifdef __DREAMCAST__
    *(shz_alias_uint32_t *)&point->q = bits;
#else
    nm_memcpy(&point->q, &bits, sizeof(bits));
#endif
}

/* Validate once when configuring/changing the view, before cached operations.
 * Cache once per instance vertex, reuse across indexed faces. The cache and
 * view must belong to the same validated camera. UV scaling can follow projection when
 * different materials share source UV units; scale uq/vq before submission. */
int nm_view_valid(const nm_view_t *view);
/* Conservative object-AABB/frustum test. Bounds are min XYZ then max XYZ;
 * matrix is column-major affine 3x4, focus applies to transformed X/Y.
 * Invalid/overflow-prone input is retained, not falsely rejected. */
int nm_bounds_visible(const nm_view_t *view, const float bounds[6],
                      const float matrix[12], const float focus[2]);
/* 0=outside, 1=intersecting/uncertain, 2=strictly inside all five planes,
 * including the conservative FTRV rounding envelope. */
int nm_bounds_classify(const nm_view_t *view, const float bounds[6],
                      const float matrix[12], const float focus[2]);
#ifdef __DREAMCAST__
typedef shz_vec4_t nm_bounds_vector_t;
#else
typedef struct { float e[4]; } nm_bounds_vector_t;
#endif
typedef struct {
    nm_bounds_vector_t plane[5], error[5];
    int valid; /* 0 none (classify says 1), 1 general, 2 every value below 2^84 (geometry.c, the bounded form) */
} nm_bounds_planes_t;
/* Prepare once per camera/instance; bounds stay in original model coordinates. */
void nm_bounds_planes_prepare(nm_bounds_planes_t *out, const nm_view_t *view,
    const float matrix[12], const float focus[2]);
int nm_bounds_planes_classify(const nm_bounds_planes_t *planes, const float bounds[6]);
/* A cheap decision of the same classification: 0, 1 or 2 only when
 * nm_bounds_planes_classify (planes of this view, matrix and focus) returns
 * the same value, else 3 (take the planes). geometry.c states the margin. */
int nm_bounds_quick_classify(const nm_view_t *view, const float bounds[6], const float matrix[12], const float focus[2]);
/* Keep the per-vertex path visible to the SH4 compiler: transformed coordinates
 * can stay in registers instead of being spilled across a call for each point.
 * These are the same strict operations used by uncached triangle projection. */
/* A view's limits from finite values: a compare, not libm's fmaxf/fminf and their two classifications an instance. */
static inline float nm_view_limit_low(float value, float limit) { return value > limit ? value : limit; }
static inline float nm_view_limit_high(float value, float limit) { return value < limit ? value : limit; }
static inline int nm_geometry_finite_vertex(nm_view_vertex_t p) {
    return nm_finite_float(p.x) && nm_finite_float(p.y) && nm_finite_float(p.z) && nm_finite_float(p.u) && nm_finite_float(p.v);
}

static inline unsigned nm_geometry_plane_mask(float distance, unsigned plane) {
    return !nm_finite_float(distance) ? 32 : (distance < 0 ? 1u << plane : 0);
}

static inline __attribute__((always_inline)) unsigned nm_geometry_classify(const nm_view_t *view, const nm_view_vertex_t *p) {
    return nm_geometry_plane_mask(p->z - view->near_z, 0) |
        nm_geometry_plane_mask(p->x + (view->center[0] - view->bounds[0]) * p->z, 1) |
        nm_geometry_plane_mask((view->bounds[1] - view->center[0]) * p->z - p->x, 2) |
        nm_geometry_plane_mask((view->center[1] - view->bounds[2]) * p->z - p->y, 3) |
        nm_geometry_plane_mask(p->y + (view->bounds[3] - view->center[1]) * p->z, 4);
}

static inline __attribute__((always_inline)) int nm_geometry_project_point(const nm_view_t *view, const nm_view_vertex_t *input, nm_screen_vertex_t *out) {
#ifdef __DREAMCAST__
    /* SH4ZAM reciprocal squares its input. Keep that square normal/finite;
     * retain the generic geometry kernel's extreme-input behavior outside it. */
    const float q = input->z >= 0x1p-63f && input->z < 0x1p64f ?
        shz_invf_fsrra(input->z) : 1.0f / input->z;
#else
    const float q = 1.0f / input->z;
#endif
    nm_screen_vertex_t p = {view->center[0] + input->x * q,
        view->center[1] - input->y * q, q, input->u * q, input->v * q};
    if (!nm_finite_float(p.x) || !nm_finite_float(p.y) || !nm_finite_float(q) || q <= 0 ||
        !nm_finite_float(p.uq) || !nm_finite_float(p.vq)) return 0;
    /* Plane arithmetic can round a boundary point a fraction outside. */
    if (p.x < view->bounds[0]) p.x = view->bounds[0];
    if (p.x > view->bounds[1]) p.x = view->bounds[1];
    if (p.y < view->bounds[2]) p.y = view->bounds[2];
    if (p.y > view->bounds[3]) p.y = view->bounds[3];
    *out = p;
    return 1;
}

static inline __attribute__((always_inline)) void nm_cache_vertex_classified(const nm_view_t *view, const nm_view_vertex_t *input, nm_cached_vertex_t *out, int inside) {
    out->view = *input;
    nm_cached_set_outside(out, 32);
    if (!nm_geometry_finite_vertex(*input)) return;
    const unsigned outside = inside ? 0 : nm_geometry_classify(view, input);
    if (!outside) {
        nm_screen_vertex_t projected;
        if (!nm_geometry_project_point(view, input, &projected)) return;
        out->x = projected.x; out->y = projected.y; out->q = projected.q;
    }
    if (outside) nm_cached_set_outside(out, outside);
}
static inline __attribute__((always_inline)) void nm_cache_vertex(const nm_view_t *view, const nm_view_vertex_t *input, nm_cached_vertex_t *out) {
    nm_cache_vertex_classified(view,input,out,0);
}
/* Bounded input (render/models/model_submission.h, sq_batch_bounded): every
 * coordinate of the transformed point is finite and below 2^62, the view is
 * valid with |centre|, |bounds| <= 2^20 and near_z >= 2^-10. Then no finite
 * test of nm_cache_vertex_classified can fail:
 * - the five plane distances are sums of two finite terms below 2^84;
 * - a point that is not outside has z >= near_z, so q = 1/z lies in
 *   (2^-62, 2^10] (the FSRRA form squares z: 2^-20..2^124, normal), and
 *   centre + x*q, u*q and v*q stay below 2^73.
 * The same expressions without those tests give the same bits. A vertex of a
 * proven-inside group skips classification as before; its z is compared with
 * near_z here, and the checked routine runs if it fails. */
static inline unsigned nm_geometry_plane_side(float distance, unsigned plane) {
    return distance < 0 ? 1u << plane : 0;
}
static inline __attribute__((always_inline)) unsigned nm_geometry_classify_bounded(const nm_view_t *view, const nm_view_vertex_t *p) {
    return nm_geometry_plane_side(p->z - view->near_z, 0) |
        nm_geometry_plane_side(p->x + (view->center[0] - view->bounds[0]) * p->z, 1) |
        nm_geometry_plane_side((view->bounds[1] - view->center[0]) * p->z - p->x, 2) |
        nm_geometry_plane_side((view->center[1] - view->bounds[2]) * p->z - p->y, 3) |
        nm_geometry_plane_side(p->y + (view->bounds[3] - view->center[1]) * p->z, 4);
}
static inline __attribute__((always_inline)) void nm_cache_vertex_bounded(const nm_view_t *view, const nm_view_vertex_t *input, nm_cached_vertex_t *out, int inside) {
    /* z < near_z, as a sign test: an inverted float compare would call
     * libgcc's unordered helper on the SH4 (finite.h). Both are finite here
     * and near_z >= 2^-10, so the difference of unequal values is normal. */
    if (inside && nm_float_negative(input->z - view->near_z)) { nm_cache_vertex_classified(view, input, out, inside); return; }
    out->view = *input;
    const unsigned outside = inside ? 0 : nm_geometry_classify_bounded(view, input);
    if (outside) { nm_cached_set_outside(out, outside); return; }
#ifdef __DREAMCAST__
    const float q = input->z >= 0x1p-63f && input->z < 0x1p64f ?
        shz_invf_fsrra(input->z) : 1.0f / input->z;
#else
    const float q = 1.0f / input->z;
#endif
    float x = view->center[0] + input->x * q, y = view->center[1] - input->y * q;
    if (x < view->bounds[0]) x = view->bounds[0];
    if (x > view->bounds[1]) x = view->bounds[1];
    if (y < view->bounds[2]) y = view->bounds[2];
    if (y > view->bounds[3]) y = view->bounds[3];
    out->x = x; out->y = y; out->q = q;
}
/* Near-plane clipping only (render/models/model_submission.h, wide view).
 * The tile accelerator bins a polygon into the tiles of the global clip area
 * it touches and discards one that touches none, and pixels are clipped to
 * the frame buffer window, so the CPU needs to cut triangles at the near
 * plane only. `guard` is the view with its side bounds moved out to a guard
 * band: a point is tested against the near plane, projected without
 * clamping, and only then compared with the band (two absolute values).
 * A point behind the near plane or beyond the band gets the plane bits of
 * the guard view, so the clipper cuts its faces at that view: no coordinate
 * outside the band ever reaches the PVR. `inside`: bounds proven inside the
 * guard view; only its z is compared, as in nm_cache_vertex_bounded.
 * Bounded input only (same conditions; the band is within 2^20). */
static inline __attribute__((always_inline)) void nm_cache_vertex_wide(const nm_view_t *guard, const nm_view_vertex_t *input, nm_cached_vertex_t *out, int inside) {
    if (nm_float_negative(input->z - guard->near_z)) {
        if (inside) { nm_cache_vertex_classified(guard, input, out, 0); return; }
        out->view = *input;
        nm_cached_set_outside(out, nm_geometry_classify_bounded(guard, input));
        return;
    }
    out->view = *input;
#ifdef __DREAMCAST__
    const float q = input->z >= 0x1p-63f && input->z < 0x1p64f ?
        shz_invf_fsrra(input->z) : 1.0f / input->z;
#else
    const float q = 1.0f / input->z;
#endif
    float x = guard->center[0] + input->x * q, y = guard->center[1] - input->y * q;
    if (!inside) {
        const float middle_x = (guard->bounds[0] + guard->bounds[1]) * 0.5f, half_x = (guard->bounds[1] - guard->bounds[0]) * 0.5f;
        const float middle_y = (guard->bounds[2] + guard->bounds[3]) * 0.5f, half_y = (guard->bounds[3] - guard->bounds[2]) * 0.5f;
        if (nm_float_negative(half_x - fabsf(x - middle_x)) | nm_float_negative(half_y - fabsf(y - middle_y))) {
            const unsigned outside = nm_geometry_classify_bounded(guard, input);
            if (outside) { nm_cached_set_outside(out, outside); return; }
            /* Plane arithmetic says inside: within rounding of the band. */
            if (x < guard->bounds[0]) x = guard->bounds[0];
            if (x > guard->bounds[1]) x = guard->bounds[1];
            if (y < guard->bounds[2]) y = guard->bounds[2];
            if (y > guard->bounds[3]) y = guard->bounds[3];
        }
    }
    out->x = x; out->y = y; out->q = q;
}
/* Internal slow path: enter through nm_project_cached_triangle below. */
unsigned nm_project_cached_clipped(const nm_view_t *view, const nm_cached_vertex_t *const points[3],
                                    unsigned any, nm_screen_vertex_t output[NM_CLIP_VERTICES]);
static inline __attribute__((always_inline)) unsigned nm_project_cached_triangle(
    const nm_view_t *view, const nm_cached_vertex_t *const points[3],
    nm_screen_vertex_t output[NM_CLIP_VERTICES]) {
    const unsigned a=nm_cached_outside(points[0]), b=nm_cached_outside(points[1]),
                   c=nm_cached_outside(points[2]);
    const unsigned any=a|b|c;
    if(any&32 || (a&b&c)) return 0;
    if(any) return nm_project_cached_clipped(view,points,any,output);
    for(unsigned i=0;i<3;++i) {
        const nm_cached_vertex_t *p=points[i];
        output[i]=(nm_screen_vertex_t){p->x,p->y,p->q,p->view.u*p->q,p->view.v*p->q};
    }
    return 3;
}

/* Input x/y already include focus; UVs are normalized. Output is logical
 * screen x/y and perspective attributes; PVR receives q, uq/q and vq/q.
 * Split source quads into a fan before calling. No allocation or side effects.
 * Returns 0 for empty/invalid input. Input/output must not alias. */
unsigned nm_project_triangle(const nm_view_t *view, const nm_view_vertex_t input[3],
                             nm_screen_vertex_t output[NM_CLIP_VERTICES]);
#endif
