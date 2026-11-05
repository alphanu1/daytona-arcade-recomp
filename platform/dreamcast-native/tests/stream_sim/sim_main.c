/* See build_sim.py. Host only.
 * usage: sim ASSETS FRAMES OUT.txt
 * environment:
 *   SIM_STAGE=0..6      scenery stage of the build (track_surface.h); default 6 (the build's default since
 *                       14 October 2026; 5 before)
 *   SIM_FILTER_ALL=0    the PVR work model's filter classes of a build with NATIVE_FILTER_ALL=0 (bilinear on
 *                       opaque textures only, road-surface class and split-cutout pieces point sampled); default:
 *                       the default build's NATIVE_FILTER_ALL=1 (every texture but the catch-fence stipple).
 *                       Only the model's counts depend on it; the stream, its digests and the reference do not.
 *   SIM_WORLD=NAME      the world pack ASSETS/NAME.dnm, NAME.dnt instead of world (course1-world: Expert, course2-world:
 *                       Advanced; gen_frames.py --course N writes their frame tables)
 *   SIM_ROAD_DEPTH=S    the road class at depth scale S from the start: the world's tables built at S (as a world attached
 *                       while the scene's scale was S), no rewrite, every frame at S; replaces the frame table's scales
 *   SIM_ROAD_DEPTH_STEPS=F:S,F:S,...  from frame F (counted from 0, in increasing order) the scale S, set as the scene
 *                       sets it (sq_road_depth_set before the frame's lists); replaces the frame table's scales
 *   SIM_ROAD_DEPTH_CYCLE=N  every Nth frame the next of 0.636, 0.647, 0.755, 0.862, 0.5; replaces the above
 *   SIM_ROAD_DEPTH_CHECK=1  every frame, after the scale is set: the emission classes hold the frame's scale, and every
 *                       mesh on the stream path holds what a build at that scale gives (see road_records_check).
 *                       Without a frame table's scale, a step or SIM_ROAD_DEPTH every frame is at NM_ROAD_DEPTH_SCALE
 *   SIM_GENERAL_EMITTER=1  submit through sq_emit_range (the inspection views' emitter) instead of the
 *                       production emitter sq_emit_batches: the stream must be the same
 *   SIM_NO_STREAM=1     no prebuilt world streams (model_submission.h, NM_SQ_STREAM): every world batch on
 *                       the queue path, as make NATIVE_SQ_STREAM=0 builds it; the stream must be the same
 *   SIM_NO_CAR_STREAM=1 no prebuilt car streams (NM_SQ_CAR_STREAM): every car part on the queue path, as make
 *                       NATIVE_SQ_CAR_STREAM=0 builds it; the stream must be the same. SIM_CAR_GUARDED=1: every car
 *                       stream list written in guarded mode (per-face TA checks); the stream must be the same
 *   SIM_CAR_EVENTS=FILE per frame the car stream's events (SQ_CAR_COUNT) and the queue path's car counts
 *   SIM_STREAM_EVENTS=FILE  per frame the stream path's events (SQ_STREAM_COUNT: faces by the path they take,
 *                       runs, marks, lean transforms) and the world's headers, vertices, transforms and faces;
 *                       the summary prints their means and the side decisions against screen winding
 *   SIM_FACES=FILE      per frame, the world faces that produced a triangle:
 *                       "W tick address h0 h1 h2 h3 luma faces"
 *   SIM_BINDINGS=FILE   per drawn binding: source index triangles argb cmd mode1 mode2
 *   SIM_DEPTH=N         software depth buffer on every Nth frame (0: off)
 *   SIM_PVR=FILE        model of what the PVR2 has to do per frame (see "PVR
 *                       work model" below): one "P tick=..." line per frame,
 *                       a summary on stdout. SIM_PT_CHUNK=vertices (default
 *                       64: the punch-through chunk size KOS writes to
 *                       ISP_FEED_CFG). SIM_CLASSES=FILE: lines "WORD NAME",
 *                       world materials with texture word WORD (header word
 *                       2) are reported as class NAME instead of their stage.
 *                       With SIM_SIDE=FILE (the frame generator's side file:
 *                       per frame the sky's heading and scroll and the HUD
 *                       quads) and SIM_HUD=DIR (the HUD pack: DIR/hud/
 *                       sheet-N.dt, hud.dp) the model also takes the sky
 *                       quads, the HUD and the cars' shadow volumes, none of
 *                       which passes through the model renderer (the
 *                       CONDITION panel's translucent rectangle is not;
 *                       the model's translucent list is the car effects'
 *                       smoke, see "PVR work model"). Switches
 *                       that mirror the measurement builds (local.cfg.mk,
 *                       NATIVE_DIAG_*): SIM_NO_HUD=1, SIM_NO_SKY=1,
 *                       SIM_NO_SHADOWS=1|volumes|receivers, SIM_NO_CARS=1,
 *                       SIM_NO_WORLD=1, SIM_FILL_PROBE=1 (the punch-through
 *                       form of NATIVE_DIAG_FILL_PROBE: two layers of a
 *                       256x256 quad, class probe; the translucent forms
 *                       are not in the model). (NATIVE_DIAG_CAR_OPAQUE and
 *                       NATIVE_DIAG_ROAD_DEPTH are build options of the
 *                       simulator: -DNM_DIAG_CAR_OPAQUE=1,
 *                       -DNM_ROAD_DEPTH_SCALE=1.0f -DNM_ROAD_DEPTH_FIXED=1.)
 *   SIM_CAR_LEVELS=FILE lines "MESH LEVEL" (car mesh, detail level 0..3 of the body table): the summary's
 *                       per-class TA statistics split the cars by level (other car meshes: class "other")
 *   SIM_CLASS_FRAMES=FILE  per frame the per-class TA vertices, triangles and strips (see "TA by class")
 *   SIM_PIXEL_FACES=FILE  pixel check of faces a restripped pack changed (tools/prepare_restrip.py --pixel-faces):
 *                       lines "FACE ROTATED DU DV" (world face index of this pack; 1 if its quad was split on the
 *                       other diagonal; the whole periods its u, v were shifted by). Every frame, every listed face
 *                       drawn whole (no corner clipped) is rasterised as submitted and as the source pack draws it
 *                       (u-DU, v-DV, the source's diagonal) and the two are compared pixel by pixel: coverage, written
 *                       depth, point-sampled texel and the sample position to 1/64 texel (see "Pixel check")
 *   SIM_LOD=FILE        per frame the level-of-detail counts (walked groups by level, changes, pops; fence panels across a
 *                       sweep plane: straddling=); SIM_LOD_GROUPS=FILE per frame the level or sweep code of each walked fence group
 *   SIM_COLOR=DIR       colour pictures DIR/TICK.ppm (640x480) of the frames
 *                       named by SIM_COLOR_TICKS=",t1,t2," (see "Colour picture"
 *                       below). SIM_COLOR_MIP=1: the catch-fence stipple from the mip level the PVR would pick.
 *                       SIM_COLOR_SMOKE=arcade: the cars' smoke as the arcade's screen door (a tr stream).
 *                       SIM_COLOR_FILTER=bilinear: the smoke filtered (smoke_bilinear): the arcade's coverage threshold,
 *                       pt-screen's alpha test at 0x80, tr-screen's and tr's blend.
 *   SIM_SMOKE_BOUND=1|2 (experiment, with SIM_PVR, pt-screen): the punch-through model without the smoke fragments whose
 *                       shape texel is a hole (1: what geometry that hugged every texel would save), and (2) without
 *                       those of the checker's transparent pixels too (no geometry can do that). SIM_SMOKE_MASKS=DIR
 *                       (NM_SMOKE_TIGHT): the smoke textures' masks as DIR/mask-RESOURCE.bin.
 */
#include <assert.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <string.h>
#include "geometry.h"
#include "meshpack.h"
#include "material_pack.h"
#include "lighting.h"
#include "render/materials/palette_banks.h"
#include "sim_shz.h"
#include "render/models/scenery_camera.h"
#include "render/scene/track_surface.h"
#include "render/scene/sky.h" /* the real NM_SKY_DEPTH guard */
#include "shadow_volume.h"
#include "render/scene/damage_bodies.h"
#define PVR_LIST_OP_POLY 0
#define PVR_LIST_TR_POLY 2
#define PVR_LIST_PT_POLY 4
#define PVR_CMD_VERTEX 0xe0000000u
#define PVR_CMD_VERTEX_EOL 0xf0000000u
typedef struct { uint32_t flags; float x,y,z,u,v; uint32_t argb,oargb; } pvr_vertex_t;
typedef struct { uint32_t cmd,mode1,mode2,mode3,padding[4]; } pvr_poly_hdr_t;
typedef void *pvr_ptr_t;
enum { STREAM_MAX=400000 };
#ifdef NATIVE_BILINEAR /* build_sim.py does not define it: the model follows the default build (NATIVE_BILINEAR=1) */
#define NM_SIM_UNFILTERED 0
#else
#define NM_SIM_UNFILTERED (getenv("SIM_UNFILTERED")!=NULL)
#endif
/* Stream index of the vertex whose colour triangle t of a strip takes with
 * Gouraud off: its last submitted vertex. */
#define SIM_PROVOKING(first,t,last) (NM_FLAT_PROVOKING_LAST ? (last) : (first)+(t))
static union { pvr_vertex_t vertex; pvr_poly_hdr_t header; } *stream;
static uint8_t *stream_source; static uint32_t *stream_face; static uint16_t *stream_binding;
static unsigned sim_face,sim_binding,sim_instance,sim_clip; static uint32_t *stream_instance; static uint8_t *stream_clip;
/* A vertex of a clipped triangle's polygon: the number of that polygon (SQ_CLIP_INPUT counts them; 0: not clipped). */
static uint32_t sim_polygon,sim_polygon_serial,*stream_polygon;
#define SQ_VERTEX_TAG(face,binding) ((void)(sim_face=(face),sim_binding=(binding),sim_instance=batch->instance_index,sim_clip=0,sim_polygon=0))
#define SQ_CLIP_TAG(polygon) ((void)(sim_clip=1,sim_polygon=(polygon) ? sim_polygon_serial : 0u))
/* A piece of tight smoke geometry (model_submission.h, NM_SMOKE_TIGHT): a polygon of its own for the reference. */
#define SQ_TIGHT_PIECE() ((void)++sim_polygon_serial)
static unsigned writes,current_source;
static void *pvr_dr_target(void) { assert(writes<STREAM_MAX); return stream+writes; }
static void pvr_dr_commit(void *p) { assert(p==(void *)(stream+writes)); stream_source[writes]=(uint8_t)current_source;
    stream_face[writes]=sim_face; stream_binding[writes]=(uint16_t)sim_binding; stream_instance[writes]=sim_instance; stream_clip[writes]=(uint8_t)sim_clip; stream_polygon[writes]=sim_polygon; ++writes; }
#include "sim_shz.h"
static const float *host_matrix;
static inline shz_vec4_t shz_xmtrx_transform_vec4(shz_vec4_t p) {
    const float *m=host_matrix; shz_vec4_t r;
    r.x=p.x*m[0]+p.y*m[3]+p.z*m[6]+p.w*m[9]; r.y=p.x*m[1]+p.y*m[4]+p.z*m[7]+p.w*m[10];
    r.z=p.x*m[2]+p.y*m[5]+p.z*m[8]+p.w*m[11]; r.w=p.w; return r;
}
#define shz_xmtrx_load_3x4(m) (host_matrix=inst->matrix)
static inline void shz_dcache_alloc_line(void *p) { (void)p; }
/* Statistics the renderer leaves to its host tools. */
static unsigned draw_opaque,draw_cutout,draw_rear,draw_checker,draw_transformed,draw_instances_rejected;
static unsigned long long draw_side_same,draw_side_tested; /* world faces walked: both sides one binding / side test made */
static uint64_t sq_joined_strips,sq_strip_vertices_saved,sq_flat_runs[2],sq_flat_run_faces[2],sq_flat_run_vertices[2];
#define SQ_COUNT(counter,amount) ((counter)+=(amount))
static void *memalign(size_t alignment,size_t size) { return aligned_alloc(alignment,(size+alignment-1)&~(alignment-1)); }
static struct { unsigned list; void (*call)(void *); void *data; } queue[512];
static unsigned queued;
static void enj_render_list_add(unsigned list,void (*call)(void *),void *data) {
    assert(queued<512); queue[queued].list=list; queue[queued].call=call; queue[queued++].data=data; }
/* Probes (counts only): how often the working tree's fast paths run. */
static unsigned long long sim_bounded_vertices,sim_all_vertices;
static unsigned long long sim_probe_emit_runs,sim_probe_emit_intact_faces,sim_probe_emit_general_faces,sim_probe_emit_outside_faces,sim_probe_emit_reproven,sim_probe_emit_runs_refused,sim_probe_emit_runs_dynamic;
static unsigned long long sim_probe_car_batches_proven,sim_probe_car_batches_tested,sim_probe_world_batches_proven,sim_probe_world_batches_tested;
static unsigned long long sim_probe_planes_prepared,sim_probe_planes_shared,sim_probe_planes_quick,sim_probe_planes_hinted,sim_probe_guard_quick;
static unsigned long long sim_probe_light_calls,sim_probe_light_repeats,sim_probe_light_normal_repeats;
#define SQ_PROBE(counter) (++sim_probe_##counter)
#define SQ_PROBE_ADD(counter,amount) (sim_probe_##counter+=(amount))
static unsigned long long sim_wide_vertices[2],sim_classic_proven;
#ifdef SIM_HAS_BOUNDED
#define nm_cache_vertex_bounded(view,point,out,inside) (++sim_bounded_vertices,sim_classic_proven+=(inside)!=0,nm_cache_vertex_bounded(view,point,out,inside))
#define nm_cache_vertex_wide(view,point,out,inside) (++sim_wide_vertices[(inside)!=0],nm_cache_vertex_wide(view,point,out,inside))
#endif
#include "sim_parts.h"
#include "sim_light.h"
#ifdef SIM_HAS_FACING
/* How often a lit car face repeats an earlier one of its instance: same
 * (normal, material, side), or same normal alone. */
static int sim_light_probe(const nm_instance_t *inst,const nm_face_t *face,int rear) {
    static const nm_instance_t *last; static struct { float n[3]; unsigned material; int rear; } seen[2048]; static unsigned count;
    if(inst!=last) { last=inst; count=0; }
    int full=0,normal=0;
    for(unsigned i=0;i<count;++i) if(!memcmp(seen[i].n,face->normal,12)) { normal=1; if(seen[i].material==face->material && seen[i].rear==rear) { full=1; break; } }
    ++sim_probe_light_calls; sim_probe_light_repeats+=full; sim_probe_light_normal_repeats+=normal;
    if(!full && count<2048) { memcpy(seen[count].n,face->normal,12); seen[count].material=face->material; seen[count].rear=rear; ++count; }
    return 0;
}
#define SQ_LIGHT_PROBE(inst,face,rear) ((void)sim_light_probe(inst,face,rear))
#endif
/* Level of detail: the level the renderer drew each walked group at this frame (255: not walked). */
static uint8_t *sim_lod_drawn;
/* World objects super-sampled by instance (model_submission.h, sq_ss_select): per live world instance this frame the
 * renderer's decision (0 plain, 1 super-sampled, NM_REFERENCE_LOD_UNWALKED no batch or no switch depth), the
 * reference's own record per mesh, and per stream word whether the header carried the TSP bit (cleared there). */
static uint8_t sim_ss_drawn[4096],*oracle_ss_state,*stream_ss;
static unsigned long long ss_batches[2],ss_frames,ss_triangles;
#define SQ_LOD_SELECTED(group,level) ((void)(sim_lod_drawn[group]=(uint8_t)(level)))
/* Catch-fence sweep (model_submission.h, NM_FENCE_SWEEP): a band polygon begins (its own clipped-polygon number); a sweep
 * decision that needed the panel's corners. */
static unsigned long long sim_sweep_polygons,sim_sweep_corner_tests;
#define SQ_SWEEP_INPUT() ((void)(++sim_polygon_serial,++sim_sweep_polygons))
#define SQ_SWEEP_DECIDED(straddles) ((void)(++sim_sweep_corner_tests))
/* Which path each batch takes (model_submission.h, SQ_PATH_POINTS / SQ_PATH_EMIT): totals per source for the summary
 * and, with SIM_PATHS=FILE, one line per frame, batch and event. An emission event of kind 0 counts the faces left when
 * the production loop is entered: a batch with a clipped face enters it once more for the rest (re-entries are counted). */
static unsigned long long sim_path_points[2][2][2],sim_path_emit[2][4][3]; /* [source][kind][batches, vertices | batches, faces, re-entries] */
static FILE *sim_paths_out; static unsigned sim_path_tick; static const void *sim_path_last[2];
static void sim_path_points_event(unsigned kind,unsigned source,unsigned instance,unsigned vertices,unsigned faces) {
    ++sim_path_points[source][kind][0]; sim_path_points[source][kind][1]+=vertices;
    if(sim_paths_out) fprintf(sim_paths_out,"%u points source=%u instance=%u %s mesh-vertices=%u mesh-faces=%u\n",sim_path_tick,source,instance,kind ? "per-vertex" : "wide-shared",vertices,faces);
}
static void sim_path_emit_event(unsigned kind,unsigned source,const void *batch,unsigned instance,unsigned list,unsigned faces) {
    if(!faces) return;
    const int again=kind==0 && sim_path_last[list!=0]==batch;
    if(again) ++sim_path_emit[source][kind][2]; else { ++sim_path_emit[source][kind][0]; sim_path_emit[source][kind][1]+=faces; }
    if(kind==1 && sim_path_last[list!=0]==batch) sim_path_emit[source][0][1]-=faces; /* the rest of a batch the production loop began */
    sim_path_last[list!=0]=batch;
    if(sim_paths_out) fprintf(sim_paths_out,"%u emit source=%u instance=%u list=%u %s faces=%u%s\n",sim_path_tick,source,instance,list,
        kind==0 ? "production" : kind==1 ? "general-unproven" : kind==2 ? "general-object" : "stream",faces,again ? " (after a clipped face)" : "");
}
#define SQ_PATH_POINTS(kind,batch) sim_path_points_event((kind),(batch)->source,(batch)->instance_index,(batch)->mesh->vertex_count,(batch)->mesh->face_count)
#define SQ_PATH_EMIT(kind,source,batch,list,faces) sim_path_emit_event((kind),(source),(batch),(batch)->instance_index,(list),(faces))
/* SIM_GENERAL_EMITTER=1: every object through sq_emit_range, the emitter of the inspection views. */
static int sim_general_emitter;
#define SQ_GENERAL_EMITTER sim_general_emitter
/* Prebuilt world streams: the alternative to the CPU side test (model_submission.h, sq_stream_rear). Per world face whose
 * side the stream emitter decides: one-sided faces (that PVR culling by winding would take over) and two-luma faces (which
 * need the decision for their colour); the one-sided faces the CPU culls (their corners would be written for the PVR to
 * cull); and, per triangle of a one-sided face with every corner projected, the sign of its screen-space area against the
 * CPU's side (a triangle whose winding disagrees, or has zero area, is one the PVR would cull differently). */
static unsigned long long cull_faces[2],cull_rear,cull_rear_vertices,cull_rear_triangles,cull_winding[2][3],cull_unprojected;
static void sim_stream_face_probe(const void *batch,const void *face,int rear);
#define SQ_STREAM_FACE_PROBE(batch,face,rear) sim_stream_face_probe((batch),(face),(rear))
/* The stream path's events (model_submission.h, SQ_STREAM_COUNT): printed per frame for the SH4 instruction model. */
static unsigned long long sim_stream_events[32],sim_stream_events_before[32]; static FILE *sim_stream_events_out;
#define SQ_STREAM_COUNT(event,amount) (sim_stream_events[(event)]+=(unsigned long long)(amount))
/* SIM_RUN_VERTEX_TABLE=1: every strip run of the production emitter through the body that reads the vertex table. */
static int sim_run_vertex_table;
#define SQ_RUN_VERTEX_TABLE sim_run_vertex_table
/* The car stream's events (model_submission.h, SQ_CAR_COUNT): printed per frame (SIM_CAR_EVENTS) for the SH4 model. */
static unsigned long long sim_car_events[32],sim_car_events_before[32]; static FILE *sim_car_events_out;
#define SQ_CAR_COUNT(event,amount) (sim_car_events[(event)]+=(unsigned long long)(amount))
/* SIM_CAR_GUARDED=1: every car stream list in guarded mode (the TA checks of sq_emit_range on every face and run). */
static int sim_car_guarded;
#define SQ_CAR_GUARDED sim_car_guarded
/* Clipped triangles (model_submission.h, SQ_CLIP_INPUT): [0] near plane bit only, [1] side bits too; SIM_CLIP_DUMP=FILE
 * writes each one as a 128-byte record (the view's centre, bounds and near_z, the three cached corners as the clipper
 * reads them, the plane bits): the sample the SH4 instruction counts of the clippers are measured on. */
static unsigned long long sim_clip_kind[2];
static FILE *sim_clip_dump; static int sim_clip_dump_checked;
static void sim_clip_input(const nm_view_t *view,const nm_cached_vertex_t *const *points,unsigned outside) {
    ++sim_clip_kind[outside!=1u]; ++sim_polygon_serial;
    if(!sim_clip_dump_checked) { sim_clip_dump_checked=1; if(getenv("SIM_CLIP_DUMP")) { sim_clip_dump=fopen(getenv("SIM_CLIP_DUMP"),"wb"); assert(sim_clip_dump); } }
    if(!sim_clip_dump) return;
    unsigned char record[128]; memset(record,0,sizeof record);
    memcpy(record,view->center,8); memcpy(record+8,view->bounds,16); memcpy(record+24,&view->near_z,4);
    for(unsigned k=0;k<3;++k) memcpy(record+28+32*k,points[k],32);
    memcpy(record+124,&outside,4);
    fwrite(record,1,sizeof record,sim_clip_dump);
}
#define SQ_CLIP_INPUT(view,points,outside) sim_clip_input((view),(points),(outside))
/* Every car light class evaluation against the exact form (SQ_CLASS_LIGHT_PROBE): the transformed normal in scalar
 * float arithmetic as the reference renderer has it, lighting.h on it with the decided side's facing; luma difference
 * of the renderer's evaluation, -2..2 (beyond: 0 and 4), and side differences. */
static unsigned long long sim_light_evaluations,sim_light_delta[5],sim_light_side,sim_light_mode[2];
static void sim_class_light_probe(unsigned mode,const float *n,const nm_material_t *material,const nm_instance_t *inst,int rear,int light) {
    if(light<0) return;
    const float *m=inst->matrix; float normal[3],points[3][3]={{0}};
    for(unsigned a=0;a<3;++a) normal[a]=n[0]*m[a]+n[1]*m[3+a]+n[2]*m[6+a];
    for(unsigned a=0;a<3;++a) points[2][a]=rear ? -normal[a] : normal[a];
    const int exact=nm_face_lighting(mode,normal,points,inst->light,material);
    ++sim_light_evaluations; ++sim_light_mode[mode&1u];
    if(exact<0 || ((exact^light)&256)) { ++sim_light_side; return; }
    const int d=(light&255)-(exact&255);
    ++sim_light_delta[d<-2 ? 0 : d>2 ? 4 : d+2];
}
#define SQ_CLASS_LIGHT_PROBE(mode,normal,material,inst,rear,light) sim_class_light_probe((mode),(normal),(material),(inst),(rear),(light))
/* Translucent list (the car effects' smoke): faces submitted, instances, frames with any; faces in the reference's
 * back-to-front order (nm_reference_translucent_key, largest first; keys within 1e-5 relative count as equal). */
static unsigned long long tr_faces,tr_instances,tr_frames,tr_ordered,tr_triangles_total,tr_triangles_max,tr_vertices,tr_vertices_max,tr_faces_max;
#define SQ_TR_COUNT(faces,instances,vertices) ((void)(tr_faces+=(faces),tr_instances+=(instances),tr_frames+=(faces)!=0, \
    tr_vertices+=(vertices),tr_vertices_max=(vertices)>tr_vertices_max ? (vertices) : tr_vertices_max, \
    tr_faces_max=(faces)>tr_faces_max ? (faces) : tr_faces_max))
#include "render/models/model_submission.h"
#if defined(SIM_HAS_STREAM) && SQ_STREAMS
static void sim_stream_face_probe(const void *data,const void *record,int rear) {
    const nm_sq_batch_t *batch=data;
    const nm_sq_stream_face_t *face=record;
    const int two=(face->flags&SQ_STREAM_TWO)!=0;
    ++cull_faces[two];
    if(two) return;
    if(rear) { ++cull_rear; cull_rear_vertices+=face->count; cull_rear_triangles+=face->count-2u; }
    /* Fan order from emission order (1,2,0,3 a quad). */
    const uintptr_t mid=sq_points_mid(batch->points);
    const nm_cached_vertex_t *p[4];
    for(unsigned k=0;k<face->count;++k) {
        static const uint8_t from_quad[4]={2,0,1,3};
        p[k]=sq_corner_point(mid,face->corner[face->count==4 ? from_quad[k] : k]);
        if(!sq_point_drawable(p[k])) { ++cull_unprojected; return; }
    }
    for(unsigned t=1;t+1<face->count;++t) {
        const double area=((double)p[t]->x-p[0]->x)*((double)p[t+1]->y-p[0]->y)-((double)p[t+1]->x-p[0]->x)*((double)p[t]->y-p[0]->y);
        ++cull_winding[rear!=0][area>0 ? 0 : area<0 ? 1 : 2];
    }
}
#endif
/* Pixel check (SIM_PIXEL_FACES). A model of the PVR's set-up in single precision: per triangle, plane equations of
 * the written depth (1/w), u/w and v/w over the screen, from its three vertices; a pixel is sampled at its centre and
 * belongs to a triangle when it lies inside all three edges (an edge through the centre counts for top and left edges
 * only, so two triangles sharing an edge never both take a pixel); u = (u/w)/(1/w). The two splits of one quad are
 * compared at every pixel of its bounding box: covered or not (by either triangle), depth bits, texel (point sampled,
 * wrapping), and whether u or v moved by more than 1/64 texel (what bilinear weights would see). */
