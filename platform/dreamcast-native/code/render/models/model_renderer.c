/* Model renderer: resources, object queueing and frame submission. */
#include "render/models/model_renderer.h"
#include "render/scene/sky.h"
#include "render/models/scenery_camera.h"
#include "geometry.h"
#include "lighting.h"
#include "render/materials/materials.h"
#include "scene_loader.h"
#include <enDjinn/enj_enDjinn.h>
#include <kos.h>
#if defined(NATIVE_PROFILE) || defined(NATIVE_CACHE_PROFILE)
#include <dc/perfctr.h>
#endif
#if defined(NATIVE_CACHE_PROFILE) && defined(ENJ_DBG_PRINT)
#error Cache profiling cannot share PRFC1 with enDjinn perf_monitor
#endif
#include <sh4zam/shz_sh4zam.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/models/model_math.h"
#include "heap.h"
#include "clock_actors.h"
#include "render/models/clock_light.h"
#include "nm_mem.h"

static int draw_inspect_strips;
/* Inspection view only: punch-through polygons carry a stipple texture (see
 * draw_inspect_stipple_prepare); 0 until the view was switched on. */
static int draw_inspect_stipple;
#ifdef DAYTONA_UNTEXTURED_BENCH
static const int draw_untextured=1;
#else
static const int draw_untextured=0;
#endif
alignas(32) static pvr_poly_hdr_t draw_inspect_headers[2];

/* ponytail: bounded selected-scene stream; reject capacity overflow. Broader
 * course/camera coverage needs measured geometry partitioning, not silent cuts.
 * Retain order; skip identical consecutive headers within each list callback.
 * Each triangle needs at most 128 TA bytes, including an unshared header. */
#define NM_DRAW_TA_BYTES (512u*1024u-64u*1024u)
/* Triangles of one frame. No buffer is sized by this number: the emission
 * queues hold every face of the packs and the stream is bounded by its bytes
 * (draw_ta_fits: 448 KiB of the 512 KiB TA vertex buffer of code/main.c, the
 * rest is the reserve for sky, shadow volumes, HUD and list ends). The cap
 * was 6,144, which stopped the frame loop ("FAIL SQ triangle capacity") at
 * 343 KiB or so of stream, well inside that budget; it is now the vertex
 * count of the byte budget, 14,336, which a stream of triangles cannot reach
 * before the byte guard does. Host measure, stage 5, both scripted races and
 * the attract scene: the shipped scenery reaches 4,247 triangles in 278 KiB,
 * trees as body and bands (assets-tb2) 5,665 in 364 KiB; about 66 bytes a
 * triangle, so the byte budget ends near 6,900 triangles of that mix. */
#ifndef NM_DRAW_TRIANGLES /* (host tools may set it: tests/stream_sim) */
#define NM_DRAW_TRIANGLES (NM_DRAW_TA_BYTES/32u)
#endif
/* Header reuse is measured, not charged once per triangle. Keep the same
 * hardware TA allocation and64KiB HUD/list reserve; guard before every write.
 * The SQ emitter passes the vertices it has written in units of three
 * (model_submission.h, SQ_TA_UNITS), so strips are charged their real bytes. */
static int draw_ta_fits(unsigned triangles,unsigned headers) {
    return triangles<=NM_DRAW_TA_BYTES/96u && headers<=(NM_DRAW_TA_BYTES-triangles*96u)/32u;
}
_Static_assert(sizeof(pvr_vertex_t)==32, "one complete cache line per PVR vertex");
static const nm_pack_t *draw_pack;
static uint8_t *draw_groups;
static unsigned draw_group_capacity; /* faces draw_groups holds (the car's largest mesh, then the world's) */
/* Depth factor of a DNM6 face layer, 1+layer*NM_LAYER_DEPTH_STEP. */
alignas(32) static float draw_layer_scale[256]={1.0f};
static void draw_layer_scale_init(void) {
    for(unsigned layer=1;layer<256;++layer) draw_layer_scale[layer]=1.0f+(float)layer*NM_LAYER_DEPTH_STEP;
}
static const nm_material_pack_t *draw_materials;
/* Host tools' source of the emission table (the simulator, the fixtures:
 * model_submission.h, sq_emit_lookup): a header and a UV mapping per binding.
 * The console never allocates one (3.6 MB for the cars' 56,530 bindings): it
 * compiles each header into the pool of the emission classes
 * (draw_emit_value below, sq_emit_records_build). Both pointers stay NULL here. */
typedef struct {
    _Alignas(32) pvr_poly_hdr_t header;
    nm_material_uv_t uv;
    pvr_ptr_t texture;
} nm_draw_material_t;
_Static_assert(sizeof(nm_draw_material_t) == 64, "aligned header and UV cache");
static nm_draw_material_t *draw_material_cache;
static uint16_t *draw_bindings;
static uint32_t *draw_runs; /* the car's material/luma runs (model_submission.h sq_lookup); NULL: draw_bindings */
static struct {
    const nm_pack_t *pack;
    const nm_material_pack_t *materials;
    nm_draw_material_t *cache;
    uint16_t *bindings;
    uint8_t *surface; /* Per material binding; published as sq_surface_bindings. */
    /* Per material binding: NM_SCENERY_POINT, the opaque pieces of split
     * cutouts (tools/split_cutouts.py). Point sampled like their
     * punch-through neighbours of the same source polygon. */
    uint8_t *point;
} world_draw;
/* Triangles and headers of the frame so far: the TA and capacity guards. */
static unsigned draw_count;
/* Statistics for host tools (the simulator and the fixtures define the hook
 * and own the counters); nothing is counted on the console. */
#ifndef SQ_COUNT
#define SQ_COUNT(counter,amount) ((void)0)
#endif
static unsigned draw_header_count;
static unsigned draw_samples;
static uint64_t draw_prepare_sum, draw_prepare_max;
static uint64_t draw_previous_update, draw_interval_sum, draw_interval_max;
#ifdef NATIVE_PROFILE
static uint64_t draw_phases[13], draw_clip_paths[5];
static unsigned draw_texture_header_reuse;
#define DRAW_PHASE_START(name) const uint64_t name = perf_cntr_timer_ns()
#define DRAW_PHASE_END(phase, start) (draw_phases[phase] += perf_cntr_timer_ns() - (start))
#define DRAW_PHASE_NEXT(phase, start, next) DRAW_PHASE_START(next); draw_phases[phase] += next - (start)
#define DRAW_PROFILE_SOURCE(source) ((void)0)
#elif defined(NATIVE_CACHE_PROFILE)
static uint64_t draw_cache_stalls[2][13], draw_cache_time[2][13];
static unsigned draw_cache_source;
static int draw_cache_counter_owned;
static int draw_cache_sampling;
static unsigned draw_cache_samples;
typedef struct { uint64_t ns, events; unsigned source; } nm_perf_sample_t;
static inline nm_perf_sample_t draw_perf_sample(void) {
    const nm_perf_sample_t sample={perf_cntr_timer_ns(),perf_cntr_count(PRFC1),draw_cache_source};
    return sample;
}
static inline void draw_perf_add(unsigned phase,nm_perf_sample_t start,nm_perf_sample_t end) {
    draw_cache_time[start.source][phase]+=end.ns-start.ns;
    draw_cache_stalls[start.source][phase]+=(end.events-start.events)&((1ULL<<48)-1);
}
#define DRAW_PROFILE_SOURCE(source) (draw_cache_source=(source))
#define DRAW_PHASE_START(name) const nm_perf_sample_t name=draw_cache_sampling ? draw_perf_sample() : (nm_perf_sample_t){0}
#define DRAW_PHASE_END(phase, start) do { if(draw_cache_sampling) draw_perf_add(phase,start,draw_perf_sample()); } while(0)
#define DRAW_PHASE_NEXT(phase, start, next) DRAW_PHASE_START(next); if(draw_cache_sampling) draw_perf_add(phase,start,next)
#else
#define DRAW_PHASE_START(name) ((void)0)
#define DRAW_PHASE_END(phase, start) ((void)0)
#define DRAW_PHASE_NEXT(phase, start, next) ((void)0)
#define DRAW_PROFILE_SOURCE(source) ((void)0)
#endif
static const unsigned draw_lists[2] = {PVR_LIST_OP_POLY, PVR_LIST_PT_POLY};

/* Screen-checker scenery panels (texture header bit 15; course 0: the catch
 * fence above the wall). The arcade draws such a polygon on every other
 * pixel of a screen checkerboard. The world packer turns them into
 * punch-through faces with a stipple texture (tools/prepare_face_order.py
 * --stipple-panels): such a pack has no screen-checker face. A pack that
 * still carries them draws them as opaque polygons of the same colour. The
 * cars' checker faces: the shadow is a modifier volume (never looked up); the
 * smoke of the car effects is the translucent class of the material packs
 * (material_pack.h, nm_material_translucent: textured, texel 15 transparent,
 * checker) and goes to the translucent list (model_submission.h,
 * "Translucent effects"). */
static unsigned draw_binding_list(const nm_material_binding_t *binding) {
    const unsigned th = binding->header[0];
    /* make NATIVE_SMOKE_MODE=pt-tex|pt-screen (NM_SMOKE_MODE 1, 2): the punch-through list instead. */
    if (binding->resource != NM_MATERIAL_SOLID && nm_material_translucent(binding->header))
        return NM_SMOKE_PUNCH ? PVR_LIST_PT_POLY : PVR_LIST_TR_POLY;
#ifdef NM_DIAG_CAR_OPAQUE
    /* make NATIVE_DIAG_CAR_OPAQUE=1 (measurement, wrong picture): the cars'
     * punch-through faces go to the opaque list, alpha ignored. */
    if (draw_materials && binding >= draw_materials->bindings &&
        binding < draw_materials->bindings + draw_materials->count) return PVR_LIST_OP_POLY;
#endif
    return ((th >> 13) & 3) == 3 ? PVR_LIST_PT_POLY : PVR_LIST_OP_POLY;
}

/* Scenery mip level-of-detail multiplier in quarter steps when the world pack
 * has mip chains: 1 = 0.25, the smallest legal value. Used only with
 * NM_MIP_CLASSES=0 (make NATIVE_MIP_CLASSES=0), and there for textures without
 * a chain too (no effect on their picture). */
#ifndef NM_WORLD_MIP_BIAS
#define NM_WORLD_MIP_BIAS 1
#endif
/* Mip D adjust per class (make NATIVE_MIP_CLASSES, default 1), quarter steps,
 * for every binding whose texture HAS a mip chain; a texture without one keeps
 * the value of the rule before (opaque scenery NM_WORLD_MIP_BIAS, everything
 * else KOS's 1.0), so a pack without chains compiles the same headers.
 *   road-surface class and grandstand composites: NM_MIP_BIAS_SURFACE
 *     (make NATIVE_MIP_BIAS_SURFACE, default 2 = 0.5)
 *   everything else, opaque and punch-through: NM_MIP_BIAS (make
 *     NATIVE_MIP_BIAS, default 4 = 1.0)
 *   the catch-fence stipple: always 1.0 (its levels are made to be picked at
 *     one texel per pixel; it is point sampled, see draw_binding_stipple).
 * The core multiplies the D it computed by the adjust (the TSP word's mip_bias
 * field, KOS dc/pvr.h pvr_mip_bias_t, PVR_MIPBIAS_0_25..3_75): below 1.0 a level change moves further from the eye,
 * so more texels fall on a pixel (sharper, more aliasing); 1.0 is the level
 * whose texels are about one pixel along the polygon's steeper screen axis.
 * The road and the stands are seen at grazing angles, where that axis is the
 * one across the surface: at 1.0 the lane lines, kerbs and crowd blur into
 * streaks (PVR mip selection is isotropic); 0.5 keeps them (previews of
 * build/dc-native-mipmaps.ds44Xc/preview, road-straight: RMS against a 4x4
 * supersampled reference 15.3 at 0.5, 17.0 at 1.0, 21.8 without chains).
 * Trees, signs, objects and cutouts face the camera and take 1.0. */
