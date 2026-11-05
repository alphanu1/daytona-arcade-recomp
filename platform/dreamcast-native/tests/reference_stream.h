/* Reference renderer, host only: the independent definition of what the
 * model renderer must hand to the PVR. Formerly the console's startup stream
 * oracle (prepare_source/prepare_draw in model_renderer.c); it now runs on
 * the host over whole recorded scripts: tests/stream_sim (build_sim.py, run
 * with SIM_ORACLE=1) compares every submitted triangle of every frame with it.
 *
 * It shares no table, cache or shortcut with the renderer: every corner of
 * every face of every instance is transformed with scalar arithmetic, the
 * light is the arcade's definition (lighting.h), the binding is found by
 * binary search in the material pack (nm_material_find), the triangle is
 * clipped by the uncached clipper (nm_project_triangle) and UVs come from
 * the pack's integer texel coordinates. No bounds, face groups, face planes,
 * light classes, position representatives, strips or emission records.
 *
 * What the renderer may differ in, and nothing else:
 *   - order of the three vertices inside a triangle (it submits strips;
 *     culling is off, so a triangle is its three vertices);
 *   - a reference triangle that does not touch the screen may be missing
 *     (instances and face groups outside the view are skipped whole);
 *   - a whole-screen view is clipped at the guard band, not at the screen
 *     edge (the PVR clips there): pass the band as `guard`.
 *   - the fields that the renderer computes in SH4ZAM's forms
 *     (render/models/model_submission.h, NM_SH4ZAM_GFX), within stated
 *     tolerances, everything else of those triangles exactly:
 *       luma of a lit car face: the renderer's light classes (sq_class_light:
 *         FIPR dot products of the supplied normal with the light carried
 *         into model space) round differently from lighting.h; the binding
 *         of luma L-1 or L+1 is accepted where the reference has L (the side
 *         must be the same), with that binding's colour and u, v, exactly.
 *         NM_REFERENCE_LUMA_STEPS. Every accepted neighbour is counted.
 *       a triangle the reference clips: the renderer's clipper (sq_clip:
 *         single precision, near plane first, FSRRA division and 1/w, lerp,
 *         the cached projection of the corners) against the reference's
 *         double precision, sides first. Such a triangle is a polygon, which
 *         both write as a fan from its first vertex; the polygons are
 *         compared (nm_reference_polygon_match) up to that first vertex,
 *         x and y within NM_REFERENCE_CLIP_PIXELS output pixels, u and v
 *         within NM_REFERENCE_CLIP_TEXELS texels of the binding's texture, z
 *         (1/w times the depth factors) within NM_REFERENCE_CLIP_DEPTH of
 *         the reference's, a zero-length edge (vertices within those
 *         tolerances) dropped from either.
 *     Every difference is recorded (nm_reference_tolerance: maxima and a
 *     histogram per field) for the includer to report.
 *   - level of detail (meshpack.h, NM_PACK_LOD_GROUPS): which of a group's
 *     shapes is drawn depends on the frames before (hysteresis), so the
 *     includer passes the level the renderer drew each group at. The
 *     reference checks that level against its own record of the level
 *     before and the pack's switch depths (nm_reference_lod_level, double
 *     precision, the centre's depth through the instance matrix), then
 *     renders that range and none of the group's other ranges: a triangle of
 *     another range has no reference triangle. A group the renderer did not
 *     walk keeps its level; its shape must then be off the screen like any
 *     other triangle that is not submitted.
 *   - catch-fence sweep (model_submission.h, NM_FENCE_SWEEP): the reference
 *     finds the sweep groups and their planes from the pack on its own
 *     (nm_reference_sweep_find: groups whose nonempty ranges are one and the
 *     same quad under catch-fence stipple materials, level 0 drawn, in a mesh
 *     with one instance; plane k the least switch_depth[k] among them). When
 *     the includer says the renderer sweeps (sweep_group), such a group's
 *     level is the renderer's code: a plain level n (the panel straddles no
 *     plane) or 0x80|n0|n1<<2 (levels n0 < n1). The reference checks it
 *     plane by plane against the depths of the panel's four corners (double
 *     precision; within NM_REFERENCE_SWEEP_MARGIN of a plane the panel counts
 *     as on one side, and the level-of-detail slack covers the renderer's
 *     single-precision depths): "near" (no corner more than the margin
 *     beyond), "beyond" (no corner more than the margin in front), else
 *     "straddling". It renders a plain level as above and, for a code, each
 *     level n0..n1 with a face as that face clipped in double precision to
 *     its band of view-space depth (plane P at z = P * focus / 280; the cut
 *     interpolates x, y, u, v along the edge and sets z onto the plane): one
 *     clipped polygon, written as a fan from its first vertex and compared
 *     with the renderer's within the clipper's tolerances (when a point of it
 *     lies outside the view, each fan triangle is a polygon of its own,
 *     clipped at the view as any triangle). A group the renderer did not walk
 *     is rendered by the reference's own decision and must be off the screen.
 *   - world objects super-sampled by instance (meshpack.h, nm_material_supersample_depth; model_submission.h,
 *     sq_ss_select): an instance of a mesh whose materials carry a switch depth is drawn with the TSP super-sample
 *     bit on its headers or without, by the frames before (hysteresis), so the includer passes the renderer's
 *     decision per instance (ss_drawn) and the bit of the header each submitted triangle came under (supersample).
 *     The reference checks the decision against its own record of the mesh's state before and the depth of the
 *     model origin (double precision, the level-of-detail slack) as a group of two levels, and every triangle of
 *     the instance must carry it; every other triangle carries no bit. An instance the renderer made no batch for
 *     keeps the mesh's state; its triangles must be off the screen.
 * Translucent class (material_pack.h, nm_material_translucent; the cars' smoke): drawn on src->list_translucent
 *   when the includer names one, else skipped like every screen-checker face of a car.
 * The includer provides geometry.h, meshpack.h, material_pack.h, lighting.h,
 * and ENJ_XSCALE. Road-surface class: its depth scale is the frame's (nm_reference_source_t.road_depth). */
#ifndef DAYTONA_NATIVE_REFERENCE_STREAM_H
#define DAYTONA_NATIVE_REFERENCE_STREAM_H
#include <float.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t source, list, visible, clipped; /* clipped (reference): the triangle was cut by a plane */
    uint16_t binding, lit;   /* lit (reference): a lit car face, its luma from the light */
    uint32_t instance, face; /* face: index in the pack's face table */
    uint32_t argb;
    float v[3][5];           /* x y z u v, as written to the PVR */
    /* Reference only. A lit face: the bindings of luma L-1 and L+1 (UINT16_MAX: none, or the binding of L),
     * their colours and each vertex's u, v under them. A clipped triangle: 1/64 texel in u and v. */
    uint32_t polygon;        /* clipped: one number per polygon (the reference's own count, the renderer's), 0 unclipped */
    uint16_t neighbour[2];
    uint32_t neighbour_argb[2];
    float neighbour_uv[2][3][2];
    float texel[2];
    uint8_t door;            /* reference only: a screen-door face (nm_reference_source_t.screen_door): u compared up to a whole number */
    uint8_t supersample;     /* submitted under a header with the TSP super-sample bit (reference: the instance's decision) */
} nm_reference_triangle_t;
/* Tolerances of the fields the renderer computes in SH4ZAM's forms (see above). */
#ifndef NM_REFERENCE_LUMA_STEPS
#define NM_REFERENCE_LUMA_STEPS 1
#endif
#ifndef NM_REFERENCE_CLIP_PIXELS
#define NM_REFERENCE_CLIP_PIXELS (1.0/16)
#endif
#ifndef NM_REFERENCE_CLIP_TEXELS
#define NM_REFERENCE_CLIP_TEXELS (1.0/64)
#endif
#ifndef NM_REFERENCE_CLIP_DEPTH
#define NM_REFERENCE_CLIP_DEPTH 0x1p-14 /* relative */
#endif
#if NM_REFERENCE_LUMA_STEPS != 0 && NM_REFERENCE_LUMA_STEPS != 1
#error "the reference knows the neighbours of a luma one step away"
#endif
/* What the tolerances admitted. Fields x, y (output pixels), u, v (texels),
 * z (relative); histogram buckets of |difference|: 0, below 2^-20, 2^-16,
 * 2^-12, 2^-8, 2^-6 (1/64), 2^-4 (1/16), beyond. */
#define NM_REFERENCE_BUCKETS 8
static struct {
    unsigned long long clipped_polygons, clipped_triangles, clipped_vertices, clipped_identical, rotated_polygons, reduced_polygons, extra_polygons;
    unsigned long long lit_triangles, neighbour_triangles[2], neighbour_faces;
    double max[5], frame_max[5]; /* frame_max: since the includer last cleared it */
    unsigned long long histogram[5][NM_REFERENCE_BUCKETS];
    uint32_t last_instance, last_face; int last_valid;
} nm_reference_tolerance;
static uint32_t nm_reference_polygon_serial = 1;
static inline unsigned nm_reference_bucket(double d) {
    static const double edge[NM_REFERENCE_BUCKETS-1] = {0, 0x1p-20, 0x1p-16, 0x1p-12, 0x1p-8, 0x1p-6, 0x1p-4};
    if (d == 0) return 0;
    unsigned b = 1;
    while (b < NM_REFERENCE_BUCKETS-1 && d >= edge[b]) ++b;
    return b;
}

