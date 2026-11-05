/* The slot machine's glass (include/render/scene/slot_glass.h). */
#include "render/scene/slot_glass.h"
#include "geometry.h"
#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
#if __has_include("slot_glass_data.h")
#include "slot_glass_data.h" /* NATIVE_SLOT_GLASS_DIR (local.cfg.mk): prepare_world_objects.py glass */
#endif

#ifdef NM_SLOT_GLASS_DATA
/* Nearer than every layer of the reel's coplanar strips (1 + layer * NM_LAYER_DEPTH_STEP, layers 0..2):
 * the glass passes GEQUAL against them, and against nothing that stands in front of the reel. */
#define SLOT_GLASS_DEPTH (1.0f + 4.0f * NM_LAYER_DEPTH_STEP)
#define SLOT_GLASS_ALPHA 0x80u   /* the screen door's half coverage */

alignas(32) static pvr_poly_hdr_t header;
alignas(32) static shz_mat3x4_t reels[NM_OBJECT_SLOTS];
static nm_view_t view;
static float focus[2];
static unsigned reel_count, compiled;
static uint32_t frames, drawn;

static uint32_t level_argb[NM_SLOT_GLASS_LEVELS];

static inline uint32_t grey_argb(unsigned c) { return SLOT_GLASS_ALPHA << 24 | c << 16 | c << 8 | c; }

static inline void vertex(float x, float y, float q, uint32_t argb, int last) {
    pvr_vertex_t *v = pvr_dr_target();
    v->flags = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v->x = x * (640.0f / 496.0f) * ENJ_XSCALE; v->y = y * (480.0f / 384.0f);
    v->z = q * SLOT_GLASS_DEPTH;
    v->u = v->v = 0.0f;
    v->argb = argb; v->oargb = 0;
    pvr_dr_commit(v);
}
static void vertex_grey(float x, float y, float q, float grey, int last) {
    grey = grey < 0.0f ? 0.0f : grey > 255.0f ? 255.0f : grey;
    vertex(x, y, q, grey_argb((uint32_t)(grey + 0.5f)), last);
}

/* The whole reel on the screen: its points as the view's arithmetic would have them (nm_cache_vertex, the projection
 * of a point inside), without a classification per point. Three reels' 96 points took 280 us a frame that way, in
 * the slot machine's frames of the longest straight. 0: a point is near an edge, the band loop below decides. */
static inline int reel_project(float points[NM_SLOT_GLASS_LEVELS][2][3]) {
    const float cx = view.center[0], cy = view.center[1], fx = focus[0], fy = focus[1];
    const float x0 = view.bounds[0] + 1.0f, x1 = view.bounds[1] - 1.0f, y0 = view.bounds[2] + 1.0f, y1 = view.bounds[3] - 1.0f;
    const float near = 2.0f * view.near_z;
    for (unsigned l = 0; l < NM_SLOT_GLASS_LEVELS; ++l) for (unsigned s = 0; s < 2; ++s) {
        const float *p = nm_slot_glass_levels[l].side[s];
        const shz_vec3_t t = shz_xmtrx_transform_point3(shz_vec3_init(p[0], p[1], p[2]));
        /* Sign tests (finite.h): an inverted compare is a libgcc call, six a point here. */
        if (!nm_float_positive(t.z - near) || !nm_float_positive(1e6f - t.z)) return 0;
        const float q = shz_invf_fsrra(t.z), x = cx + t.x * fx * q, y = cy - t.y * fy * q;
        if (!nm_float_positive(x - x0) || !nm_float_positive(x1 - x) ||
            !nm_float_positive(y - y0) || !nm_float_positive(y1 - y)) return 0;
        float *out = points[l][s];
        out[0] = x; out[1] = y; out[2] = q;
    }
    return 1;
}