#ifndef NM_MIP_CLASSES
#define NM_MIP_CLASSES 1
#endif
#if NM_MIP_CLASSES
#ifndef NM_MIP_BIAS_SURFACE
#define NM_MIP_BIAS_SURFACE 2
#endif
#ifdef NM_MIP_BIAS
#define NM_MIP_BIAS_REST NM_MIP_BIAS
#else
#define NM_MIP_BIAS_REST 4
#endif
#if NM_MIP_BIAS_SURFACE < 1 || NM_MIP_BIAS_SURFACE > 15 || NM_MIP_BIAS_REST < 1 || NM_MIP_BIAS_REST > 15
#error "mip D adjust: 1..15 quarter steps (0 is illegal)"
#endif
#elif defined(NM_MIP_BIAS_SURFACE)
#error "NM_MIP_BIAS_SURFACE needs NM_MIP_CLASSES=1"
#endif
/* make NATIVE_SUPERSAMPLE_SURFACE=1: TSP "super-sample texture" (bit 12 of
 * the TSP word, mode2) on the road-surface class: the core samples four
 * texels a pixel and averages them, the PVR's only help against the blur and
 * crawl of grazing-angle textures; texture reads of those pixels cost about
 * three to four times as much. Measurement option. */
#ifndef NM_SUPERSAMPLE_SURFACE
#define NM_SUPERSAMPLE_SURFACE 0
#endif
#define NM_TSP_SUPER_SAMPLE (1u<<12)
/* Road-surface class of a world binding (nm_render_world_attach): depth
 * scale, shadow receiver and, unless NM_ROAD_FILTER, point sampling. */
#ifndef NM_ROAD_FILTER
#define NM_ROAD_FILTER 0
#endif
static int draw_binding_surface(const nm_material_binding_t *binding, int scenery) {
    return scenery && world_draw.surface && world_draw.materials &&
        world_draw.surface[binding-world_draw.materials->bindings];
}
/* Opaque piece of a split cutout: the rest of its source polygon is drawn on
 * the punch-through list, which is never filtered. A bilinear opaque piece
 * would show a seam against it and blend the transparent entry's colour in
 * at the cut. */
static int draw_binding_point(const nm_material_binding_t *binding, int scenery) {
    return scenery && world_draw.point && world_draw.materials &&
        world_draw.point[binding-world_draw.materials->bindings];
}
/* Catch-fence stipple (tools/prepare_face_order.py stipple_header): a
 * textured punch-through header (word 0 bits 15..13 = 011) whose word 1 is
 * STIPPLE_MARK 0x8000, plus a sparser level's variant number in bits 8..9
 * (tools/lod_groups.py stipple_base; prepare_palette_materials.py gives every
 * variant its own stipple texture). A binding carries its material's header. */
static inline int draw_binding_stipple(const nm_material_binding_t *binding, int scenery) {
    return scenery && ((binding->header[0] >> 13) & 7u) == 3u && (binding->header[1] & 0xfcffu) == 0x8000u;
}
/* Grandstand composite (tools/prepare_face_order.py flatten_stacks): an
 * opaque textured header (bits 15..13 = 010) whose word 1 is COMPOSITE_MARK
 * 0x4000 plus the composite's number in the low byte. */
static inline int draw_binding_composite(const nm_material_binding_t *binding, int scenery) {
    return scenery && ((binding->header[0] >> 13) & 7u) == 2u && (binding->header[1] & 0xff00u) == 0x4000u;
}

static int compile_draw_header(const nm_material_binding_t *binding, pvr_poly_hdr_t *header, int scenery) {
    const unsigned th = binding->header[0];
    const unsigned list = draw_binding_list(binding);
    pvr_poly_cxt_t context;
    int mipmapped = 0;
    if (binding->resource == NM_MATERIAL_SOLID) {
        pvr_poly_cxt_col(&context, list);
        /* A palette pack draws flat-shaded strips whose faces differ in
         * colour (model_submission.h): its untextured faces must not blend
         * across a strip either. One colour per face is what they have. */
        if (scenery ? nm_world_materials_palettized() : nm_materials_index()->palette_index != NULL)
            context.gen.shading = PVR_SHADE_FLAT;
    } else {
        const enj_texture_info_t *tex;
        if (scenery) tex=nm_world_materials_texture(binding->resource);
        else
        {
            DRAW_PHASE_START(texture_started);
            if (!nm_materials_require(binding->resource)) return 0;
            tex=nm_materials_texture(binding->resource);
#if NM_SMOKE_MODE
            /* The translucent class on the punch-through list: its checker texture (materials.c). */
            if (nm_material_translucent(binding->header)) tex=nm_materials_smoke_texture(binding->resource);
#endif
            DRAW_PHASE_END(8,texture_started);
        }
        if (!tex) return 0;
        mipmapped = tex->flags.mipmapped;
        /* Palette shading (DNT6 car pack): the texture is a 4-bit index image
         * and the binding selects one of the gain-bucket palettes resident in
         * palette RAM. Texture, list, filter and bank make the header: every
         * luma of a texture that shares a bank compiles to the same 32 bytes,
         * which the shared-header pass then folds into one. */
        const int bank = scenery ? nm_world_materials_palette_bank(binding) : nm_materials_palette_bank(binding);
        pvr_poly_cxt_txr(&context, list,
                        bank < 0 ? tex->pvrformat : tex->pvrformat | PVR_TXRFMT_4BPP_PAL((unsigned)bank),
                        tex->width, tex->height,
                        fDtAdjustPVRPointer(&tex->hdr, tex->ptr), PVR_FILTER_NONE);
#ifdef NATIVE_BILINEAR
        /* Smooth opaque 3D textures; preserve PT alpha coverage and crisp UI.
         * The road surface class is point sampled by default (project
         * decision, 12 October 2026): asphalt, kerbs, edge lines, the finish
         * overlay and road streaks keep their texels. NM_ROAD_FILTER=1 (make
         * NATIVE_ROAD_FILTER=1) filters its opaque polygons like the rest, as
         * the arcade does. The class is fixed per binding
         * before any header is compiled, so the filter is part of the cached
         * header like texture, list and palette bank. */
        if (list==PVR_LIST_OP_POLY && (NM_ROAD_FILTER || !draw_binding_surface(binding, scenery)) &&
            !draw_binding_point(binding, scenery)) context.txr.filter=PVR_FILTER_BILINEAR;
#ifdef NM_FILTER_ALL
        /* make NATIVE_FILTER_ALL=1 (default): punch-through polygons (and the
         * opaque pieces cut from them) are filtered too. Coverage is then
         * tested on the filtered alpha against PT_ALPHA_REF (code/main.c,
         * NM_PT_ALPHA_REF: 0x80 keeps a silhouette where the texel edge is),
         * so cutout edges take some of the transparent texel's colour.
         * The catch-fence stipple stays point sampled (make
         * NATIVE_STIPPLE_FILTER=1 filters it too): a one-texel checkerboard
         * filters to a flat half alpha, which no reference value turns back
         * into a net (0xFF drops every pixel of it). */
#if !defined(NM_STIPPLE_FILTER) || !NM_STIPPLE_FILTER
        if (!draw_binding_stipple(binding, scenery))
#endif
            context.txr.filter=PVR_FILTER_BILINEAR;
#endif
#endif
        if (bank >= 0 && list == PVR_LIST_OP_POLY && NM_CAR_PALETTE_OPAQUE_IGNORES_ALPHA)
            context.txr.alpha = PVR_TXRALPHA_DISABLE;
        /* One colour per face: Gouraud off, so a strip can carry faces of
         * different light (model_submission.h, NM_FLAT_PROVOKING_LAST). */
        if (bank >= 0) context.gen.shading = PVR_SHADE_FLAT;
        context.txr.uv_flip = (th & 0x100 ? PVR_UVFLIP_U : 0) | (th & 0x200 ? PVR_UVFLIP_V : 0);
#if NM_SMOKE_MODE
        if (!scenery && nm_material_translucent(binding->header)) {
            /* Punch-through smoke (make NATIVE_SMOKE_MODE): coverage is the texture's alpha alone (MODULATE: the
             * binding's vertex alpha 0x80 of the translucent list does not enter), u repeats (pt-screen reads
             * u + n/2 for the n-th pixel of x + y), no flip. pt-tex is point sampled: a one-texel checker filtered
             * is a flat half alpha (as the catch-fence stipple, draw_binding_stipple). pt-screen keeps the bilinear
             * filter of every punch-through texture: each pixel samples one half of the texture only. */
            context.txr.env = PVR_TXRENV_MODULATE;
            context.txr.uv_clamp = PVR_UVCLAMP_NONE;
            context.txr.uv_flip = PVR_UVFLIP_NONE;
            /* tr-screen (NM_SMOKE_MODE 3): the same texture and u' on the translucent list, alpha the texel's (MODULATE),
             * blended SRCALPHA / INVSRCALPHA: a pixel takes the texel or keeps what is under it; back to front, the
             * nearest smoke texel wins, as on the arcade. NM_SMOKE_POINT: the checker point sampled (then alpha is 0 or 1
             * and every pixel is the texel's or untouched; filtered, a pixel at a hole's edge blends). */
            if (NM_SMOKE_MODE == 1 || NM_SMOKE_POINT) context.txr.filter = PVR_FILTER_NONE;
        }
#endif
    }
    context.gen.culling = PVR_CULLING_NONE;
    context.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    /* The translucent class: blended over the scene (pvr_poly_cxt_txr's translucent defaults: SRCALPHA /
     * INVSRCALPHA, texture alpha times the binding's vertex alpha 0x80), depth tested, no depth write. */
    if (list == PVR_LIST_TR_POLY) context.depth.write = PVR_DEPTHWRITE_DISABLE;
    /* Road-surface polygons receive the cars' ground shadows: cheap-shadow
     * bit only (one parameter set, ordinary vertices), darkened by the global
     * shadow scale inside an opaque modifier volume. The volumes are written
     * at the surface's scaled depth, so polygons at true depth (rails, walls,
     * cars) must not receive: they would be tested against the wrong depth. */
#ifdef NM_DIAG_NO_SHADOW_RECEIVERS
    /* make NATIVE_DIAG_NO_SHADOWS=1|receivers (measurement): no polygon receives. */
    const int receiver = 0;
#else
    const int receiver = draw_binding_surface(binding, scenery);
#endif
    context.fmt.modifier = receiver ? PVR_MODIFIER_ENABLE : PVR_MODIFIER_DISABLE;
    context.gen.modifier_mode = PVR_MODIFIER_CHEAP_SHADOW;
    /* TSP mip bias (KOS dc/pvr/pvr_header.h mip_bias, bits 11-8; dc/pvr.h
     * pvr_mip_bias_t): a 4-bit unsigned value with two fraction bits
     * (PVR_MIPBIAS_0_25 = 1 .. PVR_MIPBIAS_3_75 = 15, PVR_MIPBIAS_NORMAL = 4)
     * by which the core multiplies the D it computed; D 1 selects the
     * full-size level, D 2 the half-size one. KOS defines no value 0, so
     * 0.25 is the smallest multiplier and moves each level change four times
     * further from the eye.
     * Default (NM_MIP_CLASSES=1): a texture with a mip chain takes the value
     * of its class (above NM_MIP_CLASSES). A texture without one keeps the
     * rule before, which is the whole rule of make NATIVE_MIP_CLASSES=0:
     * opaque scenery NM_WORLD_MIP_BIAS (0.25), cars the KOS default (4 =
     * 1.0), NATIVE_MIP_BIAS overriding both; punch-through polygons 1.0. */
#if NM_MIP_CLASSES
    if (mipmapped) {
        if (draw_binding_stipple(binding, scenery)) context.txr.mipmap_bias = PVR_MIPBIAS_1_00;
        else context.txr.mipmap_bias = draw_binding_surface(binding, scenery) || draw_binding_composite(binding, scenery) ?
            NM_MIP_BIAS_SURFACE : NM_MIP_BIAS_REST;
    } else
#endif
    if (list == PVR_LIST_OP_POLY) {
#if defined(NM_MIP_BIAS) && !NM_MIP_CLASSES
        context.txr.mipmap_bias = NM_MIP_BIAS;
#else
        if (scenery) context.txr.mipmap_bias = NM_WORLD_MIP_BIAS;
#endif
    }
    (void)mipmapped;
    pvr_poly_compile(header, &context);
    /* KOS allocates a cacheline with MOVCA.L and writes only the four active
     * words. Initialize unused words AFTER compile, since MOVCA.L may discard
     * an earlier memset on a cache miss. No second-area headers here. */
    nm_memset((char *)header + 16, 0, 16);
#if NM_SUPERSAMPLE_SURFACE
    /* Road-surface class textures: TSP super-sample bit (NM_TSP_SUPER_SAMPLE).
     * A receiver of the cars' shadows uses the cheap-shadow mode: one TSP
     * word for inside and outside the volume, no second parameter set (the
     * words cleared above). A normal-mode modifier header would carry a
     * second TSP word (mode2_1) and get the bit there as well. */
    if (binding->resource != NM_MATERIAL_SOLID && draw_binding_surface(binding, scenery)) {
        header->mode2 |= NM_TSP_SUPER_SAMPLE;
        if ((header->cmd & PVR_TA_CMD_MODIFIER) && (header->cmd & PVR_TA_CMD_MODIFIERMODE)) header->mode2_1 |= NM_TSP_SUPER_SAMPLE;
    }
#endif
    /* make NATIVE_SUPERSAMPLE_FAR=1 (default with NATIVE_FILTER_ALL=1): the far level of a world level-of-detail
     * group of tools/lod_groups.py "supersample" rules (road, stripes, verges ... beyond the depth where a texel
     * is about one pixel tall) is the same face under a header whose word 1 carries SUPERSAMPLE_MARK 0x1000
     * (no other mark uses bit 12; its binding is that of the header without it): that header gets the TSP
     * super-sample bit, as NM_SUPERSAMPLE_SURFACE gives it. A pack without such headers compiles the same. */
#ifndef NM_SUPERSAMPLE_FAR
#define NM_SUPERSAMPLE_FAR 0
#endif
#if NM_SUPERSAMPLE_FAR
    if (scenery && binding->resource != NM_MATERIAL_SOLID && (th & 0x4000u) && (binding->header[1] & 0xff00u) == 0x1000u) {
        header->mode2 |= NM_TSP_SUPER_SAMPLE;
        if ((header->cmd & PVR_TA_CMD_MODIFIER) && (header->cmd & PVR_TA_CMD_MODIFIERMODE)) header->mode2_1 |= NM_TSP_SUPER_SAMPLE;
    }
#endif
    return 1;
}

