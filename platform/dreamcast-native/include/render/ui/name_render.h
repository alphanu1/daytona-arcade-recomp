#ifndef DAYTONA_NATIVE_NAME_RENDER_H
#define DAYTONA_NATIVE_NAME_RENDER_H
#include "meshpack.h"
#include "render/ui/name_draw.h"

/* Pack must come from nm_pack_load (validated indices/counts). Borrowed
 * immutable assets must outlive all queued callbacks. Colours are
 * indexed exactly like the pack's two materials, then boot mode and luma>>2. */
int nm_name_render_init(const nm_pack_t *pack,const nm_name_layout_t *layout,
    const uint16_t material_headers[2][4],const uint32_t colors[2][2][64],unsigned display_mode);
int nm_name_render_enqueue(const nm_name_entry_t *entry,const nm_name_marker_t *marker,
    const nm_drive_params_t *math,const int16_t center[2],uint32_t frame);
/*226be8 decrements32 before drawing: caller supplies31..0, once per tick. */
int nm_name_render_enqueue_intro(const nm_drive_params_t *math,const int16_t center[2],unsigned remaining);
int nm_name_render_enqueue_exit(const nm_name_entry_t *entry,const nm_drive_params_t *math,
    const int16_t center[2],unsigned phase,uint16_t tick);
/* Refuses release while the enDjinn callback is pending. Does not free assets. */
int nm_name_render_release(void);
#endif
