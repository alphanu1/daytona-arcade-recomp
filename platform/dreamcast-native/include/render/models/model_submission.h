/* Included by model_renderer.c: direct store-queue submission of cars and
 * scenery. Keeps source-point caches across OP/PT, never a frame of final PVR
 * vertices. The independent reference for this stream is host code:
 * tests/reference_stream.h, run by the submission simulator over whole
 * scripts and by tests/test_sq_quads.py on fixtures. */
#include <stddef.h>
#include "heap.h"
#include "nm_mem.h"
#include "scene_loader.h" /* nm_loader_yield: the load-time steps of a load in another screen's frames */
/* Renderer math in SH4ZAM's forms (FIPR dot products, the FSRRA reciprocal,
 * lerp): the car light classes (sq_class_light) and the triangle clipper
 * (sq_clip). They round differently from the exact forms, which the
 * reference renderer keeps (tests/reference_stream.h): the luma of a lit car
 * face may differ by one step and a clipped vertex by a fraction of a pixel
 * and of a texel; the simulator's reference check states and enforces those
 * tolerances and nothing else moves. make NATIVE_SH4ZAM_GFX=0 compiles the
 * forms before this switch (A/B; the stream is HEAD 744dd29's). Gameplay never
 * reads it. */
#ifndef NM_SH4ZAM_GFX
#define NM_SH4ZAM_GFX 1
#endif
typedef struct {
    const nm_mesh_t *mesh;
    nm_view_t view;
    nm_cached_vertex_t *points;
    uint32_t *bindings; /* face index in high half, material binding in low half */
    unsigned opaque_count, cutout_count;
    unsigned instance_index, source, object_batches;
    uint8_t bounds, prepared;
    uint8_t stream; /* SQ_BATCH_*: prepared for the prebuilt world stream (sq_stream_emit) */
    uint8_t sweep:1;       /* queue path: a catch-fence panel across a sweep plane was queued (sq_sweep_face) */
    uint8_t supersample:2; /* SQ_SS_*: an instance of a mesh with a super-sample switch depth (sq_ss_select) */
} nm_sq_batch_t;
#ifdef __DREAMCAST__
_Static_assert(sizeof(nm_sq_batch_t)==64,"two cache lines per model-part callback data");
#endif
/* Vertices per mesh the per-batch vertex marks (sq_marks) hold. */
#define SQ_MARK_VERTICES 16384u
static struct {
    nm_sq_batch_t *batches;
    nm_cached_vertex_t *points;
    uint32_t *bindings;
    unsigned batch_capacity, point_capacity, face_capacity, count;
} sq_sources[2];
static nm_scenery_camera_t sq_scenery_camera;
/* Translucent effects (the section "Translucent effects" below): per car mesh 1 when the mesh is one of the
 * translucent class (every face textured, texel 15 transparent, screen checker: material_pack.h,
 * nm_material_translucent), and this frame's visible instances of such meshes, which take no batch. */
/* The list they go to: translucent, or punch-through with make NATIVE_SMOKE_MODE=pt-tex|pt-screen (NM_SMOKE_MODE). */
#define SQ_TR_LIST (NM_SMOKE_PUNCH ? PVR_LIST_PT_POLY : PVR_LIST_TR_POLY)
/* make NATIVE_SMOKE_TIGHT=1 with pt-screen: tight smoke geometry ("Translucent effects" below, sq_tr_tight_prepare). */
#ifndef NM_SMOKE_TIGHT
#define NM_SMOKE_TIGHT 0
#endif
#if NM_SMOKE_TIGHT && !NM_SMOKE_DOOR
#error "NM_SMOKE_TIGHT needs the screen checker (NM_SMOKE_MODE 2 or 3)"
#endif
#define SQ_TR_INSTANCES_MAX 16u
#define SQ_TR_FACES_MAX 256u
static uint8_t *sq_tr_meshes;
static struct { uint16_t instance; uint8_t bounds; nm_view_t view; } sq_tr_queue[SQ_TR_INSTANCES_MAX];
static unsigned sq_tr_count;
static int sq_failed;
/* Host tools only (the simulator, the fixtures): the header each binding
 * submits, for sq_emit_lookup. The console compiles every header straight
 * into the header pool of its emission classes (sq_emit_records_build) and
 * leaves these NULL. */
static const pvr_poly_hdr_t **sq_shared_headers[2];

#ifdef NATIVE_BOUNDS_VERIFY
static void sq_verify_rejected_face(const nm_pack_t *pack, const nm_mesh_t *mesh,
        const nm_instance_t *inst, const nm_view_t *view, const nm_face_t *face) {
    nm_cached_vertex_t vertices[4];
    for(unsigned v=0;v<face->count;++v)
        cache_draw_vertex(pack,mesh,inst,view,face->indices[v],vertices+v,0);
    for(unsigned t=1;t+1<face->count;++t) {
        const nm_cached_vertex_t *points[3]={vertices,vertices+t,vertices+t+1};
        nm_screen_vertex_t output[NM_CLIP_VERTICES];
        if(nm_project_cached_triangle(view,points,output)) {
            puts("FAIL: prepared bounds rejected visible triangle"); abort();
        }
    }
}
#endif

/* The world's frame bound (nm_render_world_capacity); 0: every instance. */
static unsigned sq_world_point_bound, sq_world_face_bound;
static int sq_allocate(unsigned source, const nm_pack_t *pack) {
    if(sq_sources[source].batches)
        return sq_sources[source].points && sq_sources[source].bindings;
    /* Live headers/instances may be a selected subset. Size from the original
     * validated DNM tables. Materials have 32-byte stride, so the instance
     * table follows them directly in both legacy and aligned packs. */
    const nm_header_t *original=pack->data;
    const nm_instance_t *instances=(const void *)(pack->materials+original->materials);
    unsigned points=0,faces=0;
    for(unsigned i=0;i<original->instances;++i) {
        const nm_mesh_t *mesh=pack->meshes+instances[i].mesh;
        if(mesh->vertex_count>SQ_MARK_VERTICES) {
            printf("FAIL: a mesh of %u vertices; the renderer's vertex marks hold %u (SQ_MARK_VERTICES)\n",
                (unsigned)mesh->vertex_count,SQ_MARK_VERTICES);
            return 0;
        }
        points+=mesh->vertex_count; faces+=mesh->face_count; /* each below 2^16 a mesh, at most 2^16 instances */
    }
    /* A frame admits at most the bound's points and faces (sq_prepare_source refuses more). Expert's world has 76,201
     * points in all, its frame bound 17,339: the limit holds for what one frame admits. */
    if(source && sq_world_point_bound && sq_world_point_bound<points) points=sq_world_point_bound;
    if(source && sq_world_face_bound && sq_world_face_bound<faces) faces=sq_world_face_bound;
    if(points>65536 || faces>65536) return 0;
    /* Each admitted batch is fully initialized by sq_prepare_source. */
    sq_sources[source].batches=nm_heap_alloc(source ? "world batches" : "car batches",32,original->instances*sizeof(nm_sq_batch_t));
    sq_sources[source].points=nm_heap_alloc(source ? "world point cache" : "car point cache",32,points*sizeof(nm_cached_vertex_t));
    sq_sources[source].bindings=nm_heap_alloc(source ? "world face queues" : "car face queues",32,faces*sizeof(uint32_t));
    sq_sources[source].batch_capacity=original->instances;
    sq_sources[source].point_capacity=points;
    sq_sources[source].face_capacity=faces;
    if(!sq_sources[source].batches || !sq_sources[source].points || !sq_sources[source].bindings) return 0;
    for(unsigned v=0;v<points;++v) {
        if(!(v&2047u)) nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        nm_cached_set_outside(sq_sources[source].points+v,64);
    }
    printf("Direct SQ source%u capacity: batches=%u points=%u faces=%u bytes=%u\n",source,
        (unsigned)original->instances,points,faces,(unsigned)(original->instances*sizeof(nm_sq_batch_t)+
        points*sizeof(nm_cached_vertex_t)+faces*sizeof(uint32_t)));
    return 1;
}

#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
/* |value| <= 2^20, hence finite: one integer compare on the bits. */
static inline int sq_float_bounded(float value) {
    uint32_t bits;
    nm_memcpy(&bits,&value,sizeof(bits));
    return (bits&UINT32_C(0x7fffffff))<=UINT32_C(0x49800000);
}
/* |value| <= 2, the same way. */
static inline int sq_float_within_two(float value) {
    uint32_t bits;
    nm_memcpy(&bits,&value,sizeof(bits));
    return (bits&UINT32_C(0x7fffffff))<=UINT32_C(0x40000000);
}
/* Which side of a face the view shows, without transforming anything.
 *
 * The reference test is the sign of C = (M n).(M p + t): supplied normal n
 * and corner p (face index 2) through the instance matrix, two FTRVs and one
 * FIPR per face, after loading the corner from the vertex table. In exact
 * arithmetic that is n.G.p + n.w with G = M^T M and w = M^T t. Every instance
 * matrix of the game is a rotation and a translation (G = I within 1.2e-6 in
 * the recorded races and attract scene), so C is the face's own plane
 * constant d = n.p, a constant of the pack, plus n.w, where w is a constant
 * of the batch: A = d + n.w, one FIPR on data already at hand.
 * A is used only where it cannot disagree with C. With N = |n|1,
 * P >= |p|1 for every vertex of the mesh (from its bounds), T = |t|1,
 * mu = largest |entry| of M, gamma = sum of |G - I| over the nine entries,
 * and the project's 8 epsilon bound per FTRV row or FIPR:
 *   |C - (n.G.p + n.w)| <= 24 eps mu N (3 mu P + T)       both transforms and the dot
 *   |n.G.p - n.p|       <= gamma N P                      (computed gamma: + 216 eps mu^2)
 *   |A - (n.p + n.w)|   <= eps N (9 P + 17 mu T)          d, w, the dot and the sum
 * so |C - A| <= N K with
 *   K = eps (128 mu (mu P + T) + 32 P + 32 mu T) + (gamma + 256 eps mu^2) P,
 * which covers the three lines with a third to spare; it is raised by one
 * per cent and to at least 1e-20. A > N K means C > 0 (front), A < -N K means
 * C < 0 (rear), and |C| > N K / 101 in both cases. Anything else, or a batch
 * that fails sq_facing_batch, takes the reference test: the result is the
 * same on every face, only the work differs. Typical K: 0.012 for a car 250
 * units away, below 0.1 for the scenery; A is the distance of the eye from
 * the face's plane times |n|.
 * NATIVE_LIGHT_VERIFY evaluates the reference for every face decided this
 * way and fails on a different side or luma. */
typedef struct { float plane, reach; } nm_sq_face_plane_t; /* d and N (infinite: never decided) */
typedef struct { float eye[3], slack; } nm_sq_facing_t;    /* w and K */
static nm_sq_face_plane_t *sq_face_planes[2];
#ifdef NATIVE_LIGHT_VERIFY
static uint64_t sq_facing_decided[2][2], sq_facing_reference[2];
static float sq_facing_worst;
#endif
/* Per pack, once (after the scenery stage selection): plane constants, and
 * the checks that make the reference light unable to fail for bounded
 * matrices. Every supplied normal within 2 per component, and every material
 * finite within 2^20 with a specular control the reference accepts: then
 * |M n| <= 12, |dot with the light| <= 72, the specular base stays below
 * 2^11 and its eighth power below 2^88; no intermediate is infinite or NaN,
 * so face_light cannot return -1 and a rear face can be dropped unlit.
 * A pack that fails keeps the reference test everywhere (table NULL). */
/* Light classes of the lit pack (cars): faces of one mesh with the same
 * supplied normal (all 12 bytes) and the same material light alike on the
 * same side of one instance, so the light of a class is evaluated once per
 * instance and side and read back for its other faces (18 per cent of the
 * lit faces in the recorded races; the same normal under another material
 * is another class: the material's diffuse, ambient and specular terms enter
 * the luma; a material whose light saturates, sq_light_saturated, is one class
 * whatever the normals). sq_light_class: class per face, numbered per mesh.
 * sq_light_cache: per class and side, the batch stamp, the light and (car
 * face loop, sq_walk_car) the material binding that light selects: a repeat
 * reads neither the material nor the 256-entry luma row. */
typedef struct { uint16_t stamp[2]; int16_t light[2]; uint16_t binding[2], unused[2]; } nm_sq_light_t;
static uint16_t *sq_light_class;
static nm_sq_light_t *sq_light_cache;
static unsigned sq_light_stamp,sq_light_cache_size;
/* Light classes the cache holds at least: a mesh whose normals the scene
 * rewrites (sq_car_mesh_refresh: the player's damaged bodies) may have more
 * than any mesh of the pack as loaded. */
static unsigned sq_light_class_reserve;
/* A material whose light saturates at 255 whatever the normal and the side:
 * ambient 255 or more, diffuse not negative and, in mode 1, a specular scale
 * not negative. Both evaluations (lighting.h, sq_class_light) add a product
 * that is not negative (max(0, dot) times the diffuse, a specular term that is
 * zero or a square times the scale) to the ambient and clamp at 255; with the
 * bounded terms sq_facing_prepare requires before any class is used nothing is
 * NaN, so every face of such a material lights 255 at the front and 511 at
 * the rear. Its faces form one class per material (the pit crew's body parts:
 * ambient 255, up to 70 normals a mesh, 5 classes instead). */
static inline int sq_light_saturated(const nm_material_t *material,unsigned mode) {
    return material->ambient>=255.0f && material->diffuse>=0.0f && (!(mode&1u) || material->specular_scale>=0.0f);
}
/* The classes of one mesh into classes[face_first..]; *count their number.
 * 0: more than UINT16_MAX. */
static int sq_light_classes_mesh(const nm_pack_t *pack,const nm_mesh_t *mesh,uint16_t *classes,unsigned *count_out) {
    const nm_face_t *faces=pack->faces+mesh->face_first;
    unsigned count=0;
    for(unsigned f=0;f<mesh->face_count;++f) {
        /* Faces are sorted by material and normal: look back first. */
        unsigned found=UINT_MAX;
        const int saturated=sq_light_saturated(pack->materials+faces[f].material,mesh->mode);
        for(unsigned e=f;e-->0;)
            if(faces[e].material==faces[f].material &&
               (saturated || !memcmp(faces[e].normal,faces[f].normal,sizeof(faces[f].normal)))) {
                found=classes[mesh->face_first+e]; break;
            }
        if(found==UINT_MAX) { if(count==UINT16_MAX) return 0; found=count++; }
        classes[mesh->face_first+f]=(uint16_t)found;
    }
    *count_out=count;
    return 1;
}
static int sq_light_classes_prepare(const nm_pack_t *pack) {
    free(sq_light_class); free(sq_light_cache); sq_light_class=NULL; sq_light_cache=NULL; sq_light_stamp=0;
    const nm_header_t *original=pack->data;
    uint16_t *classes=nm_heap_alloc("car light classes",32,(original->faces ? original->faces : 1)*sizeof(*classes));
    if(!classes) return 0;
    unsigned most=sq_light_class_reserve>1 ? sq_light_class_reserve : 1;
    for(unsigned m=0;m<original->meshes;++m) {
        unsigned count;
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one mesh a step */
        if(!sq_light_classes_mesh(pack,pack->meshes+m,classes,&count)) { free(classes); return 1; }
        if(count>most) most=count;
    }
    sq_light_cache=nm_heap_zeroed("car light class cache",32,most*sizeof(*sq_light_cache));
    if(!sq_light_cache) { free(classes); return 0; }
    sq_light_cache_size=most;
    sq_light_class=classes;
    return 1;
}
/* The car face loop's view of a face, one cache line, built at load: before,
 * a walked face read three sequential streams (the 32-byte face, its plane
 * constants, its light class) and its material. plane and reach first: the
 * record is read as an nm_sq_face_plane_t by sq_face_side.
 * flags: what reject_draw_face and the list selection take from the face's
 * attribute word and its material's first header word. */
typedef struct {
    float plane, reach;
    float normal[3];
    uint16_t group, light_class, material;
    uint8_t flags, count;
    uint32_t unused;
} nm_sq_lit_face_t;
_Static_assert(sizeof(nm_sq_lit_face_t)==32 && offsetof(nm_sq_lit_face_t,plane)==offsetof(nm_sq_face_plane_t,plane) &&
    offsetof(nm_sq_lit_face_t,reach)==offsetof(nm_sq_face_plane_t,reach),"one cache line per lit face, plane constants first");
#define SQ_LIT_TWO_SIDED 1u /* attribute bit 17 */
#define SQ_LIT_DRAWN 2u     /* attribute bits 8-9 not both clear */
#define SQ_LIT_CHECKER 4u   /* material header bit 15 */
#define SQ_LIT_CUTOUT 8u    /* material header bits 13-14 both set: punch-through list */
static nm_sq_lit_face_t *sq_lit_faces;
static void sq_lit_faces_mesh(const nm_pack_t *pack,const nm_mesh_t *mesh,nm_sq_lit_face_t *table) {
    for(unsigned f=0;f<mesh->face_count;++f) {
        const nm_face_t *face=pack->faces+mesh->face_first+f;
        nm_sq_lit_face_t *out=table+mesh->face_first+f;
        const unsigned header=pack->materials[face->material].header[0];
        out->plane=sq_face_planes[0][mesh->face_first+f].plane; out->reach=sq_face_planes[0][mesh->face_first+f].reach;
        out->normal[0]=face->normal[0]; out->normal[1]=face->normal[1]; out->normal[2]=face->normal[2];
        out->group=face->group; out->light_class=sq_light_class[mesh->face_first+f]; out->material=face->material;
        out->flags=(uint8_t)((face->attribute&(1u<<17) ? SQ_LIT_TWO_SIDED : 0)|(face->attribute&(3u<<8) ? SQ_LIT_DRAWN : 0)|
            (header&0x8000 ? SQ_LIT_CHECKER : 0)|(((header>>13)&3)==3 ? SQ_LIT_CUTOUT : 0));
        out->count=face->count; out->unused=0;
    }
}
static int sq_lit_faces_prepare(const nm_pack_t *pack) {
    free(sq_lit_faces); sq_lit_faces=NULL;
    if(!sq_face_planes[0] || !sq_light_class) return 1;
    const nm_header_t *original=pack->data;
    nm_sq_lit_face_t *table=nm_heap_alloc("car lit face records",32,(original->faces ? original->faces : 1)*sizeof(*table));
    if(!table) return 0;
    for(unsigned m=0;m<original->meshes;++m) { nm_loader_yield(0); sq_lit_faces_mesh(pack,pack->meshes+m,table); }
    sq_lit_faces=table;
    return 1;
}
/* The plane constants of one mesh's faces; 0 when a supplied normal is not
 * within 2 per component (then the pack keeps the reference test). */
static int sq_face_planes_mesh(const nm_pack_t *pack,const nm_mesh_t *mesh,nm_sq_face_plane_t *table) {
    for(unsigned f=0;f<mesh->face_count;++f) {
        const nm_face_t *face=pack->faces+mesh->face_first+f;
        const float *n=face->normal,*p=pack->vertices[mesh->vertex_first+face->indices[2]].position;
        const float reach=fabsf(n[0])+fabsf(n[1])+fabsf(n[2]);
        if(!sq_float_within_two(n[0]) || !sq_float_within_two(n[1]) || !sq_float_within_two(n[2])) return 0;
        table[mesh->face_first+f].plane=n[0]*p[0]+n[1]*p[1]+n[2]*p[2];
        /* A normal too short for its transformed square to be safely
         * positive is never decided here. */
        table[mesh->face_first+f].reach=reach>=0x1p-10f ? reach*(1.0f+4*FLT_EPSILON) : HUGE_VALF;
    }
    return 1;
}
static int sq_facing_prepare(unsigned source,const nm_pack_t *pack) {
    free(sq_face_planes[source]); sq_face_planes[source]=NULL;
    if(!source) { free(sq_lit_faces); sq_lit_faces=NULL; }
    if(!source && !sq_light_classes_prepare(pack)) return 0;
    const nm_header_t *original=pack->data;
    for(unsigned m=0;m<original->materials;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one step */
        const nm_material_t *material=pack->materials+m;
        if(!sq_float_bounded(material->diffuse) || !sq_float_bounded(material->ambient) ||
           !sq_float_bounded(material->specular_scale) || material->specular_control>255) return 1;
    }
    nm_sq_face_plane_t *table=nm_heap_alloc(source ? "world face planes" : "car face planes",32,(original->faces ? original->faces : 1)*sizeof(*table));
    if(!table) return 0;
    for(unsigned m=0;m<original->meshes;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one mesh a step */
        if(!sq_face_planes_mesh(pack,pack->meshes+m,table)) { free(table); return 1; }
    }
    sq_face_planes[source]=table;
    return source ? 1 : sq_lit_faces_prepare(pack);
}
/* A car mesh whose vertex positions, supplied normals and bounds the scene
 * rewrote (render/scene/practice.c: the player's damaged bodies, same
 * topology, materials and u,v): its light classes, plane constants and hot
 * face records again, as the load made them. Nothing else of the renderer's
 * tables depends on positions or normals: emission faces and strip corners
 * hold position representatives and u,v, the prebuilt car streams the
 * representatives (the rewrite must keep which vertices share a position),
 * and the side-test slack and bounds classification read the mesh bounds
 * per batch. 0: more classes than the cache holds, or a normal beyond 2. */
static int sq_car_mesh_refresh(const nm_pack_t *pack,unsigned m) {
    const nm_header_t *original=pack->data;
    if(m>=original->meshes) return 0;
    const nm_mesh_t *mesh=pack->meshes+m;
    if(sq_light_class) {
        unsigned count;
        if(!sq_light_classes_mesh(pack,mesh,sq_light_class,&count) || count>sq_light_cache_size) return 0;
    }
    if(sq_face_planes[0] && !sq_face_planes_mesh(pack,mesh,sq_face_planes[0])) return 0;
    if(sq_lit_faces) sq_lit_faces_mesh(pack,mesh,sq_lit_faces);
    return 1;
}
/* w and K of one batch; 0 when the batch must use the reference test:
 * computed normals, a matrix that is not a rotation within 0.01 or has an
 * entry above 2, a light beyond 2 per component (lit: cars), or values out
 * of range.
 * Parts of one car share a matrix, the scenery shares the view: the terms
 * that depend on the matrix alone are kept from the previous batch. */
/* The bits of a float in memory as one integer load (no FPUL round trip, and no library memcmp/memcpy). */
typedef uint32_t __attribute__((may_alias)) nm_sq_word_t;
static inline __attribute__((always_inline)) uint32_t sq_word(const float *value) {
    return *(const nm_sq_word_t *)(const void *)value;
}
static int sq_facing_batch(nm_sq_facing_t *out,const nm_instance_t *inst,const nm_mesh_t *mesh,int lit) {
    alignas(32) static uint32_t cached_matrix[12]; /* the matrix's bits */
    static float cached_eye[3],cached_gamma,cached_mu,cached_reach;
    static int cached_valid,cached_ok;
    const float *m=inst->matrix;
    if(mesh->mode>1) return 0;
    if(lit) for(unsigned a=0;a<3;++a) if(!sq_float_within_two(inst->light[a])) return 0;
    /* A new matrix (word for word): the pit crew's parts each have their own. Compared and kept inline. */
    uint32_t differ=!cached_valid;
    for(unsigned i=0;i<12;++i) differ|=sq_word(m+i)^cached_matrix[i];
    if(differ) {
        for(unsigned i=0;i<12;++i) cached_matrix[i]=sq_word(m+i);
        cached_valid=1; cached_ok=0;
        float gamma=0,mu=0;
        for(unsigned i=0;i<12;++i) if((sq_word(m+i)&UINT32_C(0x7fffffff))>UINT32_C(0x49800000)) return 0; /* sq_float_bounded */
        for(unsigned i=0;i<9;++i) if(fabsf(m[i])>mu) mu=fabsf(m[i]);
        for(unsigned i=0;i<3;++i) {
            for(unsigned j=i;j<3;++j) {
                const float g=shz_dot6f(m[3*i],m[3*i+1],m[3*i+2],m[3*j],m[3*j+1],m[3*j+2]);
                gamma+=i==j ? fabsf(g-1.0f) : 2*fabsf(g);
            }
            cached_eye[i]=shz_dot6f(m[3*i],m[3*i+1],m[3*i+2],m[9],m[10],m[11]);
        }
        /* Finite sums of finite terms: plain compares, no unordered case. */
        if(mu>2 || gamma>0.01f || !nm_finite_float(gamma)) return 0;
        cached_gamma=gamma; cached_mu=mu; cached_reach=fabsf(m[9])+fabsf(m[10])+fabsf(m[11]);
        cached_ok=1;
    }
    if(!cached_ok) return 0;
    float extent=0;
    for(unsigned a=0;a<3;++a) {
        const float low=fabsf(mesh->bounds[a]),high=fabsf(mesh->bounds[a+3]);
        if(!sq_float_bounded(low) || !sq_float_bounded(high)) return 0;
        extent+=low>high ? low : high;
    }
    const float mu=cached_mu,reach=cached_reach;
    const float slack=(FLT_EPSILON*(128*mu*(mu*extent+reach)+32*extent+32*mu*reach)+
        (cached_gamma+256*FLT_EPSILON*mu*mu)*extent)*1.01f+1e-20f;
    if(!nm_float_positive(slack) || !nm_finite_float(slack)) return 0;
    out->eye[0]=cached_eye[0]; out->eye[1]=cached_eye[1]; out->eye[2]=cached_eye[2]; out->slack=slack;
    return 1;
}
/* +1 front (C > 0), -1 rear (C < 0), 0 undecided. */
static inline __attribute__((always_inline)) int sq_face_side(const nm_sq_facing_t *facing,
        const nm_sq_face_plane_t *plane,const float *n) {
    const float approximate=plane->plane+shz_dot6f(n[0],n[1],n[2],facing->eye[0],facing->eye[1],facing->eye[2]);
    const float margin=plane->reach*facing->slack;
    /* approximate > margin, approximate < -margin, as sign tests of a
     * difference (finite.h: no unordered compare on the SH4). An infinite
     * margin or a NaN gives neither; so does a difference the FPU flushes to
     * zero. */
    return nm_float_positive(approximate-margin) ? 1 : nm_float_negative(approximate+margin) ? -1 : 0;
}
#ifdef NATIVE_LIGHT_VERIFY
/* The reference facing of a decided face must have the decided sign. */
static int sq_face_side_verify(const nm_pack_t *pack,const nm_mesh_t *mesh,const nm_face_t *face,
        const nm_sq_facing_t *facing,const nm_sq_face_plane_t *plane,int side) {
    const float *n=face->normal;
    const float *p=pack->vertices[mesh->vertex_first+face->indices[2]].position;
    const shz_vec4_t point=shz_xmtrx_transform_vec4(shz_vec4_init(p[0],p[1],p[2],1));
    const shz_vec4_t normal=shz_xmtrx_transform_vec4(shz_vec4_init(n[0],n[1],n[2],0));
    const float reference=shz_dot6f(normal.x,normal.y,normal.z,point.x,point.y,point.z);
    const float approximate=plane->plane+shz_dot6f(n[0],n[1],n[2],facing->eye[0],facing->eye[1],facing->eye[2]);
    const float used=fabsf(reference-approximate)/(plane->reach*facing->slack);
    if(used>sq_facing_worst) sq_facing_worst=used;
    if(!nm_finite_float(reference) || (side>0 ? !(reference>0) : !(reference<0))) {
        printf("FAIL: face side from its plane %d, reference facing %g (plane value %g, margin %g)\n",
            side,(double)reference,(double)approximate,(double)(plane->reach*facing->slack));
        return 0;
    }
    return 1;
}
#endif
/* Reference light of a lit (car) face whose side is decided: the same
 * transformed normal and the same evaluation as face_light. The corner is not
 * transformed; the evaluation takes only the sign of its facing dot, and
 * gets one of that sign (plus or minus the transformed normal, whose square
 * is at least 2^-25 here). The product of the light dot and the facing dot,
 * whose sign selects the lit side, could underflow differently for a light
 * dot below 2^-60: such a face returns -2 and is lit by face_light. */
static inline int sq_sided_light_values(unsigned mode,const float *n,const nm_material_t *material,
        const nm_instance_t *inst,int rear) {
    const shz_vec4_t t=shz_xmtrx_transform_vec4(shz_vec4_init(n[0],n[1],n[2],0));
    const float normal[3]={t.x,t.y,t.z};
    float points[3][3]={{0}};
    const float dotl=shz_dot6f(normal[0],normal[1],normal[2],inst->light[0],inst->light[1],inst->light[2]);
    if(dotl!=0 && fabsf(dotl)<0x1p-60f) return -2;
    for(unsigned a=0;a<3;++a) points[2][a]=rear ? -normal[a] : normal[a];
#ifdef __DREAMCAST__
    return nm_face_lighting_eval(mode,normal,points,inst->light,material,1);
#else
    return nm_face_lighting(mode,normal,points,inst->light,material);
#endif
}
#if NM_SH4ZAM_GFX
/* The light of a car face class whose side is decided, SH4ZAM form (the form
 * above, sq_sided_light_values, is NATIVE_SH4ZAM_GFX=0's). lighting.h
 * transforms the supplied normal by the instance matrix M and dots it with
 * the light L. (M n).L = n.(M^T L): with the light carried into model space
 * once per batch or list callback (sq_light_terms, three FIPRs), a class
 * costs one FIPR of its own normal and no FTRV. The specular term reads only
 * the z of the transformed normal, (M n).z = n.(m2, m5, m8): a second FIPR,
 * mode 1 only. The facing dot of lighting.h (transformed normal against
 * corner 2) only selects the lit side through its sign, and the side is
 * decided (sq_face_side): its luminance is max(0, dotl) in front, max(0,
 * -dotl) at the rear, with no product that could underflow (so no -2 and no
 * face sent to face_light for a small light dot). Inputs are bounded
 * (sq_facing_prepare, sq_facing_batch: normals and light within 2 per
 * component, matrix entries within 2, materials within 2^20, specular base
 * below 2^11 and its eighth power below 2^88): no value can be infinite or
 * NaN, so nothing is tested. The result rounds differently from lighting.h's
 * (other operations, and FIPR's own precision on the SH4: within 8 epsilon of
 * the term magnitudes): a luma within one step of it, the rear bit equal.
 * The reference check (tests/reference_stream.h) accepts the bindings of
 * luma L-1, L, L+1 for a lit car face and counts every change of binding. */
/* FIPR of this switch's paths (shz_dot6f); host tools may model its error
 * here alone (the simulator's SIM_SH4=bound). */
#ifndef SQ_FIPR6
#define SQ_FIPR6 shz_dot6f
#endif
typedef struct { float light[3], axis_z[3], light_z; } nm_sq_light_terms_t;
static inline void sq_light_terms(nm_sq_light_terms_t *out,const nm_instance_t *inst) {
    const float *const m=inst->matrix,*const l=inst->light;
    for(unsigned c=0;c<3;++c) out->light[c]=SQ_FIPR6(m[3*c],m[3*c+1],m[3*c+2],l[0],l[1],l[2]);
    out->axis_z[0]=m[2]; out->axis_z[1]=m[5]; out->axis_z[2]=m[8];
    out->light_z=l[2];
}
static inline __attribute__((always_inline)) int sq_class_light(unsigned mode,const float *n,const nm_material_t *material,
        const nm_sq_light_terms_t *terms,int rear) {
    const float dotl=SQ_FIPR6(n[0],n[1],n[2],terms->light[0],terms->light[1],terms->light[2]);
    const float shown=rear ? -dotl : dotl;
    float luminance=(nm_float_positive(shown) ? shown : 0.0f)*material->diffuse+material->ambient;
    if(mode&1) {
        const unsigned control=material->specular_control;
        const float z=SQ_FIPR6(n[0],n[1],n[2],terms->axis_z[0],terms->axis_z[1],terms->axis_z[2]);
        float specular=(2*dotl)*z-terms->light_z;
        if(specular<0 || control==0) specular=0;
        if(control>>1) specular*=specular;
        if(control>>2) specular*=specular;
        if((control+1)>>3) specular*=specular;
        luminance+=specular*material->specular_scale;
    }
    if(luminance<0) luminance=0;
    if(luminance>255) luminance=255;
    return (int)luminance|(rear ? 256 : 0);
}
/* Two evaluations of one face agree within the SH4ZAM form's tolerance:
 * the same side, lumas at most one step apart (NATIVE_LIGHT_VERIFY). */
static inline int sq_light_close(int a,int b) {
    return a>=0 && b>=0 && !((a^b)&256) && (a&255)-(b&255)<=1 && (b&255)-(a&255)<=1;
}
/* Before the first frame (nm_render_init): sq_class_light against the exact
 * form (lighting.h on the normal transformed in scalar float arithmetic, the
 * decided side's facing, as the reference renderer has it) on fixed cases:
 * rotations about each axis and a compound one, the race light and two
 * others, eleven normals, both sides, modes 0 and 1, every specular control.
 * Each must be within one luma step with the same side. */
static __attribute__((noinline,unused)) int sq_class_light_check(void) {
    static const float rotations[5][9]={
        {1,0,0, 0,1,0, 0,0,1},
        {0.8660254f,0,-0.5f, 0,1,0, 0.5f,0,0.8660254f},
        {1,0,0, 0,0.70710678f,0.70710678f, 0,-0.70710678f,0.70710678f},
        {0.9396926f,0.3420201f,0, -0.3420201f,0.9396926f,0, 0,0,1},
        {0.6123724f,0.6123724f,-0.5f, -0.3535534f,0.7071068f,0.6123724f, 0.7071068f,-0.3535534f,0.6123724f}};
    static const float lights[3][3]={{-0.45f,-0.89f,-0.45f},{0.3f,-0.95f,0.1f},{-1.5f,0.25f,1.75f}};
    static const float normals[11][3]={{1,0,0},{0,1,0},{0,0,1},{-1,0,0},{0,-1,0},{0,0,-1},{0.57735f,0.57735f,0.57735f},
        {-0.2f,0.9f,-0.38729833f},{0.6f,-0.48f,0.64f},{1.9f,-0.3f,0.2f},{-0.05f,0.0125f,0.03f}};
    static const float terms[3][3]={{80,3.75f,4},{200,20,30},{31.5f,0,250}};
    unsigned cases=0,equal=0;
    for(unsigned r=0;r<5;++r) for(unsigned l=0;l<3;++l) {
        nm_instance_t inst; nm_memset(&inst,0,sizeof(inst));
        nm_memcpy(inst.matrix,rotations[r],sizeof(rotations[r]));
        nm_memcpy(inst.light,lights[l],sizeof(lights[l]));
        nm_sq_light_terms_t light_terms;
        sq_light_terms(&light_terms,&inst);
        for(unsigned n=0;n<11;++n) for(unsigned t=0;t<3;++t) for(unsigned control=0;control<8;++control) for(unsigned mode=0;mode<2;++mode) for(int rear=0;rear<2;++rear) {
            if(!mode && control) continue;
            nm_material_t material; nm_memset(&material,0,sizeof(material));
            material.diffuse=terms[t][0]; material.ambient=terms[t][1]; material.specular_scale=terms[t][2]; material.specular_control=control;
            const float *const m=inst.matrix,*const nn=normals[n];
            float normal[3],points[3][3]={{0}};
            for(unsigned a=0;a<3;++a) normal[a]=nn[0]*m[a]+nn[1]*m[3+a]+nn[2]*m[6+a];
            for(unsigned a=0;a<3;++a) points[2][a]=rear ? -normal[a] : normal[a];
            const int exact=nm_face_lighting(mode,normal,points,inst.light,&material);
            const int fast=sq_class_light(mode,nn,&material,&light_terms,rear);
            ++cases; equal+=exact==fast;
            if(!sq_light_close(exact,fast)) {
                printf("FAIL: SH4ZAM class light %d, exact form %d (rotation %u light %u normal %u terms %u control %u mode %u rear %d)\n",
                    fast,exact,r,l,n,t,control,mode,rear);
                return 0;
            }
        }
    }
    printf("Native SH4ZAM class light PASS: %u fixed cases within one luma step of the exact form, same side; %u equal\n",cases,equal);
    return 1;
}
#define SQ_LIGHT_SAME(a,b) sq_light_close((a),(b))
static inline int sq_sided_light(const nm_pack_t *pack,const nm_mesh_t *mesh,
        const nm_instance_t *inst,const nm_face_t *face,int rear) {
    nm_sq_light_terms_t terms;
    sq_light_terms(&terms,inst);
    return sq_class_light(mesh->mode,face->normal,&pack->materials[face->material],&terms,rear);
}
#else
#define SQ_LIGHT_SAME(a,b) ((a)==(b))
static inline int sq_sided_light(const nm_pack_t *pack,const nm_mesh_t *mesh,
        const nm_instance_t *inst,const nm_face_t *face,int rear) {
    return sq_sided_light_values(mesh->mode,face->normal,&pack->materials[face->material],inst,rear);
}
#endif
/* Host tools: every class light evaluated, against the exact form (the simulator's luma statistics). */
#ifndef SQ_CLASS_LIGHT_PROBE
#define SQ_CLASS_LIGHT_PROBE(mode,normal,material,inst,rear,light) ((void)0)
#endif
#endif

/* Baked scenery (DNM6 NM_PACK_BAKED_LIGHT, nm_render_world_attach): the
 * material binding of each live world face for the side its normal points to
 * and, two-sided faces, for the other side; back is UINT16_MAX for a
 * one-sided face. Indexed like the face table. NULL only in host fixtures
 * without a baked world: their faces take the lit path. */
typedef struct { uint16_t front, back; } nm_baked_face_t;
static nm_baked_face_t *sq_baked_faces __attribute__((unused));
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
/* Binding of the side the view shows; -1 culled (rear of a one-sided face),
 * -2 invalid arithmetic. No light: the luma is in the binding. A face whose
 * two sides agree needs no facing either. Facing is the original view-space
 * test (transformed supplied normal against transformed corner 2). */
static inline int sq_baked_binding(const nm_pack_t *pack,const nm_mesh_t *mesh,
        const nm_face_t *face,nm_baked_face_t baked,
        const nm_sq_facing_t *terms,const nm_sq_face_plane_t *plane) {
    /* Two-sided faces with one luma (every tree) and nothing else: no side
     * is chosen for them. The counters are host statistics (SQ_COUNT). */
    if(baked.back==baked.front) { SQ_COUNT(draw_side_same,1); return baked.front; }
    SQ_COUNT(draw_side_tested,1);
    const float *n=face->normal;
    /* The side from the face's own plane where that is beyond doubt
     * (sq_face_side): the same answer as the transforms below, without them
     * and without reading the corner. */
    const int side=terms ? sq_face_side(terms,plane,n) : 0;
#ifdef NATIVE_LIGHT_VERIFY
    if(side && !sq_face_side_verify(pack,mesh,face,terms,plane,side)) return -2;
    if(side) ++sq_facing_decided[1][side<0]; else ++sq_facing_reference[1];
#endif
    if(side>0) return baked.front;
    if(side<0) return baked.back==UINT16_MAX ? -1 : baked.back;
    const float *p=pack->vertices[mesh->vertex_first+face->indices[2]].position;
    const shz_vec4_t point=shz_xmtrx_transform_vec4(shz_vec4_init(p[0],p[1],p[2],1));
    const shz_vec4_t normal=shz_xmtrx_transform_vec4(shz_vec4_init(n[0],n[1],n[2],0));
    const float facing=shz_dot6f(normal.x,normal.y,normal.z,point.x,point.y,point.z);
    if(!nm_finite_float(facing)) return -2;
    if(!(facing<0)) return baked.front;
    return baked.back==UINT16_MAX ? -1 : baked.back;
}
#endif

/* Frame-counter animations (include/clock_actors.h; render/scene/practice.c picks the frame, model_renderer.c
 * nm_render_world_clock_frame writes it): base meshes of the world pack whose positions, normals, u, v and bindings
 * the scene rewrites for the frame it draws. They stay off the prebuilt streams (whose records hold the u, v, sides
 * and bindings of the load), start no offline strip run (its vertex records hold the u, v of the load) and their
 * sides are always found by the reference test (the plane constants hold the positions of the load). The live
 * specular faces leave them out: a placed actor's mesh is lit by model_renderer.c nm_render_world_clock_light. */
static uint8_t *sq_clock_meshes; /* per world mesh: 1 for a clock group's base mesh; NULL without animations */
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
/* Live specular (meshpack.h NM_PACK_LIVE_SPECULAR: Expert's glass and metal).
 * lighting.h adds to a mode-1 face (2 dotl) n_z - l_z, squared by the
 * control, times the specular scale, with n and l in view space: n_z = n.a
 * and l_z = l.a, a = (m2, m5, m8) the view-space z axis of the world
 * instances' matrix (the view: a rigid turn, so dotl = n.l is a constant of
 * the face). The term is (R.a)^k scale with R = 2 (n.l) n - l: one dot a
 * class (faces of one material and one supplied normal) and frame. The pack
 * carries the lumas without it (face.reserved/padding); here they are kept as
 * floats before saturation, the arithmetic of prepare_face_order.py
 * static_lumas, so luma = trunc(min(255, base + term)) as the reference
 * (the order of the sums is lighting.h's; the dot rounds otherwise: within
 * one luma, as the cars' sq_class_light). sq_live_update rewrites, before
 * the world's batches are walked, the baked bindings of every live face and
 * its stream record (colour, header, both bindings). Its lumas' bindings come
 * from the material/luma rows kept for these materials alone (the world's
 * dense table is freed after attach); the export holds every luma the term
 * can reach (tools/export_models.hpp race_scenery_lumas), and the attach
 * fails otherwise. Live faces start no offline strip run (their strip
 * vertices carry a colour of the load) and are always side tested. */
typedef struct {
    float r[3], scale;      /* R of the class (world), the material's specular scale */
    float base[2];          /* front and back luminance without the term, before saturation */
    uint32_t row;           /* the material's first entry in sq_live_rows (256 bindings, one a luma) */
    uint16_t control;       /* specular control */
    uint8_t two, unused;    /* 1: two-sided */
    uint8_t low[2], high[2];/* reachable lumas a side */
    uint16_t binding[2];    /* this frame's front and back bindings */
    uint32_t stamp;         /* the update that evaluated it last */
} nm_sq_live_class_t;
typedef struct { uint32_t face, record, live_class; } nm_sq_live_face_t;
typedef struct { uint32_t first, count, stamp; } nm_sq_live_mesh_t; /* its live faces in sq_live_faces */
static nm_sq_live_class_t *sq_live_classes;
static nm_sq_live_face_t *sq_live_faces;
static nm_sq_live_mesh_t *sq_live_meshes;
static uint16_t *sq_live_rows;
static unsigned sq_live_class_count,sq_live_face_count,sq_live_stamp;
static unsigned sq_live_updates; /* frames updated (log) */
static inline int sq_live_material(const nm_material_t *m,unsigned mode) {
    return (mode&1u) && m->specular_control && m->specular_scale!=0.0f && m->ambient<255.0f;
}
static inline float sq_live_power(float s,unsigned control) {
    if(s<0 || !control) s=0;
    if(control>>1) s*=s;
    if(control>>2) s*=s;
    if((control+1)>>3) s*=s;
    return s;
}
static __attribute__((unused)) void sq_live_release(void) {
    free(sq_live_classes); free(sq_live_faces); free(sq_live_rows); free(sq_live_meshes);
    sq_live_classes=NULL; sq_live_faces=NULL; sq_live_rows=NULL; sq_live_meshes=NULL; sq_live_class_count=sq_live_face_count=0;
}
/* At attach, with the world's material/luma table: the classes, the faces and
 * the rows; marks the resource of every reachable binding in `used`. 0: a
 * reachable luma has no binding, or no memory. */
static __attribute__((unused)) int sq_live_prepare(const nm_pack_t *pack,const uint16_t *bindings,const nm_material_pack_t *materials,uint8_t *used) {
    sq_live_release();
    if(!(nm_pack_flags(pack)&NM_PACK_LIVE_SPECULAR)) return 1;
    unsigned faces=0,rows=0;
    uint16_t *const row_of=nm_calloc(pack->header->materials,sizeof(*row_of));
    if(!row_of) return 0;
    for(unsigned m=0;m<pack->header->meshes;++m) {
        const nm_mesh_t *const mesh=pack->meshes+m;
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one mesh a step */
        /* A frame-counter animation's or a placed actor's mesh: lit by the clock's frames (model_renderer.c). */
        if(sq_clock_meshes && sq_clock_meshes[m]) continue;
        for(unsigned f=0;f<mesh->face_count;++f) {
            const unsigned material=pack->faces[mesh->face_first+f].material;
            if(!sq_live_material(pack->materials+material,mesh->mode)) continue;
            ++faces;
            if(!row_of[material]) row_of[material]=(uint16_t)++rows;
        }
    }
    if(!faces) { free(row_of); puts("Native live specular: flagged pack without a live face"); return 1; }
    uint32_t *const maker=nm_calloc(faces,sizeof(*maker)); /* per class: the face that made it (its normal) */
    sq_live_faces=nm_heap_alloc("world live specular faces",32,faces*sizeof(*sq_live_faces));
    sq_live_classes=nm_heap_alloc("world live specular classes",32,faces*sizeof(*sq_live_classes));
    sq_live_rows=nm_heap_alloc("world live specular rows",32,rows*256u*sizeof(*sq_live_rows));
    sq_live_meshes=nm_calloc(pack->header->meshes,sizeof(*sq_live_meshes));
    if(!maker || !sq_live_faces || !sq_live_classes || !sq_live_rows || !sq_live_meshes) {
        free(maker); free(row_of); sq_live_release(); return 0;
    }
    for(unsigned m=0;m<pack->header->materials;++m)
        if(row_of[m]) nm_memcpy(sq_live_rows+(row_of[m]-1u)*256u,bindings+m*256u,256u*sizeof(*sq_live_rows));
    const float *const l=nm_race_world_light;
    unsigned classes=0,exact=0,near=0,count=0;
    for(unsigned m=0;m<pack->header->meshes;++m) {
        const nm_mesh_t *const mesh=pack->meshes+m;
        nm_loader_yield(0); /* one mesh a step */
        if(sq_clock_meshes && sq_clock_meshes[m]) continue;
        for(unsigned f=0;f<mesh->face_count;++f) {
            if(!(f&15u)) nm_loader_yield(0); /* and some faces a step: the class search grows with the classes */
            const unsigned absolute=mesh->face_first+f;
            const nm_face_t *const face=pack->faces+absolute;
            const nm_material_t *const material=pack->materials+face->material;
            if(!sq_live_material(material,mesh->mode)) continue;
            if(!sq_live_meshes[m].count) sq_live_meshes[m].first=count;
            ++sq_live_meshes[m].count;
            const uint32_t row=(row_of[face->material]-1u)*256u;
            const uint8_t two=(face->attribute&(1u<<17))!=0;
            unsigned c=0;
            while(c<classes && !(sq_live_classes[c].row==row && sq_live_classes[c].two==two &&
                  !memcmp(pack->faces[maker[c]].normal,face->normal,sizeof(face->normal)))) ++c;
            if(c==classes) {
                nm_sq_live_class_t *const k=sq_live_classes+classes++;
                nm_memset(k,0,sizeof(*k));
                maker[c]=absolute;
                const float *const n=face->normal;
                /* prepare_face_order.py static_lumas: dotl=(n0 l0+n1 l1)+n2 l2, lit=|dotl| diffuse+ambient. */
                volatile float p0=n[0]*l[0],p1=n[1]*l[1],p2=n[2]*l[2];
                volatile float sum=p0+p1; volatile float dotl=sum+p2;
                volatile float product=fabsf(dotl)*material->diffuse; volatile float lit=product+material->ambient;
                const float dark=material->ambient;
                k->base[0]=dotl>0 ? lit : dark; k->base[1]=dotl<0 ? lit : dark;
                float reach=0;
                for(unsigned a=0;a<3;++a) { k->r[a]=(2*dotl)*n[a]-l[a]; reach+=k->r[a]*k->r[a]; }
                k->scale=material->specular_scale; k->control=(uint16_t)material->specular_control; k->two=two;
                k->row=row;
                /* The largest term: R.a is at most |R| for a unit a (the bound of the export, rounded up). */
                const float top=sq_live_power(shz_sqrtf(reach)*1.0001f,k->control)*k->scale+1.0f; /* a bound at load: SH4ZAM's root */
                for(unsigned side=0;side<(two ? 2u : 1u);++side) {
                    float lo=k->base[side],hi=k->base[side]+top;
                    if(lo<0) lo=0;
                    if(lo>255) lo=255;
                    if(hi>255) hi=255;
                    k->low[side]=(uint8_t)lo; k->high[side]=(uint8_t)hi;
                    for(unsigned luma=k->low[side];luma<=k->high[side];++luma) {
                        const uint16_t b=sq_live_rows[row+luma];
                        if(b==UINT16_MAX) {
                            printf("FAIL: live specular face %u (material %u, header %04x %04x %04x %04x): no binding for luma %u "
                                "(reachable %u..%u)\n",absolute,face->material,material->header[0],material->header[1],
                                material->header[2],material->header[3],luma,k->low[side],k->high[side]);
                            free(maker); free(row_of); sq_live_release(); return 0;
                        }
                        const nm_material_binding_t *const binding=materials->bindings+b;
                        if(binding->resource!=NM_MATERIAL_SOLID) used[binding->resource]=1;
                    }
                }
            }
            const nm_sq_live_class_t *const k=sq_live_classes+c;
            /* The bake agrees with the floats kept (one luma where the compiler fused differently). */
            const int front=(int)(k->base[0]<0 ? 0 : k->base[0]>255 ? 255 : k->base[0]);
            const int back=two ? (int)(k->base[1]<0 ? 0 : k->base[1]>255 ? 255 : k->base[1]) : face->padding;
            const int dfront=front-face->reserved,dback=back-face->padding;
            if(dfront<-1 || dfront>1 || dback<-1 || dback>1) {
                printf("FAIL: live specular face %u: baked lumas %u/%u, recomputed %d/%d\n",absolute,face->reserved,face->padding,front,back);
                free(maker); free(row_of); sq_live_release(); return 0;
            }
            if(!dfront && !dback) ++exact; else ++near;
            sq_live_faces[count++]=(nm_sq_live_face_t){absolute,UINT32_MAX,c};
        }
    }
    free(maker); free(row_of);
    sq_live_face_count=count; sq_live_class_count=classes; sq_live_updates=0; sq_live_stamp=0;
    printf("Native live specular: %u faces in %u classes of %u materials (bake equal %u, within one luma %u); tables=%u bytes\n",
        count,classes,rows,exact,near,(unsigned)(faces*(sizeof(*sq_live_faces)+sizeof(*sq_live_classes))+rows*256u*sizeof(*sq_live_rows)));
    return 1;
}
#endif


#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
/* One test per batch instead of fifteen finite tests per vertex. The loader
 * (code/meshpack.c) proved every vertex of the mesh inside mesh->bounds, so
 * with the matrix, the focus and those bounds within 2^20 each transformed
 * coordinate is a sum of three products below 2^40 and one term below 2^20:
 * finite and below 2^42, below 2^62 after the focus. geometry.h,
 * nm_cache_vertex_bounded, states what follows from that. A batch that fails
 * the test keeps the checked routine. */
static int sq_batch_bounded(const nm_instance_t *inst,const nm_mesh_t *mesh,const nm_view_t *view) {
    for(unsigned i=0;i<12;++i) if(!sq_float_bounded(inst->matrix[i])) return 0;
    for(unsigned i=0;i<6;++i) if(!sq_float_bounded(mesh->bounds[i])) return 0;
    for(unsigned i=0;i<4;++i) if(!sq_float_bounded(view->bounds[i])) return 0;
    return sq_float_bounded(inst->focus[0]) && sq_float_bounded(inst->focus[1]) &&
        sq_float_bounded(view->center[0]) && sq_float_bounded(view->center[1]) &&
        sq_float_bounded(view->near_z) && view->near_z>=0x1p-10f;
}
#define SQ_CACHE_VERTEX(index,out,proven) (wide ? \
    cache_draw_vertex_wide(pack,mesh,inst,&guard,(index),(out),(proven)) : bounded ? \
    cache_draw_vertex_bounded(pack,mesh,inst,&view,(index),(out),(proven)) : \
    cache_draw_vertex(pack,mesh,inst,&view,(index),(out),(proven)))
/* Wide view: triangles are cut at the near plane only and the PVR clips at
 * the screen edges (geometry.h, nm_cache_vertex_wide). The screen planes
 * still reject whole instances and whole face groups; no point and no
 * triangle is tested against them. A batch whose view is the whole screen
 * classifies its points against the guard view: the screen moved out by
 * SQ_GUARD_BAND logical pixels on every side (make NATIVE_GUARD_BAND; 2048
 * by default since 15 October, at most 3,282 output pixels from the origin,
 * drawn correctly on hardware where the most otherwise clipped triangles
 * occur; 1024, the earlier default, reaches 1,961). A
 * face with a corner beyond the band or behind the near plane goes through
 * the clipper with that view, so the band bounds every submitted
 * coordinate; an instance or group whose bounds lie inside the guard view
 * skips even that test. Sub-viewports keep exact CPU clipping: the PVR would
 * draw outside them. */
#ifndef SQ_GUARD_BAND
#define SQ_GUARD_BAND 2048.0f
#endif
static inline int sq_view_is_screen(const nm_view_t *view) {
    return view->bounds[0]==0 && view->bounds[1]==496 && view->bounds[2]==0 && view->bounds[3]==384;
}
/* Bounds planes of the guard view; parts of one car and all scenery cells
 * share matrix, focus and view, so the previous result is kept. */
static const nm_bounds_planes_t *sq_guard_planes(const nm_view_t *guard,const nm_instance_t *inst) {
    static nm_bounds_planes_t planes;
    alignas(32) static float key[12+2+7+3];  /* the last three words stay zero */
    static int valid;
    alignas(32) float now[12+2+7+3];
    nm_copy_words(now,inst->matrix,12*sizeof(float)); now[12]=inst->focus[0]; now[13]=inst->focus[1];
    nm_copy_words(now+14,guard,7*sizeof(float)); now[21]=now[22]=now[23]=0;
    if(!valid || memcmp(key,now,sizeof(key))) {
        nm_copy_words(key,now,sizeof(key)); valid=1;
        nm_bounds_planes_prepare(&planes,guard,inst->matrix,inst->focus);
    }
    return &planes;
}
#else
#define SQ_CACHE_VERTEX(index,out,proven) cache_draw_vertex(pack,mesh,inst,&view,(index),(out),(proven))
#endif

/* Counting hooks for host tools; nothing on the console. */
/* Host tools: a vertex of a face written by the clipper (sq_emit_face_general). */
#ifndef SQ_CLIP_TAG
#define SQ_CLIP_TAG(polygon) ((void)0)
#endif
/* Host tools: every clipped triangle, its view, corners and plane bits (the simulator's statistics, polygon numbers and sample dump). */
#ifndef SQ_CLIP_INPUT
#define SQ_CLIP_INPUT(view,points,outside) ((void)0)
#endif
/* Host tools: a piece of tight smoke geometry begins (NM_SMOKE_TIGHT; its vertices are tagged SQ_CLIP_TAG(1)). */
#ifndef SQ_TIGHT_PIECE
#define SQ_TIGHT_PIECE() ((void)0)
#endif
#ifndef SQ_COUNT
#define SQ_COUNT(counter,amount) ((void)0)
#endif
#ifndef SQ_PROBE
#define SQ_PROBE(counter) ((void)0)
#endif
#ifndef SQ_LIGHT_PROBE
#define SQ_LIGHT_PROBE(inst,face,rear) ((void)0)
#endif
/* Which path a batch takes (host tools; tests/stream_sim prints the counts).
 * SQ_PATH_POINTS(kind,batch): the vertex loop, 0 shared positions in the wide
 * view (sq_cache_points_wide), 1 the other one (sq_cache_points_checked).
 * SQ_PATH_EMIT(kind,source,batch,list,faces): the emitter of a batch's faces
 * on one list, 0 production loop (proven to fit), 1 guarded general emitter
 * because the rest of the batch could not be proven to fit, 2 general emitter
 * for the whole object (inspection view, no emission tables), 3 the prebuilt
 * world stream (sq_stream_list; `faces` is 1). */
#ifndef SQ_PATH_POINTS
#define SQ_PATH_POINTS(kind,batch) ((void)0)
#endif
#ifndef NM_FT_BEGIN /* include/frame_trace.h; host tools take this file without it */
#define NM_FT_BEGIN(name) ((void)0)
#define NM_FT_END(group,name) ((void)0)
#endif
#if !defined(SQ_PATH_EMIT) && defined(NM_DIAG_FRAME_TRACE)
#define SQ_PATH_EMIT(kind,source,batch,list,faces) (nm_ft_emit[(source)!=0][kind]+=(faces))
#endif
#ifndef SQ_PATH_EMIT
#define SQ_PATH_EMIT(kind,source,batch,list,faces) ((void)0)
#endif
/* Per source: every binding the material/luma table can select carries its
 * material's first header word (list and checker bits), so the face loop
 * takes the list from the material record it has already read instead of the
 * binding record. Zero until checked (host fixtures build their own tables). */
static int sq_material_lists[2];
/* The material/luma -> binding table. The world's is dense (256 entries a material: its baked lumas are sparse).
 * The car's is one run a material when it can be (sq_luma_runs_build): the material pack's bindings are sorted by
 * header and luma, and every car header's lumas form one run up to 255 with consecutive bindings, so the binding
 * of luma l is first + l - lo for l >= lo and none below: 4 bytes a material instead of 512 (draw_runs; NULL: the
 * dense draw_bindings). Run word: first binding | lo << 16; a material without bindings: 0xffff | 255 << 16. */
static inline unsigned sq_lookup(const uint16_t *dense,const uint32_t *runs,unsigned material,unsigned luma) {
    if(runs) {
        const uint32_t run=runs[material];
        const unsigned lo=run>>16;
        return luma>=lo ? (run&0xffffu)+(luma-lo) : UINT16_MAX;
    }
    return dense[material*256u+luma];
}
/* 1 when every material's bindings are such a run (runs written), else 0 (use the dense table). */
static int sq_luma_runs_build(const nm_pack_t *pack,const nm_material_pack_t *materials,uint32_t *runs) {
    for(unsigned m=0;m<pack->header->materials;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one step */
        unsigned lo=256,first=0;
        const nm_material_binding_t *row[256];
        nm_material_find_row(materials,pack->materials[m].header,row);
        for(unsigned l=0;l<256;++l) {
            const nm_material_binding_t *binding=row[l];
            if(lo==256) {
                if(binding) { lo=l;first=(unsigned)(binding-materials->bindings); }
            } else if(!binding || (unsigned)(binding-materials->bindings)!=first+(l-lo)) return 0;
        }
        if(lo==256) runs[m]=UINT32_C(0xffff)|UINT32_C(255)<<16;
        else if(first+(255u-lo)>=UINT16_MAX) return 0;
        else runs[m]=(uint32_t)first|(uint32_t)lo<<16;
    }
    return 1;
}
static void sq_material_lists_prepare(unsigned source,const nm_pack_t *pack,
        const nm_material_pack_t *materials,const uint16_t *lookup,const uint32_t *runs) {
    const nm_header_t *original=pack->data;
    sq_material_lists[source]=0;
    for(unsigned m=0;m<original->materials;++m) for(unsigned l=(nm_loader_yield(0),0u);l<256;++l) {
        const unsigned index=sq_lookup(lookup,runs,m,l);
        if(index!=UINT16_MAX && materials->bindings[index].header[0]!=pack->materials[m].header[0]) return;
    }
    sq_material_lists[source]=1;
}

/* Emission reads every cached point once, and by then most are out of the
 * 16 KiB data cache (a scenery cell alone caches several times that; the
 * punch-through pass comes long after the points were
 * written): the hardware profile charges its largest single share to the
 * first read of a corner. While the four vertices of one face are written,
 * the corners of the next queued face are requested, one per vertex, so the
 * line fills overlap the arithmetic instead of stalling it. A hint only: it
 * reads nothing and changes no output. */
#define SQ_POINT_PREFETCH(point) __builtin_prefetch((point),0,3)

/* What the renderer needs of a face beyond the face loop, 32 bytes, built per
 * pack at load (sq_emit_faces_prepare) from the face table, the position
 * representatives and the offline strips. The production emitter reads this
 * record and never the face or the vertex table, and a face knows whether an
 * offline strip run may start at it without scanning the strip table
 * (0.49 ms a frame for some 37 runs).
 * corner: the position representative of each corner, as index*8-32768 in a
 * signed 16-bit word (sq_corner_of): its cached point is at
 * mid + 4*corner, mid = the batch's points + 4,096 entries
 * (sq_points_mid, sq_corner_point). The SH4 loads a 16-bit word sign
 * extended and shifts by two in one instruction, so this costs what a plain
 * byte offset did (load, shift, add; before load, zero extension, add) and
 * reaches SQ_CORNER_VERTICES = 8,192 vertices a mesh where the byte offset
 * (index*32 in an unsigned word) ended at 2,048: a pack with a larger mesh
 * lost this table and with it the production emitter and the shared-position
 * vertex loop for all of its batches, which is what every tree set with more
 * than 2,048 vertices in its densest cell ran into (2,050 to 2,230; some
 * 3 ms a frame on the console). A triangle's unused fourth corner is that of
 * index 0.
 * uv: each corner's own texture coordinates (the vertex table's u,v).
 * special: SQ_FACE_RUN bits, the faces from this one to the end of its
 * offline strip when a run may start here (the face's first triangle is an
 * even triangle of the strip, so every triangle keeps its winding) and the
 * strip has a further face, else 0; SQ_FACE_DYNAMIC_UV, a corner's u,v are
 * rewritten while the scene runs (nm_render_uv_dynamic: window reflections)
 * and must be read from the vertex table when the face is drawn.
 * strip: index into pack->strip_indices of the strip vertex the run starts
 * with. */
typedef struct {
    int16_t corner[4];
    uint8_t count, layer;
    uint16_t special;
    uint32_t strip;
    uint16_t uv[4][2];
} nm_sq_face_t;
_Static_assert(sizeof(nm_sq_face_t)==32,"one cache line per emission face");
#define SQ_CORNER_VERTICES 8192u
_Static_assert(sizeof(nm_cached_vertex_t)==32 && (-8>>3)==-1,"corner words: 32-byte cached points, arithmetic right shift");
static inline int16_t sq_corner_of(unsigned index) { return (int16_t)((int)(index*8u)-32768); }
/* Addresses as integers: mid lies outside a small batch's points. */
static inline __attribute__((always_inline)) uintptr_t sq_points_mid(const nm_cached_vertex_t *points) {
    return (uintptr_t)points+(SQ_CORNER_VERTICES/2u)*sizeof(nm_cached_vertex_t);
}
static inline __attribute__((always_inline)) const nm_cached_vertex_t *sq_corner_point(uintptr_t mid,int corner) {
    return (const nm_cached_vertex_t *)(mid+(uintptr_t)(intptr_t)(corner*4));
}
/* The corner's vertex mark (sq_marks below), from the table's middle
 * (sq_marks_mid): one arithmetic shift, no bias to add. */
static inline __attribute__((always_inline)) uint8_t *sq_corner_mark(uintptr_t marks_mid,int corner) {
    return (uint8_t *)(marks_mid+(uintptr_t)(intptr_t)(corner>>3));
}
#define SQ_MARKS_MID(marks) ((uintptr_t)(marks)+SQ_CORNER_VERTICES/2u)
#define SQ_FACE_DYNAMIC_UV 0x8000u
#define SQ_FACE_RUN 0x7fffu
static nm_sq_face_t *sq_emit_faces[2];
/* The same for a vertex of an offline strip, indexed like pack->strip_indices
 * and built with the emission faces: the corner word of its position
 * representative and its own u,v. A strip run (sq_emit_run) writes each of
 * its vertices from one of these 8-byte entries; before, a vertex cost a read
 * of the strip index, of the position representative and, for a vertex that
 * is not its own representative, a 32-byte copy of the cached point patched
 * with the vertex table's u,v. */
typedef struct { int16_t corner; uint16_t uv[2], unused; } nm_sq_strip_corner_t;
_Static_assert(sizeof(nm_sq_strip_corner_t)==8,"four strip vertices per cache line");
static nm_sq_strip_corner_t *sq_strip_corners[2];
/* Vertices the current batch asked for, a byte each. The face loop only
 * stores here: it never reads or writes a cached point, whose 32-byte entries
 * are rarely in the data cache at that time.
 * SQ_MARK_WANTED: asked for by a queued face. SQ_MARK_INSIDE: asked for by a
 * face whose bounds are proven inside the view (no point test).
 * SQ_MARK_FRESH: a position representative no face asked for, computed in
 * this batch for its aliases (per-vertex mode only, below).
 * A byte instead of a bit: marking is one store (a proven face) or a load, an
 * OR and a store per corner; the three interleaved bitmaps before cost a
 * shift, a variable shift and two read-modify-writes. A batch touches
 * vertex_count bytes, 1.9 KiB for the largest shipped mesh. sq_allocate
 * refuses a pack with a larger mesh than the table holds.
 *
 * Two modes, per source:
 * - shared positions (the pack has emission faces, sq_emit_faces): a queued
 *   face marks the position representatives of its corners, so each distinct
 *   position of the mesh is transformed once and nothing is copied; 43 per
 *   cent of the world's and 61 per cent of the cars' vertices repeat an
 *   earlier position and were a 32-byte copy plus a UV patch each, more than
 *   a transform. The emitters take a corner's u,v from the face (nm_sq_face_t)
 *   or from its vertex (sq_corner), never from the cached entry.
 * - per vertex (no emission faces: host fixtures): a face marks its own
 *   vertices, and a vertex that repeats a position copies its
 *   representative's entry and patches u,v, as before. */
#define SQ_MARK_WANTED 1u
#define SQ_MARK_INSIDE 2u
#define SQ_MARK_FRESH 4u
/* Prebuilt world streams only: a face that may be clipped holds the position,
 * so its whole cached entry is written (sq_cache_points_stream). */
#define SQ_MARK_VIEW 8u
/* On the heap, sized for the largest mesh prepared so far: as a static table
 * of 16 KiB the variables the linker placed behind it lay exactly one data
 * cache size (0x4000) after its first bytes, the ones every batch marks. */
static uint8_t *sq_marks;
static unsigned sq_marks_size;
static int sq_marks_reserve(unsigned vertices) {
    const unsigned needed=(vertices+4u+31u)&~31u;
    if(needed<=sq_marks_size) return 1;
    free(sq_marks); sq_marks_size=0;
    sq_marks=nm_alloc(needed);
    if(!sq_marks) return 0;
    sq_marks_size=needed;
    return 1;
}
/* Level of detail for scenery cutouts (meshpack.h, NM_PACK_LOD_GROUPS): a
 * group is one source polygon with up to three alternative shapes, each a
 * consecutive face range. The world face walk (sq_walk_baked) walks the faces
 * outside every group as before and, per group, the faces of ONE range; the
 * other ranges are stepped over, so a shape that is not drawn costs nothing
 * per face. Chosen in sq_prepare_batch, which runs once per batch and frame
 * (in the object's first list callback) and fills the opaque and the
 * punch-through queue together: both lists draw the same shape.
 * The renderer's view of a group, one cache line, built per pack after the
 * scenery stage selection (sq_lod_prepare):
 *   centre  mesh space; its view-space depth z is one dot product with the z
 *           row of the instance matrix;
 *   depth   switch_depth of the pack, FLT_MAX where no further level exists.
 *           They are authored for the vertical focus NM_LOD_REFERENCE_FOCUS:
 *           z is scaled by reference/focus[1] once per batch instead;
 *   first, count, end   the ranges as mesh-local face indices;
 *   bounds  the mesh-local face group all faces of the group share: a group
 *           outside the view is stepped over whole, without the depth test.
 * sq_lod_state holds the level each group was last drawn at, a byte per
 * group, zero at load: a group at level n goes to n+1 when the scaled depth
 * exceeds depth[n]*NM_LOD_HYSTERESIS and back when it falls below
 * depth[n]/NM_LOD_HYSTERESIS, so a tree at a switch distance does not
 * flicker. A group that is not walked (its cell is not drawn, or its bounds
 * are outside the view) keeps its byte.
 * sq_lod_face_level: per world face 0 outside every group, else level+1; the
 * strip inspection view tints by it (SQ_LOD_TINT) and nothing else reads it. */
#include <float.h>
typedef struct {
    float centre[3];
    float depth[NM_LOD_LEVELS-1];
    uint16_t first, count[NM_LOD_LEVELS], end;
    uint16_t bounds;
} nm_sq_lod_t;
_Static_assert(sizeof(nm_sq_lod_t)==32 && NM_LOD_LEVELS==3,"one cache line per level-of-detail group");
static nm_sq_lod_t *sq_lod_groups;
static uint8_t *sq_lod_state,*sq_lod_face_level;
/* Host tools record the level a walked group was drawn at. */
#ifndef SQ_LOD_SELECTED
#define SQ_LOD_SELECTED(group,level) ((void)0)
#endif
/* Catch-fence sweep (make NATIVE_FENCE_SWEEP=1; default 0 since 15 October
 * 2026, evening: the whole-panel switching above for every group). It acts
 * only on a world pack with a fence rule (assets-ssfar4cf3 .. assets-ssfar4eu);
 * the recommended assets-ssfar4f draws the plain fence as the arcade defines
 * it, one stipple level at every distance, and has no such groups. A stipple panel of the catch
 * fence (tools/lod_groups.py, the "checker" rule: levels such as the dense
 * stipple, a sparser stipple, nothing) is one quad drawn at one level per
 * frame, so the mesh used to appear and thin out panel by panel at the
 * switch distance. With the sweep the boundary is a plane of view-space
 * depth instead: the part of a panel nearer than plane k is drawn at level k,
 * the part beyond it at level k+1, and the boundary moves continuously with
 * the camera.
 *   Which groups: found at load (sq_lod_prepare): every nonempty range one
 *     quad, all of them the same four corners (vertex indices), every
 *     material a catch-fence stipple (draw_binding_stipple's header test),
 *     level 0 drawn, a mesh no other instance of the pack draws, every such
 *     group with the same number of levels. Others keep the rules above.
 *   Planes: one set for all of them, plane k = the least switch_depth[k] of
 *     those groups (focus 280; the depth where the stipple reaches its texel
 *     size, plus the reach of the smallest panel), scaled with the focus as
 *     every switch depth. sq_lod_groups marks such a group with a negative
 *     depth[0] (its own depths are not read again).
 *   Per frame (sq_lod_select, both face walks): the view-space depths of the
 *     panel's four corners (FIPR of the instance matrix's z row), skipped
 *     when the centre lies farther than the largest panel reach from every
 *     plane. Against plane k the panel is "near" when its farthest corner is
 *     at most SQ_SWEEP_MARGIN beyond it, else "beyond" when its nearest corner
 *     is at most SQ_SWEEP_MARGIN in front of it, else it straddles it. No
 *     hysteresis: the decision is a function of this frame's depths. A panel
 *     that straddles no plane is drawn whole at its level, through the paths
 *     every group takes (level = planes it lies beyond); one that straddles
 *     plane(s) gets the code SQ_SWEEP | n0 | n1<<2 (levels n0 < n1; the same
 *     byte in sq_lod_state and the stream's per-batch levels): each level n
 *     in n0..n1 with a face is drawn clipped to the depth band between the
 *     planes inside the panel (sq_sweep_face): the face's polygon cut at the
 *     plane z = P*focus/280 (sq_sweep_depth) by the near-plane clipper's rule (Sutherland-Hodgman,
 *     t from the endpoint nearer the plane by SH4ZAM's FSRRA division, x, y,
 *     u, v by shz_lerpf, z set onto the plane), so the stipple's texels stay
 *     where they are on the panel; the cut points are projected and
 *     classified like any cached point and the band polygon is written as a
 *     fan of triangles, one clipped polygon (sq_clip as usual should a point
 *     lie outside the view).
 *   Paths: the stream path writes a straddling group in place, in face order
 *     (sq_stream_sweep); its corners get the whole transform (SQ_MARK_VIEW:
 *     the clip needs their view coordinates). The queue path queues the
 *     group's faces of levels n0..n1, and a batch with such a group
 *     (nm_sq_batch_t.sweep) is emitted by the general emitter, which clips
 *     them (the batch's other faces: the same words as the production one).
 * The reference (tests/reference_stream.h) finds the groups and planes on its
 * own, checks each code against the corner depths (double precision, with the
 * level-of-detail slack) and clips in double precision; the band polygons
 * are compared within the clipper's tolerances. */
#ifndef NM_FENCE_SWEEP
#define NM_FENCE_SWEEP 0
#endif
#if NM_FENCE_SWEEP && defined(NM_SH4ZAM_GFX) && NM_SH4ZAM_GFX && (defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD))
#define SQ_SWEEPS 1
#else
#define SQ_SWEEPS 0
#endif
#define SQ_SWEEP 0x80u
#define SQ_SWEEP_MARGIN (1.0f/16.0f) /* scaled depth units (focus 280); tests/reference_stream.h has the same */
#define SQ_SWEEP_FIRST(code) ((code)&3u)
#define SQ_SWEEP_LAST(code) (((code)>>2)&3u)
static float sq_sweep_plane[NM_LOD_LEVELS-1]; /* focus 280, increasing */
static float sq_sweep_reach;                  /* largest corner distance of a panel from its group centre, mesh units */
static unsigned sq_sweep_planes,sq_sweep_groups;
static const nm_mesh_t *sq_lod_mesh;          /* the mesh whose groups the face walk is choosing levels for (SQ_SWEEPS) */
static void sq_lod_release(void) {
    free(sq_lod_groups); free(sq_lod_state); free(sq_lod_face_level);
    sq_lod_groups=NULL; sq_lod_state=sq_lod_face_level=NULL;
#if SQ_SWEEPS
    sq_sweep_planes=sq_sweep_groups=0; sq_sweep_reach=0;
#endif
}
#if SQ_SWEEPS
/* A catch-fence stipple header (model_renderer.c, draw_binding_stipple). */
static inline int sq_sweep_stipple(const uint16_t header[4]) {
    return ((header[0]>>13)&7u)==3u && (header[1]&0xfcffu)==0x8000u;
}
/* The sweep groups of a pack (see above), after sq_lod_groups is built. */
static void sq_sweep_find(const nm_pack_t *pack) {
    const nm_header_t *original=pack->data;
    const nm_instance_t *instances=(const void *)(pack->materials+original->materials);
    unsigned levels=0,groups=0,mixed=0;
    float plane[NM_LOD_LEVELS-1]={FLT_MAX,FLT_MAX},reach=0;
    for(unsigned pass=0;pass<2;++pass) {
      if(pass) {
        /* Planes strictly increasing, far enough apart for the margins. */
        int usable=groups && !mixed && plane[0]>4.0f*SQ_SWEEP_MARGIN;
        for(unsigned k=1;usable && k+1<levels;++k) usable=plane[k]>plane[k-1]+4.0f*SQ_SWEEP_MARGIN;
        if(!usable) {
            if(groups) printf("Native fence sweep: off (%u stipple groups, %s)\n",groups,mixed ? "different level counts" : "planes too close");
            return;
        }
        sq_sweep_planes=levels-1; sq_sweep_groups=groups;
        for(unsigned k=0;k<sq_sweep_planes;++k) sq_sweep_plane[k]=plane[k];
        sq_sweep_reach=reach*1.001f+1e-3f;
      }
      for(unsigned m=0;m<original->meshes;++m) {
        const nm_mesh_t *mesh=pack->meshes+m;
        const unsigned first=mesh->padding[4]&0xffffu,count=mesh->padding[4]>>16;
        if(!count) continue;
        unsigned users=0;
        for(unsigned i=0;i<original->instances;++i) users+=instances[i].mesh==m;
        if(users!=1) continue;
        for(unsigned g=0;g<count;++g) {
            const nm_lod_group_t *group=pack->lod_groups+first+g;
            nm_sq_lod_t *out=sq_lod_groups+first+g;
            if(group->levels<2 || out->count[0]!=1) continue;
            const nm_face_t *const base=pack->faces+mesh->face_first+out->first;
            int ok=base->count==4;
            for(unsigned l=0,at=out->first;ok && l<group->levels;at+=out->count[l],++l) {
                if(out->count[l]>1) { ok=0; break; }
                if(!out->count[l]) continue;
                const nm_face_t *face=pack->faces+mesh->face_first+at;
                ok=face->count==4 && !memcmp(face->indices,base->indices,sizeof(base->indices)) &&
                    sq_sweep_stipple(pack->materials[face->material].header);
            }
            if(!ok) continue;
            if(!pass) {
                if(levels && levels!=group->levels) mixed=1;
                levels=group->levels;
                for(unsigned k=0;k+1<group->levels;++k) if(group->switch_depth[k]<plane[k]) plane[k]=group->switch_depth[k];
                for(unsigned v=0;v<4;++v) {
                    const float *p=pack->vertices[mesh->vertex_first+base->indices[v]].position;
                    const float dx=p[0]-out->centre[0],dy=p[1]-out->centre[1],dz=p[2]-out->centre[2];
                    const float d=shz_sqrtf(dx*dx+dy*dy+dz*dz); /* the group's reach at load: SH4ZAM's root */
                    if(d>reach) reach=d;
                }
                ++groups;
                continue;
            }
            out->depth[0]=-out->depth[0]; /* the mark: switch depths are positive */
        }
      }
    }
    if(sq_sweep_planes)
        printf("Native fence sweep: %u stipple panels of %u levels; planes at depth %g%s%g (focus 280); panel reach %g\n",
            sq_sweep_groups,sq_sweep_planes+1,(double)sq_sweep_plane[0],sq_sweep_planes>1 ? " and " : "",
            sq_sweep_planes>1 ? (double)sq_sweep_plane[1] : 0.0,(double)sq_sweep_reach);
}
#endif
/* After the scenery stage selection, which rewrites the pack's ranges
 * (track_surface.c): a range, or a whole group, may be empty by then. */
static int sq_lod_prepare(const nm_pack_t *pack) {
    sq_lod_release();
    const unsigned count=nm_pack_lod_count(pack);
    if(!count) return 1;
    const nm_header_t *original=pack->data;
    sq_lod_groups=nm_alloc(count*sizeof(*sq_lod_groups));
    sq_lod_state=nm_heap_zeroed("world level-of-detail state",32,count);
    sq_lod_face_level=nm_calloc(original->faces ? original->faces : 1,1);
    if(!sq_lod_groups || !sq_lod_state || !sq_lod_face_level) { sq_lod_release(); return 0; }
    unsigned faces=0,empty=0,levels[NM_LOD_LEVELS]={0};
    for(unsigned m=0;m<original->meshes;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one step */
        const nm_mesh_t *mesh=pack->meshes+m;
        const unsigned first=mesh->padding[4]&0xffffu;
        for(unsigned g=0;g<mesh->padding[4]>>16;++g) {
            const nm_lod_group_t *group=pack->lod_groups+first+g;
            nm_sq_lod_t *out=sq_lod_groups+first+g;
            const unsigned local=group->face_first-mesh->face_first;
            unsigned total=0;
            for(unsigned level=0;level<NM_LOD_LEVELS;++level) {
                for(unsigned f=0;f<group->face_count[level];++f) sq_lod_face_level[group->face_first+total+f]=(uint8_t)(level+1u);
                out->count[level]=group->face_count[level]; total+=group->face_count[level];
            }
            if(local+total>mesh->face_count || local+total>UINT16_MAX) { sq_lod_release(); return 0; }
            out->centre[0]=group->centre[0]; out->centre[1]=group->centre[1]; out->centre[2]=group->centre[2];
            for(unsigned level=0;level+1<NM_LOD_LEVELS;++level)
                out->depth[level]=level+1<group->levels ? group->switch_depth[level] : FLT_MAX;
            out->first=(uint16_t)local; out->end=(uint16_t)(local+total);
            out->bounds=(uint16_t)(total && nm_pack_has_groups(pack) ? pack->faces[group->face_first].group-mesh->padding[0] : 0);
            faces+=total; empty+=!total; ++levels[group->levels-1];
        }
    }
    printf("Native level of detail: %u groups (%u/%u/%u with one/two/three levels, %u without a face at this scenery stage), "
           "%u faces in their ranges; hysteresis %g; tables=%u bytes\n",count,levels[0],levels[1],levels[2],empty,faces,
        (double)NM_LOD_HYSTERESIS,(unsigned)(count*(sizeof(*sq_lod_groups)+1u)+original->faces));
#if SQ_SWEEPS
    sq_sweep_find(pack);
#endif
    return 1;
}
/* The level a walked group is drawn at this frame, from the level it had:
 * `scaled` is the view-space depth of its centre times reference focus over
 * focus. At most two steps either way; a depth that is not a number leaves
 * the level as it is. */
static inline __attribute__((always_inline)) unsigned sq_lod_level(const nm_sq_lod_t *group,unsigned level,float scaled) {
    const float far_side=scaled*(1.0f/NM_LOD_HYSTERESIS),near_side=scaled*NM_LOD_HYSTERESIS;
    if(level<NM_LOD_LEVELS-1 && far_side>group->depth[level]) {
        ++level;
        if(level<NM_LOD_LEVELS-1 && far_side>group->depth[level]) ++level;
    } else if(level && nm_float_negative(near_side-group->depth[level-1])) { /* a sign test: no libgcc unordered call a group */
        --level;
        if(level && nm_float_negative(near_side-group->depth[level-1])) --level;
    }
    return level;
}
/* The level group `index` is drawn at this frame, recorded in sq_lod_state:
 * the depth of its centre through the z row of the instance matrix, times
 * reference focus over focus (`row`, `scale`). One function for both face
 * walks (sq_walk_baked, sq_stream_batch_prepare), so that the console makes
 * the same decision with the same compiled arithmetic in either. */
/* Host tools count the sweep decisions that needed the corners. */
#ifndef SQ_SWEEP_DECIDED
#define SQ_SWEEP_DECIDED(straddles) ((void)0)
#endif
#if SQ_SWEEPS
/* A sweep group's level or code this frame (see the sweep above): `centre` is
 * the scaled depth of its centre; the corners (sq_lod_mesh's level-0 face of
 * the group) only when a plane lies within the panel's reach of it. */
static __attribute__((noinline)) unsigned sq_sweep_select(const nm_sq_lod_t *lod,float centre,const float row[4],float scale) {
    const float reach=sq_sweep_reach*scale+SQ_SWEEP_MARGIN;
    unsigned beyond=0,close=0;
    for(unsigned k=0;k<sq_sweep_planes;++k) {
        beyond+=centre>sq_sweep_plane[k];
        close|=fabsf(centre-sq_sweep_plane[k])<=reach;
    }
    if(!close) return beyond;
    const nm_pack_t *const pack=world_draw.pack;
    const nm_mesh_t *const mesh=sq_lod_mesh;
    const nm_face_t *const face=pack->faces+mesh->face_first+lod->first;
    float low=FLT_MAX,high=-FLT_MAX;
    for(unsigned v=0;v<4;++v) {
        const float *p=pack->vertices[mesh->vertex_first+face->indices[v]].position;
        const float z=shz_dot8f(p[0],p[1],p[2],1.0f,row[0],row[1],row[2],row[3])*scale;
        if(z<low) low=z;
        if(z>high) high=z;
    }
    unsigned first=0,last=0;
    for(unsigned k=0;k<sq_sweep_planes;++k) {
        if(high<=sq_sweep_plane[k]+SQ_SWEEP_MARGIN) continue; /* near: the panel lies in front of this plane */
        ++last;
        if(low>=sq_sweep_plane[k]-SQ_SWEEP_MARGIN) ++first;   /* beyond it */
    }
    SQ_SWEEP_DECIDED(first!=last);
    return first==last ? first : SQ_SWEEP|first|last<<2;
}
#endif
static __attribute__((noinline)) unsigned sq_lod_select(unsigned index,const float row[4],float scale) {
    const nm_sq_lod_t *const lod=sq_lod_groups+index;
    const float depth=(lod->centre[0]*row[0]+lod->centre[1]*row[1]+lod->centre[2]*row[2]+row[3])*scale;
#if SQ_SWEEPS
    if(nm_float_negative(lod->depth[0])) {
        const unsigned code=sq_sweep_select(lod,depth,row,scale);
        sq_lod_state[index]=(uint8_t)code;
        SQ_LOD_SELECTED(index,code);
        return code;
    }
#endif
    const unsigned level=sq_lod_level(lod,sq_lod_state[index],depth);
    sq_lod_state[index]=(uint8_t)level;
    SQ_LOD_SELECTED(index,level);
    return level;
}
/* Strip inspection view: a face of a group in the colour of its level (green
 * nearest, yellow, red), two shades by face parity so neighbours can be told
 * apart; other faces keep the strip colours. */
static const uint32_t sq_lod_colors[NM_LOD_LEVELS][2]={{0xff30d030,0xff209020},{0xffe0e030,0xffa0a020},{0xffe03030,0xffa02020}};
#define SQ_LOD_TINT(s,absolute,otherwise) ((s) && sq_lod_face_level && sq_lod_face_level[absolute] ? \
    sq_lod_colors[sq_lod_face_level[absolute]-1u][(absolute)&1u] : (otherwise))

/* sq_prepare_batch in four parts, each a function of its own with the batch's
 * constants in a small context: as one 13 KiB function with cars and scenery
 * interleaved through flags, the compiler kept the loops' state on the stack
 * (an eighth of the executed instructions were stack loads and stores). */
typedef struct {
    nm_sq_batch_t *batch;
    const nm_pack_t *pack;
    const nm_mesh_t *mesh;
    const nm_instance_t *inst;
    const nm_material_pack_t *materials;
    const uint16_t *lookup;
    const uint32_t *runs;            /* the car's run table (sq_lookup), NULL: lookup is dense */
    nm_view_t view;                 /* the batch's view, as queued */
    unsigned source;
    int bounds, batch_proven, material_lists;
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
    int bounded, wide;
    nm_view_t guard;                /* the view with the guard band */
    const nm_bounds_planes_t *guard_planes;
    const nm_sq_facing_t *facing;   /* NULL: reference side test on every face */
    const nm_sq_face_plane_t *face_planes;
    nm_sq_facing_t facing_terms;
#endif
} nm_sq_walk_t;

/* Group classification: 0 outside the view, 1 straddling, 2 inside. */
static __attribute__((noinline)) void sq_classify_groups(const nm_sq_walk_t *walk) {
    const nm_pack_t *const pack=walk->pack;
    const nm_mesh_t *const mesh=walk->mesh;
    const nm_instance_t *const inst=walk->inst;
    const int bounds=walk->bounds;
    DRAW_PHASE_START(bounds_started);
    nm_bounds_planes_t planes;
    const nm_bounds_planes_t *group_planes=&planes;
    if(walk->source && sq_scenery_camera.shared) group_planes=&sq_scenery_camera.planes;
    else if(nm_pack_has_groups(pack) && bounds!=2)
        nm_bounds_planes_prepare(&planes,&walk->view,inst->matrix,inst->focus);
    if(nm_pack_has_groups(pack)) for(unsigned g=0;g<mesh->padding[1];++g) {
        const float *group_bounds=nm_pack_group_bounds(pack,mesh->padding[0]+g);
        draw_groups[g]=bounds==2 ? 2 : nm_bounds_planes_classify(group_planes,group_bounds);
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
        /* 1 (straddles the screen) becomes 2 when the guard view holds it. */
        if(walk->guard_planes && !walk->batch_proven && draw_groups[g]==1 &&
           nm_bounds_planes_classify(walk->guard_planes,group_bounds)==2) draw_groups[g]=2;
#endif
    }
    DRAW_PHASE_END(4,bounds_started);
}

/* A face that is drawn: its corners are marked wanted (and proven inside)
 * and it joins the queue of its list. Two compact queues share one
 * face-sized allocation. PT grows backwards and is read backwards too, so
 * each list is walked in ascending face order. Depth no longer needs that
 * (coplanar faces carry a layer); the offline strip records do: they are
 * matched against consecutive queue entries in face order. */
static inline __attribute__((always_inline)) void sq_queue_face(const nm_face_t *face,const nm_sq_face_t *shared,
        unsigned f,unsigned index,unsigned list_header,int inside,uint32_t *queue,unsigned face_count,
        unsigned *opaque_count,unsigned *cutout_count,uint8_t *const marks) {
    if(shared) {
        /* Shared positions: the representatives of the corners. */
        const nm_sq_face_t *const record=shared+f;
        const unsigned corners=record->count;
        const uintptr_t marks_mid=SQ_MARKS_MID(marks);
        if(inside) for(unsigned v=0;v<corners;++v) *sq_corner_mark(marks_mid,record->corner[v])=SQ_MARK_WANTED|SQ_MARK_INSIDE;
        else for(unsigned v=0;v<corners;++v) *sq_corner_mark(marks_mid,record->corner[v])|=SQ_MARK_WANTED;
    } else for(unsigned v=0;v<face->count;++v) {
        const unsigned corner=face->indices[v];
        if(inside) marks[corner]=SQ_MARK_WANTED|SQ_MARK_INSIDE;
        else marks[corner]|=SQ_MARK_WANTED;
    }
    const unsigned slot=((list_header>>13)&3)==3 ? face_count-1-(*cutout_count)++ : (*opaque_count)++;
    queue[slot]=(f<<16)|index;
}
#define SQ_WALK_LOCALS \
    nm_sq_batch_t *const batch=walk->batch; \
    const nm_pack_t *const pack=walk->pack; \
    const nm_mesh_t *const mesh=walk->mesh; \
    const nm_instance_t *const inst=walk->inst; \
    const nm_face_t *const faces=pack->faces+mesh->face_first; \
    const nm_material_t *const pack_materials=pack->materials; \
    const unsigned face_count=mesh->face_count,group_first=mesh->padding[0]; \
    const int grouped=nm_pack_has_groups(pack); \
    const int batch_proven=walk->batch_proven,material_lists=walk->material_lists; \
    const uint8_t *const groups=draw_groups; \
    uint32_t *const queue=batch->bindings; \
    uint8_t *const marks=sq_marks; \
    const nm_sq_face_t *const shared=sq_emit_faces[walk->source] ? sq_emit_faces[walk->source]+mesh->face_first : NULL; \
    unsigned opaque_count=batch->opaque_count,cutout_count=batch->cutout_count

#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
/* The face loop of baked scenery: no light, only the side the view shows. */
static __attribute__((noinline)) int sq_walk_baked(const nm_sq_walk_t *walk) {
    SQ_WALK_LOCALS;
    const nm_material_pack_t *const materials=walk->materials;
    const nm_sq_facing_t *const facing=walk->facing;
    const nm_sq_face_plane_t *const face_planes=walk->face_planes;
    const nm_baked_face_t *const baked_faces=sq_baked_faces+mesh->face_first;
    /* Level-of-detail groups of this mesh, in face order (see nm_sq_lod_t).
     * The faces are walked in runs f..stop-1: the faces before the next
     * group, or the range chosen for a group. The face loop itself is the
     * plain loop it was; a mesh without groups is one run, 0..face_count. */
    const nm_sq_lod_t *lod=NULL,*lod_end=NULL;
    unsigned f=0,stop=0,resume=0;
    int in_group=0;
    float lod_row[4]={0,0,0,0},lod_scale=0;
    if(sq_lod_groups && mesh->padding[4]>>16) {
        lod=sq_lod_groups+(mesh->padding[4]&0xffffu); lod_end=lod+(mesh->padding[4]>>16);
        /* The z row of the instance matrix and reference focus over focus. */
        lod_row[0]=inst->matrix[2]; lod_row[1]=inst->matrix[5]; lod_row[2]=inst->matrix[8]; lod_row[3]=inst->matrix[11];
        lod_scale=NM_LOD_REFERENCE_FOCUS/inst->focus[1];
#if SQ_SWEEPS
        sq_lod_mesh=mesh;
#endif
    }
    for(;;) {
        /* The next run. */
        if(in_group) {
            /* The chosen range is done: step over the rest of the group. */
            f=resume; in_group=0; ++lod;
        }
        for(;;) {
            stop=lod!=lod_end ? lod->first : face_count;
            if(f<stop) break;
            if(lod==lod_end) goto walked;
            resume=lod->end;
            if(grouped && !groups[lod->bounds]) {
                /* Outside the view: no shape, and the level stays. */
                f=resume; ++lod;
                continue;
            }
            const unsigned level=sq_lod_select((unsigned)(lod-sq_lod_groups),lod_row,lod_scale);
#if SQ_SWEEPS
            if(level>=SQ_SWEEP) {
                /* A panel across a sweep plane: the faces of levels n0..n1,
                 * clipped to their bands by the general emitter. */
                const unsigned near=SQ_SWEEP_FIRST(level),far=SQ_SWEEP_LAST(level);
                f=lod->first; stop=lod->first;
                for(unsigned l=0;l<=far;++l) { if(l<near) f+=lod->count[l]; stop+=lod->count[l]; }
                in_group=1; batch->sweep=1;
                if(f<stop) break;
                f=resume; in_group=0; ++lod;
                continue;
            }
#endif
            f=lod->first+(level ? lod->count[0] : 0u)+(level>1 ? lod->count[1] : 0u);
            stop=f+lod->count[level]; in_group=1;
            if(f<stop) break;
            /* This level draws nothing. */
            f=resume; in_group=0; ++lod;
        }
      for(;f<stop;++f) {
        const nm_face_t *face=faces+f;
        const unsigned group=grouped ? groups[face->group-group_first] : 1;
        if(!group) {
#ifdef NATIVE_BOUNDS_VERIFY
            sq_verify_rejected_face(pack,mesh,inst,&walk->view,face);
#endif
            continue;
        }
        const int inside=batch_proven || (grouped && group==2);
        DRAW_PHASE_START(light_started);
        unsigned selected;
        unsigned material_header; /* Read once the face is known not to be a culled rear. */
        {
            /* Static scenery: nothing is lit; only the visible side counts. */
            const int baked=sq_baked_binding(pack,mesh,face,baked_faces[f],facing,face_planes+f);
            if(baked==-2) return 0;
            if(baked<0) { SQ_COUNT(draw_rear,1); DRAW_PHASE_END(1,light_started); continue; }
            /* A screen-checker panel the pack still carries (header bit 15)
             * is drawn like any other scenery face, on the opaque list. */
            material_header=pack_materials[face->material].header[0];
            selected=(unsigned)baked;
        }
        DRAW_PHASE_NEXT(1,light_started,material_started);
        const unsigned index=selected;
        if(index==UINT16_MAX) return 0;
        /* The binding was found by its material's header (nm_material_find
         * compares all four words), so the list is the material's; checked
         * once per pack (sq_material_lists_prepare). Else read the binding. */
        const unsigned list_header=material_lists ? material_header : materials->bindings[index].header[0];
        sq_queue_face(face,shared,f,index,list_header,inside,queue,face_count,&opaque_count,&cutout_count,marks);
        DRAW_PHASE_END(2,material_started);
      }
    }
walked:
    batch->opaque_count=opaque_count; batch->cutout_count=cutout_count;
    return 1;
}
#endif

/* The face loop of lit meshes: the cars, and any pack without baked light
 * (host fixtures). */
static __attribute__((noinline)) int sq_walk_lit(const nm_sq_walk_t *walk) {
    SQ_WALK_LOCALS;
    const nm_material_pack_t *const materials=walk->materials;
    const uint16_t *const lookup=walk->lookup;
    const unsigned source=walk->source;
    (void)source;
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
    const nm_sq_facing_t *const facing=walk->facing;
    const nm_sq_face_plane_t *const face_planes=walk->face_planes;
    /* A new stamp per lit batch; when the 16-bit stamp wraps, every entry
     * is cleared so that no old one can match. Stamp 0 is never issued. */
    const uint16_t *const light_class=!source && facing && sq_light_class ? sq_light_class+mesh->face_first : NULL;
    unsigned light_stamp=0;
    if(light_class) {
        if(++sq_light_stamp>UINT16_MAX) {
            for(unsigned c=0;c<sq_light_cache_size;++c) sq_light_cache[c]=(nm_sq_light_t){{0,0},{0,0},{0,0},{0,0}};
            sq_light_stamp=1;
        }
        light_stamp=sq_light_stamp;
    }
#endif
    for(unsigned f=0;f<face_count;++f) {
        const nm_face_t *face=faces+f;
        const unsigned group=grouped ? groups[face->group-group_first] : 1;
        if(!group) {
#ifdef NATIVE_BOUNDS_VERIFY
            sq_verify_rejected_face(pack,mesh,inst,&walk->view,face);
#endif
            continue;
        }
        const int inside=batch_proven || (grouped && group==2);
        DRAW_PHASE_START(light_started);
        unsigned selected;
        unsigned material_header; /* Read once the face is known not to be a culled rear. */
        {
        int light=-2;
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
        if(!source && facing) {
            /* Cars: the side first. A rear face of a one-sided polygon is
             * rejected whatever its luma (reject_draw_face), so it is not
             * lit; a decided front face is lit from its normal alone. */
            const int side=sq_face_side(facing,face_planes+f,face->normal);
#ifdef NATIVE_LIGHT_VERIFY
            if(side) {
                const int reference=face_light(pack,mesh,inst,face,1);
                const int sided=sq_sided_light(pack,mesh,inst,face,side<0);
                if(!sq_face_side_verify(pack,mesh,face,facing,face_planes+f,side) || reference<0 ||
                   ((reference&256)!=0)!=(side<0) || (sided!=-2 && !SQ_LIGHT_SAME(sided,reference))) {
                    printf("FAIL: car face light from its plane side %d: reference %d, sided %d\n",side,reference,sided);
                    return 0;
                }
                ++sq_facing_decided[0][side<0];
            } else ++sq_facing_reference[0];
#endif
            if(side<0 && !(face->attribute&(1u<<17))) { SQ_COUNT(draw_rear,1); DRAW_PHASE_END(1,light_started); continue; }
            if(side) {
                SQ_LIGHT_PROBE(inst,face,side<0);
                if(light_class) {
                    /* One evaluation per class and side of this batch; a
                     * face sent back to face_light (-2) is not remembered. */
                    nm_sq_light_t *const entry=sq_light_cache+light_class[f];
                    const unsigned rear=side<0;
                    if(entry->stamp[rear]==light_stamp) light=entry->light[rear];
                    else {
                        light=sq_sided_light(pack,mesh,inst,face,side<0);
                        if(light>=0) { entry->stamp[rear]=(uint16_t)light_stamp; entry->light[rear]=(int16_t)light; }
                    }
#ifdef NATIVE_LIGHT_VERIFY
                    if(light!=-2 && !SQ_LIGHT_SAME(light,face_light(pack,mesh,inst,face,1))) {
                        puts("FAIL: light class result differs from the face's own light"); return 0;
                    }
#endif
                } else light=sq_sided_light(pack,mesh,inst,face,side<0);
            }
        }
        if(light==-2)
#endif
        light=face_light(pack,mesh,inst,face,1);
        if(light<0) return 0;
        material_header=pack_materials[face->material].header[0];
        const unsigned rejection=reject_draw_face(face,material_header,light);
        SQ_COUNT(draw_rear,rejection==1); SQ_COUNT(draw_checker,rejection==3);
        if(rejection) { DRAW_PHASE_END(1,light_started); continue; }
        selected=sq_lookup(lookup,walk->runs,face->material,(unsigned)light&255u);
        }
        DRAW_PHASE_NEXT(1,light_started,material_started);
        const unsigned index=selected;
        if(index==UINT16_MAX) return 0;
        const nm_material_binding_t *binding=materials->bindings+index;
        /* The binding was found by its material's header (nm_material_find
         * compares all four words), so the list is the material's; checked
         * once per pack (sq_material_lists_prepare). Else read the binding. */
#ifdef NM_DIAG_CAR_OPAQUE
        const unsigned list_header=!source ? 0 : material_lists ? material_header : binding->header[0];
#else
        const unsigned list_header=material_lists ? material_header : binding->header[0];
#endif
        sq_queue_face(face,shared,f,index,list_header,inside,queue,face_count,&opaque_count,&cutout_count,marks);
        DRAW_PHASE_END(2,material_started);
    }
    batch->opaque_count=opaque_count; batch->cutout_count=cutout_count;
    return 1;
}

#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
/* The face loop of the cars: sq_walk_lit for a batch whose sides come from
 * plane constants, on the hot records (nm_sq_lit_face_t). Same decisions in
 * the same order: the side; a rear face of a one-sided polygon dropped
 * unlit; the light of the face's class and side, evaluated once per batch;
 * the rejections of reject_draw_face; the binding of material and luma. A
 * repeat of a class takes light and binding from the class entry. The face
 * table is read only for a face sent back to face_light (-2: a light dot
 * that could underflow), the emission face only for a queued face's marks. */
static __attribute__((noinline)) int sq_walk_car(const nm_sq_walk_t *walk) {
    nm_sq_batch_t *const batch=walk->batch;
    const nm_pack_t *const pack=walk->pack;
    const nm_mesh_t *const mesh=walk->mesh;
    const nm_instance_t *const inst=walk->inst;
    const nm_sq_lit_face_t *const records=sq_lit_faces+mesh->face_first;
    const nm_sq_face_t *const shared=sq_emit_faces[0]+mesh->face_first;
    const nm_material_t *const pack_materials=pack->materials;
    const uint16_t *const lookup=walk->lookup;
    const uint32_t *const runs=walk->runs;
    const nm_sq_facing_t *const facing=walk->facing;
    const unsigned face_count=mesh->face_count,group_first=mesh->padding[0],mode=mesh->mode;
    const int grouped=nm_pack_has_groups(pack),batch_proven=walk->batch_proven;
    const uint8_t *const groups=draw_groups;
    uint32_t *const queue=batch->bindings;
    const uintptr_t marks_mid=SQ_MARKS_MID(sq_marks);
    unsigned opaque_count=batch->opaque_count,cutout_count=batch->cutout_count;
    /* A new stamp per lit batch; when the 16-bit stamp wraps, every entry
     * is cleared so that no old one can match. Stamp 0 is never issued. */
    if(++sq_light_stamp>UINT16_MAX) {
        for(unsigned c=0;c<sq_light_cache_size;++c) sq_light_cache[c]=(nm_sq_light_t){{0,0},{0,0},{0,0},{0,0}};
        sq_light_stamp=1;
    }
    const unsigned light_stamp=sq_light_stamp;
    nm_sq_light_t *const light_cache=sq_light_cache;
#if NM_SH4ZAM_GFX
    nm_sq_light_terms_t light_terms; /* the light in model space (sq_class_light) */
    sq_light_terms(&light_terms,inst);
#endif
    for(unsigned f=0;f<face_count;++f) {
        const nm_sq_lit_face_t *const face=records+f;
        const unsigned group=grouped ? groups[face->group-group_first] : 1;
        if(!group) continue;
        const int inside=batch_proven || (grouped && group==2);
        const unsigned flags=face->flags;
        const int side=sq_face_side(facing,(const nm_sq_face_plane_t *)face,face->normal);
        if(side<0 && !(flags&SQ_LIT_TWO_SIDED)) { SQ_COUNT(draw_rear,1); continue; }
        int light=-2;
        unsigned selected=UINT16_MAX;
        if(side) {
            SQ_LIGHT_PROBE(inst,pack->faces+mesh->face_first+f,side<0);
            nm_sq_light_t *const entry=light_cache+face->light_class;
            const unsigned rear=side<0;
            if(entry->stamp[rear]==light_stamp) { light=entry->light[rear]; selected=entry->binding[rear]; }
            else {
#if NM_SH4ZAM_GFX
                light=sq_class_light(mode,face->normal,pack_materials+face->material,&light_terms,side<0);
#else
                light=sq_sided_light_values(mode,face->normal,pack_materials+face->material,inst,side<0);
#endif
                SQ_CLASS_LIGHT_PROBE(mode,face->normal,pack_materials+face->material,inst,side<0,light);
                if(light>=0) {
                    selected=sq_lookup(lookup,runs,face->material,(unsigned)light&255u);
                    entry->stamp[rear]=(uint16_t)light_stamp; entry->light[rear]=(int16_t)light; entry->binding[rear]=(uint16_t)selected;
                }
            }
        }
        if(light==-2) {
            light=face_light(pack,mesh,inst,pack->faces+mesh->face_first+f,1);
            if(light>=0) selected=sq_lookup(lookup,runs,face->material,(unsigned)light&255u);
        }
        if(light<0) return 0;
        /* reject_draw_face on the record's flags. */
        const unsigned rejection=!(flags&SQ_LIT_TWO_SIDED) && (light&256) ? 1u : !(flags&SQ_LIT_DRAWN) ? 2u : flags&SQ_LIT_CHECKER ? 3u : 0u;
        SQ_COUNT(draw_rear,rejection==1); SQ_COUNT(draw_checker,rejection==3);
        if(rejection) continue;
        if(selected==UINT16_MAX) return 0;
        const nm_sq_face_t *const corners=shared+f;
        if(inside) for(unsigned v=0;v<face->count;++v) *sq_corner_mark(marks_mid,corners->corner[v])=SQ_MARK_WANTED|SQ_MARK_INSIDE;
        else for(unsigned v=0;v<face->count;++v) *sq_corner_mark(marks_mid,corners->corner[v])|=SQ_MARK_WANTED;
        const unsigned slot=flags&SQ_LIT_CUTOUT ? face_count-1-cutout_count++ : opaque_count++;
        queue[slot]=(f<<16)|selected;
    }
    batch->opaque_count=opaque_count; batch->cutout_count=cutout_count;
    return 1;
}
#endif

/* The vertex loop. The wanted vertices in ascending order. A vertex some face of a
 * proven-inside group asked for skips classification, as before. Nothing
 * here reads a cached status: an entry is written whole (the cache line
 * is allocated without a fetch), either from its own transform or as a
 * copy of its position representative. A representative that no face
 * asked for still holds an earlier frame's projection: it is transformed
 * on first use in this batch, never trusted (fresh). */
/* Two instances: shared positions in the wide view (every batch of a frame of
 * the game: one vertex routine, no representative, no copy) and the rest
 * (per-vertex mode; bounded or checked vertex routines). */
static inline __attribute__((always_inline)) void sq_cache_points_loop(const nm_sq_walk_t *walk,const int wide_view) {
    nm_sq_batch_t *const batch=walk->batch;
    const nm_pack_t *const pack=walk->pack;
    const nm_mesh_t *const mesh=walk->mesh;
    const nm_instance_t *const inst=walk->inst;
    const nm_view_t view=walk->view;
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
    const int wide=wide_view || walk->wide,bounded=wide_view || walk->bounded;
    const nm_view_t guard=walk->guard;
#endif
    const unsigned vertex_count=mesh->vertex_count;
    uint8_t *const marks=sq_marks;
    DRAW_PHASE_START(vertices_started);
    /* Four marks at a time: most of a mesh is not asked for (rear faces,
     * rejected groups, and in shared mode every repeated position). The
     * marks past the mesh's last vertex were cleared with it. */
    for(unsigned first=0;first<vertex_count;first+=4) {
        uint32_t four;
#ifdef __DREAMCAST__
        four=*(const shz_alias_uint32_t *)(const void *)(marks+first); /* sq_marks is 32-byte aligned */
#else
        nm_memcpy(&four,marks+first,sizeof(four));
#endif
        if(!four) continue;
        for(unsigned v=first;v<first+4;++v) {
        const unsigned mark=marks[v];
        if(!(mark&SQ_MARK_WANTED)) continue;
        const int proven=(mark&SQ_MARK_INSIDE)!=0;
        /* Shared positions: every wanted vertex is a representative. */
        const unsigned representative=wide_view ? v : pack->position_refs ? pack->position_refs[mesh->vertex_first+v] : v;
        /* Source vertices are 16 bytes and visited in ascending order: ask
         * for the next cache line of the table while this one is transformed
         * (the last request of a mesh reaches into the following records of
         * the same pack file; nothing is read). */
        SQ_POINT_PREFETCH(pack->vertices+mesh->vertex_first+v+2);
#ifdef __DREAMCAST__
        shz_dcache_alloc_line(batch->points+v);
#endif
        if(representative==v) SQ_CACHE_VERTEX(v,batch->points+v,proven);
        else {
            const int fresh=(marks[representative]&(SQ_MARK_WANTED|SQ_MARK_FRESH))!=0;
            if(!fresh) {
#ifdef __DREAMCAST__
                shz_dcache_alloc_line(batch->points+representative);
#endif
                SQ_CACHE_VERTEX(representative,batch->points+representative,proven);
                marks[representative]|=SQ_MARK_FRESH;
            }
#ifdef __DREAMCAST__
            shz_memcpy32_1(batch->points+v,batch->points+representative); /* 32-byte entries, the destination line allocated above */
#else
            batch->points[v]=batch->points[representative];
#endif
            const nm_vertex_t *vertex=pack->vertices+mesh->vertex_first+v;
            batch->points[v].view.u=vertex->u;batch->points[v].view.v=vertex->v;
        }
        }
    }
    DRAW_PHASE_END(0,vertices_started);
}
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
static __attribute__((noinline)) void sq_cache_points_wide(const nm_sq_walk_t *walk) { sq_cache_points_loop(walk,1); }
#endif
static __attribute__((noinline)) void sq_cache_points_checked(const nm_sq_walk_t *walk) { sq_cache_points_loop(walk,0); }

/* ---- Prebuilt world streams, part 1: tables and the prepare step --------
 * The scenery is static and its light is baked, so what a world face hands
 * to the PVR is fixed except for its corners' x, y and depth and, for the
 * faces that need it, the side it shows. A world batch on this path
 * (NM_SQ_STREAM, the default) has no face walk and no queue: its prepare step
 * classifies the face groups (as before), chooses the level of detail of
 * each walked group (sq_lod_select, as before), marks the positions of every
 * face of the walked groups and ranges from position lists built at load,
 * and transforms them (sq_cache_points_wide, as before). The list callbacks
 * then walk the faces of their list in face order over records built at load
 * (nm_sq_stream_face_t: colour, header, depth factor, corners and texture
 * coordinates as written, side-test constants) and write each drawn face
 * straight from its record: per face the group's class, the side test where
 * one is needed, one header compare; per vertex the corner's cached x, y, q
 * and three multiplies (sq_write_stream). The decisions and their order are
 * those of the queue path (sq_walk_baked, sq_emit_face_loop, sq_emit_run),
 * so the stream is the queue path's word for word: same faces, same strip
 * runs, same headers, same clipped faces through sq_emit_face_general
 * (sq_stream_list, part 2 below).
 * Back faces stay a CPU decision: a two-sided face with two lumas needs it
 * for its colour anyway, and culling one-sided faces by winding on the PVR
 * would hand it their vertices (measured in the simulator: no gain).
 * make NATIVE_SQ_STREAM=0 builds the queue path alone (A/B on hardware). */
#ifndef NM_SQ_STREAM
#define NM_SQ_STREAM 1
#endif
#if NM_SQ_STREAM && (defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD))
#define SQ_STREAMS 1
#else
#define SQ_STREAMS 0
#endif
#if SQ_STREAMS
/* One world face, three cache lines: what every drawn face reads, its
 * texture coordinates, and its side constants. A mesh's records occupy its
 * range of the face table (after the scenery stage selection) in list order:
 * its opaque faces, then its punch-through faces, each in face order, so that
 * a list callback visits the faces of its list only (the cells with level-of-
 * detail trees interleave the two lists face by face). The mesh's rank maps a
 * face index to its record. */
typedef struct {
    uint16_t group;                 /* face.group (absolute) */
    uint8_t count, flags;           /* corners; SQ_STREAM_* */
    uint32_t argb;                  /* colour of the front binding */
    const pvr_poly_hdr_t *header;   /* pooled header of the front binding (its class) */
    float depth;                    /* class depth times the layer's factor, as the emitters form it */
    int16_t corner[4];              /* corner words (sq_corner_of), emission order: 1,2,0,3 a quad, 0,1,2 a triangle */
    uint16_t run, front;            /* faces to the end of an offline strip a run may start with (SQ_FACE_RUN), front binding */
    uint32_t strip;                 /* such a run's first strip vertex record (of the mesh's, nm_sq_stream_mesh_t) */
    float uv[4][2];                 /* u, v as written (sq_write_corner of the front class), emission order */
    nm_sq_face_plane_t side;        /* plane constant and reach (sq_face_side); reach infinite: reference test */
    float normal[3];
    uint16_t back, index;           /* binding of the rear side (UINT16_MAX: one-sided), the face's index in its mesh */
    uint32_t unused[2];
} nm_sq_stream_face_t;
#ifdef __DREAMCAST__
_Static_assert(sizeof(nm_sq_stream_face_t)==96 && offsetof(nm_sq_stream_face_t,uv)==32 && offsetof(nm_sq_stream_face_t,side)==64,
    "three cache lines per stream face");
#endif
#define SQ_STREAM_PT 1u    /* punch-through list */
#define SQ_STREAM_SIDE 2u  /* the side is tested: one-sided (rear culled) or two lumas */
#define SQ_STREAM_TWO 4u   /* two-sided with two lumas: the rear side is drawn with `back` */
/* A vertex of an offline strip for a run: u, v as written, the colour of the
 * front binding of the face whose triangle it completes (the strip's first two
 * vertices: the first face's), corner word. */
typedef struct { float uv[2]; uint32_t argb; int32_t corner; } nm_sq_stream_vertex_t;
#ifdef __DREAMCAST__
_Static_assert(sizeof(nm_sq_stream_vertex_t)==16 && offsetof(nm_sq_stream_vertex_t,argb)==8 &&
    offsetof(nm_sq_stream_vertex_t,corner)==12,"SQ_RUN_VERTEX_ASM displacements");
#endif
/* nm_sq_batch_t.stream */
#define SQ_BATCH_STREAM 1u
/* Per batch, what the list callbacks need of its prepare step. levels: the
 * class of each face group of the mesh this frame, then the level drawn of
 * each of its level-of-detail groups (draw_groups and sq_lod_state are
 * shared with the batches prepared in between). */
typedef struct {
    nm_sq_facing_t terms;
    uint8_t *levels;
    uint32_t facing; /* terms valid */
    uint32_t unused[2];
} nm_sq_stream_state_t;
/* Per mesh and list: the faces from the first of the list to the last one
 * (mesh-local face indices) and the list's records (offset from the mesh's
 * first record, count). */
typedef struct { uint16_t first, end, base, count; } nm_sq_stream_span_t;
/* Per mesh: its two spans, then where its records lie while it is on the path
 * (sq_stream_job_step): the face records in list order, the strip vertex
 * records of its offline strips (a face record's `strip` counts from the
 * mesh's first strip vertex), its position lists (sq_stream_lists' offsets
 * count from `points`) and the rank of each face. */
typedef struct {
    nm_sq_stream_span_t span[2];    /* opaque, punch-through */
    nm_sq_stream_face_t *faces;
    const nm_sq_stream_vertex_t *strips;
    const int16_t *points;
    const uint16_t *rank;           /* per face: punch-through faces of the mesh before it */
} nm_sq_stream_mesh_t;
/* Host tools: what the stream path does, by the path each face takes (the
 * simulator's per-frame model of SH4 instructions). */
#ifndef SQ_STREAM_COUNT
#define SQ_STREAM_COUNT(event,amount) ((void)0)
#endif
enum { SQ_STREAM_NOT_WALKED, SQ_STREAM_CULLED, SQ_STREAM_SIDE_TESTS, SQ_STREAM_TWO_REAR,
    SQ_STREAM_HOT_TRIANGLE, SQ_STREAM_HOT_QUAD, SQ_STREAM_HOT_HEADERS, SQ_STREAM_HELPER_FACES, SQ_STREAM_HELPER_SINGLE,
    SQ_STREAM_RUNS, SQ_STREAM_RUN_FACES, SQ_STREAM_RUN_VERTICES, SQ_STREAM_CLIPPED, SQ_STREAM_OUTSIDE, SQ_STREAM_REFERENCE,
    SQ_STREAM_STRETCHES, SQ_STREAM_LISTS, SQ_STREAM_MARKED, SQ_STREAM_GROUPS, SQ_STREAM_LOD_SELECTED, SQ_STREAM_BATCHES,
    SQ_STREAM_LEAN, SQ_STREAM_EVENTS };
static nm_sq_stream_face_t *sq_stream_faces; /* the records of every mesh (whole tables), or the record cache's pool */
static int sq_stream_wanted=1;             /* nm_render_world_streams: the scene's course takes them */
static uint32_t *sq_stream_lists;          /* first and count per face group (absolute), then per level-of-detail group and level */
static unsigned sq_stream_lod_lists;       /* index of the first level-of-detail pair in sq_stream_lists */
static uint8_t *sq_stream_meshes;          /* per mesh: 1 on this path (its records built) */
static nm_sq_stream_mesh_t *sq_stream_table; /* per mesh: spans and records */
static uint32_t *sq_stream_seen;           /* record cache: per mesh the frame (sq_stream_frame) a batch last walked it */
static uint32_t sq_stream_frame;
static nm_sq_stream_state_t *sq_stream_states; /* per world batch */
static int sq_stream_ready(const nm_sq_walk_t *walk); /* below the emitters */
/* A position only proven stream faces read (marked inside, no SQ_MARK_VIEW):
 * x, y and q of its cached entry, the arithmetic of cache_draw_vertex_wide
 * and nm_cache_vertex_wide for a point proven inside (the instance matrix in
 * XMTRX, focus, 1/z, the projection), without the view coordinates and u, v
 * that only the clipper reads. The proof (a group or batch strictly inside the
 * five planes of the guard view, nm_bounds_planes_classify's FTRV envelope
 * included) holds the near plane: no test on the console, 10 cycles a point
 * (0.2 ms in the race's heaviest frames). Host tools abort on a point behind
 * it. nm_render_init compares the two on sample points
 * (sq_stream_transform_check). */
static inline __attribute__((always_inline)) int sq_cache_point_lean(const nm_vertex_t *vertex,float focus_x,float focus_y,
        const nm_view_t *guard,nm_cached_vertex_t *out) {
    const float *const p=vertex->position;
#ifdef __DREAMCAST__
    /* shz_xmtrx_transform_vec4 of (p, 1) with the position loaded through one post-incremented pointer (the
     * same FTRV on the same fv8). */
    register float tx asm("fr8"), ty asm("fr9"), tz asm("fr10"), tw asm("fr11");
    const float *at=p;
    asm volatile("fmov.s @%[at]+,fr8\n\tfmov.s @%[at]+,fr9\n\tfmov.s @%[at],fr10\n\tfldi1 fr11\n\tftrv xmtrx,fv8"
        : "=f" (tx), "=f" (ty), "=f" (tz), "=f" (tw), [at] "+r" (at) : "m" (*(const float (*)[3])p));
    (void)tw;
    const float x=tx*focus_x,y=ty*focus_y,z=tz;
#else
    const shz_vec4_t t=shz_xmtrx_transform_vec4(shz_vec4_init(p[0],p[1],p[2],1));
    const float x=t.x*focus_x,y=t.y*focus_y,z=t.z;
    if(nm_float_negative(z-guard->near_z)) { puts("FAIL: a proven point behind the near plane"); abort(); }
#endif
#ifdef __DREAMCAST__
    /* z lies in [near_z, 2^42] (a stream batch is bounded, near_z >= 2^-10):
     * nm_cache_vertex_wide's range test, which picks FSRRA, holds. Its
     * operations (shz_invf_fsrra: FSRRA of z*z), the projection and the
     * three stores, written out: q, y, x stored last to first. */
    float square=z,px=x,py=y,cy=guard->center[1];
    shz_alias_float_t *end=(shz_alias_float_t *)(void *)(out+1);
    asm volatile(
        "fmul %[q],%[q]\n\t"
        "fsrra %[q]\n\t"
        "fmul %[q],%[x]\n\t"
        "fmul %[q],%[y]\n\t"
        "fadd %[cx],%[x]\n\t"
        "fsub %[y],%[cy]\n\t"
        "fmov.s %[q],@-%[end]\n\t"
        "fmov.s %[cy],@-%[end]\n\t"
        "fmov.s %[x],@-%[end]\n"
        : [q] "+f" (square), [x] "+f" (px), [y] "+f" (py), [cy] "+f" (cy), [end] "+r" (end)
        : [cx] "f" (guard->center[0])
        : "memory");
#else
    const float q=1.0f/z;
    out->x=guard->center[0]+x*q; out->y=guard->center[1]-y*q; out->q=q;
#endif
    return 1;
}
/* The whole cached entry of one position (cache_draw_vertex_wide), out of
 * line: the lean loop below then keeps nothing of it in registers. */
static __attribute__((noinline)) void sq_cache_point_whole(const nm_sq_walk_t *walk,unsigned v,unsigned mark) {
    cache_draw_vertex_wide(walk->pack,walk->mesh,walk->inst,&walk->guard,v,walk->batch->points+v,(mark&SQ_MARK_INSIDE)!=0);
}
/* The vertex loop of a stream batch: sq_cache_points_wide's, with the lean
 * form for the positions only proven faces hold. The marks are read four at
 * a time as one aligned word (sq_marks is 32-byte aligned; the loop of
 * sq_cache_points_wide reads them through a call to memcpy). */
static __attribute__((noinline)) void sq_cache_points_stream(const nm_sq_walk_t *walk) {
    const nm_mesh_t *const mesh=walk->mesh;
    const nm_view_t guard=walk->guard;
    const nm_vertex_t *vertex=walk->pack->vertices+mesh->vertex_first;
    nm_cached_vertex_t *point=walk->batch->points;
    const float focus_x=walk->inst->focus[0],focus_y=walk->inst->focus[1];
    const uint8_t *const marks=sq_marks;
    const unsigned words=(mesh->vertex_count+3u)/4u;
    DRAW_PHASE_START(vertices_started);
    for(unsigned word=0;word<words;++word,vertex+=4,point+=4) {
        uint32_t four;
#ifdef __DREAMCAST__
        four=((const shz_alias_uint32_t *)(const void *)marks)[word];
#else
        nm_memcpy(&four,marks+4u*word,sizeof(four));
#endif
        if(!four) continue;
        for(unsigned k=0;k<4;++k) {
            const unsigned mark=marks[4u*word+k];
            if(!(mark&SQ_MARK_WANTED)) continue;
            SQ_POINT_PREFETCH(vertex+k+2);
#ifdef __DREAMCAST__
            shz_dcache_alloc_line(point+k);
#endif
#ifndef NATIVE_BOUNDS_VERIFY
            if((mark&(SQ_MARK_INSIDE|SQ_MARK_VIEW))==SQ_MARK_INSIDE && sq_cache_point_lean(vertex+k,focus_x,focus_y,&guard,point+k)) {
                SQ_COUNT(draw_transformed,1); SQ_STREAM_COUNT(SQ_STREAM_LEAN,1);
                continue;
            }
#endif
            sq_cache_point_whole(walk,4u*word+k,mark);
        }
    }
    DRAW_PHASE_END(0,vertices_started);
}
#if defined(__DREAMCAST__) && !defined(NATIVE_BOUNDS_VERIFY) /* that build has no lean form, and these points no proof */
/* Before the first frame: the lean form against cache_draw_vertex_wide on
 * points in front of the near plane, through three instance matrices, bit
 * for bit in x, y and q. */
static __attribute__((noinline)) int sq_stream_transform_check(void) {
    static const float angles[3]={0.0f,0.7f,2.3f};
    alignas(32) nm_vertex_t vertices[12];
    nm_mesh_t mesh; nm_memset(&mesh,0,sizeof(mesh)); mesh.vertex_count=12;
    nm_pack_t pack; nm_memset(&pack,0,sizeof(pack)); pack.vertices=vertices;
    const nm_view_t guard={{248.0f,192.0f},{-1024.0f,1520.0f,-1024.0f,1408.0f},0.01f};
    for(unsigned k=0;k<12;++k) {
        vertices[k].position[0]=(float)k*7.3f-40.0f; vertices[k].position[1]=(float)(k%3)*4.1f-3.0f;
        vertices[k].position[2]=12.0f+(float)k*13.7f; vertices[k].u=(uint16_t)(k*97u); vertices[k].v=(uint16_t)(k*31u);
    }
    unsigned cases=0;
    for(unsigned a=0;a<3;++a) {
        alignas(32) nm_instance_t inst;
        nm_memset(&inst,0,sizeof(inst));
        const shz_sincos_t sc=shz_sincosf(angles[a]); /* FSCA: any three rotations serve this self-check */
        const float c=sc.cos,s=sc.sin;
        const float m[12]={c,0,-s, 0,1,0, s,0,c, 3.5f,-1.25f,60.0f+20.0f*(float)a};
        nm_memcpy(inst.matrix,m,sizeof(m));
        inst.focus[0]=310.0f; inst.focus[1]=296.0f;
        shz_xmtrx_load_3x4(&inst.transform);
        for(unsigned v=0;v<12;++v) {
            alignas(32) nm_cached_vertex_t full,lean;
            nm_memset(&full,0,sizeof(full)); nm_memset(&lean,0,sizeof(lean));
            cache_draw_vertex_wide(&pack,&mesh,&inst,&guard,v,&full,1);
            if(nm_cached_outside(&full)) continue;
            if(!sq_cache_point_lean(vertices+v,inst.focus[0],inst.focus[1],&guard,&lean) ||
               memcmp(&full.x,&lean.x,sizeof(float)) || memcmp(&full.y,&lean.y,sizeof(float)) || memcmp(&full.q,&lean.q,sizeof(float))) {
                printf("FAIL: lean world stream transform differs from cache_draw_vertex_wide (matrix %u, point %u)\n",a,v);
                return 0;
            }
            ++cases;
        }
    }
    printf("Native world stream transform PASS: the lean form equals cache_draw_vertex_wide's x, y, q in %u cases\n",cases);
    return cases>0;
}
#endif
/* Every point of the mesh nearer than the sky: the view-space depth of the
 * farthest corner of its bounds below half the sky's 1/q, so that a point
 * proven inside the guard view is drawable (sq_point_drawable) without a
 * test. Bounded batches only (finite terms). */
static int sq_stream_far(const nm_instance_t *inst,const nm_mesh_t *mesh) {
    const float *const m=inst->matrix;
    float z=m[11];
    for(unsigned a=0;a<3;++a) {
        const float low=m[2+3*a]*mesh->bounds[a],high=m[2+3*a]*mesh->bounds[a+3];
        z+=low>high ? low : high;
    }
    return nm_float_negative(z*1.0625f+1.0f-0.5f/NM_SKY_DEPTH);
}
/* The prepare step of a stream batch, after sq_classify_groups: levels of
 * detail and marks. A position is marked proven inside (no classification)
 * when a group that holds it is, as sq_queue_face marks it. The class kept
 * for the list callbacks: 0 the group is not drawn, 1 its faces test their
 * corners, 2 (SQ_CLASS_PROVEN) they do not: the group (or the batch) lies
 * inside the guard view and the mesh nearer than the sky. */
static __attribute__((noinline)) void sq_stream_batch_prepare(const nm_sq_walk_t *walk) {
    const nm_sq_batch_t *const batch=walk->batch;
    const nm_mesh_t *const mesh=walk->mesh;
    const nm_instance_t *const inst=walk->inst;
    const unsigned mesh_groups=mesh->padding[1],group_first=mesh->padding[0];
    uint8_t *const classes=sq_stream_states[batch-sq_sources[1].batches].levels;
    uint8_t *const levels=classes+mesh_groups;
    const uint8_t *const groups=draw_groups;
    const uintptr_t marks_mid=SQ_MARKS_MID(sq_marks);
    const int batch_proven=walk->batch_proven,far=sq_stream_far(inst,mesh);
    SQ_STREAM_COUNT(SQ_STREAM_BATCHES,1);
    const uint32_t *const lists=sq_stream_lists;
    const int16_t *const points=sq_stream_table[mesh-walk->pack->meshes].points;
    for(unsigned g=0;g<mesh_groups;++g) {
        const unsigned group=groups[g];
        const unsigned class=!group ? 0u : far && (batch_proven || group==2) ? 2u : 1u;
        classes[g]=(uint8_t)class;
        if(!group) continue;
        const int16_t *point=points+lists[2u*(group_first+g)];
        const int16_t *const end=point+lists[2u*(group_first+g)+1u];
        SQ_STREAM_COUNT(SQ_STREAM_MARKED,end-point); SQ_STREAM_COUNT(SQ_STREAM_GROUPS,1);
        const unsigned mark=SQ_MARK_WANTED|(batch_proven || group==2 ? SQ_MARK_INSIDE : 0u)|(class==1 ? SQ_MARK_VIEW : 0u);
        for(;point!=end;++point) *sq_corner_mark(marks_mid,*point)|=(uint8_t)mark;
    }
    const unsigned lod_count=sq_lod_groups ? mesh->padding[4]>>16 : 0u;
    if(lod_count) {
        const unsigned lod_first=mesh->padding[4]&0xffffu;
        const float row[4]={inst->matrix[2],inst->matrix[5],inst->matrix[8],inst->matrix[11]};
        const float scale=NM_LOD_REFERENCE_FOCUS/inst->focus[1];
#if SQ_SWEEPS
        sq_lod_mesh=mesh;
#endif
        for(unsigned i=0;i<lod_count;++i) {
            const unsigned group=groups[sq_lod_groups[lod_first+i].bounds];
            if(!group) continue; /* outside the view: no shape, and the level stays */
            const unsigned level=sq_lod_select(lod_first+i,row,scale);
            levels[i]=(uint8_t)level;
#if SQ_SWEEPS
            if(level>=SQ_SWEEP) {
                /* A panel across a sweep plane: its levels share the four
                 * corners; the first level in n0..n1 with a face lists them.
                 * The whole transform (SQ_MARK_VIEW): the clip reads their
                 * view coordinates (x, y, q are the lean form's bits). */
                const nm_sq_lod_t *const swept=sq_lod_groups+lod_first+i;
                unsigned n=SQ_SWEEP_FIRST(level);
                while(n<SQ_SWEEP_LAST(level) && !swept->count[n]) ++n;
                const uint32_t *const pair=lists+sq_stream_lod_lists+2u*(3u*(lod_first+i)+n);
                const int16_t *point=points+pair[0];
                const int16_t *const end=point+pair[1];
                SQ_STREAM_COUNT(SQ_STREAM_MARKED,end-point); SQ_STREAM_COUNT(SQ_STREAM_LOD_SELECTED,1);
                const unsigned mark=SQ_MARK_WANTED|SQ_MARK_VIEW|(batch_proven || group==2 ? SQ_MARK_INSIDE : 0u);
                for(;point!=end;++point) *sq_corner_mark(marks_mid,*point)|=(uint8_t)mark;
                continue;
            }
#endif
            const uint32_t *const pair=lists+sq_stream_lod_lists+2u*(3u*(lod_first+i)+level);
            const int16_t *point=points+pair[0];
            const int16_t *const end=point+pair[1];
            SQ_STREAM_COUNT(SQ_STREAM_MARKED,end-point); SQ_STREAM_COUNT(SQ_STREAM_LOD_SELECTED,1);
            const unsigned class=far && (batch_proven || group==2) ? 2u : 1u;
            const unsigned mark=SQ_MARK_WANTED|(batch_proven || group==2 ? SQ_MARK_INSIDE : 0u)|(class==1 ? SQ_MARK_VIEW : 0u);
            for(;point!=end;++point) *sq_corner_mark(marks_mid,*point)|=(uint8_t)mark;
        }
    }
#ifdef NATIVE_BOUNDS_VERIFY
    /* The faces of rejected groups, as sq_walk_baked checks them. */
    for(unsigned f=0;f<mesh->face_count;++f) {
        const nm_face_t *const face=walk->pack->faces+mesh->face_first+f;
        if(!groups[face->group-group_first]) sq_verify_rejected_face(walk->pack,mesh,inst,&walk->view,face);
    }
#endif
}
#endif

/* ---- Prebuilt car streams, part 1: tables and the prepare step ----------
 * A car part (one mesh of the car pack: a body at one of the arcade's detail
 * levels, a wheel, a player-car extra, a crash attachment) whose bounds lie
 * inside the guard view and nearer than the sky (every one of its points is
 * drawable without a test) has no face walk in its prepare step, no queue
 * and no vertex marks: the prepare step transforms the part's distinct
 * positions (the lean x, y, q form of the world streams, sq_car_batch_prepare)
 * and each list callback takes the faces of its list (a car mesh is in list
 * order, DNM6: the opaque faces, then the punch-through ones) in two passes
 * (sq_car_list). The decide pass (sq_car_decide) makes sq_walk_car's
 * decisions per face: the side from the plane constants (sq_face_side), a
 * rear of a one-sided face dropped, the light class entry of the side shown
 * (the class's light evaluated on its first use in the callback and resolved
 * once to its binding, colour and emission class: sq_car_resolve_body), and
 * lists the drawn faces. The write pass (sq_car_write) writes them as
 * sq_emit_face_loop does: the header compare and the vertices
 * (sq_car_put_face_sh4: x, y, q of the corner's position, u and v as the
 * queue path writes them from the face's emission record and the class's UV
 * mapping); strip runs as sq_emit_run (sq_car_run). The decisions and their
 * order are the queue path's, so the stream is its stream word for word.
 * (One pass, deciding and writing each face in turn, measured more SH4
 * instructions: the vertex writer and the light evaluation then share the
 * registers of one loop.)
 * The per-face records are the queue path's own (nm_sq_lit_face_t: plane
 * constants, normal, light class, material, list and side flags;
 * nm_sq_face_t: corners, u, v, layer, strip run, rewritten u, v): a second
 * copy with u, v already scaled would cost 0.4 MB, which the heap budget of a
 * stage 6 load with the sample profile does not have; the integer form costs
 * two conversions a vertex. Window reflections (nm_render_uv_dynamic) are
 * read from the vertex table when drawn, as on the queue path.
 * Parts that straddle the near plane or the guard band, parts without plane
 * constants (sq_facing_batch), the inspection views and meshes the load step
 * refuses take the queue path (sq_walk_car, sq_emit_faces_op/pt).
 * make NATIVE_SQ_CAR_STREAM=0 builds the queue path alone (A/B on hardware);
 * NATIVE_SQ_STREAM=0 turns both stream paths off. */
#ifndef NM_SQ_CAR_STREAM
#define NM_SQ_CAR_STREAM 1
#endif
#if SQ_STREAMS && NM_SQ_CAR_STREAM && !defined(NATIVE_LIGHT_VERIFY) && !defined(NM_DIAG_CAR_OPAQUE)
#define SQ_CAR_STREAMS 1
#else
#define SQ_CAR_STREAMS 0
#endif
#if SQ_CAR_STREAMS
#ifndef SQ_GENERAL_EMITTER
#define SQ_GENERAL_EMITTER 0 /* (host tools may force the general emitter: see sq_general_emitter) */
#endif
/* nm_sq_batch_t.stream: a car part on the car stream. */
#define SQ_BATCH_CAR 2u
/* Faces of a car mesh a strip run may take: the run's colours wait in a
 * table of this size (sq_car_run). The largest car mesh has 325 faces. */
#define SQ_CAR_RUN_MAX 512u
/* Per car mesh: its first position in sq_car_positions (UINT32_MAX: the
 * mesh is not on the car stream), how many, and its first punch-through face. */
typedef struct { uint32_t positions; uint16_t position_count, cutouts; } nm_sq_car_mesh_t;
static nm_sq_car_mesh_t *sq_car_meshes;
static uint16_t *sq_car_positions;  /* per mesh, its distinct positions: representatives in ascending order */
static nm_sq_facing_t *sq_car_facing; /* per car batch: the side-test terms of its prepare step */
/* Host tools: what the car stream does (the simulator's per-frame model of SH4 instructions). */
#ifndef SQ_CAR_COUNT
#define SQ_CAR_COUNT(event,amount) ((void)0)
#endif
enum { SQ_CAR_SIDES, SQ_CAR_CULLED, SQ_CAR_REFERENCE, SQ_CAR_MISSES, SQ_CAR_OWN, SQ_CAR_HOT_TRIANGLE, SQ_CAR_HOT_QUAD,
    SQ_CAR_HOT_HEADERS, SQ_CAR_HELPER, SQ_CAR_HELPER_SINGLE, SQ_CAR_RUNS, SQ_CAR_RUN_FACES, SQ_CAR_RUN_VERTICES,
    SQ_CAR_DYNAMIC, SQ_CAR_LISTS, SQ_CAR_BATCHES, SQ_CAR_LEAN, SQ_CAR_WHOLE, SQ_CAR_EVENTS };
/* On the car stream: the queue path's conditions for its fastest form
 * (shared positions in the wide view, sides from plane constants) plus the
 * world stream's for proven faces (sq_stream_batch_prepare, class 2). */
static int sq_car_ready(const nm_sq_walk_t *walk) {
    return sq_car_meshes && walk->wide && walk->batch_proven && walk->facing &&
        sq_car_meshes[walk->mesh-walk->pack->meshes].positions!=UINT32_MAX &&
        !draw_inspect_strips && !draw_untextured && !SQ_GENERAL_EMITTER && sq_stream_far(walk->inst,walk->mesh);
}
/* The prepare step of a car stream batch: every distinct position of the
 * part, the lean form (proven inside, so no point test). XMTRX holds the
 * instance matrix. */
static __attribute__((noinline)) void sq_car_batch_prepare(const nm_sq_walk_t *walk) {
    const nm_mesh_t *const mesh=walk->mesh;
    const nm_sq_car_mesh_t *const info=sq_car_meshes+(mesh-walk->pack->meshes);
    const uint16_t *position=sq_car_positions+info->positions;
    const uint16_t *const end=position+info->position_count;
    const nm_view_t guard=walk->guard;
    const nm_vertex_t *const vertices=walk->pack->vertices+mesh->vertex_first;
    nm_cached_vertex_t *const points=walk->batch->points;
    const float focus_x=walk->inst->focus[0],focus_y=walk->inst->focus[1];
    SQ_CAR_COUNT(SQ_CAR_BATCHES,1);
    DRAW_PHASE_START(vertices_started);
    /* Positions below 2^15 (SQ_CORNER_VERTICES): read as int16 (no zero extension), each address formed once. */
    for(const int16_t *at=(const int16_t *)(const void *)position,*const stop=(const int16_t *)(const void *)end;at!=stop;++at) {
        const int v=*at;
        SQ_POINT_PREFETCH(vertices+at[2]); /* the list has two entries of padding */
        const nm_vertex_t *const vertex=vertices+v;
        nm_cached_vertex_t *const point=points+v;
#ifdef __DREAMCAST__
        shz_dcache_alloc_line(point);
#endif
#ifndef NATIVE_BOUNDS_VERIFY
        if(sq_cache_point_lean(vertex,focus_x,focus_y,&guard,point)) {
            SQ_COUNT(draw_transformed,1); SQ_CAR_COUNT(SQ_CAR_LEAN,1);
            continue;
        }
#endif
        SQ_CAR_COUNT(SQ_CAR_WHOLE,1);
        sq_cache_point_whole(walk,(unsigned)v,SQ_MARK_WANTED|SQ_MARK_INSIDE);
    }
    DRAW_PHASE_END(0,vertices_started);
}
#endif

static int sq_prepare_batch(nm_sq_batch_t *batch) {
    nm_sq_walk_t walk;
    const unsigned source=batch->source;
    DRAW_PROFILE_SOURCE(source);
    const nm_pack_t *pack=source ? world_draw.pack : draw_pack;
    const nm_instance_t *inst=pack->instances+batch->instance_index;
    const nm_mesh_t *mesh=batch->mesh;
    const int bounds=batch->bounds;
    walk.batch=batch; walk.pack=pack; walk.mesh=mesh; walk.inst=inst; walk.source=source;
    walk.materials=source ? world_draw.materials : draw_materials;
    walk.lookup=source ? world_draw.bindings : draw_bindings; walk.runs=source ? NULL : draw_runs;
    walk.view=batch->view; walk.bounds=bounds; walk.material_lists=sq_material_lists[source];
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
    walk.bounded=sq_batch_bounded(inst,mesh,&walk.view);
    walk.wide=walk.bounded && sq_view_is_screen(&walk.view);
    walk.guard=walk.view;
    walk.guard.bounds[0]-=SQ_GUARD_BAND; walk.guard.bounds[1]+=SQ_GUARD_BAND;
    walk.guard.bounds[2]-=SQ_GUARD_BAND; walk.guard.bounds[3]+=SQ_GUARD_BAND;
    /* An instance that straddles the screen but lies inside the guard view
     * needs no point test at all. */
    /* Inside the guard view: nm_bounds_quick_classify first (2 only where the guard planes say 2); the planes
     * otherwise, and they are kept for the face groups (sq_classify_groups reads them only while the batch is
     * not proven). */
    walk.guard_planes=NULL;
    int batch_proven=bounds==2;
    if(!batch_proven && walk.wide) {
        if(nm_bounds_quick_classify(&walk.guard,mesh->bounds,inst->matrix,inst->focus)==2) { batch_proven=1; SQ_PROBE(guard_quick); }
        else {
            walk.guard_planes=sq_guard_planes(&walk.guard,inst);
            batch_proven=nm_bounds_planes_classify(walk.guard_planes,mesh->bounds)==2;
        }
    }
#else
    const int batch_proven=bounds==2;
#endif
    walk.batch_proven=batch_proven;
    if(source) { if(batch_proven) SQ_PROBE(world_batches_proven); else SQ_PROBE(world_batches_tested); }
    else { if(batch_proven) SQ_PROBE(car_batches_proven); else SQ_PROBE(car_batches_tested); }
#if SQ_CAR_STREAMS
    /* A car part on the prebuilt car stream: no face walk, no queue, no
     * marks. Its side-test terms (sq_facing_batch, as below) are kept for the
     * list callbacks. */
    if(!source && sq_car_meshes) {
        walk.face_planes=sq_face_planes[0] ? sq_face_planes[0]+mesh->face_first : NULL;
        walk.facing=walk.face_planes && sq_facing_batch(&walk.facing_terms,inst,mesh,1) ? &walk.facing_terms : NULL;
        if(sq_car_ready(&walk)) {
            sq_car_facing[batch-sq_sources[0].batches]=walk.facing_terms;
            batch->stream=SQ_BATCH_CAR;
            shz_xmtrx_load_3x4(&inst->transform);
            SQ_PATH_POINTS(0,batch); sq_car_batch_prepare(&walk);
            batch->view=walk.guard;
            batch->prepared=1;
            return 1;
        }
    }
#endif
    if(mesh->vertex_count>SQ_MARK_VERTICES || !sq_marks_reserve(mesh->vertex_count)) return 0;
    /* sq_marks is 32-byte aligned and holds (vertex_count+4+31)&~31 bytes (sq_marks_reserve): a multiple of 8. */
    nm_zero8(sq_marks,(mesh->vertex_count+7u)&~7u);
    NM_FT_BEGIN(ft_classify);
    sq_classify_groups(&walk);
    NM_FT_END(NM_FT_P_CLASSIFY,ft_classify);
    shz_xmtrx_load_3x4(&inst->transform);
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
    /* Face sides from plane constants (sq_face_side): the lit cars and the
     * baked scenery. NULL: the reference test on every face. */
    walk.face_planes=sq_face_planes[source] ? sq_face_planes[source]+mesh->face_first : NULL;
    walk.facing=walk.face_planes && sq_facing_batch(&walk.facing_terms,inst,mesh,!source) ? &walk.facing_terms : NULL;
#if SQ_STREAMS
    /* The record cache (sq_stream_idle): the meshes the frame walks. */
    if(source && sq_stream_seen) sq_stream_seen[mesh-pack->meshes]=sq_stream_frame;
    /* A world batch on the prebuilt stream: no face walk, no queue. */
    if(source && sq_stream_ready(&walk)) {
        nm_sq_stream_state_t *const state=sq_stream_states+(batch-sq_sources[1].batches);
        state->facing=walk.facing!=NULL;
        if(walk.facing) state->terms=walk.facing_terms;
        NM_FT_BEGIN(ft_marks);
        sq_stream_batch_prepare(&walk);
        NM_FT_END(NM_FT_P_MARKS,ft_marks);
        batch->stream=SQ_BATCH_STREAM;
        NM_FT_BEGIN(ft_points);
        SQ_PATH_POINTS(0,batch); sq_cache_points_stream(&walk);
        NM_FT_END(NM_FT_P_POINTS,ft_points);
        batch->view=walk.guard;
        batch->prepared=1;
        return 1;
    }
#endif
    /* Cars with every table of the production path take the loop on hot
     * records; NATIVE_LIGHT_VERIFY and the car-opaque diagnostic keep the
     * general lit loop, which checks and decides per face. */
#if defined(NATIVE_LIGHT_VERIFY) || defined(NM_DIAG_CAR_OPAQUE)
    const int car_records=0;
#else
    const int car_records=!source && walk.facing && sq_lit_faces && sq_emit_faces[0] && walk.material_lists;
#endif
    /* The world's material/luma table is gone once it is attached (nm_render_world_attach): baked faces only. */
    if(source && !sq_baked_faces && !walk.lookup) return 0;
    NM_FT_BEGIN(ft_walk);
    if(!(source && sq_baked_faces ? sq_walk_baked(&walk) : car_records ? sq_walk_car(&walk) : sq_walk_lit(&walk))) return 0;
    NM_FT_END(NM_FT_P_WALK,ft_walk);
#else
    if(!sq_walk_lit(&walk)) return 0;
#endif
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
    NM_FT_BEGIN(ft_wide);
    if(walk.wide && sq_emit_faces[source]) { SQ_PATH_POINTS(0,batch); sq_cache_points_wide(&walk); }
    else { SQ_PATH_POINTS(1,batch); sq_cache_points_checked(&walk); }
    NM_FT_END(NM_FT_P_WIDE,ft_wide);
    /* The emitter's clipper must use the view the points were classified
     * against: from here on the batch carries the guard view. */
    if(walk.wide) batch->view=walk.guard;
#else
    sq_cache_points_checked(&walk);
#endif
    batch->prepared=1;
    return 1;
}

/* NATIVE_DIAG_PMU=1 (measurement builds only; nothing else compiles this):
 * the two SH4 performance counters code/practice.c programs for the frame are
 * also read around each group of the model renderer's work in the list
 * callbacks, so that its per-frame PMU lines split registration by loop:
 * the prepare step of a world stream batch (sq_classify_groups,
 * sq_stream_batch_prepare, sq_cache_points_stream), of a car stream batch
 * (sq_car_batch_prepare), of a queue-path batch (face walk and vertex loop);
 * the world stream's opaque and punch-through lists (sq_stream_list), the car
 * stream's (sq_car_decide, sq_car_write), and the queue path's emitters.
 * practice.c folds this frame's sums into its per-phase table when the
 * registration ends and prints them with its own PMU lines; registration less
 * these groups is everything else (sky, HUD, shadows, enDjinn's lists). Six
 * counter reads (each three uncached register reads) per group instance. */
#ifdef NM_DIAG_PMU
enum { SQ_PMU_WORLD_PREPARE, SQ_PMU_CAR_PREPARE, SQ_PMU_QUEUE_PREPARE, SQ_PMU_WORLD_OP, SQ_PMU_WORLD_PT,
    SQ_PMU_CAR_OP, SQ_PMU_CAR_PT, SQ_PMU_QUEUE_EMIT, SQ_PMU_GROUPS };
const unsigned nm_sq_pmu_groups=SQ_PMU_GROUPS;
const char *const nm_sq_pmu_names[SQ_PMU_GROUPS]={"world-prepare","car-prepare","queue-prepare","world-op","world-pt",
    "car-op","car-pt","queue-emit"};
uint64_t nm_sq_pmu_frame[SQ_PMU_GROUPS][2]; /* this frame: the two counters' sums per group */
uint32_t nm_sq_pmu_calls[SQ_PMU_GROUPS];    /* this frame: group instances */
#define SQ_PMU_BEGIN(name) const uint64_t name##0=perf_cntr_count(PRFC0),name##1=perf_cntr_count(PRFC1)
#define SQ_PMU_END(name,group) do { const unsigned pmu_group=(group); \
    nm_sq_pmu_frame[pmu_group][0]+=perf_cntr_count(PRFC0)-name##0; nm_sq_pmu_frame[pmu_group][1]+=perf_cntr_count(PRFC1)-name##1; \
    ++nm_sq_pmu_calls[pmu_group]; } while(0)
#else
#define SQ_PMU_BEGIN(name) ((void)0)
#define SQ_PMU_END(name,group) ((void)0)
#endif

#ifndef NM_RENDER_BOUNDS_HINT
#define NM_RENDER_BOUNDS_HINT UINT32_C(0x4e480000) /* model_renderer.h */
#endif
/* World objects super-sampled by instance (meshpack.h, nm_material_supersample_depth: tools/lod_groups.py, a
 * "supersample" rule with "group": "instance"; the slot machine's reels). The far level of a level-of-detail group
 * repeats its faces under headers with the TSP super-sample bit, which for the reels' 64 meshes of 3 faces cost 192
 * more faces, groups and their stream records. Here a mesh keeps its faces and materials: a world instance of a mesh
 * whose materials carry a switch depth is drawn super-sampled while its model origin is beyond it, and the emitters
 * submit its faces under the same pooled headers with the bit set.
 *   Decision (sq_ss_select, in sq_prepare_source, once per visible instance and frame): the depth of the model
 *     origin through the z row of the instance matrix, times NM_LOD_REFERENCE_FOCUS over the focus, against the
 *     depth with the hysteresis of sq_lod_level (on beyond depth*NM_LOD_HYSTERESIS, off again below
 *     depth/NM_LOD_HYSTERESIS); the level drawn last is kept per mesh in sq_ss_level, as a group's in sq_lod_state
 *     (the reels' 192 groups had their centres at the model origin: the same arithmetic, the same switches).
 *   Headers (sq_ss_prepare, at load): the pooled headers of every binding the faces of such meshes submit
 *     (sq_ss_headers, front and rear), which no other face of either source may submit: they are written as the
 *     pool holds them, so sq_ss_batch sets or clears the bit in the pool itself when a batch of such a mesh wants the
 *     other state than the pool has (sq_ss_on), and drops the emitter's last header so that the next face writes
 *     its header again. Nothing else changes per face; batches of other meshes do not look at it.
 * make NATIVE_SUPERSAMPLE_FAR=0 leaves them plain, as it leaves the far levels (host tools: on, as the default
 * build). The reference (tests/reference_stream.h) makes the decision on its own in double precision and checks the
 * bit of the header each triangle is submitted under. */
#ifndef SQ_OBJECT_SUPERSAMPLE
#ifdef NM_SUPERSAMPLE_FAR
#define SQ_OBJECT_SUPERSAMPLE NM_SUPERSAMPLE_FAR
#else
#define SQ_OBJECT_SUPERSAMPLE 1
#endif
#endif
#define SQ_SS_PLAIN 1u   /* nm_sq_batch_t.supersample: such a mesh, drawn without the bit */
#define SQ_SS_ON 2u      /* with it */
#define SQ_SS_HEADERS_MAX 16u
#define SQ_TSP_SUPER_SAMPLE (1u<<12) /* model_renderer.c, NM_TSP_SUPER_SAMPLE */
/* A header in normal modifier mode (KOS PVR_TA_CMD_MODIFIER | PVR_TA_CMD_MODIFIERMODE) has a second TSP word for
 * inside the volumes; sq_ss_prepare refuses one (the shadow receivers are cheap-shadow road surface). */
#define SQ_TA_CMD_MODIFIER_NORMAL 0xc0u
static uint8_t *sq_ss_level;   /* per world mesh: 0 no switch depth, else SQ_SS_PLAIN or SQ_SS_ON as drawn last */
static pvr_poly_hdr_t *sq_ss_headers[SQ_SS_HEADERS_MAX];
static unsigned sq_ss_header_count,sq_ss_on;
static void sq_ss_release(void) {
    /* The pool is rebuilt with the world: nothing to clear in it. */
    free(sq_ss_level); sq_ss_level=NULL; sq_ss_header_count=sq_ss_on=0;
}
/* The state an instance of mesh `m` is drawn in this frame (see above). */
static __attribute__((noinline)) unsigned sq_ss_select(const nm_pack_t *pack,const nm_instance_t *inst,unsigned m) {
    const nm_mesh_t *const mesh=pack->meshes+m;
    const float depth=nm_material_supersample_depth(pack->materials+pack->faces[mesh->face_first].material);
    const float scaled=inst->matrix[11]*(NM_LOD_REFERENCE_FOCUS/inst->focus[1]);
    /* sq_lod_level's arithmetic for two levels (the far side against depth on, the near side against it off). */
    const float beyond=scaled*(1.0f/NM_LOD_HYSTERESIS),within=scaled*NM_LOD_HYSTERESIS;
    unsigned level=sq_ss_level[m];
    if(level==SQ_SS_PLAIN && beyond>depth) level=SQ_SS_ON;
    else if(level==SQ_SS_ON && within<depth) level=SQ_SS_PLAIN;
    sq_ss_level[m]=(uint8_t)level;
    return level;
}
/* Before a batch's faces on a list: the pooled headers in the state the batch wants (see above). */
static inline void sq_ss_batch(const nm_sq_batch_t *batch,const pvr_poly_hdr_t **previous) {
    if(!batch->supersample || (batch->supersample==SQ_SS_ON)==sq_ss_on) return;
    sq_ss_on=batch->supersample==SQ_SS_ON;
    for(unsigned i=0;i<sq_ss_header_count;++i) {
        pvr_poly_hdr_t *const header=sq_ss_headers[i];
        header->mode2^=SQ_TSP_SUPER_SAMPLE;
    }
    *previous=NULL;
}
/* Visible instances of one source become batches; faces and vertices are
 * prepared in the object's first list callback (sq_submit_object). */
/* In line, as they were before the view's call took them over GCC's limits: practice_prepare's slot (config/hot_layout.tsv). */
static inline __attribute__((always_inline)) int sq_prepare_source(unsigned source, const nm_pack_t *pack);
static inline __attribute__((always_inline)) int sq_prepare(void);
/* Out of line: a frame's hot path holds a call, not its four libm calls. */
static __attribute__((noinline)) void sq_instance_view(const nm_pack_t *pack,const nm_instance_t *inst,nm_view_t *out) {
    *out=instance_view(pack,inst);
}
static int sq_prepare_source(unsigned source, const nm_pack_t *pack) {
    DRAW_PROFILE_SOURCE(source);
    unsigned points_used=0,faces_used=0;
    if(source) {
        nm_view_t view={0};
        if(pack->header->instances) sq_instance_view(pack,pack->instances,&view);
        nm_scenery_camera_prepare(&sq_scenery_camera,pack,&view);
    }
    sq_sources[source].count=0;
    if(!source) sq_tr_count=0;
    /* Bounds planes depend on matrix, focus and view alone. Consecutive
     * instances that agree in all three (the parts of one car) use the
     * planes of the first. */
    nm_bounds_planes_t planes;
    const nm_instance_t *planes_instance=NULL;
    nm_view_t planes_view;
    /* Consecutive instances in one window (every part of every car) share their view: instance_view's four libm calls
     * and their classifications once, not per instance. */
    nm_view_t window_view;
    int16_t window[6]={0,0,0,0,0,0};
    int window_valid=0,window_view_valid=0;
    for(unsigned i=0;i<pack->header->instances;++i) {
        DRAW_PHASE_START(bounds_started);
        const nm_instance_t *inst=pack->instances+i;
        const nm_mesh_t *mesh=pack->meshes+inst->mesh;
        const int shared_view=source && sq_scenery_camera.shared;
        if(!shared_view) {
            const int16_t *const v=inst->viewport,*const c=inst->center;
            if(!window_valid || ((v[0]^window[0])|(v[1]^window[1])|(v[2]^window[2])|(v[3]^window[3])|(c[0]^window[4])|(c[1]^window[5]))) {
                sq_instance_view(pack,inst,&window_view); window_valid=1;
                for(unsigned k=0;k<4;++k) window[k]=v[k];
                window[4]=c[0]; window[5]=c[1];
                window_view_valid=nm_view_valid(&window_view);
            }
        }
        const nm_view_t view=shared_view ? sq_scenery_camera.view : window_view;
        /* (sq_scenery_camera.shared: nm_scenery_camera_prepare found its view valid) */
        if(!shared_view && !window_view_valid) { DRAW_PHASE_END(11,bounds_started); continue; }
        /* nm_bounds_quick_classify first: when it decides (0 or 2) the planes would decide the same, at a tenth
         * of their cost (the pit crew's hundred parts a frame, each with its own matrix). */
        int bounds;
        if(source && sq_scenery_camera.shared) bounds=nm_bounds_planes_classify(&sq_scenery_camera.planes,mesh->bounds);
        else if(!source && (inst->padding[0]&UINT32_C(0xfffffffc))==NM_RENDER_BOUNDS_HINT) {
            /* The scene's classification (model_renderer.h): the same as the planes'. */
            bounds=(int)(inst->padding[0]&3u); SQ_PROBE(planes_hinted);
#ifdef SQ_HINT_CHECK
            nm_bounds_planes_prepare(&planes,&view,inst->matrix,inst->focus); planes_instance=NULL;
            if(nm_bounds_planes_classify(&planes,mesh->bounds)!=bounds) {
                printf("FAIL: bounds hint %d of car instance %u, planes %d\n",bounds,i,nm_bounds_planes_classify(&planes,mesh->bounds));
                return 0;
            }
#endif
        }
        else if((bounds=nm_bounds_quick_classify(&view,mesh->bounds,inst->matrix,inst->focus))!=3) SQ_PROBE(planes_quick);
        else {
            if(!planes_instance || memcmp(planes_instance->matrix,inst->matrix,sizeof(inst->matrix)) ||
               memcmp(planes_instance->focus,inst->focus,sizeof(inst->focus)) || memcmp(&planes_view,&view,sizeof(view))) {
                nm_bounds_planes_prepare(&planes,&view,inst->matrix,inst->focus);
                planes_instance=inst; planes_view=view;
                SQ_PROBE(planes_prepared);
            } else SQ_PROBE(planes_shared);
            bounds=nm_bounds_planes_classify(&planes,mesh->bounds);
        }
        if(!source && sq_tr_meshes && sq_tr_meshes[inst->mesh]) {
            /* A translucent effect: no batch; sq_submit_translucent draws it. */
            if(bounds) {
                if(sq_tr_count==SQ_TR_INSTANCES_MAX) return 0;
                sq_tr_queue[sq_tr_count].instance=(uint16_t)i; sq_tr_queue[sq_tr_count].bounds=(uint8_t)bounds;
                sq_tr_queue[sq_tr_count++].view=view;
            }
            DRAW_PHASE_END(11,bounds_started); continue;
        }
        if(!bounds) {
#ifdef NATIVE_BOUNDS_VERIFY
            shz_xmtrx_load_3x4(&inst->transform);
            for(unsigned f=0;f<mesh->face_count;++f)
                sq_verify_rejected_face(pack,mesh,inst,&view,pack->faces+mesh->face_first+f);
#endif
            SQ_COUNT(draw_instances_rejected,1); DRAW_PHASE_END(11,bounds_started); continue;
        }
        if(sq_sources[source].count==sq_sources[source].batch_capacity ||
           mesh->vertex_count>sq_sources[source].point_capacity-points_used ||
           mesh->face_count>sq_sources[source].face_capacity-faces_used) return 0;
        nm_sq_batch_t *batch=sq_sources[source].batches+sq_sources[source].count++;
        *batch=(nm_sq_batch_t){mesh,view,sq_sources[source].points+points_used,
                                      sq_sources[source].bindings+faces_used};
        batch->instance_index=i; batch->source=source;
        batch->bounds=bounds;
#if SQ_OBJECT_SUPERSAMPLE
        if(source && sq_ss_level && sq_ss_level[inst->mesh]) batch->supersample=(uint8_t)sq_ss_select(pack,inst,inst->mesh);
#endif
        points_used+=mesh->vertex_count; faces_used+=mesh->face_count;
        DRAW_PHASE_END(11,bounds_started);
    }
    return 1;
}

/* Textures and headers are immutable while a scene runs: a frame only
 * selects its instances here. */
static int sq_prepare(void) {
    draw_count=draw_header_count=0;
    return sq_prepare_source(0,draw_pack) && (!world_draw.pack || sq_prepare_source(1,world_draw.pack));
}

/* Flat-shaded strips (palette-shaded cars, DNT6). With Gouraud off the PVR
 * takes the whole colour of a triangle from one vertex: the last one submitted
 * for it, so triangle t of a strip is coloured by strip vertex t+2 (the same
 * convention flycast implements for the Dreamcast: its Vulkan renderer needs
 * the "last vertex" provoking mode, flyinghead/flycast pull request 1681).
 * Not yet confirmed on this project's hardware: set to 0 for "first vertex"
 * (triangle t coloured by strip vertex t) if faces inside strips show their
 * neighbour's shade. Single faces are unaffected: all their vertices agree. */
#ifndef NM_FLAT_PROVOKING_LAST
#define NM_FLAT_PROVOKING_LAST 1
#endif
/* A strip run may start at any face of an offline strip: culling is off, so
 * a run that starts on an odd triangle (and so writes every triangle with the
 * other winding) draws the same pixels. SQ_RUN_EVEN_ONLY=1 restores the rule
 * of 14 October (runs start on even triangles only, each triangle keeps the
 * offline strip's winding): the simulator uses it to compare streams word for
 * word with that renderer. */
#ifndef SQ_RUN_EVEN_ONLY
#define SQ_RUN_EVEN_ONLY 0
#endif
/* Per face of the car pack: index of its first triangle inside its offline
 * strip. Non-NULL only when faces are flat shaded through palettes, which is
 * what lets a strip run across faces of different light: the header (texture,
 * list, palette bank) must match, the vertex colour need not. */
static uint16_t *sq_flat_strip_triangle[2]; /* Per source: cars, scenery. */
static int sq_flat_strips_prepare(unsigned source,const nm_pack_t *pack,const nm_material_pack_t *materials) {
    free(sq_flat_strip_triangle[source]); sq_flat_strip_triangle[source]=NULL;
    if(!materials->palette_index || !pack->strips) return 1;
    const nm_header_t *original=pack->data;
    uint16_t *table=nm_heap_alloc(source ? "world strip triangles" : "car strip triangles",0,(original->faces ? original->faces : 1)*sizeof(*table));
    if(!table) return 0;
    for(unsigned m=0;m<original->meshes;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one step */
        const nm_mesh_t *mesh=pack->meshes+m;
        for(unsigned i=0;i<mesh->padding[3];++i) {
            const nm_strip_t *strip=pack->strips+mesh->padding[2]+i;
            unsigned triangle=0;
            for(unsigned f=0;f<strip->face_count;++f) {
                if(triangle>UINT16_MAX) { free(table); return 0; }
                table[strip->face_first+f]=(uint16_t)triangle;
                triangle+=pack->faces[strip->face_first+f].count-2u;
            }
        }
    }
    sq_flat_strip_triangle[source]=table;
    return 1;
}
/* Vertex ranges whose u,v the scene rewrites every frame. */
static struct { const nm_vertex_t *first; unsigned count; } sq_dynamic_uv[8];
static unsigned sq_dynamic_uv_count;
static void sq_emit_faces_mark_dynamic(unsigned source,const nm_pack_t *pack,const nm_vertex_t *first,unsigned count) {
    const nm_header_t *original=pack->data;
    if(!sq_emit_faces[source] || first<pack->vertices || first>=pack->vertices+original->vertices) return;
    const unsigned low=(unsigned)(first-pack->vertices);
    for(unsigned m=0;m<original->meshes;++m) {
        const nm_mesh_t *mesh=pack->meshes+m;
        if(mesh->vertex_first>=low+count || mesh->vertex_first+mesh->vertex_count<=low) continue;
        for(unsigned f=0;f<mesh->face_count;++f) {
            const nm_face_t *face=pack->faces+mesh->face_first+f;
            for(unsigned v=0;v<face->count;++v)
                if(mesh->vertex_first+face->indices[v]-low<count) {
                    sq_emit_faces[source][mesh->face_first+f].special|=SQ_FACE_DYNAMIC_UV;
#if SQ_STREAMS
                    /* A prebuilt stream holds its u, v as written at load: the mesh goes back to the queue path. */
                    if(source && sq_stream_meshes) sq_stream_meshes[m]=0;
#endif
                }
        }
    }
}
static int sq_emit_faces_prepare(unsigned source,const nm_pack_t *pack) {
    free(sq_emit_faces[source]); sq_emit_faces[source]=NULL;
    free(sq_strip_corners[source]); sq_strip_corners[source]=NULL;
    const nm_header_t *original=pack->data;
    for(unsigned m=0;m<original->meshes;++m)
        if(pack->meshes[m].vertex_count>SQ_CORNER_VERTICES || pack->meshes[m].face_count>SQ_FACE_RUN) {
            /* A corner word or a run length would not hold it: such a pack
             * is drawn per vertex by the general emitter, at about twice the
             * cost per world face. The largest mesh of the richest tree set
             * so far has 2,230 vertices and 912 faces. */
            printf("Native emission faces: source %u has a mesh of %u vertices, %u faces; general emitter\n",
                source,(unsigned)pack->meshes[m].vertex_count,(unsigned)pack->meshes[m].face_count);
            return 1;
        }
    nm_sq_face_t *table=nm_heap_alloc(source ? "world emission faces" : "car emission faces",32,(original->faces ? original->faces : 1)*sizeof(*table));
    if(!table) return 0;
    nm_sq_strip_corner_t *strip_corners=NULL;
    if(sq_flat_strip_triangle[source]) {
        strip_corners=nm_heap_alloc(source ? "world strip corners" : "car strip corners",32,(original->padding[1] ? original->padding[1] : 1)*sizeof(*strip_corners));
        if(!strip_corners) { free(table); return 0; }
        nm_memset(strip_corners,0,(original->padding[1] ? original->padding[1] : 1)*sizeof(*strip_corners));
    }
    /* No offline strip run takes a live specular face (sq_live_update rewrites its colour each frame, and a
     * stream's strip vertex records hold the colours of the load): a run ends before the next live face of its
     * strip, and a live face starts none. The queue path and the stream path then cut the same runs. */
    uint8_t *live=NULL;
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
    if(source && sq_live_face_count && sq_flat_strip_triangle[source]) {
        live=nm_calloc((original->faces+7u)/8u,1);
        if(!live) { free(table); free(strip_corners); return 0; }
        for(unsigned i=0;i<sq_live_face_count;++i) live[sq_live_faces[i].face>>3]|=(uint8_t)(1u<<(sq_live_faces[i].face&7u));
    }
#endif
#define SQ_FACE_LIVE(f) (live && (live[(f)>>3]>>((f)&7u)&1u))
    for(unsigned m=0;m<original->meshes;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one step */
        const nm_mesh_t *mesh=pack->meshes+m;
        for(unsigned f=0;f<mesh->face_count;++f) {
            const nm_face_t *face=pack->faces+mesh->face_first+f;
            nm_sq_face_t *out=table+mesh->face_first+f;
            for(unsigned v=0;v<4;++v) {
                const unsigned index=v<face->count ? face->indices[v] : 0;
                const nm_vertex_t *vertex=pack->vertices+mesh->vertex_first+index;
                const unsigned representative=pack->position_refs ? pack->position_refs[mesh->vertex_first+index] : index;
                out->corner[v]=sq_corner_of(v<face->count ? representative : 0);
                out->uv[v][0]=vertex->u; out->uv[v][1]=vertex->v;
            }
            out->count=face->count; out->layer=face->layer; out->special=0; out->strip=0;
        }
        if(!sq_flat_strip_triangle[source]) continue;
        for(unsigned i=0;i<mesh->padding[3];++i) {
            const nm_strip_t *strip=pack->strips+mesh->padding[2]+i;
            unsigned triangles=0;
            for(unsigned f=0;f<strip->face_count;++f) {
                const unsigned triangle=sq_flat_strip_triangle[source][strip->face_first+f];
                triangles+=pack->faces[strip->face_first+f].count-2u;
                unsigned reach=0;
                while(f+reach<strip->face_count && !SQ_FACE_LIVE(strip->face_first+f+reach)) ++reach;
                if((SQ_RUN_EVEN_ONLY && (triangle&1)) || reach<2) continue;
                table[strip->face_first+f].special=(uint16_t)reach;
                table[strip->face_first+f].strip=strip->vertex_first+triangle;
            }
            /* The strip's vertices: mesh-local indices, like a face's. */
            if(strip->vertex_first+triangles+2u>original->padding[1]) { free(live); free(table); free(strip_corners); return 0; }
            for(unsigned v=0;v<triangles+2u;++v) {
                const unsigned index=pack->strip_indices[strip->vertex_first+v];
                const nm_vertex_t *vertex=pack->vertices+mesh->vertex_first+index;
                nm_sq_strip_corner_t *out=strip_corners+strip->vertex_first+v;
                out->corner=sq_corner_of(pack->position_refs ? pack->position_refs[mesh->vertex_first+index] : index);
                out->uv[0]=vertex->u; out->uv[1]=vertex->v; out->unused=0;
            }
        }
    }
#undef SQ_FACE_LIVE
    free(live);
    sq_emit_faces[source]=table; sq_strip_corners[source]=strip_corners;
    for(unsigned i=0;i<sq_dynamic_uv_count;++i)
        sq_emit_faces_mark_dynamic(source,pack,sq_dynamic_uv[i].first,sq_dynamic_uv[i].count);
    return 1;
}
/* nm_render_uv_dynamic: a vertex range whose u,v the scene rewrites. */
static int sq_uv_dynamic(const nm_vertex_t *vertices,unsigned count) {
    if(!vertices || !count) return 1;
    for(unsigned i=0;i<sq_dynamic_uv_count;++i)
        if(sq_dynamic_uv[i].first==vertices && sq_dynamic_uv[i].count>=count) return 1;
    if(sq_dynamic_uv_count==sizeof(sq_dynamic_uv)/sizeof(sq_dynamic_uv[0])) {
        puts("FAIL: more rewritten UV ranges than the renderer records"); return 0;
    }
    sq_dynamic_uv[sq_dynamic_uv_count].first=vertices; sq_dynamic_uv[sq_dynamic_uv_count++].count=count;
    if(draw_pack) sq_emit_faces_mark_dynamic(0,draw_pack,vertices,count);
    if(world_draw.pack) sq_emit_faces_mark_dynamic(1,world_draw.pack,vertices,count);
    return 1;
}
/* The cached point of vertex `vertex` of the batch's mesh as the general
 * emitter and the clipper read it: position, status and projection, with the
 * vertex's own u,v. Shared positions: the representative's entry, copied to
 * `held` with the u,v of the vertex table when the vertex is not its own
 * representative. Per-vertex mode: the vertex's own entry. */
static inline const nm_cached_vertex_t *sq_corner(const nm_sq_batch_t *batch,unsigned s,const nm_pack_t *pack,
        unsigned vertex,nm_cached_vertex_t *held) {
    if(!sq_emit_faces[s] || !pack->position_refs) return batch->points+vertex;
    const unsigned representative=pack->position_refs[batch->mesh->vertex_first+vertex];
    if(representative==vertex) return batch->points+vertex;
    const nm_vertex_t *own=pack->vertices+batch->mesh->vertex_first+vertex;
    *held=batch->points[representative];
    held->view.u=own->u; held->view.v=own->v;
    return held;
}
static inline int sq_face_inside(const nm_sq_batch_t *batch,unsigned s,const nm_pack_t *pack,const nm_face_t *face) {
    for(unsigned v=0;v<face->count;++v) {
        nm_cached_vertex_t held; /* only q is read: the position's entry */
        const nm_cached_vertex_t *p=sq_corner(batch,s,pack,face->indices[v],&held);
        if(nm_cached_outside(p) || p->q<=NM_SKY_DEPTH) return 0;
    }
    return 1;
}

/* Projected, on screen and nearer than the sky: nm_cached_outside(p)==0 and
 * p->q>NM_SKY_DEPTH in one integer compare. A status word has the sign bit
 * set, so it is negative as an integer; positive floats order like their
 * bits (an infinity or NaN passes both forms, zero and negatives fail both). */
static inline __attribute__((always_inline)) int sq_point_drawable(const nm_cached_vertex_t *p) {
#ifdef __DREAMCAST__
    const int32_t bits=(int32_t)*(const shz_alias_uint32_t *)&p->q;
#else
    int32_t bits;
    nm_memcpy(&bits,&p->q,sizeof(bits));
#endif
    return bits>((union { float value; int32_t bits; }){NM_SKY_DEPTH}).bits;
}

#ifndef NM_ROAD_DEPTH_SCALE
#define NM_ROAD_DEPTH_SCALE 0.5f /* render/models/model_renderer.h */
#endif
/* World binding index -> 1 when every face of its materials is road surface
 * (nm_render_world_attach). NULL: no world class, true depth everywhere. */
static const uint8_t *sq_surface_bindings;
/* The depth class of those bindings now: NM_ROAD_DEPTH_SCALE until the scene
 * sets its camera's (nm_render_road_depth, sq_road_depth_set below the
 * stream records). The emission classes and stream records hold it. */
static float sq_road_depth=NM_ROAD_DEPTH_SCALE;
/* TA bytes of the model stream: every header and every vertex is 32 bytes.
 * The guard charges what is actually written (a strip of n triangles is n+2
 * vertices, not 3n); draw_ta_fits counts in units of three vertices. The
 * count restarts with the frame's header count: no vertex precedes the
 * first header. NM_DRAW_TRIANGLES stays as a second, coarser bound. */
static unsigned sq_ta_vertices;
#define SQ_TA_UNITS(extra) ((sq_ta_vertices+(extra)+2u)/3u)
/* What the emitter needs of a material binding: the PVR header to submit
 * (the canonical one: two bindings submit the same header exactly when they
 * name the same one), the vertex colour, the UV mapping and the depth class.
 * nm_sq_emit_t is the whole value; sq_emit_lookup is its definition for host
 * tools that own a header/UV cache (fixtures without a table: the emitter
 * calls it per face).
 *
 * The table the emitters read (sq_emit_records) holds it in two parts. Among
 * the cars' 56,530 bindings only the colour changes with the light: header,
 * crop and depth class take some 2,200 distinct values. So a binding is 8
 * bytes, its colour and the address of its emission class (nm_sq_class_t, one
 * cache line: header, UV mapping, depth), and the classes are a table of
 * their own, as are the canonical headers they point at (sq_header_pool). The
 * 32 bytes a binding there were before cost 1.8 MB for the cars, and the
 * header/UV cache they were filled from another 3.6 MB during the load: the
 * console builds records, classes and pool directly (sq_emit_records_build
 * with the renderer's value function, model_renderer.c) and never has that
 * cache. The face loop pays one more load a face, the class address, and
 * reads the class line instead of the rest of the binding's line.
 * A header of source 1 that equals one of source 0 is that one: the pools
 * hold each distinct header once. */
typedef struct {
    const pvr_poly_hdr_t *header;
    uint32_t argb;
    float origin[2], scale[2];
    float depth; /* sq_road_depth for a road-surface binding, else 1 */
    uint32_t padding;
} nm_sq_emit_t;
typedef struct {
    const pvr_poly_hdr_t *header;
    float origin[2], scale[2];
    float depth;
    uint32_t padding[2];
} nm_sq_class_t;
typedef struct { uint32_t argb; const nm_sq_class_t *group; } nm_sq_bind_t;
#ifdef __DREAMCAST__
_Static_assert(sizeof(nm_sq_emit_t)==32 && sizeof(nm_sq_class_t)==32 && sizeof(nm_sq_bind_t)==8,
    "one cache line per emission class, four bindings per line");
#endif
static nm_sq_bind_t *sq_emit_records[2];
static nm_sq_class_t *sq_emit_classes[2];
static pvr_poly_hdr_t *sq_header_pool[2];
/* Classes, pooled headers, and the headers of this source found in the other
 * source's pool. */
static unsigned sq_emit_class_count[2],sq_header_pool_count[2],sq_header_borrowed[2];
static inline void sq_emit_lookup(nm_sq_emit_t *out,unsigned s,unsigned index) {
    const nm_material_pack_t *materials=s ? world_draw.materials : draw_materials;
    const nm_draw_material_t *cache=s ? world_draw.cache : draw_material_cache;
    const nm_material_binding_t *binding=materials->bindings+index;
    out->header=sq_shared_headers[s] ? sq_shared_headers[s][index] : &cache[index].header;
    out->argb=nm_material_argb(materials,binding);
    out->origin[0]=cache[index].uv.origin[0]; out->origin[1]=cache[index].uv.origin[1];
    out->scale[0]=cache[index].uv.scale[0]; out->scale[1]=cache[index].uv.scale[1];
    out->depth=s && sq_surface_bindings && sq_surface_bindings[index] ? sq_road_depth : 1.0f;
    out->padding=0;
}
/* The whole value of a binding from the table. */
static inline void sq_emit_expand(nm_sq_emit_t *out,const nm_sq_bind_t *record) {
    const nm_sq_class_t *const group=record->group;
    out->header=group->header; out->argb=record->argb;
    out->origin[0]=group->origin[0]; out->origin[1]=group->origin[1];
    out->scale[0]=group->scale[0]; out->scale[1]=group->scale[1];
    out->depth=group->depth; out->padding=0;
}
/* Distinct 32-byte keys in order of first appearance, at most 65,535: the
 * scratch of sq_emit_records_build. Open addressing over 16-bit indices. */
typedef struct { uint32_t word[8]; } nm_sq_key_t;
typedef struct { nm_sq_key_t *keys; uint16_t *slots; unsigned count, capacity, slot_count; } nm_sq_keyset_t;
static void sq_keyset_release(nm_sq_keyset_t *set) {
    free(set->keys); free(set->slots);
    *set=(nm_sq_keyset_t){0};
}
static unsigned sq_key_hash(const nm_sq_key_t *key) {
    uint32_t hash=UINT32_C(2166136261);
    for(unsigned w=0;w<8;++w) { hash=(hash^key->word[w])*UINT32_C(16777619); hash^=hash>>15; }
    return hash;
}
/* Index of the key, added when it is new; -1: no memory or too many keys. */
static int sq_keyset_index(nm_sq_keyset_t *set,const nm_sq_key_t *key) {
    if(set->slot_count)
        for(unsigned at=sq_key_hash(key)&(set->slot_count-1u);set->slots[at]!=UINT16_MAX;at=(at+1u)&(set->slot_count-1u))
            if(!memcmp(set->keys+set->slots[at],key,sizeof(*key))) return set->slots[at];
    if(set->count==UINT16_MAX) return -1;
    if(set->count==set->capacity) {
        const unsigned capacity=set->capacity ? 2u*set->capacity : 512u;
        nm_sq_key_t *const keys=nm_heap_alloc("emission class scratch (keys)",0,capacity*sizeof(*keys));
        if(!keys) return -1;
        if(set->count) nm_memcpy(keys,set->keys,set->count*sizeof(*keys));
        free(set->keys); set->keys=keys; set->capacity=capacity;
    }
    set->keys[set->count]=*key;
    if(2u*(set->count+1u)>set->slot_count) {
        const unsigned slot_count=set->slot_count ? 2u*set->slot_count : 1024u;
        uint16_t *const slots=nm_heap_alloc("emission class scratch (slots)",0,slot_count*sizeof(*slots));
        if(!slots) return -1;
        nm_memset(slots,0xff,slot_count*sizeof(*slots));
        free(set->slots); set->slots=slots; set->slot_count=slot_count;
        for(unsigned i=0;i<set->count;++i) {
            unsigned at=sq_key_hash(set->keys+i)&(slot_count-1u);
            while(slots[at]!=UINT16_MAX) at=(at+1u)&(slot_count-1u);
            slots[at]=(uint16_t)i;
        }
    }
    unsigned at=sq_key_hash(key)&(set->slot_count-1u);
    while(set->slots[at]!=UINT16_MAX) at=(at+1u)&(set->slot_count-1u);
    set->slots[at]=(uint16_t)set->count;
    return (int)set->count++;
}
static void sq_emit_records_release(unsigned s) {
    /* The other source's classes may name headers of this pool. */
    if(sq_header_pool[s] && sq_header_borrowed[!s]) {
        free(sq_emit_records[!s]); free(sq_emit_classes[!s]); free(sq_header_pool[!s]);
        sq_emit_records[!s]=NULL; sq_emit_classes[!s]=NULL; sq_header_pool[!s]=NULL;
        sq_emit_class_count[!s]=sq_header_pool_count[!s]=sq_header_borrowed[!s]=0;
    }
    free(sq_emit_records[s]); free(sq_emit_classes[s]); free(sq_header_pool[s]);
    sq_emit_records[s]=NULL; sq_emit_classes[s]=NULL; sq_header_pool[s]=NULL;
    sq_emit_class_count[s]=sq_header_pool_count[s]=sq_header_borrowed[s]=0;
}
/* The table of one source from a function that gives the value of each of
 * its bindings (0: failure; out->header may point at `scratch`, 32 bytes the
 * function may fill, or anywhere else: the bytes are what counts). One pass
 * over the bindings; the only memory besides the three tables is the scratch
 * of distinct headers and classes, a few hundred kilobytes at most, freed
 * before the return. The binding table is allocated first, at its final
 * size, and holds class numbers until the class table exists. */
typedef int (*nm_sq_emit_value_t)(unsigned s,unsigned index,nm_sq_emit_t *out,pvr_poly_hdr_t *scratch);
static int sq_emit_records_build(unsigned s,unsigned count,nm_sq_emit_value_t value) {
    _Static_assert(sizeof(pvr_poly_hdr_t)==sizeof(nm_sq_key_t),"a PVR header is one key");
    sq_emit_records_release(s);
    nm_sq_bind_t *const table=nm_heap_alloc(s ? "world emission bindings" : "car emission bindings",32,(count ? count : 1)*sizeof(*table));
    if(!table) return 0;
    nm_sq_keyset_t headers={0},classes={0};
    nm_sq_class_t *groups=NULL;
    pvr_poly_hdr_t *pool=NULL;
    const pvr_poly_hdr_t **resolved=NULL;
    int ok=1;
    for(unsigned i=0;ok && i<count;++i) {
        if(!(i&127u)) nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        nm_sq_emit_t looked_up;
        _Alignas(32) pvr_poly_hdr_t scratch; /* a whole cache line: pvr_poly_compile allocates it with MOVCA.L */
        nm_sq_key_t key;
        if(!value(s,i,&looked_up,&scratch)) { ok=0; break; }
        nm_memcpy(&key,looked_up.header,sizeof(key));
        const int header=sq_keyset_index(&headers,&key);
        if(header<0) { ok=0; break; }
        nm_memset(&key,0,sizeof(key));
        key.word[0]=(uint32_t)header;
        nm_memcpy(key.word+1,looked_up.origin,2*sizeof(float)); nm_memcpy(key.word+3,looked_up.scale,2*sizeof(float));
        nm_memcpy(key.word+5,&looked_up.depth,sizeof(float));
        const int group=sq_keyset_index(&classes,&key);
        if(group<0) { ok=0; break; }
        table[i].argb=looked_up.argb; table[i].group=(const nm_sq_class_t *)(uintptr_t)(unsigned)group;
    }
    unsigned pooled=0,borrowed=0;
    if(ok) {
        /* A header the other source already pooled is that one. */
        resolved=nm_heap_alloc("emission class scratch (headers)",0,(headers.count ? headers.count : 1)*sizeof(*resolved));
        ok=resolved!=NULL;
        for(unsigned h=0;ok && h<headers.count;++h) {
            if(!(h&7u)) nm_loader_yield(0);
            resolved[h]=NULL;
            for(unsigned o=0;o<sq_header_pool_count[!s];++o)
                if(!memcmp(sq_header_pool[!s]+o,headers.keys+h,sizeof(pvr_poly_hdr_t))) { resolved[h]=sq_header_pool[!s]+o; ++borrowed; break; }
            pooled+=!resolved[h];
        }
    }
    if(ok) {
        pool=nm_heap_alloc(s ? "world header pool" : "car header pool",32,(pooled ? pooled : 1)*sizeof(*pool));
        groups=nm_heap_alloc(s ? "world emission classes" : "car emission classes",32,(classes.count ? classes.count : 1)*sizeof(*groups));
        ok=pool && groups;
    }
    if(ok) {
        unsigned at=0;
        for(unsigned h=0;h<headers.count;++h) if(!resolved[h]) {
            nm_memcpy(pool+at,headers.keys+h,sizeof(*pool)); resolved[h]=pool+at++;
        }
        for(unsigned c=0;c<classes.count;++c) {
            const nm_sq_key_t *const key=classes.keys+c;
            nm_sq_class_t *const out=groups+c;
            out->header=resolved[key->word[0]];
            nm_memcpy(out->origin,key->word+1,2*sizeof(float)); nm_memcpy(out->scale,key->word+3,2*sizeof(float));
            nm_memcpy(&out->depth,key->word+5,sizeof(float));
            out->padding[0]=out->padding[1]=0;
        }
        for(unsigned i=0;i<count;++i) table[i].group=groups+(uintptr_t)table[i].group;
    }
    const unsigned class_count=classes.count;
    free(resolved); sq_keyset_release(&headers); sq_keyset_release(&classes);
    if(!ok) { free(table); free(pool); free(groups); return 0; }
    sq_emit_records[s]=table; sq_emit_classes[s]=groups; sq_header_pool[s]=pool;
    sq_emit_class_count[s]=class_count; sq_header_pool_count[s]=pooled; sq_header_borrowed[s]=borrowed;
    return 1;
}
/* Host tools: the table from their header/UV cache (sq_emit_lookup). */
static int sq_emit_lookup_value(unsigned s,unsigned index,nm_sq_emit_t *out,pvr_poly_hdr_t *scratch) {
    (void)scratch;
    sq_emit_lookup(out,s,index);
    return 1;
}
static int sq_emit_records_prepare(unsigned s) {
    sq_emit_records_release(s);
    const nm_material_pack_t *materials=s ? world_draw.materials : draw_materials;
    if(!materials || !(s ? world_draw.cache : draw_material_cache)) return 1;
    return sq_emit_records_build(s,materials->count,sq_emit_lookup_value);
}
/* World objects super-sampled by instance (see sq_ss_select): after the emission tables of both sources and the
 * world's baked bindings. 1 without such a mesh (no table) or when it is done; 0 (message printed) when a pooled
 * header of such a mesh is also submitted by another face, or carries the bit already. */
static int sq_ss_prepare(const nm_pack_t *pack) {
    sq_ss_release();
    const nm_header_t *original=pack->data;
    unsigned meshes=0,faces=0;
    for(unsigned m=0;m<original->meshes;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one step */
        const nm_mesh_t *mesh=pack->meshes+m;
        if(mesh->face_count && nm_material_supersample_depth(pack->materials+pack->faces[mesh->face_first].material)>0) {
            ++meshes; faces+=mesh->face_count;
        }
    }
    if(!meshes) return 1;
    if(!SQ_OBJECT_SUPERSAMPLE || !sq_emit_records[0] || !sq_emit_records[1] || !sq_baked_faces) {
        printf("Native object super-sampling: off (%u meshes with a switch depth drawn plain: %s)\n",meshes,
            !SQ_OBJECT_SUPERSAMPLE ? "NATIVE_SUPERSAMPLE_FAR=0" : "no emission tables");
        return 1;
    }
    sq_ss_level=nm_heap_zeroed("world super-sample state",32,original->meshes);
    if(!sq_ss_level) return 0;
    float least=FLT_MAX,most=0;
    for(unsigned m=0;m<original->meshes;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one step */
        const nm_mesh_t *mesh=pack->meshes+m;
        if(!mesh->face_count) continue;
        const float depth=nm_material_supersample_depth(pack->materials+pack->faces[mesh->face_first].material);
        if(!(depth>0)) continue;
        sq_ss_level[m]=SQ_SS_PLAIN;
        if(depth<least) least=depth;
        if(depth>most) most=depth;
        for(unsigned f=0;f<mesh->face_count;++f) {
            const nm_baked_face_t baked=sq_baked_faces[mesh->face_first+f];
            for(unsigned side=0;side<2;++side) {
                const unsigned binding=side ? baked.back : baked.front;
                if(binding==UINT16_MAX) continue;
                const pvr_poly_hdr_t *const header=sq_emit_records[1][binding].group->header;
                unsigned i=0;
                while(i<sq_ss_header_count && sq_ss_headers[i]!=header) ++i;
                if(i<sq_ss_header_count) continue;
                pvr_poly_hdr_t *pooled=NULL;
                for(unsigned t=0;t<2 && !pooled;++t)
                    if(sq_header_pool[t] && header>=sq_header_pool[t] && header<sq_header_pool[t]+sq_header_pool_count[t])
                        pooled=sq_header_pool[t]+(header-sq_header_pool[t]);
                if(!pooled || sq_ss_header_count==SQ_SS_HEADERS_MAX || (pooled->mode2&SQ_TSP_SUPER_SAMPLE) ||
                   (pooled->cmd&SQ_TA_CMD_MODIFIER_NORMAL)==SQ_TA_CMD_MODIFIER_NORMAL) {
                    printf("FAIL: object super-sampling: mesh %u binding %u: %s\n",m,binding,!pooled ? "header not pooled" :
                        pooled->mode2&SQ_TSP_SUPER_SAMPLE ? "header super-sampled already" :
                        sq_ss_header_count==SQ_SS_HEADERS_MAX ? "more than SQ_SS_HEADERS_MAX headers" : "normal modifier mode");
                    sq_ss_release(); return 0;
                }
                sq_ss_headers[sq_ss_header_count++]=pooled;
            }
        }
    }
    /* Those headers belong to those meshes alone: every other world face and every car binding submits another. */
    for(unsigned m=0;m<original->meshes;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one step */
        const nm_mesh_t *mesh=pack->meshes+m;
        if(sq_ss_level[m]) continue;
        for(unsigned f=0;f<mesh->face_count;++f) {
            const nm_baked_face_t baked=sq_baked_faces[mesh->face_first+f];
            for(unsigned side=0;side<2;++side) {
                const unsigned binding=side ? baked.back : baked.front;
                if(binding==UINT16_MAX) continue;
                for(unsigned i=0;i<sq_ss_header_count;++i) if(sq_emit_records[1][binding].group->header==sq_ss_headers[i]) {
                    printf("FAIL: object super-sampling: face %u of mesh %u submits a header of a super-sampled mesh\n",f,m);
                    sq_ss_release(); return 0;
                }
            }
        }
    }
    for(unsigned b=0;b<draw_materials->count;++b) {
        if(!(b&2047u)) nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        for(unsigned i=0;i<sq_ss_header_count;++i)
            if(sq_emit_records[0][b].group->header==sq_ss_headers[i]) {
                printf("FAIL: object super-sampling: car binding %u submits a header of a super-sampled mesh\n",b);
                sq_ss_release(); return 0;
            }
    }
    printf("Native object super-sampling: %u meshes (%u faces), %u headers of their own; switch depth %g..%g (focus 280), "
           "hysteresis %g; state=%u bytes\n",meshes,faces,sq_ss_header_count,(double)least,(double)most,(double)NM_LOD_HYSTERESIS,
           (unsigned)original->meshes);
    return 1;
}
/* Canonical pointers with a table (or the host tools' shared headers), else
 * the 32 bytes. */
static inline int sq_same_header(unsigned source,const nm_draw_material_t *cache,unsigned a,unsigned b) {
    /* The records name the header each binding submits. */
    if(a!=b && sq_emit_records[source])
        return sq_emit_records[source][a].group->header==sq_emit_records[source][b].group->header;
    return a==b || (sq_shared_headers[source] ? sq_shared_headers[source][a]==sq_shared_headers[source][b] :
        !memcmp(&cache[a].header,&cache[b].header,sizeof(pvr_poly_hdr_t)));
}
/* Host tools record which face and binding a vertex belongs to. */
#ifndef SQ_VERTEX_TAG
#define SQ_VERTEX_TAG(face,binding) ((void)0)
#endif
/* One TA vertex from a cached, projected point: the arithmetic of every
 * unclipped emission site, in one place.
 *   x = p->x * (640/496) * ENJ_XSCALE      y = p->y * (480/384)
 *   z = p->q * depth_scale
 *   u = (p->view.u - origin_u) * scale_u   v = (p->view.v - origin_v) * scale_v
 * sq_write_point_c is the definition. On the console the same operations are
 * written out (sq_write_point_sh4): the SH4 has no displacement addressing
 * for floats and the compiler computes an address for every field it reads
 * or writes (about 55 instructions a vertex). The five inputs are
 * consecutive words (view.u, view.v, x, y, q), read with post-increment; the
 * eight output words are stored last to first with pre-decrement: 20
 * instructions. Each value is one FSUB/FMUL or one FMUL of the same
 * operands as the C form (ENJ_XSCALE is the integer 1 or 2, so the screen
 * factor is one exact constant either way). nm_render_init compares the two
 * on sample points before anything is drawn (sq_write_point_check); host
 * builds (tests, simulator) use the C form. */
#include <stddef.h>
_Static_assert(offsetof(nm_cached_vertex_t,view.u)==12 && offsetof(nm_cached_vertex_t,view.v)==16 &&
    offsetof(nm_cached_vertex_t,x)==20 && offsetof(nm_cached_vertex_t,y)==24 &&
    offsetof(nm_cached_vertex_t,q)==28,"cached point input order");
_Static_assert(offsetof(pvr_vertex_t,flags)==0 && offsetof(pvr_vertex_t,x)==4 && offsetof(pvr_vertex_t,y)==8 &&
    offsetof(pvr_vertex_t,z)==12 && offsetof(pvr_vertex_t,u)==16 && offsetof(pvr_vertex_t,v)==20 &&
    offsetof(pvr_vertex_t,argb)==24 && offsetof(pvr_vertex_t,oargb)==28 && sizeof(pvr_vertex_t)==32,
    "PVR vertex word order");
static inline __attribute__((always_inline)) void sq_write_point_c(pvr_vertex_t *out,const nm_cached_vertex_t *p,
        uint32_t flags,float origin_u,float origin_v,float scale_u,float scale_v,float depth_scale,uint32_t argb) {
    out->flags=flags;
    out->x=p->x*(640.0f/496.0f)*ENJ_XSCALE;
    out->y=p->y*(480.0f/384.0f); out->z=p->q*depth_scale;
    out->u=(p->view.u-origin_u)*scale_u;
    out->v=(p->view.v-origin_v)*scale_v;
    out->argb=argb; out->oargb=0;
}
/* The same vertex for the production emitter: x, y, q from the cached point
 * of the corner's position, u and v as the integers of the vertex table (the
 * cached entry holds the float of the same integer: the conversion is exact
 * and the arithmetic after it is the same). sq_write_corner_c is the
 * definition; the console form converts through FPUL and is compared with it
 * before the first frame (sq_write_point_check). */
static inline __attribute__((always_inline)) void sq_write_corner_c(pvr_vertex_t *out,const nm_cached_vertex_t *p,
        uint32_t flags,unsigned u,unsigned v,float origin_u,float origin_v,float scale_u,float scale_v,float depth_scale,uint32_t argb) {
    out->flags=flags;
    out->x=p->x*(640.0f/496.0f)*ENJ_XSCALE;
    out->y=p->y*(480.0f/384.0f); out->z=p->q*depth_scale;
    out->u=((float)u-origin_u)*scale_u;
    out->v=((float)v-origin_v)*scale_v;
    out->argb=argb; out->oargb=0;
}
#ifdef __DREAMCAST__
static inline __attribute__((always_inline)) void sq_write_corner_sh4(pvr_vertex_t *out,const nm_cached_vertex_t *p,
        uint32_t flags,unsigned u_integer,unsigned v_integer,float origin_u,float origin_v,float scale_u,float scale_v,float depth_scale,uint32_t argb) {
    const shz_alias_float_t *in=(const shz_alias_float_t *)&p->x;
    shz_alias_uint32_t *end=(shz_alias_uint32_t *)(out+1);
    const float screen_x=(640.0f/496.0f)*ENJ_XSCALE,screen_y=480.0f/384.0f;
    const uint32_t zero=0;
    float u,v,x,y,q;
    asm volatile(
        "lds %[ui],fpul\n\t"
        "float fpul,%[u]\n\t"
        "lds %[vi],fpul\n\t"
        "float fpul,%[v]\n\t"
        "fmov.s @%[in]+,%[x]\n\t"
        "fmov.s @%[in]+,%[y]\n\t"
        "fmov.s @%[in],%[q]\n\t"
        "mov.l %[zero],@-%[end]\n\t"       /* oargb */
        "mov.l %[argb],@-%[end]\n\t"
        "fsub %[ov],%[v]\n\t"
        "fmul %[sv],%[v]\n\t"
        "fmov.s %[v],@-%[end]\n\t"
        "fsub %[ou],%[u]\n\t"
        "fmul %[su],%[u]\n\t"
        "fmov.s %[u],@-%[end]\n\t"
        "fmul %[ds],%[q]\n\t"
        "fmov.s %[q],@-%[end]\n\t"         /* z */
        "fmul %[sy],%[y]\n\t"
        "fmov.s %[y],@-%[end]\n\t"
        "fmul %[sx],%[x]\n\t"
        "fmov.s %[x],@-%[end]\n\t"
        "mov.l %[flags],@-%[end]\n"
        : [in] "+r" (in), [end] "+r" (end),
          [u] "=&f" (u), [v] "=&f" (v), [x] "=&f" (x), [y] "=&f" (y), [q] "=&f" (q)
        : [zero] "r" (zero), [argb] "r" (argb), [flags] "r" (flags), [ui] "r" (u_integer), [vi] "r" (v_integer),
          [ou] "f" (origin_u), [ov] "f" (origin_v), [su] "f" (scale_u), [sv] "f" (scale_v),
          [ds] "f" (depth_scale), [sx] "f" (screen_x), [sy] "f" (screen_y)
        : "memory", "fpul");
}
#define sq_write_corner sq_write_corner_sh4
#else
#define sq_write_corner sq_write_corner_c
#endif
/* The vertex of a prebuilt world stream (nm_sq_stream_face_t): x, y, q of
 * the corner's cached point (`xyq` is &point->x) times the screen factors and
 * the face's depth factor, u and v as written at load by sq_write_corner, so
 * every word equals sq_write_corner_c's for the same corner. The console
 * form reads the two texture words as integers and is compared with this one
 * before the first frame (sq_write_point_check): 16 instructions. */
static inline __attribute__((always_inline)) void sq_write_stream_c(pvr_vertex_t *out,const float *xyq,uint32_t flags,
        const float *uv,float depth_scale,uint32_t argb) {
    out->flags=flags;
    out->x=xyq[0]*(640.0f/496.0f)*ENJ_XSCALE;
    out->y=xyq[1]*(480.0f/384.0f); out->z=xyq[2]*depth_scale;
    out->u=uv[0]; out->v=uv[1];
    out->argb=argb; out->oargb=0;
}
#ifdef __DREAMCAST__
static inline __attribute__((always_inline)) void sq_write_stream_sh4(pvr_vertex_t *out,const float *xyq,uint32_t flags,
        const float *uv,float depth_scale,uint32_t argb) {
    const shz_alias_float_t *in=(const shz_alias_float_t *)xyq;
    const shz_alias_uint32_t *texture=(const shz_alias_uint32_t *)uv;
    shz_alias_uint32_t *end=(shz_alias_uint32_t *)(out+1);
    const float screen_x=(640.0f/496.0f)*ENJ_XSCALE,screen_y=480.0f/384.0f;
    const uint32_t zero=0;
    float x,y,q;
    uint32_t u,v;
    asm volatile(
        "fmov.s @%[in]+,%[x]\n\t"
        "fmov.s @%[in]+,%[y]\n\t"
        "fmov.s @%[in],%[q]\n\t"
        "mov.l @%[texture]+,%[u]\n\t"
        "mov.l @%[texture],%[v]\n\t"
        "mov.l %[zero],@-%[end]\n\t"       /* oargb */
        "mov.l %[argb],@-%[end]\n\t"
        "mov.l %[v],@-%[end]\n\t"
        "mov.l %[u],@-%[end]\n\t"
        "fmul %[ds],%[q]\n\t"
        "fmov.s %[q],@-%[end]\n\t"         /* z */
        "fmul %[sy],%[y]\n\t"
        "fmov.s %[y],@-%[end]\n\t"
        "fmul %[sx],%[x]\n\t"
        "fmov.s %[x],@-%[end]\n\t"
        "mov.l %[flags],@-%[end]\n"
        : [in] "+r" (in), [texture] "+r" (texture), [end] "+r" (end),
          [x] "=&f" (x), [y] "=&f" (y), [q] "=&f" (q), [u] "=&r" (u), [v] "=&r" (v)
        : [zero] "r" (zero), [argb] "r" (argb), [flags] "r" (flags),
          [ds] "f" (depth_scale), [sx] "f" (screen_x), [sy] "f" (screen_y)
        : "memory");
}
#define sq_write_stream sq_write_stream_sh4
#else
#define sq_write_stream sq_write_stream_c
#endif
#ifdef __DREAMCAST__
static inline __attribute__((always_inline)) void sq_write_point_sh4(pvr_vertex_t *out,const nm_cached_vertex_t *p,
        uint32_t flags,float origin_u,float origin_v,float scale_u,float scale_v,float depth_scale,uint32_t argb) {
    const shz_alias_float_t *in=(const shz_alias_float_t *)&p->view.u;
    shz_alias_uint32_t *end=(shz_alias_uint32_t *)(out+1);
    const float screen_x=(640.0f/496.0f)*ENJ_XSCALE,screen_y=480.0f/384.0f;
    const uint32_t zero=0;
    float u,v,x,y,q;
    asm volatile(
        "fmov.s @%[in]+,%[u]\n\t"
        "fmov.s @%[in]+,%[v]\n\t"
        "fmov.s @%[in]+,%[x]\n\t"
        "fmov.s @%[in]+,%[y]\n\t"
        "fmov.s @%[in],%[q]\n\t"
        "mov.l %[zero],@-%[end]\n\t"       /* oargb */
        "mov.l %[argb],@-%[end]\n\t"
        "fsub %[ov],%[v]\n\t"
        "fmul %[sv],%[v]\n\t"
        "fmov.s %[v],@-%[end]\n\t"
        "fsub %[ou],%[u]\n\t"
        "fmul %[su],%[u]\n\t"
        "fmov.s %[u],@-%[end]\n\t"
        "fmul %[ds],%[q]\n\t"
        "fmov.s %[q],@-%[end]\n\t"         /* z */
        "fmul %[sy],%[y]\n\t"
        "fmov.s %[y],@-%[end]\n\t"
        "fmul %[sx],%[x]\n\t"
        "fmov.s %[x],@-%[end]\n\t"
        "mov.l %[flags],@-%[end]\n"
        : [in] "+r" (in), [end] "+r" (end),
          [u] "=&f" (u), [v] "=&f" (v), [x] "=&f" (x), [y] "=&f" (y), [q] "=&f" (q)
        : [zero] "r" (zero), [argb] "r" (argb), [flags] "r" (flags),
          [ou] "f" (origin_u), [ov] "f" (origin_v), [su] "f" (scale_u), [sv] "f" (scale_v),
          [ds] "f" (depth_scale), [sx] "f" (screen_x), [sy] "f" (screen_y)
        : "memory");
}
#define sq_write_point sq_write_point_sh4
/* Before the first frame: the instruction form against the C form, word for
 * word, on points that exercise every field. */
static int sq_write_point_check(void) {
    static const float samples[][8]={ /* view x,y,z,u,v then x,y,q */
        {1.5f,-2.25f,10.0f,8.0f,2040.0f,123.456f,77.125f,0.1f},
        {0,0,0.01f,65535.0f,0.0f,0.0f,384.0f,100.0f},
        {-3.0f,4.0f,255.75f,1027.0f,513.0f,495.99f,0.0625f,0.00391f},
        {9.0f,9.0f,1.0f,4096.0f,21845.0f,248.5f,191.75f,1.0f}};
    static const float parameters[][5]={ /* origin u,v, scale u,v, depth */
        {0,0,1.0f/2048,1.0f/512,1.0f},{8,16,1.0f/256,1.0f/8192,0.5f},{0,0,1.0f/1024,1.0f/1024,0.5f*(1.0f+240.0f/1024.0f)}};
    for(unsigned i=0;i<sizeof(samples)/sizeof(samples[0]);++i)
        for(unsigned k=0;k<sizeof(parameters)/sizeof(parameters[0]);++k) {
            alignas(32) nm_cached_vertex_t point;
            alignas(32) pvr_vertex_t expected,written;
            nm_memcpy(&point,samples[i],sizeof(point));
            nm_memset(&written,0xa5,sizeof(written));
            const uint32_t flags=k&1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,argb=0xff102030u+i*0x01010101u+k;
            sq_write_point_c(&expected,&point,flags,parameters[k][0],parameters[k][1],parameters[k][2],
                parameters[k][3],parameters[k][4],argb);
            sq_write_point_sh4(&written,&point,flags,parameters[k][0],parameters[k][1],parameters[k][2],
                parameters[k][3],parameters[k][4],argb);
            if(memcmp(&expected,&written,sizeof(expected))) {
                printf("FAIL: SQ vertex writer differs from its C form (sample %u, parameters %u)\n",i,k);
                return 0;
            }
            /* The production form: u,v as the vertex table's integers. */
            alignas(32) pvr_vertex_t corner_c,corner_sh4;
            const unsigned u_integer=(unsigned)samples[i][3],v_integer=(unsigned)samples[i][4];
            nm_memset(&corner_sh4,0x5a,sizeof(corner_sh4));
            sq_write_corner_c(&corner_c,&point,flags,u_integer,v_integer,parameters[k][0],parameters[k][1],parameters[k][2],
                parameters[k][3],parameters[k][4],argb);
            sq_write_corner_sh4(&corner_sh4,&point,flags,u_integer,v_integer,parameters[k][0],parameters[k][1],parameters[k][2],
                parameters[k][3],parameters[k][4],argb);
            if(memcmp(&corner_c,&corner_sh4,sizeof(corner_c)) || memcmp(&corner_c,&expected,sizeof(corner_c))) {
                printf("FAIL: SQ corner writer differs from its C form or from the point writer (sample %u, parameters %u)\n",i,k);
                return 0;
            }
            /* The prebuilt stream's form: u, v as the corner writer wrote them. */
            alignas(32) pvr_vertex_t stream_c,stream_sh4;
            const float written_uv[2]={corner_sh4.u,corner_sh4.v};
            nm_memset(&stream_sh4,0x3c,sizeof(stream_sh4));
            sq_write_stream_c(&stream_c,&point.x,flags,written_uv,parameters[k][4],argb);
            sq_write_stream_sh4(&stream_sh4,&point.x,flags,written_uv,parameters[k][4],argb);
            if(memcmp(&stream_c,&stream_sh4,sizeof(stream_c)) || memcmp(&stream_c,&corner_c,sizeof(stream_c))) {
                printf("FAIL: SQ stream writer differs from its C form or from the corner writer (sample %u, parameters %u)\n",i,k);
                return 0;
            }
        }
    puts("Native SQ vertex writer PASS: the three instruction forms (cached u,v; integer u,v; prebuilt stream) equal the C form, 12 cases, all eight words");
    return 1;
}
#else
#define sq_write_point sq_write_point_c
static int sq_write_point_check(void) { return 1; }
#endif
static inline __attribute__((always_inline)) void sq_put_point(const nm_cached_vertex_t *p,uint32_t flags,
        float origin_u,float origin_v,float scale_u,float scale_v,float depth_scale,uint32_t argb) {
    pvr_vertex_t *const out=pvr_dr_target();
    sq_write_point(out,p,flags,origin_u,origin_v,scale_u,scale_v,depth_scale,argb);
    pvr_dr_commit(out);
}
/* Strip inspection view: a punch-through polygon is drawn through the
 * stipple texture (model_renderer.c, draw_inspect_stipple_prepare) with
 * screen-space UVs, one texel per SQ_STIPPLE_PIXELS output pixels. */
#ifndef SQ_STIPPLE_PIXELS
#define SQ_STIPPLE_PIXELS 3
#endif
#define SQ_STIPPLE_SCALE (1.0f/(8.0f*SQ_STIPPLE_PIXELS))
/* Colours of the strip inspection view (nm_render_inspect_set_strips). */
static const uint32_t sq_strip_colors[8]={0xffff4040,0xff40ff40,0xff4080ff,0xffffff40,
    0xffff40ff,0xff40ffff,0xffffa040,0xffa080ff};
#if defined(NM_SH4ZAM_GFX) && NM_SH4ZAM_GFX
/* (The emitter fixture, tests/test_sq_quads.py, cuts this file below the
 * definition of NM_SH4ZAM_GFX: it runs the forms before, whose output it
 * holds.) shz_lerpf is t*(b-a)+a, an FMUL and an FADD on the SH4 (the build
 * has fp-contract off): the host computes the same bits. */
#ifdef __DREAMCAST__
#define sq_lerpf shz_lerpf
#else
static inline float sq_lerpf(float a,float b,float t) { return t*(b-a)+a; }
#endif

/* A clipped vertex as the PVR takes it: x, y, q and u, v (not u/w, v/w). */
typedef struct { float x,y,q,u,v; } nm_sq_clip_vertex_t;
/* Signed distance of a point (view x, y, z) from plane `plane` of the view
 * (geometry.c, clip_distance, in float): >= 0 inside. slope: near_z, then
 * centre minus or bound minus centre per side plane (sq_clip). */
static inline __attribute__((always_inline)) float sq_clip_distance(const nm_view_vertex_t *p,unsigned plane,const float slope[5]) {
    switch(plane) {
    case 0: return p->z-slope[0];
    case 1: return p->x+slope[1]*p->z;
    case 2: return slope[2]*p->z-p->x;
    case 3: return slope[3]*p->z-p->y;
    default: return p->y+slope[4]*p->z;
    }
}
/* n words from src to dst as integers: the SH4 has no float load or store
 * with a displacement (a float copied field by field costs two or three
 * instructions), and GCC turns a memcpy of 20 bytes into a call. */
static inline __attribute__((always_inline)) void sq_copy_words(void *dst,const void *src,unsigned n) {
#ifdef __DREAMCAST__
    shz_alias_uint32_t *const d=dst; const shz_alias_uint32_t *const s=src;
    for(unsigned i=0;i<n;++i) d[i]=s[i];
#else
    nm_memcpy(dst,src,n*4u);
#endif
}
/* The bits of a float, for sign tests on stored distances (finite.h's forms). */
static inline __attribute__((always_inline)) uint32_t sq_float_bits(float value) {
    uint32_t bits; nm_memcpy(&bits,&value,sizeof(bits)); return bits;
}
#define SQ_BITS_NEGATIVE(bits) ((uint32_t)((bits)-UINT32_C(0x80000001))<UINT32_C(0x7f800000))
#define SQ_BITS_POSITIVE(bits) ((uint32_t)((bits)-UINT32_C(1))<UINT32_C(0x7f800000))
/* The triangle clipper of the renderer, SH4ZAM form (NATIVE_SH4ZAM_GFX=0:
 * nm_project_cached_triangle, geometry.c's project_classified_triangle, and
 * a division of u/w, v/w by 1/w per written vertex). In the wide view a
 * clipped triangle nearly always has a corner behind the eye, which is
 * beyond every side plane of the guard view as well (the side planes pass
 * through the eye): all five plane bits, a quarter to a third of the
 * clipped triangles of the recorded races; none has the near plane's bit
 * alone. This form, in single precision as on the console:
 *   - clips the near plane first (positive depths for every later cut), then
 *     only the side planes a remaining point is still beyond: the corners'
 *     own plane bits, the side distances of the new points; after the near
 *     cut the polygon is mostly inside all of them, and the general clipper
 *     ran a pass per plane bit of the corners;
 *   - the cut of an edge: t = |d near| / (|d near| + |d far|) from the
 *     endpoint nearer the plane (the general clipper's rule), by
 *     shz_divf_fsrra (FSRRA of the square and a multiply), the coordinates
 *     and u, v by shz_lerpf, the coordinate of the cut plane set exactly onto
 *     it as the general clipper does;
 *   - projects a corner by taking its cached x, y, q (nm_cache_vertex_wide's,
 *     no second FSRRA), a point on the near plane with 1/near_z (computed
 *     once, exactly), any other with shz_invf_fsrra, and carries u, v as they
 *     are written: the emitter no longer divides u/w and v/w by 1/w (two FSRRA
 *     divisions per written vertex, three written vertices per triangle of
 *     the fan, so corner 0 of a polygon was divided once per triangle).
 * The polygon is the general clipper's up to its first vertex (the planes'
 * order differs) and rounding: tests/reference_stream.h compares the clipped
 * polygon with the reference's (double precision, sides first) up to that
 * rotation, x and y within 1/16 output pixel, u and v within 1/64 texel, the
 * depth within NM_REFERENCE_CLIP_DEPTH of it. Returns the vertex count (3 to
 * 8), 0 for nothing to draw or invalid arithmetic. */
static __attribute__((noinline)) unsigned sq_clip(const nm_view_t *view,const nm_cached_vertex_t *const points[3],unsigned any,
        nm_sq_clip_vertex_t out[NM_CLIP_VERTICES]) {
    const float near=view->near_z,cx=view->center[0],cy=view->center[1];
    const float slope[5]={near,cx-view->bounds[0],view->bounds[1]-cx,cy-view->bounds[2],view->bounds[3]-cy};
    /* The polygon: pointers to its points, the corners' own view
     * coordinates and the cuts (a convex polygon crosses a plane at two
     * edges at most: two cuts per plane), so no pass copies a point. */
    nm_view_vertex_t cuts[2*5];
    const nm_view_vertex_t *lists[2][NM_CLIP_VERTICES+1];
    const nm_view_vertex_t **in=lists[0],**made=lists[1];
    uint8_t side[NM_CLIP_VERTICES+1]; /* per point 1 in front of the plane, 2 behind it, 0 on it */
    float distance[NM_CLIP_VERTICES+1];
    for(unsigned k=0;k<3;++k) in[k]=&points[k]->view;
    unsigned count=3,cut=0,planes=any&31u;
    for(unsigned plane=0;plane<5 && count>=3;++plane) {
        if(!(planes&(1u<<plane))) continue;
        for(unsigned i=0;i<count;++i) {
            const float d=sq_clip_distance(in[i],plane,slope);
            const uint32_t bits=sq_float_bits(d);
            if((bits&UINT32_C(0x7f800000))==UINT32_C(0x7f800000)) return 0; /* not finite */
            distance[i]=d; side[i]=(uint8_t)(SQ_BITS_POSITIVE(bits) ? 1u : SQ_BITS_NEGATIVE(bits) ? 2u : 0u);
        }
        distance[count]=distance[0]; side[count]=side[0]; in[count]=in[0];
        unsigned used=0;
        for(unsigned i=0;i<count;++i) {
            /* Edge (i, i+1): its start if in front of or on the plane,
             * then the cut if its ends lie strictly either side. */
            const unsigned sides=side[i]|side[i+1u];
            if(side[i]!=2u) made[used++]=in[i];
            if(sides==3u) {
                /* From the endpoint nearer the plane (geometry.c's rule). */
                const nm_view_vertex_t *from=in[i],*to=in[i+1u];
                float dn=fabsf(distance[i]),df=fabsf(distance[i+1u]);
                if(dn>df) { from=in[i+1u]; to=in[i]; const float swap=dn; dn=df; df=swap; }
                const float sum=dn+df;
                const float t=sum>=0x1p-63f && sum<0x1p63f ? shz_divf_fsrra(dn,sum) : dn/sum;
                if(used==NM_CLIP_VERTICES || cut==2*5) return 0;
                nm_view_vertex_t *const p=cuts+cut++;
                float z=near;
                if(plane) z=sq_lerpf(from->z,to->z,t);
                float x=sq_lerpf(from->x,to->x,t),y=sq_lerpf(from->y,to->y,t);
                /* The coordinate of the cut plane set exactly onto it. */
                if(plane==1 || plane==2) x=(view->bounds[plane-1u]-cx)*z;
                else if(plane) y=(cy-view->bounds[plane-1u])*z;
                p->x=x; p->y=y; p->z=z;
                p->u=sq_lerpf(from->u,to->u,t); p->v=sq_lerpf(from->v,to->v,t);
                made[used++]=p;
            }
        }
        const nm_view_vertex_t **const swap=in; in=made; made=swap;
        count=used;
        if(!plane) {
            /* The side planes a point of the polygon is still beyond: a
             * corner's own plane bits, a cut's distances. */
            planes=0;
            for(unsigned i=0;i<count;++i) {
                const nm_view_vertex_t *const p=in[i];
                if(p<cuts || p>=cuts+2*5) {
                    planes|=nm_cached_outside((const nm_cached_vertex_t *)(const void *)p);
                    continue;
                }
                for(unsigned side=1;side<5;++side)
                    if(nm_float_negative(sq_clip_distance(p,side,slope))) planes|=1u<<side;
            }
            planes&=any&30u;
        }
    }
    if(count<3) return 0;
    const float q_near=1.0f/near;
    for(unsigned i=0;i<count;++i) {
        const nm_view_vertex_t *const p=in[i];
        nm_sq_clip_vertex_t *const o=out+i;
        if((p<cuts || p>=cuts+2*5) && !nm_cached_outside((const nm_cached_vertex_t *)(const void *)p)) {
            /* A corner inside the guard view: its cached projection and its
             * u, v. (A corner outside a plane does not survive that plane's
             * pass; should one, it is projected below like a cut.) */
            const nm_cached_vertex_t *const c=(const nm_cached_vertex_t *)(const void *)p;
            sq_copy_words(&o->x,&c->x,3); sq_copy_words(&o->u,&c->view.u,2);
            continue;
        }
        /* p->z >= near_z > 0: the near pass removed every point behind
         * the plane, or no corner was, and a cut between points in front of
         * it is in front of it. */
        const float z=p->z;
        const float q=z==near ? q_near : z>=0x1p-63f && z<0x1p64f ? shz_invf_fsrra(z) : 1.0f/z;
        float x=cx+p->x*q,y=cy-p->y*q;
        if(!nm_finite_float(x) || !nm_finite_float(y) || !nm_finite_float(q) || !nm_float_positive(q) ||
           !nm_finite_float(p->u) || !nm_finite_float(p->v)) return 0;
        if(x<view->bounds[0]) x=view->bounds[0];
        if(x>view->bounds[1]) x=view->bounds[1];
        if(y<view->bounds[2]) y=view->bounds[2];
        if(y>view->bounds[3]) y=view->bounds[3];
        o->x=x; o->y=y; o->q=q; sq_copy_words(&o->u,&p->u,2);
    }
    return count;
}
/* Before the first frame (nm_render_init): sq_clip against the general
 * clipper (nm_project_cached_triangle, u and v as u/w and v/w divided by
 * 1/w, as the emitter did) on fixed triangles in the guard view of the
 * screen: a corner behind the eye, two, a corner beyond the guard band, one
 * of each, a corner between the eye and the near plane, both windings. The
 * polygons must have the same vertex count and agree, from some first
 * vertex, within the reference check's tolerances (1/16 output pixel in x
 * and y, 1/64 texel in u and v in the pack's units of 1/8 texel, 2^-14 of
 * the depth). */
static __attribute__((noinline,unused)) int sq_clip_check(void) {
    nm_view_t guard={{248,192},{-1024,1520,-1024,1408},0.01f};
    static const float triangles[][3][5]={
        {{-120,40,30,0,0},{150,60,40,1024,0},{20,-30,-5,512,2048}},
        {{-120,40,-30,0,0},{150,60,-4,1024,0},{20,-30,15,512,2048}},
        {{-5000,40,2,0,0},{150,60,40,1024,0},{20,-30,25,512,2048}},
        {{-9000,-20,-8,16,16},{300,-200,60,4000,96},{-40,700,30,96,3000}},
        {{10,5,0.005f,0,0},{150,60,40,1024,0},{20,-30,25,512,2048}},
        {{20,-30,-5,512,2048},{150,60,40,1024,0},{-120,40,30,0,0}},
        {{300,9000,3,8,8},{-200,-100,50,2000,40},{250,-80,12,300,1900}},
        {{-30,-40,6,100,100},{40,-30,-60,900,200},{-20,50,-0.5f,400,1600}}};
    const float tolerance[3]={(1.0f/16)*(496.0f/640.0f),(1.0f/16)*(384.0f/480.0f),0.125f};
    float worst[4]={0,0,0,0};
    unsigned vertices=0;
    for(unsigned c=0;c<sizeof(triangles)/sizeof(triangles[0]);++c) {
        alignas(32) nm_cached_vertex_t cached[3];
        for(unsigned k=0;k<3;++k) {
            const nm_view_vertex_t p={triangles[c][k][0],triangles[c][k][1],triangles[c][k][2],triangles[c][k][3],triangles[c][k][4]};
            nm_cache_vertex_wide(&guard,&p,cached+k,0);
        }
        const nm_cached_vertex_t *const points[3]={cached,cached+1,cached+2};
        const unsigned outside=nm_cached_outside(cached)|nm_cached_outside(cached+1)|nm_cached_outside(cached+2);
        nm_screen_vertex_t general[NM_CLIP_VERTICES];
        nm_sq_clip_vertex_t fast[NM_CLIP_VERTICES];
        const unsigned expected=nm_project_cached_triangle(&guard,points,general);
        const unsigned count=!outside ? 3 : (outside&32u) || (nm_cached_outside(cached)&nm_cached_outside(cached+1)&nm_cached_outside(cached+2)) ? 0 :
            sq_clip(&guard,points,outside,fast);
        int matched=count==expected && count>=3 && outside;
        for(unsigned rotation=0;matched && rotation<count;++rotation) {
            float d[4]={0,0,0,0};
            for(unsigned i=0;i<count;++i) {
                const nm_screen_vertex_t *const g=general+(i+rotation)%count;
                const nm_sq_clip_vertex_t *const f=fast+i;
                const float e[4]={fabsf(g->x-f->x),fabsf(g->y-f->y),
                    fmaxf(fabsf(g->uq/g->q-f->u),fabsf(g->vq/g->q-f->v)),fabsf(g->q-f->q)/g->q};
                for(unsigned a=0;a<4;++a) if(e[a]>d[a]) d[a]=e[a];
            }
            if(d[0]<=tolerance[0] && d[1]<=tolerance[1] && d[2]<=tolerance[2] && d[3]<=0x1p-14f) {
                for(unsigned a=0;a<4;++a) if(d[a]>worst[a]) worst[a]=d[a];
                vertices+=count; matched=2; break;
            }
        }
        if(matched!=2) {
            printf("FAIL: SH4ZAM clipper, triangle %u: %u vertices, the general clipper %u (plane bits %u)\n",c,count,expected,outside);
            for(unsigned i=0;i<count || i<expected;++i)
                printf("  %u: %g %g %g %g %g | %g %g %g %g %g\n",i,i<count ? (double)fast[i].x : 0.0,i<count ? (double)fast[i].y : 0.0,
                    i<count ? (double)fast[i].q : 0.0,i<count ? (double)fast[i].u : 0.0,i<count ? (double)fast[i].v : 0.0,
                    i<expected ? (double)general[i].x : 0.0,i<expected ? (double)general[i].y : 0.0,i<expected ? (double)general[i].q : 0.0,
                    i<expected ? (double)(general[i].uq/general[i].q) : 0.0,i<expected ? (double)(general[i].vq/general[i].q) : 0.0);
            return 0;
        }
    }
    printf("Native SH4ZAM clipper PASS: %u fixed triangles, %u vertices as the general clipper's within 1/16 pixel, 1/64 texel, 2^-14 depth; "
        "largest differences x %g y %g (logical pixels) u,v %g (1/8 texel) depth %g\n",
        (unsigned)(sizeof(triangles)/sizeof(triangles[0])),vertices,(double)worst[0],(double)worst[1],(double)worst[2],(double)worst[3]);
    return 1;
}
#endif
/* One queued face through the general loop: a face with a corner outside the
 * view or behind the near plane (the independent triangle clipper), and every
 * face of the inspection views. Counts in the frame's globals; `previous` is
 * the header last written by the calling emitter. Returns 0 on a full stream
 * or an invalid depth. */
#if NM_SMOKE_DOOR
/* make NATIVE_SMOKE_MODE=pt-screen|tr-screen ("Translucent effects" below): while sq_door_q > 0 every vertex the general emitter
 * writes takes depth sq_door_q (times the depth factors) and u' = u/2 + (x + y)/2 - sq_door_base (x, y: output
 * pixels; sq_door_base an integer). */
static float sq_door_q, sq_door_base;
#endif
static __attribute__((noinline)) int sq_emit_face_general(unsigned list,unsigned s,const nm_sq_batch_t *batch,
        unsigned f,unsigned index,const nm_sq_emit_t *uv,float depth_scale,const pvr_poly_hdr_t *selected,
        const pvr_poly_hdr_t **previous,int inspect) {
    const int untextured=draw_untextured;
    const int stipple=inspect && draw_inspect_stipple && list==PVR_LIST_PT_POLY;
    const nm_pack_t *pack=s ? world_draw.pack : draw_pack;
    const nm_face_t *face=pack->faces+batch->mesh->face_first+f;
    const uint32_t argb=uv->argb;
    (void)index;
    /* Fan (0,1,2),(0,2,3) becomes strip (1,2,0,3).
     * Keep the original diagonal, winding and triangle order.
     * Clipped faces retain the independent triangle clipper. */
    SQ_PROBE(emit_general_faces);
    nm_cached_vertex_t held[4];
    const nm_cached_vertex_t *corner[4]={NULL,NULL,NULL,NULL};
    for(unsigned v=0;v<face->count;++v) corner[v]=sq_corner(batch,s,pack,face->indices[v],held+v);
    int quad=face->count==4;
    for(unsigned v=0;quad && v<4;++v)
        quad=nm_cached_outside(corner[v])==0;
    for(unsigned t=1;t+1<face->count;++t) {
        DRAW_PHASE_START(clip_started);
        const nm_cached_vertex_t *input[3]={corner[0],corner[t],corner[t+1]};
        if(quad && t==1) {
            const nm_cached_vertex_t *first=input[0];
            input[0]=input[1]; input[1]=input[2]; input[2]=first;
        }
        nm_screen_vertex_t projected[NM_CLIP_VERTICES];
        const unsigned outside=nm_cached_outside(input[0])|
            nm_cached_outside(input[1])|nm_cached_outside(input[2]);
#ifdef NATIVE_PROFILE
        const unsigned common=nm_cached_outside(input[0])&
            nm_cached_outside(input[1])&nm_cached_outside(input[2]);
        ++draw_clip_paths[outside&32 || common ? 1 : outside ? 2 : 0];
        draw_clip_paths[3]+=(outside&30)!=0;
        draw_clip_paths[4]+=(outside&1)!=0;
#endif
        /* Fully visible triangles go straight from the shared cache
         * to SQ. Only clipping needs the projected polygon array. */
        if(outside) SQ_CLIP_INPUT(&batch->view,input,outside);
#if defined(NM_SH4ZAM_GFX) && NM_SH4ZAM_GFX
        /* The clipper's polygon as written (sq_clip): u, v, not u/w, v/w. */
        nm_sq_clip_vertex_t cut[NM_CLIP_VERTICES];
        (void)projected;
        const unsigned count=!outside ? 3 : (outside&32u) || (nm_cached_outside(input[0])&nm_cached_outside(input[1])&nm_cached_outside(input[2])) ? 0 :
            sq_clip(&batch->view,input,outside,cut);
#else
        const unsigned count=outside ? nm_project_cached_triangle(&batch->view,input,projected) : 3;
#endif
        DRAW_PHASE_NEXT(3,clip_started,emit_started);
        for(unsigned n=1;n+1<count;++n) {
            /* One colour per emitted strip: both halves of an
             * intact quad share it; clipped triangle strips don't. */
            const uint32_t color=inspect ? SQ_LOD_TINT(s,batch->mesh->face_first+f,sq_strip_colors[
                (f+3*batch->instance_index+5*s+3*(quad ? 1 : t)+n)&7]) :
                untextured ? argb|0xff000000u : argb;
            const int header_changed=*previous!=selected;
            /* Vertices this step writes: an intact quad is one
             * four-vertex strip (three now, one with its second
             * triangle); every other triangle is three. */
            const unsigned step_vertices=quad ? (t==1 ? 4u : 0u) : 3u;
            if(!draw_ta_fits(SQ_TA_UNITS(step_vertices),draw_header_count+header_changed)) {
                printf("FAIL SQ TA budget: triangles=%u vertices=%u headers=%u next-header=%u source=%u\n",
                    draw_count,sq_ta_vertices,draw_header_count,header_changed,s);return 0;
            }
            if(draw_count+(quad && t==1 ? 2 : 1)>NM_DRAW_TRIANGLES) {
                printf("FAIL SQ triangle capacity=%u TA-bytes=%u source=%u\n",
                    draw_count,sq_ta_vertices*32+draw_header_count*32,s);return 0;
            }
            sq_ta_vertices+=step_vertices;
            const unsigned corners[3]={0,n,n+1};
            for(unsigned v=0;v<3;++v)
#if defined(NM_SH4ZAM_GFX) && NM_SH4ZAM_GFX
                if((outside ? cut[corners[v]].q : input[v]->q)<=NM_SKY_DEPTH) return 0;
#else
                if((outside ? projected[corners[v]].q : input[v]->q)<=NM_SKY_DEPTH) return 0;
#endif
            if(header_changed) {
                pvr_poly_hdr_t *header=pvr_dr_target();
                *header=*selected;
                pvr_dr_commit(header);
                *previous=selected; ++draw_header_count;
            }
            /* The second triangle reuses the two vertices already
             * in the TA strip. */
            for(unsigned v=quad && t==2 ? 2 : 0;v<3;++v) {
                float x,y,q,u=0,vcoord=0;
                if(!outside) {
                    const nm_cached_vertex_t *p=input[v];
                    x=p->x; y=p->y; q=p->q;
                    if(!untextured) { u=p->view.u; vcoord=p->view.v; }
#if defined(NM_SH4ZAM_GFX) && NM_SH4ZAM_GFX
                } else {
                    const nm_sq_clip_vertex_t *p=cut+corners[v];
                    x=p->x; y=p->y; q=p->q;
                    if(!untextured) { u=p->u; vcoord=p->v; }
#else
                } else {
                    const nm_screen_vertex_t *p=projected+corners[v];
                    x=p->x; y=p->y; q=p->q;
                    if(!untextured) { u=shz_divf_fsrra(p->uq,q); vcoord=shz_divf_fsrra(p->vq,q); }
#endif
                }
                pvr_vertex_t *out=pvr_dr_target();
                out->flags=v==2 && (!quad || t==2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                out->x=x*(640.0f/496.0f)*ENJ_XSCALE; out->y=y*(480.0f/384.0f); out->z=q*depth_scale;
                out->u=stipple ? out->x*SQ_STIPPLE_SCALE : untextured ? 0 : (u-uv->origin[0])*uv->scale[0];
                out->v=stipple ? out->y*SQ_STIPPLE_SCALE : untextured ? 0 : (vcoord-uv->origin[1])*uv->scale[1];
#if NM_SMOKE_DOOR
                if(sq_door_q>0.0f) { out->z=sq_door_q*depth_scale; out->u=out->u*0.5f+0.5f*(out->x+out->y)-sq_door_base; }
#endif
                SQ_VERTEX_TAG(batch->mesh->face_first+f,index); SQ_CLIP_TAG(outside!=0);
            out->argb=color; out->oargb=0;
                pvr_dr_commit(out);
            }
            ++draw_count;
            SQ_COUNT(draw_opaque,list==PVR_LIST_OP_POLY); SQ_COUNT(draw_cutout,list==PVR_LIST_PT_POLY);
        }
        DRAW_PHASE_END(5,emit_started);
    }
    return 1;
}
#if SQ_SWEEPS
/* ---- Catch-fence sweep: the band of a panel (see sq_sweep_select) ---------
 * One cut of the polygon in[0..n) (view coordinates; src: its cached point,
 * NULL for a point made by an earlier cut) at the plane z = depth, keeping
 * the part beyond it (`beyond`) or in front of it: the near-plane clipper's
 * rule (sq_clip): a vertex on the plane stays, an edge whose ends lie
 * strictly either side is cut from the end nearer the plane, t by FSRRA
 * division, x, y, u, v by shz_lerpf, z set onto the plane. Returns the
 * vertex count of out (at most n+1), 0 for nothing or a value that is not
 * finite. */
#ifndef SQ_SWEEP_INPUT
#define SQ_SWEEP_INPUT() ((void)0) /* host tools: a band polygon begins (its own clipped-polygon number) */
#endif
static unsigned sq_sweep_cut(const nm_view_vertex_t *in,const nm_cached_vertex_t *const *src,unsigned n,float depth,int beyond,
        nm_view_vertex_t *out,const nm_cached_vertex_t **src_out) {
    float distance[NM_CLIP_VERTICES+1];
    uint8_t side[NM_CLIP_VERTICES+1];
    for(unsigned i=0;i<n;++i) {
        const float d=beyond ? in[i].z-depth : depth-in[i].z;
        const uint32_t bits=sq_float_bits(d);
        if((bits&UINT32_C(0x7f800000))==UINT32_C(0x7f800000)) return 0;
        distance[i]=d; side[i]=(uint8_t)(SQ_BITS_POSITIVE(bits) ? 1u : SQ_BITS_NEGATIVE(bits) ? 2u : 0u);
    }
    distance[n]=distance[0]; side[n]=side[0];
    unsigned used=0;
    for(unsigned i=0;i<n;++i) {
        const unsigned j=i+1u<n ? i+1u : 0u;
        if(side[i]!=2u) { if(used==NM_CLIP_VERTICES) return 0; out[used]=in[i]; src_out[used++]=src[i]; }
        if((side[i]|side[i+1u])==3u) {
            const nm_view_vertex_t *from=in+i,*to=in+j;
            float dn=fabsf(distance[i]),df=fabsf(distance[i+1u]);
            if(dn>df) { from=in+j; to=in+i; const float swap=dn; dn=df; df=swap; }
            const float sum=dn+df;
            const float t=sum>=0x1p-63f && sum<0x1p63f ? shz_divf_fsrra(dn,sum) : dn/sum;
            if(used==NM_CLIP_VERTICES) return 0;
            nm_view_vertex_t *const p=out+used;
            p->x=sq_lerpf(from->x,to->x,t); p->y=sq_lerpf(from->y,to->y,t); p->z=depth;
            p->u=sq_lerpf(from->u,to->u,t); p->v=sq_lerpf(from->v,to->v,t);
            src_out[used++]=NULL;
        }
    }
    return used;
}
/* The band [near, far) of view-space depth (a negative bound: none) of face f
 * of the batch, drawn as one clipped polygon (several when a point of it lies
 * outside the view: each fan triangle then on its own, through sq_clip as
 * sq_emit_face_general clips). Writes and counts as sq_emit_face_general;
 * `previous` the header last written. 0 on a full stream or an invalid
 * depth. */
static __attribute__((noinline)) int sq_sweep_face(unsigned list,unsigned s,const nm_sq_batch_t *batch,unsigned f,unsigned index,
        const nm_sq_emit_t *uv,float depth_scale,const pvr_poly_hdr_t *selected,const pvr_poly_hdr_t **previous,int inspect,
        float near,float far) {
    const int untextured=draw_untextured;
    const int stipple=inspect && draw_inspect_stipple && list==PVR_LIST_PT_POLY;
    const nm_pack_t *pack=s ? world_draw.pack : draw_pack;
    const nm_face_t *face=pack->faces+batch->mesh->face_first+f;
    nm_cached_vertex_t held[4],made[NM_CLIP_VERTICES];
    nm_view_vertex_t polygon[2][NM_CLIP_VERTICES];
    const nm_cached_vertex_t *source[2][NM_CLIP_VERTICES];
    unsigned n=face->count;
    for(unsigned v=0;v<n;++v) {
        source[0][v]=sq_corner(batch,s,pack,face->indices[v],held+v);
        if(nm_cached_outside(source[0][v])&32u) return 1; /* invalid arithmetic: nothing, as the clipper */
        polygon[0][v]=source[0][v]->view;
    }
    unsigned at=0;
    if(near>=0) { n=sq_sweep_cut(polygon[at],source[at],n,near,1,polygon[at^1u],source[at^1u]); at^=1u; }
    if(far>=0 && n>=3) { n=sq_sweep_cut(polygon[at],source[at],n,far,0,polygon[at^1u],source[at^1u]); at^=1u; }
    if(n<3) return 1;
    const nm_view_vertex_t *const p=polygon[at];
    const nm_cached_vertex_t **const point=source[at];
    unsigned any=0;
    for(unsigned i=0;i<n;++i) {
        if(!point[i]) { nm_cache_vertex_classified(&batch->view,p+i,made+i,0); point[i]=made+i; }
        any|=nm_cached_outside(point[i]);
    }
    if(!any) SQ_SWEEP_INPUT();
    for(unsigned t=1;t+1<n;++t) {
        DRAW_PHASE_START(clip_started);
        const nm_cached_vertex_t *input[3]={point[0],point[t],point[t+1]};
        const float corner_uv[3][2]={{p[0].u,p[0].v},{p[t].u,p[t].v},{p[t+1].u,p[t+1].v}};
        const unsigned outside=nm_cached_outside(input[0])|nm_cached_outside(input[1])|nm_cached_outside(input[2]);
        nm_sq_clip_vertex_t cut[NM_CLIP_VERTICES];
        unsigned count=3;
        if(outside) {
            if((outside&32u) || (nm_cached_outside(input[0])&nm_cached_outside(input[1])&nm_cached_outside(input[2]))) count=0;
            else { SQ_CLIP_INPUT(&batch->view,input,outside); count=sq_clip(&batch->view,input,outside,cut); }
        } else if(any) SQ_SWEEP_INPUT();
        DRAW_PHASE_NEXT(3,clip_started,emit_started);
        for(unsigned k=1;k+1<count;++k) {
            const uint32_t color=inspect ? SQ_LOD_TINT(s,batch->mesh->face_first+f,sq_strip_colors[(f+3*batch->instance_index+5*s+3*t+k)&7]) :
                untextured ? uv->argb|0xff000000u : uv->argb;
            const int header_changed=*previous!=selected;
            if(!draw_ta_fits(SQ_TA_UNITS(3),draw_header_count+header_changed)) {
                printf("FAIL SQ TA budget: triangles=%u vertices=%u headers=%u next-header=%u source=%u\n",
                    draw_count,sq_ta_vertices,draw_header_count,header_changed,s);return 0;
            }
            if(draw_count+1>NM_DRAW_TRIANGLES) {
                printf("FAIL SQ triangle capacity=%u TA-bytes=%u source=%u\n",draw_count,sq_ta_vertices*32+draw_header_count*32,s);return 0;
            }
            const unsigned corners[3]={0,k,k+1};
            for(unsigned v=0;v<3;++v) if((outside ? cut[corners[v]].q : input[v]->q)<=NM_SKY_DEPTH) return 0;
            sq_ta_vertices+=3;
            if(header_changed) {
                pvr_poly_hdr_t *header=pvr_dr_target();
                *header=*selected;
                pvr_dr_commit(header);
                *previous=selected; ++draw_header_count;
            }
            for(unsigned v=0;v<3;++v) {
                float x,y,q,u,vcoord;
                if(!outside) { x=input[v]->x; y=input[v]->y; q=input[v]->q; u=corner_uv[v][0]; vcoord=corner_uv[v][1]; }
                else { const nm_sq_clip_vertex_t *c=cut+corners[v]; x=c->x; y=c->y; q=c->q; u=c->u; vcoord=c->v; }
                pvr_vertex_t *out=pvr_dr_target();
                out->flags=v==2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                out->x=x*(640.0f/496.0f)*ENJ_XSCALE; out->y=y*(480.0f/384.0f); out->z=q*depth_scale;
                out->u=stipple ? out->x*SQ_STIPPLE_SCALE : untextured ? 0 : (u-uv->origin[0])*uv->scale[0];
                out->v=stipple ? out->y*SQ_STIPPLE_SCALE : untextured ? 0 : (vcoord-uv->origin[1])*uv->scale[1];
                SQ_VERTEX_TAG(batch->mesh->face_first+f,index); SQ_CLIP_TAG(1);
                out->argb=color; out->oargb=0;
                pvr_dr_commit(out);
            }
            ++draw_count;
            SQ_COUNT(draw_opaque,list==PVR_LIST_OP_POLY); SQ_COUNT(draw_cutout,list==PVR_LIST_PT_POLY);
        }
        DRAW_PHASE_END(5,emit_started);
    }
    return 1;
}
/* Sweep plane k in view-space depth for an instance of vertical focus
 * `focus`: plane * focus / 280, two multiplies (no reciprocal: a cut along an
 * edge nearly parallel to the plane moves along it by the plane's error over
 * the edge's depth slope, and an FSRRA division's 2^-20 there exceeds the
 * reference's 1/64 texel on a far, minified panel). */
static inline float sq_sweep_depth(unsigned k,float focus) {
    return sq_sweep_plane[k]*(focus*(1.0f/NM_LOD_REFERENCE_FOCUS));
}
/* The view-space band of a queued face of a group across a sweep plane this
 * frame (sq_lod_state holds its code: the group's mesh has one instance):
 * 1 with *near, *far (negative: no such bound), else 0 (an ordinary face). */
static int sq_sweep_band(const nm_sq_batch_t *batch,unsigned f,float *near,float *far) {
    const nm_mesh_t *const mesh=batch->mesh;
    const unsigned absolute=mesh->face_first+f;
    if(!sq_lod_groups || !sq_lod_face_level || !sq_lod_face_level[absolute] || !(mesh->padding[4]>>16)) return 0;
    const nm_sq_lod_t *low=sq_lod_groups+(mesh->padding[4]&0xffffu),*high=low+(mesh->padding[4]>>16);
    while(high-low>1) { const nm_sq_lod_t *middle=low+(high-low)/2; if(middle->first<=f) low=middle; else high=middle; }
    if(f<low->first || f>=low->end || !nm_float_negative(low->depth[0])) return 0;
    const unsigned code=sq_lod_state[low-sq_lod_groups];
    if(code<SQ_SWEEP) return 0;
    const unsigned level=sq_lod_face_level[absolute]-1u;
    const float focus=world_draw.pack->instances[batch->instance_index].focus[1];
    *near=level>SQ_SWEEP_FIRST(code) ? sq_sweep_depth(level-1u,focus) : -1.0f;
    *far=level<SQ_SWEEP_LAST(code) ? sq_sweep_depth(level,focus) : -1.0f;
    return 1;
}
/* Before the first frame (nm_render_init): sq_sweep_cut against the cut in
 * double precision on fixed quads (both sides of a plane, a corner on it,
 * two planes): the same vertex count, and within the reference check's
 * tolerances in view coordinates (z exact on the plane; x, y and u, v within
 * 2^-14 relative). */
static __attribute__((noinline,unused)) int sq_sweep_check(void) {
    static const float quads[][4][5]={
        {{-40,10,30,0,0},{-40,-20,30,0,800},{60,-20,70,1600,800},{60,10,70,1600,0}},
        {{80,30,90,0,0},{80,-5,95,0,400},{-30,-5,40,3000,400},{-30,30,35,3000,0}},
        {{5,8,50,16,16},{5,-8,50,16,1040},{25,-8,62,2064,1040},{25,8,58,2064,16}},
        {{-200,40,20,0,0},{-200,-15,20,0,600},{-120,-15,120,9000,600},{-120,40,120,9000,0}}};
    static const float planes[][2]={{50,-1},{-1,50},{50,-1},{-1,58},{35,60},{60,100}};
    unsigned checked=0;
    for(unsigned c=0;c<sizeof(quads)/sizeof(quads[0]);++c) for(unsigned b=0;b<sizeof(planes)/sizeof(planes[0]);++b) {
        nm_view_vertex_t poly[2][NM_CLIP_VERTICES]; const nm_cached_vertex_t *src[2][NM_CLIP_VERTICES];
        double exact[2][NM_CLIP_VERTICES][5];
        unsigned n=4,m=4,at=0;
        for(unsigned v=0;v<4;++v) {
            poly[0][v]=(nm_view_vertex_t){quads[c][v][0],quads[c][v][1],quads[c][v][2],quads[c][v][3],quads[c][v][4]}; src[0][v]=NULL;
            for(unsigned a=0;a<5;++a) exact[0][v][a]=quads[c][v][a];
        }
        for(unsigned cut=0;cut<2;++cut) {
            const float depth=planes[b][cut];
            if(depth<0 || n<3) continue;
            n=sq_sweep_cut(poly[at],src[at],n,depth,!cut,poly[at^1u],src[at^1u]);
            unsigned k=0;
            for(unsigned i=0;i<m;++i) {
                const unsigned j=(i+1u)%m;
                const double di=cut ? depth-exact[at][i][2] : exact[at][i][2]-depth,dj=cut ? depth-exact[at][j][2] : exact[at][j][2]-depth;
                if(di>=0) { nm_memcpy(exact[at^1u][k],exact[at][i],sizeof(exact[0][0])); ++k; }
                if((di>0 && dj<0) || (di<0 && dj>0)) {
                    const double t=di/(di-dj);
                    for(unsigned a=0;a<5;++a) exact[at^1u][k][a]=exact[at][i][a]+t*(exact[at][j][a]-exact[at][i][a]);
                    exact[at^1u][k][2]=depth; ++k;
                }
            }
            m=k; at^=1u;
        }
        if(n!=m) { printf("FAIL: fence sweep cut, quad %u planes %u: %u vertices, exact %u\n",c,b,n,m); return 0; }
        for(unsigned i=0;i<n;++i) {
            const float got[5]={poly[at][i].x,poly[at][i].y,poly[at][i].z,poly[at][i].u,poly[at][i].v};
            for(unsigned a=0;a<5;++a) {
                const double d=fabs((double)got[a]-exact[at][i][a]),scale=fabs(exact[at][i][a])>1 ? fabs(exact[at][i][a]) : 1.0;
                if(a==2 ? d!=0 && (src[at][i] || d>scale*0x1p-20) : d>scale*0x1p-14) {
                    printf("FAIL: fence sweep cut, quad %u planes %u vertex %u field %u: %.9g, exact %.9g\n",c,b,i,a,(double)got[a],exact[at][i][a]);
                    return 0;
                }
            }
        }
        checked+=n;
    }
    printf("Native fence sweep cut PASS: %u fixed quads x %u plane pairs, %u vertices as the exact cut's within 2^-14\n",
        (unsigned)(sizeof(quads)/sizeof(quads[0])),(unsigned)(sizeof(planes)/sizeof(planes[0])),checked);
    return 1;
}
#endif
/* The general emitter, one batch on one list from queue position `first`:
 * the inspection views (strip colours, untextured benchmark), host fixtures
 * that build no emission tables, and a batch the production emitter below
 * (sq_emit_batches) cannot prove to fit. Every face is guarded against the TA
 * budget and the triangle cap. It is also the definition the production
 * emitter is compared with by the submission simulator
 * (SIM_GENERAL_EMITTER=1 runs every frame through this one: the two streams
 * are equal word for word). Not on the frame's hot path. */
static __attribute__((noinline)) int sq_emit_batch_general(unsigned list,unsigned s,const nm_sq_batch_t *batch,
        unsigned first,const pvr_poly_hdr_t **previous_header) {
    const pvr_poly_hdr_t *previous=*previous_header;
    const int inspect=draw_inspect_strips;
    const int untextured=draw_untextured;
    const int stipple=inspect && draw_inspect_stipple && list==PVR_LIST_PT_POLY;
    {
        DRAW_PROFILE_SOURCE(s);
        const nm_pack_t *pack=s ? world_draw.pack : draw_pack;
        const nm_draw_material_t *cache=s ? world_draw.cache : draw_material_cache;
        const nm_sq_bind_t *const records=sq_emit_records[s];
        {
            const int cutout=list==PVR_LIST_PT_POLY;
            const unsigned face_count=cutout ? batch->cutout_count : batch->opaque_count;
            unsigned strip_at=batch->mesh->padding[2];
            const unsigned strip_end=strip_at+batch->mesh->padding[3];
            for(unsigned i=first;i<face_count;++i) {
                const uint32_t entry=batch->bindings[cutout ? batch->mesh->face_count-1-i : i];
                const unsigned f=entry>>16, index=entry&65535u;
                const nm_face_t *face=pack->faces+batch->mesh->face_first+f;
                nm_sq_emit_t looked_up;
                if(records) sq_emit_expand(&looked_up,records+index); else sq_emit_lookup(&looked_up,s,index);
                const nm_sq_emit_t *const uv=&looked_up;
                /* Road surface only: see NM_ROAD_DEPTH_SCALE. Cars, rails and
                 * walls keep true depth. A face on layer n of its plane (DNM6)
                 * is written 1+n*NM_LAYER_DEPTH_STEP nearer, in either class:
                 * a decal wins over what it lies on in any submission order.
                 * Strips are one material and one layer, hence one scale. */
                float depth_scale=uv->depth;
                if(face->layer) depth_scale*=draw_layer_scale[face->layer];
                const float origin_u=uv->origin[0],origin_v=uv->origin[1];
                const float scale_u=uv->scale[0],scale_v=uv->scale[1];
                const pvr_poly_hdr_t *selected=uv->header;
                if(inspect || untextured) selected=draw_inspect_headers+cutout;
                const uint32_t argb=uv->argb;
#if SQ_SWEEPS
                if(batch->sweep) {
                    /* A face of a catch-fence panel across a sweep plane: its band only. */
                    float near,far;
                    if(sq_sweep_band(batch,f,&near,&far)) {
                        if(!sq_sweep_face(list,s,batch,f,index,uv,depth_scale,selected,&previous,inspect,near,far)) return 0;
                        continue;
                    }
                }
#endif
                if(sq_flat_strip_triangle[s]) {
                    /* Longest run of queued, unclipped, consecutive faces of
                     * one offline strip that share this header: one TA strip,
                     * each triangle coloured through its provoking vertex.
                     * Culling is off, so a run may start at any face
                     * (SQ_RUN_EVEN_ONLY: on an even triangle only). */
                    const unsigned absolute=batch->mesh->face_first+f;
                    while(strip_at<strip_end && pack->strips[strip_at].face_first+pack->strips[strip_at].face_count<=absolute) ++strip_at;
                    const nm_strip_t *flat=strip_at<strip_end && pack->strips[strip_at].face_first<=absolute ? pack->strips+strip_at : NULL;
                    unsigned run=0,triangles=0;
                    if(flat && !(SQ_RUN_EVEN_ONLY && (sq_flat_strip_triangle[s][absolute]&1)) && sq_face_inside(batch,s,pack,face)) {
                        const unsigned limit=flat->face_first+flat->face_count-absolute;
                        for(run=1,triangles=face->count-2u;run<limit && i+run<face_count;++run) {
                            const uint32_t next=batch->bindings[cutout ? batch->mesh->face_count-1-i-run : i+run];
                            if((next>>16)!=f+run || !sq_same_header(s,cache,next&65535u,index) ||
                               !sq_face_inside(batch,s,pack,face+run)) break;
                            triangles+=face[run].count-2u;
                        }
                    }
                    if(run>1) {
                        const int header_changed=previous!=selected;
                        if(draw_count+triangles>NM_DRAW_TRIANGLES ||
                           !draw_ta_fits(SQ_TA_UNITS(triangles+2u),draw_header_count+header_changed)) return 0;
                        DRAW_PHASE_START(strip_started);
                        if(header_changed) {
                            pvr_poly_hdr_t *header=pvr_dr_target(); *header=*selected; pvr_dr_commit(header);
                            previous=selected; ++draw_header_count;
                        }
                        sq_ta_vertices+=triangles+2u;
                        const uint16_t *indices=pack->strip_indices+flat->vertex_first+sq_flat_strip_triangle[s][absolute];
                        unsigned at=0;
                        for(unsigned j=0;j<run;++j) {
                            const unsigned member=batch->bindings[cutout ? batch->mesh->face_count-1-i-j : i+j]&65535u;
                            nm_sq_emit_t member_looked_up;
                            if(!records) sq_emit_lookup(&member_looked_up,s,member);
                            const uint32_t member_colour=records ? records[member].argb : member_looked_up.argb;
                            const uint32_t member_argb=inspect ? SQ_LOD_TINT(s,absolute+j,sq_strip_colors[(f+j+3*batch->instance_index+4)&7]) :
                                untextured ? member_colour|0xff000000u : member_colour;
                            /* Vertices whose colour this face supplies. */
                            unsigned own=face[j].count-2u;
                            if(j==(NM_FLAT_PROVOKING_LAST ? 0u : run-1)) own+=2;
                            for(unsigned v=0;v<own;++v,++at) {
                                nm_cached_vertex_t held;
                                const nm_cached_vertex_t *p=sq_corner(batch,s,pack,indices[at],&held);
                                SQ_VERTEX_TAG(batch->mesh->face_first+f+j,member);
                                if(!untextured && !stipple) {
                                    sq_put_point(p,at==triangles+1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                                        origin_u,origin_v,scale_u,scale_v,depth_scale,member_argb);
                                    continue;
                                }
                                pvr_vertex_t *out=pvr_dr_target();
                                out->flags=at==triangles+1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                                out->x=p->x*(640.0f/496.0f)*ENJ_XSCALE;
                                out->y=p->y*(480.0f/384.0f); out->z=p->q*depth_scale;
                                out->u=stipple ? out->x*SQ_STIPPLE_SCALE : untextured ? 0 : (p->view.u-uv->origin[0])*uv->scale[0];
                                out->v=stipple ? out->y*SQ_STIPPLE_SCALE : untextured ? 0 : (p->view.v-uv->origin[1])*uv->scale[1];
                                out->argb=member_argb; out->oargb=0;
                                pvr_dr_commit(out);
                            }
                        }
                        DRAW_PHASE_END(5,strip_started);
#ifdef NATIVE_PROFILE
                        draw_clip_paths[0]+=triangles;
#endif
                        draw_count+=triangles;
                        SQ_COUNT(draw_opaque,list==PVR_LIST_OP_POLY ? triangles : 0);
                        SQ_COUNT(draw_cutout,list==PVR_LIST_PT_POLY ? triangles : 0);
                        SQ_COUNT(sq_flat_runs[s],1); SQ_COUNT(sq_flat_run_faces[s],run); SQ_COUNT(sq_flat_run_vertices[s],triangles+2u);
                        SQ_PROBE(emit_runs);
                        SQ_COUNT(sq_joined_strips,1); SQ_COUNT(sq_strip_vertices_saved,2*(run-1));
                        i+=run-1;
                        continue;
                    }
                }
                /* The common face: every corner projected and nearer than the
                 * sky, drawn as it is. One strip of its three or four cached
                 * points, exactly the vertices, order and flags of the general
                 * loop below (fan (0,1,2),(0,2,3) as strip (1,2,0,3)), without
                 * its per-triangle clip classification and per-vertex
                 * branches. A clipped face and the inspection modes take
                 * the general loop. */
                if(!inspect && !untextured) {
                    const unsigned corners=face->count;
                    const nm_cached_vertex_t *corner[4];
                    nm_cached_vertex_t held[4];
                    int intact=1;
                    for(unsigned v=0;v<corners;++v) {
                        corner[v]=sq_corner(batch,s,pack,face->indices[v],held+v);
                        intact&=sq_point_drawable(corner[v]);
                    }
                    if(intact) {
                        SQ_PROBE(emit_intact_faces);
                        const int header_changed=previous!=selected;
                        if(!draw_ta_fits(SQ_TA_UNITS(corners),draw_header_count+header_changed)) {
                            printf("FAIL SQ TA budget: triangles=%u vertices=%u headers=%u next-header=%u source=%u\n",
                                draw_count,sq_ta_vertices,draw_header_count,header_changed,s);return 0;
                        }
                        if(draw_count+corners-2u>NM_DRAW_TRIANGLES) {
                            printf("FAIL SQ triangle capacity=%u TA-bytes=%u source=%u\n",
                                draw_count,sq_ta_vertices*32+draw_header_count*32,s);return 0;
                        }
                        DRAW_PHASE_START(intact_started);
                        if(header_changed) {
                            pvr_poly_hdr_t *header=pvr_dr_target(); *header=*selected; pvr_dr_commit(header);
                            previous=selected; ++draw_header_count;
                        }
                        sq_ta_vertices+=corners;
                        {
                            /* Corners of the next queued face (SQ_POINT_PREFETCH);
                             * the last face of a list requests its own again.
                             * A triangle's unused fourth index is zero. */
                            const unsigned ahead=i+1<face_count ? i+1 : i;
                            const nm_face_t *const next=pack->faces+batch->mesh->face_first+
                                (batch->bindings[cutout ? batch->mesh->face_count-1-ahead : ahead]>>16);
                            SQ_VERTEX_TAG(batch->mesh->face_first+f,index);
                            if(corners==4) {
                                SQ_POINT_PREFETCH(batch->points+next->indices[0]);
                                sq_put_point(corner[1],PVR_CMD_VERTEX,origin_u,origin_v,scale_u,scale_v,depth_scale,argb);
                                SQ_POINT_PREFETCH(batch->points+next->indices[1]);
                                sq_put_point(corner[2],PVR_CMD_VERTEX,origin_u,origin_v,scale_u,scale_v,depth_scale,argb);
                                SQ_POINT_PREFETCH(batch->points+next->indices[2]);
                                sq_put_point(corner[0],PVR_CMD_VERTEX,origin_u,origin_v,scale_u,scale_v,depth_scale,argb);
                                SQ_POINT_PREFETCH(batch->points+next->indices[3]);
                                sq_put_point(corner[3],PVR_CMD_VERTEX_EOL,origin_u,origin_v,scale_u,scale_v,depth_scale,argb);
                            } else {
                                SQ_POINT_PREFETCH(batch->points+next->indices[0]);
                                sq_put_point(corner[0],PVR_CMD_VERTEX,origin_u,origin_v,scale_u,scale_v,depth_scale,argb);
                                SQ_POINT_PREFETCH(batch->points+next->indices[1]);
                                sq_put_point(corner[1],PVR_CMD_VERTEX,origin_u,origin_v,scale_u,scale_v,depth_scale,argb);
                                SQ_POINT_PREFETCH(batch->points+next->indices[2]);
                                sq_put_point(corner[2],PVR_CMD_VERTEX_EOL,origin_u,origin_v,scale_u,scale_v,depth_scale,argb);
                                SQ_POINT_PREFETCH(batch->points+next->indices[3]);
                            }
                        }
                        DRAW_PHASE_END(5,intact_started);
#ifdef NATIVE_PROFILE
                        draw_clip_paths[0]+=corners-2u;
#endif
                        draw_count+=corners-2u;
                        SQ_COUNT(draw_opaque,list==PVR_LIST_OP_POLY ? corners-2u : 0);
                        SQ_COUNT(draw_cutout,list==PVR_LIST_PT_POLY ? corners-2u : 0);
                        continue;
                    }
                }
                if(!sq_emit_face_general(list,s,batch,f,index,uv,depth_scale,selected,&previous,inspect)) return 0;
            }
        }
    }
    *previous_header=previous;
    return 1;
}
static int sq_emit_range(unsigned list,
        unsigned source_first, unsigned source_end, unsigned batch_first, unsigned batch_end) {
    const pvr_poly_hdr_t *previous=NULL;
    if(!draw_header_count) sq_ta_vertices=0;
    for(unsigned s=source_first;s<source_end;++s)
        for(unsigned b=batch_first;b<sq_sources[s].count && b<batch_end;++b) {
            sq_ss_batch(sq_sources[s].batches+b,&previous);
            SQ_PATH_EMIT(2,s,sq_sources[s].batches+b,list,list==PVR_LIST_PT_POLY ? sq_sources[s].batches[b].cutout_count : sq_sources[s].batches[b].opaque_count);
            if(!sq_emit_batch_general(list,s,sq_sources[s].batches+b,0,&previous)) return 0;
        }
    return 1;
}

/* n more unclipped faces fit the frame's stream whatever they turn out to be:
 * each writes at most four vertices (a strip run of r faces: at most 2r+2),
 * two triangles and one header, and draw_ta_fits only tightens as vertices
 * and headers grow. While this holds the per-face guards of sq_emit_range
 * cannot fail, and the production emitter does not evaluate them. */
static inline int sq_faces_fit(unsigned n) {
    return draw_count+2u*n<=NM_DRAW_TRIANGLES && draw_ta_fits((sq_ta_vertices+4u*n+2u)/3u,draw_header_count+n);
}
/* The store-queue target address in a local of the emitting loop.
 * pvr_dr_target() is `pvr_dr_addr ^= 32` on a global, which the compiler must
 * load and store around every vertex (the vertex writer and the queue flush
 * are assembly that may touch memory). The same alternation in a register,
 * written back before anything else submits: the same addresses in the same
 * order. Host builds keep the harness's pvr_dr_target()/pvr_dr_commit(). */
#ifdef __DREAMCAST__
typedef uint32_t nm_sq_target_t;
#define SQ_DR_BEGIN() (pvr_dr_addr)
#define SQ_DR_TARGET(state) __builtin_assume_aligned((void *)((state)^=32),32)
#define SQ_DR_COMMIT(out) sq_flush(out)
#define SQ_DR_END(state) ((void)(pvr_dr_addr=(state)))
#else
typedef unsigned nm_sq_target_t;
#define SQ_DR_BEGIN() 0u
#define SQ_DR_TARGET(state) ((void)(state),pvr_dr_target())
#define SQ_DR_COMMIT(out) pvr_dr_commit(out)
#define SQ_DR_END(state) ((void)(state))
#endif
#ifndef SQ_PROBE_ADD
#define SQ_PROBE_ADD(counter,amount) ((void)0)
#endif
static inline __attribute__((always_inline)) int sq_corner_inside(const nm_cached_vertex_t *p) {
    return !(nm_cached_outside(p) || p->q<=NM_SKY_DEPTH); /* sq_face_inside, one corner */
}
/* A strip run for the production emitter, by the rule of sq_emit_range: the
 * longest run of queued, unclipped, consecutive faces of one offline strip
 * that share the first face's header, starting at `entry` (a face whose
 * record allows a run and whose corners the caller found intact), `left`
 * entries remaining including it. Returns the faces written as one TA strip,
 * or 1 when there is no run (nothing is written). No guards: the caller has
 * proven the batch (sq_faces_fit). Counts in the frame's globals.
 * A run of n faces costs the call, one header compare and its n faces: per
 * member the queue entry, the header of its binding when the binding differs,
 * its corners' status words, and per vertex one 8-byte strip entry and the
 * vertex writer with the store-queue address in a register. */
/* Host tools may send every run through the vertex-table body below, the one
 * a run with rewritten u,v takes (the simulator's SIM_RUN_VERTEX_TABLE). */
#ifndef SQ_RUN_VERTEX_TABLE
#define SQ_RUN_VERTEX_TABLE 0
#endif
static __attribute__((noinline)) unsigned sq_emit_run(unsigned list,unsigned s,const nm_sq_batch_t *batch,
        const uint32_t *entry,unsigned left,int step,const pvr_poly_hdr_t **previous) {
    const nm_sq_bind_t *const records=sq_emit_records[s];
    const uintptr_t points=sq_points_mid(batch->points);
    const unsigned f=*entry>>16, index=*entry&65535u;
    const nm_sq_face_t *const face=sq_emit_faces[s]+batch->mesh->face_first+f;
    const nm_sq_class_t *const uv=records[index].group;
    const pvr_poly_hdr_t *const selected=uv->header;
    unsigned run=1,triangles=face->count-2u;
    unsigned special=face->special; /* of every member: a rewritten u,v anywhere in the run */
    const unsigned strip_faces=special&SQ_FACE_RUN;
    const unsigned limit=strip_faces<left ? strip_faces : left;
    {
        /* The first face is intact (the caller tested its corners). A member
         * shares two corners with the face before it; which ones depends on
         * the strip, so all of its corners are tested: four loads and
         * compares. sq_point_drawable is sq_corner_inside, the test of
         * sq_emit_range, in one compare (see there). */
        const uint32_t *next_entry=entry+step;
        const nm_sq_face_t *member=face+1;
        for(;run<limit;++run,++member,next_entry+=step) {
            const uint32_t next=*next_entry;
            if((next>>16)!=f+run || ((next&65535u)!=index && records[next&65535u].group->header!=selected)) break;
            const unsigned corners=member->count;
            int member_inside=sq_point_drawable(sq_corner_point(points,member->corner[0]))&
                sq_point_drawable(sq_corner_point(points,member->corner[1]))&sq_point_drawable(sq_corner_point(points,member->corner[2]));
            if(corners==4) member_inside&=sq_point_drawable(sq_corner_point(points,member->corner[3]));
            if(!member_inside) break;
            triangles+=corners-2u; special|=member->special;
        }
    }
    if(run<2) { SQ_PROBE(emit_runs_refused); return 1; }
    DRAW_PHASE_START(strip_started);
    nm_sq_target_t target=SQ_DR_BEGIN();
    if(*previous!=selected) {
        pvr_poly_hdr_t *header=SQ_DR_TARGET(target); *header=*selected; SQ_DR_COMMIT(header);
        *previous=selected; ++draw_header_count;
    }
    sq_ta_vertices+=triangles+2u;
    float depth_scale=uv->depth;
    const unsigned strip_layer=face->layer; /* one layer per strip */
    if(strip_layer) depth_scale*=draw_layer_scale[strip_layer];
    const float origin_u=uv->origin[0],origin_v=uv->origin[1],scale_u=uv->scale[0],scale_v=uv->scale[1];
    const nm_sq_strip_corner_t *corner=sq_strip_corners[s]+face->strip;
    if(!(special&SQ_FACE_DYNAMIC_UV) && !SQ_RUN_VERTEX_TABLE) {
        /* One 8-byte entry per vertex: the cached point of its position and
         * its own u,v; the colour changes with the face. A face supplies the
         * colour of its triangles' provoking vertices: its count-2 vertices,
         * and the strip's first two go with the first (NM_FLAT_PROVOKING_LAST)
         * or the last face. */
        const uint32_t *member_entry=entry;
        const nm_sq_face_t *member=face;
        const nm_sq_strip_corner_t *const last_corner=corner+triangles+1u;
        for(unsigned j=0;j<run;++j,++member,member_entry+=step) {
            const unsigned binding=*member_entry&65535u;
            const uint32_t member_argb=records[binding].argb;
            unsigned own=member->count-2u;
            if(j==(NM_FLAT_PROVOKING_LAST ? 0u : run-1)) own+=2;
            do {
                SQ_VERTEX_TAG(batch->mesh->face_first+f+j,binding);
                pvr_vertex_t *const out=SQ_DR_TARGET(target);
                sq_write_corner(out,sq_corner_point(points,corner->corner),corner==last_corner ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                    corner->uv[0],corner->uv[1],origin_u,origin_v,scale_u,scale_v,depth_scale,member_argb);
                SQ_DR_COMMIT(out);
                ++corner;
            } while(--own);
        }
        SQ_DR_END(target);
    } else {
        /* A run with a face whose u,v the scene rewrites (window
         * reflections): every vertex through the vertex table. */
        SQ_DR_END(target);
        const nm_pack_t *const pack=s ? world_draw.pack : draw_pack;
        const uint16_t *indices=pack->strip_indices+face->strip;
        unsigned at=0;
        for(unsigned j=0;j<run;++j) {
            const unsigned member=entry[step*(int)j]&65535u;
            const uint32_t member_argb=records[member].argb;
            /* Vertices whose colour this face supplies. */
            unsigned own=face[j].count-2u;
            if(j==(NM_FLAT_PROVOKING_LAST ? 0u : run-1)) own+=2;
            for(unsigned v=0;v<own;++v,++at) {
                SQ_VERTEX_TAG(batch->mesh->face_first+f+j,member);
                nm_cached_vertex_t held;
                sq_put_point(sq_corner(batch,s,pack,indices[at],&held),at==triangles+1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                    origin_u,origin_v,scale_u,scale_v,depth_scale,member_argb);
            }
        }
    }
    DRAW_PHASE_END(5,strip_started);
#ifdef NATIVE_PROFILE
    draw_clip_paths[0]+=triangles;
#endif
    draw_count+=triangles;
    SQ_COUNT(draw_opaque,list==PVR_LIST_OP_POLY ? triangles : 0);
    SQ_COUNT(draw_cutout,list==PVR_LIST_PT_POLY ? triangles : 0);
    SQ_COUNT(sq_flat_runs[s],1); SQ_COUNT(sq_flat_run_faces[s],run); SQ_COUNT(sq_flat_run_vertices[s],triangles+2u);
    SQ_PROBE(emit_runs);
    if((special&SQ_FACE_DYNAMIC_UV) || SQ_RUN_VERTEX_TABLE) SQ_PROBE(emit_runs_dynamic);
    SQ_COUNT(sq_joined_strips,1); SQ_COUNT(sq_strip_vertices_saved,2*(run-1));
    (void)list;
    return run;
}
/* The production emitter's loop over the queued faces of one batch on one
 * list, from queue position `first`; `step` is +1 (opaque queue, read
 * upwards) or -1 (punch-through queue, read downwards) and a constant of
 * each of the two instances below. Same words in the same order as
 * sq_emit_range; what differs is the work around them:
 * - a face is read from its 16-byte emission record (nm_sq_face_t), never
 *   from the face table, and a strip run is tried only where the record
 *   allows one and the next queue entry is the next face;
 * - no TA or triangle guard: the caller has proven all remaining faces
 *   (sq_faces_fit);
 * - the loop touches no global: store-queue address, last header, vertex and
 *   header counts are locals, written back (SQ_FLUSH) before a strip run, a
 *   clipped face or the return. Triangles of intact faces are their vertices
 *   minus two per face, counted from the queue positions passed.
 * Returns the queue position after the last face it handled: the face count
 * when the list is done, less after a clipped face (written through
 * sq_emit_face_general with its own guards), so that the caller proves the
 * rest again. UINT_MAX: the clipped face failed. */
/* Window reflections: this frame's u,v of a face from the vertex table. */
static __attribute__((noinline)) void sq_live_uv(unsigned s,const nm_sq_batch_t *batch,unsigned f,uint16_t uv[4][2]) {
    const nm_pack_t *const pack=s ? world_draw.pack : draw_pack;
    const nm_face_t *const face=pack->faces+batch->mesh->face_first+f;
    for(unsigned v=0;v<4;++v) {
        const nm_vertex_t *vertex=pack->vertices+batch->mesh->vertex_first+(v<face->count ? face->indices[v] : 0);
        uv[v][0]=vertex->u; uv[v][1]=vertex->v;
    }
}
#define SQ_FLUSH(intact) do { \
    const unsigned flushed_faces=(unsigned)(intact),flushed_triangles=vertices-2u*flushed_faces; \
    sq_ta_vertices+=vertices; draw_count+=flushed_triangles; draw_header_count=headers; vertices=0; \
    *previous_header=previous; SQ_DR_END(target); \
    SQ_PROBE_ADD(emit_intact_faces,flushed_faces); \
    SQ_COUNT(draw_opaque,step>0 ? flushed_triangles : 0); SQ_COUNT(draw_cutout,step<0 ? flushed_triangles : 0); \
    SQ_CLIP_PATH_COUNT(flushed_triangles); } while(0)
#ifdef NATIVE_PROFILE
#define SQ_CLIP_PATH_COUNT(triangles) (draw_clip_paths[0]+=(triangles))
#else
#define SQ_CLIP_PATH_COUNT(triangles) ((void)(triangles))
#endif
#define SQ_PUT(point,k,flags) do { \
    pvr_vertex_t *const put_out=SQ_DR_TARGET(target); \
    sq_write_corner(put_out,(point),(flags),uvs[k][0],uvs[k][1],origin_u,origin_v,scale_u,scale_v,depth_scale,argb); \
    SQ_DR_COMMIT(put_out); } while(0)
static inline __attribute__((always_inline)) unsigned sq_emit_face_loop(const unsigned s,const nm_sq_batch_t *const batch,
        const unsigned first,const pvr_poly_hdr_t **const previous_header,const int step) {
    const unsigned list=step<0 ? PVR_LIST_PT_POLY : PVR_LIST_OP_POLY;
    const unsigned face_count=step<0 ? batch->cutout_count : batch->opaque_count;
    const nm_sq_face_t *const faces=sq_emit_faces[s]+batch->mesh->face_first;
    const nm_sq_bind_t *const records=sq_emit_records[s];
    const uintptr_t points=sq_points_mid(batch->points);
    /* The punch-through queue grows down from the end of the batch's entries
     * and is read downwards: ascending face order either way. */
    const uint32_t *const origin=step<0 ? batch->bindings+batch->mesh->face_count-1 : batch->bindings;
    const uint32_t *entry=origin+step*(int)first;
    const uint32_t *const last=origin+step*(int)(face_count-1);
    const uint32_t *mark=entry; /* entries from here to `entry` were written as intact faces */
    const pvr_poly_hdr_t *previous=*previous_header;
    unsigned headers=draw_header_count,vertices=0,intact_before=0;
    nm_sq_target_t target=SQ_DR_BEGIN();
    DRAW_PHASE_START(emit_started);
    for(;;entry+=step) {
        const unsigned f=*entry>>16, index=*entry&65535u;
        const nm_sq_face_t *const face=faces+f;
        const unsigned special=face->special;
        const uint16_t (*uvs)[2]=face->uv;
        uint16_t live_uv[4][2];
        const unsigned corners=face->count;
        const nm_cached_vertex_t *const corner0=sq_corner_point(points,face->corner[0]);
        const nm_cached_vertex_t *const corner1=sq_corner_point(points,face->corner[1]);
        const nm_cached_vertex_t *const corner2=sq_corner_point(points,face->corner[2]);
        const nm_cached_vertex_t *const corner3=sq_corner_point(points,face->corner[3]);
        /* The common face: every corner projected and nearer than the sky. */
        int intact=sq_point_drawable(corner0)&sq_point_drawable(corner1)&sq_point_drawable(corner2);
        if(corners==4) intact&=sq_point_drawable(corner3);
        if(!intact) {
            /* A face with a corner behind the near plane or beyond the guard
             * band. Most of them have all corners beyond the same plane (the
             * part of a face group, straddling the view, that is outside
             * it): the clipper returns nothing for each of its triangles
             * (nm_project_cached_triangle: an invalid corner, or a plane
             * bit all three share; fan (0,1,2),(0,2,3)), so nothing is
             * written and no state changes. Decided here on the four status
             * words; the loop goes on. */
            const unsigned o0=nm_cached_outside(corner0),o1=nm_cached_outside(corner1),o2=nm_cached_outside(corner2);
            const unsigned o3=corners==4 ? nm_cached_outside(corner3) : o2;
            if((((o0|o1|o2)&32u) || (o0&o1&o2)) && (((o0|o2|o3)&32u) || (o0&o2&o3))) {
                SQ_PROBE(emit_outside_faces);
                if(entry==last) { entry-=step; break; } /* the flush below counts the faces up to `entry` */
                /* Not an intact face: the intact faces before it are counted
                 * now (their vertices stay in `vertices`). */
                intact_before+=(unsigned)((entry-mark)*step); mark=entry+step;
                continue;
            }
            /* A clipped face: the general loop with its own guards. It may
             * write more than the four vertices and one header the proof
             * allowed it, so the rest of the batch is proven again; if that
             * fails the caller hands the rest to the guarded emitter. */
            SQ_FLUSH(intact_before+(unsigned)((entry-mark)*step)); intact_before=0;
            nm_sq_emit_t clipped;
            sq_emit_expand(&clipped,records+index);
            float clipped_depth=clipped.depth;
            if(face->layer) clipped_depth*=draw_layer_scale[face->layer];
            if(!sq_emit_face_general(list,s,batch,f,index,&clipped,clipped_depth,clipped.header,previous_header,0)) return UINT_MAX;
            if(entry==last || !sq_faces_fit((unsigned)((last-entry)*step))) {
                DRAW_PHASE_END(5,emit_started);
                return (unsigned)((entry-origin)*step)+1u;
            }
            SQ_PROBE(emit_reproven);
            previous=*previous_header; headers=draw_header_count; target=SQ_DR_BEGIN();
            mark=entry+step;
            continue;
        }
        if(special) {
          if(special&SQ_FACE_DYNAMIC_UV) { sq_live_uv(s,batch,f,live_uv); uvs=live_uv; }
          /* A strip run may start here (its first face is intact) and the
           * next queued face is the next face of the strip. */
          if((special&SQ_FACE_RUN) && entry!=last && (entry[step]>>16)==f+1) {
            SQ_FLUSH(intact_before+(unsigned)((entry-mark)*step)); intact_before=0;
            const unsigned run=sq_emit_run(list,s,batch,entry,(unsigned)((last-entry)*step)+1u,step,previous_header);
            previous=*previous_header; headers=draw_header_count; target=SQ_DR_BEGIN();
            mark=entry;
            if(run>1) {
                entry+=step*(int)(run-1); mark=entry+step;
                if(entry==last) break;
                continue;
            }
          }
        }
        /* The binding's colour and its class: header, UV mapping, depth. */
        const nm_sq_bind_t *const bound=records+index;
        const nm_sq_class_t *const uv=bound->group;
        const uint32_t argb=bound->argb;
        const pvr_poly_hdr_t *const selected=uv->header;
        const float origin_u=uv->origin[0],origin_v=uv->origin[1];
        const float scale_u=uv->scale[0],scale_v=uv->scale[1];
        float depth_scale=uv->depth;
        const unsigned layer=face->layer;
        if(layer) depth_scale*=draw_layer_scale[layer]; /* see sq_emit_range */
        if(previous!=selected) {
            pvr_poly_hdr_t *header=SQ_DR_TARGET(target); *header=*selected; SQ_DR_COMMIT(header);
            previous=selected; ++headers;
        }
        vertices+=corners;
        /* Corners of the next queued face (SQ_POINT_PREFETCH); the last face
         * of a list requests its own again. One strip of the three or four
         * cached points: fan (0,1,2),(0,2,3) as strip (1,2,0,3). */
        const nm_sq_face_t *const next=faces+((entry!=last ? entry[step] : *entry)>>16);
        SQ_VERTEX_TAG(batch->mesh->face_first+f,index);
        if(corners==4) {
            SQ_POINT_PREFETCH(sq_corner_point(points,next->corner[0]));
            SQ_PUT(corner1,1,PVR_CMD_VERTEX);
            SQ_POINT_PREFETCH(sq_corner_point(points,next->corner[1]));
            SQ_PUT(corner2,2,PVR_CMD_VERTEX);
            SQ_POINT_PREFETCH(sq_corner_point(points,next->corner[2]));
            SQ_PUT(corner0,0,PVR_CMD_VERTEX);
            SQ_POINT_PREFETCH(sq_corner_point(points,next->corner[3]));
            SQ_PUT(corner3,3,PVR_CMD_VERTEX_EOL);
        } else {
            SQ_POINT_PREFETCH(sq_corner_point(points,next->corner[0]));
            SQ_PUT(corner0,0,PVR_CMD_VERTEX);
            SQ_POINT_PREFETCH(sq_corner_point(points,next->corner[1]));
            SQ_PUT(corner1,1,PVR_CMD_VERTEX);
            SQ_POINT_PREFETCH(sq_corner_point(points,next->corner[2]));
            SQ_PUT(corner2,2,PVR_CMD_VERTEX_EOL);
        }
        if(entry==last) break;
    }
    SQ_FLUSH(intact_before+(unsigned)((entry-mark)*step+1));
    DRAW_PHASE_END(5,emit_started);
    return face_count;
}
static __attribute__((noinline)) unsigned sq_emit_faces_op(unsigned s,const nm_sq_batch_t *batch,unsigned first,
        const pvr_poly_hdr_t **previous) { return sq_emit_face_loop(s,batch,first,previous,1); }
static __attribute__((noinline)) unsigned sq_emit_faces_pt(unsigned s,const nm_sq_batch_t *batch,unsigned first,
        const pvr_poly_hdr_t **previous) { return sq_emit_face_loop(s,batch,first,previous,-1); }
/* Host tools may force the general emitter (the simulator's SIM_GENERAL_EMITTER). */
#ifndef SQ_GENERAL_EMITTER
#define SQ_GENERAL_EMITTER 0
#endif
/* The inspection views and builds without emission tables keep the general
 * emitter (sq_emit_range, which reads the queues); a frame of the game takes
 * the production one. */
static inline int sq_general_emitter(unsigned source) {
    return draw_inspect_strips || draw_untextured || SQ_GENERAL_EMITTER || !sq_emit_records[source] || !sq_emit_faces[source];
}

#if SQ_STREAMS
/* ---- Prebuilt world streams, part 2: the emitter and the tables ---------
 * (part 1, with the prepare step, is above sq_prepare_batch.) */
static int sq_stream_ready(const nm_sq_walk_t *walk) {
    return sq_stream_faces && walk->wide && sq_stream_meshes[walk->mesh-walk->pack->meshes] && !sq_general_emitter(1);
}
#ifndef SQ_STREAM_FACE_PROBE
/* Host tools: every face (record) the stream emitter decides a side for. */
#define SQ_STREAM_FACE_PROBE(batch,face,rear) ((void)0)
#endif
/* The emitter's state: constants of the batch and list, and the stream's
 * running state, which the hot loop (sq_stream_stretch) keeps in locals and
 * stores here before it calls one of the helpers. classes: the batch's class
 * of each face group (sq_stream_batch_prepare), by absolute group: 0 not
 * drawn, 1 drawn with a corner test, 2 drawn without one (SQ_CLASS_PROVEN).
 * counts: vertices in the low half, strips in the high half since the last
 * commit (triangles = vertices - 2 strips). */
typedef struct {
    const nm_sq_batch_t *batch;
    const pvr_poly_hdr_t **previous_header;
    const nm_sq_stream_face_t *faces, *stop, *span_end; /* the mesh's first face, end of the stretch, of the list */
    const nm_sq_stream_vertex_t *strips;               /* the mesh's strip vertex records */
    const uint16_t *rank;                              /* the mesh's face ranks */
    const uint8_t *classes;
    const nm_sq_facing_t *facing;
    uintptr_t points;
    int xmtrx, guarded, cutout;
    const pvr_poly_hdr_t *previous;
    unsigned headers, counts;
    nm_sq_target_t target;
} nm_sq_stream_walk_t;
#define SQ_CLASS_PROVEN 2u
/* The stream's counts into the frame's globals (sq_ta_vertices, draw_count,
 * draw_header_count, the caller's last header, pvr_dr_addr). */
static void sq_stream_commit(nm_sq_stream_walk_t *w) {
    const unsigned vertices=w->counts&0xffffu,triangles=vertices-2u*(w->counts>>16);
    sq_ta_vertices+=vertices; draw_count+=triangles; draw_header_count=w->headers;
    SQ_COUNT(draw_opaque,w->cutout ? 0 : triangles); SQ_COUNT(draw_cutout,w->cutout ? triangles : 0); SQ_CLIP_PATH_COUNT(triangles);
    w->counts=0; *w->previous_header=w->previous; SQ_DR_END(w->target);
}
/* The side of a face the plane constants leave undecided (or a batch without
 * them), by the reference test of sq_baked_binding: it transforms corner 2
 * and the normal, so the instance matrix is loaded into XMTRX on first use in
 * a list callback. 1 rear, 0 front, -1 invalid arithmetic. */
static __attribute__((noinline)) int sq_stream_reference(nm_sq_stream_walk_t *w,const nm_sq_stream_face_t *face) {
    const nm_pack_t *const pack=world_draw.pack;
    const nm_sq_batch_t *const batch=w->batch;
    const nm_mesh_t *const mesh=batch->mesh;
    const nm_instance_t *const inst=pack->instances+batch->instance_index;
    const unsigned f=face->index;
    if(!w->xmtrx) { shz_xmtrx_load_3x4(&inst->transform); w->xmtrx=1; }
    (void)inst;
    const int binding=sq_baked_binding(pack,mesh,pack->faces+mesh->face_first+f,sq_baked_faces[mesh->face_first+f],NULL,NULL);
    if(binding==-2) return -1;
    SQ_STREAM_FACE_PROBE(batch,face,binding!=(int)face->front);
    return binding!=(int)face->front;
}
#ifdef NATIVE_LIGHT_VERIFY
static __attribute__((noinline)) int sq_stream_verify(nm_sq_stream_walk_t *w,const nm_sq_stream_face_t *face,int side) {
    const nm_pack_t *const pack=world_draw.pack;
    const nm_sq_batch_t *const batch=w->batch;
    const nm_instance_t *const inst=pack->instances+batch->instance_index;
    if(!w->xmtrx) { shz_xmtrx_load_3x4(&inst->transform); w->xmtrx=1; }
    ++sq_facing_decided[1][side<0];
    return sq_face_side_verify(pack,batch->mesh,pack->faces+batch->mesh->face_first+face->index,w->facing,&face->side,side);
}
#endif
/* Is the face drawn (its group walked), and with which binding: 1 drawn,
 * 0 not (group not drawn, culled rear), -1 invalid arithmetic.
 * The side as sq_baked_binding decides it: from the plane constants where
 * they are beyond doubt (sq_face_side), else by the reference test. */
static inline __attribute__((always_inline)) int sq_stream_decide(nm_sq_stream_walk_t *w,const nm_sq_stream_face_t *face,
        unsigned *group,unsigned *binding,uint32_t *argb,const pvr_poly_hdr_t **selected) {
    const unsigned flags=face->flags;
    const unsigned class=w->classes[face->group];
    if(!class) { SQ_STREAM_COUNT(SQ_STREAM_NOT_WALKED,1); return 0; }
    *group=class; *binding=face->front; *argb=face->argb; *selected=face->header;
    if(!(flags&SQ_STREAM_SIDE)) { SQ_COUNT(draw_side_same,1); return 1; }
    SQ_COUNT(draw_side_tested,1); SQ_STREAM_COUNT(SQ_STREAM_SIDE_TESTS,1);
    const int side=w->facing ? sq_face_side(w->facing,&face->side,face->normal) : 0;
    int rear;
    if(side) {
#ifdef NATIVE_LIGHT_VERIFY
        if(!sq_stream_verify(w,face,side)) return -1;
#endif
        SQ_STREAM_FACE_PROBE(w->batch,face,side<0);
        rear=side<0;
    } else {
#ifdef NATIVE_LIGHT_VERIFY
        ++sq_facing_reference[1];
#endif
        SQ_STREAM_COUNT(SQ_STREAM_REFERENCE,1);
        rear=sq_stream_reference(w,face);
        if(rear<0) return -1;
    }
    if(!rear) return 1;
    if(!(flags&SQ_STREAM_TWO)) { SQ_COUNT(draw_rear,1); SQ_STREAM_COUNT(SQ_STREAM_CULLED,1); return 0; }
    SQ_STREAM_COUNT(SQ_STREAM_TWO_REAR,1);
    const nm_sq_bind_t *const record=sq_emit_records[1]+face->back;
    *binding=face->back; *argb=record->argb; *selected=record->group->header;
    return 1;
}
/* Guarded mode only (the list could not be proven to fit): the next write of
 * `v` vertices, `t` triangles and maybe a header fits, as sq_emit_range
 * checks it. Prints the general emitter's message when it does not. */
static int sq_stream_fits(const nm_sq_stream_walk_t *w,unsigned v,unsigned t,int header) {
    const unsigned vertices=w->counts&0xffffu,triangles=vertices-2u*(w->counts>>16);
    if(draw_count+triangles+t<=NM_DRAW_TRIANGLES && draw_ta_fits((sq_ta_vertices+vertices+v+2u)/3u,w->headers+(unsigned)header)) return 1;
    printf("FAIL SQ TA budget: triangles=%u vertices=%u headers=%u next-header=%u source=1\n",
        draw_count+triangles,sq_ta_vertices+vertices,w->headers,(unsigned)header);
    return 0;
}
#define SQ_STREAM_PUT(corner_word,texture,flags,depth_scale,colour) do { \
    pvr_vertex_t *const put_out=SQ_DR_TARGET(target); \
    sq_write_stream(put_out,(const float *)(xyq+(uintptr_t)(intptr_t)((int)(corner_word)*4)),(flags),(texture),(depth_scale),(colour)); \
    SQ_DR_COMMIT(put_out); } while(0)
/* A single face of a stream record: its 3 or 4 vertices in emission order,
 * with sq_write_stream's words, the last one ending the strip. `a` and `b`
 * are the store-queue addresses the vertices alternate between (first, second,
 * first, second); the caller passes the same two values twice, and the check
 * before the first frame (sq_stream_face_check) four separate buffers, so
 * that every vertex is compared with sq_write_stream_c. 23 instructions a
 * vertex, 3 more for the last one's flag word. The caller toggles its target
 * once per vertex. */
#ifdef __DREAMCAST__
#define SQ_STREAM_VERTEX_ASM(corner,u,v,address,flags_setup,flags) \
    "mov.w @(" corner ",%[face]),r0\n\t" \
    "shll2 r0\n\t" \
    "add %[xyq],r0\n\t" \
    "fmov.s @r0+,%[x]\n\t" \
    "fmov.s @r0+,%[y]\n\t" \
    "fmov.s @r0,%[q]\n\t" \
    "mov.l @(" u ",%[face]),r2\n\t" \
    "mov.l @(" v ",%[face]),r3\n\t" \
    "mov %[" address "],r1\n\t" \
    "add #32,r1\n\t" \
    "mov #0,r0\n\t" \
    "mov.l r0,@-r1\n\t" \
    "mov.l %[argb],@-r1\n\t" \
    "fmul %[ds],%[q]\n\t" \
    "mov.l r3,@-r1\n\t" \
    "fmul %[sy],%[y]\n\t" \
    "mov.l r2,@-r1\n\t" \
    "fmul %[sx],%[x]\n\t" \
    flags_setup \
    "fmov.s %[q],@-r1\n\t" \
    "fmov.s %[y],@-r1\n\t" \
    "fmov.s %[x],@-r1\n\t" \
    "mov.l " flags ",@-r1\n\t" \
    "pref @r1\n\t"
/* The last vertex's flag word, PVR_CMD_VERTEX_EOL (0xf0000000), built in r2. */
#define SQ_STREAM_EOL_ASM "mov #-16,r2\n\tshll16 r2\n\tshll8 r2\n\t"
static inline __attribute__((always_inline)) void sq_stream_put_face_sh4(const nm_sq_stream_face_t *face,uintptr_t xyq,
        uintptr_t a0,uintptr_t a1,uintptr_t a2,uintptr_t a3,uint32_t argb,float depth_scale) {
    _Static_assert(offsetof(nm_sq_stream_face_t,corner)==16 && offsetof(nm_sq_stream_face_t,uv)==32,"the displacements below");
    _Static_assert(PVR_CMD_VERTEX_EOL==0xf0000000u,"SQ_STREAM_EOL_ASM");
    const float screen_x=(640.0f/496.0f)*ENJ_XSCALE,screen_y=480.0f/384.0f;
    const uint32_t vertex=PVR_CMD_VERTEX;
    float x,y,q;
    if(face->count==4) asm volatile(
        SQ_STREAM_VERTEX_ASM("16","32","36","a0","","%[vertex]")
        SQ_STREAM_VERTEX_ASM("18","40","44","a1","","%[vertex]")
        SQ_STREAM_VERTEX_ASM("20","48","52","a2","","%[vertex]")
        SQ_STREAM_VERTEX_ASM("22","56","60","a3",SQ_STREAM_EOL_ASM,"r2")
        : [x] "=&f" (x), [y] "=&f" (y), [q] "=&f" (q)
        : [face] "r" (face), [xyq] "r" (xyq), [a0] "r" (a0), [a1] "r" (a1), [a2] "r" (a2), [a3] "r" (a3),
          [argb] "r" (argb), [vertex] "r" (vertex),
          [ds] "f" (depth_scale), [sx] "f" (screen_x), [sy] "f" (screen_y)
        : "r0", "r1", "r2", "r3", "memory");
    else asm volatile(
        SQ_STREAM_VERTEX_ASM("16","32","36","a0","","%[vertex]")
        SQ_STREAM_VERTEX_ASM("18","40","44","a1","","%[vertex]")
        SQ_STREAM_VERTEX_ASM("20","48","52","a2",SQ_STREAM_EOL_ASM,"r2")
        : [x] "=&f" (x), [y] "=&f" (y), [q] "=&f" (q)
        : [face] "r" (face), [xyq] "r" (xyq), [a0] "r" (a0), [a1] "r" (a1), [a2] "r" (a2),
          [argb] "r" (argb), [vertex] "r" (vertex),
          [ds] "f" (depth_scale), [sx] "f" (screen_x), [sy] "f" (screen_y)
        : "r0", "r1", "r2", "r3", "memory");
}
/* Before the first frame: sq_stream_put_face_sh4 against sq_write_stream_c,
 * every vertex of a triangle and a quad into four buffers, all eight words. */
static __attribute__((noinline)) int sq_stream_face_check(void) {
    alignas(32) nm_cached_vertex_t points[4];
    alignas(32) pvr_vertex_t written[4],expected[4];
    alignas(32) nm_sq_stream_face_t face;
    nm_memset(points,0,sizeof(points)); nm_memset(&face,0,sizeof(face));
    for(unsigned k=0;k<4;++k) {
        points[k].x=12.5f+(float)k*101.25f; points[k].y=-3.0f+(float)k*77.5f; points[k].q=0.01f+(float)k*0.37f;
        face.corner[3-k]=(int16_t)(k*8u); /* corner words: index*8 (sq_corner_of minus its bias) */
        face.uv[k][0]=(float)k*0.125f+0.0625f; face.uv[k][1]=1.0f-(float)k*0.25f;
    }
    const uintptr_t xyq=(uintptr_t)points+offsetof(nm_cached_vertex_t,x);
    for(unsigned count=3;count<=4;++count) {
        face.count=(uint8_t)count;
        const float depth=count==3 ? 0.5f*(1.0f+3.0f/1024.0f) : 1.0f;
        const uint32_t argb=0xff123456u+count;
        nm_memset(written,0xa5,sizeof(written));
        sq_stream_put_face_sh4(&face,xyq,(uintptr_t)(written+0),(uintptr_t)(written+1),(uintptr_t)(written+2),(uintptr_t)(written+3),argb,depth);
        for(unsigned k=0;k<count;++k)
            sq_write_stream_c(expected+k,(const float *)(xyq+(uintptr_t)(intptr_t)(face.corner[k]*4)),k+1==count ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                face.uv[k],depth,argb);
        if(memcmp(written,expected,count*sizeof(pvr_vertex_t))) {
            printf("FAIL: SQ stream face writer differs from sq_write_stream_c (%u vertices)\n",count);
            return 0;
        }
    }
    puts("Native SQ stream face writer PASS: every vertex of a triangle and a quad equals sq_write_stream_c, all eight words");
    return 1;
}
/* The vertices of a strip run (sq_stream_run) from its strip vertex records:
 * the first two with the colour `argb` (the run's first face's), every other
 * one with its record's colour, the last one ending the strip. Vertex k goes
 * to store-queue address a (k even) or b (k odd): the two the caller's target
 * alternates between. Per vertex sq_write_stream's words, 22 instructions (the
 * face writer's 23 less the address add and the zero load, plus the colour
 * load); the loop takes four vertices at a time, 3 instructions for the loop:
 * 22.75 a vertex. A record's 32-bit corner word lets every load of four
 * records use a displacement. n is at least 4. */
#define SQ_RUN_VERTEX_ASM(record,colour,address,flags_setup,flags) \
    "mov.l @(" record "+12,%[record]),r0\n\t" \
    "shll2 r0\n\t" \
    "add %[xyq],r0\n\t" \
    "fmov.s @r0+,%[x]\n\t" \
    "fmov.s @r0+,%[y]\n\t" \
    "fmov.s @r0,%[q]\n\t" \
    "mov.l @(" record "+0,%[record]),r2\n\t" \
    "mov.l @(" record "+4,%[record]),r3\n\t" \
    "mov %[" address "],r1\n\t" \
    "mov.l %[zero],@-r1\n\t" \
    colour \
    "fmul %[ds],%[q]\n\t" \
    "mov.l r3,@-r1\n\t" \
    "fmul %[sy],%[y]\n\t" \
    "mov.l r2,@-r1\n\t" \
    "fmul %[sx],%[x]\n\t" \
    flags_setup \
    "fmov.s %[q],@-r1\n\t" \
    "fmov.s %[y],@-r1\n\t" \
    "fmov.s %[x],@-r1\n\t" \
    "mov.l " flags ",@-r1\n\t" \
    "pref @r1\n\t"
#define SQ_RUN_COLOUR_RECORD(record) "mov.l @(" record "+8,%[record]),r0\n\tmov.l r0,@-r1\n\t"
#define SQ_RUN_COLOUR_REGISTER "mov.l %[argb],@-r1\n\t"
#define SQ_RUN_RECORD(record,address) SQ_RUN_VERTEX_ASM(record,SQ_RUN_COLOUR_RECORD(record),address,"","%[vertex]")
#define SQ_RUN_LAST(record,address) SQ_RUN_VERTEX_ASM(record,SQ_RUN_COLOUR_RECORD(record),address,SQ_STREAM_EOL_ASM,"r2")
#define SQ_RUN_OPERANDS [x] "=&f" (x), [y] "=&f" (y), [q] "=&f" (q)
#define SQ_RUN_INPUTS [xyq] "r" (xyq), [a] "r" (a32), [b] "r" (b32), [zero] "r" (zero), [vertex] "r" (vertex), \
    [ds] "f" (depth_scale), [sx] "f" (screen_x), [sy] "f" (screen_y)
#define SQ_RUN_CLOBBERS "r0", "r1", "r2", "r3", "memory"
static inline __attribute__((always_inline)) void sq_stream_put_run_sh4(const nm_sq_stream_vertex_t *record,unsigned n,
        uintptr_t xyq,uintptr_t a,uintptr_t b,uint32_t argb,float depth_scale) {
    _Static_assert(PVR_CMD_VERTEX_EOL==0xf0000000u,"SQ_STREAM_EOL_ASM");
    float screen_x=(640.0f/496.0f)*ENJ_XSCALE,screen_y=480.0f/384.0f;
    const uint32_t vertex=PVR_CMD_VERTEX,zero=0;
    const uintptr_t a32=a+32u,b32=b+32u;
    float x,y,q;
    /* The three factors in registers before the first block: loading a
     * constant or a float from memory may need r0, which the blocks use. */
    asm("" : "+f" (screen_x), "+f" (screen_y), "+f" (depth_scale));
    /* Vertices 0 and 1: the run's first colour. */
    asm volatile(
        SQ_RUN_VERTEX_ASM("0",SQ_RUN_COLOUR_REGISTER,"a","","%[vertex]")
        SQ_RUN_VERTEX_ASM("16",SQ_RUN_COLOUR_REGISTER,"b","","%[vertex]")
        : SQ_RUN_OPERANDS : [record] "r" (record), [argb] "r" (argb), SQ_RUN_INPUTS : SQ_RUN_CLOBBERS);
    record+=2;
    /* Vertices 2 .. n-2 (vertex 2 goes to a): four at a time, then a pair,
     * then one; the last one. */
    const unsigned middle=n-3u;
    unsigned fours=middle>>2;
    if(fours) asm volatile(
        "1:\n\t"
        SQ_RUN_RECORD("0","a") SQ_RUN_RECORD("16","b") SQ_RUN_RECORD("32","a") SQ_RUN_RECORD("48","b")
        "dt %[fours]\n\t"
        "bf/s 1b\n\t"
        "add #64,%[record]\n\t"
        : SQ_RUN_OPERANDS, [record] "+r" (record), [fours] "+r" (fours) : SQ_RUN_INPUTS : "t", SQ_RUN_CLOBBERS);
    if(middle&2u) {
        asm volatile(SQ_RUN_RECORD("0","a") SQ_RUN_RECORD("16","b") : SQ_RUN_OPERANDS : [record] "r" (record), SQ_RUN_INPUTS : SQ_RUN_CLOBBERS);
        record+=2;
    }
    if(middle&1u) asm volatile(SQ_RUN_RECORD("0","a") SQ_RUN_LAST("16","b") : SQ_RUN_OPERANDS : [record] "r" (record), SQ_RUN_INPUTS : SQ_RUN_CLOBBERS);
    else asm volatile(SQ_RUN_LAST("0","a") : SQ_RUN_OPERANDS : [record] "r" (record), SQ_RUN_INPUTS : SQ_RUN_CLOBBERS);
}
/* Before the first frame: sq_stream_put_run_sh4 against sq_write_stream_c
 * (colour and flags of the C form in sq_stream_run). Each instance of the
 * vertex code is compared on its own: the blocks above, with separate buffers
 * instead of the two alternating store-queue addresses, so that no vertex is
 * overwritten. The control flow (fours, pair, odd vertex, last one, the
 * records' advance) through the two vertices a whole run leaves in a and b,
 * for every run of 4 to 16 vertices. */
static __attribute__((noinline)) int sq_stream_run_check(void) {
    alignas(32) nm_cached_vertex_t points[16];
    alignas(32) nm_sq_stream_vertex_t records[16];
    alignas(32) pvr_vertex_t written[4],expected[16];
    nm_memset(points,0,sizeof(points)); nm_memset(records,0,sizeof(records));
    for(unsigned k=0;k<16;++k) {
        points[k].x=12.5f+(float)k*101.25f; points[k].y=-3.0f+(float)k*77.5f; points[k].q=0.01f+(float)k*0.37f;
        records[k].corner=sq_corner_of(15u-k); records[k].argb=0xff100000u+k*0x010203u;
        records[k].uv[0]=(float)k*0.125f+0.0625f; records[k].uv[1]=1.0f-(float)k*0.25f;
    }
    /* Corner words are index*8-32768 (sq_corner_of), taken from the points' middle (sq_points_mid), as in sq_stream_run. */
    const uintptr_t mid=sq_points_mid(points)+offsetof(nm_cached_vertex_t,x);
    float depth_scale=0.5f*(1.0f+3.0f/1024.0f),screen_x=(640.0f/496.0f)*ENJ_XSCALE,screen_y=480.0f/384.0f;
    const uint32_t argb=0xff123456u,vertex=PVR_CMD_VERTEX,zero=0;
    asm("" : "+f" (screen_x), "+f" (screen_y), "+f" (depth_scale)); /* see sq_stream_put_run_sh4 */
    for(unsigned n=4;n<=16;++n) {
        for(unsigned k=0;k<n;++k)
            sq_write_stream_c(expected+k,(const float *)(mid+(uintptr_t)(intptr_t)(records[k].corner*4)),k+1==n ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                records[k].uv,depth_scale,k<2 ? argb : records[k].argb);
        nm_memset(written,0xa5,sizeof(written));
        sq_stream_put_run_sh4(records,n,mid,(uintptr_t)(written+0),(uintptr_t)(written+1),argb,depth_scale);
        if(memcmp(written+(n&1),expected+n-2,sizeof(pvr_vertex_t)) || memcmp(written+!(n&1),expected+n-1,sizeof(pvr_vertex_t))) {
            printf("FAIL: SQ stream run writer differs from sq_write_stream_c (run of %u vertices)\n",n);
            return 0;
        }
    }
    /* Every block on its own: the first two (register colour), the four of the loop, the pair, the odd one and the
     * last one after it, the last one alone. */
    float x,y,q;
    static const uint8_t vertices_of[5]={2,4,2,2,1};
    for(unsigned form=0;form<5;++form) {
        const nm_sq_stream_vertex_t *record=records+3u*form;
        const uintptr_t xyq=mid;
        uintptr_t a32=(uintptr_t)(written+0)+32u,b32=(uintptr_t)(written+1)+32u;
        const uintptr_t c32=(uintptr_t)(written+2)+32u,d32=(uintptr_t)(written+3)+32u;
        nm_memset(written,0xa5,sizeof(written));
        if(form==0) asm volatile(
            SQ_RUN_VERTEX_ASM("0",SQ_RUN_COLOUR_REGISTER,"a","","%[vertex]")
            SQ_RUN_VERTEX_ASM("16",SQ_RUN_COLOUR_REGISTER,"b","","%[vertex]")
            : SQ_RUN_OPERANDS : [record] "r" (record), [argb] "r" (argb), SQ_RUN_INPUTS : SQ_RUN_CLOBBERS);
        else if(form==1) asm volatile(
            SQ_RUN_RECORD("0","a") SQ_RUN_RECORD("16","b") SQ_RUN_RECORD("32","c") SQ_RUN_RECORD("48","d")
            : SQ_RUN_OPERANDS : [record] "r" (record), [c] "r" (c32), [d] "r" (d32), SQ_RUN_INPUTS : SQ_RUN_CLOBBERS);
        else if(form==2) asm volatile(SQ_RUN_RECORD("0","a") SQ_RUN_RECORD("16","b") : SQ_RUN_OPERANDS : [record] "r" (record), SQ_RUN_INPUTS : SQ_RUN_CLOBBERS);
        else if(form==3) asm volatile(SQ_RUN_RECORD("0","a") SQ_RUN_LAST("16","b") : SQ_RUN_OPERANDS : [record] "r" (record), SQ_RUN_INPUTS : SQ_RUN_CLOBBERS);
        else asm volatile(SQ_RUN_LAST("0","a") : SQ_RUN_OPERANDS : [record] "r" (record), SQ_RUN_INPUTS : SQ_RUN_CLOBBERS);
        for(unsigned k=0;k<vertices_of[form];++k) {
            const unsigned at=3u*form+k;
            const int last=(form==3 && k==1) || form==4;
            pvr_vertex_t want;
            sq_write_stream_c(&want,(const float *)(mid+(uintptr_t)(intptr_t)(records[at].corner*4)),last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                records[at].uv,depth_scale,form==0 ? argb : records[at].argb);
            if(memcmp(written+k,&want,sizeof(want))) {
                printf("FAIL: SQ stream run writer, vertex %u of block %u differs from sq_write_stream_c\n",k,form);
                return 0;
            }
        }
    }
    puts("Native SQ stream run writer PASS: runs of 4 to 16 vertices and every block equal sq_write_stream_c, all eight words");
    return 1;
}
#endif
/* A strip run from `face` (drawn, intact, run != 0), by the rule of
 * sq_emit_run: the following faces of its offline strip while each is drawn
 * (same list, walked group, side shown), has this header and is intact.
 * Writes it and returns the faces it took (at least two), or 1 (nothing
 * written), or 0 on failure. */
static __attribute__((noinline)) unsigned sq_stream_run(nm_sq_stream_walk_t *w,const nm_sq_stream_face_t *face,
        unsigned binding,uint32_t argb,const pvr_poly_hdr_t *selected) {
    const nm_sq_batch_t *const batch=w->batch;
    const uintptr_t points=w->points,xyq=points+offsetof(nm_cached_vertex_t,x);
    unsigned run=1,triangles=face->count-2u;
    int tried=0; /* the next face is drawn: the queue path calls sq_emit_run (host statistics) */
    /* Every face of the run shows the colour of its front binding: the strip
     * vertex records hold it (sq_stream_put_run_sh4). */
    int plain=binding==face->front;
    for(const unsigned limit=face->run;run<limit && face+run<w->stop;++run) {
        const nm_sq_stream_face_t *const member=face+run;
        unsigned group,member_binding; uint32_t member_argb; const pvr_poly_hdr_t *member_header;
        const int drawn=sq_stream_decide(w,member,&group,&member_binding,&member_argb,&member_header);
        if(drawn<0) return 0;
        if(!drawn) break;
        tried=1;
        if(member_header!=selected) break;
        plain&=member_binding==member->front;
        if(group!=SQ_CLASS_PROVEN) {
            int inside=sq_point_drawable(sq_corner_point(points,member->corner[0]))&
                sq_point_drawable(sq_corner_point(points,member->corner[1]))&sq_point_drawable(sq_corner_point(points,member->corner[2]));
            if(member->count==4) inside&=sq_point_drawable(sq_corner_point(points,member->corner[3]));
            if(!inside) break;
        }
        triangles+=member->count-2u;
    }
    if(run<2) { if(tried) SQ_PROBE(emit_runs_refused); return 1; }
    const int header_changed=w->previous!=selected;
    if(w->guarded && !sq_stream_fits(w,triangles+2u,triangles,header_changed)) return 0;
    nm_sq_target_t target=w->target;
    if(header_changed) {
        pvr_poly_hdr_t *header=SQ_DR_TARGET(target); *header=*selected; SQ_DR_COMMIT(header);
        w->previous=selected; ++w->headers;
    }
    w->counts+=triangles+2u+0x10000u;
    const nm_sq_stream_vertex_t *corner=w->strips+face->strip;
    const nm_sq_stream_vertex_t *const last_corner=corner+triangles+1u;
    const float depth_scale=face->depth;
#if NM_FLAT_PROVOKING_LAST
    if(plain) {
#ifdef __DREAMCAST__
        /* The first vertex goes where SQ_DR_TARGET puts it next. */
        const uintptr_t first=(uintptr_t)(target^32u),second=(uintptr_t)target;
        sq_stream_put_run_sh4(corner,triangles+2u,xyq,first,second,argb,depth_scale);
        if(triangles&1u) target^=32u;
#else
        /* sq_stream_put_run_sh4's words: colours from the records. */
        const nm_sq_stream_face_t *member=face;
        unsigned completed=0; /* triangles of the faces before `member` */
        for(unsigned k=0;k<triangles+2u;++k,++corner) {
            if(k>=2u && k-2u>=completed+member->count-2u) completed+=(member++)->count-2u;
            SQ_VERTEX_TAG(batch->mesh->face_first+member->index,member->front);
            SQ_STREAM_PUT(corner->corner,corner->uv,corner==last_corner ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,depth_scale,
                k<2u ? argb : corner->argb);
        }
#endif
    } else
#endif
    for(unsigned j=0;j<run;++j) {
        const nm_sq_stream_face_t *const member=face+j;
        uint32_t member_argb=argb;
        unsigned member_binding=binding;
        if(j) {
            /* Drawn, as found above: only a two-luma face can show its rear. */
            member_argb=member->argb; member_binding=member->front;
            if(member->flags&SQ_STREAM_TWO) {
                unsigned group; const pvr_poly_hdr_t *member_header;
                if(sq_stream_decide(w,member,&group,&member_binding,&member_argb,&member_header)<=0) return 0;
            }
        }
        /* The colour of a face's triangles goes on their provoking vertices
         * (NM_FLAT_PROVOKING_LAST); the strip's first two vertices go with
         * the first face. */
        unsigned own=member->count-2u;
        if(j==(NM_FLAT_PROVOKING_LAST ? 0u : run-1)) own+=2;
        (void)member_binding;
        do {
            SQ_VERTEX_TAG(batch->mesh->face_first+member->index,member_binding);
            SQ_STREAM_PUT(corner->corner,corner->uv,corner==last_corner ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,depth_scale,member_argb);
            ++corner;
        } while(--own);
    }
    w->target=target;
    SQ_STREAM_COUNT(SQ_STREAM_RUNS,1); SQ_STREAM_COUNT(SQ_STREAM_RUN_FACES,run); SQ_STREAM_COUNT(SQ_STREAM_RUN_VERTICES,triangles+2u);
    SQ_COUNT(sq_flat_runs[1],1); SQ_COUNT(sq_flat_run_faces[1],run); SQ_COUNT(sq_flat_run_vertices[1],triangles+2u);
    SQ_PROBE(emit_runs);
    SQ_COUNT(sq_joined_strips,1); SQ_COUNT(sq_strip_vertices_saved,2*(run-1));
    (void)batch;
    return run;
}
/* One drawn face the hot loop does not write itself: a face of a group not
 * proven (corner test; a face that is not intact is dropped, every triangle
 * beyond one plane, or clipped by sq_emit_face_general as in
 * sq_emit_face_loop), a face a strip run may start with, and every face in
 * guarded mode. Returns the faces it took, 0 on failure. */
static __attribute__((noinline)) unsigned sq_stream_face(nm_sq_stream_walk_t *w,const nm_sq_stream_face_t *face,
        unsigned group,unsigned binding,uint32_t argb,const pvr_poly_hdr_t *selected) {
    const nm_sq_batch_t *const batch=w->batch;
    const uintptr_t points=w->points,xyq=points+offsetof(nm_cached_vertex_t,x);
    const unsigned corners=face->count;
    if(group!=SQ_CLASS_PROVEN) {
        /* The corners' status words, as sq_emit_face_loop tests them. */
        const nm_cached_vertex_t *const e0=sq_corner_point(points,face->corner[0]),*const e1=sq_corner_point(points,face->corner[1]);
        const nm_cached_vertex_t *const e2=sq_corner_point(points,face->corner[2]),*const e3=sq_corner_point(points,face->corner[3]);
        int intact=sq_point_drawable(e0)&sq_point_drawable(e1)&sq_point_drawable(e2);
        if(corners==4) intact&=sq_point_drawable(e3);
        if(!intact) {
            /* Fan order (0,1,2),(0,2,3) from emission order 1,2,0,3. */
            const nm_cached_vertex_t *const p0=corners==4 ? e2 : e0,*const p1=corners==4 ? e0 : e1,*const p2=corners==4 ? e1 : e2;
            const unsigned o0=nm_cached_outside(p0),o1=nm_cached_outside(p1),o2=nm_cached_outside(p2);
            const unsigned o3=corners==4 ? nm_cached_outside(e3) : o2;
            if((((o0|o1|o2)&32u) || (o0&o1&o2)) && (((o0|o2|o3)&32u) || (o0&o2&o3))) {
                SQ_PROBE(emit_outside_faces); SQ_STREAM_COUNT(SQ_STREAM_OUTSIDE,1); return 1;
            }
            SQ_STREAM_COUNT(SQ_STREAM_CLIPPED,1);
            sq_stream_commit(w);
            nm_sq_emit_t clipped;
            sq_emit_expand(&clipped,sq_emit_records[1]+binding);
            NM_FT_BEGIN(ft_general);
            if(!sq_emit_face_general(w->cutout ? PVR_LIST_PT_POLY : PVR_LIST_OP_POLY,1,batch,face->index,binding,&clipped,
                                     face->depth,clipped.header,w->previous_header,0)) return 0;
            NM_FT_END(NM_FT_GENERAL,ft_general);
            w->previous=*w->previous_header; w->headers=draw_header_count; w->target=SQ_DR_BEGIN();
            /* It may have written more than four vertices and a header: the
             * rest of the list is proven again. */
            w->guarded=!sq_faces_fit((unsigned)(w->span_end-face)-1u);
            if(!w->guarded) SQ_PROBE(emit_reproven);
            return 1;
        }
    }
    if(face->run) {
        const unsigned run=sq_stream_run(w,face,binding,argb,selected);
        if(run!=1) return run;
    }
    const int header_changed=w->previous!=selected;
    if(w->guarded && !sq_stream_fits(w,corners,corners-2u,header_changed)) return 0;
    nm_sq_target_t target=w->target;
    if(header_changed) {
        pvr_poly_hdr_t *header=SQ_DR_TARGET(target); *header=*selected; SQ_DR_COMMIT(header);
        w->previous=selected; ++w->headers;
    }
    w->counts+=corners+0x10000u;
    SQ_PROBE(emit_intact_faces); SQ_STREAM_COUNT(SQ_STREAM_HELPER_SINGLE,1);
    SQ_VERTEX_TAG(batch->mesh->face_first+face->index,binding);
    const float depth_scale=face->depth;
    SQ_STREAM_PUT(face->corner[0],face->uv[0],PVR_CMD_VERTEX,depth_scale,argb);
    SQ_STREAM_PUT(face->corner[1],face->uv[1],PVR_CMD_VERTEX,depth_scale,argb);
    if(corners==4) {
        SQ_STREAM_PUT(face->corner[2],face->uv[2],PVR_CMD_VERTEX,depth_scale,argb);
        SQ_STREAM_PUT(face->corner[3],face->uv[3],PVR_CMD_VERTEX_EOL,depth_scale,argb);
    } else SQ_STREAM_PUT(face->corner[2],face->uv[2],PVR_CMD_VERTEX_EOL,depth_scale,argb);
    w->target=target;
    (void)batch;
    return 1;
}
/* Every face of the stretch [face, w->stop) through sq_stream_face (guarded
 * mode). 1, or 0 on failure. */
static __attribute__((noinline)) int sq_stream_stretch_guarded(nm_sq_stream_walk_t *w,const nm_sq_stream_face_t *face) {
    while(face<w->stop) {
        unsigned group,binding; uint32_t argb; const pvr_poly_hdr_t *selected;
        const int drawn=sq_stream_decide(w,face,&group,&binding,&argb,&selected);
        if(drawn<0) return 0;
        if(!drawn) { ++face; continue; }
        const unsigned used=sq_stream_face(w,face,group,binding,argb,selected);
        if(!used) return 0;
        face+=used;
    }
    return 1;
}
/* The hot loop over one stretch of faces [face, w->stop): a drawn face of a
 * proven group that starts no strip run is written here, every other drawn
 * face by sq_stream_face. 1, or 0 on failure. */
static inline __attribute__((always_inline)) int sq_stream_stretch(nm_sq_stream_walk_t *const w,const nm_sq_stream_face_t *face,
        const int cutout) {
    const nm_sq_batch_t *const batch=w->batch;
    const nm_sq_stream_face_t *const stop=w->stop;
    const uintptr_t xyq=w->points+offsetof(nm_cached_vertex_t,x);
    const pvr_poly_hdr_t *previous=w->previous;
    unsigned counts=w->counts;
    nm_sq_target_t target=w->target;
    for(;face!=stop;++face) {
        SQ_POINT_PREFETCH(face+2); SQ_POINT_PREFETCH(face[2].uv);
        unsigned group,binding; uint32_t argb; const pvr_poly_hdr_t *selected;
        const int drawn=sq_stream_decide(w,face,&group,&binding,&argb,&selected);
        if(drawn<=0) { if(drawn<0) return 0; continue; }
        if(group!=SQ_CLASS_PROVEN || face->run) {
            SQ_STREAM_COUNT(SQ_STREAM_HELPER_FACES,1);
            w->previous=previous; w->counts=counts; w->target=target;
            const unsigned used=sq_stream_face(w,face,group,binding,argb,selected);
            if(!used) return 0;
            previous=w->previous; counts=w->counts; target=w->target;
            face+=used-1u;
            if(w->guarded) return sq_stream_stretch_guarded(w,face+1);
            continue;
        }
        /* A single face of a proven group: one strip of its corners. */
        if(previous!=selected) {
            pvr_poly_hdr_t *header=SQ_DR_TARGET(target); *header=*selected; SQ_DR_COMMIT(header);
            previous=selected; ++w->headers; SQ_STREAM_COUNT(SQ_STREAM_HOT_HEADERS,1);
        }
        const unsigned corners=face->count;
        counts+=corners+0x10000u;
        SQ_STREAM_COUNT(corners==4 ? SQ_STREAM_HOT_QUAD : SQ_STREAM_HOT_TRIANGLE,1);
        SQ_PROBE(emit_intact_faces);
        SQ_VERTEX_TAG(batch->mesh->face_first+face->index,binding);
        const float depth_scale=face->depth;
#ifdef __DREAMCAST__
        {
            /* The four stores of SQ_DR_TARGET: target^32, target, target^32, target. */
            const uintptr_t first=(uintptr_t)(target^32u),second=(uintptr_t)target;
            sq_stream_put_face_sh4(face,xyq,first,second,first,second,argb,depth_scale);
            if(corners!=4) target^=32u;
        }
#else
        SQ_STREAM_PUT(face->corner[0],face->uv[0],PVR_CMD_VERTEX,depth_scale,argb);
        SQ_STREAM_PUT(face->corner[1],face->uv[1],PVR_CMD_VERTEX,depth_scale,argb);
        if(corners==4) {
            SQ_STREAM_PUT(face->corner[2],face->uv[2],PVR_CMD_VERTEX,depth_scale,argb);
            SQ_STREAM_PUT(face->corner[3],face->uv[3],PVR_CMD_VERTEX_EOL,depth_scale,argb);
        } else SQ_STREAM_PUT(face->corner[2],face->uv[2],PVR_CMD_VERTEX_EOL,depth_scale,argb);
#endif
    }
    w->previous=previous; w->counts=counts; w->target=target;
    (void)batch; (void)cutout;
    return 1;
}
#if SQ_SWEEPS
/* A catch-fence panel across a sweep plane (code from sq_sweep_select) on the
 * stream: each level n0..n1 with a face of this list, side and binding
 * decided as for any stream face, written clipped to its band by
 * sq_sweep_face, in face order. The stream's counts are committed before and
 * the rest of the list is proven again after, as after a clipped face
 * (sq_stream_face). 1, or 0 on failure. */
static __attribute__((noinline)) int sq_stream_sweep(nm_sq_stream_walk_t *w,const nm_sq_lod_t *lod,unsigned code) {
    const nm_sq_batch_t *const batch=w->batch;
    const nm_mesh_t *const mesh=batch->mesh;
    const float focus=world_draw.pack->instances[batch->instance_index].focus[1];
    const uint16_t *const rank=w->rank;
    const unsigned records=(unsigned)(w->span_end-w->faces);
    const unsigned near_level=SQ_SWEEP_FIRST(code),far_level=SQ_SWEEP_LAST(code);
    unsigned f=lod->first;
    for(unsigned level=0;level<=far_level;f+=lod->count[level],++level) {
        if(level<near_level || !lod->count[level]) continue;
        const unsigned at=w->cutout ? rank[f] : f-rank[f];
        if(at>=records || w->faces[at].index!=f) continue; /* on the other list */
        const nm_sq_stream_face_t *const face=w->faces+at;
        unsigned group,binding; uint32_t argb; const pvr_poly_hdr_t *selected;
        const int drawn=sq_stream_decide(w,face,&group,&binding,&argb,&selected);
        if(drawn<0) return 0;
        if(!drawn) continue;
        SQ_STREAM_COUNT(SQ_STREAM_CLIPPED,1);
        sq_stream_commit(w);
        nm_sq_emit_t emit;
        sq_emit_expand(&emit,sq_emit_records[1]+binding);
        const float near=level>near_level ? sq_sweep_depth(level-1u,focus) : -1.0f;
        const float far=level<far_level ? sq_sweep_depth(level,focus) : -1.0f;
        if(!sq_sweep_face(w->cutout ? PVR_LIST_PT_POLY : PVR_LIST_OP_POLY,1,batch,f,binding,&emit,face->depth,selected,
                          w->previous_header,0,near,far)) return 0;
        w->previous=*w->previous_header; w->headers=draw_header_count; w->target=SQ_DR_BEGIN();
        w->guarded=!sq_faces_fit((unsigned)(w->span_end-face)-1u);
        (void)argb; (void)group;
    }
    return 1;
}
#endif
/* The faces of one stream batch on one list, in face order: the walk of
 * sq_walk_baked (stretches between level-of-detail groups and the range
 * drawn of each walked group, group classes, sides) and the emission of
 * sq_emit_face_loop and sq_emit_run, fused, over the classes and levels of
 * the batch's prepare step. The TA proof covers every face of the list's
 * span at once (sq_faces_fit); a span that cannot be proven, and the rest of
 * one after a clipped face that cannot, are written in guarded mode with the
 * checks of sq_emit_range. 1, or 0 on failure. */
static inline __attribute__((always_inline)) int sq_stream_list(const nm_sq_batch_t *const batch,
        const pvr_poly_hdr_t **const previous_header,const int cutout) {
    const nm_pack_t *const pack=world_draw.pack;
    const nm_mesh_t *const mesh=batch->mesh;
    const unsigned mesh_index=(unsigned)(mesh-pack->meshes);
    const nm_sq_stream_mesh_t *const entry=sq_stream_table+mesh_index;
    const nm_sq_stream_span_t span=entry->span[cutout];
    if(!span.count) return 1;
    const nm_sq_stream_state_t *const state=sq_stream_states+(batch-sq_sources[1].batches);
    const uint8_t *const levels=state->levels+mesh->padding[1];
    /* Punch-through faces before face index f of the mesh; the list's record
     * of the first face at or after f is base + that (or f minus that). */
    const uint16_t *const rank=entry->rank;
    const unsigned face_count=mesh->face_count,cutouts=entry->span[1].count;
#define SQ_STREAM_RANK(f) (cutout ? ((f)<face_count ? rank[f] : cutouts) : (f)-((f)<face_count ? rank[f] : cutouts))
    nm_sq_stream_walk_t w;
    w.batch=batch; w.previous_header=previous_header;
    w.faces=entry->faces+span.base; w.span_end=w.faces+span.count; w.strips=entry->strips; w.rank=rank;
    w.classes=(const uint8_t *)((uintptr_t)state->levels-mesh->padding[0]);
    w.facing=state->facing ? &state->terms : NULL;
    w.points=sq_points_mid(batch->points);
    w.xmtrx=0; w.guarded=!sq_faces_fit(span.count); w.cutout=cutout;
    w.previous=*previous_header; w.headers=draw_header_count; w.counts=0; w.target=SQ_DR_BEGIN();
    SQ_STREAM_COUNT(SQ_STREAM_LISTS,1);
    DRAW_PHASE_START(emit_started);
    const nm_sq_lod_t *lod=NULL,*lod_end=NULL;
    if(sq_lod_groups && mesh->padding[4]>>16) { lod=sq_lod_groups+(mesh->padding[4]&0xffffu); lod_end=lod+(mesh->padding[4]>>16); }
    const nm_sq_lod_t *const lod_base=lod;
    while(lod!=lod_end && lod->end<=span.first) ++lod;
    unsigned f=span.first;
    while(f<span.end) {
        /* The next stretch [f, stop): the faces before the next group, or the
         * range drawn of a group (none when its bounds are outside). */
        unsigned stop,next;
        if(lod!=lod_end && lod->first<=f) {
            next=lod->end;
            if(w.classes[mesh->padding[0]+lod->bounds]) {
                const unsigned level=levels[lod-lod_base];
#if SQ_SWEEPS
                if(level>=SQ_SWEEP) {
                    if(!sq_stream_sweep(&w,lod,level)) { sq_stream_commit(&w); return 0; }
                    ++lod; f=next;
                    continue;
                }
#endif
                unsigned first=lod->first+(level ? lod->count[0] : 0u)+(level>1 ? lod->count[1] : 0u);
                stop=first+lod->count[level];
                if(first<f) first=f;
                f=first;
            } else stop=f;
            ++lod;
        } else next=stop=lod!=lod_end ? lod->first : span.end;
        if(stop>span.end) stop=span.end;
        if(f<stop) {
            /* The list's records of the stretch. */
            const unsigned first=SQ_STREAM_RANK(f),end=SQ_STREAM_RANK(stop);
            if(first<end) {
                SQ_STREAM_COUNT(SQ_STREAM_STRETCHES,1);
                w.stop=w.faces+end;
                if(!(w.guarded ? sq_stream_stretch_guarded(&w,w.faces+first) : sq_stream_stretch(&w,w.faces+first,cutout))) {
                    sq_stream_commit(&w); return 0;
                }
            }
        }
        f=next;
    }
#undef SQ_STREAM_RANK
    sq_stream_commit(&w);
    DRAW_PHASE_END(5,emit_started);
    return 1;
}
static __attribute__((noinline)) int sq_stream_emit_op(const nm_sq_batch_t *batch,const pvr_poly_hdr_t **previous) {
    return sq_stream_list(batch,previous,0);
}
static __attribute__((noinline)) int sq_stream_emit_pt(const nm_sq_batch_t *batch,const pvr_poly_hdr_t **previous) {
    return sq_stream_list(batch,previous,1);
}

#if SQ_CAR_STREAMS
/* ---- Prebuilt car streams, part 2: the emitter and the tables -----------
 * (part 1, with the prepare step, is above sq_prepare_batch.)
 * A light class entry of the list callback being written, per class and side
 * (index 2*class+rear): the binding the class's light selects and what the
 * emitter takes from it, colour and emission class (header, UV mapping,
 * depth). stamp: the callback that resolved it. A class is (supplied normal,
 * material) of one mesh, and the material decides the list, so the two
 * callbacks of a part never share an entry; a callback takes a new stamp
 * (when the 16-bit stamp wraps every entry is cleared; 0 is never issued). */
typedef struct { uint16_t stamp, binding; uint32_t argb; const nm_sq_class_t *group; uint32_t unused; } nm_sq_car_entry_t;
#ifdef __DREAMCAST__
_Static_assert(sizeof(nm_sq_car_entry_t)==16,"two entries (front, rear) per class in a cache line");
#endif
static nm_sq_car_entry_t *sq_car_entries;
static unsigned sq_car_entry_count,sq_car_stamp;
/* The colours (and, for host tools, bindings) of a strip run's faces. */
alignas(32) static uint32_t sq_car_run_argb[SQ_CAR_RUN_MAX];
alignas(32) static uint16_t sq_car_run_binding[SQ_CAR_RUN_MAX];
/* Host tools may write every list in guarded mode (the simulator's SIM_CAR_GUARDED). */
#ifndef SQ_CAR_GUARDED
#define SQ_CAR_GUARDED 0
#endif
/* The drawn faces of the list being written (sq_car_decide), and one more. */
alignas(32) static uint32_t sq_car_drawn[SQ_CAR_RUN_MAX+1u];
/* The emitter's state, as nm_sq_stream_walk_t: constants of the batch and
 * list, and the running state the write pass (sq_car_write) keeps in locals
 * and stores here before it calls a helper. counts: vertices in the low
 * half, strips in the high half since the last commit. */
typedef struct {
    const nm_sq_batch_t *batch;
    const pvr_poly_hdr_t **previous_header;
    const nm_sq_lit_face_t *lit;     /* the mesh's hot face records (plane constants, normal, class, material, flags) */
    const nm_sq_face_t *faces;       /* the mesh's emission records (corners, u, v, layer, strip run) */
    const nm_sq_facing_t *facing;
    nm_sq_car_entry_t *entries;
    uintptr_t points;
    unsigned stamp, mode;            /* the callback's stamp, the mesh's light mode (0 or 1) */
    const nm_instance_t *inst;       /* what the light needs, once per callback */
    const nm_material_t *materials;
    const uint16_t *lookup;          /* material and luma -> binding (draw_bindings, or the runs: sq_lookup) */
    const uint32_t *runs;
    const nm_sq_bind_t *records;     /* binding -> colour and emission class */
    int guarded, cutout;
    const pvr_poly_hdr_t *previous;
    unsigned headers, counts;
    nm_sq_target_t target;
#if NM_SH4ZAM_GFX
    nm_sq_light_terms_t light_terms; /* the instance's light in model space, once per callback (sq_class_light) */
#endif
} nm_sq_car_walk_t;
static void sq_car_commit(nm_sq_car_walk_t *w) {
    const unsigned vertices=w->counts&0xffffu,triangles=vertices-2u*(w->counts>>16);
    sq_ta_vertices+=vertices; draw_count+=triangles; draw_header_count=w->headers;
    SQ_COUNT(draw_opaque,w->cutout ? 0 : triangles); SQ_COUNT(draw_cutout,w->cutout ? triangles : 0); SQ_CLIP_PATH_COUNT(triangles);
    w->counts=0; *w->previous_header=w->previous; SQ_DR_END(w->target);
}
/* Guarded mode only (the list could not be proven to fit): sq_stream_fits for the cars. */
static int sq_car_fits(const nm_sq_car_walk_t *w,unsigned v,unsigned t,int header) {
    const unsigned vertices=w->counts&0xffffu,triangles=vertices-2u*(w->counts>>16);
    if(draw_count+triangles+t<=NM_DRAW_TRIANGLES && draw_ta_fits((sq_ta_vertices+vertices+v+2u)/3u,w->headers+(unsigned)header)) return 1;
    printf("FAIL SQ TA budget: triangles=%u vertices=%u headers=%u next-header=%u source=0\n",
        draw_count+triangles,sq_ta_vertices+vertices,w->headers,(unsigned)header);
    return 0;
}
/* The binding face f shows, by sq_walk_car's rules, when the decide pass
 * meets a class entry this callback has not resolved yet, or for a face lit
 * on its own (sq_car_entry_of): side +1 front, -1 rear (decided from the plane constants),
 * 0 undecided. A decided side evaluates the light of the face's class and
 * side (sq_sided_light_values) and keeps the result in the class entry; a
 * light it sends back (-2: a light dot that could underflow) and an
 * undecided side are lit by face_light for this face alone (`own`), as
 * sq_walk_car does, which also decides the side of an undecided face (its
 * rejections: reject_draw_face). Returns the entry to draw the face with,
 * NULL when the face is not drawn; *failed on invalid arithmetic or a light
 * without a binding (the queue path's failures). The meshes on this path
 * have no face that reject_draw_face refuses for its own attribute or
 * material (sq_car_stream_prepare), so a decided side is drawn. */
static inline __attribute__((always_inline)) const nm_sq_car_entry_t *sq_car_resolve_body(nm_sq_car_walk_t *w,unsigned f,int side,
        nm_sq_car_entry_t *own,int *failed) {
    const nm_sq_lit_face_t *const face=w->lit+f;
    const nm_instance_t *const inst=w->inst; /* XMTRX holds its matrix (sq_car_list) */
    nm_sq_car_entry_t *entry=NULL;
    int light=-2;
    unsigned selected=UINT16_MAX;
    if(side) {
        SQ_LIGHT_PROBE(inst,draw_pack->faces+w->batch->mesh->face_first+f,side<0);
        SQ_CAR_COUNT(SQ_CAR_MISSES,1);
        /* sq_walk_car's evaluation; one instance per mode (a car mesh on this
         * path has mode 0 or 1: sq_facing_batch), so that the computed-normal
         * branch of the evaluation is not part of the code. */
        const nm_material_t *const material=w->materials+face->material;
#if NM_SH4ZAM_GFX
        light=w->mode ? sq_class_light(1u,face->normal,material,&w->light_terms,side<0) :
            sq_class_light(0u,face->normal,material,&w->light_terms,side<0);
#else
        light=w->mode ? sq_sided_light_values(1u,face->normal,material,inst,side<0) :
            sq_sided_light_values(0u,face->normal,material,inst,side<0);
#endif
        SQ_CLASS_LIGHT_PROBE(w->mode,face->normal,material,inst,side<0,light);
        if(light>=0) { entry=w->entries+2u*face->light_class+(unsigned)(side<0); selected=sq_lookup(w->lookup,w->runs,face->material,(unsigned)light&255u); }
    } else SQ_CAR_COUNT(SQ_CAR_REFERENCE,1);
    if(light==-2) {
        SQ_CAR_COUNT(SQ_CAR_OWN,1);
        const nm_mesh_t *const mesh=w->batch->mesh;
        light=face_light(draw_pack,mesh,inst,draw_pack->faces+mesh->face_first+f,1);
        if(light>=0) selected=sq_lookup(w->lookup,w->runs,face->material,(unsigned)light&255u);
    }
    if(light<0) { *failed=1; return NULL; }
    /* reject_draw_face on the record's flags (an undecided side only: a
     * decided rear of a one-sided face never gets here). */
    const unsigned flags=face->flags;
    if((!(flags&SQ_LIT_TWO_SIDED) && (light&256)) || !(flags&SQ_LIT_DRAWN) || (flags&SQ_LIT_CHECKER)) {
        SQ_COUNT(draw_rear,!(flags&SQ_LIT_TWO_SIDED) && (light&256)); return NULL;
    }
    if(selected==UINT16_MAX) { *failed=1; return NULL; }
    nm_sq_car_entry_t *const out=entry ? entry : own;
    const nm_sq_bind_t *const record=w->records+selected;
    out->binding=(uint16_t)selected; out->argb=record->argb; out->group=record->group;
    if(entry) entry->stamp=(uint16_t)w->stamp;
    return out;
}
static __attribute__((noinline)) const nm_sq_car_entry_t *sq_car_resolve(nm_sq_car_walk_t *w,unsigned f,int side,
        nm_sq_car_entry_t *own,int *failed) {
    return sq_car_resolve_body(w,f,side,own,failed);
}
/* The vertices of one face from its emission record and the entry of the
 * side it shows: sq_write_corner's words in sq_emit_face_loop's order (fan
 * (0,1,2),(0,2,3) as strip (1,2,0,3)), the last one ending the strip. `a`
 * and `b` are the store-queue addresses the vertices alternate between
 * (first, second, first, second); the check before the first frame
 * (sq_car_face_check) passes four separate buffers. Per vertex: the corner's
 * cached x, y, q, the record's integer u, v converted (FLOAT through FPUL),
 * less the origin, times the scale (FSUB, FMUL: sq_write_corner_sh4's
 * operations), 31 instructions; 3 more for the last one's flag word. u and v
 * are below 32768 (sq_car_stream_prepare): the sign-extending 16-bit load
 * gives the vertex table's value. */
#ifdef __DREAMCAST__
#define SQ_CAR_VERTEX_ASM(corner,u,v,address,flags_setup,flags) \
    "mov.w @(" corner ",%[face]),r0\n\t" \
    "shll2 r0\n\t" \
    "add %[xyq],r0\n\t" \
    "fmov.s @r0+,%[x]\n\t" \
    "fmov.s @r0+,%[y]\n\t" \
    "fmov.s @r0,%[q]\n\t" \
    "mov.w @(" u ",%[face]),r0\n\t" \
    "lds r0,fpul\n\t" \
    "mov.w @(" v ",%[face]),r0\n\t" \
    "float fpul,%[u]\n\t" \
    "lds r0,fpul\n\t" \
    "float fpul,%[v]\n\t" \
    "mov %[" address "],r1\n\t" \
    "add #32,r1\n\t" \
    "mov #0,r0\n\t" \
    "mov.l r0,@-r1\n\t" \
    "fsub %[ov],%[v]\n\t" \
    "mov.l %[argb],@-r1\n\t" \
    "fmul %[sv],%[v]\n\t" \
    "fsub %[ou],%[u]\n\t" \
    "fmov.s %[v],@-r1\n\t" \
    "fmul %[su],%[u]\n\t" \
    "fmul %[ds],%[q]\n\t" \
    "fmov.s %[u],@-r1\n\t" \
    "fmul %[sy],%[y]\n\t" \
    "fmov.s %[q],@-r1\n\t" \
    "fmul %[sx],%[x]\n\t" \
    flags_setup \
    "fmov.s %[y],@-r1\n\t" \
    "fmov.s %[x],@-r1\n\t" \
    "mov.l " flags ",@-r1\n\t" \
    "pref @r1\n\t"
#define SQ_CAR_FACE_OPERANDS [x] "=&f" (x), [y] "=&f" (y), [q] "=&f" (q), [u] "=&f" (u), [v] "=&f" (v)
#define SQ_CAR_FACE_INPUTS [face] "r" (face), [xyq] "r" (xyq), [argb] "r" (argb), [vertex] "r" (vertex), \
    [ou] "f" (origin_u), [ov] "f" (origin_v), [su] "f" (scale_u), [sv] "f" (scale_v), \
    [ds] "f" (depth_scale), [sx] "f" (screen_x), [sy] "f" (screen_y)
static inline __attribute__((always_inline)) void sq_car_put_face_sh4(const nm_sq_face_t *face,uintptr_t xyq,
        uintptr_t a0,uintptr_t a1,uintptr_t a2,uintptr_t a3,uint32_t argb,
        float origin_u,float origin_v,float scale_u,float scale_v,float depth_scale) {
    _Static_assert(offsetof(nm_sq_face_t,corner)==0 && offsetof(nm_sq_face_t,uv)==16,"the displacements below");
    _Static_assert(PVR_CMD_VERTEX_EOL==0xf0000000u,"SQ_STREAM_EOL_ASM");
    float screen_x=(640.0f/496.0f)*ENJ_XSCALE,screen_y=480.0f/384.0f;
    const uint32_t vertex=PVR_CMD_VERTEX;
    float x,y,q,u,v;
    /* The seven factors in registers before the block: loading a constant or
     * a float from memory may need r0, which the block uses (as in
     * sq_stream_put_run_sh4). */
    asm("" : "+f" (screen_x), "+f" (screen_y), "+f" (depth_scale), "+f" (origin_u), "+f" (origin_v), "+f" (scale_u), "+f" (scale_v));
    if(face->count==4) asm volatile(
        SQ_CAR_VERTEX_ASM("2","20","22","a0","","%[vertex]")
        SQ_CAR_VERTEX_ASM("4","24","26","a1","","%[vertex]")
        SQ_CAR_VERTEX_ASM("0","16","18","a2","","%[vertex]")
        SQ_CAR_VERTEX_ASM("6","28","30","a3",SQ_STREAM_EOL_ASM,"r2")
        : SQ_CAR_FACE_OPERANDS
        : SQ_CAR_FACE_INPUTS, [a0] "r" (a0), [a1] "r" (a1), [a2] "r" (a2), [a3] "r" (a3)
        : "r0", "r1", "r2", "fpul", "memory");
    else asm volatile(
        SQ_CAR_VERTEX_ASM("0","16","18","a0","","%[vertex]")
        SQ_CAR_VERTEX_ASM("2","20","22","a1","","%[vertex]")
        SQ_CAR_VERTEX_ASM("4","24","26","a2",SQ_STREAM_EOL_ASM,"r2")
        : SQ_CAR_FACE_OPERANDS
        : SQ_CAR_FACE_INPUTS, [a0] "r" (a0), [a1] "r" (a1), [a2] "r" (a2)
        : "r0", "r1", "r2", "fpul", "memory");
}
/* Before the first frame: sq_car_put_face_sh4 against sq_write_corner_c, every
 * vertex of a triangle and a quad into four buffers, all eight words. */
static __attribute__((noinline)) int sq_car_face_check(void) {
    alignas(32) nm_cached_vertex_t points[4];
    alignas(32) pvr_vertex_t written[4],expected[4];
    alignas(32) nm_sq_face_t face;
    nm_memset(points,0,sizeof(points)); nm_memset(&face,0,sizeof(face));
    static const uint8_t order[2][4]={{0,1,2,2},{1,2,0,3}};
    for(unsigned k=0;k<4;++k) {
        points[k].x=12.5f+(float)k*101.25f; points[k].y=-3.0f+(float)k*77.5f; points[k].q=0.01f+(float)k*0.37f;
        face.corner[3-k]=(int16_t)(k*8u); /* corner words: index*8 (sq_corner_of minus its bias) */
        face.uv[k][0]=(uint16_t)(k*1031u+7u); face.uv[k][1]=(uint16_t)(32767u-k*4093u);
    }
    const uintptr_t xyq=(uintptr_t)points+offsetof(nm_cached_vertex_t,x);
    for(unsigned count=3;count<=4;++count) {
        face.count=(uint8_t)count;
        const float depth=count==3 ? 1.0f+3.0f/1024.0f : 1.0f;
        const float origin_u=count==3 ? 8.0f : 0.0f,origin_v=count==3 ? 16.0f : 0.0f,scale_u=1.0f/256.0f,scale_v=count==3 ? 1.0f/8192.0f : 1.0f/2048.0f;
        const uint32_t argb=0xff123456u+count;
        nm_memset(written,0xa5,sizeof(written));
        sq_car_put_face_sh4(&face,xyq,(uintptr_t)(written+0),(uintptr_t)(written+1),(uintptr_t)(written+2),(uintptr_t)(written+3),argb,
            origin_u,origin_v,scale_u,scale_v,depth);
        for(unsigned k=0;k<count;++k) {
            const unsigned c=order[count==4][k];
            sq_write_corner_c(expected+k,(const nm_cached_vertex_t *)(xyq-offsetof(nm_cached_vertex_t,x)+(uintptr_t)(intptr_t)(face.corner[c]*4)),
                k+1==count ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,face.uv[c][0],face.uv[c][1],origin_u,origin_v,scale_u,scale_v,depth,argb);
        }
        if(memcmp(written,expected,count*sizeof(pvr_vertex_t))) {
            printf("FAIL: SQ car stream face writer differs from sq_write_corner_c (%u vertices)\n",count);
            return 0;
        }
    }
    puts("Native SQ car stream face writer PASS: every vertex of a triangle and a quad equals sq_write_corner_c, all eight words");
    return 1;
}
#endif
/* The decide pass of a list callback (sq_car_list): the faces of [f, end) in
 * face order, each with sq_walk_car's decisions: the side (sq_face_side), a
 * rear of a one-sided face dropped, the light class entry of the side shown
 * resolved on its first use (sq_car_resolve_body; a face lit on its own is
 * resolved again when it is written). The drawn faces go to `drawn` as
 * face<<16 | entry index (SQ_CAR_ALONE: lit on its own), which is then
 * exactly the queue sq_walk_car fills for this list. Returns their number,
 * UINT_MAX on failure. Nothing is written to the stream here: the loop has
 * the registers to itself. (Evaluating the claimed classes in a pass of
 * their own after this one costs more per class than it saves per face.) */
#define SQ_CAR_ALONE 0xffffu
#ifdef __DREAMCAST__
/* sq_face_side of a hot face record: the same FIPR fv4,fv8 on the same registers as shz_dot6f(normal, eye)
 * (fr5, fr4, fr7 = n0, n1, n2, fr6 = 0; fr9, fr8, fr11 = e0, e1, e2, fr10 = 0), the normal loaded through one
 * post-incremented pointer; the rest as sq_face_side. */
static inline __attribute__((always_inline)) int sq_face_side_lit(const nm_sq_lit_face_t *lit,float e0,float e1,float e2,
        float slack) {
    register float n1 asm("fr4"), n0 asm("fr5"), w1 asm("fr6"), n2 asm("fr7");
    register float y2 asm("fr8")=e1, x2 asm("fr9")=e0, w2 asm("fr10")=0.0f, dot asm("fr11")=e2;
    const float *at=lit->normal;
    asm("fmov.s @%[at]+,fr5\n\tfmov.s @%[at]+,fr4\n\tfmov.s @%[at],fr7\n\tfldi0 fr6\n\tfipr fv4,fv8"
        : "+f" (dot), "=&f" (n1), "=&f" (n0), "=&f" (w1), "=&f" (n2), [at] "+r" (at)
        : "f" (y2), "f" (x2), "f" (w2), "m" (*lit));
    const float approximate=lit->plane+dot;
    const float margin=lit->reach*slack;
    return nm_float_positive(approximate-margin) ? 1 : nm_float_negative(approximate+margin) ? -1 : 0;
}
#endif
static __attribute__((noinline)) unsigned sq_car_decide(nm_sq_car_walk_t *w,unsigned f,const unsigned end,uint32_t *drawn) {
    const nm_sq_lit_face_t *const lits=w->lit;
    const nm_sq_facing_t *const facing=w->facing;
    const nm_sq_car_entry_t *const entries=w->entries;
    const unsigned stamp=w->stamp;
#ifdef __DREAMCAST__
    const float e0=facing->eye[0],e1=facing->eye[1],e2=facing->eye[2],slack=facing->slack;
#endif
    unsigned n=0;
    for(;f!=end;++f) {
        SQ_POINT_PREFETCH(lits+f+4);
        const nm_sq_lit_face_t *const lit=lits+f;
#ifdef __DREAMCAST__
        const int side=sq_face_side_lit(lit,e0,e1,e2,slack);
#else
        const int side=sq_face_side(facing,(const nm_sq_face_plane_t *)lit,lit->normal);
#endif
        SQ_CAR_COUNT(SQ_CAR_SIDES,1);
        if(side<0 && !(lit->flags&SQ_LIT_TWO_SIDED)) { SQ_COUNT(draw_rear,1); SQ_CAR_COUNT(SQ_CAR_CULLED,1); continue; }
        unsigned index=SQ_CAR_ALONE;
        if(side) {
            index=2u*lit->light_class+(unsigned)(side<0);
            if(entries[index].stamp!=stamp) {
                /* A class's first face this callback: out of line (a few a part since saturated materials are one
                 * class), so that the loop keeps its values in registers. */
                nm_sq_car_entry_t own;
                int failed=0;
                const nm_sq_car_entry_t *const resolved=sq_car_resolve(w,f,side,&own,&failed);
                if(failed) return UINT_MAX;
                if(!resolved) continue;
                if(resolved==&own) index=SQ_CAR_ALONE;
            }
        } else {
            nm_sq_car_entry_t own;
            int failed=0;
            if(!sq_car_resolve(w,f,0,&own,&failed)) { if(failed) return UINT_MAX; continue; }
        }
        drawn[n++]=(f<<16)|index;
    }
    return n;
}
/* The entry of drawn face `e` (a decide pass entry): its class entry, or one
 * resolved again into `own` (SQ_CAR_ALONE: the same evaluation, the same
 * result). NULL on failure. */
static const nm_sq_car_entry_t *sq_car_entry_of(nm_sq_car_walk_t *w,uint32_t e,nm_sq_car_entry_t *own) {
    if((e&0xffffu)!=SQ_CAR_ALONE) return w->entries+(e&0xffffu);
    const unsigned f=e>>16;
    const nm_sq_lit_face_t *const lit=w->lit+f;
    int failed=0;
    const nm_sq_car_entry_t *const entry=sq_car_resolve(w,f,sq_face_side(w->facing,(const nm_sq_face_plane_t *)lit,lit->normal),own,&failed);
    return failed ? NULL : entry;
}
#define SQ_CAR_PUT(k,flags) do { \
    pvr_vertex_t *const put_out=SQ_DR_TARGET(target); \
    sq_write_corner(put_out,sq_corner_point(points,face->corner[k]),(flags),uvs[k][0],uvs[k][1],origin_u,origin_v,scale_u,scale_v,depth_scale,argb); \
    SQ_DR_COMMIT(put_out); } while(0)
/* A strip run from drawn[i] (`first` its entry), by the rule of sq_emit_run
 * on the list of drawn faces (the queue): the following entries while each
 * is the next face of the offline strip and has this header (a car part on
 * this path is intact), at most to the end of the list. Writes it as
 * sq_emit_run does (sq_strip_corners and the first face's UV mapping and
 * depth; each face's colour on its own vertices; rewritten u, v from the
 * vertex table) and returns the faces it took (at least two), or 1 (nothing
 * written), or 0 on failure. */
static __attribute__((noinline)) unsigned sq_car_run(nm_sq_car_walk_t *w,const uint32_t *drawn,unsigned i,unsigned n,
        const nm_sq_car_entry_t *first) {
    const nm_sq_batch_t *const batch=w->batch;
    const uintptr_t points=w->points;
    const unsigned f=drawn[i]>>16;
    const nm_sq_face_t *const face=w->faces+f;
    const nm_sq_class_t *const uv=first->group;
    const pvr_poly_hdr_t *const selected=uv->header;
    unsigned special=face->special; /* of every member: a rewritten u, v anywhere in the run */
    const unsigned strip_faces=special&SQ_FACE_RUN,left=n-i;
    const unsigned limit=strip_faces<left ? strip_faces : left;
    unsigned run=1,triangles=face->count-2u;
    sq_car_run_argb[0]=first->argb; sq_car_run_binding[0]=first->binding;
    for(;run<limit;++run) {
        const uint32_t next=drawn[i+run];
        if((next>>16)!=f+run) break;
        nm_sq_car_entry_t own;
        const nm_sq_car_entry_t *const entry=sq_car_entry_of(w,next,&own);
        if(!entry) return 0;
        if(entry->group->header!=selected) break;
        sq_car_run_argb[run]=entry->argb; sq_car_run_binding[run]=entry->binding;
        triangles+=face[run].count-2u; special|=face[run].special;
    }
    if(run<2) { SQ_PROBE(emit_runs_refused); return 1; }
    const int header_changed=w->previous!=selected;
    if(w->guarded && !sq_car_fits(w,triangles+2u,triangles,header_changed)) return 0;
    nm_sq_target_t target=w->target;
    if(header_changed) {
        pvr_poly_hdr_t *header=SQ_DR_TARGET(target); *header=*selected; SQ_DR_COMMIT(header);
        w->previous=selected; ++w->headers;
    }
    w->counts+=triangles+2u+0x10000u;
    float depth_scale=uv->depth;
    const unsigned strip_layer=face->layer; /* one layer per strip */
    if(strip_layer) depth_scale*=draw_layer_scale[strip_layer];
    const float origin_u=uv->origin[0],origin_v=uv->origin[1],scale_u=uv->scale[0],scale_v=uv->scale[1];
    const nm_sq_strip_corner_t *corner=sq_strip_corners[0]+face->strip;
    if(!(special&SQ_FACE_DYNAMIC_UV)) {
        /* sq_emit_run's body: one 8-byte entry per vertex; a face supplies the
         * colour of its triangles' provoking vertices, the strip's first two
         * go with the first face (NM_FLAT_PROVOKING_LAST) or the last. */
        const nm_sq_strip_corner_t *const last_corner=corner+triangles+1u;
        for(unsigned j=0;j<run;++j) {
            const uint32_t argb=sq_car_run_argb[j];
            unsigned own=face[j].count-2u;
            if(j==(NM_FLAT_PROVOKING_LAST ? 0u : run-1)) own+=2;
            do {
                SQ_VERTEX_TAG(batch->mesh->face_first+f+j,sq_car_run_binding[j]);
                pvr_vertex_t *const out=SQ_DR_TARGET(target);
                sq_write_corner(out,sq_corner_point(points,corner->corner),corner==last_corner ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                    corner->uv[0],corner->uv[1],origin_u,origin_v,scale_u,scale_v,depth_scale,argb);
                SQ_DR_COMMIT(out);
                ++corner;
            } while(--own);
        }
    } else {
        /* A run with a face whose u, v the scene rewrites (window
         * reflections): every vertex's u, v from the vertex table, as
         * sq_emit_run. (Not through sq_corner: the lean transform writes
         * x, y, q of a position only, so its cached u, v are not the vertex's.) */
        const nm_pack_t *const pack=draw_pack;
        const nm_vertex_t *const vertices=pack->vertices+batch->mesh->vertex_first;
        const uint16_t *const indices=pack->strip_indices+face->strip;
        unsigned at=0;
        for(unsigned j=0;j<run;++j) {
            const uint32_t argb=sq_car_run_argb[j];
            unsigned own=face[j].count-2u;
            if(j==(NM_FLAT_PROVOKING_LAST ? 0u : run-1)) own+=2;
            for(unsigned v=0;v<own;++v,++at,++corner) {
                SQ_VERTEX_TAG(batch->mesh->face_first+f+j,sq_car_run_binding[j]);
                const nm_vertex_t *const vertex=vertices+indices[at];
                pvr_vertex_t *const out=SQ_DR_TARGET(target);
                sq_write_corner(out,sq_corner_point(points,corner->corner),at==triangles+1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                    vertex->u,vertex->v,origin_u,origin_v,scale_u,scale_v,depth_scale,argb);
                SQ_DR_COMMIT(out);
            }
        }
        SQ_PROBE(emit_runs_dynamic); SQ_CAR_COUNT(SQ_CAR_DYNAMIC,run);
    }
    w->target=target;
    SQ_CAR_COUNT(SQ_CAR_RUNS,1); SQ_CAR_COUNT(SQ_CAR_RUN_FACES,run); SQ_CAR_COUNT(SQ_CAR_RUN_VERTICES,triangles+2u);
    SQ_COUNT(sq_flat_runs[0],1); SQ_COUNT(sq_flat_run_faces[0],run); SQ_COUNT(sq_flat_run_vertices[0],triangles+2u);
    SQ_PROBE(emit_runs);
    SQ_COUNT(sq_joined_strips,1); SQ_COUNT(sq_strip_vertices_saved,2*(run-1));
    return run;
}
/* One drawn face the write pass does not write itself: a face lit on its
 * own, a face a strip run may start with (the next drawn face is the next
 * face of its strip, as sq_emit_face_loop asks) or whose u, v the scene
 * rewrites, and every face in guarded mode. Returns the drawn faces it
 * took, 0 on failure. */
static __attribute__((noinline)) unsigned sq_car_face(nm_sq_car_walk_t *w,const uint32_t *drawn,unsigned i,unsigned n) {
    const nm_sq_batch_t *const batch=w->batch;
    const unsigned f=drawn[i]>>16;
    SQ_CAR_COUNT(SQ_CAR_HELPER,1);
    nm_sq_car_entry_t own;
    const nm_sq_car_entry_t *const entry=sq_car_entry_of(w,drawn[i],&own);
    if(!entry) return 0;
    const nm_sq_face_t *const face=w->faces+f;
    if((face->special&SQ_FACE_RUN) && i+1u<n && (drawn[i+1u]>>16)==f+1u) {
        const unsigned run=sq_car_run(w,drawn,i,n,entry);
        if(run!=1) return run;
    }
    const nm_sq_class_t *const uv=entry->group;
    const pvr_poly_hdr_t *const selected=uv->header;
    const uint32_t argb=entry->argb;
    const unsigned corners=face->count;
    const int header_changed=w->previous!=selected;
    if(w->guarded && !sq_car_fits(w,corners,corners-2u,header_changed)) return 0;
    nm_sq_target_t target=w->target;
    if(header_changed) {
        pvr_poly_hdr_t *header=SQ_DR_TARGET(target); *header=*selected; SQ_DR_COMMIT(header);
        w->previous=selected; ++w->headers;
    }
    w->counts+=corners+0x10000u;
    const uint16_t (*uvs)[2]=face->uv;
    uint16_t live_uv[4][2];
    if(face->special&SQ_FACE_DYNAMIC_UV) { sq_live_uv(0,batch,f,live_uv); uvs=live_uv; SQ_CAR_COUNT(SQ_CAR_DYNAMIC,1); }
    float depth_scale=uv->depth;
    const unsigned layer=face->layer;
    if(layer) depth_scale*=draw_layer_scale[layer]; /* the face's layer, as the write pass */
    const float origin_u=uv->origin[0],origin_v=uv->origin[1],scale_u=uv->scale[0],scale_v=uv->scale[1];
    const uintptr_t points=w->points;
    SQ_PROBE(emit_intact_faces); SQ_CAR_COUNT(SQ_CAR_HELPER_SINGLE,1);
    SQ_VERTEX_TAG(batch->mesh->face_first+f,entry->binding);
    if(corners==4) {
        SQ_CAR_PUT(1,PVR_CMD_VERTEX); SQ_CAR_PUT(2,PVR_CMD_VERTEX); SQ_CAR_PUT(0,PVR_CMD_VERTEX); SQ_CAR_PUT(3,PVR_CMD_VERTEX_EOL);
    } else {
        SQ_CAR_PUT(0,PVR_CMD_VERTEX); SQ_CAR_PUT(1,PVR_CMD_VERTEX); SQ_CAR_PUT(2,PVR_CMD_VERTEX_EOL);
    }
    w->target=target;
    return 1;
}
/* Every drawn face through sq_car_face (guarded mode). 1, or 0 on failure. */
static __attribute__((noinline)) int sq_car_write_guarded(nm_sq_car_walk_t *w,const uint32_t *drawn,unsigned n) {
    for(unsigned i=0;i<n;) {
        const unsigned used=sq_car_face(w,drawn,i,n);
        if(!used) return 0;
        i+=used;
    }
    return 1;
}
/* The write pass over the n drawn faces of one list (sq_emit_face_loop on the
 * decide pass's queue): a face whose entry is its class's and that starts no
 * strip run and has no rewritten u, v is written here, every other face by
 * sq_car_face. drawn[n] is readable (the next face's record is requested
 * while one is written). 1, or 0 on failure. */
static inline __attribute__((always_inline)) int sq_car_write(nm_sq_car_walk_t *const w,const uint32_t *const drawn,const unsigned n) {
    const nm_sq_batch_t *const batch=w->batch;
    const nm_sq_face_t *const faces=w->faces;
    const nm_sq_car_entry_t *const entries=w->entries;
    const uintptr_t points=w->points;
    const pvr_poly_hdr_t *previous=w->previous;
    unsigned counts=w->counts;
    nm_sq_target_t target=w->target;
    for(unsigned i=0;i<n;++i) {
        const uint32_t e=drawn[i];
        const unsigned f=e>>16,index=e&0xffffu;
        const nm_sq_face_t *const face=faces+f;
        SQ_POINT_PREFETCH(faces+(drawn[i+1u]>>16));
        if(index==SQ_CAR_ALONE || face->special) {
            w->previous=previous; w->counts=counts; w->target=target;
            const unsigned used=sq_car_face(w,drawn,i,n);
            if(!used) return 0;
            previous=w->previous; counts=w->counts; target=w->target;
            i+=used-1u;
            continue;
        }
        const nm_sq_car_entry_t *const entry=entries+index;
        const nm_sq_class_t *const uv=entry->group;
        const pvr_poly_hdr_t *const selected=uv->header;
        const uint32_t argb=entry->argb;
        if(previous!=selected) {
            pvr_poly_hdr_t *header=SQ_DR_TARGET(target); *header=*selected; SQ_DR_COMMIT(header);
            previous=selected; ++w->headers; SQ_CAR_COUNT(SQ_CAR_HOT_HEADERS,1);
        }
        const unsigned corners=face->count;
        counts+=corners+0x10000u;
        float depth_scale=uv->depth;
        const unsigned layer=face->layer;
        if(layer) depth_scale*=draw_layer_scale[layer]; /* as sq_emit_face_loop (see sq_emit_range) */
        const float origin_u=uv->origin[0],origin_v=uv->origin[1],scale_u=uv->scale[0],scale_v=uv->scale[1];
        SQ_CAR_COUNT(corners==4 ? SQ_CAR_HOT_QUAD : SQ_CAR_HOT_TRIANGLE,1);
        SQ_PROBE(emit_intact_faces);
        SQ_VERTEX_TAG(batch->mesh->face_first+f,entry->binding);
#ifdef __DREAMCAST__
        {
            /* The four stores of SQ_DR_TARGET: target^32, target, target^32, target. */
            const uintptr_t first=(uintptr_t)(target^32u),second=(uintptr_t)target;
            sq_car_put_face_sh4(face,points+offsetof(nm_cached_vertex_t,x),first,second,first,second,argb,
                origin_u,origin_v,scale_u,scale_v,depth_scale);
            if(corners!=4) target^=32u;
        }
#else
        const uint16_t (*const uvs)[2]=face->uv;
        if(corners==4) {
            SQ_CAR_PUT(1,PVR_CMD_VERTEX); SQ_CAR_PUT(2,PVR_CMD_VERTEX); SQ_CAR_PUT(0,PVR_CMD_VERTEX); SQ_CAR_PUT(3,PVR_CMD_VERTEX_EOL);
        } else {
            SQ_CAR_PUT(0,PVR_CMD_VERTEX); SQ_CAR_PUT(1,PVR_CMD_VERTEX); SQ_CAR_PUT(2,PVR_CMD_VERTEX_EOL);
        }
#endif
    }
    w->previous=previous; w->counts=counts; w->target=target;
    (void)batch;
    return 1;
}
/* The faces of one car stream batch on one list, in face order: the decide
 * pass, then the write pass. The TA proof covers the drawn faces at once
 * (sq_faces_fit: the queue path's proof of the same queue); a list that
 * cannot be proven is written in guarded mode with the checks of
 * sq_emit_range. 1, or 0 on failure. */
static inline __attribute__((always_inline)) int sq_car_list(const nm_sq_batch_t *const batch,
        const pvr_poly_hdr_t **const previous_header,const int cutout) {
    const nm_mesh_t *const mesh=batch->mesh;
    const nm_sq_car_mesh_t *const info=sq_car_meshes+(mesh-draw_pack->meshes);
    const unsigned first=cutout ? info->cutouts : 0u,end=cutout ? mesh->face_count : info->cutouts;
    if(first==end) return 1;
    if(++sq_car_stamp>UINT16_MAX) {
        for(unsigned e=0;e<sq_car_entry_count;++e) sq_car_entries[e].stamp=0;
        sq_car_stamp=1;
    }
    nm_sq_car_walk_t w;
    w.batch=batch; w.previous_header=previous_header;
    w.lit=sq_lit_faces+mesh->face_first; w.faces=sq_emit_faces[0]+mesh->face_first;
    w.facing=sq_car_facing+(batch-sq_sources[0].batches);
    w.entries=sq_car_entries; w.points=sq_points_mid(batch->points);
    w.stamp=sq_car_stamp; w.cutout=cutout; w.mode=mesh->mode;
    w.inst=draw_pack->instances+batch->instance_index; w.materials=draw_pack->materials;
    w.lookup=draw_bindings; w.runs=draw_runs; w.records=sq_emit_records[0];
    /* The instance matrix for the light (sq_sided_light_values, face_light). */
    { const nm_instance_t *const inst=w.inst; shz_xmtrx_load_3x4(&inst->transform); }
#if NM_SH4ZAM_GFX
    sq_light_terms(&w.light_terms,w.inst);
#endif
    SQ_CAR_COUNT(SQ_CAR_LISTS,1);
    DRAW_PHASE_START(emit_started);
    uint32_t *const drawn=sq_car_drawn;
    const unsigned n=sq_car_decide(&w,first,end,drawn);
    if(n==UINT_MAX) return 0;
    if(!n) return 1;
    drawn[n]=drawn[n-1u]; /* read ahead by the last face */
    w.guarded=SQ_CAR_GUARDED || !sq_faces_fit(n);
    w.previous=*previous_header; w.headers=draw_header_count; w.counts=0; w.target=SQ_DR_BEGIN();
    const int ok=w.guarded ? sq_car_write_guarded(&w,drawn,n) : sq_car_write(&w,drawn,n);
    sq_car_commit(&w);
    DRAW_PHASE_END(5,emit_started);
    return ok;
}
static __attribute__((noinline)) int sq_car_emit_op(const nm_sq_batch_t *batch,const pvr_poly_hdr_t **previous) {
    return sq_car_list(batch,previous,0);
}
static __attribute__((noinline)) int sq_car_emit_pt(const nm_sq_batch_t *batch,const pvr_poly_hdr_t **previous) {
    return sq_car_list(batch,previous,1);
}
#endif

/* The tables, per world pack after the scenery stage selection, once the
 * emission records (headers, colours, classes), the emission faces, the
 * baked bindings, the plane constants and the level-of-detail groups exist.
 * A mesh joins the path when nothing of it needs the queue path: no rewritten
 * u,v, and every binding a face or the members of an offline strip can show
 * has the same UV mapping and depth class as the face's (strip's first face's)
 * front binding (the u, v and depth written are then fixed: the queue path
 * takes them from the binding of the side shown, a run from its first face).
 * Position lists: per face group the distinct positions of its faces outside
 * every level-of-detail group, per level-of-detail group and level those of
 * that range. */
static unsigned sq_stream_mesh_count,sq_stream_bytes;
/* The record cache, for a course whose world does not take the whole tables
 * (nm_render_world_streams(0); course_assets.h NM_COURSE_WORLD_STREAMS: the
 * records of Expert's 26,487 faces would need 2.5 MB): a pool of
 * NM_SQ_STREAM_POOL bytes holds the records of the meshes the frames walk,
 * built between two frames (sq_stream_idle, from the idle wait for the PVR:
 * code/audio/idle_wait.c) in the time the wait leaves, a whole mesh at a time,
 * the meshes walked longest ago evicted for them. A mesh without its records
 * takes the queue path, which submits the same words; the records are those
 * of the whole tables (sq_stream_job_step, the same code). 0: no cache. */
#ifndef NM_SQ_STREAM_POOL
#define NM_SQ_STREAM_POOL (448u*1024u)
#endif
static uint32_t sq_stream_pool_wanted=NM_SQ_STREAM_POOL; /* host tools may set another size before the attach */
/* A mesh not walked for this many frames may be evicted. */
#define SQ_STREAM_KEEP_FRAMES 8u
/* Per mesh at load: its block (bytes: 0 never on the path), its first strip
 * vertex and the strip vertices and positions it holds. */
typedef struct { uint32_t bytes, strip_first, strips, points; } nm_sq_stream_plan_t;
static nm_sq_stream_plan_t *sq_stream_plans;
static const nm_pack_t *sq_stream_pack;
static uint32_t sq_stream_pool_bytes;      /* record cache: its pool (sq_stream_faces), 0 for the whole tables */
static uint32_t *sq_stream_offset;         /* record cache: per mesh its block's offset in the pool, UINT32_MAX none */
static uint16_t *sq_stream_resident;       /* record cache: the meshes with a block, by offset */
static unsigned sq_stream_resident_count;
static uint8_t *sq_stream_scratch;         /* record cache: the builder's seen and in_lod */
static unsigned sq_stream_largest;
static float sq_stream_face_us;            /* record cache: microseconds a face built (measured) */
static struct { uint32_t built, faces, evicted, refused, frames, busy_frames, missed, missed_faces, max_us, high; uint64_t us; } sq_stream_cache_stats;
static void sq_stream_report(void) {
    if(!sq_stream_pool_bytes) return;
    printf("Native world stream cache: %lu frames, %lu meshes built (%lu faces, %lu us, longest %lu us, %.2f us a face), "
        "%lu evicted, %lu refused (no room), most in use %lu of %lu bytes; walked without records: %lu batches in %lu frames, %lu faces\n",
        (unsigned long)sq_stream_cache_stats.frames,(unsigned long)sq_stream_cache_stats.built,(unsigned long)sq_stream_cache_stats.faces,
        (unsigned long)sq_stream_cache_stats.us,(unsigned long)sq_stream_cache_stats.max_us,(double)sq_stream_face_us,
        (unsigned long)sq_stream_cache_stats.evicted,(unsigned long)sq_stream_cache_stats.refused,
        (unsigned long)sq_stream_cache_stats.high,(unsigned long)sq_stream_pool_bytes,
        (unsigned long)sq_stream_cache_stats.missed,(unsigned long)sq_stream_cache_stats.busy_frames,
        (unsigned long)sq_stream_cache_stats.missed_faces);
}
static void sq_stream_release(void) {
    sq_stream_report();
    free(sq_stream_faces); free(sq_stream_lists); free(sq_stream_meshes); free(sq_stream_table); free(sq_stream_states);
    free(sq_stream_plans); free(sq_stream_seen); free(sq_stream_offset); free(sq_stream_resident); free(sq_stream_scratch);
    sq_stream_faces=NULL; sq_stream_lists=NULL; sq_stream_meshes=NULL; sq_stream_table=NULL; sq_stream_states=NULL;
    sq_stream_plans=NULL; sq_stream_seen=NULL; sq_stream_offset=NULL; sq_stream_resident=NULL; sq_stream_scratch=NULL;
    sq_stream_pack=NULL;
    sq_stream_lod_lists=sq_stream_mesh_count=sq_stream_bytes=sq_stream_pool_bytes=sq_stream_resident_count=sq_stream_largest=0;
    nm_memset(&sq_stream_cache_stats,0,sizeof(sq_stream_cache_stats));
}
static int sq_stream_same_class(const nm_sq_class_t *a,const nm_sq_class_t *b) {
    return a==b || (!memcmp(a->origin,b->origin,sizeof(a->origin)) && !memcmp(a->scale,b->scale,sizeof(a->scale)) &&
        !memcmp(&a->depth,&b->depth,sizeof(a->depth)));
}
/* u, v exactly as the queue path writes them for this class (its writer). */
static void sq_stream_uv(const nm_sq_class_t *mapping,unsigned u,unsigned v,float out[2]) {
    alignas(32) pvr_vertex_t written;
    alignas(32) nm_cached_vertex_t point;
    nm_memset(&point,0,sizeof(point));
    sq_write_corner(&written,&point,PVR_CMD_VERTEX,u,v,mapping->origin[0],mapping->origin[1],mapping->scale[0],mapping->scale[1],1.0f,0);
    out[0]=written.u; out[1]=written.v;
}
/* The distinct positions of faces [first, end) of `mesh` that `take` accepts
 * (group `group`, or every face for group UINT_MAX), appended at *at. */
static unsigned sq_stream_collect(const nm_pack_t *pack,const nm_mesh_t *mesh,unsigned first,unsigned end,unsigned group,
        const uint8_t *in_lod,uint8_t *seen,int16_t *out) {
    const nm_sq_face_t *const emit=sq_emit_faces[1]+mesh->face_first;
    unsigned count=0;
    for(unsigned f=first;f<end;++f) {
        if(group!=UINT_MAX && (in_lod[f] || pack->faces[mesh->face_first+f].group!=group)) continue;
        for(unsigned v=0;v<emit[f].count;++v) {
            const int16_t corner=emit[f].corner[v];
            const unsigned index=(unsigned)((int)corner+32768)>>3;
            if(seen[index]) continue;
            seen[index]=1; out[count++]=corner;
        }
    }
    for(unsigned i=0;i<count;++i) seen[(unsigned)((int)out[i]+32768)>>3]=0;
    return count;
}
static int sq_stream_cutout(const nm_pack_t *pack,const nm_face_t *face) {
    return ((pack->materials[face->material].header[0]>>13)&3)==3;
}
/* The faces of `mesh` in its level-of-detail groups. */
static void sq_stream_in_lod(const nm_mesh_t *mesh,uint8_t *in_lod) {
    nm_memset(in_lod,0,mesh->face_count);
    const unsigned lod_first=mesh->padding[4]&0xffffu,lod_here=sq_lod_groups ? mesh->padding[4]>>16 : 0u;
    for(unsigned i=0;i<lod_here;++i) for(unsigned f=sq_lod_groups[lod_first+i].first;f<sq_lod_groups[lod_first+i].end;++f) in_lod[f]=1;
}
/* At load, per mesh: the spans (sq_stream_table), the range of its strip
 * vertices and the size of its position lists, and so its block: the face
 * records, the strip vertex records, the position lists, the ranks.
 * `points` holds a mesh's corners (scratch). */
static void sq_stream_plan(const nm_pack_t *pack,unsigned m,uint8_t *seen,uint8_t *in_lod,int16_t *points) {
    const nm_mesh_t *const mesh=pack->meshes+m;
    nm_sq_stream_plan_t *const plan=sq_stream_plans+m;
    nm_sq_stream_mesh_t *const entry=sq_stream_table+m;
    nm_memset(plan,0,sizeof(*plan)); nm_memset(entry,0,sizeof(*entry));
    nm_sq_stream_span_t *const spans=entry->span;
    spans[0].first=spans[1].first=UINT16_MAX;
    if(mesh->face_count>UINT16_MAX || (sq_clock_meshes && sq_clock_meshes[m])) return; /* a frame-counter animation: its records would hold one frame */
    for(unsigned f=0;f<mesh->face_count;++f) {
        nm_sq_stream_span_t *const span=spans+sq_stream_cutout(pack,pack->faces+mesh->face_first+f);
        ++span->count;
        if(f<span->first) span->first=(uint16_t)f;
        span->end=(uint16_t)(f+1u);
    }
    spans[0].base=0; spans[1].base=spans[0].count;
    for(unsigned l=0;l<2;++l) if(spans[l].first==UINT16_MAX) spans[l].first=spans[l].end=0;
    if(sq_strip_corners[1] && pack->strips && mesh->padding[3]) {
        uint32_t low=UINT32_MAX,high=0;
        for(unsigned i=0;i<mesh->padding[3];++i) {
            const nm_strip_t *const strip=pack->strips+mesh->padding[2]+i;
            unsigned triangles=0;
            for(unsigned k=0;k<strip->face_count;++k) triangles+=pack->faces[strip->face_first+k].count-2u;
            if(strip->vertex_first<low) low=strip->vertex_first;
            if(strip->vertex_first+triangles+2u>high) high=strip->vertex_first+triangles+2u;
        }
        plan->strip_first=low; plan->strips=high-low;
    }
    sq_stream_in_lod(mesh,in_lod);
    unsigned count=0;
    /* nm_loader_yield: a group a step (each walks the whole mesh). */
    for(unsigned g=0;g<mesh->padding[1];++g) {
        nm_loader_yield(0);
        count+=sq_stream_collect(pack,mesh,0,mesh->face_count,mesh->padding[0]+g,in_lod,seen,points);
    }
    const unsigned lod_first=mesh->padding[4]&0xffffu,lod_here=nm_pack_lod_count(pack) ? mesh->padding[4]>>16 : 0u;
    for(unsigned i=0;i<lod_here;++i) {
        const nm_sq_lod_t *const lod=sq_lod_groups+lod_first+i;
        unsigned first=lod->first;
        nm_loader_yield(0);
        for(unsigned level=0;level<NM_LOD_LEVELS;++level) {
            count+=sq_stream_collect(pack,mesh,first,first+lod->count[level],UINT_MAX,in_lod,seen,points);
            first+=lod->count[level];
        }
    }
    plan->points=count;
    const size_t bytes=mesh->face_count*sizeof(nm_sq_stream_face_t)+plan->strips*sizeof(nm_sq_stream_vertex_t)+
        (count+mesh->face_count)*sizeof(int16_t);
    plan->bytes=mesh->face_count ? (uint32_t)((bytes+31u)&~(size_t)31u) : 0u;
}
/* At load: whether a mesh may join the path, the rules of the records (its
 * plan keeps 0 bytes otherwise): no rewritten u, v; a two-luma face's rear
 * binding, every binding a live face's term can reach and every side of an
 * offline strip's members have the face's (strip's first face's) mapping and
 * depth class. */
static int sq_stream_check(const nm_pack_t *pack,unsigned m) {
    const nm_mesh_t *const mesh=pack->meshes+m;
    const nm_sq_bind_t *const records=sq_emit_records[1];
    unsigned live_at=0,live_end=0;
    if(sq_live_meshes && sq_live_meshes[m].count) { live_at=sq_live_meshes[m].first; live_end=live_at+sq_live_meshes[m].count; }
    for(unsigned f=0;f<mesh->face_count;++f) {
        const unsigned absolute=mesh->face_first+f;
        const nm_baked_face_t baked=sq_baked_faces[absolute];
        const nm_sq_class_t *const front=records[baked.front].group;
        if(baked.back!=baked.front && baked.back!=UINT16_MAX && !sq_stream_same_class(front,records[baked.back].group)) return 0;
        if(sq_emit_faces[1][absolute].special&SQ_FACE_DYNAMIC_UV) return 0;
        if(live_at<live_end && sq_live_faces[live_at].face==absolute) {
            const nm_sq_live_class_t *const k=sq_live_classes+sq_live_faces[live_at++].live_class;
            for(unsigned side=0;side<(k->two ? 2u : 1u);++side)
                for(unsigned luma=k->low[side];luma<=k->high[side];++luma)
                    if(!sq_stream_same_class(front,records[sq_live_rows[k->row+luma]].group)) return 0;
        }
    }
    for(unsigned i=0;sq_stream_plans[m].strips && i<mesh->padding[3];++i) {
        const nm_strip_t *const strip=pack->strips+mesh->padding[2]+i;
        const nm_sq_class_t *const mapping=records[sq_baked_faces[strip->face_first].front].group;
        for(unsigned k=0;k<strip->face_count;++k) {
            const nm_baked_face_t baked=sq_baked_faces[strip->face_first+k];
            if(!sq_stream_same_class(mapping,records[baked.front].group) ||
               (baked.back!=UINT16_MAX && !sq_stream_same_class(mapping,records[baked.back].group))) return 0;
        }
    }
    return 1;
}
/* Building one mesh's records into its block (its plan's bytes, 32-byte
 * aligned), in steps (the record cache builds between frames, a slice at a
 * time): the ranks, the face records, the strip vertex records, then the
 * position lists and their entries of sq_stream_lists; the mesh is then ready
 * for sq_stream_meshes. A live face's record is refreshed from its baked
 * binding when the mesh is done (sq_live_update rewrote those meanwhile).
 * `tested` and `two`: faces with a side test and with two lumas. */
typedef struct {
    unsigned mesh, phase, cursor, live_at, live_end, tested, two, at;
    uint8_t *block;
} nm_sq_stream_job_t;
static void sq_stream_job_start(nm_sq_stream_job_t *job,unsigned m,uint8_t *block) {
    nm_memset(job,0,sizeof(*job));
    job->mesh=m; job->block=block;
    if(sq_live_meshes && sq_live_meshes[m].count) { job->live_at=sq_live_meshes[m].first; job->live_end=job->live_at+sq_live_meshes[m].count; }
}
/* Up to `faces` more faces' worth of work (UINT_MAX: to the end). 1 when the mesh is done. */
static int sq_stream_job_step(const nm_pack_t *pack,nm_sq_stream_job_t *job,uint8_t *seen,uint8_t *in_lod,unsigned faces) {
    const unsigned s=1,m=job->mesh;
    const nm_mesh_t *const mesh=pack->meshes+m;
    const nm_sq_stream_plan_t plan=sq_stream_plans[m];
    nm_sq_stream_mesh_t *const entry=sq_stream_table+m;
    const nm_sq_stream_span_t *const spans=entry->span;
    nm_sq_stream_face_t *const records_out=(nm_sq_stream_face_t *)(void *)job->block;
    nm_sq_stream_vertex_t *const strips=(nm_sq_stream_vertex_t *)(void *)(job->block+mesh->face_count*sizeof(nm_sq_stream_face_t));
    int16_t *const points=(int16_t *)(void *)(strips+plan.strips);
    uint16_t *const rank=(uint16_t *)(void *)(points+plan.points);
    const nm_sq_bind_t *const records=sq_emit_records[s];
    static const uint8_t quad_order[4]={1,2,0,3},triangle_order[4]={0,1,2,2};
    if(job->phase==0) {
        /* Records in list order: the opaque faces, then the punch-through ones. */
        unsigned cutouts=0;
        for(unsigned f=0;f<mesh->face_count;++f) { rank[f]=(uint16_t)cutouts; cutouts+=(unsigned)sq_stream_cutout(pack,pack->faces+mesh->face_first+f); }
        sq_stream_in_lod(mesh,in_lod);
        job->phase=1; job->cursor=0;
        faces=faces>mesh->face_count/8u ? faces-mesh->face_count/8u : 0u;
    }
    if(job->phase==1) {
        unsigned f=job->cursor;
        for(;f<mesh->face_count && faces;++f,--faces) {
            const unsigned absolute=mesh->face_first+f;
            const nm_face_t *const face=pack->faces+absolute;
            const nm_sq_face_t *const emit=sq_emit_faces[s]+absolute;
            const nm_baked_face_t baked=sq_baked_faces[absolute];
            const int pt=sq_stream_cutout(pack,face);
            nm_sq_stream_face_t *const out=records_out+(pt ? spans[1].base+rank[f] : f-rank[f]);
            const nm_sq_class_t *const front=records[baked.front].group;
            int sided=baked.back!=baked.front;
            const int two_lumas=sided && baked.back!=UINT16_MAX;
            out->group=face->group; out->count=face->count;
            out->flags=(uint8_t)((pt ? SQ_STREAM_PT : 0u)|(sided ? SQ_STREAM_SIDE : 0u)|(two_lumas ? SQ_STREAM_TWO : 0u));
            out->argb=records[baked.front].argb; out->header=front->header;
            float depth=front->depth;
            if(face->layer) depth*=draw_layer_scale[face->layer];
            out->depth=depth;
            const uint8_t *const order=face->count==4 ? quad_order : triangle_order;
            /* u, v as the queue path's writer forms them (sq_write_corner_c; the assembly forms equal it: the
             * vertex writer check before the first frame). */
            const float origin_u=front->origin[0],origin_v=front->origin[1],scale_u=front->scale[0],scale_v=front->scale[1];
            for(unsigned k=0;k<4;++k) {
                out->corner[k]=emit->corner[order[k]];
                out->uv[k][0]=((float)emit->uv[order[k]][0]-origin_u)*scale_u;
                out->uv[k][1]=((float)emit->uv[order[k]][1]-origin_v)*scale_v;
            }
            out->run=(uint16_t)(emit->special&SQ_FACE_RUN); out->strip=out->run ? emit->strip-plan.strip_first : 0u;
            out->front=baked.front; out->back=baked.back; out->index=(uint16_t)f;
            out->unused[0]=out->unused[1]=0;
            if(job->live_at<job->live_end && sq_live_faces[job->live_at].face==absolute) {
                /* Live specular (sq_live_update): side tested, no run, its record rewritten each frame. */
                nm_sq_live_face_t *const live=sq_live_faces+job->live_at++;
                const nm_sq_live_class_t *const k=sq_live_classes+live->live_class;
                out->flags|=(uint8_t)(SQ_STREAM_SIDE|(k->two ? SQ_STREAM_TWO : 0u));
                out->run=0; out->strip=0;
                live->record=(uint32_t)(out-records_out);
                sided=1;
            }
            if(sq_face_planes[s]) out->side=sq_face_planes[s][absolute];
            else { out->side.plane=0; out->side.reach=HUGE_VALF; }
            out->normal[0]=face->normal[0]; out->normal[1]=face->normal[1]; out->normal[2]=face->normal[2];
            job->tested+=(unsigned)sided; job->two+=(unsigned)two_lumas;
        }
        job->cursor=f;
        if(f<mesh->face_count) return 0;
        job->phase=2; job->cursor=0;
    }
    if(job->phase==2) {
        /* Offline strips: the colour of the face whose triangle a vertex completes, u, v of the first face's mapping. */
        unsigned i=job->cursor;
        for(;plan.strips && i<mesh->padding[3] && faces;++i) {
            const nm_strip_t *const strip=pack->strips+mesh->padding[2]+i;
            const nm_sq_class_t *const mapping=records[sq_baked_faces[strip->face_first].front].group;
            unsigned triangles=0;
            for(unsigned k=0;k<strip->face_count;++k) triangles+=pack->faces[strip->face_first+k].count-2u;
            unsigned member=0,completed=0; /* the face whose triangle vertex v completes, triangles before it */
            for(unsigned v=0;v<triangles+2u;++v) {
                const nm_sq_strip_corner_t *const in=sq_strip_corners[s]+strip->vertex_first+v;
                nm_sq_stream_vertex_t *const out=strips+(strip->vertex_first-plan.strip_first)+v;
                if(v>=2) {
                    while(v-2u>=completed+pack->faces[strip->face_first+member].count-2u) completed+=pack->faces[strip->face_first+member++].count-2u;
                }
                out->corner=in->corner;
                out->argb=records[sq_baked_faces[strip->face_first+member].front].argb;
                out->uv[0]=((float)in->uv[0]-mapping->origin[0])*mapping->scale[0];
                out->uv[1]=((float)in->uv[1]-mapping->origin[1])*mapping->scale[1];
            }
            faces=faces>strip->face_count ? faces-strip->face_count : 0u;
        }
        job->cursor=i;
        if(plan.strips && i<mesh->padding[3]) return 0;
        job->phase=3;
        if(!faces) return 0;
    }
    /* Position lists: per face group the distinct positions of its faces outside every level-of-detail group,
     * in face order (sq_stream_collect's order), from the faces bucketed by group once; then per
     * level-of-detail group and level. `order` and `starts` follow in_lod in the builder's scratch. */
    uint16_t *const order=(uint16_t *)(void *)(((uintptr_t)in_lod+mesh->face_count+1u)&~(uintptr_t)1u);
    uint16_t *const starts=order+mesh->face_count;
    const unsigned groups=mesh->padding[1];
    if(job->phase==3) {
        nm_memset(starts,0,(groups+1u)*sizeof(uint16_t));
        for(unsigned f=0;f<mesh->face_count;++f) if(!in_lod[f]) ++starts[pack->faces[mesh->face_first+f].group-mesh->padding[0]+1u];
        for(unsigned g=0;g<groups;++g) starts[g+1u]=(uint16_t)(starts[g+1u]+starts[g]);
        for(unsigned f=0;f<mesh->face_count;++f) if(!in_lod[f]) {
            const unsigned g=pack->faces[mesh->face_first+f].group-mesh->padding[0];
            order[starts[g]++]=(uint16_t)f;
        }
        for(unsigned g=groups;g>0;--g) starts[g]=starts[g-1u];
        starts[0]=0;
        job->phase=4; job->cursor=0; job->at=0;
        faces=faces>mesh->face_count/4u ? faces-mesh->face_count/4u : 0u;
        if(!faces) return 0;
    }
    if(job->phase==4) {
        const nm_sq_face_t *const emit=sq_emit_faces[s]+mesh->face_first;
        unsigned g=job->cursor,at=job->at;
        for(;g<groups && faces;++g) {
            unsigned n=0;
            for(unsigned i=starts[g];i<starts[g+1u];++i) {
                const unsigned f=order[i];
                for(unsigned v=0;v<emit[f].count;++v) {
                    const int16_t corner=emit[f].corner[v];
                    const unsigned index=(unsigned)((int)corner+32768)>>3;
                    if(seen[index]) continue;
                    seen[index]=1; points[at+n++]=corner;
                }
            }
            for(unsigned i=0;i<n;++i) seen[(unsigned)((int)points[at+i]+32768)>>3]=0;
            sq_stream_lists[2u*(mesh->padding[0]+g)]=at; sq_stream_lists[2u*(mesh->padding[0]+g)+1u]=n; at+=n;
            const unsigned cost=starts[g+1u]-starts[g]+1u;
            faces=faces>cost ? faces-cost : 0u;
        }
        job->cursor=g; job->at=at;
        if(g<groups) return 0;
        job->phase=5;
        if(!faces) return 0;
    }
    unsigned at=job->at;
    const unsigned lod_first=mesh->padding[4]&0xffffu,lod_here=nm_pack_lod_count(pack) ? mesh->padding[4]>>16 : 0u;
    for(unsigned i=0;i<lod_here;++i) {
        const nm_sq_lod_t *const lod=sq_lod_groups+lod_first+i;
        unsigned first=lod->first;
        for(unsigned level=0;level<NM_LOD_LEVELS;++level) {
            const unsigned n=sq_stream_collect(pack,mesh,first,first+lod->count[level],UINT_MAX,in_lod,seen,points+at);
            uint32_t *const pair=sq_stream_lists+sq_stream_lod_lists+2u*(3u*(lod_first+i)+level);
            pair[0]=at; pair[1]=n; at+=n; first+=lod->count[level];
        }
    }
    /* Live faces: this frame's bindings (sq_live_update may have run between the steps). */
    for(unsigned i=sq_live_meshes && sq_live_meshes[m].count ? sq_live_meshes[m].first : 0u;
        sq_live_meshes && i<sq_live_meshes[m].first+sq_live_meshes[m].count;++i) {
        const nm_sq_live_face_t *const e=sq_live_faces+i;
        if(e->record==UINT32_MAX) continue;
        nm_sq_stream_face_t *const out=records_out+e->record;
        const nm_baked_face_t baked=sq_baked_faces[e->face];
        out->front=baked.front; out->back=baked.back;
        out->argb=records[baked.front].argb; out->header=records[baked.front].group->header;
    }
    entry->faces=records_out; entry->strips=strips; entry->points=points; entry->rank=rank;
    job->phase=6;
    return 1;
}
static nm_sq_stream_job_t sq_stream_job;   /* record cache: the mesh being built (mesh UINT_MAX: none) */
static int sq_stream_prepare(const nm_pack_t *pack) {
    sq_stream_release();
    const unsigned s=1;
    const uint32_t pool=sq_stream_pool_wanted&~31u;
    const int cache=!sq_stream_wanted && pool>0u;
    const char *why=!sq_stream_wanted && !cache ? "not for this course (nm_render_world_streams)" : !sq_emit_faces[s] ? "no emission faces" : !sq_emit_records[s] ? "no emission records" :
        !sq_baked_faces ? "no baked bindings" : !sq_material_lists[s] ? "lists not from the material header" :
        !nm_pack_has_groups(pack) || !pack->position_refs ? "no face groups or shared positions" :
        !sq_sources[s].batches ? "no batches" : NULL;
    if(why) { printf("Native world streams: off (%s); every world batch on the queue path\n",why); return 1; }
    const nm_header_t *const original=pack->data;
    const unsigned lod_count=nm_pack_lod_count(pack);
    unsigned largest=1,slot=1,corners=1;
    for(unsigned m=0;m<original->meshes;++m) {
        if(!(m&63u)) nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        const nm_mesh_t *const mesh=pack->meshes+m;
        unsigned here=0;
        for(unsigned f=0;f<mesh->face_count;++f) here+=pack->faces[mesh->face_first+f].count;
        if(here>corners) corners=here;
        if(mesh->vertex_count>largest) largest=mesh->vertex_count;
        if(mesh->face_count>largest) largest=mesh->face_count;
        const unsigned levels=mesh->padding[1]+(lod_count ? mesh->padding[4]>>16 : 0u);
        if(levels>slot) slot=levels;
    }
    const unsigned group_count=original->reserved;
    const unsigned capacity=sq_sources[s].batch_capacity;
    const unsigned meshes=original->meshes ? original->meshes : 1u;
    const size_t list_words=2u*(group_count+3u*lod_count)+1u;
    const size_t state_bytes=capacity*(sizeof(nm_sq_stream_state_t)+((slot+3u)&~3u));
    sq_stream_lists=nm_heap_alloc("world stream list index",32,list_words*sizeof(uint32_t));
    sq_stream_table=nm_heap_alloc("world stream meshes",32,meshes*sizeof(nm_sq_stream_mesh_t));
    sq_stream_meshes=nm_heap_alloc("world stream mesh flags",0,meshes);
    sq_stream_plans=nm_heap_alloc("world stream mesh plans",0,meshes*sizeof(nm_sq_stream_plan_t));
    sq_stream_states=nm_heap_alloc("world stream batch state",32,state_bytes ? state_bytes : 32u);
    const size_t scratch_bytes=2u*largest+2u+(2u*largest+1u)*sizeof(uint16_t); /* seen, in_lod, order, starts */
    uint8_t *const scratch=nm_heap_alloc(cache ? "world stream builder scratch" : "world stream scratch",0,scratch_bytes);
    int16_t *const corner_scratch=nm_heap_alloc("world stream position scratch",0,corners*sizeof(int16_t));
    if(!sq_stream_lists || !sq_stream_table || !sq_stream_meshes || !sq_stream_plans || !sq_stream_states || !scratch || !corner_scratch) {
        free(corner_scratch); free(scratch); sq_stream_release(); return 0;
    }
    {
        uint8_t *const levels=(uint8_t *)(sq_stream_states+capacity);
        for(unsigned b=0;b<capacity;++b) { nm_memset(sq_stream_states+b,0,sizeof(*sq_stream_states)); sq_stream_states[b].levels=levels+b*((slot+3u)&~3u); }
    }
    nm_memset(sq_stream_lists,0,list_words*sizeof(uint32_t));
    nm_memset(sq_stream_meshes,0,meshes);
    sq_stream_lod_lists=2u*group_count;
    uint8_t *const seen=scratch,*const in_lod=scratch+largest;
    nm_memset(seen,0,largest);
    size_t total=0;
    unsigned eligible=0,eligible_faces=0,biggest=0;
    for(unsigned m=0;m<original->meshes;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one step */
        sq_stream_plan(pack,m,seen,in_lod,corner_scratch);
        if(sq_stream_plans[m].bytes && !sq_stream_check(pack,m)) sq_stream_plans[m].bytes=0;
        total+=sq_stream_plans[m].bytes;
        if(sq_stream_plans[m].bytes) { ++eligible; eligible_faces+=pack->meshes[m].face_count; }
        if(sq_stream_plans[m].bytes>biggest) biggest=sq_stream_plans[m].bytes;
    }
    free(corner_scratch);
    sq_stream_pack=pack;
    if(cache) {
        sq_stream_faces=nm_heap_alloc("world stream record pool",32,pool);
        sq_stream_seen=nm_heap_alloc("world stream walked frames",0,meshes*sizeof(uint32_t));
        sq_stream_offset=nm_heap_alloc("world stream block offsets",0,meshes*sizeof(uint32_t));
        sq_stream_resident=nm_heap_alloc("world stream resident meshes",0,meshes*sizeof(uint16_t));
        if(!sq_stream_faces || !sq_stream_seen || !sq_stream_offset || !sq_stream_resident) { free(scratch); sq_stream_release(); return 0; }
        nm_memset(sq_stream_seen,0,meshes*sizeof(uint32_t));
        for(unsigned m=0;m<meshes;++m) sq_stream_offset[m]=UINT32_MAX;
        for(unsigned m=0;m<original->meshes;++m) if(sq_stream_plans[m].bytes>pool) sq_stream_plans[m].bytes=0;
        sq_stream_scratch=scratch; sq_stream_largest=largest;
        sq_stream_pool_bytes=pool; sq_stream_frame=1u; sq_stream_face_us=0.0f; sq_stream_job.mesh=UINT_MAX;
        sq_stream_bytes=(unsigned)(pool+list_words*sizeof(uint32_t)+meshes*(sizeof(nm_sq_stream_mesh_t)+1u+sizeof(nm_sq_stream_plan_t)+
            2u*sizeof(uint32_t)+sizeof(uint16_t))+state_bytes+scratch_bytes);
        printf("Native world streams: record cache of %u bytes for %u meshes, %u faces (all meshes' records %lu bytes, "
            "the largest mesh's %u); built between frames as the meshes are walked; tables=%u bytes\n",
            (unsigned)pool,eligible,eligible_faces,(unsigned long)total,biggest,sq_stream_bytes);
        return 1;
    }
    sq_stream_faces=nm_heap_alloc("world stream records",32,total ? total : 32u);
    if(!sq_stream_faces) { free(scratch); sq_stream_release(); return 0; }
    unsigned on_path=0,faces_on=0,tested=0,two=0,excluded=0,strip_vertices=0,positions=0;
    size_t at=0;
    for(unsigned m=0;m<original->meshes;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one step */
        const nm_mesh_t *const mesh=pack->meshes+m;
        if(!sq_stream_plans[m].bytes) { if(mesh->face_count) ++excluded; continue; }
        nm_sq_stream_job_t job;
        sq_stream_job_start(&job,m,(uint8_t *)sq_stream_faces+at);
        (void)sq_stream_job_step(pack,&job,seen,in_lod,UINT_MAX);
        tested+=job.tested; two+=job.two;
        at+=sq_stream_plans[m].bytes; strip_vertices+=sq_stream_plans[m].strips; positions+=sq_stream_plans[m].points;
        sq_stream_meshes[m]=1;
        ++on_path; faces_on+=mesh->face_count;
    }
    free(scratch);
    sq_stream_mesh_count=on_path;
    sq_stream_bytes=(unsigned)(total+list_words*sizeof(uint32_t)+meshes*(sizeof(nm_sq_stream_mesh_t)+1u+sizeof(nm_sq_stream_plan_t))+state_bytes);
    printf("Native world streams: %u meshes, %u faces (%u with a side test, %u of them with two lumas), %u meshes on the queue path; "
        "%u strip vertices, %u positions in %u lists; tables=%u bytes\n",on_path,faces_on,tested,two,excluded,strip_vertices,positions,
        group_count+3u*lod_count,sq_stream_bytes);
    return 1;
}
/* The record cache between two frames: the frame just drawn walked the meshes
 * whose sq_stream_seen is the frame number before this one's. While the time
 * left (budget_us, of the idle wait) lasts, the mesh being built takes its
 * next steps (SQ_STREAM_STEP faces each, the clock read between them), then
 * the largest walked mesh without records gets a block (the run of free space
 * and blocks of meshes walked longest ago, none in the last
 * SQ_STREAM_KEEP_FRAMES, that frees the most stale space first) and its build
 * starts. A mesh joins the path when its build is done. Nothing of the
 * frame's work holds a record across the wait: a batch takes the path at its
 * prepare step (sq_prepare_batch, in the list callbacks after it). Returns
 * the microseconds spent. */
#ifdef __DREAMCAST__
#define SQ_STREAM_NOW_US() timer_us_gettime64()
#else
static uint64_t sq_stream_host_us; /* host tools: the builder's own clock, 3 us a face */
#define SQ_STREAM_NOW_US() sq_stream_host_us
#endif
#define SQ_STREAM_STEP 16u
static unsigned sq_stream_place(unsigned bytes,unsigned frame,unsigned *first,unsigned *end,uint32_t *offset) {
    /* Windows [i, j) of resident blocks evicted plus the free space around them: the one whose newest evicted
     * block was walked longest ago, then the fewest bytes evicted. The block being built stays. */
    const unsigned n=sq_stream_resident_count;
    uint32_t best_stamp=UINT32_MAX,best_evicted=UINT32_MAX;
    int found=0;
    for(unsigned i=0;i<=n;++i) {
        const uint32_t start=i ? sq_stream_offset[sq_stream_resident[i-1u]]+sq_stream_plans[sq_stream_resident[i-1u]].bytes : 0u;
        uint32_t stamp=0,evicted=0;
        for(unsigned j=i;;++j) {
            const uint32_t stop=j<n ? sq_stream_offset[sq_stream_resident[j]] : sq_stream_pool_bytes;
            if(stop-start>=bytes) {
                if(stamp<best_stamp || (stamp==best_stamp && evicted<best_evicted)) {
                    best_stamp=stamp; best_evicted=evicted; *first=i; *end=j; *offset=start; found=1;
                }
                break;
            }
            if(j==n) break;
            const unsigned r=sq_stream_resident[j];
            if(r==sq_stream_job.mesh) break;
            const uint32_t walked=sq_stream_meshes[r] ? sq_stream_seen[r] : 0u; /* a mesh gone back to the queue path: free */
            if(walked+SQ_STREAM_KEEP_FRAMES>=frame) break;
            if(walked>stamp) stamp=walked;
            evicted+=sq_stream_plans[r].bytes;
        }
    }
    return (unsigned)found;
}
static unsigned sq_stream_idle(unsigned budget_us) {
    if(!sq_stream_pool_bytes || !sq_stream_pack) return 0;
    const uint32_t frame=++sq_stream_frame,walked=frame-1u;
    const nm_pack_t *const pack=sq_stream_pack;
    const unsigned meshes=((const nm_header_t *)pack->data)->meshes;
    ++sq_stream_cache_stats.frames;
    {
        unsigned missed=0;
        for(unsigned m=0;m<meshes;++m)
            if(sq_stream_seen[m]==walked && !sq_stream_meshes[m] && sq_stream_plans[m].bytes) { ++missed; sq_stream_cache_stats.missed_faces+=pack->meshes[m].face_count; }
        sq_stream_cache_stats.missed+=missed; sq_stream_cache_stats.busy_frames+=missed!=0;
    }
    const uint64_t begin=SQ_STREAM_NOW_US();
    uint64_t now=begin;
    while(now-begin<budget_us) {
        if(sq_stream_job.mesh==UINT_MAX) {
            unsigned want=UINT_MAX;
            for(unsigned m=0;m<meshes;++m)
                if(sq_stream_seen[m]==walked && sq_stream_offset[m]==UINT32_MAX && sq_stream_plans[m].bytes &&
                   (want==UINT_MAX || pack->meshes[m].face_count>pack->meshes[want].face_count)) want=m;
            if(want==UINT_MAX) break;
            unsigned first,end; uint32_t offset;
            if(!sq_stream_place(sq_stream_plans[want].bytes,frame,&first,&end,&offset)) { ++sq_stream_cache_stats.refused; break; }
            for(unsigned j=first;j<end;++j) {
                const unsigned r=sq_stream_resident[j];
                sq_stream_meshes[r]=0; sq_stream_offset[r]=UINT32_MAX; ++sq_stream_cache_stats.evicted;
            }
            /* The resident list without [first, end), the new block at `first`. */
            nm_memmove(sq_stream_resident+first+1u,sq_stream_resident+end,(sq_stream_resident_count-end)*sizeof(uint16_t));
            sq_stream_resident_count=sq_stream_resident_count-(end-first)+1u;
            sq_stream_resident[first]=(uint16_t)want; sq_stream_offset[want]=offset;
            sq_stream_job_start(&sq_stream_job,want,(uint8_t *)sq_stream_faces+offset);
            uint32_t high=0;
            for(unsigned j=0;j<sq_stream_resident_count;++j) {
                const unsigned r=sq_stream_resident[j];
                if(sq_stream_offset[r]+sq_stream_plans[r].bytes>high) high=sq_stream_offset[r]+sq_stream_plans[r].bytes;
            }
            if(high>sq_stream_cache_stats.high) sq_stream_cache_stats.high=high;
        }
        const int done=sq_stream_job_step(pack,&sq_stream_job,sq_stream_scratch,sq_stream_scratch+sq_stream_largest,SQ_STREAM_STEP);
#ifndef __DREAMCAST__
        sq_stream_host_us+=3u*SQ_STREAM_STEP;
#endif
        if(done) {
            const unsigned m=sq_stream_job.mesh;
            sq_stream_meshes[m]=1; sq_stream_job.mesh=UINT_MAX;
            ++sq_stream_cache_stats.built; sq_stream_cache_stats.faces+=pack->meshes[m].face_count;
        }
        now=SQ_STREAM_NOW_US();
    }
    const unsigned used=(unsigned)(now-begin);
    sq_stream_cache_stats.us+=used;
    if(used>sq_stream_cache_stats.max_us) sq_stream_cache_stats.max_us=used;
    if(sq_stream_cache_stats.faces) sq_stream_face_us=(float)sq_stream_cache_stats.us/(float)sq_stream_cache_stats.faces;
    return used;
}
#else
static void sq_stream_release(void) {}
static unsigned sq_stream_idle(unsigned budget_us) { (void)budget_us; return 0; }
static int sq_stream_prepare(const nm_pack_t *pack) {
    (void)pack;
#if !NM_SQ_STREAM
    puts("Native world streams: off (NM_SQ_STREAM=0); every world batch on the queue path");
#endif
    return 1;
}
#endif
/* The road-surface depth class at another scale (nm_render_road_depth: the
 * scene's camera changed, a rare event). The world's emission classes of the
 * road bindings: a class key holds the depth, so while it is below 1 no other
 * binding has such a class. Then every prebuilt stream record whose binding is
 * road surface (every binding its face can take has the front's class,
 * sq_stream_check): its class's depth times the face's layer factor, as
 * sq_stream_job_step forms it. A mesh the record cache is building starts
 * again (what it wrote is not on the path yet); records built later read the
 * classes. In a frame's setup or between frames: nothing holds a class or a
 * record across it, and the stream stays the queue path's word for word.
 * Cost: one pass over the bindings and over the records of the meshes on the
 * path (a whole-table course's ~15,000). */
static __attribute__((noinline)) void sq_road_depth_set(float scale) {
    if(scale==sq_road_depth || !(scale>0.0f && scale<1.0f) || !(sq_road_depth<1.0f)) return;
    sq_road_depth=scale;
    nm_sq_class_t *const classes=sq_emit_classes[1];
    const nm_sq_bind_t *const records=sq_emit_records[1];
    if(!classes || !records || !sq_surface_bindings || !world_draw.materials) return;
    for(unsigned b=0;b<world_draw.materials->count;++b)
        if(sq_surface_bindings[b]) classes[records[b].group-classes].depth=scale;
#if SQ_STREAMS
    const nm_pack_t *const pack=sq_stream_pack;
    if(!sq_stream_faces || !pack) return;
    const unsigned meshes=((const nm_header_t *)pack->data)->meshes;
    for(unsigned m=0;m<meshes;++m) {
        if(!sq_stream_meshes[m]) continue;
        const nm_mesh_t *const mesh=pack->meshes+m;
        const nm_face_t *const faces=pack->faces+mesh->face_first;
        nm_sq_stream_face_t *const out=sq_stream_table[m].faces;
        for(unsigned r=0;r<mesh->face_count;++r) {
            if(!sq_surface_bindings[out[r].front]) continue;
            const unsigned layer=faces[out[r].index].layer;
            const float depth=records[out[r].front].group->depth;
            out[r].depth=layer ? depth*draw_layer_scale[layer] : depth;
        }
    }
    if(sq_stream_pool_bytes && sq_stream_job.mesh!=UINT_MAX)
        sq_stream_job_start(&sq_stream_job,sq_stream_job.mesh,sq_stream_job.block);
#endif
}
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
/* Before the world's batches are walked: this frame's bindings of the live faces of the meshes the frame draws
 * (`pack`'s live instance table, the visible cells; a class shared by several is evaluated once). `view` is the
 * world instances' matrix (column-major 3x4). */
static __attribute__((unused,noinline)) void sq_live_update(const nm_pack_t *pack,const float *view,const nm_sq_bind_t *records) {
    if(!sq_live_class_count || !records) return;
    const float a0=view[2],a1=view[5],a2=view[8];
    const uint32_t stamp=++sq_live_stamp;
    for(unsigned n=0;n<pack->header->instances;++n) {
        nm_sq_live_mesh_t *const lm=sq_live_meshes+pack->instances[n].mesh;
        if(!lm->count || lm->stamp==stamp) continue;
        lm->stamp=stamp;
        for(unsigned i=lm->first;i<lm->first+lm->count;++i) {
            const nm_sq_live_face_t *const e=sq_live_faces+i;
            nm_sq_live_class_t *const k=sq_live_classes+e->live_class;
            if(k->stamp!=stamp) {
                k->stamp=stamp;
#ifdef __DREAMCAST__
                const float dot=shz_dot6f(k->r[0],k->r[1],k->r[2],a0,a1,a2);
#else
                const float dot=k->r[0]*a0+k->r[1]*a1+k->r[2]*a2;
#endif
                const float term=sq_live_power(dot,k->control)*k->scale;
                for(unsigned side=0;side<2u;++side) {
                    float luminance=k->base[side]+term;
                    if(luminance<0) luminance=0;
                    if(luminance>255) luminance=255;
                    unsigned luma=(unsigned)(int)luminance;
                    /* Within the exported range (the dot's rounding cannot leave it by more than the margin). */
                    if(luma<k->low[side]) luma=k->low[side];
                    if(luma>k->high[side]) luma=k->high[side];
                    k->binding[side]=sq_live_rows[k->row+luma];
                }
            }
            const uint16_t front=k->binding[0],back=k->two ? k->binding[1] : UINT16_MAX;
            sq_baked_faces[e->face]=(nm_baked_face_t){front,back};
#if SQ_STREAMS
            if(e->record!=UINT32_MAX && sq_stream_meshes && sq_stream_meshes[pack->instances[n].mesh]) {
                nm_sq_stream_face_t *const out=sq_stream_table[pack->instances[n].mesh].faces+e->record;
                out->front=front; out->back=back;
                out->argb=records[front].argb; out->header=records[front].group->header;
            }
#endif
        }
    }
    ++sq_live_updates;
}
#endif
#if SQ_CAR_STREAMS
/* The car stream's tables, per car pack once its emission records, emission
 * faces, plane constants, light classes and hot face records exist
 * (nm_render_init): per mesh its distinct positions (the representatives
 * its faces' corner words name) and its first punch-through face; per car
 * batch the side-test terms; the light class entries of one list callback
 * (two per class of the mesh with the most). A mesh joins the path when its
 * faces are in list order (opaque, then punch-through: DNM6 sorts them so),
 * reject_draw_face refuses none of them for its attribute or material (a
 * screen-checker or undrawn face), every u, v of its faces is below 32768
 * (sq_car_put_face_sh4) and a strip run's colours fit (SQ_CAR_RUN_MAX). */
static unsigned sq_car_mesh_count,sq_car_bytes;
static void sq_car_stream_release(void) {
    free(sq_car_meshes); free(sq_car_positions); free(sq_car_facing); free(sq_car_entries);
    sq_car_meshes=NULL; sq_car_positions=NULL; sq_car_facing=NULL; sq_car_entries=NULL;
    sq_car_entry_count=sq_car_stamp=sq_car_mesh_count=sq_car_bytes=0;
}
/* The distinct positions of a mesh's faces, in ascending order, at `out`
 * (NULL: count them). `seen` holds a zero byte per vertex of the mesh and is
 * left so. */
static unsigned sq_car_collect(const nm_mesh_t *mesh,uint8_t *seen,uint16_t *out) {
    const nm_sq_face_t *const emit=sq_emit_faces[0]+mesh->face_first;
    unsigned low=UINT_MAX,high=0,count=0;
    for(unsigned f=0;f<mesh->face_count;++f)
        for(unsigned v=0;v<emit[f].count;++v) {
            const unsigned index=(unsigned)((int)emit[f].corner[v]+32768)>>3;
            seen[index]=1; if(index<low) low=index; if(index>high) high=index;
        }
    for(unsigned v=low;v<=high && low!=UINT_MAX;++v) if(seen[v]) { if(out) out[count]=(uint16_t)v; ++count; seen[v]=0; }
    return count;
}
static int sq_car_stream_prepare(const nm_pack_t *pack) {
    sq_car_stream_release();
    const char *why=!sq_emit_faces[0] ? "no emission faces" : !sq_emit_records[0] ? "no emission records" :
        !sq_lit_faces || !sq_light_class || !sq_light_cache_size ? "no hot face records (plane constants, light classes)" :
        !sq_material_lists[0] ? "lists not from the material header" : nm_pack_has_groups(pack) ? "face groups" :
        !pack->position_refs ? "no shared positions" : !sq_sources[0].batches ? "no batches" : NULL;
    if(why) { printf("Native car streams: off (%s); every car part on the queue path\n",why); return 1; }
    const nm_header_t *const original=pack->data;
    const unsigned capacity=sq_sources[0].batch_capacity;
    unsigned largest=1,positions=0;
    for(unsigned m=0;m<original->meshes;++m) if(pack->meshes[m].vertex_count>largest) largest=pack->meshes[m].vertex_count;
    uint8_t *const seen=nm_heap_alloc("car stream scratch",0,largest);
    if(!seen) return 0;
    nm_memset(seen,0,largest);
    for(unsigned m=0;m<original->meshes;++m) positions+=sq_car_collect(pack->meshes+m,seen,NULL);
    sq_car_meshes=nm_heap_alloc("car stream meshes",32,(original->meshes ? original->meshes : 1u)*sizeof(*sq_car_meshes));
    sq_car_positions=nm_heap_alloc("car stream positions",32,(positions+2u)*sizeof(*sq_car_positions));
    sq_car_facing=nm_heap_alloc("car stream batch state",32,(capacity ? capacity : 1u)*sizeof(*sq_car_facing));
    sq_car_entry_count=2u*sq_light_cache_size;
    sq_car_entries=nm_heap_alloc("car stream class entries",32,sq_car_entry_count*sizeof(*sq_car_entries));
    if(!sq_car_meshes || !sq_car_positions || !sq_car_facing || !sq_car_entries) { free(seen); sq_car_stream_release(); return 0; }
    nm_memset(sq_car_entries,0,sq_car_entry_count*sizeof(*sq_car_entries));
    nm_memset(sq_car_facing,0,(capacity ? capacity : 1u)*sizeof(*sq_car_facing));
    sq_car_positions[positions]=sq_car_positions[positions+1u]=0; /* the prefetch in sq_car_batch_prepare reads two ahead */
    unsigned at=0,on_path=0,faces_on=0,excluded=0,cutout_faces=0,run_faces=0;
    for(unsigned m=0;m<original->meshes;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one step */
        const nm_mesh_t *const mesh=pack->meshes+m;
        const nm_sq_lit_face_t *const lit=sq_lit_faces+mesh->face_first;
        const nm_sq_face_t *const emit=sq_emit_faces[0]+mesh->face_first;
        nm_sq_car_mesh_t *const out=sq_car_meshes+m;
        const unsigned count=sq_car_collect(mesh,seen,sq_car_positions+at);
        unsigned cutouts=mesh->face_count,runs=0;
        int ok=mesh->face_count<=SQ_CAR_RUN_MAX && count<=UINT16_MAX;
        for(unsigned f=0;f<mesh->face_count;++f) {
            const unsigned flags=lit[f].flags;
            if(flags&SQ_LIT_CUTOUT) { if(cutouts==mesh->face_count) cutouts=f; }
            else if(cutouts!=mesh->face_count) ok=0;        /* an opaque face after a punch-through one */
            if((flags&(SQ_LIT_DRAWN|SQ_LIT_CHECKER))!=SQ_LIT_DRAWN) ok=0;
            for(unsigned v=0;v<emit[f].count;++v) if(emit[f].uv[v][0]>=32768u || emit[f].uv[v][1]>=32768u) ok=0;
            runs+=(emit[f].special&SQ_FACE_RUN)!=0;
        }
        out->positions=ok ? at : UINT32_MAX; out->position_count=(uint16_t)(ok ? count : 0u); out->cutouts=(uint16_t)cutouts;
        at+=count;
        if(ok) { ++on_path; faces_on+=mesh->face_count; cutout_faces+=mesh->face_count-cutouts; run_faces+=runs; } else ++excluded;
    }
    free(seen);
    sq_car_mesh_count=on_path;
    sq_car_bytes=(unsigned)(original->meshes*sizeof(*sq_car_meshes)+(positions+2u)*sizeof(*sq_car_positions)+
        capacity*sizeof(*sq_car_facing)+sq_car_entry_count*sizeof(*sq_car_entries));
    printf("Native car streams: %u meshes, %u faces (%u punch-through, %u may start a strip run), %u meshes on the queue path; "
        "%u positions; %u light class entries; tables=%u bytes\n",on_path,faces_on,cutout_faces,run_faces,excluded,at,sq_car_entry_count,sq_car_bytes);
    return 1;
}
#else
static void sq_car_stream_release(void) {}
static int sq_car_stream_prepare(const nm_pack_t *pack) {
    (void)pack;
#if !NM_SQ_CAR_STREAM || !NM_SQ_STREAM
    puts("Native car streams: off (NM_SQ_CAR_STREAM=0 or NM_SQ_STREAM=0); every car part on the queue path");
#endif
    return 1;
}
#endif
/* The production emitter: the batches of one object on one list. Each batch
 * is proven to fit (sq_faces_fit) and written by the loop above; after a
 * clipped face the rest is proven again. A batch that cannot be proven, near
 * the TA budget or the triangle cap, goes through the guarded general emitter
 * from where it stands. Needs the emission records of the bindings (whose
 * classes name canonical headers: two bindings submit the same header exactly
 * when their classes point at the same one) and of the faces; without them
 * (host fixtures) sq_submit_object uses sq_emit_range. */
static int sq_emit_batches(unsigned list,unsigned s,unsigned batch_first,unsigned batch_end) {
    DRAW_PROFILE_SOURCE(s);
    const int cutout=list==PVR_LIST_PT_POLY;
    const pvr_poly_hdr_t *previous=NULL;
    if(!draw_header_count) sq_ta_vertices=0;
    if(batch_end>sq_sources[s].count) batch_end=sq_sources[s].count;
    for(unsigned b=batch_first;b<batch_end;++b) {
        const nm_sq_batch_t *const batch=sq_sources[s].batches+b;
        sq_ss_batch(batch,&previous);
#if SQ_STREAMS
        if(batch->stream) {
#if SQ_CAR_STREAMS
            if(batch->stream==SQ_BATCH_CAR) {
                /* A car part on the prebuilt car stream (sq_car_list). */
                SQ_PATH_EMIT(3,s,batch,list,1u);
#ifdef NM_DIAG_PMU
                SQ_PMU_BEGIN(pmu_car);
                const int car_ok=cutout ? sq_car_emit_pt(batch,&previous) : sq_car_emit_op(batch,&previous);
                SQ_PMU_END(pmu_car,cutout ? SQ_PMU_CAR_PT : SQ_PMU_CAR_OP);
                if(!car_ok) return 0;
#else
                if(!(cutout ? sq_car_emit_pt(batch,&previous) : sq_car_emit_op(batch,&previous))) return 0;
#endif
                continue;
            }
#endif
            /* A world batch on the prebuilt stream (sq_stream_list). */
            SQ_PATH_EMIT(3,s,batch,list,1u);
#ifdef NM_DIAG_PMU
            SQ_PMU_BEGIN(pmu_world);
            const int world_ok=cutout ? sq_stream_emit_pt(batch,&previous) : sq_stream_emit_op(batch,&previous);
            SQ_PMU_END(pmu_world,cutout ? SQ_PMU_WORLD_PT : SQ_PMU_WORLD_OP);
            if(!world_ok) return 0;
#else
            if(!(cutout ? sq_stream_emit_pt(batch,&previous) : sq_stream_emit_op(batch,&previous))) return 0;
#endif
            continue;
        }
#endif
#if SQ_SWEEPS
        if(batch->sweep) {
            /* A catch-fence panel across a sweep plane: the general emitter clips it (sq_sweep_face). */
            const unsigned faces=cutout ? batch->cutout_count : batch->opaque_count;
            SQ_PATH_EMIT(1,s,batch,list,faces);
            if(faces && !sq_emit_batch_general(list,s,batch,0,&previous)) return 0;
            continue;
        }
#endif
        SQ_PMU_BEGIN(pmu_queue);
        const unsigned face_count=cutout ? batch->cutout_count : batch->opaque_count;
        for(unsigned done=0;done<face_count;) {
            if(!sq_faces_fit(face_count-done)) {
                SQ_PATH_EMIT(1,s,batch,list,face_count-done);
                if(!sq_emit_batch_general(list,s,batch,done,&previous)) return 0;
                break;
            }
            SQ_PATH_EMIT(0,s,batch,list,face_count-done);
            done=cutout ? sq_emit_faces_pt(s,batch,done,&previous) : sq_emit_faces_op(s,batch,done,&previous);
            if(done==UINT_MAX) return 0;
        }
        SQ_PMU_END(pmu_queue,SQ_PMU_QUEUE_EMIT);
    }
    return 1;
}
/* One borrowed batch range per object, shared by its OP and PT callbacks.
 * Each callback starts with an explicit header; other objects may change TA state. */
#ifdef NM_DIAG_FRAME_TRACE
static unsigned sq_ft_opponent;
#endif
static void sq_submit_object(nm_sq_batch_t *object,unsigned list) {
    DRAW_PROFILE_SOURCE(object->source);
#ifdef NM_DIAG_FRAME_TRACE
    const unsigned ft_kind=object->source ? 0 : sq_ft_opponent ? 2 : 1;
    const uint32_t ft_start=nm_ft_clock();
#endif
#ifdef NATIVE_CACHE_PROFILE
    DRAW_PHASE_START(cache_submit_started);
#endif
    const unsigned first=(unsigned)(object-sq_sources[object->source].batches);
#ifdef NM_DIAG_PMU
    for(unsigned b=0;!sq_failed && b<object->object_batches;++b)
        if(!object[b].prepared) {
            SQ_PMU_BEGIN(pmu_prepare);
            if(!sq_prepare_batch(object+b)) sq_failed=1;
            SQ_PMU_END(pmu_prepare,object[b].stream==1u /* SQ_BATCH_STREAM */ ? SQ_PMU_WORLD_PREPARE :
                object[b].stream ? SQ_PMU_CAR_PREPARE : SQ_PMU_QUEUE_PREPARE);
        }
#else
    for(unsigned b=0;!sq_failed && b<object->object_batches;++b)
        if(!object[b].prepared && !sq_prepare_batch(object+b)) sq_failed=1;
#endif
#ifdef NM_DIAG_FRAME_TRACE
    const uint32_t ft_prepared=nm_ft_clock();
    const uint32_t ft_prep=nm_ft_delta(ft_start,ft_prepared);
    const unsigned ft_tris=draw_count;
    nm_ft_group[NM_FT_WORLD_PREP+ft_kind]+=ft_prep;++nm_ft_calls[NM_FT_WORLD_PREP+ft_kind];
#endif
    /* The inspection views and builds without emission tables keep the
     * general emitter; a frame of the game takes the production one. */
    const int general=sq_general_emitter(object->source);
    SQ_PMU_BEGIN(pmu_general); /* (the production emitter's groups: sq_emit_batches) */
    if(!sq_failed && !(general ? sq_emit_range(list,object->source,object->source+1,first,first+object->object_batches) :
                                 sq_emit_batches(list,object->source,first,first+object->object_batches))) {
        puts("FAIL: object SQ emit; stopping on next update"); sq_failed=1;
    }
#ifdef NM_DIAG_PMU
    if(general) SQ_PMU_END(pmu_general,SQ_PMU_QUEUE_EMIT);
#endif
#ifdef NATIVE_CACHE_PROFILE
    DRAW_PHASE_END(10,cache_submit_started);
#endif
#ifdef NM_DIAG_FRAME_TRACE
    {
        const unsigned group=NM_FT_WORLD_OP+2*ft_kind+(list==PVR_LIST_PT_POLY);
        const uint32_t emit=nm_ft_delta(ft_prepared,nm_ft_clock());
        nm_ft_group[group]+=emit;++nm_ft_calls[group];
        const unsigned pt=list==PVR_LIST_PT_POLY,tris=draw_count-ft_tris;
        if(!ft_kind) {
            const unsigned b=(unsigned)(object-sq_sources[1].batches);
            if(b<NM_FT_OBJECTS) {
                nm_ft_object_t *o=nm_ft_objects+b;
                o->mesh=(uint16_t)(object->mesh-world_draw.pack->meshes);o->faces=(uint16_t)object->mesh->face_count;
                o->vertices=(uint16_t)object->mesh->vertex_count;
                o->prep=(uint16_t)(o->prep+ft_prep);
                if(pt) { o->pt=(uint16_t)(o->pt+emit);o->tri_pt=(uint16_t)tris; } else { o->op=(uint16_t)(o->op+emit);o->tri_op=(uint16_t)tris; }
                if(b>=nm_ft_object_count) nm_ft_object_count=b+1;
            }
        } else if(ft_kind==2 && object->instance_index<NM_FT_PARTS) {
            nm_ft_part_t *o=nm_ft_parts+object->instance_index;
            o->prep=(uint16_t)(o->prep+ft_prep);o->tri=(uint16_t)(o->tri+tris);
            if(pt) o->pt=(uint16_t)(o->pt+emit); else o->op=(uint16_t)(o->op+emit);
        }
    }
#endif
}
static void sq_submit_object_op(void *data) { sq_submit_object(data,PVR_LIST_OP_POLY); }
static void sq_submit_object_pt(void *data) { sq_submit_object(data,PVR_LIST_PT_POLY); }

/* ---- Translucent effects: the checker materials on the translucent list -----
 * The arcade draws a polygon whose texture header has the screen-checker bit 15 on every other pixel of a
 * screen checkerboard (raster.cpp), sorted with every other polygon by its nearest depth (attribute bits 10-11 =
 * 1, the painter's key). Among the cars' meshes these are the smoke models of the car effects (car_effects.h:
 * 231f86, 232206), every face textured with texel 15 transparent. On the PVR such a material is the translucent
 * class (material_pack.h, nm_material_translucent: palette entry 15 alpha 0, vertex alpha 0x80): its header
 * (compile_draw_header) is on the translucent list, alpha blended (SRCALPHA, INVSRCALPHA, texture alpha times
 * vertex alpha), depth compare against the scene, no depth write, culling off, filtered like the punch-through
 * cutouts. Half the light of the polygon over what lies behind it is what the screen door shows averaged over two
 * pixels, without its dither pattern; texel 15 stays a hole.
 * The instances of such meshes take no batch (sq_prepare_source queues them in sq_tr_queue); one translucent
 * list callback transforms their vertices with the same cache, lights their faces with the same rule as the
 * reference, sorts every face of the frame back to front by its nearest view-space depth (the arcade's key;
 * a stable insertion sort: equal keys keep the instances' order) and submits them in that order through the
 * general face emitter (sq_emit_face_general: the same clipper, UVs and depth as every car face). The PVR runs the
 * translucent list in pre-sort mode (code/main.c): polygons in submission order, no per-pixel sort, so a frame's
 * at most ~90 effect polygons cost their samples once. At most SQ_TR_INSTANCES_MAX instances and
 * SQ_TR_FACES_MAX faces a frame (the effects reach 10 / 87).
 * make NATIVE_SMOKE_MODE=pt-tex|pt-screen (an experiment; NM_SMOKE_MODE 1, 2): the same faces on the punch-through
 * list, depth written, not sorted (the nearest polygon wins each pixel, as the arcade's front-to-back painter), with
 * the textures of materials.c (nm_materials_smoke_texture):
 *   pt-tex: every other texel transparent, point sampled; the checker is the texture's, so it grows, shrinks and turns
 *     with each puff, and two puffs cover each other's holes.
 *   pt-screen: every other screen pixel transparent, as the arcade (raster.cpp draws pixels with x ^ y odd only, on
 *     every checker polygon: overlapping puffs never cover more than half the pixels). The PVR interpolates u/w
 *     and 1/w linearly on the screen; with one 1/w on every vertex of a face, u itself is linear on the screen, so
 *     u' = u/2 + (x + y)/2 (x, y output pixels; texture twice as wide, the right half transparent) reads the smoke
 *     texel at pixels of one parity and the transparent half at the others, whatever the size and angle of the
 *     puff. The cost: one depth per face (its nearest corner's, the arcade's painter key: only what lies nearer
 *     than that corner hides any of the face, as the arcade's painter orders it whole) and an affine instead of a
 *     perspective texture on the face (the puffs are small and face the camera).
 *   tr-screen (NM_SMOKE_MODE 3): the pt-screen faces (texture, u', one depth per face) on the translucent list, sorted
 *     back to front as tr, alpha the texel's: a pixel takes the smoke texel or keeps what is under it, and the last
 *     (nearest) puff over a pixel wins it: the arcade's painter, as pt-screen's depth test. The punch-through list peels
 *     every hole a pixel lies under in a pass of its own (a spin cloud: 9-10 layers of 74-91% holes); the pre-sorted
 *     translucent list touches each covered sample once. Point sampled (NM_SMOKE_POINT), its pixels are pt-screen's
 *     point-sampled ones; filtered, a sample at a hole's edge blends instead of being cut at alpha 1/2.
 *   NM_SMOKE_TIGHT: either form drawn in tight pieces (sq_tr_tight_prepare below). */
static nm_cached_vertex_t *sq_tr_points;
static unsigned sq_tr_point_capacity, sq_tr_mesh_vertices;
#if NM_SMOKE_DOOR && NM_SMOKE_TIGHT
/* make NATIVE_SMOKE_MODE=pt-screen NATIVE_SMOKE_TIGHT=1: tight smoke geometry. The smoke textures are 45-91% texel 15
 * (holes); on the punch-through list every covered sample costs the TSP a texture read and the alpha test, and every
 * hole layer a pixel lies under costs its tile another punch-through pass. So each smoke face is drawn as up to
 * NM_SMOKE_PIECES rectangles of its own texture (pieces) that hold every texel a sample can see: the texels that are
 * not texel 15, grown by one texel on every side (the bilinear footprint of a sample, either sample-point
 * convention), within the face's UV rectangle. Outside the pieces the face reads only holes, so nothing that is drawn
 * changes. Chosen at load (sq_tr_tight_prepare) per face: the fewest texels plus NM_SMOKE_PIECE_TEXELS for every
 * piece (its four TA vertices and two triangles), over one rectangle and two or three slabs of the face.
 * A piece's corners are points of the face's plane (the face's corners interpolated in view space: a smoke face is a
 * parallelogram with a rectangle of UVs) with their exact UVs; the PVR interpolates inside a piece as it did inside
 * the face, so a piece corner reads exactly what the arcade's perspective-correct raster reads there and the affine
 * drift inside a piece is smaller than inside the face. Every piece keeps its face's one depth (the face's nearest
 * corner: the arcade's painter key) and the face's u' base. A face with a corner, or a piece with a corner, outside
 * the view is drawn whole, as before (the clipper). nm_smoke_texture_mask (materials.c; the simulator's own on the
 * host) gives the texture's non-hole texels. */
#ifndef NM_SMOKE_PIECES
#define NM_SMOKE_PIECES 2
#endif
#ifndef NM_SMOKE_PIECE_TEXELS
#define NM_SMOKE_PIECE_TEXELS 48
#endif
_Static_assert(NM_SMOKE_PIECES>=1 && NM_SMOKE_PIECES<=3,"NM_SMOKE_PIECES: 1..3");
int nm_smoke_texture_mask(unsigned resource,unsigned side,uint8_t *mask);
#define SQ_TR_TIGHT_WHOLE 255u
typedef struct { float a[2],b[2]; } nm_sq_tr_piece_t;   /* fractions of the face's s (a) and t (b) ranges */
typedef struct { uint16_t first; uint8_t count; uint8_t roles; } nm_sq_tr_tight_t;
/* roles: corner k of the face has s at the high end when bit 2k is set, t when bit 2k+1 is. count SQ_TR_TIGHT_WHOLE: the
 * face is drawn whole (not a parallelogram with a rectangle of UVs inside its texture, or a texture without a mask). */
static nm_sq_tr_tight_t *sq_tr_tight;
static uint16_t *sq_tr_tight_base;              /* per car mesh: its first face's entry, UINT16_MAX for no translucent mesh */
static nm_sq_tr_piece_t *sq_tr_pieces;
static uint8_t sq_tr_wide[SQ_TR_INSTANCES_MAX]; /* this frame: the instance's points were cached on the guard view */
/* Cover the set cells of a w x h grid (w, h <= 72) by at most NM_SMOKE_PIECES rectangles [x0,x1) x [y0,y1): one,
 * two slabs or three slabs across either axis, the cheapest by cells covered + NM_SMOKE_PIECE_TEXELS a rectangle.
 * Returns the count (0: no cell set). */
static unsigned sq_tr_cover(const uint8_t *cells,unsigned w,unsigned h,unsigned out[3][4]) {
    int lo[2][72],hi[2][72];                  /* per column (axis 0) / row (axis 1): first and last set cell, hi<lo when none */
    for(unsigned i=0;i<w;++i) { lo[0][i]=(int)h; hi[0][i]=-1; }
    for(unsigned j=0;j<h;++j) { lo[1][j]=(int)w; hi[1][j]=-1; }
    for(unsigned j=0;j<h;++j) for(unsigned i=0;i<w;++i) if(cells[j*w+i]) {
        if((int)j<lo[0][i]) lo[0][i]=(int)j;
        if((int)j>hi[0][i]) hi[0][i]=(int)j;
        if((int)i<lo[1][j]) lo[1][j]=(int)i;
        if((int)i>hi[1][j]) hi[1][j]=(int)i;
    }
    /* The bounding rectangle of the slab [c0,c1) along `axis` (0: columns), as x0 x1 y0 y1; its area, 0 when empty. */
#define SQ_TR_SLAB(axis,c0,c1,r) do { int first_=-1,last_=-1,low_=1<<20,high_=-1; \
        for(int c_=(int)(c0);c_<(int)(c1);++c_) if(hi[axis][c_]>=lo[axis][c_]) { if(first_<0) first_=c_; last_=c_; \
            if(lo[axis][c_]<low_) low_=lo[axis][c_]; if(hi[axis][c_]>high_) high_=hi[axis][c_]; } \
        if(first_<0) { r[0]=r[1]=r[2]=r[3]=0; } \
        else if(!(axis)) { r[0]=(unsigned)first_; r[1]=(unsigned)last_+1u; r[2]=(unsigned)low_; r[3]=(unsigned)high_+1u; } \
        else { r[0]=(unsigned)low_; r[1]=(unsigned)high_+1u; r[2]=(unsigned)first_; r[3]=(unsigned)last_+1u; } } while(0)
#define SQ_TR_COST(r) ((r[1]-r[0])*(r[3]-r[2])+((r[1]>r[0]) ? (unsigned)NM_SMOKE_PIECE_TEXELS : 0u))
    unsigned best[3][4],count=0,cost;
    SQ_TR_SLAB(0,0,w,best[0]);
    if(best[0][1]==best[0][0]) return 0;
    count=1; cost=SQ_TR_COST(best[0]);
    for(unsigned axis=0;axis<2 && NM_SMOKE_PIECES>=2;++axis) {
        const unsigned n=axis ? h : w;
        for(unsigned c=1;c<n;++c) {
            unsigned r[3][4];
            SQ_TR_SLAB(axis,0,c,r[0]); SQ_TR_SLAB(axis,c,n,r[1]);
            const unsigned k=SQ_TR_COST(r[0])+SQ_TR_COST(r[1]);
            if(k<cost) { cost=k; count=0; for(unsigned p=0;p<2;++p) if(r[p][1]>r[p][0]) nm_memcpy(best[count++],r[p],sizeof r[p]); }
            for(unsigned d=c+1;d<n && NM_SMOKE_PIECES>=3;++d) {
                SQ_TR_SLAB(axis,0,c,r[0]); SQ_TR_SLAB(axis,c,d,r[1]); SQ_TR_SLAB(axis,d,n,r[2]);
                const unsigned k3=SQ_TR_COST(r[0])+SQ_TR_COST(r[1])+SQ_TR_COST(r[2]);
                if(k3<cost) { cost=k3; count=0; for(unsigned p=0;p<3;++p) if(r[p][1]>r[p][0]) nm_memcpy(best[count++],r[p],sizeof r[p]); }
            }
        }
    }
#undef SQ_TR_SLAB
#undef SQ_TR_COST
    nm_memcpy(out,best,sizeof best);
    return count;
}
/* The pieces of every face of the translucent meshes (flags: sq_translucent_prepare's per mesh). 0: out of memory. */
static int sq_tr_tight_prepare(const nm_pack_t *pack,const uint8_t *flags) {
    const nm_header_t *original=pack->data;
    unsigned faces=0;
    for(unsigned m=0;m<original->meshes;++m) if(flags[m]) faces+=pack->meshes[m].face_count;
    sq_tr_tight_base=nm_heap_alloc("smoke tight bases",32,original->meshes*sizeof(*sq_tr_tight_base));
    sq_tr_tight=nm_heap_alloc("smoke tight faces",32,faces*sizeof(*sq_tr_tight));
    sq_tr_pieces=nm_heap_alloc("smoke tight pieces",32,faces*NM_SMOKE_PIECES*sizeof(*sq_tr_pieces));
    if(!sq_tr_tight_base || !sq_tr_tight || !sq_tr_pieces) return 0;
    /* Masks of the textures met so far (at most 8; a texture beyond that: its faces whole). */
    enum { MASKS=8, SIDE_MAX=64 };
    uint8_t (*mask)[SIDE_MAX*SIDE_MAX]=nm_alloc((MASKS+1)*SIDE_MAX*SIDE_MAX); unsigned mask_resource[MASKS],masks=0;
    if(!mask) return 0;
    uint8_t *const cells=mask[MASKS]; /* the face's grid */
    unsigned at=0,used=0,whole=0,pieces_total=0; double area_face=0,area_tight=0;
    for(unsigned m=0;m<original->meshes;++m) {
        sq_tr_tight_base[m]=flags[m] ? (uint16_t)at : UINT16_MAX;
        if(!flags[m]) continue;
        const nm_mesh_t *mesh=pack->meshes+m;
        for(unsigned f=0;f<mesh->face_count;++f) {
            nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one face a step (5 ms each on hardware) */
            nm_sq_tr_tight_t *entry=sq_tr_tight+at++;
            entry->first=(uint16_t)used; entry->count=SQ_TR_TIGHT_WHOLE; entry->roles=0;
            const nm_face_t *face=pack->faces+mesh->face_first+f;
            /* One texture and UV mapping for every light class of the material. */
            const nm_material_binding_t *binding=NULL; nm_material_uv_t uv={0}; int same=1;
            for(unsigned l=0;l<256 && same;++l) {
                const unsigned b=sq_lookup(draw_bindings,draw_runs,face->material,l);
                if(b==UINT16_MAX) continue;
                const nm_material_binding_t *candidate=draw_materials->bindings+b;
                const nm_material_uv_t candidate_uv=nm_material_uv(draw_materials,candidate);
                if(!binding) { binding=candidate; uv=candidate_uv; }
                else same=candidate->resource==binding->resource && !memcmp(&candidate_uv,&uv,sizeof uv);
            }
            if(face->count!=4 || !binding || !same || binding->resource==NM_MATERIAL_SOLID) { ++whole; continue; }
            const nm_texture_record_t *record=draw_materials->textures+binding->resource;
            const unsigned side=record->width;
            if(record->height!=side || side>SIDE_MAX) { ++whole; continue; }
            /* Texel coordinates of the corners: s = (u - origin) * scale * side. */
            float s[4],t[4]; const nm_vertex_t *vertex[4];
            for(unsigned k=0;k<4;++k) {
                vertex[k]=pack->vertices+mesh->vertex_first+face->indices[k];
                s[k]=(vertex[k]->u-uv.origin[0])*uv.scale[0]*(float)side; t[k]=(vertex[k]->v-uv.origin[1])*uv.scale[1]*(float)side;
            }
            float s0=s[0],s1=s[0],t0=t[0],t1=t[0];
            for(unsigned k=1;k<4;++k) { if(s[k]<s0) s0=s[k]; if(s[k]>s1) s1=s[k]; if(t[k]<t0) t0=t[k]; if(t[k]>t1) t1=t[k]; }
            unsigned roles=0,seen=0; int rect=s0<s1 && t0<t1 && s0>=0 && t0>=0 && s1<=(float)side && t1<=(float)side;
            for(unsigned k=0;k<4 && rect;++k) {
                const unsigned role=(s[k]==s1 ? 1u : s[k]==s0 ? 0u : 4u)|(t[k]==t1 ? 2u : t[k]==t0 ? 0u : 4u);
                if(role&4u || seen&(1u<<role)) rect=0;
                seen|=1u<<(role&3u); roles|=(role&3u)<<(2*k);
            }
            /* A parallelogram: the corners with roles 0 and 3 sum as those with roles 1 and 2 (within 1/1000 of its size). */
            if(rect) {
                const float *p[4];
                for(unsigned k=0;k<4;++k) p[(roles>>(2*k))&3u]=vertex[k]->position;
                float size=0,gap=0;
                for(unsigned a=0;a<3;++a) {
                    const float d=fabsf(p[0][a]+p[3][a]-p[1][a]-p[2][a]),e=fabsf(p[1][a]-p[0][a])+fabsf(p[2][a]-p[0][a]);
                    if(d>gap) gap=d; if(e>size) size=e;
                }
                rect=gap<=size*1e-3f;
            }
            unsigned slot=0;
            while(rect && slot<masks && mask_resource[slot]!=binding->resource) ++slot;
            if(rect && slot==masks && masks<MASKS) {
                nm_loader_yield(0); /* the texture's mask: a VRAM read of the whole texture */
                if(nm_smoke_texture_mask(binding->resource,side,mask[masks])) mask_resource[masks++]=binding->resource;
            }
            if(!rect || slot==masks) { ++whole; continue; }
            /* Cells: the texels the face's rectangle touches; set when a non-hole texel lies within one texel (v wraps:
             * the texture repeats; across u the neighbours are the transparent half). */
            const int x0=(int)floorf(s0),x1=(int)ceilf(s1),y0=(int)floorf(t0),y1=(int)ceilf(t1);
            const unsigned w=(unsigned)(x1-x0),h=(unsigned)(y1-y0);
            for(unsigned j=0;j<h;++j) {
                int row[3]; /* the wrapped rows of dy = -1, 0, 1 (once a row: the remainders are slow) */
                for(int dy=-1;dy<=1;++dy) row[dy+1]=(((y0+(int)j+dy)%(int)side)+(int)side)%(int)side;
                for(unsigned i=0;i<w;++i) {
                    uint8_t any=0;
                    for(int dy=-1;dy<=1 && !any;++dy) for(int dx=-1;dx<=1 && !any;++dx) {
                        const int x=x0+(int)i+dx,y=row[dy+1];
                        if(x>=0 && x<(int)side) any=mask[slot][y*side+(unsigned)x];
                    }
                    cells[j*w+i]=any;
                }
            }
            unsigned rects[3][4];
            const unsigned count=sq_tr_cover(cells,w,h,rects);
#ifdef SQ_TR_TIGHT_DEBUG
            printf("TIGHT mesh %u face %u resource %u box %g %g %g %g pieces %u\n",m,f,binding->resource,s0,s1,t0,t1,count);
#endif
            entry->count=(uint8_t)count; entry->roles=(uint8_t)roles;
            area_face+=(double)(s1-s0)*(t1-t0);
            for(unsigned p=0;p<count;++p) {
                const float a0=fmaxf((float)(x0+(int)rects[p][0]),s0),a1=fminf((float)(x0+(int)rects[p][1]),s1);
                const float b0=fmaxf((float)(y0+(int)rects[p][2]),t0),b1=fminf((float)(y0+(int)rects[p][3]),t1);
                area_tight+=(double)(a1-a0)*(b1-b0);
                sq_tr_pieces[used++]=(nm_sq_tr_piece_t){{(a0-s0)/(s1-s0),(a1-s0)/(s1-s0)},{(b0-t0)/(t1-t0),(b1-t0)/(t1-t0)}};
            }
            pieces_total+=count;
        }
    }
    free(mask);
    printf("Native smoke tight geometry: %u faces, %u pieces (at most %u a face, %u texels a piece), %u faces whole; "
        "texture area %.1f%% of the faces'\n",at,pieces_total,(unsigned)NM_SMOKE_PIECES,(unsigned)NM_SMOKE_PIECE_TEXELS,whole,
        area_face>0 ? 100.0*area_tight/area_face : 0.0);
    return 1;
}
static void sq_tr_tight_release(void) {
    free(sq_tr_tight_base); sq_tr_tight_base=NULL; free(sq_tr_tight); sq_tr_tight=NULL; free(sq_tr_pieces); sq_tr_pieces=NULL;
}
/* Face f of the batch in pieces: 1 drawn, 0 not (a corner outside the view: draw it whole), -1 a full stream. */
static int sq_tr_emit_tight(const nm_sq_batch_t *batch,unsigned slot,unsigned f,unsigned binding,const nm_sq_emit_t *uv,
        float depth_scale,const pvr_poly_hdr_t **previous) {
    const nm_pack_t *pack=draw_pack;
    const nm_sq_tr_tight_t *entry=sq_tr_tight+sq_tr_tight_base[batch->mesh-pack->meshes]+f;
    if(entry->count==SQ_TR_TIGHT_WHOLE || draw_untextured) return 0;
    const nm_face_t *face=pack->faces+batch->mesh->face_first+f;
    nm_cached_vertex_t held[4]; const nm_cached_vertex_t *role[4];
    for(unsigned k=0;k<4;++k) {
        const nm_cached_vertex_t *c=sq_corner(batch,0,pack,face->indices[k],held+k);
        if(nm_cached_outside(c)) return 0;
        role[(entry->roles>>(2*k))&3u]=c;
    }
    const nm_view_vertex_t o=role[0]->view;
    const float da[5]={role[1]->view.x-o.x,role[1]->view.y-o.y,role[1]->view.z-o.z,role[1]->view.u-o.u,role[1]->view.v-o.v};
    const float db[5]={role[2]->view.x-o.x,role[2]->view.y-o.y,role[2]->view.z-o.z,role[2]->view.u-o.u,role[2]->view.v-o.v};
    nm_cached_vertex_t point[3][4];
    for(unsigned p=0;p<entry->count;++p) {
        const nm_sq_tr_piece_t *piece=sq_tr_pieces+entry->first+p;
        for(unsigned k=0;k<4;++k) {
            const unsigned r=(entry->roles>>(2*k))&3u;
            const float a=piece->a[r&1u],b=piece->b[r>>1];
            const nm_view_vertex_t in={o.x+a*da[0]+b*db[0],o.y+a*da[1]+b*db[1],o.z+a*da[2]+b*db[2],o.u+a*da[3]+b*db[3],o.v+a*da[4]+b*db[4]};
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
            if(sq_tr_wide[slot]) nm_cache_vertex_wide(&batch->view,&in,point[p]+k,0); else
#endif
            nm_cache_vertex_classified(&batch->view,&in,point[p]+k,0);
            if(nm_cached_outside(point[p]+k) || point[p][k].q<=NM_SKY_DEPTH) return 0;
        }
    }
    const pvr_poly_hdr_t *selected=uv->header;
    for(unsigned p=0;p<entry->count;++p) {
        const int header_changed=*previous!=selected;
        if(!draw_ta_fits(SQ_TA_UNITS(4),draw_header_count+header_changed)) {
            printf("FAIL SQ TA budget: triangles=%u vertices=%u headers=%u next-header=%u source=0\n",
                draw_count,sq_ta_vertices,draw_header_count,header_changed); return -1;
        }
        if(draw_count+2>NM_DRAW_TRIANGLES) {
            printf("FAIL SQ triangle capacity=%u TA-bytes=%u source=0\n",draw_count,sq_ta_vertices*32+draw_header_count*32); return -1;
        }
        sq_ta_vertices+=4;
        if(header_changed) {
            pvr_poly_hdr_t *header=pvr_dr_target();
            *header=*selected;
            pvr_dr_commit(header);
            *previous=selected; ++draw_header_count;
        }
        /* The face's strip order (sq_emit_face_general: fan (0,1,2),(0,2,3) as strip (1,2,0,3)). */
        static const uint8_t strip[4]={1,2,0,3};
        SQ_TIGHT_PIECE();
        for(unsigned k=0;k<4;++k) {
            const nm_cached_vertex_t *c=point[p]+strip[k];
            pvr_vertex_t *out=pvr_dr_target();
            out->flags=k==3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            out->x=c->x*(640.0f/496.0f)*ENJ_XSCALE; out->y=c->y*(480.0f/384.0f); out->z=sq_door_q*depth_scale;
            out->u=(c->view.u-uv->origin[0])*uv->scale[0]; out->v=(c->view.v-uv->origin[1])*uv->scale[1];
            out->u=0.5f*out->u+0.5f*(out->x+out->y)-sq_door_base;
            SQ_VERTEX_TAG(batch->mesh->face_first+f,binding); SQ_CLIP_TAG(1);
            out->argb=uv->argb; out->oargb=0;
            pvr_dr_commit(out);
        }
        draw_count+=2;
        SQ_COUNT(draw_cutout,2);
    }
    return 1;
}
#endif
static int sq_translucent_prepare(const nm_pack_t *pack) {
    const nm_header_t *original=pack->data;
    free(sq_tr_meshes); sq_tr_meshes=NULL; free(sq_tr_points); sq_tr_points=NULL; sq_tr_count=0;
#if NM_SMOKE_DOOR && NM_SMOKE_TIGHT
    sq_tr_tight_release();
#endif
    unsigned meshes=0,faces=0,largest=0;
    uint8_t *flags=nm_calloc(original->meshes ? original->meshes : 1u,1);
    if(!flags) return 0;
    for(unsigned m=0;m<original->meshes;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h): one step */
        const nm_mesh_t *mesh=pack->meshes+m;
        int all=mesh->face_count!=0;
        for(unsigned f=0;all && f<mesh->face_count;++f) {
            const nm_face_t *face=pack->faces+mesh->face_first+f;
            all=(face->attribute&(3u<<8))!=0 && nm_material_translucent(pack->materials[face->material].header);
        }
        if(!all) continue;
        flags[m]=1; ++meshes; faces+=mesh->face_count;
        if(mesh->vertex_count>largest) largest=mesh->vertex_count;
    }
    if(!meshes) { free(flags); puts("Native translucent effects: none in the car pack"); return 1; }
#if NM_SMOKE_DOOR && NM_SMOKE_TIGHT
    if(!sq_tr_tight_prepare(pack,flags)) { free(flags); return 0; }
#endif
    sq_tr_meshes=flags; sq_tr_mesh_vertices=largest;
    sq_tr_point_capacity=SQ_TR_INSTANCES_MAX*largest;
    sq_tr_points=nm_heap_alloc("translucent effect points",32,sq_tr_point_capacity*sizeof(*sq_tr_points));
    if(!sq_tr_points) return 0;
    printf("Native translucent effects: %u car meshes, %u faces of the translucent class (checker); at most %u "
        "instances and %u faces a frame, %s\n",meshes,faces,SQ_TR_INSTANCES_MAX,SQ_TR_FACES_MAX,
        NM_SMOKE_MODE==1 ? "punch-through, a checker of texels (NATIVE_SMOKE_MODE=pt-tex), not sorted" :
        NM_SMOKE_MODE==2 ? "punch-through, a checker of screen pixels (NATIVE_SMOKE_MODE=pt-screen), not sorted" :
        NM_SMOKE_MODE==3 ? "translucent, a checker of screen pixels (NATIVE_SMOKE_MODE=tr-screen), pre-sorted back to front" :
        "pre-sorted back to front");
    return 1;
}
static void sq_translucent_release(void) {
    free(sq_tr_meshes); sq_tr_meshes=NULL; free(sq_tr_points); sq_tr_points=NULL; sq_tr_count=0;
#if NM_SMOKE_DOOR && NM_SMOKE_TIGHT
    sq_tr_tight_release();
#endif
}
#ifndef SQ_TR_COUNT
#define SQ_TR_COUNT(faces,instances,vertices) ((void)0) /* host tools: the frame's translucent faces, instances, vertices */
#endif
typedef struct { float key; uint16_t face; uint8_t slot; uint8_t rear; uint16_t binding; } nm_sq_tr_face_t;
static nm_sq_batch_t sq_tr_batches[SQ_TR_INSTANCES_MAX];
static int sq_translucent_emit(void) {
    const nm_pack_t *pack=draw_pack;
    static nm_sq_tr_face_t order[SQ_TR_FACES_MAX];
    unsigned count=0,points_used=0;
    for(unsigned q=0;q<sq_tr_count;++q) {
        const nm_instance_t *inst=pack->instances+sq_tr_queue[q].instance;
        const nm_mesh_t *mesh=pack->meshes+inst->mesh;
        if(mesh->vertex_count>sq_tr_point_capacity-points_used) return 0;
        nm_sq_batch_t *batch=sq_tr_batches+q;
        nm_memset(batch,0,sizeof(*batch));
        batch->mesh=mesh; batch->view=sq_tr_queue[q].view; batch->points=sq_tr_points+points_used;
        batch->instance_index=sq_tr_queue[q].instance; batch->source=0; batch->bounds=sq_tr_queue[q].bounds;
        points_used+=mesh->vertex_count;
        const nm_view_t view=batch->view;
#if NM_SMOKE_DOOR && NM_SMOKE_TIGHT
        sq_tr_wide[q]=0;
#endif
        shz_xmtrx_load_3x4(&inst->transform);
#if defined(__DREAMCAST__) || defined(NM_HOST_BAKED_WORLD)
        /* A whole-screen view: classified against the guard view, cut at the near plane and the band only (the
         * PVR clips at the screen edges), as every other batch of the frame (sq_prepare_batch, wide). */
        if(sq_batch_bounded(inst,mesh,&view) && sq_view_is_screen(&view)) {
            nm_view_t guard=view;
            guard.bounds[0]-=SQ_GUARD_BAND; guard.bounds[1]+=SQ_GUARD_BAND;
            guard.bounds[2]-=SQ_GUARD_BAND; guard.bounds[3]+=SQ_GUARD_BAND;
            batch->view=guard;
            for(unsigned v=0;v<mesh->vertex_count;++v)
                cache_draw_vertex_wide(pack,mesh,inst,&guard,v,batch->points+v,batch->bounds==2);
#if NM_SMOKE_DOOR && NM_SMOKE_TIGHT
            sq_tr_wide[q]=1;
#endif
        } else
#endif
        for(unsigned v=0;v<mesh->vertex_count;++v)
            cache_draw_vertex(pack,mesh,inst,&view,v,batch->points+v,batch->bounds==2);
        for(unsigned f=0;f<mesh->face_count;++f) {
            const nm_face_t *face=pack->faces+mesh->face_first+f;
            unsigned common=127u;
            float nearest=FLT_MAX;
            for(unsigned v=0;v<face->count;++v) {
                const nm_cached_vertex_t *p=batch->points+face->indices[v];
                const unsigned outside=nm_cached_outside(p);
                common&=outside;
                if(!outside && p->q<=NM_SKY_DEPTH) common=127u; /* beyond the sky: never drawn */
                if(p->view.z<nearest) nearest=p->view.z;
            }
            if(common) continue;                                  /* every corner beyond one plane */
            const int light=face_light(pack,mesh,inst,face,1);
            if(light<0) return 0;
            if((light&256) && !(face->attribute&(1u<<17))) continue; /* rear of a one-sided face */
            const unsigned binding=sq_lookup(draw_bindings,draw_runs,face->material,(unsigned)light&255u);
            if(binding==UINT16_MAX || count==SQ_TR_FACES_MAX) return 0;
            /* Back to front: a larger nearest depth first; equal keys keep their order. The punch-through forms
             * (NM_SMOKE_MODE) write depth: the nearest polygon wins its pixels in any order, no sort. */
            unsigned at=count++;
#if !NM_SMOKE_PUNCH
            while(at && order[at-1].key<nearest) { order[at]=order[at-1]; --at; }
#endif
            order[at]=(nm_sq_tr_face_t){nearest,(uint16_t)f,(uint8_t)q,(uint8_t)((light&256)!=0),(uint16_t)binding};
        }
    }
    SQ_TR_COUNT(count,sq_tr_count,points_used);
    if(!draw_header_count) sq_ta_vertices=0;
    const pvr_poly_hdr_t *previous=NULL;
    for(unsigned i=0;i<count;++i) {
        const nm_sq_tr_face_t *o=order+i;
        const nm_sq_batch_t *batch=sq_tr_batches+o->slot;
        const nm_face_t *face=pack->faces+batch->mesh->face_first+o->face;
        nm_sq_emit_t emit;
        if(sq_emit_records[0]) sq_emit_expand(&emit,sq_emit_records[0]+o->binding); else sq_emit_lookup(&emit,0,o->binding);
        const float depth_scale=emit.depth*draw_layer_scale[face->layer];   /* draw_layer_scale[0] is 1 */
#if NM_SMOKE_DOOR
        /* pt-screen: one 1/w for the whole face, that of its nearest corner (the sort key; the near plane's for a
         * face cut by it), so that the PVR interpolates u linearly on the screen; and an integer near half the
         * smallest x + y of its corners on the screen, which keeps u small (whole texture widths change nothing). */
        sq_door_q=1.0f/(o->key>batch->view.near_z ? o->key : batch->view.near_z);
        float low=FLT_MAX;
        for(unsigned v=0;v<face->count;++v) {
            const nm_cached_vertex_t *p=batch->points+face->indices[v];
            const float sum=p->x*(640.0f/496.0f)*ENJ_XSCALE+p->y*(480.0f/384.0f);
            if(!nm_cached_outside(p) && sum<low) low=sum;
        }
        sq_door_base=low<FLT_MAX ? floorf(0.5f*low) : 0.0f;
#endif
#if NM_SMOKE_DOOR && NM_SMOKE_TIGHT
        /* Tight geometry: the face's pieces, or the face whole when a corner lies outside the view. */
        const int tight=sq_tr_emit_tight(batch,o->slot,o->face,o->binding,&emit,depth_scale,&previous);
        const int emitted=tight<0 ? 0 : tight ? 1 :
            sq_emit_face_general(SQ_TR_LIST,0,batch,o->face,o->binding,&emit,depth_scale,emit.header,&previous,0);
#else
        const int emitted=sq_emit_face_general(SQ_TR_LIST,0,batch,o->face,o->binding,&emit,depth_scale,emit.header,&previous,0);
#endif
#if NM_SMOKE_DOOR
        sq_door_q=0.0f;
#endif
        if(!emitted) return 0;
    }
    return 1;
}
static void sq_submit_translucent(void *data) {
    (void)data;
#ifdef NM_DIAG_FRAME_TRACE
    const uint32_t ft_start=nm_ft_clock();
#endif
    if(!sq_failed && !sq_translucent_emit()) { puts("FAIL: translucent effects SQ emit; stopping on next update"); sq_failed=1; }
#ifdef NM_DIAG_FRAME_TRACE
    nm_ft_group[NM_FT_TRANSLUCENT]+=nm_ft_delta(ft_start,nm_ft_clock());++nm_ft_calls[NM_FT_TRANSLUCENT];
#endif
}

/* Borrowed visible opponents, in detail order; reset only before queueing.
 * 44 is the authored scene limit, not a per-polygon callback pool. */
static struct {
    nm_sq_batch_t *objects[44];
    unsigned count;
} sq_opponents[2];
static void sq_submit_opponents(void *data) {
    const unsigned pass=(unsigned *)data-draw_lists;
#ifdef NM_DIAG_FRAME_TRACE
    sq_ft_opponent=1;nm_ft_opponents=sq_opponents[0].count;
#endif
    for(unsigned i=0;!sq_failed && i<sq_opponents[pass].count;++i)
        sq_submit_object(sq_opponents[pass].objects[i],draw_lists[pass]);
#ifdef NM_DIAG_FRAME_TRACE
    sq_ft_opponent=0;
#endif
}

static int sq_queue_objects(const unsigned *car_parts,unsigned cars,unsigned player) {
    if(cars>44 || player>cars || (!car_parts && cars)) return 0;
    sq_opponents[0].count=sq_opponents[1].count=0;
    if(car_parts) {
        if(car_parts[0] || car_parts[cars]!=draw_pack->header->instances) return 0;
        for(unsigned i=0;i<cars;++i) if(car_parts[i]>car_parts[i+1]) return 0;
    }
    for(unsigned s=0;s<(world_draw.pack ? 2u : 1u);++s) {
        unsigned car=0;
        for(unsigned first=0;first<sq_sources[s].count;) {
            nm_sq_batch_t *object=sq_sources[s].batches+first;
            unsigned end=first+1;
            if(!s) {
                if(car_parts) {
                    while(car<cars && object->instance_index>=car_parts[car+1]) ++car;
                    if(car==cars) return 0;
                    while(end<sq_sources[s].count &&
                          sq_sources[s].batches[end].instance_index<car_parts[car+1]) ++end;
                } else end=sq_sources[s].count; /* Single-player car and all its parts. */
            }
            object->object_batches=end-first;
            unsigned opaque=0,cutout=0;
            for(unsigned b=first;b<end;++b) {
                const nm_sq_batch_t *batch=sq_sources[s].batches+b;
                /* Preserve PT ordering without touching faces at enqueue.
                 * An empty list callback emits nothing; OP prepares both once. */
                opaque|=batch->opaque_count | !batch->prepared;
                cutout|=batch->cutout_count | !batch->prepared;
            }
            if(!s && car_parts && car!=player) {
                if(opaque) sq_opponents[0].objects[sq_opponents[0].count++]=object;
                if(cutout) sq_opponents[1].objects[sq_opponents[1].count++]=object;
            } else {
                if(opaque) enj_render_list_add(PVR_LIST_OP_POLY,sq_submit_object_op,object);
                if(cutout) enj_render_list_add(PVR_LIST_PT_POLY,sq_submit_object_pt,object);
            }
            first=end;
        }
        if(!s) for(unsigned pass=0;pass<2;++pass)
            if(sq_opponents[pass].count)
                enj_render_list_add(draw_lists[pass],sq_submit_opponents,(void *)(draw_lists+pass));
    }
    if(sq_tr_count) enj_render_list_add(SQ_TR_LIST,sq_submit_translucent,NULL);
    return 1;
}