#ifdef NATIVE_BOUNDS_VERIFY
static unsigned bounds_verified;
#endif
static inline __attribute__((always_inline)) void cache_draw_vertex(const nm_pack_t *pack, const nm_mesh_t *mesh, const nm_instance_t *inst,
                              const nm_view_t *view, unsigned v, nm_cached_vertex_t *points, int inside) {
    const nm_vertex_t *vertex = &pack->vertices[mesh->vertex_first + v];
    const float *p = vertex->position;
    const shz_vec4_t t = shz_xmtrx_transform_vec4(shz_vec4_init(p[0], p[1], p[2], 1));
    const nm_view_vertex_t point = {t.x * inst->focus[0], t.y * inst->focus[1],
                                  t.z, vertex->u, vertex->v};
#ifdef NATIVE_BOUNDS_VERIFY
    bounds_verified+=inside!=0;
    if(inside && nm_geometry_classify(view,&point)) {
        puts("FAIL: whole-block inside classification"); abort();
    }
#endif
    nm_cache_vertex_classified(view, &point, points, inside);
    SQ_COUNT(draw_transformed,1);
}

/* cache_draw_vertex for a batch whose inputs are bounded (model_submission.h,
 * sq_batch_bounded): the same transform, focus, classification and
 * projection, without the finite tests that such input cannot fail. */
static inline __attribute__((always_inline)) void cache_draw_vertex_bounded(const nm_pack_t *pack, const nm_mesh_t *mesh, const nm_instance_t *inst,
                              const nm_view_t *view, unsigned v, nm_cached_vertex_t *points, int inside) {
    const nm_vertex_t *vertex = &pack->vertices[mesh->vertex_first + v];
    const float *p = vertex->position;
    const shz_vec4_t t = shz_xmtrx_transform_vec4(shz_vec4_init(p[0], p[1], p[2], 1));
    const nm_view_vertex_t point = {t.x * inst->focus[0], t.y * inst->focus[1],
                                  t.z, vertex->u, vertex->v};
#ifdef NATIVE_BOUNDS_VERIFY
    bounds_verified+=inside!=0;
    if(inside && nm_geometry_classify(view,&point)) {
        puts("FAIL: whole-block inside classification"); abort();
    }
    {
        /* The checked routine on the same point: status and, for a projected
         * point, every stored word must agree. */
        nm_cached_vertex_t checked;
        nm_cache_vertex_classified(view, &point, &checked, inside);
        nm_cache_vertex_bounded(view, &point, points, inside);
        if(nm_cached_outside(&checked)!=nm_cached_outside(points) || memcmp(&checked.view,&points->view,sizeof(checked.view)) ||
           (!nm_cached_outside(&checked) && (memcmp(&checked.x,&points->x,sizeof(float)) ||
            memcmp(&checked.y,&points->y,sizeof(float)) || memcmp(&checked.q,&points->q,sizeof(float))))) {
            puts("FAIL: bounded vertex path differs from the checked one"); abort();
        }
    }
#else
    nm_cache_vertex_bounded(view, &point, points, inside);
#endif
    SQ_COUNT(draw_transformed,1);
}

/* The same for a wide-view batch (model_submission.h): `guard` is the view
 * with the guard band; side planes are left to the PVR. */
static inline __attribute__((always_inline)) void cache_draw_vertex_wide(const nm_pack_t *pack, const nm_mesh_t *mesh, const nm_instance_t *inst,
                              const nm_view_t *guard, unsigned v, nm_cached_vertex_t *points, int inside) {
    const nm_vertex_t *vertex = &pack->vertices[mesh->vertex_first + v];
    const float *p = vertex->position;
    const shz_vec4_t t = shz_xmtrx_transform_vec4(shz_vec4_init(p[0], p[1], p[2], 1));
    const nm_view_vertex_t point = {t.x * inst->focus[0], t.y * inst->focus[1],
                                  t.z, vertex->u, vertex->v};
#ifdef NATIVE_BOUNDS_VERIFY
    bounds_verified+=inside!=0;
    if(inside && nm_geometry_classify(guard,&point)) {
        puts("FAIL: whole-block inside classification (guard view)"); abort();
    }
#endif
    nm_cache_vertex_wide(guard, &point, points, inside);
#ifdef NATIVE_BOUNDS_VERIFY
    /* A projected point lies inside the guard band and is the checked
     * projection up to its clamp; a status equals the checked plane bits. */
    {
        nm_cached_vertex_t checked;
        nm_cache_vertex_classified(guard, &point, &checked, 0);
        const unsigned status=nm_cached_outside(points), reference=nm_cached_outside(&checked);
        if(status ? status!=reference : (reference&1) || points->q!=(reference ? points->q : checked.q) ||
           points->x<guard->bounds[0] || points->x>guard->bounds[1] || points->y<guard->bounds[2] || points->y>guard->bounds[3]) {
            puts("FAIL: wide vertex path differs from the checked one"); abort();
        }
    }
#endif
    SQ_COUNT(draw_transformed,1);
}

static unsigned reject_draw_face(const nm_face_t *face, unsigned header, int lighting) {
    if (!(face->attribute & (1u << 17)) && (lighting & 256)) return 1;
    if (!(face->attribute & (3u << 8))) return 2;
    /* Screen-space checker is not texture alpha; still explicitly omitted. */
    return header & 0x8000 ? 3 : 0;
}

#include "frame_trace.h"
#include "render/models/model_submission.h"
#ifdef NM_DIAG_FRAME_TRACE
uint32_t nm_ft_group[NM_FT_GROUPS],nm_ft_calls[NM_FT_GROUPS],nm_ft_emit[2][4],nm_ft_opponents;
nm_ft_object_t nm_ft_objects[NM_FT_OBJECTS];
nm_ft_car_t nm_ft_cars[NM_FT_CARS];
nm_ft_part_t nm_ft_parts[NM_FT_PARTS];
uint32_t nm_ft_object_count,nm_ft_car_count;
void nm_ft_render_counts(uint32_t out[3]) { out[0]=draw_count;out[1]=draw_header_count;out[2]=sq_ta_vertices; }
#endif


/* The value of one material binding for the emission table
 * (model_submission.h, sq_emit_records_build): its PVR header compiled into
 * `scratch` (32-byte aligned: pvr_poly_compile allocates the line with
 * MOVCA.L), colour, UV mapping and depth class. Binding format, filter and
 * list are constants of the loaded packs. A palette scenery binding whose
 * texture the selected stage does not load (nm_world_materials_upload) has
 * no face and gets no header: cmd stays 0. */
static int draw_emit_value(unsigned s,unsigned index,nm_sq_emit_t *out,pvr_poly_hdr_t *scratch) {
    const nm_material_pack_t *materials=s ? world_draw.materials : draw_materials;
    const nm_material_binding_t *binding=materials->bindings+index;
    const nm_material_uv_t uv=nm_material_uv(materials,binding);
    nm_memset(scratch,0,sizeof(*scratch));
    if(!(s && nm_world_materials_palettized() && binding->resource!=NM_MATERIAL_SOLID &&
         !nm_world_materials_texture(binding->resource)) && !compile_draw_header(binding,scratch,(int)s)) return 0;
    out->header=scratch;
    out->argb=nm_material_argb(materials,binding);
    out->origin[0]=uv.origin[0]; out->origin[1]=uv.origin[1];
    out->scale[0]=uv.scale[0]; out->scale[1]=uv.scale[1];
    out->depth=s && sq_surface_bindings && sq_surface_bindings[index] ? sq_road_depth : 1.0f;
    out->padding=0;
    return 1;
}

/* Every binding of a source against a second, independent compile: what the
 * emission table gives for it (its class and the pooled header that class
 * names) is that header, colour, UV mapping and depth, byte for byte. Run
 * when the table is built and again for the cars once the world is loaded. */
static int check_resident_headers(unsigned s) {
    const nm_material_pack_t *materials=s ? world_draw.materials : draw_materials;
    unsigned absent=0;
    for(unsigned i=0;i<materials->count;++i) {
        if(!(i&511u)) nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        alignas(32) pvr_poly_hdr_t reference;
        nm_sq_emit_t expected,stored;
        if(!sq_emit_records[s] || !draw_emit_value(s,i,&expected,&reference)) return 0;
        sq_emit_expand(&stored,sq_emit_records[s]+i);
        if(memcmp(stored.header,&reference,sizeof(reference)) || stored.argb!=expected.argb ||
           memcmp(stored.origin,expected.origin,sizeof(stored.origin)) || memcmp(stored.scale,expected.scale,sizeof(stored.scale)) ||
           memcmp(&stored.depth,&expected.depth,sizeof(stored.depth))) {
            printf("FAIL: emission table differs from the compiled header of source %u binding %u\n",s,i);
            return 0;
        }
        absent+=!reference.cmd;
    }
    printf("Native resident headers startup PASS: source=%d bindings=%u (all32 bytes), %u without a loaded texture\n",
        (int)s,(unsigned)materials->count-absent,absent);
    return 1;
}

