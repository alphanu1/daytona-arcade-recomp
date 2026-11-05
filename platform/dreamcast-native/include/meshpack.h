#ifndef DAYTONA_NATIVE_MESHPACK_H
#define DAYTONA_NATIVE_MESHPACK_H
#include <stdint.h>
#ifdef __DREAMCAST__
#include <sh4zam/shz_matrix.h>
#endif

/* DNM1/2/3/4/5/6, little-endian IEEE float32. Fixed tables in this order.
 * No ROM bus, strip decoding or pointer fixups on the console. */
/* ponytail: one startup-resident pack; split/stream measured sectors before
 * exceeding this guard. It is not an accepted full-course RAM budget (the
 * heap is: tools/heap_budget.py). 3 MiB since Expert, whose world pack is
 * about 2.3 MB: every table offset and count of the format is 32-bit, face
 * groups stay below 65536 (face.group), so the format has no 2 MiB bound. */
#define NM_PACK_MAX_BYTES (3u * 1024u * 1024u)
#define NM_PACK_MAGIC 0x314d4e44u
#define NM_PACK_MAGIC_SHARED 0x324d4e44u
#define NM_PACK_MAGIC_GROUPED 0x334d4e44u
#define NM_PACK_MAGIC_NORMAL_GROUPED 0x344d4e44u
#define NM_PACK_MAGIC_STRIPS 0x354d4e44u
#define NM_PACK_MAGIC_LAYERED 0x364d4e44u
/* Explicit layout variant: zero-pad before EVERY table and at EOF to 32 bytes.
 * Record strides/bytes are unchanged; legacy packs retain contiguous tables.
 * This aligns 32/64-byte records, not each packed 16/112-byte record. */
#define NM_PACK_ALIGNED 0x80000000u
/* DNM3 retains DNM2 tables, pads position refs to four bytes with zeroes,
 * then appends header.reserved group bounds. mesh.padding[0:2] is its group
 * first/count; face.group selects an absolute group. All other padding stays
 * zero. Face/vertex/instance order is unchanged; bounds enclose every member. */
typedef struct { float bounds[6]; } nm_face_group_t;
/* DNM4 keeps DNM3's conservative spatial bounds and adds an offline normal
 * cone plus the p2-point sphere of the supplied-normal facing test. The
 * renderer uses the bounds only: it decides each face's side from the face's
 * own plane (model_submission.h, sq_face_side) and no longer evaluates the
 * cone. The fields stay in the format. */
typedef struct {
    float bounds[6];
    float normal_center[3], normal_radius;
    float point_center[3], point_radius;
    uint32_t flags;
    /* Serialized as zero. */
    float normal_length;
} nm_normal_group_t;
/* Bit 0 says the offline normal/point bounds form a conservative cone.  The
 * same cone can prove a group wholly front- or back-facing at runtime. */
#define NM_NORMAL_GROUP_CONE 1u
/* DNM5 retains every DNM4 table, zero-pads after normal_groups to 32 bytes,
 * then appends strips and their contiguous local uint16 mesh vertex indices.
 * Header.padding[0:2] holds the nonzero strip/index counts; [2:4] stays zero.
 * Mesh.padding[0:2] retains groups; [2:4] holds strip first/count, [4] is zero.
 * Records follow mesh and nonoverlapping source-face order; even a mesh without
 * strips records the current strip cursor. Final file padding to 4 bytes is zero.
 * Each strip covers at least two consecutive faces of one material, preserving
 * their ordered fan triangles and winding by complete XYZ+UV vertex bytes.
 * Vertex count is 2 + sum(face.count - 2), at most 65535; record padding is zero. */
