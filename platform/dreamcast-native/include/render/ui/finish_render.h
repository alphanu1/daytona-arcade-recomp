#ifndef DAYTONA_NATIVE_FINISH_RENDER_H
#define DAYTONA_NATIVE_FINISH_RENDER_H
#include "finish_actor.h"
int nm_finish_render_load(const char *root);
int nm_finish_render_enqueue(const nm_finish_actor_t *actor, int branch,
                             const int16_t center[2]);
void nm_finish_render_unload(void);
#endif
