#ifndef DAYTONA_INSPECT_CAMERA_H
#define DAYTONA_INSPECT_CAMERA_H
#include <sh4zam/shz_sh4zam.h>

typedef struct {
    alignas(32) shz_mat3x4_t base;
    shz_vec3_t position;
    float yaw,pitch;
} nm_inspect_camera_t;

void nm_inspect_camera_reset(nm_inspect_camera_t *camera,const float view[12]);
/* Local movement in units/second; angles in radians/second. XMTRX clobbered. */
void nm_inspect_camera_step(nm_inspect_camera_t *camera,float dt,
    float yaw,float pitch,shz_vec3_t movement,float view[12]);
shz_vec3_t nm_inspect_camera_position(const float view[12]);
#endif
