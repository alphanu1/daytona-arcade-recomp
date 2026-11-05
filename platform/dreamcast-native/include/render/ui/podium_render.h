/* The podium's 2D screen (tools/prepare_podium_screen.py: the arcade's object b948, ticks ba18).
 *
 * With the object's tick k = 1598 - timer (nm_podium_t; k = 0 on the spawn tick, its first ba18 pass):
 *   banner "Congratulations!" (2878aec, layer A): from k = 0, rising one 8-pixel row a tick from row 49 to
 *     row 1 (baf4), over the cinematic and the static screen;
 *   staff photo (model 288cb94, 3D): from k = 1213 (befc), T(0, 0, 2) Rx(0x4000) S(s) seen with focus 280
 *     (context 5020c0, 235e18) about the screen point (248, 196) (the centre of its projection, measured on
 *     the arcade's frame 14280), s = 0.29 + 0.05 min(n, 26), n = 40 at its first draw, one less a tick:
 *     the whole screen for 15 ticks, then it shrinks into its frame in 26;
 *   background (2884a8e, layer B, behind the photo): from timer 384 (bc80; here 383: its palette goes in the
 *     cars' banks only when no car or scenery frame is still being rendered, under the full-screen photo);
 *   SEGA (2872a2a) and AM2 (287659c) from k = 1408, "PRESENTED BY" from 1409; layer A erased at k = 1570;
 *   timer 40..29: layer A's row scroll (bcf4, from timer 42 on; on screen two ticks later), even lines
 *     25 (40 - timer) pixels to the right, odd lines to the left.
 * The overlay's 2D parts are point sampled PAL4 (layer A on the punch-through list over everything, the
 * background opaque behind), the photo bilinear (the arcade filters it) on the opaque list in front of the
 * scene. Palettes: cinematic.dp (banner, photo) in banks 60..61, written on the cinematic's first frame (the
 * result labels' banks, free until the result hand-off; 61 is the HUD's while the race is drawn when the HUD
 * carries another course's map); static.dp from bank 2 after the cleanup (palette_banks.h).
 * Presentation only. Without the generated header (NATIVE_PODIUM_SCREEN_DIR) nothing is drawn. */
#ifndef DAYTONA_NATIVE_PODIUM_RENDER_H
#define DAYTONA_NATIVE_PODIUM_RENDER_H
#include <stdint.h>
#include "podium.h"

enum { NM_PODIUM_PART_BANNER, NM_PODIUM_PART_PHOTO, NM_PODIUM_PART_BACKGROUND, NM_PODIUM_PART_SEGA,
       NM_PODIUM_PART_AM2, NM_PODIUM_PART_TEXT };
typedef struct { uint16_t role, bank, x, y, width, height, texture_width, texture_height; } nm_podium_part_t;

/* ASSET_ROOT/podium/ at the race scene's load (a new race reloads). 0: failure (1 when the build has none). */
int nm_podium_render_load(const char *root);
void nm_podium_render_unload(void);
/* One frame of the podium (phase 28, after this frame's ticks): queues what the screen shows. */
int nm_podium_render_enqueue(const nm_podium_t *podium);
#endif