typedef struct {
    const nm_pack_t *pack;
    const nm_material_pack_t *materials;
    int scenery;            /* baked light per side, screen-checker panels drawn */
    const uint8_t *surface; /* per binding: road-surface depth class, or NULL */
    float road_depth;       /* that class's depth scale this frame (the camera's, render/models/model_renderer.h) */
    float guard;            /* logical pixels added around a whole-screen view; 0: clip at the view */
    unsigned list_opaque, list_cutout;
    /* Level of detail; all NULL for a pack without groups. lod: the pack's
     * table as loaded (after the scenery stage selection). lod_drawn: per
     * group the level the renderer drew this frame, NM_REFERENCE_LOD_UNWALKED
     * if it did not walk the group. lod_state: the reference's own record of
     * each group's level, zero at the start, updated here. */
    const nm_lod_group_t *lod;
    const uint8_t *lod_drawn;
    uint8_t *lod_state;
    /* Catch-fence sweep: per group 1 for a sweep group (nm_reference_sweep_find) when the renderer sweeps, NULL when
     * it does not (make NATIVE_FENCE_SWEEP=0, the default: every group by the rules above); the planes (focus 280) and
     * their count. */
    const uint8_t *sweep_group;
    const float *sweep_plane;
    unsigned sweep_planes;
    /* The list of a car face of the translucent class (material_pack.h, nm_material_translucent: the smoke of the
     * car effects), 0: such faces are screen checker and not drawn. The order of those triangles (back to front by
     * the nearest depth of each face) is the includer's check (nm_reference_translucent_key). */
    unsigned list_translucent;
    /* make NATIVE_SMOKE_MODE=pt-screen (model_submission.h, "Translucent effects"): the translucent class drawn as a
     * screen checker: every vertex of a face at the depth of its nearest corner (1/near_z for a corner before the
     * near plane), u' = u/2 + (x + y)/2 (x, y output pixels) up to a whole number (nm_reference_door). */
    int screen_door;
    /* make NATIVE_SMOKE_TIGHT=1 (model_submission.h, sq_tr_tight_prepare): the pieces of translucent face f of car mesh
     * `mesh`, the renderer's table: count (0: the face reads only holes), piece[p] = {a0, a1, b0, b1} as fractions of
     * the face's s and t ranges, roles (2 bits a corner: s high, t high), or -1 (the face whole). NULL: faces whole.
     * The reference makes each piece itself: its corners are the face's corners interpolated in double precision
     * (view space), projected without the renderer's float steps, and compared as a polygon within the clipper's
     * tolerances. A face whose corners or piece corners are not all inside the view is drawn whole. */
    int (*tight)(unsigned mesh, unsigned f, float piece[][4], unsigned *roles);
    /* World objects super-sampled by instance (see the top); both NULL: no instance is, and no triangle may be.
     * ss_drawn: per instance the renderer's decision this frame (0 plain, 1 super-sampled, NM_REFERENCE_LOD_UNWALKED
     * no batch, or a mesh without a switch depth); ss_state: the reference's own record per mesh, zero at the start. */
    const uint8_t *ss_drawn;
    uint8_t *ss_state;
} nm_reference_source_t;
/* The sort key of a face of the translucent list: the nearest view-space depth of its corners, double precision
 * (model_submission.h sorts the frame's translucent faces by it, largest first). */
static inline double nm_reference_translucent_key(const nm_pack_t *pack, unsigned instance, unsigned face_index) {
    const nm_instance_t *inst = pack->instances + instance;
    const nm_mesh_t *mesh = pack->meshes + inst->mesh;
    const nm_face_t *face = pack->faces + face_index;
    const float *m = inst->matrix;
    double nearest = 1e300;
    for (unsigned v = 0; v < face->count; ++v) {
        const float *p = pack->vertices[mesh->vertex_first + face->indices[v]].position;
        const double z = (double)p[0]*m[2] + (double)p[1]*m[5] + (double)p[2]*m[8] + (double)m[11];
        if (z < nearest) nearest = z;
    }
    return nearest;
}
#define NM_REFERENCE_LOD_UNWALKED 255u
#define NM_REFERENCE_SWEEP 0x80u
#define NM_REFERENCE_SWEEP_MARGIN (1.0/16.0) /* scaled depth units (focus 280): model_submission.h, SQ_SWEEP_MARGIN */
/* Relative and absolute slack of the depth compare: the renderer forms the
 * centre's depth in single precision from world coordinates of a few
 * thousand units. Three orders of magnitude inside the hysteresis band. */
#define NM_REFERENCE_LOD_SLACK 1e-4

/* The level a group goes to from `level` at scaled depth `scaled`: up while
 * the depth exceeds switch_depth*hysteresis, down while it is below
 * switch_depth/hysteresis. */
static inline unsigned nm_reference_lod_level(const nm_lod_group_t *group, unsigned level, double scaled) {
    const double hysteresis = (double)NM_LOD_HYSTERESIS;
    for (;;) {
        if (level + 1 < group->levels && scaled > (double)group->switch_depth[level] * hysteresis) ++level;
        else if (level && scaled < (double)group->switch_depth[level-1] / hysteresis) --level;
        else return level;
    }
}

/* The sweep groups of a pack (see the top): marks them in group[] (one byte a
 * group, zeroed by the caller), writes the planes, returns their count (0: no
 * sweep: no group, different level counts, planes not increasing by more
 * than four margins). The pack's own instance table (before any frame). */
static unsigned nm_reference_sweep_find(const nm_pack_t *pack, uint8_t *group, float plane[NM_LOD_LEVELS - 1]) {
    const unsigned count = nm_pack_lod_count(pack);
    unsigned levels = 0, found = 0;
    int mixed = 0;
    float least[NM_LOD_LEVELS - 1] = {FLT_MAX, FLT_MAX};
    for (unsigned m = 0; m < pack->header->meshes; ++m) {
        const nm_mesh_t *mesh = pack->meshes + m;
        const unsigned first = mesh->padding[4] & 0xffffu, n = mesh->padding[4] >> 16;
        if (!n || !count) continue;
        unsigned users = 0;
        for (unsigned i = 0; i < pack->header->instances; ++i) users += pack->instances[i].mesh == m;
        if (users != 1) continue;
        for (unsigned g = first; g < first + n; ++g) {
            const nm_lod_group_t *lod = pack->lod_groups + g;
            if (lod->levels < 2 || lod->face_count[0] != 1) continue;
            const nm_face_t *base = pack->faces + lod->face_first;
            int ok = base->count == 4;
            unsigned at = lod->face_first;
            for (unsigned l = 0; ok && l < lod->levels; at += lod->face_count[l], ++l) {
                if (lod->face_count[l] > 1) ok = 0;
                else if (lod->face_count[l]) {
                    const nm_face_t *face = pack->faces + at;
                    const uint16_t *h = pack->materials[face->material].header;
                    ok = face->count == 4 && !memcmp(face->indices, base->indices, sizeof(base->indices)) &&
                        ((h[0] >> 13) & 7u) == 3u && (h[1] & 0xfcffu) == 0x8000u;
                }
            }
            if (!ok) continue;
            if (levels && levels != lod->levels) mixed = 1;
            levels = lod->levels;
            for (unsigned k = 0; k + 1 < levels; ++k) if (lod->switch_depth[k] < least[k]) least[k] = lod->switch_depth[k];
            group[g] = 1; ++found;
        }
    }
    int usable = found && !mixed && least[0] > 4.0 * NM_REFERENCE_SWEEP_MARGIN;
    for (unsigned k = 1; usable && k + 1 < levels; ++k) usable = least[k] > least[k - 1] + 4.0 * NM_REFERENCE_SWEEP_MARGIN;
    if (!usable) { for (unsigned g = 0; g < count; ++g) group[g] = 0; return 0; }
    for (unsigned k = 0; k + 1 < levels; ++k) plane[k] = least[k];
    return levels - 1;
}
/* View-space depths (unscaled) of the four corners of a sweep group's panel, double precision. */
static inline void nm_reference_sweep_depths(const nm_pack_t *pack, const nm_instance_t *inst, const nm_lod_group_t *group, double *low, double *high) {
    const nm_mesh_t *mesh = pack->meshes + inst->mesh;
    const nm_face_t *face = pack->faces + group->face_first;
    const float *m = inst->matrix;
    *low = 1e300; *high = -1e300;
    for (unsigned v = 0; v < 4; ++v) {
        const float *p = pack->vertices[mesh->vertex_first + face->indices[v]].position;
        const double z = (double)p[0] * m[2] + (double)p[1] * m[5] + (double)p[2] * m[8] + (double)m[11];
        if (z < *low) *low = z;
        if (z > *high) *high = z;
    }
}
/* The code (or level) of a sweep group for this instance: the renderer's, checked (-1 with a message when the corner
 * depths do not allow it), or the reference's own for a group the renderer did not walk. */
static int nm_reference_sweep_select(const nm_reference_source_t *src, unsigned source, unsigned instance, unsigned index) {
    const nm_lod_group_t *group = src->lod + index;
    const nm_instance_t *inst = src->pack->instances + instance;
    const double scale = (double)NM_LOD_REFERENCE_FOCUS / (double)inst->focus[1], margin = NM_REFERENCE_SWEEP_MARGIN;
    double low, high;
    nm_reference_sweep_depths(src->pack, inst, group, &low, &high);
    low *= scale; high *= scale;
    const unsigned drawn = src->lod_drawn[index], planes = src->sweep_planes;
    if (drawn == NM_REFERENCE_LOD_UNWALKED) {
        unsigned first = 0, last = 0;
        for (unsigned k = 0; k < planes; ++k) {
            if (high <= src->sweep_plane[k] + margin) continue;
            ++last; first += low >= src->sweep_plane[k] - margin;
        }
        return first == last ? (int)first : (int)(NM_REFERENCE_SWEEP | first | last << 2);
    }
    const unsigned first = drawn >= NM_REFERENCE_SWEEP ? drawn & 3u : drawn, last = drawn >= NM_REFERENCE_SWEEP ? (drawn >> 2) & 3u : drawn;
    const double slack = (fabs(low) > fabs(high) ? fabs(low) : fabs(high)) * NM_REFERENCE_LOD_SLACK + 1e-2;
    int ok = last <= planes && first <= last && (drawn < NM_REFERENCE_SWEEP ? drawn < group->levels : first < last && drawn < 0x90u);
    for (unsigned k = 0; ok && k < planes; ++k) {
        const double p = src->sweep_plane[k];
        if (k >= last) ok = high <= p + margin + slack;                                    /* near */
        else if (k < first) ok = high > p + margin - slack && low >= p - margin - slack;  /* beyond */
        else ok = high > p + margin - slack && low < p - margin + slack;                  /* straddling */
    }
    if (!ok) {
        printf("reference mismatch: source %u instance %u fence sweep group %u drawn as %s %u..%u; its corners lie at scaled depths "
               "%.6g..%.6g, planes %.6g %.6g (margin %g)\n", source, instance, index, drawn >= NM_REFERENCE_SWEEP ? "bands" : "level",
               first, last, low, high, planes ? (double)src->sweep_plane[0] : 0.0, planes > 1 ? (double)src->sweep_plane[1] : 0.0, margin);
        return -1;
    }
    src->lod_state[index] = (uint8_t)drawn;
    return (int)drawn;
}

