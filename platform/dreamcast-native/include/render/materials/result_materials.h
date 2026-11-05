#ifndef DAYTONA_RESULT_MATERIALS_H
#define DAYTONA_RESULT_MATERIALS_H
#include <enDjinn/enj_enDjinn.h>
int nm_result_materials_load(const char *root);
void nm_result_materials_unload(void);
/* Writes the glyph palettes (entries 32..959, shared with the car palettes
 * while cars are drawn). Call once at the result hand-off, outside render
 * callbacks, before the first result frame; repeated calls are no-ops. */
int nm_result_materials_palettes_apply(void);
/* Borrowed until unload. Use explicit PVR_TXRFMT_4BPP_PAL(*bank) and global
 * ARGB8888; enDjinn's inferred per-texture palette_format is not authoritative.
 * Missing/invalid bindings return NULL and leave bank unchanged. */
const enj_texture_info_t *nm_result_material(const uint16_t header[4],unsigned luma,unsigned *bank);
#endif
