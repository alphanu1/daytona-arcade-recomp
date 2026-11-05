#ifndef DAYTONA_GAMEPLAY_NAME_SCENE_H
#define DAYTONA_GAMEPLAY_NAME_SCENE_H
#include "render/ui/name_draw.h"

typedef struct {
    nm_name_entry_t entry,draw_entry;
    nm_name_marker_t marker;
    unsigned intro_remaining,finish_requested;
    int16_t remaining;
    unsigned draw_seconds;
    int draw_intro; /*31..0 entrance; -1 active. */
    uint32_t commands[2]; /* Selection/acceptance intents for this tick only. */
} nm_name_scene_t;

int nm_name_scene_init(nm_name_scene_t *scene,const nm_name_layout_t *layout);
/* One fixed tick, no PVR/controller polling. Returns0 invalid,1 active,
 * 2 requests original name commit; caller owns commit/exit phases afterwards.
 * draw_entry preserves original draw-before-input order for deferred rendering. */
int nm_name_scene_step(nm_name_scene_t *scene,const nm_name_layout_t *layout,
                      int8_t steering,unsigned accept_edge);
#endif