typedef struct { float x,y,z,u,v; } pix_vertex_t;
typedef struct { int ok; float area; pix_vertex_t a; float e[3][3]; float dz[3],du[3],dv[3]; } pix_tri_t;
static pix_tri_t pix_setup(const pix_vertex_t *p0,const pix_vertex_t *p1,const pix_vertex_t *p2) {
    pix_tri_t t; memset(&t,0,sizeof t);
    const pix_vertex_t *p[3]={p0,p1,p2};
    float area=(p1->x-p0->x)*(p2->y-p0->y)-(p2->x-p0->x)*(p1->y-p0->y);
    if(area==0) return t;
    if(area<0) { p[1]=p2; p[2]=p1; area=-area; }
    t.ok=1; t.area=area; t.a=*p[0];
    for(unsigned k=0;k<3;++k) {
        const pix_vertex_t *a=p[k],*b=p[(k+1)%3];
        /* inside: (b.x-a.x)*(y-a.y)-(b.y-a.y)*(x-a.x) >= 0; e = {dy coefficient, dx coefficient, constant} */
        t.e[k][0]=-(b->y-a->y); t.e[k][1]=b->x-a->x; t.e[k][2]=(b->y-a->y)*a->x-(b->x-a->x)*a->y;
    }
    const float w[3][3]={{p[0]->z,p[1]->z,p[2]->z},{p[0]->u*p[0]->z,p[1]->u*p[1]->z,p[2]->u*p[2]->z},{p[0]->v*p[0]->z,p[1]->v*p[1]->z,p[2]->v*p[2]->z}};
    float *out[3]={t.dz,t.du,t.dv};
    for(unsigned k=0;k<3;++k) {
        const float f1=w[k][1]-w[k][0],f2=w[k][2]-w[k][0];
        out[k][0]=(f1*(p[2]->y-p[0]->y)-f2*(p[1]->y-p[0]->y))/area;
        out[k][1]=(f2*(p[1]->x-p[0]->x)-f1*(p[2]->x-p[0]->x))/area;
        out[k][2]=w[k][0];
    }
    return t;
}
static int pix_inside(const pix_tri_t *t,float x,float y) {
    if(!t->ok) return 0;
    for(unsigned k=0;k<3;++k) {
        const float e=t->e[k][0]*x+t->e[k][1]*y+t->e[k][2];
        if(e<0) return 0;
        /* top-left: an edge going down the screen (left) or a horizontal edge going right (top) owns its pixels */
        if(e==0 && !(t->e[k][0]<0 || (t->e[k][0]==0 && t->e[k][1]<0))) return 0;
    }
    return 1;
}
static void pix_sample(const pix_tri_t *t,float x,float y,float *z,float *u,float *v) {
    const float dx=x-t->a.x,dy=y-t->a.y;
    *z=t->dz[2]+t->dz[0]*dx+t->dz[1]*dy;
    *u=(t->du[2]+t->du[0]*dx+t->du[1]*dy)/ *z; *v=(t->dv[2]+t->dv[0]*dx+t->dv[1]*dy)/ *z;
}
typedef struct { unsigned face; uint8_t rotated; int du,dv; } pix_face_t;
static pix_face_t *pix_faces; static unsigned pix_face_count; static int32_t *pix_lookup;
static unsigned long long pix_checked,pix_frames_faces,pix_pixels,pix_coverage,pix_depth,pix_texel,pix_subtexel,pix_faces_differing,pix_clipped,pix_odd;
/* One quad (4 distinct vertices of its two submitted triangles) or triangle, as submitted and as the source draws it. */
static void pix_compare(const pix_face_t *pf,const pix_vertex_t tri[2][3],unsigned triangles,unsigned w,unsigned h) {
    pix_vertex_t now[2][3],before[2][3];
    memcpy(now,tri,sizeof(pix_vertex_t)*3*triangles);
    if(triangles==2) {
        /* The diagonal: the two vertices both triangles hold. */
        unsigned shared[2],n=0,other[2]={3,3};
        for(unsigned i=0;i<3;++i) for(unsigned j=0;j<3;++j) if(!memcmp(tri[0]+i,tri[1]+j,sizeof(pix_vertex_t)) && n<2) shared[n++]=i;
        if(n!=2) { ++pix_odd; return; }
        for(unsigned i=0;i<3;++i) if(i!=shared[0] && i!=shared[1]) other[0]=i;
        for(unsigned j=0;j<3;++j) if(memcmp(tri[1]+j,tri[0]+shared[0],sizeof(pix_vertex_t)) && memcmp(tri[1]+j,tri[0]+shared[1],sizeof(pix_vertex_t))) other[1]=j;
        const pix_vertex_t d0=tri[0][shared[0]],d1=tri[0][shared[1]],o0=tri[0][other[0]],o1=tri[1][other[1]];
        if(pf->rotated) { before[0][0]=o0; before[0][1]=o1; before[0][2]=d0; before[1][0]=o0; before[1][1]=o1; before[1][2]=d1; }
        else memcpy(before,tri,sizeof(pix_vertex_t)*6);
    } else memcpy(before,tri,sizeof(pix_vertex_t)*3);
    for(unsigned t=0;t<triangles;++t) for(unsigned k=0;k<3;++k) { before[t][k].u-=(float)pf->du; before[t][k].v-=(float)pf->dv; }
    pix_tri_t a[2],b[2];
    float lx=1e30f,ly=1e30f,hx=-1e30f,hy=-1e30f;
    for(unsigned t=0;t<triangles;++t) {
        a[t]=pix_setup(now[t],now[t]+1,now[t]+2); b[t]=pix_setup(before[t],before[t]+1,before[t]+2);
        for(unsigned k=0;k<3;++k) { lx=fminf(lx,now[t][k].x); hx=fmaxf(hx,now[t][k].x); ly=fminf(ly,now[t][k].y); hy=fmaxf(hy,now[t][k].y); }
    }
    int x0=(int)floorf(lx)-1,x1=(int)ceilf(hx)+1,y0=(int)floorf(ly)-1,y1=(int)ceilf(hy)+1;
    if(x0<0) x0=0; if(y0<0) y0=0; if(x1>640*ENJ_XSCALE) x1=640*ENJ_XSCALE; if(y1>480) y1=480;
    const unsigned long long before_count=pix_coverage+pix_depth+pix_texel+pix_subtexel;
    ++pix_checked;
    for(int y=y0;y<y1;++y) for(int x=x0;x<x1;++x) {
        const float cx=(float)x+0.5f,cy=(float)y+0.5f;
        int ia=-1,ib=-1;
        for(unsigned t=0;t<triangles;++t) { if(pix_inside(a+t,cx,cy)) ia=(int)t; if(pix_inside(b+t,cx,cy)) ib=(int)t; }
        if(ia<0 && ib<0) continue;
        ++pix_pixels;
        if((ia<0)!=(ib<0)) { ++pix_coverage; continue; }
        float za,ua,va,zb,ub,vb;
        pix_sample(a+ia,cx,cy,&za,&ua,&va); pix_sample(b+ib,cx,cy,&zb,&ub,&vb);
        if(memcmp(&za,&zb,sizeof za)) ++pix_depth;
        const long ta=(long)floorf(ua*(float)w),tb=(long)floorf(ub*(float)w),sa=(long)floorf(va*(float)h),sb=(long)floorf(vb*(float)h);
        if(((ta-tb)%(long)w) || ((sa-sb)%(long)h)) ++pix_texel;
        else if(fabsf((ua-ub-(float)(ta-tb)/(float)w)*(float)w)>1.0f/64 || fabsf((va-vb-(float)(sa-sb)/(float)h)*(float)h)>1.0f/64) ++pix_subtexel;
    }
    pix_faces_differing+=pix_coverage+pix_depth+pix_texel+pix_subtexel!=before_count;
}
/* SIM_ORACLE=1: every frame's submitted stream against the reference renderer
 * (platform/dreamcast-native/tests/reference_stream.h), triangle by triangle. */
#include "reference_stream.h"
static nm_draw_material_t *sim_cache[2];
static float stream_reach[3];
static nm_reference_triangle_t *oracle_fast,*oracle_reference; static uint32_t *oracle_order;
static unsigned long long oracle_frames,oracle_matched,oracle_skipped,oracle_reference_total,oracle_instances;
static int oracle_order_compare(const void *a,const void *b) {
    const nm_reference_triangle_t *x=oracle_fast+*(const uint32_t *)a,*y=oracle_fast+*(const uint32_t *)b;
    if(x->source!=y->source) return x->source<y->source ? -1 : 1;
    if(x->instance!=y->instance) return x->instance<y->instance ? -1 : 1;
    if(x->list!=y->list) return x->list<y->list ? -1 : 1;
    /* The translucent list is submitted back to front (model_submission.h): the reference's face order per instance. */
    if(x->list==PVR_LIST_TR_POLY && x->face!=y->face) return x->face<y->face ? -1 : 1;
    return x<y ? -1 : x>y;
}

typedef struct { uint8_t source,list,layer,pad; uint16_t object,header; uint32_t face; float v[3][5]; uint32_t argb; } tri_t;
typedef struct { uint8_t source,list; float v[3][5]; } geo_t;
/* No face identity: what the PVR receives per triangle. wrap: UVs reduced by whole texture periods per triangle. */
typedef struct { uint8_t source,list; uint16_t header; uint32_t argb; float v[3][5]; } col_t;
static int col_compare(const void *a,const void *b) { return memcmp(a,b,sizeof(col_t)); }
/* A triangle as the picture sees it (culling is off): its three vertices in any order, the colour of its
 * provoking vertex and the header's 32 bytes (not the header's first-seen index, which follows submission
 * order). The frame's "set" digest is the sorted set of these: two packs that draw the same triangles in
 * any order and either winding have the same one; "setwrap" also takes u and v modulo their period. */
typedef struct { uint8_t source,list,pad[2]; uint32_t argb; uint64_t header; float v[3][5]; } set_t;
static int set_compare(const void *a,const void *b) { return memcmp(a,b,sizeof(set_t)); }
static void set_fill(set_t *out,unsigned s,unsigned list,uint64_t header,uint32_t argb,float key[3][5]) {
    memset(out,0,sizeof *out); out->source=(uint8_t)s; out->list=(uint8_t)list; out->header=header; out->argb=argb;
    unsigned order[3]={0,1,2};
    for(unsigned i=0;i<3;++i) for(unsigned j=i+1;j<3;++j) if(memcmp(key[order[j]],key[order[i]],sizeof key[0])<0) { unsigned t=order[i]; order[i]=order[j]; order[j]=t; }
    for(unsigned i=0;i<3;++i) memcpy(out->v[i],key[order[i]],sizeof key[0]);
}
static int geo_compare(const void *a,const void *b) { return memcmp(a,b,sizeof(geo_t)); }
static int tri_compare(const void *a,const void *b) { return memcmp(a,b,sizeof(tri_t)); }
static uint64_t fnv(const void *data,size_t n,uint64_t h) { const uint8_t *p=data; while(n--) h=(h^*p++)*UINT64_C(1099511628211); return h; }
/* Bindings actually drawn, per source: triangles, colour and header. */
static uint64_t *use_triangles[2]; static uint32_t *use_argb[2]; static uint16_t *use_header[2];

static nm_pack_t packs[2]; static nm_material_pack_t mats[2];
static nm_header_t live_header[2];
static const nm_instance_t *world_original; static unsigned world_original_count;
static unsigned object_boundary,object_unjudged; static unsigned long long object_checked,object_frames,object_reference_frames;
static const pvr_poly_hdr_t **shared[2];
static pvr_poly_hdr_t canon[65536]; static unsigned canon_count;
static uint8_t world_slots[NM_MATERIAL_PALETTES_MAX];
static unsigned palette_banks,world_vram,world_textures,stage_build;
/* TA by class: world opaque, world punch-through, then cars by detail level (0..3, 4 other) and list. Per class
 * vertices, triangles, strips (EOL), headers; totals and per-frame maxima. */
enum { SIM_CLASSES=12 };
static const char *const sim_class_names[SIM_CLASSES]={"world-op","world-pt","car-op-L0","car-pt-L0","car-op-L1","car-pt-L1",
    "car-op-L2","car-pt-L2","car-op-L3","car-pt-L3","car-op-other","car-pt-other"};
static uint8_t sim_car_level[4096];
static unsigned long long sim_class_total[SIM_CLASSES][4],sim_class_max[SIM_CLASSES][4];
static uint32_t *live_to_pack; /* live world face index (after the stage selection) -> index in the pack */
/* Level of detail statistics. Class of a group: 1 a fence panel (stipple material), 0 anything else (trees). */
static unsigned lod_count; static uint8_t *lod_class,*lod_before,*oracle_lod_state;
static unsigned long long lod_walked[2],lod_at_level[2][NM_LOD_LEVELS],lod_changes[2],lod_first_seen[2]; static unsigned lod_changes_max[2],lod_walked_max[2];
/* A pop: a group that was walked in the frame before changes level while a triangle of it is submitted in this frame or was in the frame before. */
static unsigned long long lod_pops[2],lod_running[2]; static unsigned lod_pops_max[2]; static uint32_t *lod_last_walk; static uint8_t *lod_shown;
static uint8_t *lod_seen; static FILE *lod_out;
/* Sweep: fence panels across a sweep plane per frame (drawn as clipped bands), and the frames with any. */
static unsigned long long lod_straddling,lod_straddling_frames; static unsigned lod_straddling_max;
static unsigned sim_lod_jump(unsigned a,unsigned b) {
    const unsigned a0=a>=0x80u ? a&3u : a,a1=a>=0x80u ? (a>>2)&3u : a,b0=b>=0x80u ? b&3u : b,b1=b>=0x80u ? (b>>2)&3u : b;
    return a1<b0 || b1<a0;
}
static uint8_t *oracle_sweep_group; static float oracle_sweep_plane[NM_LOD_LEVELS-1]; static unsigned oracle_sweep_planes;

/* The player's damaged bodies (car-damage.dnb, render/scene/damage_bodies.h): variants written into the clean body's
 * mesh, after which nm_render_car_mesh_refresh (sq_car_mesh_refresh) redoes that mesh's tables. */
static nm_damage_bodies_t damage_bodies; static unsigned damage_mesh=UINT_MAX;
/* SIM_DAMAGE_CHECK=manual: the same for the manual car's body 960dae (render/scene/practice.c): the variants written
 * into its mesh in its own face order (nm_damage_body_face_map), the light class reserve one a face (practice_assets.c
 * with the manual transmission). */
static uint16_t damage_faces[1024]; static const uint16_t *damage_face_of;
/* SIM_DAMAGE_CHECK=1: every variant written and refreshed, then the tables of the whole pack built again from scratch
 * (sq_facing_prepare, as nm_render_init): light classes, plane constants and hot face records must be equal.
 * SIM_DAMAGE_CHECK=hoods / hoods-manual: the same for the hood view's hoods (car-hoods.dnb) in the clean hood 94fff9's
 * mesh / the manual clean hood 9506a8's in its face order (render/scene/practice.c practice_player_hood). */
static int damage_check(void) {
    const nm_header_t *original=packs[0].data; const nm_mesh_t *mesh=packs[0].meshes+damage_mesh;
    const unsigned first=mesh->face_first,count=mesh->face_count;
    uint16_t *classes=malloc(count*2); nm_sq_face_plane_t *planes=malloc(count*sizeof(*planes)); nm_sq_lit_face_t *lit=malloc(count*sizeof(*lit));
    unsigned bad=0;
    for(unsigned v=0;v<damage_bodies.count;++v) {
        if(!nm_damage_body_apply_mapped(&damage_bodies,v,packs,damage_mesh,damage_face_of) || !sq_car_mesh_refresh(packs,damage_mesh)) { printf("damage check: variant %u refused\n",v); return 0; }
        unsigned light_classes=0;
        if(!sq_light_classes_mesh(packs,mesh,sq_light_class,&light_classes)) { printf("damage check: variant %u classes refused\n",v); return 0; }
        memcpy(classes,sq_light_class+first,count*2); memcpy(planes,sq_face_planes[0]+first,count*sizeof(*planes));
        memcpy(lit,sq_lit_faces+first,count*sizeof(*lit));
        const unsigned size=sq_light_cache_size;
        if(!sq_facing_prepare(0,packs) || sq_light_cache_size!=size || !sq_light_class || !sq_face_planes[0] || !sq_lit_faces) { printf("damage check: tables refused\n"); return 0; }
        const int same=!memcmp(classes,sq_light_class+first,count*2) && !memcmp(planes,sq_face_planes[0]+first,count*sizeof(*planes)) &&
            !memcmp(lit,sq_lit_faces+first,count*sizeof(*lit));
        bad+=!same;
        printf("damage check: variant %2u model %06x%s: %u light classes of %u, refreshed tables %s the rebuilt ones\n",v,
            (unsigned)nm_damage_body(&damage_bodies,v)->model,damage_face_of ? " in the manual car's mesh" : "",light_classes,sq_light_cache_size,
            same ? "equal" : "DIFFER from");
    }
    (void)original; free(classes); free(planes); free(lit);
    return !bad;
}

#ifdef SIM_HAS_LIVE
/* The live specular faces (model_submission.h sq_live_update) against the reference arithmetic: for TRIALS view turns
 * (yaw over the whole circle, pitch up to 75 degrees either way, a small roll), each live face's binding of either
 * side must be that of lighting.h's luma for the supplied normal and the race light turned by the view (the side
 * forced by corner 2 along or against the normal), or of a luma one step from it (the dot rounds otherwise); its
 * stream record must carry that binding's colour and header. Also counts the classes whose luma moved. */
static int live_check(const nm_pack_t *pack,unsigned trials) {
    if(!sq_live_face_count) { fputs("live check: the world pack has no live specular face\n",stderr); return 0; }
    unsigned checked=0,exact=0,step=0,bad=0,records=0,record_bad=0,lifted=0;
    uint8_t *moved=calloc(sq_live_class_count,1);
    uint16_t *first=malloc(sq_live_class_count*sizeof(*first));
    uint32_t seed=12345;
    for(unsigned t=0;t<trials;++t) {
        seed=seed*1103515245u+12345u; const float yaw=(float)(seed>>8)*(6.2831853f/16777216.0f);
        seed=seed*1103515245u+12345u; const float pitch=((float)(seed>>8)/16777216.0f-0.5f)*2.618f;
        seed=seed*1103515245u+12345u; const float roll=((float)(seed>>8)/16777216.0f-0.5f)*0.3f;
        const float cy=cosf(yaw),sy=sinf(yaw),cp=cosf(pitch),sp=sinf(pitch),cr=cosf(roll),sr=sinf(roll);
        /* rows of R = Rz(roll) Rx(pitch) Ry(yaw) */
        const float ry[3][3]={{cy,0,-sy},{0,1,0},{sy,0,cy}}, rx[3][3]={{1,0,0},{0,cp,sp},{0,-sp,cp}}, rz[3][3]={{cr,sr,0},{-sr,cr,0},{0,0,1}};
        float a[3][3],r[3][3];
        for(int i=0;i<3;++i) for(int j=0;j<3;++j) { a[i][j]=0; for(int k=0;k<3;++k) a[i][j]+=rx[i][k]*ry[k][j]; }
        for(int i=0;i<3;++i) for(int j=0;j<3;++j) { r[i][j]=0; for(int k=0;k<3;++k) r[i][j]+=rz[i][k]*a[k][j]; }
        float view[12]={0}; /* column-major: column c holds R's column c */
        for(int c=0;c<3;++c) for(int row=0;row<3;++row) view[3*c+row]=r[row][c];
        sq_live_update(pack,view,sq_emit_records[1]);
        float light[3];
        for(int row=0;row<3;++row) light[row]=r[row][0]*nm_race_world_light[0]+r[row][1]*nm_race_world_light[1]+r[row][2]*nm_race_world_light[2];
        for(unsigned i=0;i<sq_live_face_count;++i) {
            const nm_sq_live_face_t *e=sq_live_faces+i;
            const nm_sq_live_class_t *k=sq_live_classes+e->live_class;
            const nm_face_t *face=pack->faces+e->face;
            float n[3];
            for(int row=0;row<3;++row) n[row]=r[row][0]*face->normal[0]+r[row][1]*face->normal[1]+r[row][2]*face->normal[2];
            for(unsigned side=0;side<(k->two ? 2u : 1u);++side) {
                float points[3][3]={{0}};
                for(int c=0;c<3;++c) points[2][c]=side ? -n[c] : n[c];
                const int reference=nm_face_lighting(1,n,points,light,pack->materials+face->material);
                const unsigned got=side ? sq_baked_faces[e->face].back : sq_baked_faces[e->face].front;
                const uint16_t *row=world_draw.bindings+face->material*256u;
                const int luma=reference&255;
                ++checked;
                if(row[luma]==got) ++exact;
                else if((luma>0 && row[luma-1]==got) || (luma<255 && row[luma+1]==got)) ++step;
                else { if(bad<8) fprintf(stderr,"live check: face %u side %u luma %d, binding %u not %u\n",(unsigned)e->face,side,luma,got,row[luma]); ++bad; }
                if(!side) {
                    if(!t) first[e->live_class]=(uint16_t)got;
                    else if(first[e->live_class]!=got) moved[e->live_class]=1;
                    if(luma>(int)k->base[0]) ++lifted;
                }
            }
#if SQ_STREAMS
            unsigned live_mesh=0;
            while(live_mesh<packs[1].header->meshes && !(packs[1].meshes[live_mesh].face_first<=e->face &&
                  e->face<packs[1].meshes[live_mesh].face_first+packs[1].meshes[live_mesh].face_count)) ++live_mesh;
            if(sq_stream_meshes && sq_stream_meshes[live_mesh] && e->record!=UINT32_MAX) {
                const nm_sq_stream_face_t *out=sq_stream_table[live_mesh].faces+e->record;
                const unsigned front=sq_baked_faces[e->face].front;
                ++records;
                record_bad+=out->front!=front || out->argb!=sq_emit_records[1][front].argb || out->header!=sq_emit_records[1][front].group->header ||
                    out->back!=sq_baked_faces[e->face].back || !(out->flags&SQ_STREAM_SIDE) || out->run;
            }
#endif
        }
    }
    unsigned classes_moved=0;
    for(unsigned c=0;c<sq_live_class_count;++c) classes_moved+=moved[c];
    printf("live check: %u turns, %u live faces in %u classes; %u side lumas: %u the reference's binding, %u one step off, %u wrong; "
        "%u front lumas above the specular-free level; %u classes changed binding; stream records %u checked, %u wrong\n",
        trials,sq_live_face_count,sq_live_class_count,checked,exact,step,bad,lifted,classes_moved,records,record_bad);
    free(moved); free(first);
    return !bad && !record_bad && classes_moved;
}
#endif