/* DNM6 is the Dreamcast draw order. tools/prepare_face_order.py sorts the
 * faces of each mesh once, offline, by PVR list, material and layer (checker
 * faces last) and chains edge-adjacent faces of one group into strips; the
 * arcade's submission order is gone. What that order used to decide is kept
 * as face.layer: 0 for a base face, n+1 for a face that lies in the plane of
 * and overlaps a face of layer n which the arcade drew before it (same mesh,
 * or an earlier part of the same car drawn with the same matrix). The
 * renderer multiplies the written depth by 1+layer*NM_LAYER_DEPTH_STEP.
 * Layout: the DNM2 tables, zero padding to 4 bytes, then each optional DNM4/5
 * table exactly as in DNM5 when its count is nonzero: header.reserved normal
 * groups; header.padding[0:2] strips/indices (both zero or both nonzero,
 * preceded by zero padding to 32 bytes). header.padding[2] is the number of
 * layers in use (1..255) and every face.layer is below it; [3] stays zero.
 * Mesh.padding is as in DNM5, zero where a table is absent. A strip covers
 * faces of one material AND one layer. A DNM6 strip triangle may have either
 * winding (every header the renderer compiles has culling off, so a triangle
 * is its three vertices; tools/prepare_restrip.py chains faces that way); the
 * triangles still follow face order and each face's fan order.
 * Final file padding to 4 bytes.
 * Vertices, materials, instances, position refs and group records keep their
 * DNM2/4 bytes and order; only the face table is permuted.
 * header.padding[3] holds the optional flags of static scenery packs
 * (tools/prepare_face_order.py --static-light / --scenery-classes):
 * NM_PACK_BAKED_LIGHT: the scenery does not move relative to its light, so
 *   each face carries its constant luma: face.reserved for the side its
 *   supplied normal points to (facing >= 0), face.padding for the other side
 *   of a two-sided face (zero for one-sided faces). Computed offline with the
 *   reference arithmetic of lighting.h; the renderer evaluates no light.
 * NM_PACK_SCENERY_CLASSES: material.padding[0] is the scenery stage that
 *   first draws the material (0 reduced view: road, kerbs, barriers; 1 other
 *   opaque faces; 2 opaque ground; 3 punch-through cutouts; 4 screen-checker
 *   panels), plus NM_SCENERY_SURFACE when its faces take the road-surface
 *   depth class (render/models/model_renderer.h, NM_ROAD_DEPTH_SCALE), plus
 *   NM_SCENERY_POINT for the opaque material of a split cutout
 *   (tools/split_cutouts.py): its faces are the wholly opaque parts of
 *   punch-through faces whose other parts are still drawn on the
 *   punch-through list, so it is point sampled as that list is and keeps
 *   the stage of the cutout it came from.
 *   Stage 5 (NM_SCENERY_STAGE_OBJECTS) is not a material rule: it marks the
 *   materials of the world objects (NM_PACK_WORLD_OBJECTS). material.padding[1]
 *   is zero, or for a textured world-object material the bits of a positive
 *   float, its super-sample switch depth (nm_material_supersample_depth).
 * NM_PACK_LOD_GROUPS (tools/prepare_face_order.py --lod-groups): level of
 *   detail for scenery cutouts. Bits 16..31 of the same word hold the number
 *   of nm_lod_group_t records (1..65535; zero without the flag). The table
 *   is the last one of the file: after the strip indices (or whatever table
 *   is last), zero padding to 4 bytes (32 in an aligned pack), then the
 *   records, then the usual final padding. mesh.padding[4] is the mesh's
 *   first record in its low half and its record count in its high half
 *   (zero for a mesh without groups; a pack without the flag keeps the word
 *   zero); records follow mesh order and, inside a mesh, face order.
 *   A group is one source polygon drawn as one of up to three alternative
 *   shapes, all in the source polygon's plane: `levels` contiguous face
 *   ranges, level 0 (nearest) first, starting at face_first. A range may be
 *   empty: that level draws nothing. The renderer draws exactly one range
 *   per group and frame: level n+1 instead of level n beyond the view-space
 *   depth switch_depth[n] of `centre` (mesh space), with hysteresis; the
 *   depths are authored for a vertical focus of NM_LOD_REFERENCE_FOCUS and
 *   scale with the instance's focus[1]. Unused depths and counts are zero;
 *   used depths are positive and do not decrease. Faces outside every group
 *   are always drawn. All faces of a group carry the same face.group (one
 *   bounds test decides the group), and an offline strip lies inside one
 *   range or outside every group. Layers: each range is layered on its own
 *   against the faces outside the group (two ranges of one group are never
 *   drawn together and have no order between them).
 * NM_PACK_WORLD_OBJECTS (tools/prepare_models.py --world-objects): the
 *   instances behind those of the scenery cells are the arcade's world
 *   objects (the actors of its per-course table 236940, routine 2002c),
 *   already placed in world coordinates and baked like the cells. Their
 *   instance.padding[0] is NM_OBJECT_INSTANCE plus: the scenery cell of the
 *   actor's position (16e10); NM_OBJECT_NEAR when the actor tests the near
 *   bitmap 501540 instead of the draw bitmap 501520; a selector kind and a
 *   variant: the instance is drawn only while the scene's value for that
 *   kind equals the variant (kind 0: always). Object instances are
 *   contiguous, follow every cell instance, and their materials take
 *   scenery stage NM_SCENERY_STAGE_OBJECTS. Without the flag every instance
 *   padding word is zero. */
