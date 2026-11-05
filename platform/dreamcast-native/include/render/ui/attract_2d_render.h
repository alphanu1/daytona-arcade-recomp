/* The attract's 2D layer on the PVR (include/attract_2d.h has what it holds): the footer, the AM2 block and the
 * blinking line over the cinematic, the ranking of phases 4/5 and the V.R. demo's text and view boxes.
 *
 * Every tile word of layer A with ink is an 8x8 cell of one atlas (tools/prepare_attract_2d.py: ASSET_ROOT/
 * attract-2d/attract-2d.dt, ARGB1555, resident from power-on: 256 KB of VRAM), point sampled, on the punch-through
 * list in front of every scene polygon (depth written, compared always); the scanlines the ranking's line table
 * hides are cut out of the cells. Retained: the TA records (sprites, a run of cells whose atlas slots follow as
 * one) are built when the visible part changes, and each frame only copies them. The screen's 496 x 384 pixels scale to 640 x 480 as the other 2D screens.
 * The layer's state follows the attract's iterations through the calls below (code/practice.c), the original's
 * order (attract_2d.h). Presentation only. Without the generated header (NATIVE_ATTRACT_2D_DIR) or the atlas
 * nothing is drawn. */
#ifndef DAYTONA_NATIVE_ATTRACT_2D_RENDER_H
#define DAYTONA_NATIVE_ATTRACT_2D_RENDER_H
#include <stdint.h>
#include "race_records.h"

/* At power-on. 0: failure (1 when the build or the asset set has none). */
int nm_attract_2d_render_load(const char *root);
void nm_attract_2d_render_unload(void);
/* Phase 2 of a loop: COURSE (501460), its RECORDS (the first five are drawn), VIEW (501710 now). */
void nm_attract_2d_phase2(unsigned course, const nm_race_record_t *records, unsigned view);
/* A cinematic iteration (the scene start: phase 3) with its 5010b0; LAST: phase 5's last. */
void nm_attract_2d_cinematic(unsigned phase, uint32_t counter, int last);
/* A demo iteration: phase 6 (its start), then 7. */
void nm_attract_2d_demo(unsigned phase, uint32_t counter);
/* The layer is not drawn until the next phase 2 (the title, a game). */
void nm_attract_2d_off(void);
/* This frame's layer: the punch-through list's callback. */
int nm_attract_2d_render_enqueue(void);
#endif
