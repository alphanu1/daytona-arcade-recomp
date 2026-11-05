#ifndef DAYTONA_NATIVE_MATERIALS_H
#define DAYTONA_NATIVE_MATERIALS_H
#include <enDjinn/enj_texture.h>
#include "material_pack.h"
#include "render/materials/palette_banks.h"
/* Car and scenery textures are DNT6 palette packs, uploaded once per scene
 * load and resident until unload; nothing is uploaded while a scene runs.
 * Outside render callbacks, while PVR is initialized. Load once; explicit
 * unload fences GPU completion before releasing allocations. The car pack
 * writes its palettes to banks NM_CAR_PALETTE_BANK_FIRST.. at load. A pack of
 * an earlier format is refused with a message. */
int nm_materials_load(const char *path);
/* The resource is resident (always, once loaded). */
int nm_materials_require(unsigned resource);
void nm_materials_unload(void);
const nm_material_binding_t *nm_materials_find(const uint16_t header[4], unsigned luma);
/* Descriptor owns the original allocation; use fDtAdjustPVRPointer only for
 * submission of its small-codebook VQ header, never for freeing it. */
const enj_texture_info_t *nm_materials_texture(unsigned resource);
#if NM_SMOKE_MODE
/* make NATIVE_SMOKE_MODE=pt-tex|pt-screen|tr-screen: the smoke texture made from a translucent-class resource
 * (materials.c), else NULL. */
const enj_texture_info_t *nm_materials_smoke_texture(unsigned resource);
#if NM_SMOKE_DOOR && defined(NM_SMOKE_TIGHT) && NM_SMOKE_TIGHT
/* make NATIVE_SMOKE_TIGHT=1: the non-hole texels of the resource's smoke texture (model_submission.h, sq_tr_tight_prepare). */
int nm_smoke_texture_mask(unsigned resource, unsigned side, uint8_t *mask);
#endif
#endif
/* Borrowed immutable metadata, valid until unload. */
const nm_material_pack_t *nm_materials_index(void);
/* Palette RAM bank of a textured binding of the loaded DNT6 car pack, else -1. */
int nm_materials_palette_bank(const nm_material_binding_t *binding);
int nm_world_materials_load(const char *path);
const nm_material_pack_t *nm_world_materials_index(void);
/* World pack (palette-shaded scenery). Load opens it and writes its
 * palettes into the banks it shares with the cars (palette_banks.h); the
 * renderer then names the textures the selected scenery draws (one byte per
 * resource, NULL for all) and they are uploaded once into one VRAM block.
 * Absent resources have no texture and no header for the whole scene. */
int nm_world_materials_palettized(void);
int nm_world_materials_upload(const uint8_t *used);
/* Palette RAM bank of a textured binding of the loaded DNT6 world pack, else -1. */
int nm_world_materials_palette_bank(const nm_material_binding_t *binding);
const enj_texture_info_t *nm_world_materials_texture(unsigned resource);
void nm_world_materials_unload(void);
#endif