/* NM_PACK_LIVE_SPECULAR (tools/prepare_face_order.py --live-specular), only
 *   with NM_PACK_BAKED_LIGHT: some faces have an effective specular term
 *   (mode 1, material specular control and scale non-zero, ambient below
 *   255): it reads the view-space z of the normal, so it follows the camera's
 *   turn. Their face.reserved/padding hold the lumas without it; the renderer
 *   adds it every frame (model_submission.h, sq_live_specular). */
#define NM_PACK_LIVE_SPECULAR 16u
#define NM_PACK_BAKED_LIGHT 1u
#define NM_PACK_SCENERY_CLASSES 2u
#define NM_PACK_LOD_GROUPS 4u
#define NM_PACK_WORLD_OBJECTS 8u
#define NM_OBJECT_INSTANCE 0x80000000u
#define NM_OBJECT_CELL(word) ((word)&0xffu)
#define NM_OBJECT_NEAR 0x100u
#define NM_OBJECT_KIND(word) (((word)>>12)&15u)
#define NM_OBJECT_VARIANT(word) (((word)>>16)&0xffu)
/* Selector kinds. BOARD: the gantry board of 221d24 (0 "CHECK", 1 "GOAL",
 * 2 "START"). BEARING: one of 64 models by the camera's bearing from a
 * fixed point (223b28). The animated actors (presentation only), whose
 * state is a function of the scene's ticks: TURN, the near ticks n of the
 * instance's cell this scene (nm_intro_scene_t.object_near_ticks) modulo
 * 256: the die 220be4 (heading 26031+256n) and the symbol wheel 220b14
 * (rotation about Z 256n), 256 poses each; CREW, (n mod 25)/8: the pit
 * crews 220d44 (lists A, B, C, B); SCREEN, (5010b0>>3)&3 with 5010b0 the
 * original's tick counter: the video screen 22024c; RANK..RANK+4, the spawn
 * slot of the car in race position 1..5 (ranking 503080): the panels of the
 * leader board 221dd8 (list 238dfc[slot]); SLOT..SLOT+2, reel k of the
 * slot machine 2204fc (nm_slot_machine_t.reel[k], 0..64; list 2385f8[64]
 * is list 0, so 64 draws variant 0). The instances of one cell, near flag
 * and kind are contiguous with ascending variants (a selection run). */
#define NM_OBJECT_ALWAYS 0u
#define NM_OBJECT_BOARD 1u
#define NM_OBJECT_BEARING 2u
#define NM_OBJECT_TURN 3u
#define NM_OBJECT_CREW 4u
#define NM_OBJECT_SCREEN 5u
#define NM_OBJECT_RANK 6u
#define NM_OBJECT_RANKS 5u
#define NM_OBJECT_SLOT 11u
#define NM_OBJECT_SLOTS 3u
/* CONE: the props 223114 (include/props.h), one PLACED instance a cone at its rest pose, variant its
 * task index (id - 1). Drawn as the props' last tick drew it: knocked over and moving, at rest when its
 * cell is in the draw bitmap; without prop state (an older race-start header) at rest by its cell. */
#define NM_OBJECT_CONE 14u
#define NM_OBJECT_KINDS 15u /* the selector kinds; the kind field holds 16 */
/* CLOCK: a frame-counter animation of Advanced or Expert (include/clock_actors.h): one instance a group, its base
 * mesh in world coordinates, variant its place in the selection run (222090's two alternatives). The frame it
 * draws and its condition come from the course's DNA1 file, which rewrites the mesh's positions, normals, u, v and
 * bindings; the selector loop of the other kinds skips it (it is not below NM_OBJECT_KINDS). With PLACED (a PLACED
 * group of the file: Expert's birds, horses, statue) the mesh is in model coordinates and the actor's tick
 * (include/expert_actors.h) gives the placement each frame; instance.matrix holds a rest placement. */