static int setup(const char *assets) {
    char file[512];
    const char *names[2]={"car",getenv("SIM_WORLD") ? getenv("SIM_WORLD") : "world"};
    for(unsigned s=0;s<2;++s) {
        snprintf(file,sizeof file,"%s/%s.dnm",assets,names[s]); if(!nm_pack_load(packs+s,file)) return 0;
        snprintf(file,sizeof file,"%s/%s.dnt",assets,names[s]); if(!nm_material_pack_open_cpu(mats+s,file)) return 0;
    }
    const char *check=getenv("SIM_DAMAGE_CHECK");
    const int hoods=check && !strncmp(check,"hoods",5);
    snprintf(file,sizeof file,"%s/%s",assets,hoods ? "car-hoods.dnb" : "car-damage.dnb");
    if(!(hoods ? nm_damage_hoods_load(&damage_bodies,file) : nm_damage_bodies_load(&damage_bodies,file))) return 0;
    if(hoods && !damage_bodies.data) { printf("damage check: %s absent\n",file); return 0; }
    if(damage_bodies.data) {
        for(unsigned i=0;i<packs[0].header->instances;++i) if(packs[0].instances[i].source_address==damage_bodies.body) damage_mesh=packs[0].instances[i].mesh;
        if(damage_mesh==UINT_MAX) return 0;
        sq_light_class_reserve=damage_bodies.classes;
        if(check && (!strcmp(check,"manual") || !strcmp(check,"hoods-manual"))) {
            const uint32_t twin=hoods ? 0x9506a8u : 0x960daeu;
            unsigned manual=UINT_MAX;
            for(unsigned i=0;i<packs[0].header->instances;++i) if(packs[0].instances[i].source_address==twin) manual=packs[0].instances[i].mesh;
            if(manual==UINT_MAX || !nm_damage_body_face_map(packs,damage_mesh,manual,damage_faces,1024)) { printf("damage check: no manual map\n"); return 0; }
            damage_mesh=manual;damage_face_of=damage_faces;
            if(damage_bodies.faces>sq_light_class_reserve) sq_light_class_reserve=damage_bodies.faces;
        }
    }
    world_original=packs[1].instances; world_original_count=packs[1].header->instances;
    const unsigned material_stage=stage_build>=1 ? stage_build-1 : 0;
    live_to_pack=malloc(packs[1].header->faces*4);
    for(unsigned m=0;m<packs[1].header->meshes;++m) {
        /* The selection keeps the faces of the selected materials in pack order. */
        const nm_mesh_t *mesh=packs[1].meshes+m; unsigned live=0;
        for(unsigned f=0;f<mesh->face_count;++f)
            if((packs[1].materials[packs[1].faces[mesh->face_first+f].material].padding[0]&NM_SCENERY_STAGE_MASK)<=material_stage)
                live_to_pack[mesh->face_first+live++]=mesh->face_first+f;
    }
    if(!nm_scenery_stage_select(packs+1,material_stage)) return 0;
    const unsigned flags=nm_pack_flags(packs+1); (void)flags;
    unsigned vertices=0,faces=0;
    for(unsigned s=0;s<2;++s) for(unsigned m=0;m<packs[s].header->meshes;++m) {
        if(packs[s].meshes[m].vertex_count>vertices) vertices=packs[s].meshes[m].vertex_count;
        if(packs[s].meshes[m].face_count>faces) faces=packs[s].meshes[m].face_count;
    }
    fprintf(stderr,"largest mesh: %u vertices, %u faces\n",vertices,faces);
    draw_groups=malloc(faces+1);
    draw_layer_scale_init();
    uint16_t *lookup[2]; nm_draw_material_t *cache[2];
    for(unsigned s=0;s<2;++s) {
        const nm_pack_t *pack=packs+s;
        lookup[s]=malloc(pack->header->materials*256*sizeof(uint16_t));
        cache[s]=calloc(mats[s].count,sizeof(nm_draw_material_t));
        for(unsigned m=0;m<pack->header->materials;++m) for(unsigned l=0;l<256;++l) {
            const nm_material_binding_t *b=nm_material_find(mats+s,pack->materials[m].header,l);
            lookup[s][m*256+l]=b ? (uint16_t)(b-mats[s].bindings) : UINT16_MAX;
        }
        for(unsigned i=0;i<mats[s].count;++i) cache[s][i].uv=nm_material_uv(mats+s,mats[s].bindings+i);
        use_triangles[s]=calloc(mats[s].count,sizeof(uint64_t)); use_argb[s]=calloc(mats[s].count,4); use_header[s]=calloc(mats[s].count,2);
    }
    /* The car's table as the renderer makes it (model_submission.h sq_luma_runs_build): one run a material when
     * the pack allows it, else the dense table. */
    draw_runs=malloc(packs[0].header->materials*sizeof(uint32_t));
    if(!draw_runs || !sq_luma_runs_build(packs,mats,draw_runs)) { free(draw_runs);draw_runs=NULL; }
    for(unsigned m=0;draw_runs && m<packs[0].header->materials;++m) for(unsigned l=0;l<256;++l)
        if(sq_lookup(NULL,draw_runs,m,l)!=lookup[0][m*256+l]) { fprintf(stderr,"car luma run %u/%u differs from the dense table\n",m,l);return 0; }
    fprintf(stderr,"car material/luma table: %s\n",draw_runs ? "one run a material (equal to the dense table)" : "dense");
#ifdef SIM_HAS_LISTS
    for(unsigned s=0;s<2;++s) { sq_material_lists_prepare(s,packs+s,mats+s,lookup[s],s ? NULL : draw_runs);
        fprintf(stderr,"lists from the material header, source %u: %s\n",s,sq_material_lists[s] ? "on" : "off"); }
#endif
    sim_cache[0]=cache[0]; sim_cache[1]=cache[1];
    draw_pack=packs; draw_materials=mats; draw_material_cache=cache[0]; draw_bindings=lookup[0];
    world_draw.pack=packs+1; world_draw.materials=mats+1; world_draw.cache=cache[1]; world_draw.bindings=lookup[1];
    if(!(flags&NM_PACK_BAKED_LIGHT) || !(flags&NM_PACK_SCENERY_CLASSES) || !mats[1].palette_index) { fputs("world pack is not the baked palette pack\n",stderr); return 0; }
    uint8_t *used=calloc(mats[1].resources,1);
    { /* Road surface class and baked bindings: same rules as nm_render_world_attach. */
        const nm_pack_t *pack=packs+1;
        uint8_t *material_class=calloc(pack->header->materials,1);
        world_draw.surface=calloc(mats[1].count,1);
        nm_baked_face_t *table=malloc(pack->header->faces*sizeof(*table));
        unsigned checker=0,live=0;
        for(unsigned m=0;m<pack->header->meshes;++m) {
            const nm_mesh_t *mesh=pack->meshes+m;
            for(unsigned f=0;f<mesh->face_count;++f) {
                const nm_face_t *face=pack->faces+mesh->face_first+f;
                material_class[face->material]|=pack->materials[face->material].padding[0]&NM_SCENERY_SURFACE ? 1 : 2;
                nm_baked_face_t *out=table+mesh->face_first+f;
                const int two_sided=(face->attribute&(1u<<17))!=0;
                out->front=world_draw.bindings[face->material*256+face->reserved];
                out->back=two_sided ? world_draw.bindings[face->material*256+face->padding] : UINT16_MAX;
                assert(out->front!=UINT16_MAX && (!two_sided || out->back!=UINT16_MAX));
                for(unsigned side=0;side<(two_sided ? 2u : 1u);++side) {
                    const nm_material_binding_t *binding=mats[1].bindings+(side ? out->back : out->front);
                    if(binding->resource!=NM_MATERIAL_SOLID) used[binding->resource]=1;
                }
                ++live;
                checker+=(pack->materials[face->material].header[0]&0x8000)!=0;
            }
        }
        for(unsigned m=0;m<pack->header->materials;++m) for(unsigned l=0;l<256;++l) {
            const unsigned index=world_draw.bindings[m*256+l];
            if(index!=UINT16_MAX) world_draw.surface[index]|=material_class[m]==1 ? 1 : 2;
        }
        unsigned n=0;
        for(unsigned i=0;i<mats[1].count;++i) { world_draw.surface[i]=world_draw.surface[i]==1; n+=world_draw.surface[i]; }
        sq_surface_bindings=world_draw.surface; free(material_class);
        sq_baked_faces=table;
#ifdef SIM_HAS_LIVE
        /* As nm_render_world_attach: the live specular faces (NM_PACK_LIVE_SPECULAR) and their reachable bindings. */
        if(!sq_live_prepare(pack,world_draw.bindings,mats+1,used)) return 0;
#endif
        for(unsigned r=0;r<mats[1].resources;++r) if(used[r]) { world_vram+=mats[1].textures[r].size-32; ++world_textures; }
        fprintf(stderr,"stage %u: live world faces %u, checker %u, road surface bindings %u, world textures %u of %u = %u bytes VRAM\n",
            stage_build,live,checker,n,world_textures,(unsigned)mats[1].resources,world_vram);
    }
    palette_banks=nm_material_palette_share(mats,mats+1,world_slots,NM_CAR_PALETTE_BANKS);
    if(!palette_banks) { fputs("palette banks exceeded\n",stderr); return 0; }
    fprintf(stderr,"palette banks in use %u of %u (cars %u, world %u palettes)\n",palette_banks,NM_CAR_PALETTE_BANKS,
        (unsigned)mats[0].palette_count,(unsigned)mats[1].palette_count);
    /* Synthetic PVR headers: what compile_draw_header makes distinct (list,
     * texture, UV flip, palette bank, shading, shadow receiver), then the
     * same canonical sharing as sq_share_resident_headers. */
    for(unsigned s=0;s<2;++s) {
        shared[s]=malloc(mats[s].count*sizeof(*shared[s]));
        for(unsigned i=0;i<mats[s].count;++i) {
            const nm_material_binding_t *b=mats[s].bindings+i;
            const unsigned th=b->header[0];
            pvr_poly_hdr_t h={0};
            if(s && b->resource!=NM_MATERIAL_SOLID && !used[b->resource]) { shared[s][i]=&cache[s][i].header; continue; } /* No header. */
            h.cmd=0x80840000u|(draw_binding_list(b)<<24);
            h.mode1=b->resource==NM_MATERIAL_SOLID ? 0 : 1u+b->resource+(s<<20);
            h.mode2=b->resource==NM_MATERIAL_SOLID ? 0 : (th&0x300);
            const int palette=nm_material_palette(mats+s,b);
            if(palette>=0) h.mode2|=(NM_CAR_PALETTE_BANK_FIRST+(s ? world_slots[palette] : (unsigned)palette))<<16;
            if(mats[s].palette_index) h.cmd|=1u<<30; /* stands for "Gouraud off": textured and solid alike */
            h.mode3=s && world_draw.surface[i];
            unsigned c=0;
            for(;c<canon_count;++c) if(!memcmp(canon+c,&h,sizeof h)) break;
            if(c==canon_count) { assert(canon_count<65536); canon[canon_count++]=h; }
            cache[s][i].header=h; shared[s][i]=canon+c;
        }
        sq_shared_headers[s]=shared[s];
#ifdef SIM_HAS_RECORDS
        if(!getenv("SIM_NO_RECORDS") && !sq_emit_records_prepare(s)) return 0;
        fprintf(stderr,"emission records, source %u: %s\n",s,sq_emit_records[s] ? "on" : "off");
#endif
        if(!sq_flat_strips_prepare(s,packs+s,mats+s) || !sq_emit_faces_prepare(s,packs+s)) return 0;
#ifdef SIM_HAS_FACING
        if(!getenv("SIM_NO_FACING") && !sq_facing_prepare(s,packs+s)) return 0;
        fprintf(stderr,"face sides from plane constants, source %u: %s\n",s,sq_face_planes[s] ? "on" : "off");
#endif
    }
    free(used);
#ifdef SIM_HAS_CLASSES
    /* The emission table in two parts (bindings and classes, model_submission.h) against the values it was
     * built from: every binding expands to its header bytes, colour, UV mapping and depth class; two bindings
     * name the same pooled header exactly when their headers are equal (the emitters compare addresses), across
     * both sources; a class is stored once. */
    if(sq_emit_records[0] && sq_emit_records[1]) {
        unsigned long long bindings=0;
        for(unsigned s=0;s<2;++s) {
            for(unsigned i=0;i<mats[s].count;++i) {
                nm_sq_emit_t expected,stored;
                sq_emit_lookup(&expected,s,i); sq_emit_expand(&stored,sq_emit_records[s]+i);
                const nm_sq_class_t *group=sq_emit_records[s][i].group;
                const pvr_poly_hdr_t *header=group->header;
                int pooled=0;
                for(unsigned t=0;t<2;++t) pooled|=header>=sq_header_pool[t] && header<sq_header_pool[t]+sq_header_pool_count[t];
                if(!pooled || group<sq_emit_classes[s] || group>=sq_emit_classes[s]+sq_emit_class_count[s] ||
                   memcmp(stored.header,expected.header,sizeof(pvr_poly_hdr_t)) || stored.argb!=expected.argb ||
                   memcmp(stored.origin,expected.origin,sizeof stored.origin) || memcmp(stored.scale,expected.scale,sizeof stored.scale) ||
                   memcmp(&stored.depth,&expected.depth,sizeof stored.depth)) {
                    fprintf(stderr,"emission table differs from its source: source %u binding %u\n",s,i); return 0;
                }
                ++bindings;
            }
            for(unsigned i=0;i<sq_header_pool_count[s];++i) for(unsigned t=s;t<2;++t)
                for(unsigned j=t==s ? i+1 : 0;j<sq_header_pool_count[t];++j)
                    if(!memcmp(sq_header_pool[s]+i,sq_header_pool[t]+j,sizeof(pvr_poly_hdr_t))) { fputs("a header is pooled twice\n",stderr); return 0; }
            for(unsigned i=0;i<sq_emit_class_count[s];++i) for(unsigned j=i+1;j<sq_emit_class_count[s];++j)
                if(sq_emit_classes[s][i].header==sq_emit_classes[s][j].header && !memcmp(sq_emit_classes[s][i].origin,sq_emit_classes[s][j].origin,5*sizeof(float))) {
                    fputs("an emission class is stored twice\n",stderr); return 0;
                }
        }
        fprintf(stderr,"emission table: %llu bindings equal to their source values; classes cars %u, world %u; pooled headers cars %u, world %u more and %u of the cars'\n",
            bindings,sq_emit_class_count[0],sq_emit_class_count[1],sq_header_pool_count[0],sq_header_pool_count[1],sq_header_borrowed[1]);
    }
#endif
#ifdef SIM_HAS_WIDE
    sq_wide_view=!getenv("SIM_NO_WIDE");
    fprintf(stderr,"wide view (near-plane clipping only): %s\n",sq_wide_view ? "on" : "off");
#endif
    /* The console sizes the world's point cache and face queues to one frame (nm_render_world_capacity, the 5x5 cells
     * around the camera); here the renderer's limit of 2^16 stands in for it (Expert's world has 76,201 points in all). */
    sq_world_point_bound=sq_world_face_bound=65536;
    if(!sq_allocate(0,packs) || !sq_allocate(1,packs+1) || !sq_translucent_prepare(packs)) return 0; /* as nm_render_init */
    /* As nm_render_world_attach: after the stage selection. */
    lod_count=nm_pack_lod_count(packs+1);
    if(lod_count) {
        lod_last_walk=calloc(lod_count,4); lod_shown=calloc(lod_count,1);
        sim_lod_drawn=malloc(lod_count); lod_class=calloc(lod_count,1); lod_before=calloc(lod_count,1); oracle_lod_state=calloc(lod_count,1); lod_seen=calloc(lod_count,1);
        if(!sq_lod_prepare(packs+1)) return 0;
        unsigned by_class[2]={0,0};
        for(unsigned g=0;g<lod_count;++g) {
            const nm_lod_group_t *group=packs[1].lod_groups+g;
            const unsigned faces=(unsigned)group->face_count[0]+group->face_count[1]+group->face_count[2];
            for(unsigned f=0;f<faces;++f) if(packs[1].materials[packs[1].faces[group->face_first+f].material].header[1]==0x8000u) lod_class[g]=1;
            ++by_class[lod_class[g]];
        }
        fprintf(stderr,"level of detail: %u groups (%u cutouts, %u fence panels)\n",lod_count,by_class[0],by_class[1]);
        /* The reference's own sweep groups and planes (reference_stream.h), whatever the renderer was built with. */
        oracle_sweep_group=calloc(lod_count,1);
        oracle_sweep_planes=nm_reference_sweep_find(packs+1,oracle_sweep_group,oracle_sweep_plane);
        unsigned sweep_groups=0; for(unsigned g=0;g<lod_count;++g) sweep_groups+=oracle_sweep_group[g];
        fprintf(stderr,"fence sweep: renderer %s (%u groups, %u planes); reference finds %u groups, %u planes\n",SQ_SWEEPS ? "on" : "off",
            sq_sweep_groups,sq_sweep_planes,sweep_groups,oracle_sweep_planes);
#if SQ_SWEEPS
        assert(sq_sweep_planes==oracle_sweep_planes && sq_sweep_groups==sweep_groups);
        for(unsigned k=0;k<sq_sweep_planes;++k) assert(sq_sweep_plane[k]==oracle_sweep_plane[k]);
#endif
    }
#ifdef SIM_HAS_SS
    /* As nm_render_world_attach: the super-sampled objects' own headers, after both emission tables. */
    if(!sq_ss_prepare(packs+1)) return 0;
    oracle_ss_state=calloc(packs[1].header->meshes,1);
    fprintf(stderr,"object super-sampling: %s (%u headers)\n",sq_ss_level ? "on" : "off",sq_ss_header_count);
#endif
#if defined(SIM_HAS_STREAM) && SQ_STREAMS
    /* As nm_render_world_attach: the prebuilt world streams after every table they are built from.
     * SIM_NO_STREAM=1: none (make NATIVE_SQ_STREAM=0 builds the same renderer without them). */
    /* SIM_STREAM_POOL=BYTES: the record cache of a course without the whole tables (model_submission.h,
     * NM_SQ_STREAM_POOL), built between frames with SIM_STREAM_BUDGET microseconds of the builder's estimate
     * (default 2000; the host clock counts 3 a face built): the stream must be the same. */
    if(getenv("SIM_STREAM_POOL")) { sq_stream_wanted=0; sq_stream_pool_wanted=(uint32_t)strtoul(getenv("SIM_STREAM_POOL"),NULL,0); }
    if(!getenv("SIM_NO_STREAM") && !sq_stream_prepare(packs+1)) return 0;
    fprintf(stderr,"prebuilt world streams: %s\n",sq_stream_faces ? "on" : "off");
#endif
#ifdef SIM_HAS_LIVE
    /* SIM_LIVE_CHECK=N: N view turns through sq_live_update against lighting.h (see live_check); then exit. */
    if(getenv("SIM_LIVE_CHECK")) exit(live_check(packs+1,(unsigned)atoi(getenv("SIM_LIVE_CHECK"))) ? 0 : 1);
#endif
#if defined(SIM_HAS_CAR_STREAM) && SQ_CAR_STREAMS
    /* As nm_render_init: the prebuilt car streams after the car's emission records, emission faces, plane constants and
     * batches. SIM_NO_CAR_STREAM=1: none (make NATIVE_SQ_CAR_STREAM=0 builds the same renderer without them). */
    if(!getenv("SIM_NO_CAR_STREAM") && !sq_car_stream_prepare(packs)) return 0;
    fprintf(stderr,"prebuilt car streams: %s\n",sq_car_meshes ? "on" : "off");
#endif
    /* SIM_DAMAGE_VARIANT=k: the scene drew damaged body k (0..15: crash_model[0][k]; 16 the clean body) all along. */
    if(damage_bodies.data && getenv("SIM_DAMAGE_CHECK") && !damage_check()) return 0;
    if(damage_bodies.data && getenv("SIM_DAMAGE_VARIANT")) {
        const unsigned v=(unsigned)atoi(getenv("SIM_DAMAGE_VARIANT"));
        if(!nm_damage_body_apply(&damage_bodies,v,packs,damage_mesh) || !sq_car_mesh_refresh(packs,damage_mesh)) return 0;
        fprintf(stderr,"damaged body %u (%06x) in mesh %u\n",v,(unsigned)nm_damage_body(&damage_bodies,v)->model,damage_mesh);
    }
    for(unsigned s=0;s<2;++s) { live_header[s]=*packs[s].header; packs[s].header=live_header+s; }
    stream=malloc(STREAM_MAX*sizeof(*stream)); stream_source=malloc(STREAM_MAX);
    stream_face=malloc(STREAM_MAX*4); stream_binding=malloc(STREAM_MAX*2); stream_instance=malloc(STREAM_MAX*4); stream_clip=calloc(STREAM_MAX,1); stream_polygon=calloc(STREAM_MAX,4);
    stream_ss=calloc(STREAM_MAX,1);
    return stream_ss && stream && stream_source && stream_face && stream_binding;
}

/* ---- software depth buffer -------------------------------------------- */
enum { DW=640, DH=480 };
typedef struct { float x[3],y[3],z[3]; float scale; uint8_t source,list,layer,surface,stage; uint32_t face;
    float u[3],v[3]; uint32_t strip; uint16_t binding; uint8_t kind; uint32_t argb;
    /* extra: 0 a model triangle; 1 HUD quad (sheet); 2 sky page; 3 sky fill. bilinear/textured: how the TSP samples it. */
    uint8_t extra,sheet,bilinear,textured; } dtri_t;
/* Detail of the differences: [winner kind][true-nearest kind][winner layered][ratio bucket]; kinds: car, then world stage 0..4. */
static uint64_t depth_detail[7][7][2][4];
/* World face pairs (written winner, true nearest) more than 0.1% apart: pixels and largest ratio. */
enum { PAIR_SLOTS=1u<<18 };
static struct { uint64_t key,pixels; float ratio; } *pair_table;
static void pair_add(uint32_t winner,uint32_t nearest,double ratio) {
    if(!pair_table) pair_table=calloc(PAIR_SLOTS,sizeof(*pair_table));
    const uint64_t key=((uint64_t)(winner+1)<<32)|(nearest+1);
    unsigned slot=(unsigned)((key*UINT64_C(0x9e3779b97f4a7c15))>>46);
    while(pair_table[slot].key && pair_table[slot].key!=key) slot=(slot+1)&(PAIR_SLOTS-1);
    pair_table[slot].key=key; ++pair_table[slot].pixels;
    if(ratio>pair_table[slot].ratio) pair_table[slot].ratio=(float)ratio;
}
static float *depth_written,*depth_true,*winner_true_depth; static int32_t *winner_written,*winner_true; static uint8_t *top_layer;
static struct {
    uint64_t pixels,same,car_over_surface,scenery_over_surface,scenery_over_surface_clear,decal_by_layer,decal_lost,tie,other;
    double car_ratio_max,scenery_ratio_max; long frames; uint64_t layered_pixels;
} depth_total;
/* Double precision: triangles now reach far outside the window (the PVR clips
 * them), where float32 edge functions would lose whole pixels. */
static inline int tri_setup(const dtri_t *t,double *a,double *b,double *c,int box[4]) {
    const double x0=t->x[0],y0=t->y[0],x1=t->x[1],y1=t->y[1],x2=t->x[2],y2=t->y[2];
    const double area=(x1-x0)*(y2-y0)-(x2-x0)*(y1-y0);
    if(!(fabs(area)>1e-6)) return 0;
    const double z0=t->z[0],z1=t->z[1],z2=t->z[2];
    *a=((z1-z0)*(y2-y0)-(z2-z0)*(y1-y0))/area;
    *b=((x1-x0)*(z2-z0)-(x2-x0)*(z1-z0))/area;
    *c=z0-*a*x0-*b*y0;
    double lx=fmin(x0,fmin(x1,x2)),hx=fmax(x0,fmax(x1,x2)),ly=fmin(y0,fmin(y1,y2)),hy=fmax(y0,fmax(y1,y2));
    box[0]=(int)fmax(0,floor(lx)); box[1]=(int)fmin(DW-1,ceil(hx)); box[2]=(int)fmax(0,floor(ly)); box[3]=(int)fmin(DH-1,ceil(hy));
    return box[0]<=box[1] && box[2]<=box[3];
}
static inline int tri_covers(const dtri_t *t,double px,double py) {
    double sign=0;
    for(unsigned e=0;e<3;++e) {
        const unsigned n=(e+1)%3;
        const double w=((double)t->x[n]-t->x[e])*(py-t->y[e])-((double)t->y[n]-t->y[e])*(px-t->x[e]);
        if(w==0) continue;
        if(sign==0) sign=w; else if((w>0)!=(sign>0)) return 0;
    }
    return 1;
}
/* The picture as far as geometry decides it: per pixel of the 640x480 window
 * the face (source, list, live face index) that wins the depth test among
 * opaque and punch-through polygons, and the set of translucent faces that
 * cover the pixel. Independent of how a face is cut into triangles. */
static uint32_t *image_winner,*image_translucent; static uint64_t image_digest;
/* Fill: every pixel sample a submitted triangle covers inside the window, by
 * kind (car, world stage 0..4) and list (OP, PT, TR), and per world pack face
 * with the samples where that face is the depth winner. */
static uint64_t fill_total[7][3],fill_face[1u<<16],win_face[1u<<16];
/* Punch-through samples per frame, and those not behind an opaque-list
 * polygon (written depth): the opaque list is resolved first, so only the
 * latter reach the punch-through passes of a tile. World and cars. */
static uint64_t pt_sum,pt_max,pt_open_sum,pt_open_max,pt_world_sum,pt_world_max; static float *fill_opaque;
/* Sampled frames in which a pack face submitted a triangle. */
static uint32_t seen_face[1u<<16],seen_stamp[1u<<16],seen_frame;
static void fill_frame(const dtri_t *tris,unsigned count) {
    if(!fill_opaque) fill_opaque=malloc(DW*DH*sizeof(float));
    for(unsigned i=0;i<DW*DH;++i) fill_opaque[i]=0;
    uint64_t frame_pt=0,frame_open=0,frame_world=0;
    ++seen_frame;
    for(unsigned pass=0;pass<2;++pass) for(unsigned i=0;i<count;++i) {
        const dtri_t *t=tris+i;
        if((t->list==PVR_LIST_OP_POLY)!=(pass==0)) continue;
        if(t->source && seen_stamp[live_to_pack[t->face]]!=seen_frame) { seen_stamp[live_to_pack[t->face]]=seen_frame; ++seen_face[live_to_pack[t->face]]; }
        double a,b,c; int box[4];
        if(!tri_setup(t,&a,&b,&c,box)) continue;
        uint64_t covered=0;
        for(int y=box[2];y<=box[3];++y) for(int x=box[0];x<=box[1];++x) {
            if(!tri_covers(t,x+0.5,y+0.5)) continue;
            ++covered;
            const float z=(float)(a*(x+0.5)+b*(y+0.5)+c);
            float *o=fill_opaque+(unsigned)y*DW+(unsigned)x;
            if(!pass) { if(z>*o) *o=z; }
            else if(t->list==PVR_LIST_PT_POLY) frame_open+=z>=*o;
        }
        fill_total[!t->source ? 0 : 1u+t->stage][t->list==PVR_LIST_OP_POLY ? 0 : t->list==PVR_LIST_PT_POLY ? 1 : 2]+=covered;
        if(t->list==PVR_LIST_PT_POLY) { frame_pt+=covered; if(t->source) frame_world+=covered; }
        if(t->source) fill_face[live_to_pack[t->face]]+=covered;
    }
    pt_sum+=frame_pt; pt_open_sum+=frame_open; pt_world_sum+=frame_world;
    if(frame_pt>pt_max) pt_max=frame_pt;
    if(frame_open>pt_open_max) pt_open_max=frame_open;
    if(frame_world>pt_world_max) pt_world_max=frame_world;
}
static void depth_frame(const dtri_t *tris,unsigned count) {
    const unsigned n=DW*DH;
    fill_frame(tris,count);
    if(!depth_written) {
        depth_written=malloc(n*4); depth_true=malloc(n*4); winner_true_depth=malloc(n*4);
        winner_written=malloc(n*4); winner_true=malloc(n*4); top_layer=malloc(n);
    }
    for(unsigned i=0;i<n;++i) { depth_written[i]=depth_true[i]=0; winner_written[i]=winner_true[i]=-1; top_layer[i]=0; }
    /* Opaque list, then punch-through (drawn as covering: texel alpha is not
     * modelled); the translucent list does not decide a winner. */
    for(unsigned pass=0;pass<2;++pass) for(unsigned i=0;i<count;++i) {
        const dtri_t *t=tris+i;
        if(t->list!=(pass ? PVR_LIST_PT_POLY : PVR_LIST_OP_POLY)) continue;
        double a,b,c; int box[4];
        if(!tri_setup(t,&a,&b,&c,box)) continue;
        for(int y=box[2];y<=box[3];++y) for(int x=box[0];x<=box[1];++x) {
            const double px=x+0.5,py=y+0.5;
            if(!tri_covers(t,px,py)) continue;
            const float z=(float)(a*px+b*py+c),zt=z/t->scale;
            const unsigned p=(unsigned)y*DW+(unsigned)x;
            if(z>=depth_written[p]) { depth_written[p]=z; winner_written[p]=(int32_t)i; winner_true_depth[p]=zt; }
            if(zt>depth_true[p]) { depth_true[p]=zt; winner_true[p]=(int32_t)i; }
        }
    }
    for(unsigned i=0;i<count;++i) {
        const dtri_t *t=tris+i;
        if(t->list==PVR_LIST_TR_POLY || !t->layer) continue;
        double a,b,c; int box[4];
        if(!tri_setup(t,&a,&b,&c,box)) continue;
        for(int y=box[2];y<=box[3];++y) for(int x=box[0];x<=box[1];++x) {
            const double px=x+0.5,py=y+0.5;
            if(!tri_covers(t,px,py)) continue;
            const unsigned p=(unsigned)y*DW+(unsigned)x;
            const float zt=(float)(a*px+b*py+c)/t->scale;
            /* In the top plane of this pixel: within 1e-4 of the nearest true depth. */
            if(zt>=depth_true[p]*(1.0f-1e-4f) && t->layer>top_layer[p]) top_layer[p]=t->layer;
        }
    }
    if(!image_winner) { image_winner=malloc(n*4); image_translucent=malloc(n*4); }
    for(unsigned p=0;p<n;++p) {
        image_translucent[p]=0;
        if(winner_written[p]<0) { image_winner[p]=0; continue; }
        const dtri_t *w=tris+winner_written[p];
        image_winner[p]=1u+w->face+((uint32_t)w->source<<24)+((uint32_t)(w->list==PVR_LIST_PT_POLY)<<25);
    }
    for(unsigned i=0;i<count;++i) {
        const dtri_t *t=tris+i;
        if(t->list!=PVR_LIST_TR_POLY) continue;
        double a,b,c; int box[4];
        if(!tri_setup(t,&a,&b,&c,box)) continue;
        for(int y=box[2];y<=box[3];++y) for(int x=box[0];x<=box[1];++x) {
            if(!tri_covers(t,x+0.5,y+0.5)) continue;
            const unsigned p=(unsigned)y*DW+(unsigned)x;
            /* Only where the panel is not behind the opaque winner; a set, so
             * a pixel on the shared diagonal of a face counts it once. */
            if((float)(a*(x+0.5)+b*(y+0.5)+c)>=depth_written[p]) image_translucent[p]|=(t->face+1u)*2654435761u;
        }
    }
    for(unsigned p=0;p<n;++p) if(winner_written[p]>=0 && tris[winner_written[p]].source) ++win_face[live_to_pack[tris[winner_written[p]].face]];
    image_digest=fnv(image_translucent,n*4,fnv(image_winner,n*4,UINT64_C(1469598103934665603)));
    for(unsigned p=0;p<n;++p) {
        if(winner_written[p]<0) continue;
        ++depth_total.pixels;
        const dtri_t *w=tris+winner_written[p],*t=tris+winner_true[p];
        const int in_top=winner_true_depth[p]>=depth_true[p]*(1.0f-1e-4f);
        if(top_layer[p]) ++depth_total.layered_pixels;
        if(in_top && top_layer[p]>w->layer) { ++depth_total.decal_lost; continue; }
        if(w==t) { ++depth_total.same; continue; }
        const double ratio=(double)depth_true[p]/(double)winner_true_depth[p]; /* >1: the written winner is truly behind */
        const int wc=!w->source ? 0 : w->surface ? 1 : 2, tc=!t->source ? 0 : t->surface ? 1 : 2;
        {
            const unsigned wk=!w->source ? 0 : 1u+w->stage, tk=!t->source ? 0 : 1u+t->stage;
            const unsigned bucket=ratio<1.001 ? 0 : ratio<1.01 ? 1 : ratio<1.1 ? 2 : 3;
            if(!(in_top && w->layer>t->layer)) {
                ++depth_detail[wk][tk][w->layer!=0][bucket];
                if(bucket && w->source && t->source) pair_add(live_to_pack[w->face],live_to_pack[t->face],ratio);
            }
        }
        if(in_top && w->layer>t->layer) ++depth_total.decal_by_layer;
        else if(wc==0 && tc==1) { ++depth_total.car_over_surface; if(ratio>depth_total.car_ratio_max) depth_total.car_ratio_max=ratio; }
        else if(wc==2 && tc==1) {
            ++depth_total.scenery_over_surface; depth_total.scenery_over_surface_clear+=ratio>1.02;
            if(ratio>depth_total.scenery_ratio_max) depth_total.scenery_ratio_max=ratio;
        }
        else if(ratio<1.0+1e-5) ++depth_total.tie;
        else ++depth_total.other;
    }
    ++depth_total.frames;
}

/* ---- PVR work model (SIM_PVR) -------------------------------------------
 * What the tile renderer is charged for in this model: the tile-based
 * deferred renderer as KOS drives it (dc/pvr.h, dc/pvr/pvr_regs.h
 * PVR_ISP_FEED_CFG, PVR_OPB_CFG) and as the console's render times were
 * fitted (38 hardware runs, render ms = 7.2 + 28.4 ns x pixels textured for
 * the punch-through alpha test + 216 ns x punch-through triangle passes):
 * - The TA files every triangle under each 32x32 tile its bounding box
 *   touches (tritiles), a strip as entries of up to four triangles (entries).
 * - Opaque list, per tile: the ISP processes every filed triangle once, the
 *   TSP textures the visible pixels once.
 * - Punch-through list, per tile: the filed triangles are taken in chunks of
 *   SIM_PT_CHUNK vertices in registration order. Per chunk the ISP finds the
 *   nearest not yet examined fragment of every pixel in front of what is
 *   drawn so far, the TSP textures those pixels and the alpha test decides:
 *   opaque texel, the pixel is drawn (and hides what lies behind it); clear
 *   texel, the pixel goes to the next pass. Passes repeat until no triangle
 *   of the chunk has a fragment left (discard mode: a triangle without one
 *   leaves the list). So:
 *     pt_passes     passes over all tiles and chunks
 *     pt_tripasses  triangles processed by the ISP, summed over the passes
 *     pt_linepasses the same in tile lines the triangles cross (the model
 *                   charges the ISP one step per line of a triangle; op_lines
 *                   is the opaque list's count, processed once)
 *     pt_tsp        pixels textured for the alpha test (pt_tsp_clear of them
 *                   were transparent and bought nothing)
 * - pt_px: punch-through pixel samples as rasterised, hidden or not;
 *   pt_layers: punch-through fragments per pixel, the largest of a tile, as
 *   max and mean over the tiles that have any.
 * Texel alpha: point sampled from the base level, palette entry 15 of a punch-through binding is
 * the hole (tools/prepare_palette_materials.py). Equal depths are not
 * ordered as the hardware would. A model for ranking, not a timing.
 *
 * Opaque list, per class and in all (modelled as: the ISP processes every
 * opaque polygon of the tile, the TSP then textures the visible pixels):
 *   op_lines     tile lines the opaque triangles cross (ISP clocks)
 *   op_px        pixel samples they cover (the overdraw the ISP removes)
 *   vis_*        the 307,200 pixels by what the TSP has to do for the one
 *                visible opaque fragment: bilinear (four palette texels),
 *                point (one), flat (untextured), skytex (VQ, point), bg (no
 *                polygon: the background plane)
 *   op_vis_tritiles  triangle-tile entries with at least one visible pixel:
 *                each needs its texture and shading parameters set up
 *                (a per-entry cost the model counts but does not price; the
 *                hardware fit above leaves it in the constant term)
 *   op_tile_entries_max / op_tiles_over7 (pt_ likewise): object list entries
 *                of the fullest tile, and tiles whose entries exceed the
 *                seven an 8-word object pointer block holds before it links
 *                to an overflow block
 * With SIM_SIDE the frame also holds, as on the console:
 * - the sky: up to four opaque quads at NM_SKY_DEPTH (class sky);
 * - the HUD (class hud), texel alpha from the sheets: from a side file of
 *   whole quads, punch-through quads at depth 1 after every model (the HUD
 *   until 13 October); from one of tight rectangles (render/ui/hud_draw.h),
 *   opaque cores and punch-through pieces at their depths in front of the
 *   models, or all punch-through at depth 1 with SIM_HUD_FLAT=1;
 * - the cars' shadow volumes (code/render/scene/practice.c,
 *   practice_shadow_volumes: the same outline, reach, projection and
 *   rejection rules), on the opaque modifier list:
 *     mod_volumes, mod_tris, mod_tritiles, mod_tiles (tiles with an entry),
 *     mod_lines (tile lines of the volume triangles, charged to the ISP
 *     like opaque polygons of the same size: a model assumption, not
 *     measured separately),
 *     mod_px (pixel samples of the volume triangles), mod_inside (pixels
 *     whose opaque surface lies inside a volume), mod_shadowed (those whose
 *     surface is a receiver, the road-surface class: drawn again by the
 *     TSP with the shadow intensity).
 *   extra_ta: TA bytes of sky, HUD and volumes.
 * Translucent list (the car effects' smoke, pre-sorted: code/main.c), per frame:
 *   tr_tris, tr_strips, tr_tritiles (filed as above), tr_lines (tile lines: ISP clocks, one pass in pre-sort mode),
 *   tr_px (samples rasterised), tr_front_px (of them in front of the opaque surface, which the TSP textures and
 *   blends; punch-through occluders are not subtracted, so an upper bound), tr_layers_max (deepest per-pixel stack
 *   of front samples), tr_ns: tr_front_px at the pre-sort price measured on hardware, 9.5 ns a sample
 *   (build/dc-native-gpu-split.ScloIy/hw/results.txt, stage 3; auto-sort 9.7 at two layers, punch-through 11.9). */
