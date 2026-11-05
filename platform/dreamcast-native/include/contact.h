#ifndef DAYTONA_NATIVE_CONTACT_H
#define DAYTONA_NATIVE_CONTACT_H
#include "force_math.h"

typedef struct {
    float xyz[4][3]; uint32_t surface, heading;
    float recovery_height, recovery_center[3];
} nm_contact_quad_t;
typedef struct {
    uint32_t slot, list_words, quad_count, max_candidates, payload_bytes;
    void *storage;
    const uint16_t *grid, *lists;
    const nm_contact_quad_t *quads;
} nm_contact_pack_t;
typedef struct { float height; uint32_t road, surface, mask; } nm_contact_result_t;

#ifdef __cplusplus
extern "C" {
#endif
/* Load once before gameplay; zero-initialize pack, unload before replacing.
 * Packed data is immutable while queries run. No query allocation or ROM bus. */
int nm_contact_load(nm_contact_pack_t *pack, const char *path);
void nm_contact_unload(nm_contact_pack_t *pack);
int nm_contact_cell(float x, float z, unsigned *cell);
/* Source16e10: same rounded/wrapped coordinates,128-unit scenery grid.
 * Shared by active/held player updates and native practice spawn/reset. */
static inline int nm_contact_scenery_cell(float x, float z, uint16_t *out) {
    unsigned cell;
    if (!out || !nm_contact_cell(x,z,&cell)) return 0;
    *out=(uint16_t)(((cell>>3)&15) | ((cell>>6)&240));
    return 1;
}
/* Commands32/36: perimeter-ordered X/Z half-planes, inclusive edges.
 * Returns1 inside,0 outside,-1 invalid arithmetic. Y is not consumed. */
int nm_contact_contains(const float xyz[4][3], float x, float z);
int nm_contact_query(const nm_contact_pack_t *pack, const nm_drive_params_t *math,
                     float x, float z, nm_contact_result_t *state);
#ifdef __cplusplus
}
#endif
#endif