/* Two bindings submit the same header exactly when their classes name the
 * same one (the emitters compare addresses): no two pooled headers of the
 * two sources are equal. */
static int check_header_pools(void) {
    unsigned absent=0,with_header=0;
    for(unsigned s=0;s<2;++s) {
        const nm_material_pack_t *materials=s ? world_draw.materials : draw_materials;
        if(!sq_emit_records[s]) return 0;
        for(unsigned i=0;i<materials->count;++i) {
            if(sq_emit_records[s][i].group->header->cmd) ++with_header; else ++absent;
        }
        for(unsigned i=0;i<sq_header_pool_count[s];++i) for(unsigned t=(nm_loader_yield(0),s);t<2;++t)
            for(unsigned j=t==s ? i+1 : 0;j<sq_header_pool_count[t];++j)
                if(!memcmp(sq_header_pool[s]+i,sq_header_pool[t]+j,sizeof(pvr_poly_hdr_t))) {
                    puts("FAIL: header pool holds one header twice"); return 0;
                }
    }
    const unsigned unique=sq_header_pool_count[0]+sq_header_pool_count[1];
    printf("Native shared headers PASS: bindings=%u unique=%u (all32 bytes), %u without header\n",with_header,unique,absent);
    printf("Native header pool: %u headers, %u bytes (cars %u, world %u more and %u of the cars'); emission classes cars %u, world %u, %u bytes; "
        "bindings %u bytes; no per-binding header/UV cache\n",unique,unique*(unsigned)sizeof(pvr_poly_hdr_t),
        sq_header_pool_count[0],sq_header_pool_count[1],sq_header_borrowed[1],sq_emit_class_count[0],sq_emit_class_count[1],
        (sq_emit_class_count[0]+sq_emit_class_count[1])*(unsigned)sizeof(nm_sq_class_t),
        (unsigned)((draw_materials->count+world_draw.materials->count)*sizeof(nm_sq_bind_t)));
    return 1;
}

static void report_face_layers(const char *name,const nm_pack_t *pack) {
    const nm_header_t *original=pack->data;
    unsigned layered=0,strips=0;
    unsigned faces=0;
    /* Live mesh ranges: the reduced view compacts faces and strips per mesh. */
    for(unsigned m=0;m<original->meshes;++m) {
        const nm_mesh_t *mesh=pack->meshes+m;
        for(unsigned f=0;f<mesh->face_count;++f) layered+=pack->faces[mesh->face_first+f].layer!=0;
        faces+=mesh->face_count;
        if(pack->strips) strips+=mesh->padding[3];
    }
    printf("Native %s draw order: %s; %u of %u faces on a layer above their plane (depth step %g per layer); %u offline strips\n",
        name,(original->magic&~NM_PACK_ALIGNED)==NM_PACK_MAGIC_LAYERED ? "DNM6, sorted offline" : "source order",
        layered,faces,(double)NM_LAYER_DEPTH_STEP,strips);
}

/* Mesh UVs and every exported luma variant are immutable. Validate their
 * cross-pack crop contract once, including faces outside the startup camera. */
static int check_material_uv_ranges(const nm_pack_t *pack, const nm_material_pack_t *materials,
        const uint16_t *lookup, const uint32_t *runs) {
    uint16_t (*bounds)[4]=nm_heap_alloc("UV range scratch",0,pack->header->materials*sizeof(*bounds));
    if(!bounds) return 0;
    for(unsigned m=0;m<pack->header->materials;++m) {
        bounds[m][0]=bounds[m][1]=UINT16_MAX; bounds[m][2]=bounds[m][3]=0;
    }
    for(unsigned m=0;m<pack->header->meshes;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        const nm_mesh_t *mesh=pack->meshes+m;
        for(unsigned f=0;f<mesh->face_count;++f) {
            const nm_face_t *face=pack->faces+mesh->face_first+f;
            if(!(face->attribute&(3u<<8)) || (pack->materials[face->material].header[0]&0x8000)) continue;
            uint16_t *b=bounds[face->material];
            for(unsigned v=0;v<face->count;++v) {
                const nm_vertex_t *p=pack->vertices+mesh->vertex_first+face->indices[v];
                if(p->u<b[0]) b[0]=p->u;
                if(p->v<b[1]) b[1]=p->v;
                if(p->u>b[2]) b[2]=p->u;
                if(p->v>b[3]) b[3]=p->v;
            }
        }
    }
    unsigned checked=0;
    for(unsigned m=0;m<pack->header->materials;++m) if(nm_loader_yield(0),bounds[m][0]<=bounds[m][2])
        for(unsigned l=0;l<256;++l) {
            const unsigned index=sq_lookup(lookup,runs,m,l);
            if(index==UINT16_MAX) continue; /* Missing light still fails at use. */
            const nm_material_uv_t uv=nm_material_uv(materials,materials->bindings+index);
            if(!nm_material_uv_contains(&uv,bounds[m][0],bounds[m][1]) ||
               !nm_material_uv_contains(&uv,bounds[m][2],bounds[m][3])) {
                printf("FAIL: immutable UV range material=%u luma=%u\n",m,l);
                free(bounds); return 0;
            }
            ++checked;
        }
    free(bounds);
    printf("Native immutable UV ranges PASS: %u material/luma variants\n",checked);
    return 1;
}

/* The strip inspection view shows which list a polygon is on: opaque-list
 * polygons solid in their debug colour, punch-through polygons through a
 * checkerboard of that colour with every other texel punched out. The
 * texture is 8x8 ARGB1555 (128 bytes of VRAM, twiddled; no palette), point
 * sampled, with screen-space UVs: one texel covers SQ_STIPPLE_PIXELS output
 * pixels (model_submission.h). It is allocated when the view is first switched
 * on and freed with the renderer: nothing in VRAM and nothing per frame in
 * normal play. Without VRAM for it the view stays solid as before. */
static pvr_ptr_t draw_inspect_texture;
static void draw_inspect_stipple_release(void) {
    if(draw_inspect_texture) pvr_mem_free(draw_inspect_texture);
    draw_inspect_texture=NULL; draw_inspect_stipple=0;
}
static void draw_inspect_stipple_prepare(void) {
    if(draw_inspect_stipple) return;
    draw_inspect_texture=pvr_mem_malloc(8*8*sizeof(uint16_t));
    if(!draw_inspect_texture) return;
    /* Twiddled order interleaves the bits of x and y: the parity of x+y is
     * the parity of the two lowest index bits. */
    uint16_t *texels=(uint16_t *)draw_inspect_texture;
    for(unsigned i=0;i<64;++i) texels[i]=((i^(i>>1))&1) ? 0xffffu : 0x0000u;
    pvr_poly_cxt_t context;
    pvr_poly_cxt_txr(&context,PVR_LIST_PT_POLY,PVR_TXRFMT_ARGB1555,8,8,draw_inspect_texture,PVR_FILTER_NONE);
    context.gen.culling=PVR_CULLING_NONE;
    context.depth.comparison=PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(draw_inspect_headers+1,&context);
    nm_memset((char *)(draw_inspect_headers+1)+16,0,16);
    draw_inspect_stipple=1;
}
void nm_render_inspect_set_strips(int enabled) {
    draw_inspect_strips=enabled!=0;
    if(draw_inspect_strips) draw_inspect_stipple_prepare();
}
int nm_render_inspect_strips(void) { return draw_inspect_strips; }

int nm_render_init(const nm_pack_t *pack) {
#ifdef DAYTONA_UNTEXTURED_BENCH
    puts("Native untextured benchmark: solid OP/PT, no emitted UV; normal lighting/material selection retained; cutout coverage differs");
#endif
    draw_inspect_strips=0;
    draw_inspect_stipple_release(); /* the solid headers are compiled below */
    draw_layer_scale_init();
    for(unsigned pass=0;pass<2;++pass) {
        pvr_poly_cxt_t context;
        pvr_poly_cxt_col(&context,draw_lists[pass]);
        context.gen.culling=PVR_CULLING_NONE;
        context.depth.comparison=PVR_DEPTHCMP_GEQUAL;
        pvr_poly_compile(draw_inspect_headers+pass,&context);
        nm_memset((char *)(draw_inspect_headers+pass)+16,0,16);
    }
    if (draw_pack) return 0;
#ifdef NATIVE_CACHE_PROFILE
    perf_cntr_event_t event;perf_cntr_clock_t clock;
    const int running=perf_cntr_config(PRFC1,&event,&clock);
    const uint64_t count=perf_cntr_count(PRFC1);
    printf("Native cache counter probe: PRFC1 running=%d event=%u clock=%u count=%llu timer=%d\n",
        running,(unsigned)event,(unsigned)clock,(unsigned long long)count,perf_cntr_timer_enabled());
    /* This finite standalone build has no PRFC1 consumer (perf_monitor is
     * excluded above). Hardware may retain an undefined event47/count0 state
     * across loader launches; initialize our counter rather than assume reset. */
    if (!perf_cntr_timer_enabled()) {
        puts("FAIL: cache profile requires intact KOS timer");return 0;
    }
    perf_cntr_start(PRFC1,NATIVE_CACHE_PROFILE_EVENT,PMCR_COUNT_CPU_CYCLES);
    draw_cache_counter_owned=1;
    if (!perf_cntr_config(PRFC1,&event,&clock) ||
        event!=NATIVE_CACHE_PROFILE_EVENT || clock!=PMCR_COUNT_CPU_CYCLES) {
        puts("FAIL: PRFC1 did not accept profiling event");return 0;
    }
    printf("Native render PMU event=%u clock=CPU; paired active-time/event scopes, instrumented\n",(unsigned)event);
#endif
#ifdef NATIVE_BILINEAR
    puts(NM_ROAD_FILTER ? "Native texture filtering: OP bilinear including the road surface class (NATIVE_ROAD_FILTER=1); PT/sky/UI nearest" :
        "Native texture filtering: OP bilinear except the road surface class (point sampled); PT/sky/UI nearest");
#else
    puts("Native texture filtering: unfiltered (point sampling on every list; mip chains kept, level chosen by the PVR)");
#endif
#ifdef NATIVE_PROFILE
    /* Borrow KOS's default CPU timer; never reconfigure engine-owned counters.
     * Only phase intervals use it. Whole-frame/fence times remain wall time. */
    if (!perf_cntr_timer_enabled()) { puts("FAIL: native phase profile needs KOS CPU timer"); return 0; }
#endif
    draw_pack = pack;
    nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
    report_face_layers("car",pack);
    nm_loader_yield(0);
    unsigned face_capacity = 0;
    for (unsigned i = 0; i < pack->header->meshes; ++i)
        if (pack->meshes[i].face_count > face_capacity) face_capacity = pack->meshes[i].face_count;
    draw_groups = nm_heap_alloc("face group scratch",32,face_capacity * sizeof(*draw_groups));
    if (!draw_groups) return 0;
    draw_group_capacity = face_capacity;
    /* ponytail: dense 256-luma lookup per source material, capped at 512 KiB.
     * Sector-specific compact indices replace this if inventories exceed it.
     * It indexes observed variants, never quantizes or substitutes a light. */
    if (pack->header->materials > 1024) return 0;
    draw_materials = nm_materials_index();
    /* One run a material (model_submission.h sq_lookup) when the pack allows it, else the dense table. */
    draw_runs = nm_heap_alloc("car material/luma lookup",32,pack->header->materials * sizeof(*draw_runs));
    if (!draw_runs) return 0;
    if (!sq_luma_runs_build(pack,draw_materials,draw_runs)) {
        free(draw_runs); draw_runs = NULL;
        draw_bindings = nm_heap_alloc("car material/luma lookup",32,pack->header->materials * 256 * sizeof(*draw_bindings));
        if (!draw_bindings) return 0;
        for (unsigned m = 0; m < pack->header->materials; ++m) {
            nm_loader_yield(0);
            const nm_material_binding_t *row[256];
            nm_material_find_row(draw_materials, pack->materials[m].header, row);
            for (unsigned l = 0; l < 256; ++l)
                draw_bindings[m*256+l] = row[l] ? (uint16_t)(row[l] - draw_materials->bindings) : UINT16_MAX;
        }
    }
    printf("Native car material/luma table: %s (%u materials, %u bytes)\n",draw_runs ? "one run a material" : "dense",
        (unsigned)pack->header->materials,(unsigned)(pack->header->materials*(draw_runs ? 4u : 512u)));
    /* nm_loader_yield between the steps: a load in another screen's frames (scene_loader.h). */
    nm_loader_yield(0);
    if(!check_material_uv_ranges(pack,draw_materials,draw_bindings,draw_runs)) return 0;
    nm_loader_yield(0);
    sq_material_lists_prepare(0,pack,draw_materials,draw_bindings,draw_runs);
    nm_loader_yield(0);
    { static int point_checked; if(!point_checked && !sq_write_point_check()) return 0; point_checked=1; }
    nm_loader_yield(0);
    if(!sq_flat_strips_prepare(0,pack,draw_materials)) return 0;
    nm_loader_yield(0);
    if(!sq_emit_faces_prepare(0,pack)) return 0;
    nm_loader_yield(0);
    /* The code-against-code checks of the writers and the SH4ZAM forms, on fixed cases: once per power-on
     * (the attract loop reloads the renderer at every course; nm_loader_yield between them). */
    static int checked_once;
    if(!checked_once) {
#if SQ_STREAMS && defined(__DREAMCAST__)
#ifndef NATIVE_BOUNDS_VERIFY
    if(!sq_stream_transform_check()) return 0;
    nm_loader_yield(0);
#endif
    if(!sq_stream_face_check()) return 0;
    nm_loader_yield(0);
    if(!sq_stream_run_check()) return 0;
    nm_loader_yield(0);
#endif
#if SQ_CAR_STREAMS && defined(__DREAMCAST__)
    if(!sq_car_face_check()) return 0;
    nm_loader_yield(0);
#endif
#if NM_SH4ZAM_GFX && defined(__DREAMCAST__)
    /* The SH4ZAM forms of the renderer (model_submission.h, NM_SH4ZAM_GFX)
     * against the exact ones on fixed cases. */
    if(!sq_class_light_check()) return 0;
    nm_loader_yield(0);
    if(!sq_clip_check()) return 0;
    nm_loader_yield(0);
#endif
#if SQ_SWEEPS && defined(__DREAMCAST__)
    /* The catch-fence sweep's cut (model_submission.h, NM_FENCE_SWEEP) against the exact one. */
    if(!sq_sweep_check()) return 0;
    nm_loader_yield(0);
#endif
    checked_once=1;
    }
#ifdef __DREAMCAST__
    if(!sq_facing_prepare(0,pack)) return 0;
    nm_loader_yield(0);
#endif
    if(!sq_allocate(0,pack)) return 0;
    nm_loader_yield(0);
    if(!sq_translucent_prepare(pack)) return 0;
    nm_loader_yield(0);
    /* Every car header, compiled once into the pool of the emission classes
     * and checked against a second compile. */
    if(!sq_emit_records_build(0,draw_materials->count,draw_emit_value)) return 0;
    nm_loader_yield(0);
    if(!check_resident_headers(0)) return 0;
    nm_loader_yield(0);
    /* The prebuilt car streams (model_submission.h, NM_SQ_CAR_STREAM): from the
     * emission records, emission faces, plane constants and light classes above. */
    if(!sq_car_stream_prepare(pack)) return 0;
    return 1;
}