/* The level the reference renders group `index` at for this instance, or -1
 * (message printed): the renderer's level is not what the level before and
 * the depth allow. */
static int nm_reference_lod_select(const nm_reference_source_t *src, unsigned source, unsigned instance, unsigned index) {
    if (src->sweep_group && src->sweep_group[index]) return nm_reference_sweep_select(src, source, instance, index);
    const nm_lod_group_t *group = src->lod + index;
    const nm_instance_t *inst = src->pack->instances + instance;
    const float *m = inst->matrix;
    const unsigned before = src->lod_state[index], drawn = src->lod_drawn[index];
    if (drawn == NM_REFERENCE_LOD_UNWALKED) return (int)before;
    const double z = (double)group->centre[0]*m[2] + (double)group->centre[1]*m[5] + (double)group->centre[2]*m[8] + (double)m[11];
    const double scaled = z * (double)NM_LOD_REFERENCE_FOCUS / (double)inst->focus[1];
    const double slack = fabs(scaled) * NM_REFERENCE_LOD_SLACK + 1e-2;
    const unsigned low = nm_reference_lod_level(group, before, scaled - slack), high = nm_reference_lod_level(group, before, scaled + slack);
    /* In the band between two switch depths both neighbours are allowed only
     * through the level before; outside it the level is determined. */
    if (drawn >= group->levels || (drawn != low && drawn != high)) {
        printf("reference mismatch: source %u instance %u level-of-detail group %u drawn at level %u; from level %u at scaled depth %.6g "
               "(switch depths %.6g %.6g, hysteresis %g) the reference allows level %u%s\n", source, instance, index, drawn, before, scaled,
               (double)group->switch_depth[0], (double)group->switch_depth[1], (double)NM_LOD_HYSTERESIS, low, low != high ? " or the next" : "");
        return -1;
    }
    /* Consistent with the thresholds within the hysteresis band, whatever the history. */
    if ((drawn && scaled + slack < (double)group->switch_depth[drawn-1] / (double)NM_LOD_HYSTERESIS) ||
        (drawn + 1 < group->levels && scaled - slack > (double)group->switch_depth[drawn] * (double)NM_LOD_HYSTERESIS)) {
        printf("reference mismatch: source %u instance %u level-of-detail group %u at level %u outside its hysteresis band (scaled depth %.6g)\n",
               source, instance, index, drawn, scaled);
        return -1;
    }
    src->lod_state[index] = (uint8_t)drawn;
    return (int)drawn;
}

/* Whether the reference renders instance `instance` super-sampled (0 or 1), or -1 (message printed): the renderer's
 * decision is not what the mesh's state before and the depth of the model origin allow. */
static int nm_reference_ss_select(const nm_reference_source_t *src, unsigned source, unsigned instance, double depth) {
    const nm_instance_t *inst = src->pack->instances + instance;
    const unsigned before = src->ss_state[inst->mesh], drawn = src->ss_drawn[instance];
    if (drawn == NM_REFERENCE_LOD_UNWALKED) return (int)before;
    const double scaled = (double)inst->matrix[11] * (double)NM_LOD_REFERENCE_FOCUS / (double)inst->focus[1];
    const double slack = fabs(scaled) * NM_REFERENCE_LOD_SLACK + 1e-2, hysteresis = (double)NM_LOD_HYSTERESIS;
    /* Two levels: on beyond depth*hysteresis, off below depth/hysteresis, else as before. */
    const unsigned low = before ? !(scaled - slack < depth / hysteresis) : scaled - slack > depth * hysteresis;
    const unsigned high = before ? !(scaled + slack < depth / hysteresis) : scaled + slack > depth * hysteresis;
    if (drawn > 1 || (drawn != low && drawn != high) ||
        (drawn && scaled + slack < depth / hysteresis) || (!drawn && scaled - slack > depth * hysteresis)) {
        printf("reference mismatch: source %u instance %u (mesh %u) drawn %s; from %s at scaled depth %.6g (switch depth %.6g, "
               "hysteresis %g) the reference allows %s%s\n", source, instance, (unsigned)inst->mesh, drawn ? "super-sampled" : "plain",
               before ? "super-sampled" : "plain", scaled, depth, hysteresis, low ? "super-sampled" : "plain", low != high ? " or the other" : "");
        return -1;
    }
    src->ss_state[inst->mesh] = (uint8_t)drawn;
    return (int)drawn;
}

static inline nm_view_t nm_reference_view(const nm_pack_t *pack, const nm_instance_t *inst) {
    nm_view_t view;
    view.center[0] = (float)(pack->header->crtc[0] + inst->center[0]);
    view.center[1] = (float)(384 - inst->center[1] + pack->header->crtc[1]);
    view.bounds[0] = fmaxf((float)(inst->viewport[0] + pack->header->render[0]), 0);
    view.bounds[1] = fminf((float)(inst->viewport[2] + pack->header->render[0] + 1), 496);
    view.bounds[2] = fmaxf((float)(384 - inst->viewport[3] + pack->header->render[1]), 0);
    view.bounds[3] = fminf((float)(384 - inst->viewport[1] + pack->header->render[1] + 1), 384);
    view.near_z = 0.01f;
    return view;
}

/* Does the triangle share a point with the rectangle [0,w] x [0,h]? Exact
 * separating-axis test in double precision; touching counts. */
static inline int nm_reference_on_screen(const float v[3][5], double w, double h) {
    double lx = v[0][0], hx = lx, ly = v[0][1], hy = ly;
    for (unsigned i = 1; i < 3; ++i) {
        if (v[i][0] < lx) lx = v[i][0];
        if (v[i][0] > hx) hx = v[i][0];
        if (v[i][1] < ly) ly = v[i][1];
        if (v[i][1] > hy) hy = v[i][1];
    }
    if (hx < 0 || lx > w || hy < 0 || ly > h) return 0;
    const double corner[4][2] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
    for (unsigned e = 0; e < 3; ++e) {
        const unsigned n = (e + 1) % 3, o = (e + 2) % 3;
        const double ex = (double)v[n][0] - v[e][0], ey = (double)v[n][1] - v[e][1];
        const double inside = ex * ((double)v[o][1] - v[e][1]) - ey * ((double)v[o][0] - v[e][0]);
        if (inside == 0) continue; /* degenerate: the box test decided */
        unsigned beyond = 0;
        for (unsigned c = 0; c < 4; ++c) {
            const double side = ex * (corner[c][1] - v[e][1]) - ey * (corner[c][0] - v[e][0]);
            beyond += inside > 0 ? side < 0 : side > 0;
        }
        if (beyond == 4) return 0;
    }
    return 1;
}

/* u, v of one vertex under `binding`, as the renderer must write them: an
 * unclipped triangle from the pack's integer texel coordinates, a clipped one
 * from the projected u/w, v/w. */
static inline void nm_reference_uv(const nm_reference_source_t *src, const nm_material_binding_t *binding, const nm_vertex_t *vertex,
        const nm_screen_vertex_t *p, int clipped, float out[2]) {
    if (!clipped) {
        /* An unclipped triangle: integer texel arithmetic and a
         * power-of-two division, no projection round trip. */
        for (unsigned a = 0; a < 2; ++a) {
            unsigned extent = 32u << ((binding->header[0] >> (a * 3)) & 7), origin = 0;
            if (binding->resource != NM_MATERIAL_SOLID) {
                const nm_texture_record_t *texture = src->materials->textures + binding->resource;
                extent = a ? texture->height : texture->width;
                if (binding->color != UINT32_MAX) origin = (binding->color >> (a * 16)) & 65535;
            }
            out[a] = (float)((int)(a ? vertex->v : vertex->u) - (int)(origin * 8)) / (float)(extent * 8);
        }
    } else {
        const nm_material_uv_t uv = nm_material_uv(src->materials, binding);
        out[0] = (p->uq / p->q - uv.origin[0]) * uv.scale[0];
        out[1] = (p->vq / p->q - uv.origin[1]) * uv.scale[1];
    }
}

/* The screen-door form of a triangle of a translucent-class face (screen_door): depth q (1/w of the face's nearest
 * corner) times the depth factors, u halved plus half of x + y, less the whole number nearest half of x + y of the
 * first vertex (the renderer subtracts another whole number: u is compared up to one). */