static void render(void *unused) {
    (void)unused;
    pvr_poly_hdr_t *h = pvr_dr_target(); *h = header; pvr_dr_commit(h);
    alignas(32) float screen[NM_SLOT_GLASS_LEVELS][2][3];
    nm_cached_vertex_t points[NM_SLOT_GLASS_LEVELS][2];
    for (unsigned r = 0; r < reel_count; ++r) {
        shz_xmtrx_load_3x4(reels + r);
        if (reel_project(screen)) {
            /* One strip a band, left and right side by level. */
            for (unsigned b = 0; b < NM_SLOT_GLASS_BANDS; ++b) {
                const unsigned first = nm_slot_glass_first[b], end = nm_slot_glass_first[b + 1];
                for (unsigned l = first; l < end; ++l) for (unsigned s = 0; s < 2; ++s) {
                    const float *p = screen[l][s];
                    vertex(p[0], p[1], p[2], level_argb[l], l + 1 == end && s);
                }
            }
            continue;
        }
        for (unsigned l = 0; l < NM_SLOT_GLASS_LEVELS; ++l) for (unsigned s = 0; s < 2; ++s) {
            const float *p = nm_slot_glass_levels[l].side[s];
            const shz_vec3_t t = shz_xmtrx_transform_point3(shz_vec3_init(p[0], p[1], p[2]));
            /* The grey travels in u (and v) through the clipper. */
            const float grey = (float)nm_slot_glass_levels[l].grey;
            const nm_view_vertex_t focused = {t.x * focus[0], t.y * focus[1], t.z, grey, grey};
            nm_cache_vertex(&view, &focused, &points[l][s]);
        }
        for (unsigned b = 0; b < NM_SLOT_GLASS_BANDS; ++b) {
            const unsigned first = nm_slot_glass_first[b], end = nm_slot_glass_first[b + 1];
            unsigned outside = 0;
            for (unsigned l = first; l < end; ++l) outside |= nm_cached_outside(&points[l][0]) | nm_cached_outside(&points[l][1]);
            if (!outside) {
                /* The band inside the view: one strip, left and right side by level. */
                for (unsigned l = first; l < end; ++l) for (unsigned s = 0; s < 2; ++s) {
                    const nm_cached_vertex_t *p = &points[l][s];
                    vertex_grey(p->x, p->y, p->q, p->view.u, l + 1 == end && s);
                }
                continue;
            }
            for (unsigned l = first; l + 1 < end; ++l) for (unsigned k = 0; k < 2; ++k) {
                /* Two triangles a level: (l,0) (l,1) (l+1,0); (l,1) (l+1,1) (l+1,0); clipped at the view. */
                const nm_cached_vertex_t *triangle[3] = {
                    k ? &points[l][1] : &points[l][0], k ? &points[l + 1][1] : &points[l][1], &points[l + 1][0]};
                nm_screen_vertex_t polygon[NM_CLIP_VERTICES];
                const unsigned count = nm_project_cached_triangle(&view, triangle, polygon);
                for (unsigned i = 1; i + 1 < count; ++i) {
                    const unsigned corner[3] = {0, i, i + 1};
                    for (unsigned j = 0; j < 3; ++j) {
                        const nm_screen_vertex_t *p = polygon + corner[j];
                        vertex_grey(p->x, p->y, p->q, shz_divf_fsrra(p->uq, p->q), j == 2);
                    }
                }
            }
        }
    }
    ++frames;
}

int nm_slot_glass_enqueue(const nm_header_t *world, const nm_instance_t *const placed[NM_OBJECT_SLOTS], unsigned count) {
    if (!world || !count || count > NM_OBJECT_SLOTS) return 1;
    if (!compiled) {
        pvr_poly_cxt_t context;
        pvr_poly_cxt_col(&context, PVR_LIST_TR_POLY);
        context.gen.shading = PVR_SHADE_GOURAUD;
        context.gen.culling = PVR_CULLING_NONE;   /* the facing test below, the arcade's */
        context.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        context.depth.write = PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(&header, &context);
        for (unsigned l = 0; l < NM_SLOT_GLASS_LEVELS; ++l) level_argb[l] = grey_argb(nm_slot_glass_levels[l].grey);
        compiled = 1;   /* no print here: a console write inside a frame masks interrupts */
    }
    /* The world's view (model_math.h instance_view), from the reels' own instances. */
    const nm_instance_t *first = placed[0];
    view = (nm_view_t){{(float)(world->crtc[0] + first->center[0]), (float)(384 - first->center[1] + world->crtc[1])},
        {nm_view_limit_low(first->viewport[0] + world->render[0], 0), nm_view_limit_high(first->viewport[2] + world->render[0] + 1, 496),
         nm_view_limit_low(384 - first->viewport[3] + world->render[1], 0), nm_view_limit_high(384 - first->viewport[1] + world->render[1] + 1, 384)},
        0.01f};
    if (!nm_view_valid(&view)) return 1;
    focus[0] = first->focus[0]; focus[1] = first->focus[1];
    reel_count = 0;
    for (unsigned r = 0; r < count; ++r) {
        const float *m = placed[r]->matrix;
        /* One-sided: the arcade's rear test (lighting.h: the normal against corner 2, in view space). */
        const float *n = nm_slot_glass_normal, *c = nm_slot_glass_corner;
        float dot = 0.0f;
        for (unsigned a = 0; a < 3; ++a) {
            const float normal = m[a] * n[0] + m[3 + a] * n[1] + m[6 + a] * n[2];
            const float corner = m[a] * c[0] + m[3 + a] * c[1] + m[6 + a] * c[2] + m[9 + a];
            dot += normal * corner;
        }
        if (dot < 0.0f) continue;
        shz_memcpy4(reels[reel_count++].elem, m, sizeof(reels[0].elem));
    }
    if (!reel_count) return 1;
    ++drawn;
    enj_render_list_add(PVR_LIST_TR_POLY, render, NULL);
    return 1;
}

void nm_slot_glass_report(void) {
    printf("Native slot machine glass: translucent Gouraud bands, alpha 1/2; queued in %lu frames, drawn in %lu\n",
           (unsigned long)drawn, (unsigned long)frames);
}
#else
int nm_slot_glass_enqueue(const nm_header_t *world, const nm_instance_t *const placed[NM_OBJECT_SLOTS], unsigned count) {
    (void)world; (void)placed; (void)count;
    return 1;
}
void nm_slot_glass_report(void) { puts("Native slot machine glass: no slot_glass_data.h (NATIVE_SLOT_GLASS_DIR): not drawn"); }
#endif
