#ifndef DAYTONA_NATIVE_RENDER_TITLE_RENDER_H
#define DAYTONA_NATIVE_RENDER_TITLE_RENDER_H

#include "title.h"

/* The generated title data is immutable; textures and PVR headers live here. */
int nm_title_render_load(void);
int nm_title_render_enqueue(uint64_t tick);
int nm_title_render_release(void);

#endif