int nm_render_car_material_header(unsigned material,const uint16_t header[4]) {
    if(!draw_pack || !draw_materials || !header || material>=draw_pack->header->materials) return 0;
    uint16_t next[256];
    for(unsigned l=0;l<256;++l) {
        const nm_material_binding_t *binding=nm_material_find(draw_materials,header,l);
        next[l]=binding ? (uint16_t)(binding-draw_materials->bindings) : UINT16_MAX;
        if(next[l]==UINT16_MAX && sq_lookup(draw_bindings,draw_runs,material,l)!=UINT16_MAX) return 0;
    }
    if(draw_runs) {
        unsigned lo=256;
        for(unsigned l=0;l<256 && lo==256;++l) if(next[l]!=UINT16_MAX) lo=l;
        if(lo==256) return 0;
        for(unsigned l=lo;l<256;++l) if(next[l]!=(uint16_t)(next[lo]+(l-lo))) return 0;
        draw_runs[material]=(uint32_t)next[lo]|(uint32_t)lo<<16;
    } else nm_memcpy(draw_bindings+material*256u,next,sizeof(next));
    return 1;
}

void nm_render_world_capacity(unsigned points,unsigned faces) {
    sq_world_point_bound=points;sq_world_face_bound=faces;
}
void nm_render_world_streams(int enabled) { sq_stream_wanted=enabled!=0; }
float nm_render_road_depth(float scale) { sq_road_depth_set(scale); return sq_road_depth; }
unsigned nm_render_world_stream_idle(unsigned budget_us) { return sq_stream_idle(budget_us); }

/* Frame-counter animations (include/clock_actors.h): the course's DNA1 file, owned by the scene
 * (render/scene/practice_assets.c), and what the renderer keeps of it. */
static struct {
    const void *data;                     /* NULL: none */
    unsigned groups;
    uint16_t *bindings;                   /* front and back binding of each state of every group (UINT16_MAX: none) */
    uint32_t first[NM_CLOCK_GROUPS_MAX];  /* each group's first state pair in bindings */
    uint32_t *uv_corners;                 /* per corner of a face that uses a vertex with changing u, v: face<<2|corner,
                                           * then the vertex's slot in the group's u, v sets */
    uint32_t uv_first[NM_CLOCK_GROUPS_MAX+1];
    int16_t applied[NM_CLOCK_GROUPS_MAX]; /* the frame the tables hold; -1: frame 0 of the load */
    uint8_t live[NM_CLOCK_GROUPS_MAX];    /* 1: its mesh kept every face (scenery stage) */
    /* PLACED groups: a row of 256 bindings (one a luma) per material their faces use, and each face's row. */
    uint16_t *light_rows;
    uint16_t *light_face_row;             /* per face of the PLACED groups' meshes, from light_first */
    uint32_t light_first[NM_CLOCK_GROUPS_MAX];
    /* The key the baked lumas of each PLACED group hold (clock_light.h); light_valid 0 after a frame change or the
     * attach. light_specular: a face of the group has a specular material (the key then reads the view too). */
    nm_clock_light_key_t light_key[NM_CLOCK_GROUPS_MAX];
    uint8_t light_valid[NM_CLOCK_GROUPS_MAX],light_specular[NM_CLOCK_GROUPS_MAX];
    unsigned light_updates,light_kept;   /* placements lit; placements whose key equalled the last lit */
} world_clock;
static void clock_release(void) {
    free(world_clock.bindings); free(world_clock.uv_corners); free(sq_clock_meshes);
    free(world_clock.light_rows); free(world_clock.light_face_row);
    world_clock.bindings=NULL; world_clock.uv_corners=NULL; sq_clock_meshes=NULL; world_clock.groups=0;
    world_clock.light_rows=NULL; world_clock.light_face_row=NULL;
}
int nm_render_world_clocks(const void *data) {
    if(world_draw.pack) return 0;
    world_clock.data=data;
    return 1;
}
static void clock_extend(float bounds[6],const float p[3]) {
    for(unsigned a=0;a<3;++a) { if(p[a]<bounds[a]) bounds[a]=p[a]; if(p[a]>bounds[3+a]) bounds[3+a]=p[a]; }
}
/* At attach, before anything reads them: the bounds of each group's mesh and face groups enclose every stored
 * frame (interpolated frames lie between two of them). 0: no memory. */
static int clock_attach_bounds(const nm_pack_t *pack) {
    clock_release();
    if(!world_clock.data) return 1;
    const nm_clock_header_t *h=world_clock.data;
    const nm_header_t *original=pack->data;
    sq_clock_meshes=nm_calloc(original->meshes,1);
    if(!sq_clock_meshes) return 0;
    world_clock.groups=h->groups;
    for(unsigned i=0;i<h->groups;++i) {
        const nm_clock_group_t *g=nm_clock_groups(world_clock.data)+i;
        nm_mesh_t *mesh=(nm_mesh_t *)pack->meshes+g->mesh;
        world_clock.applied[i]=-1; world_clock.light_valid[i]=0; world_clock.light_specular[i]=0;
        world_clock.live[i]=mesh->face_count==g->faces && mesh->vertex_count==g->vertices;
        if(!world_clock.live[i]) continue;
        sq_clock_meshes[g->mesh]=1;
        if(!g->rate || !g->points) continue;
        const uint16_t *vertex_point=nm_clock_table(world_clock.data,g,NM_CLOCK_VERTEX_POINT);
        const int16_t *positions=nm_clock_table(world_clock.data,g,NM_CLOCK_POSITIONS);
        const nm_vertex_t *vertices=pack->vertices+mesh->vertex_first;
        /* nm_loader_yield: a frame, and a stretch of its faces, a step (the bounds only grow: the order is free). */
        for(unsigned k=0;k<nm_clock_stored(g);++k) {
            nm_loader_yield(0);
            const int16_t *frame=positions+3u*g->points*k;
            for(unsigned v=0;v<g->vertices;++v) if(vertex_point[v]!=NM_CLOCK_STILL) {
                float p[3]; nm_clock_point(g,frame,frame,0,vertex_point[v],p); clock_extend(mesh->bounds,p);
            }
            if(!nm_pack_has_groups(pack)) continue;
            for(unsigned f=0;f<mesh->face_count;++f) {
                if(!(f&31u)) nm_loader_yield(0);
                const nm_face_t *face=pack->faces+mesh->face_first+f;
                float *bounds=(float *)nm_pack_group_bounds(pack,face->group);
                for(unsigned c=0;c<face->count;++c) {
                    const unsigned v=face->indices[c];
                    float p[3];
                    if(vertex_point[v]==NM_CLOCK_STILL) nm_memcpy(p,vertices[v].position,sizeof(p));
                    else nm_clock_point(g,frame,frame,0,vertex_point[v],p);
                    clock_extend(bounds,p);
                }
            }
        }
    }
    return 1;
}
/* With the world's material pack, before its upload: each state's bindings (their textures marked resident), and
 * every binding a two-sided face of a frame needs. 0: a binding missing, or no memory. */
