#ifndef DAYTONA_NATIVE_RENDER_SELECT_RENDER_H
#define DAYTONA_NATIVE_RENDER_SELECT_RENDER_H
#include "select.h"

/* The course/transmission select and GENTLEMEN screens (tools/prepare_select.py: ROOT/select/select.dsl and
 * its PAL4 textures, loaded at the select's start and released before the race loads). The pose comes from
 * the select's state (select.h) and the frame counter 5010b0 (the panels' and frame's blink). */
int nm_select_render_load(const char *root);
int nm_select_render_enqueue(const nm_select_t *select, uint32_t counter);
int nm_select_render_release(void);

#endif
