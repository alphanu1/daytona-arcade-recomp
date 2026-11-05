#ifndef DAYTONA_NATIVE_RENDER_PRACTICE_H
#define DAYTONA_NATIVE_RENDER_PRACTICE_H
#include "practice_scene.h"
/* Gameplay queues one scene. Resident path does no geometry work here.
 * Callback failure sets scene->failed; gameplay cleans up next update, never
 * while an enDjinn PVR list is open. Streaming diagnostics prepare before PVR. */
int nm_practice_render_enqueue(nm_practice_scene_t *scene);
/* Load after gameplay spawn. Release inside enDjinn, before PVR shutdown;
 * also safe after a partial load failure. CPU scene packs remain game-owned. */
int nm_practice_render_load(nm_practice_scene_t *scene,const char *root,const char **stage);
void nm_practice_render_release(void);
void nm_practice_render_unload(nm_practice_scene_t *scene);
/* 6fe0 (tunnel 1) and 7008 (0; others_only: 6f60's restore of the shared windows alone): the window materials'
 * texture headers as 19dc8 writes them (word 0 |= 0x3c0, word 2 = word 2 & ~0x1fff | 0x0c0c) or their own.
 * 0: no renderer, or the car pack has no tunnel bindings (an asset set from before tools/tunnel_windows.py). */
int nm_practice_render_tunnel(nm_practice_scene_t *scene,int tunnel,int others_only);
/* Startup-only presentation assembly and independent math gates. */
extern const nm_view_t nm_practice_render_view;
int nm_practice_render_pose(nm_practice_scene_t *scene);
void nm_practice_render_matrix(const float m[12]);
int nm_practice_render_check_transform(const float xyz[3],const float m[12],float w);
int nm_practice_render_lighting(nm_practice_scene_t *scene);
int nm_practice_render_parts(nm_practice_scene_t *scene);
int nm_practice_render_scenery(nm_practice_scene_t *scene);
/* 3D projection of this tick (original focus word 501734 and window centre);
 * enable=0 restores the template's resting UI centre for the 2D actors. */
int nm_practice_render_projection(nm_practice_scene_t *scene,int enable);
/* Shadow instances leave the mesh stream and become modifier volumes; call
 * after both part lists. */
int nm_practice_render_shadows(nm_practice_scene_t *scene);
/* Race player's own part list; call after nm_practice_render_parts. */
int nm_practice_render_player_car(nm_practice_scene_t *scene);
#endif
