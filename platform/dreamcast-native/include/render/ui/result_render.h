#ifndef DAYTONA_RESULT_RENDER_H
#define DAYTONA_RESULT_RENDER_H
#include "result_actor.h"
int nm_result_render_load(const char *root,const nm_drive_params_t *math);
int nm_result_render_enqueue(const nm_result_actor_t *actor,const nm_drive_params_t *math,const int16_t center[2]);
void nm_result_render_unload(void);
#endif