static inline void nm_reference_door(nm_reference_triangle_t *tri, float q, float depth) {
    const float base = floorf(0.5f * (tri->v[0][0] + tri->v[0][1]));
    for (unsigned v = 0; v < 3; ++v) {
        tri->v[v][2] = q * depth;
        tri->v[v][3] = tri->v[v][3] * 0.5f + 0.5f * (tri->v[v][0] + tri->v[v][1]) - base;
        for (unsigned k = 0; k < 2; ++k)
            tri->neighbour_uv[k][v][0] = tri->neighbour_uv[k][v][0] * 0.5f + 0.5f * (tri->v[v][0] + tri->v[v][1]) - base;
    }
    /* The tolerance: 1/64 texel of the double-width texture, or four float steps of the largest u' (a face cut
     * far out in the guard band keeps u' near half its x + y there; renderer and reference round it differently). */
    float largest = 0;
    for (unsigned v = 0; v < 3; ++v) {
        if (fabsf(tri->v[v][3]) > largest) largest = fabsf(tri->v[v][3]);
        /* The renderer adds (x + y)/2 before it subtracts its own whole number: its rounding is that of (x + y)/2. */
        if (0.5f * fabsf(tri->v[v][0] + tri->v[v][1]) > largest) largest = 0.5f * fabsf(tri->v[v][0] + tri->v[v][1]);
    }
    tri->texel[0] = fmaxf(tri->texel[0] * 0.5f, 4.0f * FLT_EPSILON * largest);
    tri->door = 1;
}
/* |a - b| up to a whole number. */
static inline double nm_reference_wrapped(double d) { return fabs(d - floor(d + 0.5)); }

/* One triangle of a sweep band polygon (double precision vertices x, y focused, z, u, v): projected onto the view in
 * double precision, written as the renderer writes it. */
static inline void nm_reference_band_vertex(const nm_view_t *view, const double p[5], const nm_material_uv_t *uv, float depth, float out[5]) {
    const double q = 1.0 / p[2];
    double x = view->center[0] + p[0] * q, y = view->center[1] - p[1] * q;
    if (x < view->bounds[0]) x = view->bounds[0];
    if (x > view->bounds[1]) x = view->bounds[1];
    if (y < view->bounds[2]) y = view->bounds[2];
    if (y > view->bounds[3]) y = view->bounds[3];
    out[0] = (float)(x * (640.0 / 496.0) * ENJ_XSCALE); out[1] = (float)(y * (480.0 / 384.0)); out[2] = (float)(q * depth);
    out[3] = (float)((p[3] - uv->origin[0]) * uv->scale[0]); out[4] = (float)((p[4] - uv->origin[1]) * uv->scale[1]);
}
/* The polygon p[0..n) cut at view depth `plane`, keeping the side beyond it (beyond) or in front of it. */
static unsigned nm_reference_band_cut(double p[][5], unsigned n, double plane, int beyond, double out[][5]) {
    unsigned used = 0;
    for (unsigned i = 0; i < n; ++i) {
        const unsigned j = (i + 1) % n;
        const double di = beyond ? p[i][2] - plane : plane - p[i][2], dj = beyond ? p[j][2] - plane : plane - p[j][2];
        if (di >= 0) memcpy(out[used++], p[i], sizeof(p[0]));
        if ((di > 0 && dj < 0) || (di < 0 && dj > 0)) {
            const double t = di / (di - dj);
            for (unsigned a = 0; a < 5; ++a) out[used][a] = p[i][a] + t * (p[j][a] - p[i][a]);
            out[used++][2] = plane;
        }
    }
    return used;
}

/* The triangles of one face of an instance, appended at *count. 0, or -1
 * (message printed). band: NULL, or the view-space depths a sweep band lies
 * between (negative: no such bound); the face is then drawn clipped to it. */