#define NM_OBJECT_CLOCK 15u
#define NM_OBJECT_RUN(word) ((word)&0xf1ffu) /* cell, near flag and kind */
/* Placement flags. PLACED: the instance's mesh is in model coordinates and
 * instance.matrix is its placement in the world (column-major 3x4); it is
 * drawn with the view matrix times that placement (the others are baked in
 * world coordinates and drawn with the view matrix itself). SPIN, with
 * PLACED: times a rotation about Z by 256n (1/65536 turn; Rz of the
 * original's command 1616, [[c,s,0],[-s,c,0],[0,0,1]] applied to model
 * points), n the near ticks of the instance's cell (the symbol wheel 220b14).
 * MESHES: the instance stands for NM_OBJECT_MESH_COUNT consecutive meshes
 * from instance.mesh and draws mesh instance.mesh+v for its kind's value v
 * (nothing when v is not below the count): the die's 256 baked poses (TURN),
 * the leader board's numbers (RANK k, five PLACED instances sharing them),
 * the slot machine's 64 reel models (SLOT k, three PLACED instances). */
#define NM_OBJECT_PLACED 0x200u
#define NM_OBJECT_SPIN 0x400u
#define NM_OBJECT_MESHES 0x800u
#define NM_OBJECT_MESH_COUNT(word) (NM_OBJECT_VARIANT(word)+1u)
#define NM_PACK_LOD_COUNT_SHIFT 16
#define NM_LOD_LEVELS 3u
#define NM_LOD_REFERENCE_FOCUS 280.0f
/* The renderer's hysteresis: a group at level n goes to n+1 when the scaled
 * depth exceeds switch_depth[n]*NM_LOD_HYSTERESIS and back to n when it falls
 * below switch_depth[n]/NM_LOD_HYSTERESIS. */
#define NM_LOD_HYSTERESIS 1.1f
typedef struct {
    uint32_t face_first;                /* absolute, as nm_strip_t.face_first */
    uint16_t face_count[NM_LOD_LEVELS]; /* consecutive ranges, level 0 first */
    uint8_t levels, spare;              /* 1..3 levels; zero */
    float centre[3];
    float switch_depth[NM_LOD_LEVELS-1];
} nm_lod_group_t;
#define NM_SCENERY_STAGE_MASK 0xffu
#define NM_SCENERY_STAGE_LAST 5u
#define NM_SCENERY_STAGE_CHECKER 4u
#define NM_SCENERY_STAGE_OBJECTS 5u
#define NM_SCENERY_SURFACE 0x100u
#define NM_SCENERY_POINT 0x200u
/* Relative depth step per DNM6 face layer: written depth = 1/z times
 * (1+layer*step), composed with the road-surface scale (and therefore with
 * the shadow volumes, which share that scale). Host measurement
 * (640x480 software depth buffer with one float32 plane equation per
 * triangle, 129 frames of both scripted races and the attract scene plus 352
 * frames of every car body at view depths 10..255 in 16 poses, 4.2 million
 * layered pixels, 1,458 of them beyond depth 100):
 * - lower bound: a face authored in the plane of its base loses pixels to it
 *   at step 1e-5 (12 pixels) and none from 3.1e-5 up. 1/z interpolation
 *   error does not grow with distance; at the far end of the car range
 *   (z 256) the step is 3.8e-6 in depth, 16,000 float32 steps. 2^-10 keeps a
 *   factor 32 in hand for the PowerVR's own set-up precision and the FSRRA
 *   reciprocal (2^-20 relative), neither of which the host model has.
 * - upper bound: the step moves a layered face step*z toward the eye (0.25
 *   units at z 256, 4 mm at the follow camera). Where such a face passes
 *   behind another, their visible boundary shifts by about step*focus/tan
 *   of their angle: a quarter pixel at 45 degrees, at any distance (188
 *   pixels in the 129 frames; 49 at 2^-12, 679 at 2^-8). No road decal ever
 *   won over a car in the sampled frames: against the road-surface scale of
 *   0.5 the step changes a car's margin by one part in a thousand.
 * It cannot lift a decal that lies BELOW its base: see same_corner_dips in
 * tools/prepare_face_order.py. */