static int clock_attach_bindings(const nm_pack_t *pack,const nm_material_pack_t *materials,uint8_t *used) {
    if(!world_clock.data) return 1;
    unsigned states=0,corners=0;
    for(unsigned i=0;i<world_clock.groups;++i) {
        const nm_clock_group_t *g=nm_clock_groups(world_clock.data)+i;
        world_clock.first[i]=states; states+=g->states;
        if(!world_clock.live[i] || !g->uv_vertices) continue;
        const nm_mesh_t *mesh=pack->meshes+g->mesh;
        for(unsigned f=0;f<mesh->face_count;++f) corners+=pack->faces[mesh->face_first+f].count;
    }
    world_clock.bindings=nm_heap_alloc("world clock bindings",32,2u*states*sizeof(uint16_t));
    world_clock.uv_corners=nm_heap_alloc("world clock u, v corners",32,(2u*corners+1u)*sizeof(uint32_t));
    if(!world_clock.bindings || !world_clock.uv_corners) return 0;
    unsigned at=0;
    for(unsigned i=0;i<world_clock.groups;++i) {
        const nm_clock_group_t *g=nm_clock_groups(world_clock.data)+i;
        world_clock.uv_first[i]=at;
        const nm_clock_state_t *table=nm_clock_table(world_clock.data,g,NM_CLOCK_STATES);
        uint16_t *out=world_clock.bindings+2u*world_clock.first[i];
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): a group, a frame, some faces a step */
        for(unsigned s=0;s<g->states;++s) {
            if(!(s&63u)) nm_loader_yield(0);
            for(unsigned side=0;side<2;++side) {
                const nm_material_binding_t *b=nm_material_find(materials,table[s].header,table[s].luma[side]);
                out[2u*s+side]=b ? (uint16_t)(b-materials->bindings) : UINT16_MAX;
            }
        }
        if(!world_clock.live[i]) continue;
        const nm_mesh_t *mesh=pack->meshes+g->mesh;
        const uint16_t *state_faces=nm_clock_table(world_clock.data,g,NM_CLOCK_STATE_FACES);
        const uint16_t *face_states=nm_clock_table(world_clock.data,g,NM_CLOCK_FACE_STATES);
        for(unsigned k=0;k<g->frames;++k) for(unsigned j=(nm_loader_yield(0),0u);j<g->state_faces;++j) {
            const unsigned s=face_states[k*g->state_faces+j];
            const nm_face_t *face=pack->faces+mesh->face_first+state_faces[j];
            const int two=(face->attribute&(1u<<17))!=0;
            if(out[2u*s]==UINT16_MAX || (two && out[2u*s+1u]==UINT16_MAX)) {
                const nm_clock_state_t *st=table+s;
                printf("FAIL: world clock group %u (mesh %u) frame %u face %u: no binding for header %04x %04x %04x %04x "
                    "luma %u/%u\n",i,(unsigned)g->mesh,k,(unsigned)state_faces[j],st->header[0],st->header[1],
                    st->header[2],st->header[3],st->luma[0],st->luma[1]);
                return 0;
            }
            for(unsigned side=0;side<(two ? 2u : 1u);++side) {
                const nm_material_binding_t *b=materials->bindings+out[2u*s+side];
                if(b->resource!=NM_MATERIAL_SOLID) used[b->resource]=1;
            }
        }
        if(!g->uv_vertices) continue;
        const uint16_t *uv_vertices=nm_clock_table(world_clock.data,g,NM_CLOCK_UV_VERTICES);
        for(unsigned f=0;f<mesh->face_count;++f) {
            if(!(f&15u)) nm_loader_yield(0);
            const nm_face_t *face=pack->faces+mesh->face_first+f;
            for(unsigned c=0;c<face->count;++c)
                for(unsigned slot=0;slot<g->uv_vertices;++slot)
                    if(uv_vertices[slot]==face->indices[c]) {
                        world_clock.uv_corners[2u*at]=(mesh->face_first+f)<<2|c; world_clock.uv_corners[2u*at+1u]=slot; ++at;
                    }
        }
    }
    world_clock.uv_first[world_clock.groups]=at;
    /* PLACED groups: every luma a face can show (its material's ambient up: the export's keys hold them). */
    unsigned placed_faces=0,rows=0;
    for(unsigned i=0;i<world_clock.groups;++i) {
        const nm_clock_group_t *g=nm_clock_groups(world_clock.data)+i;
        world_clock.light_first[i]=placed_faces;
        if(world_clock.live[i] && (g->flags&NM_CLOCK_GROUP_PLACED)) placed_faces+=pack->meshes[g->mesh].face_count;
    }
    if(placed_faces) {
        uint16_t *row_of=nm_calloc(pack->header->materials,sizeof(*row_of));
        world_clock.light_face_row=nm_heap_alloc("world clock light faces",32,placed_faces*sizeof(uint16_t));
        if(!row_of || !world_clock.light_face_row) { free(row_of); return 0; }
        for(unsigned i=0;i<world_clock.groups;++i) {
            const nm_clock_group_t *g=nm_clock_groups(world_clock.data)+i;
            if(!world_clock.live[i] || !(g->flags&NM_CLOCK_GROUP_PLACED)) continue;
            const nm_mesh_t *mesh=pack->meshes+g->mesh;
            nm_loader_yield(0);
            for(unsigned f=0;f<mesh->face_count;++f) {
                const unsigned m=pack->faces[mesh->face_first+f].material;
                if(!row_of[m]) row_of[m]=(uint16_t)++rows;
                world_clock.light_face_row[world_clock.light_first[i]+f]=(uint16_t)(row_of[m]-1u);
                if((mesh->mode&1u) && pack->materials[m].specular_control) world_clock.light_specular[i]=1;
            }
        }
        world_clock.light_rows=nm_heap_alloc("world clock light rows",32,rows*256u*sizeof(uint16_t));
        if(!world_clock.light_rows) { free(row_of); return 0; }
        for(unsigned m=0;m<pack->header->materials;++m) if(row_of[m]) {
            const nm_material_t *material=pack->materials+m;
            uint16_t *row=world_clock.light_rows+(row_of[m]-1u)*256u;
            const nm_material_binding_t *found[256];
            nm_loader_yield(0);
            nm_material_find_row(materials,material->header,found);
            for(unsigned luma=0;luma<256;++luma) {
                const nm_material_binding_t *b=found[luma];
                row[luma]=b ? (uint16_t)(b-materials->bindings) : UINT16_MAX;
                if(luma<(unsigned)material->ambient) continue;
                if(!b) {
                    printf("FAIL: world clock material %u (header %04x %04x %04x %04x): no binding for luma %u\n",m,
                        material->header[0],material->header[1],material->header[2],material->header[3],luma);
                    free(row_of); return 0;
                }
                if(b->resource!=NM_MATERIAL_SOLID) used[b->resource]=1;
            }
        }
        free(row_of);
    }
    unsigned live=0;
    for(unsigned i=0;i<world_clock.groups;++i) live+=world_clock.live[i];
    printf("Native world clock animations: %u of %u groups, %u states, %u u, v corners, %u placed faces, %u light rows; "
        "tables=%u bytes\n",live,world_clock.groups,states,at,placed_faces,rows,
        (unsigned)(2u*states*sizeof(uint16_t)+(2u*corners+1u)*sizeof(uint32_t)+placed_faces*2u+rows*512u));
    return 1;
}
/* After the emission faces and plane constants exist: no offline strip run starts at a face of a group, and a
 * group whose positions or normals move finds every side by the reference test. */
static void clock_attach_tables(const nm_pack_t *pack) {
    for(unsigned i=0;world_clock.data && i<world_clock.groups;++i) {
        const nm_clock_group_t *g=nm_clock_groups(world_clock.data)+i;
        if(!world_clock.live[i]) continue;
        const nm_mesh_t *mesh=pack->meshes+g->mesh;
        for(unsigned f=0;f<mesh->face_count;++f) {
            if(sq_emit_faces[1]) sq_emit_faces[1][mesh->face_first+f].special&=(uint16_t)~SQ_FACE_RUN;
#ifdef __DREAMCAST__
            if(g->rate && sq_face_planes[1]) { sq_face_planes[1][mesh->face_first+f].plane=0; sq_face_planes[1][mesh->face_first+f].reach=HUGE_VALF; }
#endif
        }
    }
}
void nm_render_world_clock_frame(unsigned group,unsigned frame) {
    const nm_pack_t *pack=world_draw.pack;
    if(!pack || !world_clock.data || group>=world_clock.groups || !world_clock.live[group] ||
       world_clock.applied[group]==(int)frame || !sq_baked_faces) return;
    NM_FT_BEGIN(ft_frame);
    const nm_clock_group_t *g=nm_clock_groups(world_clock.data)+group;
    if(frame>=g->frames) return;
    world_clock.applied[group]=(int16_t)frame;
    world_clock.light_valid[group]=0;   /* a PLACED group's normals and state bindings change: lit again */
    const nm_mesh_t *mesh=pack->meshes+g->mesh;
    nm_vertex_t *vertices=(nm_vertex_t *)pack->vertices+mesh->vertex_first;
    nm_face_t *faces=(nm_face_t *)pack->faces+mesh->face_first;
    if(g->rate) {
        unsigned a,b; float t;
        nm_clock_between(g,frame,&a,&b,&t);
        if(g->points) {
            const uint16_t *vertex_point=nm_clock_table(world_clock.data,g,NM_CLOCK_VERTEX_POINT);
            const int16_t *positions=nm_clock_table(world_clock.data,g,NM_CLOCK_POSITIONS);
            const int16_t *pa=positions+3u*g->points*a,*pb=positions+3u*g->points*b;
            if(g->points<=NM_CLOCK_MOVED_POINTS) {
                /* Each point once, then its vertex records (a corner of a face each: a horse's 155 points are 423
                 * records; per record, the horses took 1.4 ms of a frame on hardware). */
                alignas(32) float moved[NM_CLOCK_MOVED_POINTS][3];
                if(g->rate==1) for(unsigned p=0;p<g->points;++p) nm_clock_point_stored(g,pa,p,moved[p]);
                else for(unsigned p=0;p<g->points;++p) nm_clock_point(g,pa,pb,t,p,moved[p]);
                for(unsigned v=0;v<g->vertices;++v) {
                    const unsigned p=vertex_point[v];
                    if(p==NM_CLOCK_STILL) continue;
                    float *const out=vertices[v].position;
                    out[0]=moved[p][0]; out[1]=moved[p][1]; out[2]=moved[p][2];
                }
            } else for(unsigned v=0;v<g->vertices;++v)
                if(vertex_point[v]!=NM_CLOCK_STILL) nm_clock_point(g,pa,pb,t,vertex_point[v],vertices[v].position);
        }
        const uint16_t *normal_faces=nm_clock_table(world_clock.data,g,NM_CLOCK_NORMAL_FACES);
        const int16_t *normals=nm_clock_table(world_clock.data,g,NM_CLOCK_NORMALS);
        const int16_t *na=normals+4u*g->normal_faces*a,*nb=normals+4u*g->normal_faces*b;
        if(g->rate==1) for(unsigned j=0;j<g->normal_faces;++j)
            for(unsigned axis=0;axis<3;++axis)
                faces[normal_faces[j]].normal[axis]=nm_clock_normal_stored(na[4u*j+axis]);
        else for(unsigned j=0;j<g->normal_faces;++j)
            for(unsigned axis=0;axis<3;++axis)
                faces[normal_faces[j]].normal[axis]=nm_clock_normal(na[4u*j+axis],nb[4u*j+axis],t);
    }
    const uint16_t *state_faces=nm_clock_table(world_clock.data,g,NM_CLOCK_STATE_FACES);
    const uint16_t *face_states=(const uint16_t *)nm_clock_table(world_clock.data,g,NM_CLOCK_FACE_STATES)+frame*g->state_faces;
    const uint16_t *bindings=world_clock.bindings+2u*world_clock.first[group];
    for(unsigned j=0;j<g->state_faces;++j) {
        const unsigned f=state_faces[j],s=face_states[j];
        const int two=(faces[f].attribute&(1u<<17))!=0;
        sq_baked_faces[mesh->face_first+f]=(nm_baked_face_t){bindings[2u*s],two ? bindings[2u*s+1u] : (uint16_t)UINT16_MAX};
    }
    if(g->uv_vertices) {
        const uint16_t *uv_vertices=nm_clock_table(world_clock.data,g,NM_CLOCK_UV_VERTICES);
        const uint16_t *uv_frames=nm_clock_table(world_clock.data,g,NM_CLOCK_UV_FRAMES);
        const uint16_t *set=(const uint16_t *)nm_clock_table(world_clock.data,g,NM_CLOCK_UV_SETS)+2u*g->uv_vertices*uv_frames[frame];
        for(unsigned slot=0;slot<g->uv_vertices;++slot) {
            vertices[uv_vertices[slot]].u=set[2u*slot]; vertices[uv_vertices[slot]].v=set[2u*slot+1u];
        }
        if(sq_emit_faces[1])
            for(unsigned c=world_clock.uv_first[group];c<world_clock.uv_first[group+1u];++c) {
                const uint32_t at=world_clock.uv_corners[2u*c],slot=world_clock.uv_corners[2u*c+1u];
                sq_emit_faces[1][at>>2].uv[at&3u][0]=set[2u*slot]; sq_emit_faces[1][at>>2].uv[at&3u][1]=set[2u*slot+1u];
            }
    }
    NM_FT_END(NM_FT_S_FRAME,ft_frame);
}

