/* The slot machine's glass, face 4 of every reel list 2385f8[k] (spec-slot.md): a screen-checker face (texture
 * header e401, texel 15 transparent) that the arcade draws on every other pixel over the reels: a dark band at the
 * top and at the bottom of each reel (the texture's opaque columns, a grey ramp 0..130), clear in between. Here:
 * the bands as Gouraud strips through the texel centres (the bilinear reading of the texture; no texture, no
 * palette bank), alpha 1/2 on the pre-sorted translucent list (code/main.c), before the cars' effects; depth
 * compared against the reel, just nearer than its layered strips. Drawn with each reel that the scene drew this
 * frame (its PLACED instance's matrix and the world's view), one-sided as the arcade's light test decides. The
 * bands come from the object export (tools/prepare_world_objects.py glass: slot_glass_data.h in
 * NATIVE_SLOT_GLASS_DIR); without that header nothing is drawn. Presentation only. */
#ifndef DAYTONA_NATIVE_SLOT_GLASS_H
#define DAYTONA_NATIVE_SLOT_GLASS_H
#include "meshpack.h"

/* The reels the scene placed this frame (instances of kinds NM_OBJECT_SLOT..+2): queues the bands. 0: failure. */
int nm_slot_glass_enqueue(const nm_header_t *world, const nm_instance_t *const placed[NM_OBJECT_SLOTS], unsigned count);
void nm_slot_glass_report(void);
#endif