static int nm_reference_face(const nm_reference_source_t *src, unsigned source, unsigned instance, const nm_view_t *view_in,
        unsigned f, nm_reference_triangle_t *out, unsigned capacity, unsigned *count_io, const double *band) {
    const nm_pack_t *pack = src->pack;
    const nm_instance_t *inst = pack->instances + instance;
    const nm_mesh_t *mesh = pack->meshes + inst->mesh;
    const float *m = inst->matrix;
    const nm_view_t view = *view_in;
    const float screen_w = 640.0f * ENJ_XSCALE, screen_h = 480.0f;
    unsigned count = *count_io;
    {
        const nm_face_t *face = pack->faces + mesh->face_first + f;
        const nm_material_t *material = pack->materials + face->material;
        /* View-space corners (unfocused) and supplied normal. */
        float corner[4][3], normal[3];
        for (unsigned v = 0; v < face->count; ++v) {
            const float *p = pack->vertices[mesh->vertex_first + face->indices[v]].position;
            for (unsigned a = 0; a < 3; ++a) corner[v][a] = p[0]*m[a] + p[1]*m[3+a] + p[2]*m[6+a] + m[9+a];
        }
        for (unsigned a = 0; a < 3; ++a)
            normal[a] = face->normal[0]*m[a] + face->normal[1]*m[3+a] + face->normal[2]*m[6+a];
        float points[3][3];
        memcpy(points, corner, sizeof(points));
        int light = nm_face_lighting(mesh->mode, normal, points, inst->light, material);
        if (light < 0) { printf("reference: invalid light, source %u instance %u face %u\n", source, instance, f); return -1; }
        /* Baked scenery: the pack's luma for the side the view shows. */
        if (src->scenery) light = (light & 256) | (light & 256 ? face->padding : face->reserved);
        if (!(face->attribute & (1u << 17)) && (light & 256)) goto done; /* rear of a one-sided face */
        if (!(face->attribute & (3u << 8))) goto done;
        /* Screen checker on a car: not drawn, but the translucent class (material_pack.h) when the includer names
         * the list it goes to (list_translucent). */
        const int translucent = !src->scenery && src->list_translucent && nm_material_translucent(material->header);
        if ((material->header[0] & 0x8000) && !src->scenery && !translucent) goto done;
        const nm_material_binding_t *binding = nm_material_find(src->materials, material->header, (unsigned)light & 255);
        if (!binding) {
            printf("reference: no binding, source %u instance %u face %u material %u luma %d\n", source, instance, f, face->material, light & 255);
            return -1;
        }
        const unsigned index = (unsigned)(binding - src->materials->bindings);
        /* A lit car face: the bindings one luma step either side (the renderer's light rounds otherwise). */
        const nm_material_binding_t *neighbour[2] = {NULL, NULL};
        if (!src->scenery && NM_REFERENCE_LUMA_STEPS)
            for (unsigned k = 0; k < 2; ++k) {
                const int luma = (light & 255) + (k ? 1 : -1);
                if (luma < 0 || luma > 255) continue;
                neighbour[k] = nm_material_find(src->materials, material->header, (unsigned)luma);
                if (neighbour[k] == binding) neighbour[k] = NULL;
            }
        const unsigned list = translucent ? src->list_translucent :
            ((material->header[0] >> 13) & 3) == 3 ? src->list_cutout : src->list_opaque;
        const nm_material_uv_t uv = nm_material_uv(src->materials, binding);
        const float depth = 1.0f + (float)face->layer * NM_LAYER_DEPTH_STEP;
        /* z is q times one factor, the class's depth times the layer's (the record's depth, model_submission.h): at a
         * scale that is not a power of two (q*layer)*scale rounds otherwise. */
        const float road = src->surface && src->surface[index] ? src->road_depth : 1.0f;
        float door_q = 0;
        if (translucent && src->screen_door) {
            float nearest = corner[0][2];
            for (unsigned v = 1; v < face->count; ++v) if (corner[v][2] < nearest) nearest = corner[v][2];
            door_q = 1.0f / (nearest > view.near_z ? nearest : view.near_z);
        }
        if (band) {
            /* A sweep band: the face's polygon in double precision, cut at the band's planes. */
            double poly[2][NM_CLIP_VERTICES + 2][5];
            unsigned n = face->count, at = 0;
            for (unsigned v = 0; v < n; ++v) {
                const nm_vertex_t *vertex = pack->vertices + mesh->vertex_first + face->indices[v];
                const float *p = vertex->position;
                double c[3];
                for (unsigned a = 0; a < 3; ++a) c[a] = (double)p[0]*m[a] + (double)p[1]*m[3+a] + (double)p[2]*m[6+a] + (double)m[9+a];
                poly[0][v][0] = c[0] * inst->focus[0]; poly[0][v][1] = c[1] * inst->focus[1]; poly[0][v][2] = c[2];
                poly[0][v][3] = vertex->u; poly[0][v][4] = vertex->v;
            }
            if (band[0] >= 0) { n = nm_reference_band_cut(poly[at], n, band[0], 1, poly[at ^ 1]); at ^= 1; }
            if (band[1] >= 0 && n >= 3) { n = nm_reference_band_cut(poly[at], n, band[1], 0, poly[at ^ 1]); at ^= 1; }
            if (n < 3) goto done;
            double (*q)[5] = poly[at];
            int any = 0;
            for (unsigned i = 0; i < n; ++i) {
                const double z = q[i][2];
                any |= z < view.near_z || q[i][0] + (view.center[0] - view.bounds[0]) * z < 0 || (view.bounds[1] - view.center[0]) * z - q[i][0] < 0 ||
                    (view.center[1] - view.bounds[2]) * z - q[i][1] < 0 || q[i][1] + (view.bounds[3] - view.center[1]) * z < 0;
            }
            if (!any) ++nm_reference_polygon_serial;
            for (unsigned t = 1; t + 1 < n; ++t) {
                const unsigned corners[3] = {0, t, t + 1};
                nm_screen_vertex_t projected[NM_CLIP_VERTICES];
                unsigned vertices = 3;
                if (any) {
                    nm_view_vertex_t input[3];
                    for (unsigned v = 0; v < 3; ++v) {
                        const double *c = q[corners[v]];
                        input[v] = (nm_view_vertex_t){(float)c[0], (float)c[1], (float)c[2], (float)c[3], (float)c[4]};
                    }
                    vertices = nm_project_triangle(&view, input, projected);
                    if (vertices >= 3) ++nm_reference_polygon_serial;
                }
                for (unsigned k = 1; k + 1 < vertices; ++k) {
                    if (count == capacity) { puts("reference: triangle capacity"); return -1; }
                    nm_reference_triangle_t *tri = out + count++;
                    memset(tri, 0, sizeof(*tri));
                    tri->source = (uint8_t)source; tri->list = (uint8_t)list; tri->binding = (uint16_t)index;
                    tri->instance = instance; tri->face = mesh->face_first + f;
                    tri->argb = nm_material_argb(src->materials, binding);
                    tri->clipped = 1; tri->lit = 0; tri->polygon = nm_reference_polygon_serial;
                    tri->texel[0] = (float)(NM_REFERENCE_CLIP_TEXELS * 8.0 * uv.scale[0]);
                    tri->texel[1] = (float)(NM_REFERENCE_CLIP_TEXELS * 8.0 * uv.scale[1]);
                    tri->neighbour[0] = tri->neighbour[1] = UINT16_MAX;
                    const unsigned fan[3] = {0, k, k + 1};
                    for (unsigned v = 0; v < 3; ++v) {
                        if (!any) nm_reference_band_vertex(&view, q[corners[fan[v]]], &uv, depth * road, tri->v[v]);
                        else {
                            const nm_screen_vertex_t p = projected[fan[v]];
                            tri->v[v][0] = p.x * (640.0f / 496.0f) * ENJ_XSCALE; tri->v[v][1] = p.y * (480.0f / 384.0f);
                            tri->v[v][2] = p.q * (depth * road);
                            nm_reference_uv(src, binding, NULL, &p, 1, tri->v[v] + 3);
                        }
                    }
                    tri->visible = (uint8_t)nm_reference_on_screen(tri->v, screen_w, screen_h);
                }
            }
            goto done;
        }
        if (door_q > 0 && src->tight && face->count == 4) {
            float piece[4][4];
            unsigned roles = 0;
            const int pieces = src->tight(inst->mesh, f, piece, &roles);
            if (pieces >= 0) {
                /* View-space corners (focused) in double, by role: s low/high (bit 0), t low/high (bit 1). */
                double c[4][5];
                int inside = 1;
                for (unsigned v = 0; v < 4; ++v) {
                    const nm_vertex_t *vertex = pack->vertices + mesh->vertex_first + face->indices[v];
                    const float *p = vertex->position;
                    double *o = c[(roles >> (2 * v)) & 3u];
                    for (unsigned a = 0; a < 3; ++a) o[a] = (double)p[0]*m[a] + (double)p[1]*m[3+a] + (double)p[2]*m[6+a] + (double)m[9+a];
                    o[0] *= inst->focus[0]; o[1] *= inst->focus[1]; o[3] = vertex->u; o[4] = vertex->v;
                    const nm_view_vertex_t input = {corner[v][0] * inst->focus[0], corner[v][1] * inst->focus[1], corner[v][2], vertex->u, vertex->v};
                    if (nm_geometry_classify(&view, &input)) inside = 0;
                }
                double q[4][4][5];
                for (int p = 0; p < pieces && inside; ++p) for (unsigned v = 0; v < 4; ++v) {
                    const unsigned r = (roles >> (2 * v)) & 3u;
                    const double a = piece[p][r & 1u], b = piece[p][2 + (r >> 1)];
                    for (unsigned k = 0; k < 5; ++k) q[p][v][k] = c[0][k] + a * (c[1][k] - c[0][k]) + b * (c[2][k] - c[0][k]);
                    const nm_view_vertex_t input = {(float)q[p][v][0], (float)q[p][v][1], (float)q[p][v][2], (float)q[p][v][3], (float)q[p][v][4]};
                    if (nm_geometry_classify(&view, &input)) inside = 0;
                }
                if (inside) {
                    for (int p = 0; p < pieces; ++p) {
                        ++nm_reference_polygon_serial;
                        for (unsigned t = 1; t + 1 < 4; ++t) {
                            if (count == capacity) { puts("reference: triangle capacity"); return -1; }
                            nm_reference_triangle_t *tri = out + count++;
                            memset(tri, 0, sizeof(*tri));
                            tri->source = (uint8_t)source; tri->list = (uint8_t)list; tri->binding = (uint16_t)index;
                            tri->instance = instance; tri->face = mesh->face_first + f;
                            tri->argb = nm_material_argb(src->materials, binding);
                            tri->clipped = 1; tri->lit = !src->scenery; tri->polygon = nm_reference_polygon_serial;
                            tri->texel[0] = (float)(NM_REFERENCE_CLIP_TEXELS * 8.0 * uv.scale[0]);
                            tri->texel[1] = (float)(NM_REFERENCE_CLIP_TEXELS * 8.0 * uv.scale[1]);
                            for (unsigned k = 0; k < 2; ++k) {
                                tri->neighbour[k] = neighbour[k] ? (uint16_t)(neighbour[k] - src->materials->bindings) : UINT16_MAX;
                                tri->neighbour_argb[k] = neighbour[k] ? nm_material_argb(src->materials, neighbour[k]) : 0;
                            }
                            const unsigned fan[3] = {0, t, t + 1};
                            for (unsigned v = 0; v < 3; ++v) {
                                nm_reference_band_vertex(&view, q[p][fan[v]], &uv, depth * road, tri->v[v]);
                                for (unsigned k = 0; k < 2; ++k) if (neighbour[k]) {
                                    const nm_material_uv_t nuv = nm_material_uv(src->materials, neighbour[k]);
                                    float n5[5];
                                    nm_reference_band_vertex(&view, q[p][fan[v]], &nuv, depth * road, n5);
                                    tri->neighbour_uv[k][v][0] = n5[3]; tri->neighbour_uv[k][v][1] = n5[4];
                                }
                            }
                            nm_reference_door(tri, door_q, depth * road);
                            tri->visible = (uint8_t)nm_reference_on_screen(tri->v, screen_w, screen_h);
                        }
                    }
                    goto done;
                }
            }
        }
        for (unsigned t = 1; t + 1 < face->count; ++t) {
            const unsigned corners[3] = {0, t, t + 1};
            nm_view_vertex_t input[3];
            unsigned outside = 0;
            for (unsigned v = 0; v < 3; ++v) {
                const nm_vertex_t *vertex = pack->vertices + mesh->vertex_first + face->indices[corners[v]];
                if (uv.cropped && !nm_material_uv_contains(&uv, vertex->u, vertex->v)) {
                    printf("reference: UV outside the crop, source %u instance %u face %u\n", source, instance, f);
                    return -1;
                }
                input[v] = (nm_view_vertex_t){corner[corners[v]][0] * inst->focus[0], corner[corners[v]][1] * inst->focus[1],
                    corner[corners[v]][2], vertex->u, vertex->v};
                outside |= nm_geometry_classify(&view, &input[v]);
            }
            nm_screen_vertex_t projected[NM_CLIP_VERTICES];
            const unsigned vertices = nm_project_triangle(&view, input, projected);
            if (outside) ++nm_reference_polygon_serial;
            for (unsigned n = 1; n + 1 < vertices; ++n) {
                if (count == capacity) { puts("reference: triangle capacity"); return -1; }
                nm_reference_triangle_t *tri = out + count++;
                memset(tri, 0, sizeof(*tri));
                tri->source = (uint8_t)source; tri->list = (uint8_t)list; tri->binding = (uint16_t)index;
                tri->instance = instance; tri->face = mesh->face_first + f;
                tri->argb = nm_material_argb(src->materials, binding);
                tri->clipped = outside != 0; tri->lit = !src->scenery;
                tri->polygon = outside ? nm_reference_polygon_serial : 0;
                tri->texel[0] = (float)(NM_REFERENCE_CLIP_TEXELS * 8.0 * uv.scale[0]);
                tri->texel[1] = (float)(NM_REFERENCE_CLIP_TEXELS * 8.0 * uv.scale[1]);
                for (unsigned k = 0; k < 2; ++k) {
                    tri->neighbour[k] = neighbour[k] ? (uint16_t)(neighbour[k] - src->materials->bindings) : UINT16_MAX;
                    tri->neighbour_argb[k] = neighbour[k] ? nm_material_argb(src->materials, neighbour[k]) : 0;
                }
                const unsigned fan[3] = {0, n, n + 1};
                for (unsigned v = 0; v < 3; ++v) {
                    const nm_screen_vertex_t p = projected[fan[v]];
                    tri->v[v][0] = p.x * (640.0f / 496.0f) * ENJ_XSCALE;
                    tri->v[v][1] = p.y * (480.0f / 384.0f);
                    tri->v[v][2] = p.q * (depth * road);
                    /* (A clipped triangle's vertex fan[v] is an output of the clipper, not a corner.) */
                    const nm_vertex_t *vertex = outside ? NULL : pack->vertices + mesh->vertex_first + face->indices[corners[fan[v]]];
                    nm_reference_uv(src, binding, vertex, &p, outside != 0, tri->v[v] + 3);
                    for (unsigned k = 0; k < 2; ++k)
                        if (neighbour[k]) nm_reference_uv(src, neighbour[k], vertex, &p, outside != 0, tri->neighbour_uv[k][v]);
                }
                if (door_q > 0) nm_reference_door(tri, door_q, depth * road);
                tri->visible = (uint8_t)nm_reference_on_screen(tri->v, screen_w, screen_h);
            }
        }
    }
done:
    *count_io = count;
    return 0;
}

/* Triangles of one instance, both lists, in face order. Returns the count,
 * or -1 (message printed): invalid arithmetic, a face without a binding or
 * with UVs outside its crop, or capacity. */
