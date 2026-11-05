#pragma once
#include "race_scene.h"
#include "hud.h"
/* In-race HUD: resident PAL4 sheets, cached headers, direct SQ submission.
 * load at scene load; reset at race entry; tick once after every gameplay
 * tick; enqueue once per rendered race frame. No per-frame uploads or heap.
 * enqueue(flat): 0 draws by depth (opaque cores on the opaque list, the rest
 * punch-through, in front of the 3D picture); nonzero draws everything on the
 * punch-through list in paint order without a depth test, for a frame that
 * also holds an overlay drawn that way (code/render/ui/hud_render.c). */
int nm_hud_render_load(const char *root);
void nm_hud_render_unload(void);
/* reset and tick take the HUD state that the race owner computes with the
 * gameplay's float rules (include/hud.h; code/practice.c: the renderer's units
 * are built with -ffast-math, the CONDITION panel's rules must not be); the
 * state must live until the next call. */
void nm_hud_render_reset(const nm_hud_t *hud);
void nm_hud_render_tick(const nm_hud_t *hud);
int nm_hud_render_enqueue(int flat);