#define PVR_TR_SAMPLE_NS 9.5
#define PVR_PT_SAMPLE_NS 11.9
enum { PT_W=DW/32, PT_H=DH/32, PT_TILES=PT_W*PT_H, PVR_KINDS=40, PVR_BUILTIN_KINDS=11, KIND_ROAD=7, KIND_HUD=8, KIND_SKY=9, KIND_PROBE=10 };
enum { V_OP_TRIS,V_OP_STRIPS,V_OP_ENTRIES,V_OP_TRITILES,V_PT_TRIS,V_PT_STRIPS,V_PT_ENTRIES,V_PT_TRITILES,V_PT_PX,V_PT_TILES,V_PT_CHUNKS,
       V_PT_PASSES,V_PT_TRIPASSES,V_PT_TSP,V_PT_TSP_CLEAR,V_PT_LAYERS_MAX,V_PT_LAYERS_MEAN100,V_PT_PASS_MAX,V_OP_LINES,V_PT_LINEPASSES,
       V_OP_PX,V_VIS_BILINEAR,V_VIS_POINT,V_VIS_FLAT,V_VIS_SKYTEX,V_VIS_BG,V_OP_TILE_ENTRIES_MAX,V_OP_TILES_OVER7,V_PT_TILE_ENTRIES_MAX,V_PT_TILES_OVER7,
       V_MOD_VOLUMES,V_MOD_TRIS,V_MOD_TRITILES,V_MOD_TILES,V_MOD_LINES,V_MOD_PX,V_MOD_INSIDE,V_MOD_SHADOWED,V_EXTRA_TA,V_OP_VIS_TRITILES,
       V_TR_TRIS,V_TR_STRIPS,V_TR_TRITILES,V_TR_LINES,V_TR_PX,V_TR_FRONT_PX,V_TR_LAYERS_MAX,V_TR_NS,
       V_SMOKE_TRIS,V_SMOKE_PX,V_SMOKE_FRONT_PX,V_SMOKE_LAYERS_MAX,V_SMOKE_NS,V_COUNT };
static const char *pvr_names[V_COUNT]={"op_tris","op_strips","op_entries","op_tritiles","pt_tris","pt_strips","pt_entries","pt_tritiles","pt_px","pt_tiles","pt_chunks",
    "pt_passes","pt_tripasses","pt_tsp","pt_tsp_clear","pt_layers_max","pt_layers_mean100","pt_pass_max","op_lines","pt_linepasses",
    "op_px","vis_bilinear","vis_point","vis_flat","vis_skytex","vis_bg","op_tile_entries_max","op_tiles_over7","pt_tile_entries_max","pt_tiles_over7",
    "mod_volumes","mod_tris","mod_tritiles","mod_tiles","mod_lines","mod_px","mod_inside","mod_shadowed","extra_ta","op_vis_tritiles",
    "tr_tris","tr_strips","tr_tritiles","tr_lines","tr_px","tr_front_px","tr_layers_max","tr_ns",
    "smoke_tris","smoke_px","smoke_front_px","smoke_layers_max","smoke_ns"};
/* Opaque list per class, beside tris/tritiles/strips: tile lines, covered samples, visible pixels, of them bilinear. */
enum { KO_LINES,KO_PX,KO_VISIBLE,KO_BILINEAR,KO_VIS_TRITILES,KO_COUNT };
static const char *pvr_kind_op_fields[KO_COUNT]={"lines","px","visible","bilinear","vis_tritiles"};
static uint64_t pvr_kind_op_total[PVR_KINDS][KO_COUNT];
enum { K_TRIS,K_TRITILES,K_PX,K_TRIPASSES,K_TSP,K_CLEAR,K_STRIPS,K_COUNT };
static const char *pvr_kind_fields[K_COUNT]={"tris","tritiles","px","tripasses","tsp","tsp_clear","strips"}; /* TA vertices of a class = tris + 2*strips */
/* Classes: cars; world faces by material stage, those of the road-surface depth class apart as "road"; HUD; sky. */
static char pvr_kind_names[PVR_KINDS][24]={"car","reduced","opaque","ground","cutout","checker","objects","road","hud","sky","probe"}; static unsigned pvr_kinds=PVR_BUILTIN_KINDS;
static struct { uint16_t word; uint8_t kind; } pvr_class[256]; static unsigned pvr_classes;
static FILE *pvr_out; static unsigned pvr_chunk=21; static int sim_smoke_bound;
static uint64_t pvr_total[V_COUNT],pvr_max[V_COUNT],pvr_kind_total[PVR_KINDS][2][K_COUNT]; static long pvr_frames;
static struct { uint64_t value; unsigned tick; } pvr_worst[2][6];
typedef struct { unsigned w,h; uint8_t *opaque,*index; } pvr_alpha_t;
static pvr_alpha_t *pvr_alpha[2];
static const pvr_alpha_t *pvr_texture(unsigned s,unsigned resource) {
    if(!pvr_alpha[s]) pvr_alpha[s]=calloc(mats[s].resources,sizeof(pvr_alpha_t));
    pvr_alpha_t *a=pvr_alpha[s]+resource;
    if(a->opaque) return a;
    const nm_texture_record_t *r=mats[s].textures+resource;
    uint8_t *file=malloc(r->size);
    if(!nm_material_texture_read(mats+s,resource,file,r->size)) { fprintf(stderr,"SIM_PVR: texture %u of source %u cannot be read\n",resource,s); exit(1); }
    const uint32_t mode=(uint32_t)file[16]|(uint32_t)file[17]<<8|(uint32_t)file[18]<<16|(uint32_t)file[19]<<24;
    if((mode>>27&7)!=5) { fputs("SIM_PVR needs PAL4 textures\n",stderr); exit(1); }
    a->w=r->width; a->h=r->height; a->opaque=malloc(a->w*a->h); a->index=malloc(a->w*a->h);
    /* A square mip chain ends with the base level, the only one sampled here. */
    unsigned base=32;
    if(mode>>31) { base+=2; for(unsigned level=2;level<a->w;level*=2) base+=level*level/2; }
    const unsigned side=a->w<a->h ? a->w : a->h;
    for(unsigned y=0;y<a->h;++y) for(unsigned x=0;x<a->w;++x) {
        unsigned index=(x/side+y/side)*side*side;
        for(unsigned bit=0;(1u<<bit)<side;++bit) index|=((y>>bit)&1u)<<(2*bit) | ((x>>bit)&1u)<<(2*bit+1);
        a->index[y*a->w+x]=(file[base+index/2]>>(index&1 ? 4 : 0))&15;
        a->opaque[y*a->w+x]=a->index[y*a->w+x]!=15;
    }
    free(file); return a;
}
#if NM_SMOKE_DOOR && NM_SMOKE_TIGHT
/* The renderer's table of tight smoke pieces for the reference (nm_reference_source_t.tight). */
static int sim_tight(unsigned mesh,unsigned f,float piece[][4],unsigned *roles) {
    if(!sq_tr_tight || sq_tr_tight_base[mesh]==UINT16_MAX) return -1;
    const nm_sq_tr_tight_t *e=sq_tr_tight+sq_tr_tight_base[mesh]+f;
    if(e->count==SQ_TR_TIGHT_WHOLE) return -1;
    for(unsigned p=0;p<e->count;++p) { const nm_sq_tr_piece_t *q=sq_tr_pieces+e->first+p;
        piece[p][0]=q->a[0]; piece[p][1]=q->a[1]; piece[p][2]=q->b[0]; piece[p][3]=q->b[1]; }
    *roles=e->roles; return e->count;
}
#define SIM_TIGHT sim_tight
#else
#define SIM_TIGHT NULL
#endif
#if NM_SMOKE_DOOR && NM_SMOKE_TIGHT
/* The renderer's tight smoke geometry (model_submission.h, sq_tr_tight_prepare): the texture's non-hole texels, from the
 * car material pack (materials.c reads the console's copy). */
int nm_smoke_texture_mask(unsigned resource,unsigned side,uint8_t *mask) {
    const pvr_alpha_t *a=pvr_texture(0,resource);
    if(a->w!=side || a->h!=side) return 0;
    for(unsigned i=0;i<side*side;++i) mask[i]=a->index[i]!=15;
    if(getenv("SIM_SMOKE_MASKS")) { /* experiment: the masks as raw bytes, one file per texture */
        char name[512]; snprintf(name,sizeof name,"%s/mask-%u.bin",getenv("SIM_SMOKE_MASKS"),resource);
        FILE *f=fopen(name,"wb"); if(f) { fwrite(mask,1,side*side,f); fclose(f); } }
    return 1;
}
#endif
/* Level `level` (0: the base) of a square PAL4 texture with a mip chain, as rows of palette indices (SIM_COLOR_MIP); NULL
 * without a chain or beyond its 2x2 level. Cached. */
static const uint8_t *pvr_texture_level(unsigned s,unsigned resource,unsigned level) {
    static uint8_t **levels[2]; static unsigned counts[2];
    const nm_texture_record_t *r=mats[s].textures+resource;
    if(!levels[s]) { counts[s]=mats[s].resources; levels[s]=calloc(counts[s]*8u,sizeof(uint8_t *)); }
    if(level>=8 || r->width!=r->height || (r->width>>level)<2) return NULL;
    uint8_t **slot=levels[s]+resource*8u+level;
    if(*slot) return *slot;
    uint8_t *file=malloc(r->size);
    if(!nm_material_texture_read(mats+s,resource,file,r->size)) { free(file); return NULL; }
    const uint32_t mode=(uint32_t)file[16]|(uint32_t)file[17]<<8|(uint32_t)file[18]<<16|(uint32_t)file[19]<<24;
    if((mode>>27&7)!=5 || !(mode>>31)) { free(file); return NULL; }
    const unsigned side=r->width>>level;
    unsigned base=32+2;
    for(unsigned l=2;l<side;l*=2) base+=l*l/2;
    uint8_t *rows=malloc(side*side);
    for(unsigned y=0;y<side;++y) for(unsigned x=0;x<side;++x) {
        unsigned index=0;
        for(unsigned bit=0;(1u<<bit)<side;++bit) index|=((y>>bit)&1u)<<(2*bit) | ((x>>bit)&1u)<<(2*bit+1);
        rows[y*side+x]=(file[base+index/2]>>(index&1 ? 4 : 0))&15;
    }
    free(file); *slot=rows; return rows;
}
static inline void pvr_plane(const dtri_t *t,const double q[3],double out[3]) {
    const double x0=t->x[0],y0=t->y[0],x1=t->x[1],y1=t->y[1],x2=t->x[2],y2=t->y[2];
    const double area=(x1-x0)*(y2-y0)-(x2-x0)*(y1-y0);
    out[0]=((q[1]-q[0])*(y2-y0)-(q[2]-q[0])*(y1-y0))/area;
    out[1]=((x1-x0)*(q[2]-q[0])-(x2-x0)*(q[1]-q[0]))/area;
    out[2]=q[0]-out[0]*x0-out[1]*y0;
}
static inline unsigned pvr_wrap(double coordinate,unsigned size,int mirror) {
    long texel=(long)floor(coordinate*size);
    if(!mirror) return (unsigned)(((texel%(long)size)+(long)size)%(long)size);
    const long period=2*(long)size; texel=((texel%period)+period)%period;
    return (unsigned)(texel>=(long)size ? period-1-texel : texel);
}
/* Sky, HUD and shadow volumes of the frame (SIM_SIDE, SIM_HUD). */
static FILE *side_in; static pvr_alpha_t hud_sheets[8]; static unsigned hud_sheet_count;
static int sim_no_hud,sim_no_sky,sim_no_volumes,sim_no_receivers,sim_no_cars,sim_no_world,sim_fill_probe;
static pvr_alpha_t probe_alpha; /* 8x8, solid where x and y are even: a quarter of the texels */
static float shadow_outline[2][NM_SHADOW_OUTLINE_MAX][3]; static unsigned shadow_outline_count[2];
static struct { unsigned count; const uint8_t *kind; const float (*matrix)[12]; const float *view; float focus; } frame_shadows;
/* The road class's depth scale of this frame (render/scene/practice.c practice_road_depth_pick): the reference and the
 * models take this value, the renderer its tables as sq_road_depth_set left them. */
static float sim_road_depth=NM_ROAD_DEPTH_SCALE;
static uint64_t frame_extra_ta;
static int hud_sheets_load(const char *folder) {
    char name[512]; uint32_t palette[2+16*8];
    snprintf(name,sizeof name,"%s/hud/hud.dp",folder);
    FILE *f=fopen(name,"rb"); if(!f) return 0;
    if(fread(palette,4,2,f)!=2 || palette[0]!=0x4c415044u || palette[1]>16*8 || fread(palette+2,4,palette[1],f)!=palette[1]) { fclose(f); return 0; }
    fclose(f);
    for(hud_sheet_count=0;hud_sheet_count<8;++hud_sheet_count) { /* A palette bank can span sheets: as many as there are files. */
        snprintf(name,sizeof name,"%s/hud/sheet-%u.dt",folder,hud_sheet_count);
        if(!(f=fopen(name,"rb"))) break;
        uint32_t head[8]; if(fread(head,4,8,f)!=8 || head[0]!=0x78546344u || (head[4]>>27&7)!=5) { fclose(f); return 0; }
        pvr_alpha_t *a=hud_sheets+hud_sheet_count; a->w=head[3]&0xffffu; a->h=head[3]>>16;
        uint8_t *data=malloc(a->w*a->h/2); if(fread(data,1,a->w*a->h/2,f)!=a->w*a->h/2) { fclose(f); return 0; }
        fclose(f);
        a->opaque=malloc(a->w*a->h); a->index=malloc(a->w*a->h);
        const unsigned side=a->w<a->h ? a->w : a->h;
        for(unsigned y=0;y<a->h;++y) for(unsigned x=0;x<a->w;++x) {
            unsigned index=(x/side+y/side)*side*side;
            for(unsigned bit=0;(1u<<bit)<side;++bit) index|=((y>>bit)&1u)<<(2*bit) | ((x>>bit)&1u)<<(2*bit+1);
            a->index[y*a->w+x]=(data[index/2]>>(index&1 ? 4 : 0))&15;
            a->opaque[y*a->w+x]=a->index[y*a->w+x]!=0; /* Entry 0 of every bank is the transparent one (prepare_hud.py). */
        }
        free(data);
    }
    return hud_sheet_count>0;
}
/* One quad in strip order (four corners x,y,u,v) as two triangles of the model. */
static unsigned extra_quad(dtri_t *out,const float corner[4][4],float z,unsigned list,unsigned kind,unsigned extra,unsigned sheet,uint32_t strip) {
    for(unsigned t=0;t<2;++t) {
        dtri_t *d=out+t; memset(d,0,sizeof *d);
        d->list=(uint8_t)list; d->kind=(uint8_t)kind; d->extra=(uint8_t)extra; d->sheet=(uint8_t)sheet; d->scale=1.0f; d->strip=strip;
        d->textured=extra!=3; d->argb=0xffffffffu;
        for(unsigned v=0;v<3;++v) { const float *c=corner[t+v]; d->x[v]=c[0]; d->y[v]=c[1]; d->z[v]=z; d->u[v]=c[2]; d->v[v]=c[3]; }
    }
    return 2;
}
/* The frame's shadow volumes as practice_shadow_volumes submits them; returns the triangle count. */
static unsigned shadow_triangles(dtri_t *out,unsigned capacity,unsigned *volumes) {
    unsigned count=0; *volumes=0;
    const float cx=248.0f,cy=384.0f-(270.0f-128.0f),focus=frame_shadows.focus; /* PRACTICE_CENTER_X, 384-PRACTICE_CENTER_Y */
    const float *up=frame_shadows.view+3;
    for(unsigned s=0;s<frame_shadows.count;++s) {
        const unsigned kind=frame_shadows.kind[s],n=kind<2 ? shadow_outline_count[kind] : 0;
        if(!n) continue;
        float ring[2][NM_SHADOW_OUTLINE_MAX][3]; const float *m=frame_shadows.matrix[s];
        unsigned valid=1,left=1,right=1,above=1,below=1;
        for(unsigned i=0;i<n && valid;++i) {
            const float *p=shadow_outline[kind][i];
            const float tx=p[0]*m[0]+p[1]*m[3]+p[2]*m[6]+m[9],ty=p[0]*m[1]+p[1]*m[4]+p[2]*m[7]+m[10],tz=p[0]*m[2]+p[1]*m[5]+p[2]*m[8]+m[11];
            for(unsigned side=0;side<2;++side) {
                const float reach=side ? -1.0f : 1.0f; /* PRACTICE_SHADOW_REACH */
                const float x=tx+up[0]*reach,y=ty+up[1]*reach,z=tz+up[2]*reach;
                if(!(z>0.1f)) { valid=0;break; }
                const float q=1.0f/z; float *o=ring[side][i];
                o[0]=cx+x*focus*q;o[1]=cy-y*focus*q;o[2]=q*sim_road_depth;
                left&=o[0]<0;right&=o[0]>496;above&=o[1]<0;below&=o[1]>384;
                o[0]*=640.0f/496.0f;o[1]*=480.0f/384.0f;
            }
        }
        if(!valid || left || right || above || below) continue;
        ++*volumes;
#define SHADOW_TRI(a,b,c) do { assert(count<capacity); dtri_t *d=out+count++; memset(d,0,sizeof *d); d->strip=s; \
        const float *q_[3]={a,b,c}; for(unsigned v_=0;v_<3;++v_) { d->x[v_]=q_[v_][0]; d->y[v_]=q_[v_][1]; d->z[v_]=q_[v_][2]; } } while(0)
        for(unsigned i=0;i<n;++i) { const unsigned j=(i+1)%n;
            SHADOW_TRI(ring[0][i],ring[0][j],ring[1][i]); SHADOW_TRI(ring[0][j],ring[1][j],ring[1][i]); }
        for(unsigned i=1;i+1<n;++i) SHADOW_TRI(ring[0][0],ring[0][i],ring[0][i+1]);
        for(unsigned i=1;i+1<n;++i) SHADOW_TRI(ring[1][0],ring[1][i+1],ring[1][i]);
#undef SHADOW_TRI
    }
    return count;
}
static void pvr_frame(const dtri_t *tris,unsigned count,unsigned tick) {
    static float *opaque; static uint16_t *layers; static uint32_t *bin_first,*bin_fill,*bins; static unsigned bins_capacity;
    static int (*boxes)[4]; static uint32_t line_tile[PT_TILES],line_mask[PT_TILES];
    if(!opaque) { opaque=malloc(DW*DH*sizeof(float)); layers=malloc(DW*DH*2); bin_first=malloc((PT_TILES+1)*4); bin_fill=malloc(PT_TILES*4); boxes=malloc(STREAM_MAX*sizeof(*boxes)); }
    uint64_t v[V_COUNT]={0},kind[PVR_KINDS][2][K_COUNT]={{{0}}},kop[PVR_KINDS][KO_COUNT]={{0}};
    static int32_t *winner; static uint16_t tile_entries[2][PT_TILES];
    if(!winner) winner=malloc(DW*DH*4);
    for(unsigned i=0;i<DW*DH;++i) { opaque[i]=0; layers[i]=0; winner[i]=-1; }
    memset(tile_entries,0,sizeof tile_entries);
    for(unsigned t=0;t<=PT_TILES;++t) bin_first[t]=0;
    /* Filing by bounding box, entries per strip and tile; opaque depth; punch-through fragments per pixel. */
    static uint32_t tile_strip[PT_TILES],tile_run[PT_TILES];
    for(unsigned t=0;t<PT_TILES;++t) tile_strip[t]=UINT32_MAX;
    uint32_t last_strip[2]={UINT32_MAX,UINT32_MAX};
    for(unsigned i=0;i<count;++i) {
        const dtri_t *t=tris+i; const unsigned pt=t->list==PVR_LIST_PT_POLY;
        if(t->list!=PVR_LIST_OP_POLY && !pt) { boxes[i][0]=1; boxes[i][1]=0; continue; }
        double a,b,c; int box[4];
        ++v[pt ? V_PT_TRIS : V_OP_TRIS]; ++kind[t->kind][pt][K_TRIS];
        if(t->strip!=last_strip[pt]) { last_strip[pt]=t->strip; ++v[pt ? V_PT_STRIPS : V_OP_STRIPS]; ++kind[t->kind][pt][K_STRIPS]; }
        if(!tri_setup(t,&a,&b,&c,box)) { boxes[i][0]=1; boxes[i][1]=0; continue; }
        memcpy(boxes[i],box,sizeof box);
        for(int ty=box[2]/32;ty<=box[3]/32;++ty) for(int tx=box[0]/32;tx<=box[1]/32;++tx) {
            const unsigned tile=(unsigned)ty*PT_W+(unsigned)tx;
            ++v[pt ? V_PT_TRITILES : V_OP_TRITILES]; ++kind[t->kind][pt][K_TRITILES];
            /* One entry holds up to four consecutive triangles of a strip. */
            const uint32_t key=t->strip*2+pt;
            if(tile_strip[tile]!=key || tile_run[tile]==4) { tile_strip[tile]=key; tile_run[tile]=0; ++v[pt ? V_PT_ENTRIES : V_OP_ENTRIES]; ++tile_entries[pt][tile]; }
            ++tile_run[tile];
            if(pt) ++bin_first[tile+1];
        }
        uint64_t covered=0;
        for(int y=box[2];y<=box[3];++y) for(int x=box[0];x<=box[1];++x) {
            if(!tri_covers(t,x+0.5,y+0.5)) continue;
            const unsigned p=(unsigned)y*DW+(unsigned)x;
            if(!pt) { const unsigned tile=(unsigned)(y/32)*PT_W+(unsigned)(x/32);
                if(line_tile[tile]!=i+1) { line_tile[tile]=i+1; line_mask[tile]=0; }
                if(!(line_mask[tile]>>(y&31)&1)) { line_mask[tile]|=1u<<(y&31); ++v[V_OP_LINES]; ++kop[t->kind][KO_LINES]; } }
            ++covered;
            if(pt) ++layers[p];
            else { const float z=(float)(a*(x+0.5)+b*(y+0.5)+c);
                if(z>=opaque[p]) winner[p]=(int32_t)i; /* GEQUAL: the later polygon wins a tie */
                if(z>opaque[p]) opaque[p]=z; }
        }
        if(pt) { v[V_PT_PX]+=covered; kind[t->kind][1][K_PX]+=covered; }
        else { v[V_OP_PX]+=covered; kop[t->kind][KO_PX]+=covered; }
    }
    /* The translucent list, after the opaque one has decided every pixel's surface. */
    { static uint16_t *stack; if(!stack) stack=malloc(DW*DH*2);
      unsigned any=0; uint32_t last=UINT32_MAX;
      for(unsigned i=0;i<count;++i) {
          const dtri_t *t=tris+i;
          if(t->list!=PVR_LIST_TR_POLY) continue;
          if(!any) { memset(stack,0,DW*DH*2); any=1; }
          ++v[V_TR_TRIS]; if(t->strip!=last) { last=t->strip; ++v[V_TR_STRIPS]; }
          double a,b,c; int box[4];
          if(!tri_setup(t,&a,&b,&c,box)) continue;
          for(int ty=box[2]/32;ty<=box[3]/32;++ty) for(int tx=box[0]/32;tx<=box[1]/32;++tx) ++v[V_TR_TRITILES];
          uint32_t row_tile=UINT32_MAX,row_mask=0;
          for(int y=box[2];y<=box[3];++y) for(int x=box[0];x<=box[1];++x) {
              if(!tri_covers(t,x+0.5,y+0.5)) continue;
              const unsigned p=(unsigned)y*DW+(unsigned)x,tile=(unsigned)(y/32)*PT_W+(unsigned)(x/32);
              if(tile!=row_tile) { row_tile=tile; row_mask=0; }
              if(!(row_mask>>(y&31)&1)) { row_mask|=1u<<(y&31); ++v[V_TR_LINES]; }
              ++v[V_TR_PX];
              if((float)(a*(x+0.5)+b*(y+0.5)+c)>=opaque[p]) { ++v[V_TR_FRONT_PX]; if(++stack[p]>v[V_TR_LAYERS_MAX]) v[V_TR_LAYERS_MAX]=stack[p]; }
          }
      }
      v[V_TR_NS]=(uint64_t)(v[V_TR_FRONT_PX]*PVR_TR_SAMPLE_NS+0.5); }
    /* The cars' smoke (the translucent class, make NATIVE_SMOKE_MODE) on whichever list it is: samples rasterised,
     * of them in front of the opaque surface, deepest stack, and the fill-probe price of those front samples:
     * PVR_TR_SAMPLE_NS (9.5) on the pre-sorted translucent list, PVR_PT_SAMPLE_NS (11.9) on the punch-through list
     * (build/dc-native-gpu-split.ScloIy/hw/results.txt, stage 3: both per sample of a 256x256 quad two layers deep;
     * the punch-through probe's texture has a quarter of its texels opaque). The punch-through samples are also in
     * the pt_* counts above. */
    { static uint16_t *stack; if(!stack) stack=malloc(DW*DH*2);
      unsigned any=0; uint64_t front_tr=0,front_pt=0;
      for(unsigned i=0;i<count;++i) {
          const dtri_t *t=tris+i;
          if(t->source || t->extra || t->binding>=mats[0].count || mats[0].bindings[t->binding].resource==NM_MATERIAL_SOLID ||
             !nm_material_translucent(mats[0].bindings[t->binding].header)) continue;
          if(!any) { memset(stack,0,DW*DH*2); any=1; }
          ++v[V_SMOKE_TRIS];
          double a,b,c; int box[4];
          if(!tri_setup(t,&a,&b,&c,box)) continue;
          for(int y=box[2];y<=box[3];++y) for(int x=box[0];x<=box[1];++x) {
              if(!tri_covers(t,x+0.5,y+0.5)) continue;
              const unsigned p=(unsigned)y*DW+(unsigned)x;
              ++v[V_SMOKE_PX];
              if((float)(a*(x+0.5)+b*(y+0.5)+c)>=opaque[p]) {
                  ++v[V_SMOKE_FRONT_PX]; if(t->list==PVR_LIST_TR_POLY) ++front_tr; else ++front_pt;
                  if(++stack[p]>v[V_SMOKE_LAYERS_MAX]) v[V_SMOKE_LAYERS_MAX]=stack[p]; }
          }
      }
      v[V_SMOKE_NS]=(uint64_t)(front_tr*PVR_TR_SAMPLE_NS+front_pt*PVR_PT_SAMPLE_NS+0.5); }
    for(unsigned l=0;l<2;++l) for(unsigned t=0;t<PT_TILES;++t) {
        if(tile_entries[l][t]>v[l ? V_PT_TILE_ENTRIES_MAX : V_OP_TILE_ENTRIES_MAX]) v[l ? V_PT_TILE_ENTRIES_MAX : V_OP_TILE_ENTRIES_MAX]=tile_entries[l][t];
        v[l ? V_PT_TILES_OVER7 : V_OP_TILES_OVER7]+=tile_entries[l][t]>7;
    }
    /* What the TSP textures after the opaque pass: the one visible fragment of every pixel. */
    static uint32_t *seen_in_tile; static unsigned seen_capacity;
    if(count>seen_capacity) { free(seen_in_tile); seen_capacity=count*2; seen_in_tile=malloc(seen_capacity*4); }
    memset(seen_in_tile,0,count*4);
    for(unsigned tile=0;tile<PT_TILES;++tile) for(unsigned y=0;y<32;++y) for(unsigned x=0;x<32;++x) {
        const int32_t w=winner[((tile/PT_W)*32+y)*DW+(tile%PT_W)*32+x];
        if(w>=0 && seen_in_tile[w]!=tile+1) { seen_in_tile[w]=tile+1; ++v[V_OP_VIS_TRITILES]; ++kop[tris[w].kind][KO_VIS_TRITILES]; }
    }
    for(unsigned p=0;p<DW*DH;++p) {
        if(winner[p]<0) { ++v[V_VIS_BG]; continue; }
        const dtri_t *t=tris+winner[p];
        ++kop[t->kind][KO_VISIBLE]; kop[t->kind][KO_BILINEAR]+=t->bilinear;
        ++v[t->extra==2 ? V_VIS_SKYTEX : !t->textured ? V_VIS_FLAT : t->bilinear ? V_VIS_BILINEAR : V_VIS_POINT];
    }
    /* Shadow volumes: every volume triangle is filed and processed like an opaque one; a pixel is inside a
     * volume when an odd number of its faces lie in front of the pixel's opaque surface. */
    if(frame_shadows.count && !sim_no_volumes) {
        static dtri_t *volume; static uint8_t *parity,*inside; static uint8_t mod_tile[PT_TILES];
        enum { VOLUME_TRIS=NM_SHADOW_VOLUMES_MAX*(4*NM_SHADOW_OUTLINE_MAX) };
        if(!volume) { volume=malloc(VOLUME_TRIS*sizeof(dtri_t)); parity=malloc(DW*DH); inside=malloc(DW*DH); }
        unsigned volumes; const unsigned n=shadow_triangles(volume,VOLUME_TRIS,&volumes);
        memset(inside,0,DW*DH); memset(mod_tile,0,sizeof mod_tile);
        v[V_MOD_VOLUMES]=volumes; v[V_MOD_TRIS]=n; frame_extra_ta+=64ull*n+64ull*volumes;
        for(unsigned first=0;first<n;) {
            unsigned end=first; while(end<n && volume[end].strip==volume[first].strip) ++end;
            int reach[4]={DW,-1,DH,-1};
            for(unsigned i=first;i<end;++i) {
                const dtri_t *t=volume+i; double a,b,c; int box[4];
                if(!tri_setup(t,&a,&b,&c,box)) continue;
                for(int ty=box[2]/32;ty<=box[3]/32;++ty) for(int tx=box[0]/32;tx<=box[1]/32;++tx) { ++v[V_MOD_TRITILES]; mod_tile[ty*PT_W+tx]=1; }
                if(reach[1]<reach[0]) { memcpy(reach,box,sizeof reach); for(int y=box[2];y<=box[3];++y) memset(parity+y*DW+box[0],0,(size_t)(box[1]-box[0]+1)); }
                else { /* grow the cleared rectangle */
                    const int nx0=box[0]<reach[0] ? box[0] : reach[0],nx1=box[1]>reach[1] ? box[1] : reach[1],ny0=box[2]<reach[2] ? box[2] : reach[2],ny1=box[3]>reach[3] ? box[3] : reach[3];
                    for(int y=ny0;y<=ny1;++y) for(int x=nx0;x<=nx1;++x) if(y<reach[2] || y>reach[3] || x<reach[0] || x>reach[1]) parity[y*DW+x]=0;
                    reach[0]=nx0; reach[1]=nx1; reach[2]=ny0; reach[3]=ny1;
                }
                uint32_t rows_tile=UINT32_MAX,rows_mask=0; /* lines per tile, tiles visited row by row */
                for(int ty=box[2]/32;ty<=box[3]/32;++ty) for(int tx=box[0]/32;tx<=box[1]/32;++tx) {
                    rows_tile=(uint32_t)(ty*PT_W+tx); rows_mask=0;
                    const int y0=ty*32>box[2] ? ty*32 : box[2],y1=ty*32+31<box[3] ? ty*32+31 : box[3],x0=tx*32>box[0] ? tx*32 : box[0],x1=tx*32+31<box[1] ? tx*32+31 : box[1];
                    for(int y=y0;y<=y1;++y) for(int x=x0;x<=x1;++x) {
                        if(!tri_covers(t,x+0.5,y+0.5)) continue;
                        ++v[V_MOD_PX];
                        if(!(rows_mask>>(y&31)&1)) { rows_mask|=1u<<(y&31); ++v[V_MOD_LINES]; }
                        if((float)(a*(x+0.5)+b*(y+0.5)+c)>opaque[y*DW+x]) parity[y*DW+x]^=1;
                    }
                }
                (void)rows_tile;
            }
            if(reach[1]>=reach[0]) for(int y=reach[2];y<=reach[3];++y) for(int x=reach[0];x<=reach[1];++x) if(parity[y*DW+x]) inside[y*DW+x]=1;
            first=end;
        }
        for(unsigned t=0;t<PT_TILES;++t) v[V_MOD_TILES]+=mod_tile[t];
        for(unsigned p=0;p<DW*DH;++p) if(inside[p] && winner[p]>=0) {
            ++v[V_MOD_INSIDE];
            if(!sim_no_receivers && tris[winner[p]].surface) ++v[V_MOD_SHADOWED];
        }
    }
    v[V_EXTRA_TA]=frame_extra_ta;
    for(unsigned t=0;t<PT_TILES;++t) { bin_first[t+1]+=bin_first[t]; bin_fill[t]=bin_first[t]; }
    if(bin_first[PT_TILES]>bins_capacity) { bins_capacity=bin_first[PT_TILES]*2; free(bins); bins=malloc(bins_capacity*4); }
    for(unsigned i=0;i<count;++i) {
        if(tris[i].list!=PVR_LIST_PT_POLY || boxes[i][0]>boxes[i][1]) continue;
        for(int ty=boxes[i][2]/32;ty<=boxes[i][3]/32;++ty) for(int tx=boxes[i][0]/32;tx<=boxes[i][1]/32;++tx) bins[bin_fill[(unsigned)ty*PT_W+(unsigned)tx]++]=i;
    }
    uint64_t layer_sum=0;
    for(unsigned tile=0;tile<PT_TILES;++tile) {
        const unsigned filed=bin_first[tile+1]-bin_first[tile];
        if(!filed) continue;
        ++v[V_PT_TILES];
        const unsigned x0=(tile%PT_W)*32,y0=(tile/PT_W)*32;
        unsigned deepest=0; float drawn[1024];
        for(unsigned y=0;y<32;++y) for(unsigned x=0;x<32;++x) {
            const unsigned p=(y0+y)*DW+x0+x; drawn[y*32+x]=opaque[p]; if(layers[p]>deepest) deepest=layers[p]; }
        layer_sum+=deepest; if(deepest>v[V_PT_LAYERS_MAX]) v[V_PT_LAYERS_MAX]=deepest;
        unsigned tile_passes=0;
        for(unsigned first=0;first<filed;first+=pvr_chunk) {
            const unsigned n=filed-first<pvr_chunk ? filed-first : pvr_chunk;
            ++v[V_PT_CHUNKS];
            static struct { uint32_t rows[32]; double z[3],u[3],v[3]; const dtri_t *t; const pvr_alpha_t *alpha; int live,mirror,door; } set[256];
            assert(n<=256);
            for(unsigned k=0;k<n;++k) {
                const dtri_t *t=tris+bins[bin_first[tile]+first+k];
                const double zq[3]={t->z[0],t->z[1],t->z[2]},uq[3]={t->u[0]*zq[0],t->u[1]*zq[1],t->u[2]*zq[2]},vq[3]={t->v[0]*zq[0],t->v[1]*zq[1],t->v[2]*zq[2]};
                pvr_plane(t,zq,set[k].z); pvr_plane(t,uq,set[k].u); pvr_plane(t,vq,set[k].v);
                set[k].t=t; set[k].live=1;
                if(t->extra) { set[k].alpha=t->extra==4 ? &probe_alpha : hud_sheets+t->sheet; set[k].mirror=0; }
                else {
                    const nm_material_binding_t *binding=mats[t->source].bindings+t->binding;
                    set[k].alpha=binding->resource==NM_MATERIAL_SOLID ? NULL : pvr_texture(t->source,binding->resource);
                    set[k].mirror=(binding->header[0]>>8)&3;
                }
                /* pt-screen smoke (NM_SMOKE_MODE 2): u reads a texture twice as wide whose right half is holes. */
                set[k].door=NM_SMOKE_MODE && !t->source && !t->extra && set[k].alpha &&
                    nm_material_translucent(mats[0].bindings[t->binding].header);
                for(unsigned y=0;y<32;++y) { uint32_t row=0;
                    for(unsigned x=0;x<32;++x) if(tri_covers(t,x0+x+0.5,y0+y+0.5)) row|=1u<<x;
                    set[k].rows[y]=row; }
                if(set[k].door && NM_SMOKE_MODE==2 && sim_smoke_bound) for(unsigned y=0;y<32;++y) for(uint32_t row=set[k].rows[y];row;row&=row-1) {
                    /* SIM_SMOKE_BOUND (experiment): 1 drops the fragments whose shape texel is a hole (the bound of tight
                     * geometry), 2 also those of the checker's transparent pixels (unreachable: no geometry skips them). */
                    const unsigned x=(unsigned)__builtin_ctz(row); const double px=x0+x+0.5,py=y0+y+0.5;
                    const double w=set[k].z[0]*px+set[k].z[1]*py+set[k].z[2];
                    const double tu=(set[k].u[0]*px+set[k].u[1]*py+set[k].u[2])/w,tv=(set[k].v[0]*px+set[k].v[1]*py+set[k].v[2])/w;
                    const unsigned w2=2*set[k].alpha->w,du=(unsigned)((long long)floor(tu*w2)&(long long)(w2-1));
                    const int hole=!set[k].alpha->opaque[pvr_wrap(tv,set[k].alpha->h,0)*set[k].alpha->w+du%set[k].alpha->w];
                    if(hole || (sim_smoke_bound==2 && du>=set[k].alpha->w)) set[k].rows[y]&=~(1u<<x);
                }
            }
            float peel[1024]; int16_t best[1024]; float best_z[1024];
            for(unsigned p=0;p<1024;++p) peel[p]=FLT_MAX;
            for(;;) {
                unsigned live=0,found=0;
                for(unsigned p=0;p<1024;++p) best[p]=-1;
                for(unsigned k=0;k<n;++k) {
                    if(!set[k].live) continue;
                    ++live; int any=0; ++kind[set[k].t->kind][1][K_TRIPASSES];
                    for(unsigned y=0;y<32;++y) v[V_PT_LINEPASSES]+=set[k].rows[y]!=0;
                    for(unsigned y=0;y<32;++y) for(uint32_t row=set[k].rows[y];row;row&=row-1) {
                        const unsigned x=(unsigned)__builtin_ctz(row),p=y*32+x;
                        const float z=(float)(set[k].z[0]*(x0+x+0.5)+set[k].z[1]*(y0+y+0.5)+set[k].z[2]);
                        if(!(z>=drawn[p]) || !(z<peel[p])) continue;
                        any=1;
                        if(best[p]<0 || z>best_z[p]) { best[p]=(int16_t)k; best_z[p]=z; }
                    }
                    if(!any) set[k].live=0;
                }
                if(!live) break;
                ++v[V_PT_PASSES]; ++tile_passes; v[V_PT_TRIPASSES]+=live;
                for(unsigned p=0;p<1024;++p) {
                    if(best[p]<0) continue;
                    const unsigned k=(unsigned)best[p]; const double px=x0+p%32+0.5,py=y0+p/32+0.5;
                    int solid=1;
                    if(set[k].alpha) {
                        const double w=set[k].z[0]*px+set[k].z[1]*py+set[k].z[2];
                        const double tu=(set[k].u[0]*px+set[k].u[1]*py+set[k].u[2])/w,tv=(set[k].v[0]*px+set[k].v[1]*py+set[k].v[2])/w;
                        if(set[k].door && NM_SMOKE_MODE==1) { /* pt-tex: texels with u + v odd are holes too */
                            const unsigned tu1=pvr_wrap(tu,set[k].alpha->w,0),tv1=pvr_wrap(tv,set[k].alpha->h,0);
                            solid=!((tu1+tv1)&1) && set[k].alpha->opaque[tv1*set[k].alpha->w+tu1];
                        }
                        else if(set[k].door) {
                            const unsigned w2=2*set[k].alpha->w,du=(unsigned)((long long)floor(tu*w2)&(long long)(w2-1));
                            solid=du<set[k].alpha->w && set[k].alpha->opaque[pvr_wrap(tv,set[k].alpha->h,0)*set[k].alpha->w+du];
                        }
                        else solid=set[k].alpha->opaque[pvr_wrap(tv,set[k].alpha->h,set[k].mirror&2)*set[k].alpha->w+pvr_wrap(tu,set[k].alpha->w,set[k].mirror&1)];
                    }
                    ++v[V_PT_TSP]; ++kind[set[k].t->kind][1][K_TSP]; ++found;
                    if(solid) { drawn[p]=best_z[p]; peel[p]=-FLT_MAX; }
                    else { peel[p]=best_z[p]; ++v[V_PT_TSP_CLEAR]; ++kind[set[k].t->kind][1][K_CLEAR]; }
                }
                (void)found;
            }
        }
        if(tile_passes>v[V_PT_PASS_MAX]) v[V_PT_PASS_MAX]=tile_passes;
    }
    v[V_PT_LAYERS_MEAN100]=v[V_PT_TILES] ? layer_sum*100/v[V_PT_TILES] : 0;
    fprintf(pvr_out,"P tick=%u",tick);
    for(unsigned k=0;k<V_COUNT;++k) { fprintf(pvr_out," %s=%llu",pvr_names[k],(unsigned long long)v[k]);
        pvr_total[k]+=v[k]; if(v[k]>pvr_max[k]) pvr_max[k]=v[k]; }
    for(unsigned c=0;c<pvr_kinds;++c) {
        fprintf(pvr_out," %s:op=%llu,%llu,%llu",pvr_kind_names[c],(unsigned long long)kind[c][0][K_TRIS],(unsigned long long)kind[c][0][K_TRITILES],(unsigned long long)kind[c][0][K_STRIPS]);
        for(unsigned k=0;k<KO_COUNT;++k) { fprintf(pvr_out,",%llu",(unsigned long long)kop[c][k]); pvr_kind_op_total[c][k]+=kop[c][k]; }
        fputs(":pt=",pvr_out);
        for(unsigned k=0;k<K_COUNT;++k) { fprintf(pvr_out,"%s%llu",k ? "," : "",(unsigned long long)kind[c][1][k]); }
        for(unsigned l=0;l<2;++l) for(unsigned k=0;k<K_COUNT;++k) pvr_kind_total[c][l][k]+=kind[c][l][k];
    }
    fputc('\n',pvr_out);
    const uint64_t rank[2]={v[V_PT_TRIPASSES],v[V_PT_TSP]};
    for(unsigned r=0;r<2;++r) {
        unsigned at=6; while(at && rank[r]>pvr_worst[r][at-1].value) --at;
        if(at<6) { for(unsigned k=5;k>at;--k) pvr_worst[r][k]=pvr_worst[r][k-1]; pvr_worst[r][at].value=rank[r]; pvr_worst[r][at].tick=tick; }
    }
    ++pvr_frames;
}
/* ---- Colour picture (SIM_COLOR) ------------------------------------------
 * The submitted triangles drawn by a software rasteriser: opaque list, then
 * punch-through list with the alpha test, one depth buffer (written depth,
 * GEQUAL, later wins a tie as registration order does on the opaque list).
 * Texel: point sampled from the base level with perspective-correct UVs per
 * triangle (as the PVR interpolates: u*w, v*w and w linear on the screen),
 * colour = palette entry x vertex colour / 255; a punch-through binding's
 * entry 15 is the hole. Bilinear filtering of opaque polygons, the sky, the
 * HUD and shadows are not drawn (background 0x5080c0). For comparing asset
 * sets, not a picture of the console's output. */