static int nm_reference_instance(const nm_reference_source_t *src, unsigned source, unsigned instance,
        nm_reference_triangle_t *out, unsigned capacity) {
    const nm_pack_t *pack = src->pack;
    const nm_instance_t *inst = pack->instances + instance;
    const nm_mesh_t *mesh = pack->meshes + inst->mesh;
    nm_view_t view = nm_reference_view(pack, inst);
    if (!nm_view_valid(&view)) return 0;
    if (src->guard > 0 && view.bounds[0] == 0 && view.bounds[1] == 496 && view.bounds[2] == 0 && view.bounds[3] == 384) {
        view.bounds[0] -= src->guard; view.bounds[1] += src->guard;
        view.bounds[2] -= src->guard; view.bounds[3] += src->guard;
    }
    unsigned count = 0;
    int supersample = 0;
    if (src->ss_state && mesh->face_count) {
        const float depth = nm_material_supersample_depth(pack->materials + pack->faces[mesh->face_first].material);
        if (depth > 0 && (supersample = nm_reference_ss_select(src, source, instance, (double)depth)) < 0) return -1;
    }
    /* Level-of-detail groups of the mesh, in face order: of each, only the
     * range of the level drawn is rendered. */
    unsigned lod_at = src->lod ? mesh->padding[4] & 0xffffu : 0;
    const unsigned lod_end = src->lod ? lod_at + (mesh->padding[4] >> 16) : 0;
    for (unsigned f = 0; f < mesh->face_count; ++f) {
        while (lod_at < lod_end && src->lod[lod_at].face_first - mesh->face_first == f) {
            const nm_lod_group_t *group = src->lod + lod_at;
            const int level = nm_reference_lod_select(src, source, instance, lod_at);
            if (level < 0) return -1;
            unsigned first = f, total = 0;
            for (unsigned l = 0; l < NM_LOD_LEVELS; ++l) {
                if (l < (unsigned)level) first += group->face_count[l];
                total += group->face_count[l];
            }
            /* The faces of the level drawn (none: that level draws nothing),
             * then on behind the group, where another group may start. */
            if (level >= (int)NM_REFERENCE_SWEEP) {
                /* A sweep group across plane(s): each level n0..n1 with a face, clipped to its band. */
                const unsigned near = (unsigned)level & 3u, far = ((unsigned)level >> 2) & 3u;
                const double unscale = (double)inst->focus[1] / (double)NM_LOD_REFERENCE_FOCUS;
                unsigned at = f;
                for (unsigned l = 0; l <= far; at += group->face_count[l], ++l) {
                    if (l < near || !group->face_count[l]) continue;
                    const double band[2] = {l > near ? src->sweep_plane[l - 1] * unscale : -1.0, l < far ? src->sweep_plane[l] * unscale : -1.0};
                    if (nm_reference_face(src, source, instance, &view, at, out, capacity, &count, band) < 0) return -1;
                }
            } else
            for (unsigned g = first; g < first + group->face_count[level]; ++g)
                if (nm_reference_face(src, source, instance, &view, g, out, capacity, &count, NULL) < 0) return -1;
            ++lod_at;
            f += total;
        }
        if (f >= mesh->face_count) break;
        if (nm_reference_face(src, source, instance, &view, f, out, capacity, &count, NULL) < 0) return -1;
    }
    for (unsigned t = 0; t < count; ++t) out[t].supersample = (uint8_t)supersample;
    return (int)count;
}

/* The binding a submitted triangle may have against reference triangle a:
 * 1 (with *choice -1: a's own, 0 / 1: that of luma L-1 / L+1 of a lit face),
 * or 0. The colour must be that binding's. */
static inline int nm_reference_binding(const nm_reference_triangle_t *a, const nm_reference_triangle_t *b, int *choice) {
    if (a->source != b->source || a->list != b->list || a->face != b->face || a->supersample != b->supersample) return 0;
    *choice = -1;
    if (a->binding == b->binding) return a->argb == b->argb;
    if (!a->lit) return 0;
    for (int k = 0; k < 2; ++k)
        if (a->neighbour[k] == b->binding) { *choice = k; return a->neighbour_argb[k] == b->argb; }
    return 0;
}
/* 0, or 1 + the vertex order (0..5) that matches an unclipped triangle bit for bit. */
static const uint8_t nm_reference_order[6][3] = {{0,1,2},{1,2,0},{2,0,1},{0,2,1},{2,1,0},{1,0,2}};
static inline int nm_reference_match(const nm_reference_triangle_t *a, const nm_reference_triangle_t *b, int *choice) {
    if (!nm_reference_binding(a, b, choice)) return 0;
    if (a->door) {
        /* Screen door: x, y, z, v bit for bit, u up to a whole number and within 1/64 texel. */
        for (unsigned o = 0; o < 6; ++o) {
            int same = 1;
            for (unsigned r = 0; r < 3 && same; ++r) {
                const float *e = a->v[r], *g = b->v[nm_reference_order[o][r]];
                same = e[0] == g[0] && e[1] == g[1] && e[2] == g[2] && e[4] == g[4] &&
                    nm_reference_wrapped((double)e[3] - g[3]) <= a->texel[0];
            }
            if (same) return 1 + (int)o;
        }
        return 0;
    }
    float expected[3][5];
    memcpy(expected, a->v, sizeof(expected));
    if (*choice >= 0) for (unsigned r = 0; r < 3; ++r) { expected[r][3] = a->neighbour_uv[*choice][r][0]; expected[r][4] = a->neighbour_uv[*choice][r][1]; }
    for (unsigned o = 0; o < 6; ++o)
        if (!memcmp(expected[0], b->v[nm_reference_order[o][0]], sizeof(expected[0])) && !memcmp(expected[1], b->v[nm_reference_order[o][1]], sizeof(expected[0])) &&
            !memcmp(expected[2], b->v[nm_reference_order[o][2]], sizeof(expected[0]))) return 1 + (int)o;
    return 0;
}
static inline int nm_reference_same(const nm_reference_triangle_t *a, const nm_reference_triangle_t *b) {
    int k;
    return nm_reference_match(a, b, &k) != 0;
}
static inline void nm_reference_record_binding(const nm_reference_triangle_t *a, int k, unsigned triangles) {
    if (a->lit) nm_reference_tolerance.lit_triangles += triangles;
    if (k < 0) return;
    nm_reference_tolerance.neighbour_triangles[k] += triangles;
    if (!nm_reference_tolerance.last_valid || nm_reference_tolerance.last_instance != a->instance || nm_reference_tolerance.last_face != a->face)
        ++nm_reference_tolerance.neighbour_faces;
    nm_reference_tolerance.last_valid = 1; nm_reference_tolerance.last_instance = a->instance; nm_reference_tolerance.last_face = a->face;
}

/* A clipped triangle is a polygon, written as the fan (P0, Pn, Pn+1) of its
 * vertices. The renderer and the reference clip in different plane orders
 * and precisions, so their polygons have the same vertices up to the first
 * one and rounding; a vertex within the tolerances of the one before it (a
 * cut that missed a corner by a rounding error) ends a zero-length edge and
 * may be missing from the other (nm_reference_polygon_match). Up to
 * NM_CLIP_VERTICES+1 vertices; 0 if the triangles are not one fan. */
static inline unsigned nm_reference_polygon(const nm_reference_triangle_t *t, unsigned n, int k, float out[][5]) {
    /* The first triangle may start at any of its corners: a strip's first triangle (1,2,0) of a quad, as a piece of tight
     * smoke geometry is written (model_submission.h, sq_tr_emit_tight), is the fan (0,1,2),(0,2,3) rotated. */
    for (unsigned rotation = 0; rotation < 3; ++rotation) {
        unsigned count = 0;
        int fan = 1;
        for (unsigned j = 0; j < n && fan; ++j) {
            const float *first = t[0].v[rotation], *previous = j == 1 ? t[0].v[(rotation + 2) % 3] : j ? t[j-1].v[2] : NULL;
            if (j && (memcmp(t[j].v[0], first, sizeof(t[0].v[0])) || memcmp(t[j].v[1], previous, sizeof(t[0].v[0])))) { fan = 0; break; }
            for (unsigned r = j ? 2 : 0; r < 3; ++r) {
                if (count == NM_CLIP_VERTICES + 1) return 0;
                const unsigned c = j ? r : (r + rotation) % 3;
                memcpy(out[count], t[j].v[c], sizeof(out[0]));
                if (k >= 0) { out[count][3] = t[j].neighbour_uv[k][c][0]; out[count][4] = t[j].neighbour_uv[k][c][1]; }
                ++count;
            }
        }
        if (fan) return count;
    }
    return 0;
}
/* |a-b| per field in the units of the tolerances: output pixels, texels, depth relative to the reference's. */
static inline void nm_reference_difference(const float a[5], const float b[5], const float texel[2], double d[5], int door) {
    d[0] = fabs((double)a[0] - b[0]); d[1] = fabs((double)a[1] - b[1]);
    /* door: u up to a whole number (nm_reference_door), and as the texture coordinate it stands for, u' - (x + y)/2: a
     * vertex placed within the pixel tolerance carries its own (x + y)/2, which is not a texture error. */
    d[2] = texel[0] > 0 ? (door ? nm_reference_wrapped(((double)a[3] - 0.5 * ((double)a[0] + a[1])) - ((double)b[3] - 0.5 * ((double)b[0] + b[1]))) :
        fabs((double)a[3] - b[3])) * (NM_REFERENCE_CLIP_TEXELS / texel[0]) : 0;
    d[3] = texel[1] > 0 ? fabs((double)a[4] - b[4]) * (NM_REFERENCE_CLIP_TEXELS / texel[1]) : 0;
    d[4] = fabs((double)a[2] - b[2]) / fabs((double)a[2]);
}
static inline int nm_reference_close(const float a[5], const float b[5], const float texel[2], int door) {
    double d[5];
    nm_reference_difference(a, b, texel, d, door);
    return d[0] <= NM_REFERENCE_CLIP_PIXELS && d[1] <= NM_REFERENCE_CLIP_PIXELS && d[2] <= NM_REFERENCE_CLIP_TEXELS &&
        d[3] <= NM_REFERENCE_CLIP_TEXELS && d[4] <= NM_REFERENCE_CLIP_DEPTH;
}
/* Vertex i of polygon p (n vertices) is a zero-length edge's end: within the
 * tolerances of a neighbour. */
