#ifndef DAYTONA_TRACK_SURFACE_H
#define DAYTONA_TRACK_SURFACE_H
#include "meshpack.h"
/* Startup-only draw selection on a validated, writable beginner pack.
 * Physics/visibility assets are untouched; this is not a serializable DNM. */
int nm_track_surface_select(nm_pack_t *pack);
/* Scenery drawn by this build, for measuring the frame budget stage by stage
 * on hardware (make NATIVE_SCENERY_STAGE=n). Each stage adds to the one
 * before it; the asset directory is the same for all of them:
 *   0  reduced view: road, kerbs, immediate barriers; no sky
 *   1  + sky
 *   2  + other opaque faces: walls, stands, signs, buildings
 *   3  + opaque ground: terrain and infield; the grandstands with their
 *        crowd (one opaque composite face per stand polygon in a pack made
 *        with tools/prepare_face_order.py --flatten-stacks)
 *   4  + punch-through cutouts: trees, fences, signs
 *   5  + the screen-checker panels: the catch fence above the wall, drawn
 *        as punch-through stipple (--stipple-panels)
 *   6  + world objects: what the arcade's actor table draws outside the
 *        cells (a pack made with tools/prepare_world_objects.py; course 0:
 *        cones, sign towers, gantry board, the stand's roof band)
 * The distance cut is the arcade's own visible-cell list in every stage. */
/* Default: stage 6, everything (project decision, 14 October 2026; the make
 * default in local.cfg.mk is the same). Stage 3 was the default from 12
 * October: then the fullest stage that nearly held 60 fps on hardware. The
 * others stay selectable. */
#ifndef NATIVE_SCENERY_STAGE
#define NATIVE_SCENERY_STAGE 6
#endif
#define NM_SCENERY_SKY (NATIVE_SCENERY_STAGE>=1)
/* Material stage (meshpack.h, NM_PACK_SCENERY_CLASSES) drawn by that build. */
#define NM_SCENERY_MATERIAL_STAGE (NATIVE_SCENERY_STAGE>=1 ? NATIVE_SCENERY_STAGE-1u : 0u)
/* Same contract as nm_track_surface_select for material stages 0..5 of a
 * pack with scenery classes. A pack without them selects the audited reduced
 * view at stage 0 and is left complete otherwise. */
int nm_scenery_stage_select(nm_pack_t *pack,unsigned material_stage);
#endif