/* SIM_COLOR_FILTER=bilinear (smoke only): a bilinear sample of a PAL4 texture at texel coordinates (s, t) (texel centres
 * at +1/2), u period `wu` texels of which the first `shape` are the texture (the rest holes: pt-screen / tr-screen's
 * transparent half), v period the texture's height. rgb: the four palette colours weighted (as the PVR filters a
 * palette texture: entry 15's colour enters with its weight), alpha: the weight of the non-hole texels. arcade=1: as
 * src/runtime/raster.cpp (fetch_bilinear_texel<true>): a hole takes its neighbour's colour (u first, then v), so no
 * hole colour enters. */
static void smoke_bilinear(const pvr_alpha_t *tex,const uint32_t *palette,double s,double t,unsigned wu,unsigned shape,int arcade,
        double rgb[3],double *alpha) {
    s-=0.5; t-=0.5;
    const long si=(long)floor(s),ti=(long)floor(t); const double fs=s-si,ft=t-ti;
    int hole[2][2]; uint32_t col[2][2];
    for(unsigned j=0;j<2;++j) for(unsigned i=0;i<2;++i) {
        const unsigned u=(unsigned)((((si+(long)i)%(long)wu)+(long)wu)%(long)wu),v=(unsigned)((((ti+(long)j)%(long)tex->h)+(long)tex->h)%(long)tex->h);
        const unsigned index=u<shape ? tex->index[v*tex->w+u%tex->w] : 15u;
        hole[j][i]=index==15; col[j][i]=palette[index];
    }
    if(arcade) for(unsigned j=0;j<2;++j) {
        if(hole[j][0] && !hole[j][1]) col[j][0]=col[j][1];
        if(hole[j][1] && !hole[j][0]) col[j][1]=col[j][0];
    }
    double row[2][3],cov[2];
    for(unsigned j=0;j<2;++j) {
        for(unsigned k=0;k<3;++k) row[j][k]=((col[j][0]>>(16-8*k))&255u)*(1-fs)+((col[j][1]>>(16-8*k))&255u)*fs;
        cov[j]=(!hole[j][0])*(1-fs)+(!hole[j][1])*fs;
    }
    if(arcade) { /* a row without coverage takes the other row's colour */
        if(cov[0]==0 && cov[1]>0) memcpy(row[0],row[1],sizeof row[0]);
        if(cov[1]==0 && cov[0]>0) memcpy(row[1],row[0],sizeof row[1]);
    }
    for(unsigned k=0;k<3;++k) rgb[k]=row[0][k]*(1-ft)+row[1][k]*ft;
    *alpha=cov[0]*(1-ft)+cov[1]*ft;
}
static void colour_frame(const dtri_t *tris,unsigned count,unsigned tick,const char *folder) {
    static float *depth; static uint8_t *rgb;
    if(!depth) { depth=malloc(DW*DH*sizeof(float)); rgb=malloc(DW*DH*3); }
    for(unsigned i=0;i<DW*DH;++i) { depth[i]=0; rgb[3*i]=0x50; rgb[3*i+1]=0x80; rgb[3*i+2]=0xc0; }
    /* SIM_COLOR_MARK=WORD: world faces with this texture word (header word 2) in magenta (punch-through) or white (opaque). */
    const unsigned mark=getenv("SIM_COLOR_MARK") ? (unsigned)atoi(getenv("SIM_COLOR_MARK")) : 0;
    /* SIM_COLOR_MIP=1: the catch-fence stipple (draw_binding_stipple) sampled from the mip level the PVR would pick, closer to
     * the console than the base level: level floor(log2(texels per pixel)), texels per pixel the larger of the two screen-axis
     * gradients of u, v (D adjust 1.0), point sampled, down to the 2x2 level. A preview: the PVR's own D is not modelled to the bit. */
    const int mip=getenv("SIM_COLOR_MIP")!=NULL;
    /* The cars' smoke (the translucent class) as each NATIVE_SMOKE_MODE draws it: tr on the translucent list after
     * the punch-through one, in submission order, blended at alpha 0x80/255 (texel 15 a hole), no depth write;
     * pt-tex on the punch-through list, texels with u + v odd holes too; pt-screen on the punch-through list, u read
     * across a texture twice as wide whose right half is holes. SIM_COLOR_SMOKE=arcade: the arcade's screen door at
     * this resolution instead (a tr stream: back to front): every other pixel ((x + y) odd) written opaque, the
     * later (nearer) polygon over the earlier, texel 15 a hole, no depth write. Point sampled, as the rest. */
    const int arcade=getenv("SIM_COLOR_SMOKE") && !strcmp(getenv("SIM_COLOR_SMOKE"),"arcade");
    const int bilinear=getenv("SIM_COLOR_FILTER") && !strcmp(getenv("SIM_COLOR_FILTER"),"bilinear");
    static const unsigned pass_list[3]={PVR_LIST_OP_POLY,PVR_LIST_PT_POLY,PVR_LIST_TR_POLY};
    for(unsigned pass=0;pass<3;++pass) for(unsigned i=0;i<count;++i) {
        const dtri_t *t=tris+i;
        if(t->list!=pass_list[pass]) continue;
        const int smoke=!t->source && mats[0].bindings[t->binding].resource!=NM_MATERIAL_SOLID &&
            nm_material_translucent(mats[0].bindings[t->binding].header);
        const int door=smoke && arcade && t->list==PVR_LIST_TR_POLY;
        double a,b,c; int box[4];
        if(!tri_setup(t,&a,&b,&c,box)) continue;
        const nm_material_binding_t *binding=mats[t->source].bindings+t->binding;
        const pvr_alpha_t *texture=binding->resource==NM_MATERIAL_SOLID ? NULL : pvr_texture(t->source,binding->resource);
        const int palette=nm_material_palette(mats+t->source,binding),mirror=(binding->header[0]>>8)&3;
        const double zq[3]={t->z[0],t->z[1],t->z[2]},uq[3]={t->u[0]*zq[0],t->u[1]*zq[1],t->u[2]*zq[2]},vq[3]={t->v[0]*zq[0],t->v[1]*zq[1],t->v[2]*zq[2]};
        double pz[3],pu[3],pv[3]; pvr_plane(t,zq,pz); pvr_plane(t,uq,pu); pvr_plane(t,vq,pv);
        for(int y=box[2];y<=box[3];++y) for(int x=box[0];x<=box[1];++x) {
            const double px=x+0.5,py=y+0.5;
            if(!tri_covers(t,px,py)) continue;
            if(door && !((x+y)&1)) continue;
            const unsigned p=(unsigned)y*DW+(unsigned)x;
            const float z=(float)(a*px+b*py+c);
            if(!(z>=depth[p])) continue;
            if(smoke && bilinear && texture && palette>=0) {
                /* SIM_COLOR_FILTER=bilinear: the smoke filtered (smoke_bilinear). arcade: coverage under 1/2 is a hole;
                 * pt-screen: alpha at PT_ALPHA_REF 0x80 or more is drawn; tr-screen: blended by alpha; tr: alpha x 0x80. */
                const double w=pz[0]*px+pz[1]*py+pz[2];
                const double u=(pu[0]*px+pu[1]*py+pu[2])/w,v=(pv[0]*px+pv[1]*py+pv[2])/w;
                const int screen=NM_SMOKE_DOOR && !door;
                double rgbf[3],alpha;
                smoke_bilinear(texture,mats[t->source].palettes+16*palette,u*(screen ? 2.0*texture->w : texture->w),v*texture->h,
                    screen ? 2u*texture->w : texture->w,texture->w,door,rgbf,&alpha);
                double blend;
                if(door || t->list==PVR_LIST_PT_POLY) { if(alpha*255.0<(door ? 127.5 : 128.0)) continue; blend=1.0; }
                else blend=NM_SMOKE_MODE==3 ? alpha : alpha*(double)(t->argb>>24)/255.0;
                if(blend<=0) continue;
                for(unsigned k=0;k<3;++k) {
                    const double src=rgbf[k]*((t->argb>>(16-8*k))&255u)/255.0;
                    rgb[3*p+k]=(uint8_t)fmin(255.0,floor(src*blend+rgb[3*p+k]*(1.0-blend)+0.5));
                }
                if(t->list==PVR_LIST_PT_POLY) depth[p]=z;
                continue;
            }
            uint32_t colour=0xffffffffu;
            if(texture && palette>=0) {
                const double w=pz[0]*px+pz[1]*py+pz[2];
                const double u=(pu[0]*px+pu[1]*py+pu[2])/w,v=(pv[0]*px+pv[1]*py+pv[2])/w;
                unsigned tu=pvr_wrap(u,texture->w,mirror&1);
                const unsigned tv=pvr_wrap(v,texture->h,mirror&2);
                if(smoke && NM_SMOKE_DOOR && !door) {
                    tu=(unsigned)((long long)floor(u*2.0*texture->w)&(long long)(2*texture->w-1));
                    if(tu>=texture->w) continue;                    /* the transparent half */
                }
                if(smoke && t->list==PVR_LIST_PT_POLY && NM_SMOKE_MODE==1 && ((tu+tv)&1)) continue;
                unsigned index=texture->index[tv*texture->w+tu];
                if(mip && t->source && ((binding->header[0]>>13)&7u)==3u && (binding->header[1]&0xfcffu)==0x8000u) {
                    const double ux=(pu[0]-u*pz[0])/w*texture->w,uy=(pu[1]-u*pz[1])/w*texture->w;
                    const double vx=(pv[0]-v*pz[0])/w*texture->h,vy=(pv[1]-v*pz[1])/w*texture->h;
                    const double rho=fmax(hypot(ux,vx),hypot(uy,vy));
                    unsigned level=rho>1 ? (unsigned)floor(log2(rho)) : 0u;
                    const uint8_t *rows=NULL;
                    while(level && !(rows=pvr_texture_level(t->source,binding->resource,level))) --level;
                    if(level && rows) { const unsigned side=texture->w>>level; index=rows[pvr_wrap(v,side,mirror&2)*side+pvr_wrap(u,side,mirror&1)]; }
                }
                if(pass && index==15) continue;
                colour=mats[t->source].palettes[16*palette+index];
            }
            if(t->list==PVR_LIST_TR_POLY && !door && !(smoke && NM_SMOKE_MODE==3)) {
                for(unsigned k=0;k<3;++k) {
                    const unsigned src=((colour>>(16-8*k))&255u)*((t->argb>>(16-8*k))&255u)/255u,alpha=t->argb>>24;
                    rgb[3*p+k]=(uint8_t)((src*alpha+rgb[3*p+k]*(255u-alpha)+127u)/255u);
                }
                continue;
            }
            if(!door && t->list!=PVR_LIST_TR_POLY) depth[p]=z;   /* tr-screen (point): the texel, no depth write */
            for(unsigned k=0;k<3;++k) rgb[3*p+k]=(uint8_t)(((colour>>(16-8*k))&255u)*((t->argb>>(16-8*k))&255u)/255u);
            if(mark && t->source && packs[1].materials[packs[1].faces[t->face].material].header[2]==mark) { rgb[3*p]=255; rgb[3*p+1]=(uint8_t)(pass ? 0 : 255); rgb[3*p+2]=255; }
        }
    }
    char name[512]; snprintf(name,sizeof name,"%s/%u.ppm",folder,tick);
    FILE *out=fopen(name,"wb"); assert(out);
    fprintf(out,"P6\n%d %d\n255\n",DW,DH); fwrite(rgb,3,DW*DH,out); fclose(out);
}
static void pvr_summary(void) {
    printf("PVR work model (%ld frames, punch-through chunk %u triangles): per frame mean / max\n",pvr_frames,pvr_chunk);
    for(unsigned k=0;k<V_COUNT;++k) printf("  pvr %-18s %10.1f %8llu\n",pvr_names[k],(double)pvr_total[k]/pvr_frames,(unsigned long long)pvr_max[k]);
    printf("  pvr by class, per frame mean: opaque list tris tritiles strips");
    for(unsigned k=0;k<KO_COUNT;++k) printf(" %s",pvr_kind_op_fields[k]);
    fputs(" | punch-through",stdout);
    for(unsigned k=0;k<K_COUNT;++k) printf(" %s",pvr_kind_fields[k]);
    putchar('\n');
    for(unsigned c=0;c<pvr_kinds;++c) {
        printf("  pvr class %-10s %8.1f %8.1f %8.1f",pvr_kind_names[c],(double)pvr_kind_total[c][0][K_TRIS]/pvr_frames,(double)pvr_kind_total[c][0][K_TRITILES]/pvr_frames,(double)pvr_kind_total[c][0][K_STRIPS]/pvr_frames);
        for(unsigned k=0;k<KO_COUNT;++k) printf(" %9.1f",(double)pvr_kind_op_total[c][k]/pvr_frames);
        fputs(" |",stdout);
        for(unsigned k=0;k<K_COUNT;++k) printf(" %9.1f",(double)pvr_kind_total[c][1][k]/pvr_frames);
        putchar('\n');
    }
    for(unsigned r=0;r<2;++r) { printf("  pvr worst frames by %s:",r ? "pt_tsp" : "pt_tripasses");
        for(unsigned k=0;k<6 && pvr_worst[r][k].value;++k) printf(" tick %u (%llu)",pvr_worst[r][k].tick,(unsigned long long)pvr_worst[r][k].value);
        putchar('\n'); }
}

/* SIM_ROAD_DEPTH, SIM_ROAD_DEPTH_STEPS and SIM_ROAD_DEPTH_CYCLE (see the top), and what the run did with them. */
static const float road_presets[5]={1.05f/1.65f,1.10f/1.70f,1.85f/2.45f,3.74f/4.34f,NM_ROAD_DEPTH_SCALE}; /* h/(h+0.6) of VR1..VR4, others */
static float road_fixed,road_step_scale[64],road_low=FLT_MAX,road_high;
static long road_step_frame[64],road_cycle;
static unsigned road_steps,road_changes,road_restarts,road_rejoined;
static unsigned long long road_restart_records,road_checked_frames,road_checked_meshes,road_checked_faces;
static int road_check;
static uint8_t *road_restarted; /* per mesh: a change came in the middle of its build (record cache) */
static uint8_t *road_scratch;   /* a build of one mesh: its block, then the builder's seen and in_lod */
static size_t road_scratch_block;
static unsigned road_scratch_largest;
static void road_depth_setup(void) {
    const char *fixed=getenv("SIM_ROAD_DEPTH"),*steps=getenv("SIM_ROAD_DEPTH_STEPS"),*cycle=getenv("SIM_ROAD_DEPTH_CYCLE");
    road_check=getenv("SIM_ROAD_DEPTH_CHECK")!=NULL;
#ifdef NM_ROAD_DEPTH_FIXED
    assert(!fixed && !steps && !cycle); /* make NATIVE_DIAG_ROAD_DEPTH: one scale for every camera */
#endif
    if(fixed) { road_fixed=strtof(fixed,NULL); assert(road_fixed>0.0f && road_fixed<1.0f); sq_road_depth=sim_road_depth=road_fixed; }
    if(cycle) { road_cycle=atol(cycle); assert(road_cycle>0); }
    for(const char *at=steps;at && *at;) {
        char *end; assert(road_steps<64);
        road_step_frame[road_steps]=strtol(at,&end,10); assert(*end==':');
        road_step_scale[road_steps++]=strtof(end+1,&end); assert(!*end || *end==',');
        at=*end ? end+1 : end;
    }
}
/* The frame's scale: set as the scene sets it. A change in the middle of a mesh's build (record cache) is noted, with
 * the records of road faces the build has written: sq_road_depth_set must start that mesh again. */