static inline int nm_reference_droppable(float p[][5], unsigned n, unsigned i, const float texel[2], int door) {
    return n > 1 && (nm_reference_close(p[i], p[(i + n - 1) % n], texel, door) || nm_reference_close(p[i], p[(i + 1) % n], texel, door));
}
/* Reference polygon r[0..rn) against submitted polygon s[0..sn): 1 when the
 * bindings agree and, from some first vertex of the submitted polygon, the
 * two vertex sequences agree within the tolerances, where either may have a
 * vertex the other lacks if it ends a zero-length edge of its own polygon
 * (recorded); else 0. */
static inline int nm_reference_polygon_match(const nm_reference_triangle_t *r, unsigned rn, const nm_reference_triangle_t *s, unsigned sn) {
    int k;
    for (unsigned j = 0; j < sn; ++j) {
        int kj;
        if (!nm_reference_binding(r, s + j, &kj)) return 0;
        if (!j) k = kj; else if (kj != k) return 0;
    }
    float a[NM_CLIP_VERTICES + 2][5], b[NM_CLIP_VERTICES + 2][5];
    const unsigned na = nm_reference_polygon(r, rn, k, a), nb = nm_reference_polygon(s, sn, -1, b);
    if (!na || !nb) return 0;
    unsigned pair[2 * (NM_CLIP_VERTICES + 2)][2];
    for (unsigned rotation = 0; rotation < nb; ++rotation) {
        unsigned i = 0, j = 0, pairs = 0, dropped = 0;
        while (i < na || j < nb) {
            const unsigned jb = (j + rotation) % nb;
            if (i < na && j < nb && nm_reference_close(a[i], b[jb], r->texel, r->door)) { pair[pairs][0] = i++; pair[pairs++][1] = jb; ++j; }
            else if (i < na && nm_reference_droppable(a, na, i, r->texel, r->door)) { ++i; ++dropped; }
            else if (j < nb && nm_reference_droppable(b, nb, jb, r->texel, r->door)) { ++j; ++dropped; }
            else break;
        }
        if (i < na || j < nb) continue;
        nm_reference_record_binding(r, k, sn);
        ++nm_reference_tolerance.clipped_polygons; nm_reference_tolerance.clipped_triangles += sn;
        nm_reference_tolerance.rotated_polygons += rotation != 0;
        nm_reference_tolerance.reduced_polygons += dropped != 0;
        for (unsigned q = 0; q < pairs; ++q) {
            double d[5];
            nm_reference_difference(a[pair[q][0]], b[pair[q][1]], r->texel, d, r->door);
            ++nm_reference_tolerance.clipped_vertices;
            nm_reference_tolerance.clipped_identical += d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0 && d[4] == 0;
            for (unsigned f = 0; f < 5; ++f) {
                if (d[f] > nm_reference_tolerance.max[f]) nm_reference_tolerance.max[f] = d[f];
                if (d[f] > nm_reference_tolerance.frame_max[f]) nm_reference_tolerance.frame_max[f] = d[f];
                ++nm_reference_tolerance.histogram[f][nm_reference_bucket(d[f])];
            }
        }
        return 1;
    }
    return 0;
}
static inline void nm_reference_print(const char *who, const nm_reference_triangle_t *t) {
    printf("  %s face %u binding %u argb %08x%s\n", who, t->face, t->binding, (unsigned)t->argb, t->polygon ? " (clipped)" : "");
    for (unsigned v = 0; v < 3; ++v) printf("  %s %.9g %.9g %.9g %.9g %.9g\n", who, (double)t->v[v][0], (double)t->v[v][1], (double)t->v[v][2], (double)t->v[v][3], (double)t->v[v][4]);
}

/* A submitted clipped polygon that touches no pixel of the screen and has no
 * reference polygon: the two clippers may disagree on a sliver whose
 * vertices lie within rounding of each other (one returns fewer than three,
 * or a plane test of a point within rounding of the plane differs), and such
 * a sliver off the screen draws nothing. Passed over (*i moves past it, 1),
 * as the reference's own off-screen triangles are; counted. 0 otherwise. */
static inline int nm_reference_extra(const nm_reference_triangle_t *submitted, unsigned submitted_count, unsigned *i) {
    const nm_reference_triangle_t *const s = submitted + *i;
    if (!s->polygon) return 0;
    unsigned n = 1;
    while (*i + n < submitted_count && submitted[*i + n].polygon == s->polygon) ++n;
    for (unsigned j = 0; j < n; ++j) if (nm_reference_on_screen(s[j].v, 640.0 * ENJ_XSCALE, 480.0)) return 0;
    ++nm_reference_tolerance.extra_polygons; *i += n;
    return 1;
}

/* The submitted triangles of one instance and list, in submission order,
 * against the reference triangles of that instance (both lists, face order).
 * Every submitted unclipped triangle must be the next reference triangle of
 * its list, bit for bit (but for the luma tolerance of a lit face); the
 * triangles of a clipped one (one polygon) must be the next reference
 * polygon, compared as polygons within the tolerances. A reference triangle
 * or polygon may be passed over only if it does not touch the screen, and so
 * may a submitted clipped polygon (nm_reference_extra).
 * Returns the number of reference triangles passed over, or -1 (message
 * printed). */
static int nm_reference_compare(const nm_reference_triangle_t *reference, unsigned reference_count,
        const nm_reference_triangle_t *submitted, unsigned submitted_count, unsigned list) {
    unsigned at = 0, i = 0;
    int skipped = 0;
    for (;;) {
        while (at < reference_count && reference[at].list != list) ++at;
        if (at == reference_count && i < submitted_count && nm_reference_extra(submitted, submitted_count, &i)) continue;
        if (at == reference_count) {
            if (i == submitted_count) return skipped;
            printf("reference mismatch: source %u instance %u list %u: submitted face %u binding %u has no reference triangle\n",
                submitted[i].source, submitted[i].instance, list, submitted[i].face, submitted[i].binding);
            nm_reference_print("submitted", submitted + i);
            return -1;
        }
        const nm_reference_triangle_t *const r = reference + at;
        /* The next item of each side: a triangle, or the triangles of one clipped polygon. */
        unsigned rn = 1, sn = i < submitted_count ? 1 : 0;
        if (r->polygon) while (at + rn < reference_count && reference[at + rn].polygon == r->polygon) ++rn;
        if (sn && submitted[i].polygon) while (i + sn < submitted_count && submitted[i + sn].polygon == submitted[i].polygon) ++sn;
        int matched = 0;
        if (sn && !r->polygon != !submitted[i].polygon) matched = 0;
        else if (sn && r->polygon) matched = nm_reference_polygon_match(r, rn, submitted + i, sn);
        else if (sn) {
            int choice;
            matched = nm_reference_match(r, submitted + i, &choice) != 0;
            if (matched) nm_reference_record_binding(r, choice, 1);
        }
        if (matched) { at += rn; i += sn; continue; }
        unsigned visible = 0;
        for (unsigned j = 0; j < rn; ++j) visible |= reference[at + j].visible;
        /* Two off-screen polygons of one face that differ (a sliver either
         * clipper cut otherwise): both passed over. */
        if (!visible && r->polygon && sn && submitted[i].polygon && submitted[i].face == r->face) {
            unsigned next = i;
            if (nm_reference_extra(submitted, submitted_count, &next)) { skipped += (int)rn; at += rn; i = next; continue; }
        }
        if (visible && sn && nm_reference_extra(submitted, submitted_count, &i)) continue;
        if (visible) {
            printf("reference mismatch: source %u instance %u list %u: reference face %u binding %u argb %08x %sis on screen and %s\n",
                r->source, r->instance, list, r->face, r->binding, (unsigned)r->argb, r->polygon ? "(clipped) " : "",
                sn ? "is not what was submitted next" : "was not submitted");
            for (unsigned j = 0; j < rn; ++j) nm_reference_print("reference", reference + at + j);
            for (unsigned j = 0; j < sn; ++j) nm_reference_print("submitted", submitted + i + j);
            if (getenv("NM_REFERENCE_DEBUG")) {
                for (unsigned j = 0; j < reference_count; ++j) if (reference[j].list == list && reference[j].face < r->face + 2 && reference[j].face + 40 > r->face) {
                    printf("R%u poly %u vis %u ", j, reference[j].polygon, reference[j].visible); nm_reference_print("ref", reference + j); }
                for (unsigned j = 0; j < submitted_count; ++j) if (submitted[j].face < r->face + 2 && submitted[j].face + 40 > r->face) {
                    printf("S%u poly %u ", j, submitted[j].polygon); nm_reference_print("sub", submitted + j); }
            }
            return -1;
        }
        skipped += (int)rn; at += rn;
    }
}