#ifndef NM_LAYER_DEPTH_STEP
#define NM_LAYER_DEPTH_STEP (1.0f/1024.0f)
#endif
typedef struct {
    uint32_t face_first; /* Absolute source face index. */
    uint16_t face_count, vertex_count;
    uint32_t vertex_first; /* Offset into the appended strip indices. */
    uint32_t padding[5];
} nm_strip_t;
typedef struct {
    uint32_t magic, size, meshes, vertices, faces, materials, instances, reserved;
    int32_t crtc[2], render[2];
    uint32_t padding[4];
} nm_header_t;
typedef struct {
    uint32_t vertex_first, vertex_count, face_first, face_count;
    float bounds[6];
    uint32_t mode, padding[5];
} nm_mesh_t;
typedef struct { float position[3]; uint16_t u, v; } nm_vertex_t;
typedef struct {
    uint16_t indices[4], material;
    /* Zero in serialized packs, except NM_PACK_BAKED_LIGHT packs (front
     * luma). For other world packs the renderer fills it once after loading
     * with the static one-sided diffuse luma. */
    uint8_t count, reserved;
    uint32_t attribute;
    float normal[3];
    /* Absolute face group (DNM3+, else zero). Shares the former 32-bit
     * padding word: the validator keeps groups below 65536. */
    uint16_t group;
    /* DNM6 coplanar draw layer; zero in every earlier format. padding is
     * zero, except in NM_PACK_BAKED_LIGHT packs: back luma of a two-sided face. */
    uint8_t layer, padding;
} nm_face_t;
typedef struct {
    uint16_t header[4];
    float diffuse, ambient, specular_scale;
    uint32_t specular_control, padding[2];
} nm_material_t;
/* Super-sample switch depth of a world-object material (tools/lod_groups.py, a
 * "supersample" rule with "group": "instance"; material.padding[1], 0 none):
 * an instance of a PLACED world object whose mesh uses it is drawn with the
 * TSP super-sample bit on its headers while the view-space depth of its model
 * origin times NM_LOD_REFERENCE_FOCUS over the focus is beyond it, with the
 * hysteresis of the level-of-detail groups (NM_LOD_HYSTERESIS;
 * render/models/model_submission.h, sq_ss_select). Every face of such a mesh
 * has the same depth (code/meshpack.c). The slot machine's reels: their 64
 * meshes keep one shape and one set of materials at every distance. */
static inline float nm_material_supersample_depth(const nm_material_t *material) {
    union { uint32_t word; float depth; } bits = { material->padding[1] };
    return bits.depth;
}
typedef struct {
    uint32_t mesh, source_address, window, reverse;
    union {
        float matrix[12]; /* Serialized column-major DNM matrix. */
#ifdef __DREAMCAST__
        shz_mat3x4_t transform; /* Same bytes, direct XMTRX load. */
#endif
    };
    float focus[2], light[3];
    int16_t viewport[4], center[2];
    uint32_t padding[4];
} nm_instance_t;
typedef struct {
    void *data;
    const nm_header_t *header;
    const nm_mesh_t *meshes;
    const nm_vertex_t *vertices;
    const nm_face_t *faces;
    const nm_material_t *materials;
    const nm_instance_t *instances;
    /* DNM2 appends one local uint16 position representative per vertex.
     * Earlier/self, bit-identical XYZ, no chains. UVs remain per vertex. */
    const uint16_t *position_refs;
    const nm_face_group_t *groups;
    const nm_normal_group_t *normal_groups;
    const nm_strip_t *strips;
    const uint16_t *strip_indices;
    const nm_lod_group_t *lod_groups; /* NM_PACK_LOD_GROUPS, else NULL */
} nm_pack_t;

/* The flag word of a loaded DNM6 pack, NM_PACK_* (the header
 * pointer may be a live copy; data is the validated file). */
static inline unsigned nm_pack_flags(const nm_pack_t *pack) {
    const nm_header_t *original=(const nm_header_t *)pack->data;
    return (original->magic&~NM_PACK_ALIGNED)==NM_PACK_MAGIC_LAYERED ? original->padding[3] : 0;
}
/* Records of the level-of-detail table (zero without NM_PACK_LOD_GROUPS). */
static inline unsigned nm_pack_lod_count(const nm_pack_t *pack) {
    return pack->lod_groups ? nm_pack_flags(pack)>>NM_PACK_LOD_COUNT_SHIFT : 0;
}
static inline int nm_pack_has_groups(const nm_pack_t *pack) {
    return pack->groups || pack->normal_groups;
}
static inline const float *nm_pack_group_bounds(const nm_pack_t *pack, unsigned group) {
    return pack->normal_groups ? pack->normal_groups[group].bounds : pack->groups[group].bounds;
}

/* Initialize to zero. Failed loads leave it unchanged; unload is idempotent. */
int nm_pack_load(nm_pack_t *pack, const char *path);
void nm_pack_unload(nm_pack_t *pack);
#endif