void nm_render_world_clock_light(unsigned group,const float model[12],const float placed[12],const float view[12]) {
    const nm_pack_t *pack=world_draw.pack;
    if(!pack || !world_clock.data || group>=world_clock.groups || !world_clock.live[group] || !sq_baked_faces ||
       !world_clock.light_rows) return;
    const nm_clock_group_t *g=nm_clock_groups(world_clock.data)+group;
    if(!(g->flags&NM_CLOCK_GROUP_PLACED)) return;
    /* clock_light.h: the lumas depend on the frame's normals and this key alone. The same key as the last placement
     * lit (the tick repeated by a frame, a standing actor) leaves the baked lumas as they are. */
    nm_clock_light_key_t key;
    nm_clock_light_key(&key,model,placed,view,nm_race_world_light,world_clock.light_specular[group]);
    if(world_clock.light_valid[group] && !memcmp(&key,&world_clock.light_key[group],sizeof(key))) {
        ++world_clock.light_kept; return;
    }
    const nm_mesh_t *mesh=pack->meshes+g->mesh;
    _Static_assert(sizeof(nm_baked_face_t)==2*sizeof(uint16_t),"front, back");
    NM_FT_BEGIN(ft_light);
    nm_clock_light_faces(pack->faces+mesh->face_first,mesh->face_count,pack->materials,mesh->mode&1u,&key,
        world_clock.light_rows,world_clock.light_face_row+world_clock.light_first[group],
        (uint16_t (*)[2])(sq_baked_faces+mesh->face_first));
    NM_FT_END(NM_FT_S_LIGHT,ft_light);
    world_clock.light_key[group]=key; world_clock.light_valid[group]=1;
    ++world_clock.light_updates;
}

int nm_render_world_attach(const nm_pack_t *pack) {
    if (!draw_pack || world_draw.pack || !pack || pack->header->materials>1024) return 0;
    world_draw.pack=pack; world_draw.materials=nm_world_materials_index();
    if (!world_draw.materials->data) return 0;
    /* Frame-counter animations: their meshes' bounds hold every frame before anything reads them. */
    nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
    if(!clock_attach_bounds(pack)) return 0;
    nm_loader_yield(0);
    report_face_layers("world",pack);
    nm_loader_yield(0);
    const unsigned pack_flags=nm_pack_flags(pack);
    /* Scenery light is a constant of the pack (front luma in face.reserved,
     * back luma of two-sided faces in face.padding); nothing evaluates it here. */
    if(!(pack_flags&NM_PACK_BAKED_LIGHT)) {
        puts("FAIL: world.dnm has no baked light (DNM6 flag NM_PACK_BAKED_LIGHT; pack it with tools/prepare_face_order.py --static-light)");
        return 0;
    }
    puts("Native world light: baked in the pack (NM_PACK_BAKED_LIGHT); no light is evaluated");
    unsigned faces=0;
    const nm_pack_t *packs[2]={draw_pack,pack};
    for (unsigned p=0;p<2;++p) for (unsigned m=0;m<packs[p]->header->meshes;++m)
        if (packs[p]->meshes[m].face_count>faces) faces=packs[p]->meshes[m].face_count;
    if (faces>draw_group_capacity) { /* a larger scratch table, 32-byte aligned like the first (no realloc: it keeps no alignment) */
        void *larger=nm_heap_alloc("face group scratch",32,faces*sizeof(*draw_groups));
        if (!larger) return 0;
        free(draw_groups); draw_groups=larger; draw_group_capacity=faces;
    }
    world_draw.bindings=nm_heap_alloc("world material/luma lookup",32,pack->header->materials*256*sizeof(*world_draw.bindings));
    if (!world_draw.bindings) return 0;
    for (unsigned m=0;m<pack->header->materials;++m) {
        nm_loader_yield(0);
        const nm_material_binding_t *row[256];
        nm_material_find_row(world_draw.materials,pack->materials[m].header,row);
        for (unsigned l=0;l<256;++l) world_draw.bindings[m*256+l]=row[l] ? (uint16_t)(row[l]-world_draw.materials->bindings) : UINT16_MAX;
    }
    nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): between the steps */
    if(!check_material_uv_ranges(pack,world_draw.materials,world_draw.bindings,NULL)) return 0;
    nm_loader_yield(0);
    sq_material_lists_prepare(1,pack,world_draw.materials,world_draw.bindings,NULL);
    nm_loader_yield(0);
    /* Road-surface class, from geometry: a pack material is surface when it
     * has faces and every one of them faces road-up within about 41 degrees
     * (course 0 banking is 31). Source normals of drivable faces point to -Y.
     * Rails and walls have horizontal or mixed normals and stay unclassified.
     * A binding is surface only if all pack materials sharing its header are. */
    {
        uint8_t *material_class=nm_calloc(pack->header->materials,1);
        world_draw.surface=nm_calloc(world_draw.materials->count,1);
        world_draw.point=nm_calloc(world_draw.materials->count,1);
        if(!material_class || !world_draw.surface || !world_draw.point) { free(material_class);return 0; }
        for(unsigned m=0;m<pack->header->meshes;++m) {
            nm_loader_yield(0);
            const nm_mesh_t *mesh=pack->meshes+m;
            for(unsigned f=0;f<mesh->face_count;++f) {
                const nm_face_t *face=pack->faces+mesh->face_first+f;
                const float *n=face->normal;
                const float squared=n[0]*n[0]+n[1]*n[1]+n[2]*n[2];
                const int up=mesh->mode<2 && squared>0 && n[1]<0 && n[1]*n[1]>=0.5625f*squared;
                /* Packs with scenery classes carry the decision (made offline
                 * by the same rule for the reduced view's materials). */
                material_class[face->material]|=pack_flags&NM_PACK_SCENERY_CLASSES ?
                    (pack->materials[face->material].padding[0]&NM_SCENERY_SURFACE ? 1 : 2) : up ? 1 : 2;
            }
        }
        unsigned surface_materials=0,surface_bindings=0,used=0;
        for(unsigned m=0;m<pack->header->materials;++m) {
            if(!(m&15u)) nm_loader_yield(0);
            used+=material_class[m]!=0;surface_materials+=material_class[m]==1;
            for(unsigned l=0;l<256;++l) {
                const unsigned index=world_draw.bindings[m*256+l];
                if(index!=UINT16_MAX) world_draw.surface[index]|=material_class[m]==1 ? 1 : 2;
                if(index!=UINT16_MAX && (pack_flags&NM_PACK_SCENERY_CLASSES) &&
                   (pack->materials[m].padding[0]&NM_SCENERY_POINT)) world_draw.point[index]=1;
            }
        }
        unsigned point_bindings=0;
        for(unsigned i=0;i<world_draw.materials->count;++i) {
            world_draw.surface[i]=world_draw.surface[i]==1;
            surface_bindings+=world_draw.surface[i];
            point_bindings+=world_draw.point[i];
        }
        if(point_bindings) printf("Native split cutouts: %u opaque bindings point sampled\n",point_bindings);
        free(material_class);
        sq_surface_bindings=world_draw.surface;
#ifdef NM_ROAD_DEPTH_FIXED
        printf("Native road surface class: %u of %u drawn materials, %u bindings; depth scale %g for every camera "
            "(NATIVE_DIAG_ROAD_DEPTH)\n",surface_materials,used,surface_bindings,(double)sq_road_depth);
#else
        printf("Native road surface class: %u of %u drawn materials, %u bindings; depth scale %g now: a race view's "
            "h/(h+%g) of its eye height h, %g for every other camera\n",surface_materials,used,surface_bindings,
            (double)sq_road_depth,(double)NM_ROAD_SINK,(double)NM_ROAD_DEPTH_SCALE);
#endif
    }
    {
        /* Binding of each live face per side, once: the pack's lumas, this
         * material pack's bindings. No per-frame light or lookup remains. */
        nm_baked_face_t *table=nm_heap_alloc("world baked bindings",32,(pack->header->faces ? pack->header->faces : 1)*sizeof(*table));
        uint8_t *used=nm_calloc(world_draw.materials->resources,1);
        sq_baked_faces=table;
        if(!table || !used) { free(used); return 0; }
        unsigned faces_live=0,two_lumas=0,checker=0;
        for(unsigned m=0;m<pack->header->meshes;++m) {
            nm_loader_yield(0);
            const nm_mesh_t *mesh=pack->meshes+m;
            for(unsigned f=0;f<mesh->face_count;++f) {
                const nm_face_t *face=pack->faces+mesh->face_first+f;
                nm_baked_face_t *out=table+mesh->face_first+f;
                const int two_sided=(face->attribute&(1u<<17))!=0;
                out->front=world_draw.bindings[face->material*256+face->reserved];
                out->back=two_sided ? world_draw.bindings[face->material*256+face->padding] : UINT16_MAX;
                if(out->front==UINT16_MAX || (two_sided && out->back==UINT16_MAX)) {
                    printf("FAIL: world face %u of mesh %u has no binding for its baked luma %u/%u\n",
                        f,m,face->reserved,face->padding);
                    free(used); return 0;
                }
                for(unsigned side=0;side<(two_sided ? 2u : 1u);++side) {
                    const nm_material_binding_t *binding=world_draw.materials->bindings+(side ? out->back : out->front);
                    if(binding->resource!=NM_MATERIAL_SOLID) used[binding->resource]=1;
                }
                ++faces_live; two_lumas+=two_sided && out->front!=out->back;
                checker+=(pack->materials[face->material].header[0]&0x8000)!=0;
            }
        }
        printf("Native world baked bindings: %u live faces, %u two-sided with a luma per side, %u checker panels (%s); table=%u bytes\n",
            faces_live,two_lumas,checker,!checker ? "none selected" : "opaque list",
            (unsigned)(pack->header->faces*sizeof(*table)));
        /* Live specular faces (NM_PACK_LIVE_SPECULAR): every luma their term can reach is resident too. */
        nm_loader_yield(0);
        if(!sq_live_prepare(pack,world_draw.bindings,world_draw.materials,used)) { free(used); return 0; }
        nm_loader_yield(0);
        /* Frame-counter animations: every binding a frame shows is resident too. */
        if(!clock_attach_bindings(pack,world_draw.materials,used)) { free(used); return 0; }
        const int uploaded=!nm_world_materials_palettized() || nm_world_materials_upload(used);
        free(used);
        if(!uploaded) return 0;
    }
    nm_loader_yield(0);
    if(!sq_flat_strips_prepare(1,pack,world_draw.materials)) return 0;
    nm_loader_yield(0);
    if(!sq_emit_faces_prepare(1,pack)) return 0;
    nm_loader_yield(0);
    if(!sq_lod_prepare(pack)) return 0;
    nm_loader_yield(0);
#ifdef __DREAMCAST__
    if(!sq_facing_prepare(1,pack)) return 0;
#endif
    if(!sq_allocate(1,pack)) return 0;
    clock_attach_tables(pack);
    printf("Native face groups: car=%u world=%u; shared visibility scratch=%u bytes\n",
        (unsigned)draw_pack->header->reserved,(unsigned)pack->header->reserved,faces);
    /* The cars' table is unchanged by the world: checked once more against
     * fresh compiles. The world's is built after its textures are resident
     * and its road-surface class is known; a world header that equals a car
     * header is the car pool's. */
    nm_loader_yield(0);
    if(!check_resident_headers(0)) return 0;
    nm_loader_yield(0);
    if(!sq_emit_records_build(1,world_draw.materials->count,draw_emit_value)) return 0;
    nm_loader_yield(0);
    if(!check_resident_headers(1) || !check_header_pools()) return 0;
    nm_loader_yield(0);
    /* World objects super-sampled by instance (model_submission.h, sq_ss_select): their own pooled headers. */
    nm_loader_yield(0);
    if(!sq_ss_prepare(pack)) return 0;
    nm_loader_yield(0);
    /* The prebuilt world streams (model_submission.h, NM_SQ_STREAM): from the
     * emission records, the emission faces, the baked bindings, the plane
     * constants and the level-of-detail groups built above. */
    if(!sq_stream_prepare(pack)) return 0;
    /* The world's material/luma table (256 entries a material) is read only above: the baked bindings, the
     * material lists, the UV check, the road-surface and point classes. The world face walk takes the baked
     * bindings (sq_walk_baked), so the table goes back to the heap while the scene runs. */
    if(sq_baked_faces) { free(world_draw.bindings); world_draw.bindings=NULL; }
    return 1;
}