/* World objects (meshpack.h, NM_PACK_WORLD_OBJECTS). Which object instances
 * a frame must hold, from the original's rules alone: the pack's own object
 * words, the frame's cell bitmaps (501520 draw, 501540 near; the scene's cell
 * lists as nm_scenery_bitmaps writes them) and the race state the actors
 * read, none of it through the scene's selection code.
 *   kind ALWAYS  (2203c4, 220470, 2231c8): the bit of the actor's cell.
 *   kind BOARD   (221d64): state[0] bit 0 -> 2, else 1 when laps state[2]
 *                minus completed state[1] is 0 or 1 as an unsigned word
 *                (cmpobge: a count past the total -> 0), else 0; no lap
 *                total (state[2] == 0: attract) -> 0. Podium frames carry
 *                the race's start word and total and the podium player's
 *                count; their state[3] stays 0 (bearing origin zero).
 *   kinds TURN, CREW, SCREEN, RANK (FRM3 frame tables: `animated`, else
 *                NULL and such an instance is an error): animated[c/2]
 *                halves are the near ticks n of cell c this scene (low half
 *                for even c): TURN n mod 256 (220be4 heading 26031+256n,
 *                220b14 Z angle 256n), CREW (n mod 25)/8 (220dc0..220de0);
 *                animated[128] the original's tick counter 5010b0: SCREEN
 *                (5010b0>>3)&3 (2202a8); animated[129+k] the spawn slot of
 *                the car in position k+1: RANK k (221e58).
 *   kinds SLOT k (FRM4 frame tables: `slots`, else NULL and such an
 *                instance is an error): slots[k] reel k of the slot machine
 *                2204fc after the tick (0..64): list 2385f8[r], and list 64
 *                is list 0 (22066c ld (r9)[r10*4]); a value above 64 is an
 *                error. Drawn on the draw bit of its cell, as 220550 runs
 *                the reels only then.
 *   kind BEARING (223b48): model ((-a)>>5)&63, a the 16-bit angle of the
 *                camera entity 501240 seen from (210,-380), here from atan2
 *                in double precision instead of the original's arctangent
 *                table: a frame within NM_REFERENCE_BEARING_SLACK angle
 *                units of a model boundary may hold either neighbour
 *                (counted in *boundary). The entity's position: zero
 *                without a race; the player's position state[4], state[5]
 *                (float bits) in the follow view (camera mode state[6] != 1);
 *                otherwise what it was
 *                (*origin, kept by the caller from frame to frame; frame
 *                tables sample ticks, so during a crash camera the bearing
 *                model is not judged: *unjudged).
 *   state[7] bit31: the pit stop suspends the actor creator and all 64
 *                world-actor rows (dd9c..ddd4, D+128..S+511; include/pit.h):
 *                no world object is drawn.
 * `drawable(mesh)` says whether the build's scenery stage left the mesh a
 * face. frame: the frame's world instances, cells first. Returns the number
 * of object instances checked, or -1 with a message. */
#define NM_REFERENCE_BEARING_SLACK 1.0
static int nm_reference_world_objects(const nm_instance_t *pack_instances, unsigned pack_count,
        const nm_mesh_t *meshes, unsigned pack_mesh_count, const float view[12], const uint32_t bitmaps[16],
        const uint32_t state[8], const uint32_t *animated, const uint32_t *slots, double origin[2], const nm_instance_t *frame,
        unsigned frame_count, unsigned *boundary, unsigned *unjudged) {
    unsigned board = 0;
    if (state[2]) board = state[0] & 1u ? 2u : state[2] - state[1] <= 1u ? 1u : 0u;
    int stale = 0;
    if (!state[3]) origin[0] = origin[1] = 0;
    else if (state[6] != 1) {
        float position[2]; memcpy(position, state + 4, sizeof position);
        origin[0] = position[0]; origin[1] = position[1];
    } else stale = !(state[0] & 1u); /* rolling-start camera: still the scene's initial zero */
    const double x = origin[0], z = origin[1];
    /* 0f0f(z0, x, z, x0): angle whose cosine part is z0-z and sine part x-x0. */
    const double turn = atan2(x - 210.0, -380.0 - z) * (32768.0 / 3.14159265358979323846);
    unsigned bearing[2];
    for (unsigned side = 0; side < 2; ++side) {
        const long a = lrint(floor(turn + (side ? NM_REFERENCE_BEARING_SLACK : -NM_REFERENCE_BEARING_SLACK)));
        const long wrapped = ((a % 65536) + 65536) % 65536, angle = wrapped < 32768 ? wrapped : wrapped - 65536;
        bearing[side] = (unsigned)((-angle) >> 5) & 63u;
    }
    unsigned at = 0, checked = 0;
    while (at < frame_count && !(frame[at].padding[0] & NM_OBJECT_INSTANCE)) ++at;
    for (unsigned i = 0; i < pack_count; ++i) {
        const uint32_t word = pack_instances[i].padding[0];
        if (!(word & NM_OBJECT_INSTANCE)) continue;
        const unsigned cell = NM_OBJECT_CELL(word), kind = NM_OBJECT_KIND(word);
        unsigned variant = NM_OBJECT_VARIANT(word), mesh = pack_instances[i].mesh;
        /* A frame-counter animation's instance: no selector of this list places it (render/scene/practice.c,
         * practice_clocks; tests/test_clock_lockstep.py has its frames). gen_frames.py's tables leave them out. */
        if (kind == NM_OBJECT_CLOCK) continue;
        int wanted = !(state[7] >> 31) &&
            (int)(bitmaps[(word & NM_OBJECT_NEAR ? 8u : 0u) + (cell >> 5)] >> (cell & 31) & 1u), either = 0;
        if (wanted && kind == NM_OBJECT_BOARD) wanted = variant == board;
        const unsigned n = animated ? (unsigned)(animated[cell >> 1] >> (16 * (cell & 1)) & 0xffffu) : 0u;
        /* NM_OBJECT_CONE: at rest in every reference frame (no log has a knocked cone): drawn by its cell. */
        if (kind >= NM_OBJECT_TURN && kind != NM_OBJECT_CONE) {
            if (!animated || kind >= NM_OBJECT_KINDS || (kind >= NM_OBJECT_SLOT && !slots)) {
                printf("reference mismatch: world object %u has animated kind %u; it needs FRM%c frame tables\n", i, kind,
                    kind >= NM_OBJECT_SLOT ? '4' : '3');
                return -1;
            }
            if (kind >= NM_OBJECT_SLOT && kind < NM_OBJECT_SLOT + NM_OBJECT_SLOTS && slots[kind - NM_OBJECT_SLOT] > 64u) {
                printf("reference mismatch: slot machine reel %u is %u (0..64)\n", kind - NM_OBJECT_SLOT, (unsigned)slots[kind - NM_OBJECT_SLOT]);
                return -1;
            }
            const unsigned value = kind == NM_OBJECT_TURN ? n % 256u : kind == NM_OBJECT_CREW ? n % 25u / 8u :
                kind == NM_OBJECT_SCREEN ? animated[128] >> 3 & 3u :
                kind >= NM_OBJECT_SLOT && kind < NM_OBJECT_SLOT + NM_OBJECT_SLOTS ? slots[kind - NM_OBJECT_SLOT] % 64u :
                animated[129 + kind - NM_OBJECT_RANK];
            if (word & NM_OBJECT_MESHES) {   /* one instance for a run of meshes: the value picks the mesh */
                if (wanted) wanted = value < NM_OBJECT_MESH_COUNT(word);
                mesh += value; variant = value;
            } else if (wanted) wanted = variant == value;
        }
        if (mesh >= pack_mesh_count || !meshes[mesh].face_count) continue;
        if (wanted && kind == NM_OBJECT_BEARING) {
            wanted = variant == bearing[0] || variant == bearing[1];
            either = wanted && bearing[0] != bearing[1];
        }
        const int present = at < frame_count && frame[at].padding[0] == word && frame[at].mesh == mesh &&
            frame[at].source_address == pack_instances[i].source_address;
        if (present && (word & NM_OBJECT_PLACED)) {
            /* view * placement (* Rz(256n), 220b14), double precision, against the frame's matrix. */
            const float *p = pack_instances[i].matrix;
            double model[12];
            for (unsigned k = 0; k < 12; ++k) model[k] = p[k];
            if (word & NM_OBJECT_SPIN) {
                const double turn = (double)(uint16_t)(256u * n) * (6.28318530717958647692 / 65536.0), c = cos(turn), sn = sin(turn);
                for (unsigned r = 0; r < 3; ++r) {
                    const double x = p[r], y = p[3 + r];
                    model[r] = c * x - sn * y; model[3 + r] = sn * x + c * y;
                }
            }
            for (unsigned col = 0; col < 4; ++col)
                for (unsigned r = 0; r < 3; ++r) {
                    double v = view[r] * model[3 * col] + view[3 + r] * model[3 * col + 1] + view[6 + r] * model[3 * col + 2];
                    if (col == 3) v += view[9 + r];
                    const double got = frame[at].matrix[3 * col + r];
                    if (fabs(got - v) > 1e-4 * (1.0 + fabs(v))) {
                        printf("reference mismatch: placed world object %u (model %06x, kind %u) matrix word %u is %.9g, expected %.9g\n",
                            i, (unsigned)pack_instances[i].source_address, kind, 3 * col + r, got, v);
                        return -1;
                    }
                }
        }
        if (stale && kind == NM_OBJECT_BEARING) {
            if (unjudged && present) ++*unjudged;
            if (present) { ++at; ++checked; }
            continue;
        }
        if (either) { if (boundary) ++*boundary; }
        else if (present != wanted) {
            printf("reference mismatch: world object %u (model %06x, cell %u, kind %u, variant %u) is %s but the original %s it\n",
                i, (unsigned)pack_instances[i].source_address, cell, kind, variant,
                present ? "in the frame" : "missing", wanted ? "draws" : "does not draw");
            return -1;
        }
        if (present) { ++at; ++checked; }
    }
    if (at != frame_count) {
        printf("reference mismatch: world instance %u of the frame is no object the pack and the rules give\n", at);
        return -1;
    }
    return (int)checked;
}
#endif