static void road_depth_frame(float table,long frame) {
#ifndef NM_ROAD_DEPTH_FIXED
    float scale=table>0.0f ? table : NM_ROAD_DEPTH_SCALE;
    for(unsigned i=0;i<road_steps && frame>=road_step_frame[i];++i) scale=road_step_scale[i];
    if(road_cycle) scale=road_presets[(unsigned long)(frame/road_cycle)%5u];
    if(road_fixed>0.0f) scale=road_fixed;
    assert(scale>0.0f && scale<1.0f);
    if(scale<road_low) road_low=scale;
    if(scale>road_high) road_high=scale;
    if(scale!=sim_road_depth) {
        ++road_changes;
#if defined(SIM_HAS_STREAM) && SQ_STREAMS
        if(sq_stream_pool_bytes && sq_stream_job.mesh!=UINT_MAX && sq_stream_job.phase) {
            const nm_mesh_t *const mesh=packs[1].meshes+sq_stream_job.mesh;
            const unsigned written=sq_stream_job.phase>1 ? mesh->face_count : sq_stream_job.cursor;
            unsigned road=0;
            for(unsigned f=0;f<written;++f) road+=world_draw.surface[sq_baked_faces[mesh->face_first+f].front];
            if(road) { ++road_restarts; road_restart_records+=road; road_restarted[sq_stream_job.mesh]=1; }
        }
#endif
    }
    sim_road_depth=scale;
    sq_road_depth_set(scale);
#else
    (void)table; (void)frame; road_low=road_high=sim_road_depth;
#endif
}
/* SIM_ROAD_DEPTH_CHECK: the emission classes against the frame's scale, then each mesh on the stream path against a
 * build of its block at this moment (sq_stream_job_step into scratch: what a world attached at this scale holds, by
 * the classes just checked). Every field of every face record, the strip vertices, positions and ranks. A build
 * leaves the mesh's table entry at its block, so the entry is put back; what else it writes is the same each time. */
#if defined(SIM_HAS_STREAM) && SQ_STREAMS
static int road_record_same(const nm_sq_stream_face_t *a,const nm_sq_stream_face_t *b) {
    return a->group==b->group && a->count==b->count && a->flags==b->flags && a->argb==b->argb && a->header==b->header &&
        !memcmp(&a->depth,&b->depth,sizeof a->depth) && !memcmp(a->corner,b->corner,sizeof a->corner) && a->run==b->run &&
        a->front==b->front && a->strip==b->strip && !memcmp(a->uv,b->uv,sizeof a->uv) && !memcmp(&a->side,&b->side,sizeof a->side) &&
        !memcmp(a->normal,b->normal,sizeof a->normal) && a->back==b->back && a->index==b->index;
}
#endif
static int road_records_check(unsigned tick) {
    if(sq_emit_records[1]) for(unsigned b=0;b<mats[1].count;++b) {
        const float want=world_draw.surface[b] ? sim_road_depth : 1.0f;
        if(sq_emit_records[1][b].group->depth!=want) {
            printf("road depth check: tick %u binding %u: class depth %.9g, the frame's %.9g\n",tick,b,(double)sq_emit_records[1][b].group->depth,(double)want);
            return 0;
        }
    }
#if defined(SIM_HAS_STREAM) && SQ_STREAMS
    const nm_pack_t *const pack=packs+1;
    const unsigned meshes=((const nm_header_t *)pack->data)->meshes;
    if(sq_stream_faces && !road_scratch) {
        for(unsigned m=0;m<meshes;++m) {
            if(sq_stream_plans[m].bytes>road_scratch_block) road_scratch_block=sq_stream_plans[m].bytes;
            if(pack->meshes[m].vertex_count>road_scratch_largest) road_scratch_largest=pack->meshes[m].vertex_count;
            if(pack->meshes[m].face_count>road_scratch_largest) road_scratch_largest=pack->meshes[m].face_count;
        }
        road_scratch=aligned_alloc(32,((road_scratch_block+6u*road_scratch_largest+4u)+31u)&~(size_t)31u); /* sq_stream_prepare's scratch */
        assert(road_scratch);
    }
    if(sq_stream_faces) for(unsigned m=0;m<meshes;++m) {
        if(!sq_stream_meshes[m]) continue;
        const nm_mesh_t *const mesh=pack->meshes+m;
        const nm_sq_stream_plan_t plan=sq_stream_plans[m];
        const nm_sq_stream_mesh_t kept=sq_stream_table[m];
        uint8_t *const seen=road_scratch+road_scratch_block,*const in_lod=seen+road_scratch_largest;
        memset(seen,0,road_scratch_largest);
        nm_sq_stream_job_t job;
        sq_stream_job_start(&job,m,road_scratch);
        while(!sq_stream_job_step(pack,&job,seen,in_lod,UINT_MAX)) {}
        const nm_sq_stream_mesh_t built=sq_stream_table[m];
        sq_stream_table[m]=kept;
        for(unsigned r=0;r<mesh->face_count;++r) if(!road_record_same(kept.faces+r,built.faces+r)) {
            printf("road depth check: tick %u mesh %u record %u (face %u, binding %u): depth %.9g, a build at scale %.9g holds %.9g\n",tick,m,r,
                (unsigned)kept.faces[r].index,(unsigned)kept.faces[r].front,(double)kept.faces[r].depth,(double)sim_road_depth,(double)built.faces[r].depth);
            return 0;
        }
        int same=!memcmp(kept.points,built.points,plan.points*sizeof(*kept.points)) && !memcmp(kept.rank,built.rank,mesh->face_count*sizeof(*kept.rank));
        for(unsigned i=0;plan.strips && i<mesh->padding[3];++i) { /* a stage's selection leaves gaps between the strips of the span */
            const nm_strip_t *const strip=pack->strips+mesh->padding[2]+i;
            unsigned vertices=2;
            for(unsigned k=0;k<strip->face_count;++k) vertices+=pack->faces[strip->face_first+k].count-2u;
            same=same && !memcmp(kept.strips+(strip->vertex_first-plan.strip_first),built.strips+(strip->vertex_first-plan.strip_first),vertices*sizeof(*kept.strips));
        }
        if(!same) { printf("road depth check: tick %u mesh %u: its strip vertices, positions or ranks differ from a build\n",tick,m); return 0; }
        ++road_checked_meshes; road_checked_faces+=mesh->face_count;
        if(road_restarted[m]) { road_restarted[m]=0; ++road_rejoined; }
    }
#endif
    ++road_checked_frames;
    return 1;
}
/* A frame table's scale against the rule (render/models/model_renderer.h): attract, podium and the keyed race cameras
 * NM_ROAD_DEPTH_SCALE; a race view that, or a preset's h/(h+0.6) (FLT_EPSILON: the console's division may round it). */
static int road_table_legal(float scale,unsigned attract,unsigned camera_mode) {
#ifdef NM_ROAD_DEPTH_FIXED
    return 1; /* this build's one scale is not the table's */
#endif
    if(scale==NM_ROAD_DEPTH_SCALE) return 1;
    if(attract || camera_mode==1u) return 0;
    for(unsigned k=0;k<4;++k) if(fabsf(scale-road_presets[k])<=FLT_EPSILON) return 1;
    return 0;
}
static void road_depth_summary(long frames) {
    printf("road depth: %ld frames, scales %.9g .. %.9g, %u changes",frames,(double)road_low,(double)road_high,road_changes);
    printf("; %u came in a mesh's build (%llu road records already written), %u of those meshes since on the path",road_restarts,road_restart_records,road_rejoined);
    if(road_check) printf("; checked %llu frames: classes hold the frame's scale, %llu meshes, %llu face records equal to a build at it",
        road_checked_frames,road_checked_meshes,road_checked_faces);
    putchar('\n');
}
enum { S_HEADERS,S_VERTICES,S_STRIPS,S_JOINED,S_TRIANGLES,S_FLOOR,S_OBJECTS,S_WALKED,S_QUEUED,S_TRANSFORMED,S_REAR,S_TEXTURES,S_PALETTES,S_TA,S_TR_TRIANGLES,S_COUNT };
static const char *stat_names[S_COUNT]={"headers","vertices","strips","offline-strip-joins","triangles","header-floor","objects",
    "faces-walked","faces-queued","vertices-transformed","faces-rear","textures","palette-banks","TA-bytes","translucent-triangles"};

