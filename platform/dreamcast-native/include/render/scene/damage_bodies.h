#ifndef DAYTONA_NATIVE_DAMAGE_BODIES_H
#define DAYTONA_NATIVE_DAMAGE_BODIES_H
/* The race player's damaged bodies (car-damage.dnb, tools/prepare_damage_bodies.py): the 16 bodies
 * crash_model[0][d] of the automatic car and the clean body 95ff9d as position, supplied-normal and bounds
 * variants of the clean body's mesh of car.dnm (same faces, materials and u,v in its final order). The scene
 * writes the variant the player's draw takes into that mesh (nm_damage_body_apply) and has the renderer
 * refresh its tables (nm_render_car_mesh_refresh): one mesh's tables instead of sixteen more meshes. */
#include "meshpack.h"
#include <stdint.h>

enum { NM_DAMAGE_BODY_CLEAN = 16, NM_DAMAGE_BODIES = 17 };
/* The hoods of the hood view (car-hoods.dnb, the same format: tools/prepare_damage_bodies.py --hoods): the automatic
 * car's hoods 231eb0[0][i] as variants of the clean hood's mesh, index i (player_parts.h nm_player_hood), clean first. */
enum { NM_DAMAGE_HOOD_CLEAN = 0, NM_DAMAGE_HOODS = 4 };
typedef struct { uint32_t model, index; float bounds[6]; } nm_damage_body_t; /* then positions, normals */
typedef struct {
    void *data;                /* the whole file; NULL: none loaded (the clean body only) */
    uint32_t body, vertices, faces, stride, classes;
    uint32_t count, clean;     /* variants in the file, the clean one's index */
} nm_damage_bodies_t;

/* 1: loaded, or the file is absent (data stays NULL); 0: an invalid file. count variants, the one at `clean` the
 * clean model (the file's model address); owner names the heap block. */
int nm_damage_variants_load(nm_damage_bodies_t *bodies, const char *path, const char *owner, unsigned count,
    unsigned clean);
static inline int nm_damage_bodies_load(nm_damage_bodies_t *bodies, const char *path) {
    return nm_damage_variants_load(bodies,path,"car damage bodies",NM_DAMAGE_BODIES,NM_DAMAGE_BODY_CLEAN);
}
static inline int nm_damage_hoods_load(nm_damage_bodies_t *bodies, const char *path) {
    return nm_damage_variants_load(bodies,path,"car hood variants",NM_DAMAGE_HOODS,NM_DAMAGE_HOOD_CLEAN);
}
void nm_damage_bodies_unload(nm_damage_bodies_t *bodies);
static inline const nm_damage_body_t *nm_damage_body(const nm_damage_bodies_t *bodies, unsigned variant) {
    return (const nm_damage_body_t *)((const uint8_t *)bodies->data+32u+variant*bodies->stride);
}
/* Variant `variant` (0..15: crash_model[0][variant], NM_DAMAGE_BODY_CLEAN; hoods: 0..3) into mesh `mesh` of the loaded pack:
 * positions, supplied normals, bounds. 0: no bodies loaded or the mesh is not the one they were made for. */
int nm_damage_body_apply(const nm_damage_bodies_t *bodies, unsigned variant, const nm_pack_t *pack, unsigned mesh);
/* The same into a mesh with the clean body's vertices in the same order and its faces in another: face i of the
 * mesh takes the normal of the variant's face face_of[i] (the manual car's body, 960dae). NULL: the same order. */
int nm_damage_body_apply_mapped(const nm_damage_bodies_t *bodies, unsigned variant, const nm_pack_t *pack,
    unsigned mesh, const uint16_t *face_of);
/* face_of for mesh `mesh` against the clean body's mesh `clean` of the same pack: face i of `mesh` is the first face
 * of `clean` with the same corners and count (faces with equal corners are coplanar layers whose normals are equal
 * in every variant: checked offline on car-damage.dnb). 1 when every vertex (position, u, v) is equal, every face is
 * found and its normal equals the clean face's; 0 otherwise (face_of then undefined). capacity: entries of face_of. */
int nm_damage_body_face_map(const nm_pack_t *pack, unsigned clean, unsigned mesh, uint16_t *face_of, unsigned capacity);
#endif