/* Out of line: the frame's hot path (practice_prepare, a fixed slot of config/hot_layout.tsv) only tests the count. */
static __attribute__((noinline)) void sq_live_frame(void) {
    if (world_draw.pack && world_draw.pack->header->instances && !(world_draw.pack->instances[0].padding[0]&NM_OBJECT_PLACED))
        sq_live_update(world_draw.pack,world_draw.pack->instances[0].matrix,sq_emit_records[1]);
}

__attribute__((always_inline)) inline int nm_render_enqueue_objects(const unsigned *car_parts,unsigned cars,unsigned player) {
    if(sq_failed) return 0;
#ifdef NATIVE_CACHE_PROFILE
    if (!draw_samples) {
        nm_memset(draw_cache_stalls,0,sizeof(draw_cache_stalls));
        nm_memset(draw_cache_time,0,sizeof(draw_cache_time));
        draw_cache_samples=0;
    }
    /* Skip warmup; read both counters on one frame in32, not on every face
     * of every frame. Unsampled frames still have instrumentation branches. */
    draw_cache_sampling=draw_samples>=64 && !(draw_samples&31);
    draw_cache_samples+=draw_cache_sampling;
#endif
#ifdef NATIVE_PROFILE
    /* Exclude the work of loading. */
    if (!draw_samples) {
        nm_memset(draw_phases,0,sizeof(draw_phases));
        nm_memset(draw_clip_paths,0,sizeof(draw_clip_paths));
        draw_texture_header_reuse=0;
    }
#endif
    const uint64_t started = timer_ns_gettime64();
    if (draw_previous_update) {
        const uint64_t interval = started - draw_previous_update;
        draw_interval_sum += interval;
        if (interval > draw_interval_max) draw_interval_max = interval;
    }
    draw_previous_update = started;
    if (!sq_prepare()) { puts("FAIL: direct SQ preparation"); return 0; }
    /* Live specular (model_submission.h): this frame's bindings from the view the world instances share. */
    if (sq_live_class_count) sq_live_frame();
    const uint64_t elapsed = timer_ns_gettime64() - started;
    draw_prepare_sum += elapsed;
    if (elapsed > draw_prepare_max) draw_prepare_max = elapsed;
    ++draw_samples;
    return sq_queue_objects(car_parts,cars,player);
}

int nm_render_enqueue(void) { return nm_render_enqueue_objects(NULL,0,0); }

/* Vertices whose u,v the scene rewrites while it runs (window reflections).
 * The emitter takes texture coordinates from records built at load
 * (model_submission.h, nm_sq_face_t); faces that use these vertices read the
 * vertex table when they are drawn instead. Registering a range again is
 * free; call it wherever the rewrite is made, before or after nm_render_init. */
int nm_render_uv_dynamic(const nm_vertex_t *vertices,unsigned count) { return sq_uv_dynamic(vertices,count); }
void nm_render_car_light_classes_reserve(unsigned classes) { sq_light_class_reserve=classes; }
int nm_render_car_mesh_refresh(unsigned mesh) { return draw_pack && sq_car_mesh_refresh(draw_pack,mesh); }

void nm_render_release(void) {
    printf("Direct SQ release: failed=%d (final PVR vertices generated in callbacks)\n",sq_failed);
    draw_inspect_strips=0; draw_inspect_stipple_release();
    for(unsigned s=0;s<2;++s) {
        free(sq_flat_strip_triangle[s]); sq_flat_strip_triangle[s]=NULL;
        free(sq_emit_faces[s]); sq_emit_faces[s]=NULL; free(sq_strip_corners[s]); sq_strip_corners[s]=NULL;
    }
    sq_dynamic_uv_count=0;
    free(sq_marks); sq_marks=NULL; sq_marks_size=0;
#ifdef __DREAMCAST__
    printf("Native face sides from plane constants: car %s, world %s\n",
        sq_face_planes[0] ? "on" : "off (reference test)",sq_face_planes[1] ? "on" : "off (reference test)");
#ifdef NATIVE_LIGHT_VERIFY
    printf("Native face side checks PASS: car front/rear/reference=%llu/%llu/%llu, world front/rear/reference=%llu/%llu/%llu; "
        "largest share of the margin used %.4f\n",
        (unsigned long long)sq_facing_decided[0][0],(unsigned long long)sq_facing_decided[0][1],(unsigned long long)sq_facing_reference[0],
        (unsigned long long)sq_facing_decided[1][0],(unsigned long long)sq_facing_decided[1][1],(unsigned long long)sq_facing_reference[1],
        (double)sq_facing_worst);
    nm_memset(sq_facing_decided,0,sizeof(sq_facing_decided)); nm_memset(sq_facing_reference,0,sizeof(sq_facing_reference)); sq_facing_worst=0;
#endif
    for(unsigned s=0;s<2;++s) { free(sq_face_planes[s]); sq_face_planes[s]=NULL; }
    free(sq_light_class); free(sq_light_cache); sq_light_class=NULL; sq_light_cache=NULL;
    free(sq_lit_faces); sq_lit_faces=NULL;
#endif
    if(sq_live_class_count) printf("Native live specular: %u frames updated\n",sq_live_updates);
    sq_live_release();
    if(world_clock.light_rows) printf("Native world clock light: %u placements lit, %u kept (the key of the last)\n",
        world_clock.light_updates,world_clock.light_kept);
    world_clock.light_updates=0; world_clock.light_kept=0;
    clock_release(); world_clock.data=NULL;
    free(sq_baked_faces); sq_baked_faces=NULL;
    sq_stream_release();
    sq_car_stream_release();
    sq_translucent_release();
    sq_lod_release();
    sq_ss_release();
    for(unsigned i=0;i<2;++i) {
        sq_emit_records_release(i);
        free(sq_sources[i].batches);free(sq_sources[i].points);free(sq_sources[i].bindings);
    }
    nm_memset(sq_sources,0,sizeof(sq_sources));sq_failed=0;
    sq_material_lists[0]=sq_material_lists[1]=0;
#ifdef NATIVE_CACHE_PROFILE
    if (draw_cache_counter_owned) {
        perf_cntr_stop(PRFC1);
        if (draw_cache_samples) {
            const char *names[]={"transform","light","material","clip","bounds","emit",
                "headers-inclusive","world-resources","car-require","frame-begin","callback-inclusive",
                "object-bounds","cache-reset"};
            printf("Native render PMU scopes n=%u frames=%u event=%u (active-ns/events totals; every32nd frame after64 warmup; instrumentation included):\n",
                draw_cache_samples,draw_samples,(unsigned)NATIVE_CACHE_PROFILE_EVENT);
            for (unsigned s=0;s<2;++s) for (unsigned i=0;i<13;++i)
                printf("PMU %s %s ns=%llu events=%llu\n",s ? "world" : "car",names[i],
                    (unsigned long long)draw_cache_time[s][i],(unsigned long long)draw_cache_stalls[s][i]);
            puts("PMU nesting: headers includes car-require; callback includes transform/light/material/clip/emit and their loop overhead; do not sum inclusive rows");
        }
        perf_cntr_start(PRFC1,PMCR_INIT_NO_MODE,PMCR_COUNT_CPU_CYCLES);
        perf_cntr_stop(PRFC1);draw_cache_counter_owned=0;
        printf("Native cache profile released PRFC1; KOS timer intact=%d\n",perf_cntr_timer_enabled());
    }
#endif
#ifdef NATIVE_BOUNDS_VERIFY
    printf("Native whole-block inside verified vertices: %u\n",bounds_verified);
#endif
    if (draw_samples) printf("Native draw wall time n=%u: prepare avg/max=%llu/%llu us "
        "(instance selection before callback registration; the callbacks' own time is the register time of the PVR statistics)\n", draw_samples,
        (unsigned long long)(draw_prepare_sum/draw_samples/1000),
        (unsigned long long)(draw_prepare_max/1000));
    if (draw_samples > 1) printf("Native update-to-update n=%u: avg/max=%llu/%llu us "
        "(whole loop interval, not a PVR phase counter)\n", draw_samples-1,
        (unsigned long long)(draw_interval_sum/(draw_samples-1)/1000),
        (unsigned long long)(draw_interval_max/1000));
#ifdef NATIVE_PROFILE
    printf("Native texture reused/relocated-header byte checks: %u (profile-only runtime oracle)\n",draw_texture_header_reuse);
    if (draw_samples) printf("Native CPU phase avg: transform/cache=%llu light=%llu "
        "material-pass=%llu clip=%llu view/bounds=%llu emit=%llu us (active CPU time, excludes CPU sleep; includes timer overhead)\n",
        (unsigned long long)(draw_phases[0]/draw_samples/1000),
        (unsigned long long)(draw_phases[1]/draw_samples/1000),
        (unsigned long long)(draw_phases[2]/draw_samples/1000),
        (unsigned long long)(draw_phases[3]/draw_samples/1000),
        (unsigned long long)(draw_phases[4]/draw_samples/1000),
        (unsigned long long)(draw_phases[5]/draw_samples/1000));
    if(draw_samples) printf("Native direct clip triangles/frame: in=%llu rejected=%llu clipped=%llu xy-touch=%llu near-touch=%llu\n",
        (unsigned long long)(draw_clip_paths[0]/draw_samples),
        (unsigned long long)(draw_clip_paths[1]/draw_samples),
        (unsigned long long)(draw_clip_paths[2]/draw_samples),
        (unsigned long long)(draw_clip_paths[3]/draw_samples),
        (unsigned long long)(draw_clip_paths[4]/draw_samples));
    if(draw_samples) printf("Native direct material avg: face-selection=%llu header-scan-build-exclusive=%llu "
        "car-texture-require=%llu world-prepare=%llu frame-begin=%llu us "
        "(active CPU time; header excludes nested car require; sleeping fence time excluded)\n",
        (unsigned long long)(draw_phases[2]/draw_samples/1000),
        (unsigned long long)((draw_phases[6]-draw_phases[8])/draw_samples/1000),
        (unsigned long long)(draw_phases[8]/draw_samples/1000),
        (unsigned long long)(draw_phases[7]/draw_samples/1000),
        (unsigned long long)(draw_phases[9]/draw_samples/1000));
#endif
    free(draw_groups); draw_groups = NULL; draw_group_capacity = 0;
    free(draw_bindings); draw_bindings = NULL; free(draw_runs); draw_runs = NULL;
    free(world_draw.bindings);
    free(world_draw.surface); free(world_draw.point);
    sq_surface_bindings=NULL;
    nm_memset(&world_draw,0,sizeof(world_draw));
    draw_materials = NULL;
    draw_pack = NULL; draw_count = draw_samples = 0;
#ifdef NATIVE_CACHE_PROFILE
    draw_cache_sampling=0;
#endif
    draw_prepare_sum = draw_prepare_max = 0;
    draw_previous_update = draw_interval_sum = draw_interval_max = 0;
#ifdef NATIVE_PROFILE
    nm_memset(draw_phases, 0, sizeof(draw_phases));
    nm_memset(draw_clip_paths, 0, sizeof(draw_clip_paths));
    draw_texture_header_reuse=0;
#endif
}