int main(int argc,char **argv) {
    if(argc<4) { fprintf(stderr,"usage: %s ASSETS FRAMES OUT.txt\n",argv[0]); return 2; }
#if NM_SH4ZAM_GFX
    /* The console's start-up checks of the SH4ZAM forms (nm_render_init), here with this SH4 model (SIM_SH4).
     * SIM_NO_STARTUP_CHECKS=1 skips them (to show that the reference check alone also sees a mutant). */
    if(!getenv("SIM_NO_STARTUP_CHECKS") && (!sq_class_light_check() || !sq_clip_check())) return 1;
#endif
#if SQ_SWEEPS
    if(!getenv("SIM_NO_STARTUP_CHECKS") && !sq_sweep_check()) return 1;
    assert(SQ_SWEEP_MARGIN==(float)NM_REFERENCE_SWEEP_MARGIN);
#endif
    road_depth_setup();
    stage_build=getenv("SIM_STAGE") ? (unsigned)atoi(getenv("SIM_STAGE")) : 6;
    sim_no_cars=getenv("SIM_NO_CARS")!=NULL; sim_no_world=getenv("SIM_NO_WORLD")!=NULL;
    sim_general_emitter=getenv("SIM_GENERAL_EMITTER")!=NULL;
    sim_run_vertex_table=getenv("SIM_RUN_VERTEX_TABLE")!=NULL;
    sim_car_guarded=getenv("SIM_CAR_GUARDED")!=NULL;
    if(!setup(argv[1])) { fputs("setup failed\n",stderr); return 1; }
    if(getenv("SIM_PATHS")) { sim_paths_out=fopen(getenv("SIM_PATHS"),"w"); assert(sim_paths_out); }
    road_restarted=calloc(((const nm_header_t *)packs[1].data)->meshes+1u,1); assert(road_restarted);
    memset(sim_car_level,4,sizeof sim_car_level);
    if(getenv("SIM_CAR_LEVELS")) {
        FILE *levels=fopen(getenv("SIM_CAR_LEVELS"),"r"); assert(levels);
        unsigned mesh,level; while(fscanf(levels,"%u %u",&mesh,&level)==2) { assert(mesh<4096 && level<4); sim_car_level[mesh]=(uint8_t)level; }
        fclose(levels);
    }
    FILE *class_frames=getenv("SIM_CLASS_FRAMES") ? fopen(getenv("SIM_CLASS_FRAMES"),"w") : NULL;
    if(getenv("SIM_PIXEL_FACES")) {
        FILE *list=fopen(getenv("SIM_PIXEL_FACES"),"r"); assert(list);
        const unsigned faces=packs[1].header->faces;
        pix_lookup=malloc(faces*sizeof(int32_t)); for(unsigned f=0;f<faces;++f) pix_lookup[f]=-1;
        pix_faces=malloc(faces*sizeof(pix_face_t));
        unsigned face,rotated; int du,dv;
        while(fscanf(list,"%u %u %d %d",&face,&rotated,&du,&dv)==4) {
            assert(face<faces && pix_lookup[face]<0);
            pix_lookup[face]=(int32_t)pix_face_count; pix_faces[pix_face_count++]=(pix_face_t){face,(uint8_t)rotated,du,dv};
        }
        fclose(list);
    }
    for(unsigned s=0;s<2;++s) {
        /* The largest mesh of each pack: what the emission face table has to address. */
        const nm_header_t *original=packs[s].data; unsigned vertices=0,faces=0;
        for(unsigned m=0;m<original->meshes;++m) {
            if(packs[s].meshes[m].vertex_count>vertices) vertices=packs[s].meshes[m].vertex_count;
            if(packs[s].meshes[m].face_count>faces) faces=packs[s].meshes[m].face_count;
        }
        printf("source %u: largest mesh %u vertices, %u faces (selected stage); emission face table %s\n",s,vertices,faces,sq_emit_faces[s] ? "built" : "NOT built: general emitter, per-vertex points");
    }
    FILE *in=fopen(argv[2],"rb"),*out=fopen(argv[3],"w");
    FILE *faces_out=getenv("SIM_FACES") ? fopen(getenv("SIM_FACES"),"w") : NULL;
    const long depth_step=getenv("SIM_DEPTH") ? atol(getenv("SIM_DEPTH")) : 0;
    assert(in && out);
    if(getenv("SIM_PVR")) {
        pvr_out=fopen(getenv("SIM_PVR"),"w"); assert(pvr_out);
        if(getenv("SIM_PT_CHUNK")) pvr_chunk=(unsigned)atoi(getenv("SIM_PT_CHUNK"))/3;
        if(getenv("SIM_SMOKE_BOUND")) sim_smoke_bound=atoi(getenv("SIM_SMOKE_BOUND"));
        if(getenv("SIM_SIDE")) {
            side_in=fopen(getenv("SIM_SIDE"),"rb"); assert(side_in);
            if(!getenv("SIM_HUD") || !hud_sheets_load(getenv("SIM_HUD"))) { fputs("SIM_SIDE needs SIM_HUD=DIR with hud/sheet-N.dt and hud/hud.dp\n",stderr); return 1; }
            /* The footprint outlines of the two shadow models, as practice_shadow_bind. */
            static const uint32_t models[2]={0x8d9e42,0x8d7ab7};
            for(unsigned i=0;i<packs[0].header->instances;++i) for(unsigned k=0;k<2;++k)
                if(packs[0].instances[i].source_address==models[k] && !shadow_outline_count[k])
                    shadow_outline_count[k]=nm_shadow_outline(packs,packs[0].meshes+packs[0].instances[i].mesh,shadow_outline[k]);
            fprintf(stderr,"sky, HUD (%u sheets) and shadow volumes (outlines %u/%u vertices, %u/%u triangles a volume) are in the model\n",hud_sheet_count,
                shadow_outline_count[0],shadow_outline_count[1],nm_shadow_volume_triangles(shadow_outline_count[0]),nm_shadow_volume_triangles(shadow_outline_count[1]));
        }
        sim_no_hud=getenv("SIM_NO_HUD")!=NULL; sim_no_sky=getenv("SIM_NO_SKY")!=NULL;
        if((sim_fill_probe=getenv("SIM_FILL_PROBE")!=NULL)) {
            probe_alpha.w=probe_alpha.h=8; probe_alpha.opaque=malloc(64); probe_alpha.index=calloc(64,1);
            for(unsigned i=0;i<64;++i) probe_alpha.opaque[i]=!(i&1) && !((i>>3)&1);
        }
        const char *shadows=getenv("SIM_NO_SHADOWS");
        sim_no_volumes=shadows && (!strcmp(shadows,"1") || !strcmp(shadows,"volumes"));
        sim_no_receivers=shadows && (!strcmp(shadows,"1") || !strcmp(shadows,"receivers"));
        assert(!shadows || sim_no_volumes || sim_no_receivers);
        assert(pvr_chunk>=1 && pvr_chunk<=256);
        if(getenv("SIM_CLASSES")) {
            FILE *classes=fopen(getenv("SIM_CLASSES"),"r"); assert(classes);
            unsigned word; char name[24];
            while(fscanf(classes,"%u %23s",&word,name)==2) {
                unsigned k=PVR_BUILTIN_KINDS; while(k<pvr_kinds && strcmp(pvr_kind_names[k],name)) ++k;
                if(k==pvr_kinds) { assert(pvr_kinds<PVR_KINDS); strcpy(pvr_kind_names[pvr_kinds++],name); }
                assert(pvr_classes<256); pvr_class[pvr_classes].word=(uint16_t)word; pvr_class[pvr_classes++].kind=(uint8_t)k;
            }
            fclose(classes);
        }
    }
    unsigned strip_serial=0;
    static nm_instance_t car_instances[1024],*world_instances; static unsigned boundaries[45];
    world_instances=malloc(4096*sizeof(nm_instance_t));
    uint32_t head[8]; long frames=0;
    uint64_t total[2][S_COUNT]={{0}},maximum[2][S_COUNT]={{0}},both_total[S_COUNT]={0},both_max[S_COUNT]={0};
    tri_t *tris=malloc(STREAM_MAX*sizeof(tri_t));
    geo_t *geos=malloc(STREAM_MAX*sizeof(geo_t));
    col_t *cols=malloc(STREAM_MAX*sizeof(col_t)),*wraps=malloc(STREAM_MAX*sizeof(col_t));
    set_t *sets=malloc(STREAM_MAX*sizeof(set_t)),*setwraps=malloc(STREAM_MAX*sizeof(set_t));
    dtri_t *dtris=malloc(STREAM_MAX*sizeof(dtri_t));
    uint32_t *face_seen=calloc(packs[1].header->faces,4); /* frame stamp */
    uint16_t *face_binding=calloc(packs[1].header->faces,2);
    float min_written[3]={FLT_MAX,FLT_MAX,FLT_MAX}; /* car, world surface, other world */
    static const unsigned lists[3]={PVR_LIST_OP_POLY,PVR_LIST_TR_POLY,PVR_LIST_PT_POLY};
    while(fread(head,4,8,in)==8) {
        /* FRM1, or FRM2: the same record followed by the frame's cell bitmaps and race state (world objects), or
         * FRM3: FRM2 followed by the animated actors' state (reference_stream.h, nm_reference_world_objects), or
         * FRM4: FRM3 followed by the slot machine's three reels, or
         * FRM5: FRM4 followed by the road class's depth scale of the frame (a float; before FRM5: NM_ROAD_DEPTH_SCALE). */
        assert(head[0]>=0x314d5246u && head[0]<=0x354d5246u && head[3]<=1024 && head[4]<=4096);
        float view[12]; uint8_t kinds[48]; static float shadow_matrix[45][12];
        assert(fread(boundaries,4,45,in)==45);
        assert(fread(car_instances,sizeof(nm_instance_t),head[3],in)==head[3]);
        assert(fread(world_instances,sizeof(nm_instance_t),head[4],in)==head[4]);
        assert(fread(view,4,12,in)==12);
        assert(fread(kinds,1,48,in)==48); assert(fread(shadow_matrix,48,45,in)==45);
        for(unsigned s=0;s<3;++s) {
            uint32_t first,count; assert(fread(&first,4,1,in)==1 && fread(&count,4,1,in)==1);
            /* The frame's window reflection UVs: registered as the scene does (nm_render_uv_dynamic).
             * SIM_NO_DYNAMIC_UV=1 leaves that out: the reference check must then fail. */
            if(count && !getenv("SIM_NO_DYNAMIC_UV")) assert(sq_uv_dynamic(packs[0].vertices+first,count));
            for(unsigned v=0;v<count;++v) {
                nm_vertex_t *vertex=(nm_vertex_t *)packs[0].vertices+first+v;
                assert(fread(&vertex->u,2,1,in)==1 && fread(&vertex->v,2,1,in)==1);
            }
        }
        uint32_t trailer[24]={0},animated[137]={0};
        if(head[0]!=0x314d5246u) assert(fread(trailer,4,24,in)==24);
        if(head[0]>=0x334d5246u) assert(fread(animated,4,head[0]>=0x344d5246u ? 137 : 134,in)==(head[0]>=0x344d5246u ? 137u : 134u));
        float road_table=0;
        if(head[0]>=0x354d5246u) assert(fread(&road_table,4,1,in)==1);
        if(head[0]>=0x354d5246u && getenv("SIM_ORACLE") && !road_table_legal(road_table,head[2],trailer[22])) {
            printf("reference mismatch: tick %u (attract %u, camera mode %u) has road depth scale %.9g\n",head[1],head[2],trailer[22],(double)road_table);
            return 1;
        }
        /* nm_practice_render_scenery: a cell whose mesh has no selected face is not an instance. */
        unsigned kept=0;
        for(unsigned i=0;i<head[4];++i) if(packs[1].meshes[world_instances[i].mesh].face_count) world_instances[kept++]=world_instances[i];
        /* SIM_NO_CARS / SIM_NO_WORLD: as the measurement builds NATIVE_DIAG_NO_CARS / NATIVE_DIAG_NO_WORLD. */
        const float shadow_focus=head[3] ? car_instances[0].focus[0] : 310.0f;
        if(sim_no_cars) { head[3]=0; for(unsigned i=0;i<45;++i) boundaries[i]=0; }
        if(sim_no_world) kept=0;
        packs[0].instances=car_instances; live_header[0].instances=head[3];
        packs[1].instances=world_instances; live_header[1].instances=kept;
        /* SIM_ORACLE_NO_OBJECTS=1: frames without world objects (synthetic views): the selection is not checked. */
        if(getenv("SIM_ORACLE") && (nm_pack_flags(packs+1)&NM_PACK_WORLD_OBJECTS) && !getenv("SIM_ORACLE_NO_OBJECTS")) {
            /* The frame's object instances against the original's rules (reference_stream.h). */
            if(head[0]==0x314d5246u) { puts("reference mismatch: a pack with world objects needs FRM2 frame tables (cell bitmaps)"); return 1; }
            unsigned first=0; while(first<kept && !(world_instances[first].padding[0]&NM_OBJECT_INSTANCE)) ++first;
            static double origin[2];
            const int n=nm_reference_world_objects(world_original,world_original_count,packs[1].meshes,
                packs[1].header->meshes,view,trailer,trailer+16,
                head[0]>=0x334d5246u ? animated : NULL,head[0]>=0x344d5246u ? animated+134 : NULL,origin,
                world_instances+first,kept-first,&object_boundary,&object_unjudged);
            if(n<0) { printf("world object reference failed at tick %u (attract %u)\n",head[1],head[2]); return 1; }
            object_checked+=(unsigned)n; object_frames+=n>0; ++object_reference_frames;
        }
        writes=queued=0; sim_path_tick=head[1]; sim_path_last[0]=sim_path_last[1]=NULL;
        if(lod_count) { memset(sim_lod_drawn,NM_REFERENCE_LOD_UNWALKED,lod_count); memcpy(lod_before,sq_lod_state,lod_count); }
        memset(sim_ss_drawn,NM_REFERENCE_LOD_UNWALKED,sizeof sim_ss_drawn);
        draw_count=0; draw_opaque=draw_cutout=draw_rear=draw_checker=draw_transformed=draw_instances_rejected=0;
        draw_header_count=0; sq_joined_strips=sq_strip_vertices_saved=0;
        /* As practice_prepare: the camera's scale before the frame's lists. */
        road_depth_frame(road_table,frames);
        if(road_check && !road_records_check(head[1])) return 1;
        assert(sq_prepare_source(0,packs) && sq_prepare_source(1,packs+1));
        assert(sq_queue_objects(boundaries,head[5],head[6]));
        /* The idle wait before the registration: the record cache's step (sq_stream_idle). */
#if defined(SIM_HAS_STREAM) && SQ_STREAMS
        if(sq_stream_pool_bytes) (void)sq_stream_idle(getenv("SIM_STREAM_BUDGET") ? (unsigned)atoi(getenv("SIM_STREAM_BUDGET")) : 2000u);
#endif
        uint64_t frame_stats[2][S_COUNT]={{0}};
        for(unsigned pass=0;pass<3;++pass) for(unsigned q=0;q<queued;++q) {
            if(queue[q].list!=lists[pass]) continue;
            if(queue[q].call==sq_submit_translucent) {
                /* The car effects' smoke (model_submission.h, "Translucent effects"): one callback, no batch. */
                current_source=0; const unsigned before=writes;
                sq_submit_translucent(queue[q].data); assert(!sq_failed);
                for(unsigned i=before;i<writes;++i) stream_ss[i]=0;
                for(unsigned i=before;i<writes;++i) if(stream[i].vertex.flags<PVR_CMD_VERTEX && ((stream[i].header.cmd>>24)&7)!=SQ_TR_LIST) {
                    printf("stream check: tick %u header %u of the translucent callback names list %u\n",head[1],i,(unsigned)((stream[i].header.cmd>>24)&7));
                    return 1;
                }
                continue;
            }
            const int opponents=queue[q].call==sq_submit_opponents;
            current_source=opponents ? 0 : ((nm_sq_batch_t *)queue[q].data)->source;
            const unsigned opponent_pass=opponents ? (unsigned)((unsigned *)queue[q].data-draw_lists) : 0;
            /* One sq_emit_range call per object: its header state restarts. */
            const unsigned objects=opponents ? sq_opponents[opponent_pass].count : 1;
            for(unsigned o=0;o<objects;++o) {
                nm_sq_batch_t *object=opponents ? sq_opponents[opponent_pass].objects[o] : queue[q].data;
                const uint64_t joined=sq_joined_strips; const unsigned before=writes,rear=draw_rear,transformed=draw_transformed;
                sq_submit_object(object,lists[pass]);
                assert(!sq_failed);
                /* A super-sampled object's header: the binding's with the TSP bit (sq_ss_batch); the bit is recorded
                 * and cleared, so that the header is found among the bindings' (the reference checks the bit). */
                for(unsigned i=before;i<writes;++i) {
                    stream_ss[i]=0;
#ifdef SIM_HAS_SS
                    if(stream[i].vertex.flags<PVR_CMD_VERTEX && (stream[i].header.mode2&SQ_TSP_SUPER_SAMPLE)) {
                        stream_ss[i]=1; stream[i].header.mode2&=~SQ_TSP_SUPER_SAMPLE; }
#endif
                }
                /* Every header a list callback submits names that list (the TA takes the polygon type from the list
                 * it is in: a punch-through header in the opaque list is a wrong picture the reference, which groups
                 * triangles by their header's list, cannot see). */
                for(unsigned i=before;i<writes;++i) if(stream[i].vertex.flags<PVR_CMD_VERTEX && ((stream[i].header.cmd>>24)&7)!=lists[pass]) {
                    printf("stream check: tick %u header %u of source %u names list %u in the callback of list %u\n",head[1],i,
                        (unsigned)stream_source[i],(unsigned)((stream[i].header.cmd>>24)&7),lists[pass]);
                    return 1;
                }
                frame_stats[current_source][S_JOINED]+=sq_joined_strips-joined;
                frame_stats[current_source][S_REAR]+=draw_rear-rear;
                frame_stats[current_source][S_TRANSFORMED]+=draw_transformed-transformed; sim_all_vertices+=draw_transformed-transformed;
                { static uint8_t seen[65536]; unsigned distinct=0;
                  for(unsigned i=before;i<writes;++i) if(stream[i].vertex.flags<PVR_CMD_VERTEX) {
                      unsigned c=0; for(;c<canon_count;++c) if(!memcmp(canon+c,&stream[i].header,32)) break;
                      if(!seen[c]) { seen[c]=1; ++distinct; } }
                  for(unsigned i=before;i<writes;++i) if(stream[i].vertex.flags<PVR_CMD_VERTEX) {
                      unsigned c=0; for(;c<canon_count;++c) if(!memcmp(canon+c,&stream[i].header,32)) break; seen[c]=0; }
                  frame_stats[current_source][S_FLOOR]+=distinct; ++frame_stats[current_source][S_OBJECTS]; }
            }
        }
        { int any=0;
          for(unsigned b=0;b<sq_sources[1].count;++b) {
              const nm_sq_batch_t *batch=sq_sources[1].batches+b;
              if(!batch->supersample) continue;
              assert(batch->instance_index<sizeof sim_ss_drawn);
              sim_ss_drawn[batch->instance_index]=(uint8_t)(batch->supersample-1u);
              ++ss_batches[batch->supersample-1u]; any|=batch->supersample==2u;
          }
          ss_frames+=(unsigned)any; }
        for(unsigned s=0;s<2;++s) for(unsigned b=0;b<sq_sources[s].count;++b) {
            const nm_sq_batch_t *batch=sq_sources[s].batches+b;
            if(!batch->prepared) continue;
            frame_stats[s][S_WALKED]+=batch->mesh->face_count;
            frame_stats[s][S_QUEUED]+=batch->opaque_count+batch->cutout_count;
        }
        /* Expand strips to triangles (same rule as tests/sq_strips.h). */
        unsigned count=0,first=0,vertices=0,header=0,list=0,header_at=0;
        unsigned long long frame_class[SIM_CLASSES][4]; memset(frame_class,0,sizeof frame_class);
        /* Pixel check: triangles of the listed faces this frame (face, instance, clipped, texture size, vertices). */
        static struct { unsigned face,instance,clipped,w,h; pix_vertex_t v[3]; } *pix_tris; static unsigned pix_tri_count;
        if(pix_faces && !pix_tris) pix_tris=malloc(STREAM_MAX*sizeof(*pix_tris));
        pix_tri_count=0;
        int header_pending=0;
        const int oracle=getenv("SIM_ORACLE")!=NULL;
        if(oracle && !oracle_fast) { oracle_fast=malloc(STREAM_MAX*sizeof(*oracle_fast)); oracle_reference=malloc(STREAM_MAX*sizeof(*oracle_reference)); oracle_order=malloc(STREAM_MAX*4); }
        static uint8_t texture_seen[2][4200],bank_seen[64]; memset(texture_seen,0,sizeof texture_seen); memset(bank_seen,0,sizeof bank_seen);
        for(unsigned i=0;i<writes;++i) {
            const unsigned s=stream_source[i];
            if(stream[i].vertex.flags<PVR_CMD_VERTEX) {
                assert(!vertices);
                unsigned c=0; for(;c<canon_count;++c) if(!memcmp(canon+c,&stream[i].header,32)) break;
                assert(c<canon_count); header=c; list=(stream[i].header.cmd>>24)&7; header_at=i;
                ++frame_stats[s][S_HEADERS]; header_pending=1;
                if(canon[c].mode1) { texture_seen[s][canon[c].mode1&0xfffff]=1; bank_seen[canon[c].mode2>>16]|=(uint8_t)(1u<<s); }
                continue;
            }
            ++frame_stats[s][S_VERTICES];
            const unsigned vertex_class=(s ? 0u : 2u+2u*sim_car_level[packs[0].instances[stream_instance[i]].mesh])+(list==PVR_LIST_PT_POLY);
            ++frame_class[vertex_class][0]; frame_class[vertex_class][3]+=(unsigned)header_pending; header_pending=0;
            if(stream[i].vertex.flags==PVR_CMD_VERTEX_EOL) ++frame_class[vertex_class][2];
            { /* What the TA must never be handed: a non-finite word, a depth that is not positive, a coordinate beyond the guard band. */
              const pvr_vertex_t *p=&stream[i].vertex; const float limit_x=(496.0f+SQ_GUARD_BAND)*(640.0f/496.0f)*ENJ_XSCALE,limit_y=(384.0f+SQ_GUARD_BAND)*(480.0f/384.0f);
              if(!isfinite(p->x) || !isfinite(p->y) || !isfinite(p->z) || !isfinite(p->u) || !isfinite(p->v) || !(p->z>0) ||
                 p->x<-SQ_GUARD_BAND*(640.0f/496.0f)*ENJ_XSCALE-1 || p->x>limit_x+1 || p->y<-SQ_GUARD_BAND*(480.0f/384.0f)-1 || p->y>limit_y+1 ||
                 (p->flags!=PVR_CMD_VERTEX && p->flags!=PVR_CMD_VERTEX_EOL) || p->oargb) {
                  printf("stream check: tick %u vertex %u of source %u: %g %g %g %g %g flags %08x\n",head[1],i,s,(double)p->x,(double)p->y,(double)p->z,(double)p->u,(double)p->v,(unsigned)p->flags);
                  return 1; }
              if(fabsf(p->x-320.0f*ENJ_XSCALE)>stream_reach[0]) stream_reach[0]=fabsf(p->x-320.0f*ENJ_XSCALE);
              if(fabsf(p->y-240.0f)>stream_reach[1]) stream_reach[1]=fabsf(p->y-240.0f);
              if(p->z>stream_reach[2]) stream_reach[2]=p->z; }
            if(!vertices) first=i;
            ++vertices;
            if(vertices>=3) {
                const unsigned t=vertices-3;
                const unsigned corners[3]={first+t+(t&1),first+t+!(t&1),i};
                const unsigned provoking=SIM_PROVOKING(first,t,i);
                tri_t *tri=tris+count; memset(tri,0,sizeof *tri);
                tri->source=(uint8_t)s; tri->list=(uint8_t)list; tri->header=(uint16_t)header;
                tri->argb=stream[provoking].vertex.argb;
                const unsigned binding=stream_binding[provoking],face_index=stream_face[provoking];
                assert(binding<mats[s].count && face_index<((const nm_header_t *)packs[s].data)->faces);
                const nm_face_t *face=packs[s].faces+face_index;
                tri->face=face_index; tri->layer=face->layer;
                if(oracle) {
                    /* The header in force is the one this binding compiles to, all 32 bytes. */
                    if(memcmp(&stream[header_at].header,&sim_cache[s][binding].header,32)) {
                        printf("reference mismatch: tick %u source %u face %u binding %u submitted under another binding's header\n",head[1],s,face_index,binding); return 1; }
                    nm_reference_triangle_t *o=oracle_fast+count; memset(o,0,sizeof *o);
                    o->source=(uint8_t)s; o->list=(uint8_t)list; o->binding=(uint16_t)binding; o->instance=stream_instance[provoking]; o->face=face_index;
                    o->supersample=stream_ss[header_at]; ss_triangles+=o->supersample;
                    o->argb=stream[provoking].vertex.argb;
                    /* A triangle of a clipped polygon: the polygon's number (SQ_CLIP_TAG), the same on its three vertices. */
                    o->polygon=stream_polygon[i]; o->clipped=o->polygon!=0;
                    assert(stream_polygon[corners[0]]==o->polygon && stream_polygon[corners[1]]==o->polygon);
                    for(unsigned v=0;v<3;++v) { const pvr_vertex_t *p=&stream[corners[v]].vertex;
                        assert(stream_instance[corners[v]]==o->instance);
                        o->v[v][0]=p->x; o->v[v][1]=p->y; o->v[v][2]=p->z; o->v[v][3]=p->u; o->v[v][4]=p->v; }
                }
                dtri_t *d=dtris+count; d->source=(uint8_t)s; d->list=(uint8_t)list; d->layer=face->layer; d->face=face_index;
                d->surface=s && world_draw.surface[binding];
                d->stage=s ? (uint8_t)(packs[1].materials[face->material].padding[0]&NM_SCENERY_STAGE_MASK) : 0;
                d->scale=(d->surface ? sim_road_depth : 1.0f)*draw_layer_scale[face->layer];
                d->strip=strip_serial; d->binding=(uint16_t)binding; d->kind=s ? (uint8_t)(d->surface ? KIND_ROAD : 1u+d->stage) : 0; d->argb=tri->argb;
                d->extra=d->sheet=0; d->textured=mats[s].bindings[binding].resource!=NM_MATERIAL_SOLID;
                /* compile_draw_header. NATIVE_FILTER_ALL=1 (the default build since 14 October 2026; SIM_FILTER_ALL=0
                 * models one without it): every texture bilinear except the catch-fence stipple (draw_binding_stipple).
                 * Without it: bilinear on opaque-list textures, except the road-surface class and the opaque pieces of split cutouts. */
                { static int filter_all=-1;
                  if(filter_all<0) filter_all=!getenv("SIM_FILTER_ALL") || atoi(getenv("SIM_FILTER_ALL"))!=0;
                  const nm_material_binding_t *drawn=mats[s].bindings+binding;
                  const int stipple=s && ((drawn->header[0]>>13)&7u)==3u && (drawn->header[1]&0xfcffu)==0x8000u;
                  d->bilinear=d->textured && !NM_SIM_UNFILTERED && (filter_all ? !stipple :
                      list==PVR_LIST_OP_POLY && !(s && (d->surface || (packs[1].materials[face->material].padding[0]&NM_SCENERY_POINT)))); }
                if(s) for(unsigned k=0;k<pvr_classes;++k) if(pvr_class[k].word==packs[1].materials[face->material].header[2]) d->kind=pvr_class[k].kind;
                unsigned low=0; float key[3][5];
                for(unsigned v=0;v<3;++v) {
                    const pvr_vertex_t *p=&stream[corners[v]].vertex;
                    key[v][0]=p->x; key[v][1]=p->y; key[v][2]=p->z; key[v][3]=p->u; key[v][4]=p->v;
                    d->x[v]=p->x; d->y[v]=p->y; d->z[v]=p->z; d->u[v]=p->u; d->v[v]=p->v;
                    const unsigned kind=!s ? 0 : d->surface ? 1 : 2;
                    if(p->z<min_written[kind]) min_written[kind]=p->z;
                    if(memcmp(key[v],key[low],sizeof key[0])<0) low=v;
                }
                for(unsigned v=0;v<3;++v) memcpy(tri->v[v],key[(low+v)%3],sizeof key[0]);
                geo_t *geo=geos+count; memset(geo,0,sizeof *geo); geo->source=(uint8_t)s; geo->list=(uint8_t)list;
                memcpy(geo->v,tri->v,sizeof geo->v);
                { col_t *col=cols+count; memset(col,0,sizeof *col); col->source=(uint8_t)s; col->list=(uint8_t)list; col->header=(uint16_t)header; col->argb=tri->argb;
                  memcpy(col->v,tri->v,sizeof col->v);
                  col_t *wrap=wraps+count; *wrap=*col;
                  float wk[3][5]; memcpy(wk,key,sizeof wk);
                  for(unsigned a=0;a<2;++a) {
                      const float period=canon[header].mode2&(0x100u<<a) ? 2.0f : 1.0f;
                      float low_uv=fminf(wk[0][3+a],fminf(wk[1][3+a],wk[2][3+a]));
                      const float shift=period*floorf(low_uv/period);
                      for(unsigned v=0;v<3;++v) { const float before=wk[v][3+a]; wk[v][3+a]=before-shift;
                          /* pt-screen smoke (NM_SMOKE_MODE 2): u holds half of x + y and may lose a bit here (digest only). */
                          assert(wk[v][3+a]+shift==before || NM_SMOKE_DOOR); }
                  }
                  unsigned wl=0; for(unsigned v=1;v<3;++v) if(memcmp(wk[v],wk[wl],sizeof wk[0])<0) wl=v;
                  for(unsigned v=0;v<3;++v) memcpy(wrap->v[v],wk[(wl+v)%3],sizeof wk[0]);
                  const uint64_t header_bytes=fnv(canon+header,32,UINT64_C(1469598103934665603));
                  set_fill(sets+count,s,list,header_bytes,tri->argb,key); set_fill(setwraps+count,s,list,header_bytes,tri->argb,wk); }
                /* One colour and one header per drawn binding, always. */
                assert(!use_triangles[s][binding] || (use_argb[s][binding]==tri->argb && use_header[s][binding]==header));
                use_argb[s][binding]=tri->argb; use_header[s][binding]=(uint16_t)header; ++use_triangles[s][binding];
                if(s) { face_seen[face_index]=(uint32_t)frames+1; face_binding[face_index]=(uint16_t)binding; }
                ++frame_stats[s][S_TRIANGLES]; frame_stats[s][S_TR_TRIANGLES]+=list==PVR_LIST_TR_POLY; ++frame_class[vertex_class][1];
                if(pix_faces && s && pix_lookup[face_index]>=0) {
                    __typeof__(*pix_tris) *pt=pix_tris+pix_tri_count++;
                    const unsigned resource=mats[1].bindings[binding].resource;
                    pt->face=face_index; pt->instance=stream_instance[provoking]; pt->clipped=0;
                    pt->w=resource==NM_MATERIAL_SOLID ? 8 : mats[1].textures[resource].width; pt->h=resource==NM_MATERIAL_SOLID ? 8 : mats[1].textures[resource].height;
                    for(unsigned v=0;v<3;++v) { const pvr_vertex_t *p=&stream[corners[v]].vertex; pt->v[v]=(pix_vertex_t){p->x,p->y,p->z,p->u,p->v}; pt->clipped|=stream_clip[corners[v]]; }
                }
                ++count;
            }
            if(stream[i].vertex.flags==PVR_CMD_VERTEX_EOL) { assert(vertices>=3); vertices=0; ++frame_stats[s][S_STRIPS]; ++strip_serial; }
        }
        assert(!vertices && count==draw_count);
        if(oracle) {
            for(unsigned i=0;i<count;++i) oracle_order[i]=i;
            qsort(oracle_order,count,4,oracle_order_compare);
            static nm_reference_triangle_t *sorted; if(!sorted) sorted=malloc(STREAM_MAX*sizeof(*sorted));
            for(unsigned i=0;i<count;++i) sorted[i]=oracle_fast[oracle_order[i]];
            unsigned at=0;
            for(unsigned s=0;s<2;++s) {
                const nm_reference_source_t source={packs+s,mats+s,(int)s,s ? world_draw.surface : NULL,sim_road_depth,
                    SQ_GUARD_BAND,PVR_LIST_OP_POLY,PVR_LIST_PT_POLY,
                    s && lod_count ? packs[1].lod_groups : NULL,s && lod_count ? sim_lod_drawn : NULL,s && lod_count ? oracle_lod_state : NULL,
                    s && lod_count && SQ_SWEEPS ? oracle_sweep_group : NULL,oracle_sweep_plane,s && lod_count && SQ_SWEEPS ? oracle_sweep_planes : 0u,
                    s || !sq_tr_meshes ? 0u : SQ_TR_LIST,NM_SMOKE_DOOR,SIM_TIGHT,
                    s && oracle_ss_state ? sim_ss_drawn : NULL,s ? oracle_ss_state : NULL};
                for(unsigned instance=0;instance<packs[s].header->instances;++instance) {
                    const int n=nm_reference_instance(&source,s,instance,oracle_reference,STREAM_MAX);
                    if(n<0) { printf("reference failed at tick %u\n",head[1]); return 1; }
                    for(unsigned l=0;l<3;++l) {
                        const unsigned list_id=lists[l]; unsigned end=at; /* opaque, translucent, punch-through: the sort order */
                        while(end<count && sorted[end].source==s && sorted[end].instance==instance && sorted[end].list==list_id) ++end;
                        const int skipped=nm_reference_compare(oracle_reference,(unsigned)n,sorted+at,end-at,list_id);
                        if(skipped<0) { printf("reference mismatch at tick %u (attract %u)\n",head[1],head[2]); return 1; }
                        oracle_matched+=end-at; oracle_skipped+=(unsigned)skipped; at=end;
                    }
                    oracle_reference_total+=(unsigned)n; ++oracle_instances;
                }
            }
            if(at!=count) { printf("reference mismatch at tick %u: %u submitted triangles belong to no instance or list of the frame\n",head[1],count-at); return 1; }
            /* The translucent triangles in submission order: their faces back to front by the reference's key. */
            { double last=1e300; uint32_t last_face=UINT32_MAX,last_instance=UINT32_MAX; unsigned tr=0;
              for(unsigned i=0;i<count;++i) {
                  const nm_reference_triangle_t *o=oracle_fast+i;
                  if(o->list!=PVR_LIST_TR_POLY) continue;
                  ++tr;
                  if(o->face==last_face && o->instance==last_instance) continue;
                  const double key=nm_reference_translucent_key(packs,o->instance,o->face);
                  if(key>last+1e-5*fabs(last)+1e-6) {
                      printf("reference mismatch at tick %u: translucent face %u of instance %u (nearest depth %.9g) after one nearer (%.9g)\n",
                          head[1],o->face,o->instance,key,last); return 1; }
                  ++tr_ordered; last=key; last_face=o->face; last_instance=o->instance;
              }
              tr_triangles_total+=tr; if(tr>tr_triangles_max) tr_triangles_max=tr; }
            ++oracle_frames;
            /* SIM_TOLERANCE_LOG=FILE: per frame what the reference's tolerances admitted: tick, lit car triangles drawn with a
             * neighbouring luma's binding, clipped polygons, largest x, y (pixels), u, v (texels), z (relative) differences. */
            {
                static FILE *log; static int opened; static unsigned long long neighbours_before,polygons_before;
                if(!opened) { opened=1; if(getenv("SIM_TOLERANCE_LOG")) { log=fopen(getenv("SIM_TOLERANCE_LOG"),"w"); assert(log); } }
                const unsigned long long neighbours=nm_reference_tolerance.neighbour_triangles[0]+nm_reference_tolerance.neighbour_triangles[1];
                if(log) fprintf(log,"%u %llu %llu %.6g %.6g %.6g %.6g %.6g\n",head[1],neighbours-neighbours_before,nm_reference_tolerance.clipped_polygons-polygons_before,
                    nm_reference_tolerance.frame_max[0],nm_reference_tolerance.frame_max[1],nm_reference_tolerance.frame_max[2],nm_reference_tolerance.frame_max[3],nm_reference_tolerance.frame_max[4]);
                neighbours_before=neighbours; polygons_before=nm_reference_tolerance.clipped_polygons;
                memset(nm_reference_tolerance.frame_max,0,sizeof(nm_reference_tolerance.frame_max));
            }
        }
        if(lod_count) {
            /* Groups walked this frame by class and level; a change is a walked group drawn at another level than
             * the last time it was walked (the first time a group is walked is not a change). */
            unsigned walked[2]={0,0},level_now[2][NM_LOD_LEVELS]={{0}},changes[2]={0,0},pops[2]={0,0},running[2]={0,0},straddling=0;
            for(unsigned g=0;g<lod_count;++g) {
                const unsigned drawn=sim_lod_drawn[g],c=lod_class[g];
                const nm_lod_group_t *group=packs[1].lod_groups+g;
                const unsigned faces=(unsigned)group->face_count[0]+group->face_count[1]+group->face_count[2];
                int shown=0;
                for(unsigned f=0;f<faces && !shown;++f) shown=face_seen[group->face_first+f]==(uint32_t)frames+1;
                const int shown_before=lod_shown[g]; lod_shown[g]=(uint8_t)shown;
                if(drawn==NM_REFERENCE_LOD_UNWALKED) { assert(sq_lod_state[g]==lod_before[g] && !shown); continue; }
                assert((drawn<NM_LOD_LEVELS || (SQ_SWEEPS && drawn>=SQ_SWEEP)) && sq_lod_state[g]==drawn);
                ++walked[c];
                if(drawn>=SQ_SWEEP) ++straddling; else ++level_now[c][drawn];
                const int continued=lod_last_walk[g]==(uint32_t)frames; /* walked in the frame before (stored as frame+1) */
                running[c]+=continued && (shown || shown_before);
                /* With the sweep a level is a range of levels (code SQ_SWEEP|n0|n1<<2): a change is a jump between ranges
                 * that do not overlap (a panel moving into or out of a band polygon is continuous). */
                const unsigned before=lod_before[g],jump=sim_lod_jump(before,drawn);
                if(lod_seen[g]) { changes[c]+=jump; pops[c]+=jump && continued && (shown || shown_before); }
                else { lod_seen[g]=1; ++lod_first_seen[c]; }
                lod_last_walk[g]=(uint32_t)frames+1;
            }
            for(unsigned c=0;c<2;++c) {
                lod_walked[c]+=walked[c]; lod_changes[c]+=changes[c]; lod_pops[c]+=pops[c]; lod_running[c]+=running[c];
                if(pops[c]>lod_pops_max[c]) lod_pops_max[c]=pops[c];
                if(changes[c]>lod_changes_max[c]) lod_changes_max[c]=changes[c];
                if(walked[c]>lod_walked_max[c]) lod_walked_max[c]=walked[c];
                for(unsigned l=0;l<NM_LOD_LEVELS;++l) lod_at_level[c][l]+=level_now[c][l];
            }
            lod_straddling+=straddling; lod_straddling_frames+=straddling!=0; if(straddling>lod_straddling_max) lod_straddling_max=straddling;
            /* SIM_LOD_GROUPS=FILE: per frame the level (or sweep code) of every walked fence-panel group. */
            { static FILE *groups_out; static int opened;
              if(!opened) { opened=1; if(getenv("SIM_LOD_GROUPS")) { groups_out=fopen(getenv("SIM_LOD_GROUPS"),"w"); assert(groups_out); } }
              if(groups_out) { fprintf(groups_out,"G tick=%u",head[1]);
                  for(unsigned g=0;g<lod_count;++g) if(lod_class[g] && sim_lod_drawn[g]!=NM_REFERENCE_LOD_UNWALKED) fprintf(groups_out," %u:%u",g,sim_lod_drawn[g]);
                  fputc('\n',groups_out); } }
            if(!lod_out && getenv("SIM_LOD")) { lod_out=fopen(getenv("SIM_LOD"),"w"); assert(lod_out); }
            if(lod_out) fprintf(lod_out,"L tick=%u cutouts walked=%u levels=%u,%u,%u changes=%u pops=%u shown=%u fence walked=%u levels=%u,%u,%u changes=%u pops=%u shown=%u straddling=%u\n",head[1],
                walked[0],level_now[0][0],level_now[0][1],level_now[0][2],changes[0],pops[0],running[0],
                walked[1],level_now[1][0],level_now[1][1],level_now[1][2],changes[1],pops[1],running[1],straddling);
        }
        unsigned banks_both=0;
        for(unsigned s=0;s<2;++s) {
            for(unsigned i=0;i<4200;++i) frame_stats[s][S_TEXTURES]+=texture_seen[s][i];
            for(unsigned i=0;i<64;++i) frame_stats[s][S_PALETTES]+=(bank_seen[i]>>s)&1;
            frame_stats[s][S_TA]=32*(frame_stats[s][S_HEADERS]+frame_stats[s][S_VERTICES]);
        }
        for(unsigned i=0;i<64;++i) banks_both+=bank_seen[i]!=0;
        if(faces_out) {
            /* World faces of this frame by pack face index: D drawn (at least
             * one triangle on screen, with the luma of the side shown), Q
             * queued only (faced the view, clipped away), and one V line with
             * the view matrix for aligning the reference frame. */
            fprintf(faces_out,"V %u %u",head[1],head[2]);
            for(unsigned k=0;k<12;++k) fprintf(faces_out," %.9g",(double)view[k]);
            fputc('\n',faces_out);
            for(unsigned b=0;b<sq_sources[1].count;++b) {
                const nm_sq_batch_t *batch=sq_sources[1].batches+b;
                if(!batch->prepared) continue;
                const unsigned first=batch->mesh->face_first,n=batch->mesh->face_count;
                for(unsigned list=0;list<3;++list) {
                    const unsigned entries=list==0 ? batch->opaque_count : list==1 ? batch->cutout_count : 0;
                    for(unsigned i=0;i<entries;++i) {
                        const uint32_t entry=batch->bindings[list==0 ? i : list==1 ? n-1-i : batch->opaque_count+i];
                        const unsigned face_index=first+(entry>>16);
                        const int drawn=face_seen[face_index]==(uint32_t)frames+1;
                        fprintf(faces_out,"%c %u %u %u\n",drawn ? 'D' : 'Q',head[1],live_to_pack[face_index],
                            (unsigned)mats[1].bindings[entry&65535u].luma);
                    }
                }
            }
        }
        if(pvr_out) {
            /* Sky, HUD and shadow volumes: appended for the model only (the other consumers stop at count). */
            unsigned all=count; frame_extra_ta=0; frame_shadows.count=0;
            if(side_in) {
                /* record[2]: 0 no HUD; 1 whole quads (xy in arcade pixels, uv, sheet: 17 words); 2 the tight rectangles of
                 * render/ui/hud_draw.h (xy in output pixels, uv, depth, sheet | opaque<<8: 19 words). */
                uint32_t record[6]; assert(fread(record,4,6,side_in)==6 && record[0]==head[1] && record[1]==head[2] && record[3]<=512);
                static float quads[512][19]; const unsigned quad_words=record[2]==2 ? 19 : 17;
                for(unsigned q=0;q<record[3];++q) assert(fread(quads[q],4,quad_words,side_in)==quad_words);
                if(stage_build>=1 && !sim_no_sky) {
                    nm_sky_rect_t rects[NM_SKY_RECTS];
                    const unsigned n=nm_sky_rects((uint16_t)record[4],(uint16_t)record[5],rects); assert(n);
                    for(unsigned i=0;i<n;++i) { const nm_sky_rect_t *r=rects+i; float corner[4][4];
                        for(unsigned c=0;c<4;++c) { const unsigned dx=c>=2 ? r->width : 0,dy=c&1 ? 0 : r->height;
                            corner[c][0]=(float)(r->x+dx)*(640.0f/496.0f); corner[c][1]=(float)(r->y+dy)*(480.0f/384.0f);
                            corner[c][2]=(float)(r->u+dx)*(1.0f/512); corner[c][3]=(float)(r->v+dy)*(1.0f/512); }
                        all+=extra_quad(dtris+all,corner,NM_SKY_DEPTH,PVR_LIST_OP_POLY,KIND_SKY,r->page<4 ? 2 : 3,0,strip_serial++); }
                    frame_extra_ta+=160ull*n;
                }
                if(record[2]==2 && !sim_no_hud) {
                    /* By depth: cores on the opaque list, pieces punch-through. SIM_HUD_FLAT=1: all punch-through at depth 1
                     * in paint order (the frames with the finish actor or the debug text). A header where list or sheet changes. */
                    const int flat=getenv("SIM_HUD_FLAT")!=NULL; unsigned headers=0,last[2]={99,99};
                    for(unsigned q=0;q<record[3];++q) { float corner[4][4]; uint32_t word; memcpy(&word,quads[q]+17,4);
                        const unsigned sheet=word&255u,opaque=!flat && (word>>8&1u); assert(sheet<hud_sheet_count);
                        if(last[opaque]!=sheet) { last[opaque]=sheet; ++headers; }
                        for(unsigned c=0;c<4;++c) { corner[c][0]=quads[q][2*c]; corner[c][1]=quads[q][2*c+1]; corner[c][2]=quads[q][8+2*c]; corner[c][3]=quads[q][9+2*c]; }
                        all+=extra_quad(dtris+all,corner,flat ? 1.0f : quads[q][16],opaque ? PVR_LIST_OP_POLY : PVR_LIST_PT_POLY,KIND_HUD,1,sheet,strip_serial++); }
                    frame_extra_ta+=128ull*record[3]+32ull*headers;
                }
                if(record[2]==1 && !sim_no_hud) {
                    unsigned sheets=0;
                    for(unsigned q=0;q<record[3];++q) { float corner[4][4]; uint32_t sheet; memcpy(&sheet,quads[q]+16,4); assert(sheet<hud_sheet_count);
                        sheets|=1u<<sheet;
                        for(unsigned c=0;c<4;++c) { corner[c][0]=quads[q][2*c]*(640.0f/496.0f); corner[c][1]=quads[q][2*c+1]*(480.0f/384.0f);
                            corner[c][2]=quads[q][8+2*c]; corner[c][3]=quads[q][9+2*c]; }
                        all+=extra_quad(dtris+all,corner,1.0f,PVR_LIST_PT_POLY,KIND_HUD,1,sheet,strip_serial++); }
                    frame_extra_ta+=128ull*record[3]+32ull*(unsigned)__builtin_popcount(sheets);
                }
                if(record[2] && sim_fill_probe) /* practice_probe: far layer first, the near one a texel aside */
                    for(unsigned layer=0;layer<2;++layer) { float corner[4][4];
                        for(unsigned c=0;c<4;++c) { const float x=c>=2 ? 448.0f : 192.0f,y=c&1 ? 224.0f : 480.0f;
                            corner[c][0]=x; corner[c][1]=y; corner[c][2]=(x+(float)layer)*0.125f; corner[c][3]=y*0.125f; }
                        all+=extra_quad(dtris+all,corner,layer ? 0.9f : 0.8f,PVR_LIST_PT_POLY,KIND_PROBE,4,0,strip_serial++);
                        frame_extra_ta+=128+32u*!layer; }
                frame_shadows.count=head[7]; frame_shadows.kind=kinds; frame_shadows.matrix=(const float (*)[12])shadow_matrix; frame_shadows.view=view; frame_shadows.focus=shadow_focus;
            }
            pvr_frame(dtris,all,head[1]);
        }
        if(getenv("SIM_COLOR") && getenv("SIM_COLOR_TICKS")) {
            char want[32]; snprintf(want,sizeof want,",%u,",head[1]);
            if(strstr(getenv("SIM_COLOR_TICKS"),want)) colour_frame(dtris,count,head[1],getenv("SIM_COLOR"));
        }
        image_digest=0;
        if(depth_step && !(frames%depth_step)) {
            depth_frame(dtris,count);
            const char *ticks=getenv("SIM_IMAGE_TICKS");
            char want[32]; snprintf(want,sizeof want,",%u,",head[1]);
            if(ticks && getenv("SIM_IMAGE_DUMP") && strstr(ticks,want)) {
                char name[512]; snprintf(name,sizeof name,"%s/%u.img",getenv("SIM_IMAGE_DUMP"),head[1]);
                FILE *img=fopen(name,"wb"); assert(img);
                fwrite(image_winner,4,DW*DH,img); fwrite(image_translucent,4,DW*DH,img); fwrite(depth_written,4,DW*DH,img); fclose(img);
            }
        }
        if(getenv("SIM_DUMP_TICK") && (unsigned)atoi(getenv("SIM_DUMP_TICK"))==head[1]) {
            FILE *td=fopen(getenv("SIM_DUMP"),"w");
            for(unsigned i=0;i<count;++i) if(geos[i].source) { fprintf(td,"%u",geos[i].list);
                for(unsigned v=0;v<3;++v) for(unsigned k=0;k<5;++k) fprintf(td," %.9g",(double)geos[i].v[v][k]); fputc('\n',td); }
            fclose(td);
        }
        qsort(tris,count,sizeof(tri_t),tri_compare);
        const uint64_t digest=fnv(tris,count*sizeof(tri_t),UINT64_C(1469598103934665603));
        qsort(geos,count,sizeof(geo_t),geo_compare);
        const uint64_t geometry=fnv(geos,count*sizeof(geo_t),UINT64_C(1469598103934665603));
        qsort(cols,count,sizeof(col_t),col_compare); qsort(wraps,count,sizeof(col_t),col_compare);
        const uint64_t colour=fnv(cols,count*sizeof(col_t),UINT64_C(1469598103934665603));
        const uint64_t wrapped=fnv(wraps,count*sizeof(col_t),UINT64_C(1469598103934665603));
        fprintf(out,"F tick=%u attract=%u triangles=%u digest=%016llx geometry=%016llx colour=%016llx wrapped=%016llx image=%016llx banks=%u",head[1],head[2],count,
            (unsigned long long)digest,(unsigned long long)geometry,(unsigned long long)colour,(unsigned long long)wrapped,(unsigned long long)image_digest,banks_both);
        for(unsigned s=0;s<2;++s) {
            fprintf(out," %s:",s ? "world" : "car");
            for(unsigned k=0;k<S_COUNT;++k) {
                fprintf(out," %s=%llu",stat_names[k],(unsigned long long)frame_stats[s][k]);
                total[s][k]+=frame_stats[s][k]; if(frame_stats[s][k]>maximum[s][k]) maximum[s][k]=frame_stats[s][k];
                const uint64_t both=k==S_PALETTES ? banks_both : frame_stats[0][k]+frame_stats[1][k];
                if(s) { both_total[k]+=both; if(both>both_max[k]) both_max[k]=both; }
            }
        }
#if defined(SIM_HAS_STREAM) && SQ_STREAMS
        /* SIM_STREAM_EVENTS=FILE: this frame's stream events and world statistics, one line (the SH4 instruction model). */
        if(getenv("SIM_STREAM_EVENTS")) {
            if(!sim_stream_events_out) { sim_stream_events_out=fopen(getenv("SIM_STREAM_EVENTS"),"w"); assert(sim_stream_events_out); }
            fprintf(sim_stream_events_out,"%u",head[1]);
            for(unsigned e=0;e<SQ_STREAM_EVENTS;++e) { fprintf(sim_stream_events_out," %llu",sim_stream_events[e]-sim_stream_events_before[e]); sim_stream_events_before[e]=sim_stream_events[e]; }
            fprintf(sim_stream_events_out," | %llu %llu %llu %llu\n",(unsigned long long)frame_stats[1][S_HEADERS],(unsigned long long)frame_stats[1][S_VERTICES],
                (unsigned long long)frame_stats[1][S_TRANSFORMED],(unsigned long long)frame_stats[1][S_WALKED]);
        }
#endif
#if defined(SIM_HAS_CAR_STREAM) && SQ_CAR_STREAMS
        /* SIM_CAR_EVENTS=FILE: this frame's car stream events (SQ_CAR_COUNT), then what the queue path did with the cars
         * (faces walked by sq_walk_car, queued, rear, positions transformed, mesh vertices of the point loops, faces lit and
         * of them repeats of a light class: SQ_LIGHT_PROBE): one line, the SH4 instruction model's input. */
        if(getenv("SIM_CAR_EVENTS")) {
            static unsigned long long light_calls_before,light_repeats_before,mesh_vertices_before;
            if(!sim_car_events_out) { sim_car_events_out=fopen(getenv("SIM_CAR_EVENTS"),"w"); assert(sim_car_events_out); }
            fprintf(sim_car_events_out,"%u",head[1]);
            for(unsigned e=0;e<SQ_CAR_EVENTS;++e) { fprintf(sim_car_events_out," %llu",sim_car_events[e]-sim_car_events_before[e]); sim_car_events_before[e]=sim_car_events[e]; }
            fprintf(sim_car_events_out," | %llu %llu %llu %llu %llu %llu %llu\n",(unsigned long long)frame_stats[0][S_WALKED],(unsigned long long)frame_stats[0][S_QUEUED],
                (unsigned long long)frame_stats[0][S_REAR],(unsigned long long)frame_stats[0][S_TRANSFORMED],sim_path_points[0][0][1]-mesh_vertices_before,
                sim_probe_light_calls-light_calls_before,sim_probe_light_repeats-light_repeats_before);
            light_calls_before=sim_probe_light_calls; light_repeats_before=sim_probe_light_repeats; mesh_vertices_before=sim_path_points[0][0][1];
        }
#endif
        if(pix_faces) {
            /* The triangles of one face of one instance are consecutive in the stream: one strip, or a run. */
            for(unsigned i=0;i<pix_tri_count;) {
                unsigned j=i+1,clipped=pix_tris[i].clipped;
                while(j<pix_tri_count && pix_tris[j].face==pix_tris[i].face && pix_tris[j].instance==pix_tris[i].instance) clipped|=pix_tris[j++].clipped;
                const pix_face_t *pf=pix_faces+pix_lookup[pix_tris[i].face];
                ++pix_frames_faces;
                if(clipped || j-i>2) ++pix_clipped;
                else {
                    pix_vertex_t tri[2][3];
                    for(unsigned t=0;t<j-i;++t) memcpy(tri[t],pix_tris[i+t].v,sizeof tri[t]);
                    pix_compare(pf,(const pix_vertex_t (*)[3])tri,j-i,pix_tris[i].w,pix_tris[i].h);
                }
                i=j;
            }
        }
        for(unsigned c=0;c<SIM_CLASSES;++c) for(unsigned k=0;k<4;++k) {
            sim_class_total[c][k]+=frame_class[c][k]; if(frame_class[c][k]>sim_class_max[c][k]) sim_class_max[c][k]=frame_class[c][k]; }
        if(class_frames) {
            /* tick, per class vertices triangles strips headers, then per source (cars, world) strip runs, their faces and vertices this frame */
            static uint64_t runs_before[2],faces_before[2],vertices_before[2];
            fprintf(class_frames,"%u",head[1]);
            for(unsigned c=0;c<SIM_CLASSES;++c) fprintf(class_frames," %llu %llu %llu %llu",frame_class[c][0],frame_class[c][1],frame_class[c][2],frame_class[c][3]);
            for(unsigned k=0;k<2;++k) {
                fprintf(class_frames," %llu %llu %llu",(unsigned long long)(sq_flat_runs[k]-runs_before[k]),(unsigned long long)(sq_flat_run_faces[k]-faces_before[k]),
                    (unsigned long long)(sq_flat_run_vertices[k]-vertices_before[k]));
                runs_before[k]=sq_flat_runs[k]; faces_before[k]=sq_flat_run_faces[k]; vertices_before[k]=sq_flat_run_vertices[k];
            }
            fputc('\n',class_frames);
        }
        qsort(sets,count,sizeof(set_t),set_compare); qsort(setwraps,count,sizeof(set_t),set_compare);
        fprintf(out," set=%016llx setwrap=%016llx",(unsigned long long)fnv(sets,count*sizeof(set_t),UINT64_C(1469598103934665603)),
            (unsigned long long)fnv(setwraps,count*sizeof(set_t),UINT64_C(1469598103934665603)));
        /* Every word handed to the PVR, headers included, in submission order. */
        fprintf(out," raw=%016llx words=%u\n",(unsigned long long)fnv(stream,(size_t)writes*32,UINT64_C(1469598103934665603)),writes*8);
        ++frames;
    }
    printf("stage %u, %ld frames; world textures resident %u = %u bytes; palette banks written %u of %u\n",stage_build,frames,world_textures,world_vram,palette_banks,NM_CAR_PALETTE_BANKS);
    printf("%-22s %10s %7s %10s %7s %10s %7s\n","per frame","cars mean","max","world mean","max","both mean","max");
    for(unsigned k=0;k<S_COUNT;++k)
        printf("%-22s %10.1f %7llu %10.1f %7llu %10.1f %7llu\n",stat_names[k],(double)total[0][k]/frames,(unsigned long long)maximum[0][k],
            (double)total[1][k]/frames,(unsigned long long)maximum[1][k],(double)both_total[k]/frames,(unsigned long long)both_max[k]);
    for(unsigned s=0;s<2;++s)
        printf("paths per frame, %s: point loop wide-shared %.2f batches (%.1f mesh vertices), per-vertex %.2f (%.1f); emitter production %.2f batch lists (%.1f faces, %.2f re-entries after a clipped face), "
               "general because unproven %.2f (%.1f faces), general for the object %.2f (%.1f faces), prebuilt stream %.2f batch lists\n",s ? "world" : "cars",
            (double)sim_path_points[s][0][0]/frames,(double)sim_path_points[s][0][1]/frames,(double)sim_path_points[s][1][0]/frames,(double)sim_path_points[s][1][1]/frames,
            (double)sim_path_emit[s][0][0]/frames,(double)sim_path_emit[s][0][1]/frames,(double)sim_path_emit[s][0][2]/frames,
            (double)sim_path_emit[s][1][0]/frames,(double)sim_path_emit[s][1][1]/frames,(double)sim_path_emit[s][2][0]/frames,(double)sim_path_emit[s][2][1]/frames,
            (double)sim_path_emit[s][3][0]/frames);
    printf("probes per frame: single faces emitted intact (fast path) %.1f, through the general loop %.1f, flat runs %.1f; queued faces with every triangle outside one plane, dropped in the production loop %.1f; rest of a batch proven again after a clipped face, inside the loop %.1f; strip runs tried and not possible %.1f, runs through the vertex table (rewritten u,v) %.1f\n",
        (double)sim_probe_emit_intact_faces/frames,(double)sim_probe_emit_general_faces/frames,(double)sim_probe_emit_runs/frames,(double)sim_probe_emit_outside_faces/frames,(double)sim_probe_emit_reproven/frames,
        (double)sim_probe_emit_runs_refused/frames,(double)sim_probe_emit_runs_dynamic/frames);
    printf("probes per frame: vertices on the wide path %.1f tested against near plane and guard band, %.1f proven inside (no test); classic bounded path %.1f, of them proven %.1f\n",
        (double)sim_wide_vertices[0]/frames,(double)sim_wide_vertices[1]/frames,(double)sim_bounded_vertices/frames,(double)sim_classic_proven/frames);
    printf("probes per frame: car batches proven inside %.1f, with point tests %.1f; world batches proven %.1f, with tests (per group) %.1f\n",
        (double)sim_probe_car_batches_proven/frames,(double)sim_probe_car_batches_tested/frames,(double)sim_probe_world_batches_proven/frames,(double)sim_probe_world_batches_tested/frames);
    printf("probes per frame: car faces lit %.1f; repeating an earlier (normal, material, side) of the instance %.1f; repeating a normal %.1f\n",
        (double)sim_probe_light_calls/frames,(double)sim_probe_light_repeats/frames,(double)sim_probe_light_normal_repeats/frames);
    printf("probes per frame: instance bounds planes prepared %.1f, taken from the previous instance (same matrix, focus and view) %.1f; "
        "decided without them (nm_bounds_quick_classify) %.1f, given by the scene (the pit crew's figures) %.1f\n",
        (double)sim_probe_planes_prepared/frames,(double)sim_probe_planes_shared/frames,(double)sim_probe_planes_quick/frames,
        (double)sim_probe_planes_hinted/frames);
    printf("probes per frame: batches proven inside the guard view without its planes (nm_bounds_quick_classify) %.1f\n",
        (double)sim_probe_guard_quick/frames);
    printf("world face sides per frame: %.1f faces need no side (two-sided, both sides one binding), %.1f take the side test (one-sided faces for culling, two-sided faces with two lumas)\n",
        (double)draw_side_same/frames,(double)draw_side_tested/frames);
    printf("probes: bounded vertex path %llu of %llu transformed vertices\n",sim_bounded_vertices,sim_all_vertices);
    if(cull_faces[0]+cull_faces[1])
        printf("stream side decisions per frame: %.1f one-sided faces, %.1f two-luma faces; one-sided faces the CPU culls %.1f (%.1f vertices, %.1f triangles); "
               "winding of the one-sided faces' projected triangles (screen area >0/<0/0): shown %.1f/%.1f/%.1f, culled %.1f/%.1f/%.1f; faces not wholly projected %.1f\n",
            (double)cull_faces[0]/frames,(double)cull_faces[1]/frames,(double)cull_rear/frames,(double)cull_rear_vertices/frames,(double)cull_rear_triangles/frames,
            (double)cull_winding[0][0]/frames,(double)cull_winding[0][1]/frames,(double)cull_winding[0][2]/frames,
            (double)cull_winding[1][0]/frames,(double)cull_winding[1][1]/frames,(double)cull_winding[1][2]/frames,(double)cull_unprojected/frames);
#if defined(SIM_HAS_STREAM) && SQ_STREAMS
    if(sim_stream_events[SQ_STREAM_LISTS]) {
        static const char *names[SQ_STREAM_EVENTS]={"group-not-drawn","culled-rear","side-tests","two-luma-rear","hot-triangles","hot-quads",
            "hot-headers","helper-faces","helper-single","runs","run-faces","run-vertices","clipped","outside-dropped","reference-sides","stretches","lists",
            "marked-entries","groups-marked","lod-ranges-marked","batches","lean-transforms"};
        printf("stream events per frame:");
        for(unsigned e=0;e<SQ_STREAM_EVENTS;++e) printf(" %s=%.1f",names[e],(double)sim_stream_events[e]/frames);
        putchar('\n');
    }
#endif
#if defined(SIM_HAS_CAR_STREAM) && SQ_CAR_STREAMS
    if(sim_car_events[SQ_CAR_LISTS]) {
        static const char *names[SQ_CAR_EVENTS]={"side-tests","culled-rear","reference-sides","class-lights","face-lights","hot-triangles","hot-quads",
            "hot-headers","helper-faces","helper-single","runs","run-faces","run-vertices","rewritten-uv-faces","lists","batches","lean-transforms","whole-transforms"};
        printf("car stream events per frame:");
        for(unsigned e=0;e<SQ_CAR_EVENTS;++e) printf(" %s=%.1f",names[e],(double)sim_car_events[e]/frames);
        putchar('\n');
    }
#endif
    if(cull_faces[0]+cull_faces[1])
        printf("stream side decisions in all: one-sided %llu, two-luma %llu, culled %llu; triangle windings shown >0/<0/0 %llu/%llu/%llu, culled >0/<0/0 %llu/%llu/%llu\n",
            cull_faces[0],cull_faces[1],cull_rear,cull_winding[0][0],cull_winding[0][1],cull_winding[0][2],cull_winding[1][0],cull_winding[1][1],cull_winding[1][2]);
#if defined(SIM_HAS_FACING) && defined(NATIVE_LIGHT_VERIFY)
    printf("face side checks: car front/rear/reference=%llu/%llu/%llu, world front/rear/reference=%llu/%llu/%llu; largest share of the margin used %.5f\n",
        (unsigned long long)sq_facing_decided[0][0],(unsigned long long)sq_facing_decided[0][1],(unsigned long long)sq_facing_reference[0],
        (unsigned long long)sq_facing_decided[1][0],(unsigned long long)sq_facing_decided[1][1],(unsigned long long)sq_facing_reference[1],(double)sq_facing_worst);
#endif
    if(lod_count) {
        static const char *names[2]={"cutouts","fence panels"};
        for(unsigned c=0;c<2;++c) if(lod_walked[c])
            printf("level of detail, %s: groups walked per frame %.1f (max %u); at level 0/1/2 %.1f%% / %.1f%% / %.1f%%; level changes per frame %.3f (max %u), %llu in all; "
                "of them pops (the group was walked in the frame before and has a triangle in this frame or had one in the frame before) %.3f per frame (max %u), %llu in all, "
                "among %.1f such groups per frame\n",
                names[c],(double)lod_walked[c]/frames,lod_walked_max[c],100.0*lod_at_level[c][0]/lod_walked[c],100.0*lod_at_level[c][1]/lod_walked[c],
                100.0*lod_at_level[c][2]/lod_walked[c],(double)lod_changes[c]/frames,lod_changes_max[c],lod_changes[c],
                (double)lod_pops[c]/frames,lod_pops_max[c],lod_pops[c],(double)lod_running[c]/frames);
        if(lod_out) fclose(lod_out);
        if(sq_sweep_planes) printf("fence sweep: panels across a sweep plane (drawn as clipped bands) per frame %.3f (max %u), in %llu of %ld frames; "
            "%llu band polygons; %llu decisions needed the corners (%.2f per frame)\n",(double)lod_straddling/frames,lod_straddling_max,
            lod_straddling_frames,(long)frames,sim_sweep_polygons,sim_sweep_corner_tests,(double)sim_sweep_corner_tests/frames);
        if(oracle_frames) printf("level-of-detail reference: every walked group's level follows from its level before and its switch depths (hysteresis %g); "
            "only the faces of that range were compared\n",(double)NM_LOD_HYSTERESIS);
    }
    printf("SH4 model (SIM_SH4=%s): %llu FIPR and %llu FSRRA results moved to their error bound; renderer built with NM_SH4ZAM_GFX=%d\n",
        sim_sh4_name(),sim_sh4_fipr,sim_sh4_fsrra,(int)NM_SH4ZAM_GFX);
    printf("clipped triangles per frame: %.2f near plane only, %.2f general clipper (%llu, %llu in all)\n",
        (double)sim_clip_kind[0]/frames,(double)sim_clip_kind[1]/frames,sim_clip_kind[0],sim_clip_kind[1]);
    if(sim_light_evaluations) printf("car light classes against the exact form: %llu evaluations (%.1f per frame); mode 0/1 (specular) %llu/%llu; luma difference -2/-1/0/+1/+2 (beyond counted at the ends): %llu/%llu/%llu/%llu/%llu; side or invalid differences %llu\n",
        sim_light_evaluations,(double)sim_light_evaluations/frames,sim_light_mode[0],sim_light_mode[1],sim_light_delta[0],sim_light_delta[1],sim_light_delta[2],sim_light_delta[3],sim_light_delta[4],sim_light_side);
    if(oracle_frames) {
        static const char *field[5]={"x (pixels)","y (pixels)","u (texels)","v (texels)","z (relative)"};
        printf("reference tolerances used: %llu lit car triangles, %llu drawn with the binding of luma L-1 and %llu of luma L+1 (%llu face instances); "
            "%llu clipped polygons (%llu triangles, %llu vertices, %llu of them bit for bit; %llu polygons starting at another vertex, %llu with a zero-length edge dropped; %llu submitted off-screen slivers without a reference polygon)\n",
            nm_reference_tolerance.lit_triangles,nm_reference_tolerance.neighbour_triangles[0],
            nm_reference_tolerance.neighbour_triangles[1],nm_reference_tolerance.neighbour_faces,nm_reference_tolerance.clipped_polygons,nm_reference_tolerance.clipped_triangles,
            nm_reference_tolerance.clipped_vertices,nm_reference_tolerance.clipped_identical,nm_reference_tolerance.rotated_polygons,nm_reference_tolerance.reduced_polygons,nm_reference_tolerance.extra_polygons);
        for(unsigned f=0;f<5;++f) {
            printf("  clipped %-10s max %.3g; |difference| 0 / <2^-20 / <2^-16 / <2^-12 / <2^-8 / <1/64 / <1/16 / beyond:",field[f],nm_reference_tolerance.max[f]);
            for(unsigned b=0;b<NM_REFERENCE_BUCKETS;++b) printf(" %llu",nm_reference_tolerance.histogram[f][b]);
            putchar('\n');
        }
    }
    if(oracle_frames) printf("independent reference (tests/reference_stream.h): %llu frames, %llu instances; %llu submitted triangles each equal to the next reference triangle "
        "of their instance and list (three vertices x,y,z,u,v bit for bit, colour, binding, header; within the stated tolerances: the luma of a lit car face one step, "
        "a clipped triangle as a polygon up to its first vertex, x and y %g pixel, u and v %g texel, z %g relative); %llu of %llu reference triangles not submitted, none touching the screen; 0 mismatches\n",
        oracle_frames,oracle_instances,oracle_matched,NM_REFERENCE_CLIP_PIXELS,NM_REFERENCE_CLIP_TEXELS,NM_REFERENCE_CLIP_DEPTH,oracle_skipped,oracle_reference_total);
    if(oracle_frames && oracle_ss_state)
        printf("object super-sampling by instance (reference): %llu instances drawn super-sampled in %llu frames, %llu plain; %llu triangles "
            "under super-sampled headers, each the reference's decision (model origin against the switch depth, hysteresis); 0 mismatches\n",
            ss_batches[1],ss_frames,ss_batches[0],ss_triangles);
#if !NM_SMOKE_PUNCH
    if(oracle_frames) printf("translucent list (car effects' smoke%s): %llu frames with faces, %llu instances, %llu faces, %llu triangles (at most %llu a frame); "
        "%llu faces in the reference's back-to-front order (nearest view-space depth, largest first); 0 out of order\n",
        NM_SMOKE_MODE==3 ? ", tr-screen: one depth per face, u = u/2 + (x+y)/2 up to a whole number" : "",
        tr_frames,tr_instances,tr_faces,tr_triangles_total,tr_triangles_max,tr_ordered);
#else
    if(oracle_frames) printf("translucent list (car effects' smoke): %s: %llu frames with faces, %llu instances, %llu faces; punch-through, "
        "not sorted; every triangle as the reference's%s\n",NM_SMOKE_MODE==1 ? "pt-tex" : "pt-screen",tr_frames,tr_instances,tr_faces,
        NM_SMOKE_MODE==2 ? " (one depth per face, u = u/2 + (x+y)/2 up to a whole number)" : "");
#endif
    if(tr_frames) printf("translucent SH4 work per frame with faces: %.1f vertices transformed (at most %llu), %.1f faces lit and sorted (at most %llu)\n",
        (double)tr_vertices/tr_frames,tr_vertices_max,(double)tr_faces/tr_frames,tr_faces_max);
    if(object_reference_frames) printf("world object reference (tests/reference_stream.h): %llu frames, the object instances of each equal to what the pack's object words, "
        "the frame's cell bitmaps and the race state give; %llu instances drawn in %llu frames; %u at a bearing boundary (either neighbour accepted), %u bearing models not judged (crash camera); 0 mismatches\n",
        object_reference_frames,object_checked,object_frames,object_boundary,object_unjudged);
    printf("stream check: every vertex finite, depth positive, inside the guard band, flags valid; farthest from the screen centre x %.1f y %.1f, largest depth %g\n",
        (double)stream_reach[0],(double)stream_reach[1],(double)stream_reach[2]);
    printf("smallest written depth: cars %g, road surface %g, other scenery %g; sky is at %g (must stay below all three)\n",
        (double)min_written[0],(double)min_written[1],(double)min_written[2],(double)NM_SKY_DEPTH);
    if(pix_faces) printf("pixel check (SIM_PIXEL_FACES, %u faces): %llu drawn face instances, %llu compared whole (%llu clipped, not compared; %llu without a shared diagonal), "
        "%llu pixels; differing: coverage %llu, depth %llu, texel %llu, sample position beyond 1/64 texel %llu; face instances with a difference %llu\n",
        pix_face_count,pix_frames_faces,pix_checked,pix_clipped,pix_odd,pix_pixels,pix_coverage,pix_depth,pix_texel,pix_subtexel,pix_faces_differing);
    printf("TA by class, per frame (mean/max): vertices, triangles, vertices per triangle, strips, headers\n");
    for(unsigned c=0;c<SIM_CLASSES;++c) if(sim_class_total[c][0])
        printf("  %-13s vertices %8.1f/%5llu triangles %8.1f/%5llu  %.3f a triangle  strips %7.1f  headers %6.1f\n",sim_class_names[c],
            (double)sim_class_total[c][0]/(double)frames,sim_class_max[c][0],(double)sim_class_total[c][1]/(double)frames,sim_class_max[c][1],
            (double)sim_class_total[c][0]/(double)(sim_class_total[c][1] ? sim_class_total[c][1] : 1),(double)sim_class_total[c][2]/(double)frames,
            (double)sim_class_total[c][3]/(double)frames);
    printf("flat-shaded runs: cars %llu (%llu faces), world %llu (%llu faces)\n",(unsigned long long)sq_flat_runs[0],(unsigned long long)sq_flat_run_faces[0],
        (unsigned long long)sq_flat_runs[1],(unsigned long long)sq_flat_run_faces[1]);
    if(depth_total.frames) {
        printf("depth buffer, %ld frames at %dx%d: %llu covered pixels; written winner is the true nearest in %llu\n",depth_total.frames,DW,DH,
            (unsigned long long)depth_total.pixels,(unsigned long long)depth_total.same);
        printf("  layered (decal) pixels %llu: decal wins over its coplanar base %llu, base wins although a higher layer lies in the same plane %llu\n",
            (unsigned long long)depth_total.layered_pixels,(unsigned long long)depth_total.decal_by_layer,(unsigned long long)depth_total.decal_lost);
        printf("  car wins over road surface that is truly nearer (by design) %llu, largest true-depth ratio %.3f\n",
            (unsigned long long)depth_total.car_over_surface,depth_total.car_ratio_max);
        printf("  other scenery wins over road surface that is truly nearer %llu (more than 2%% behind: %llu), largest ratio %.3f\n",
            (unsigned long long)depth_total.scenery_over_surface,(unsigned long long)depth_total.scenery_over_surface_clear,depth_total.scenery_ratio_max);
        printf("  equal-depth ties resolved by order %llu, other differences %llu\n",(unsigned long long)depth_total.tie,(unsigned long long)depth_total.other);
        { static const char *kind[7]={"car","reduced","opaque","ground","cutout","checker","objects"};
          puts("  differences by (winner over true nearest), winner unlayered/layered, true-depth ratio <1.001 / <1.01 / <1.1 / more:");
          for(unsigned a=0;a<7;++a) for(unsigned b=0;b<7;++b) for(unsigned l=0;l<2;++l) {
              const uint64_t *d=depth_detail[a][b][l];
              if(d[0]+d[1]+d[2]+d[3]) printf("    %-7s over %-7s %s: %llu / %llu / %llu / %llu\n",kind[a],kind[b],l ? "layered  " : "unlayered",
                  (unsigned long long)d[0],(unsigned long long)d[1],(unsigned long long)d[2],(unsigned long long)d[3]);
          } }
    }
    if(depth_total.frames) {
        static const char *kind[7]={"car","reduced","opaque","ground","cutout","checker","objects"};
        uint64_t all=0;
        puts("fill (covered pixel samples per sampled frame, share of the 640x480 window): kind OP / PT / TR");
        for(unsigned k=0;k<7;++k) { all+=fill_total[k][0]+fill_total[k][1]+fill_total[k][2];
            printf("  %-8s %10.0f (%.3f) / %10.0f (%.3f) / %10.0f (%.3f)\n",kind[k],
                (double)fill_total[k][0]/depth_total.frames,(double)fill_total[k][0]/depth_total.frames/(DW*DH),
                (double)fill_total[k][1]/depth_total.frames,(double)fill_total[k][1]/depth_total.frames/(DW*DH),
                (double)fill_total[k][2]/depth_total.frames,(double)fill_total[k][2]/depth_total.frames/(DW*DH)); }
        printf("  overdraw of models (sky excluded): %.3f windows per frame\n",(double)all/depth_total.frames/(DW*DH));
        printf("punch-through samples per frame: world mean %.0f worst %llu; world and cars mean %.0f worst %llu; "
               "of those not behind an opaque-list polygon mean %.0f worst %llu\n",
            (double)pt_world_sum/depth_total.frames,(unsigned long long)pt_world_max,(double)pt_sum/depth_total.frames,(unsigned long long)pt_max,
            (double)pt_open_sum/depth_total.frames,(unsigned long long)pt_open_max);
        if(getenv("SIM_FILL")) {
            FILE *f=fopen(getenv("SIM_FILL"),"w"); assert(f);
            fprintf(f,"%ld\n",depth_total.frames);
            for(unsigned i=0;i<(1u<<16);++i) if(seen_face[i]) fprintf(f,"%u %llu %llu %u\n",i,(unsigned long long)fill_face[i],(unsigned long long)win_face[i],seen_face[i]);
            fclose(f);
        }
    }
    if(pvr_out) { pvr_summary(); fclose(pvr_out); }
    if(getenv("SIM_DEPTH_PAIRS") && pair_table) {
        FILE *pairs=fopen(getenv("SIM_DEPTH_PAIRS"),"w"); assert(pairs);
        for(unsigned i=0;i<PAIR_SLOTS;++i) if(pair_table[i].key)
            fprintf(pairs,"%u %u %llu %.4f\n",(unsigned)(pair_table[i].key>>32)-1,(unsigned)(pair_table[i].key&0xffffffffu)-1,
                (unsigned long long)pair_table[i].pixels,(double)pair_table[i].ratio);
        fclose(pairs);
    }
    if(getenv("SIM_BINDINGS")) {
        FILE *dump=fopen(getenv("SIM_BINDINGS"),"w"); assert(dump);
        for(unsigned s=0;s<2;++s) for(unsigned i=0;i<mats[s].count;++i) if(use_triangles[s][i])
            fprintf(dump,"%u %u %llu %08x %08x %08x %08x\n",s,i,(unsigned long long)use_triangles[s][i],use_argb[s][i],
                canon[use_header[s][i]].cmd,canon[use_header[s][i]].mode1,canon[use_header[s][i]].mode2);
        fclose(dump);
    }
    if(road_check && !road_records_check(UINT_MAX)) return 1; /* the meshes that joined in the last frame */
    road_depth_summary(frames);
#if defined(SIM_HAS_STREAM) && SQ_STREAMS
    sq_stream_report();
#endif
    fclose(out); if(faces_out) fclose(faces_out);
    return 0;
}
