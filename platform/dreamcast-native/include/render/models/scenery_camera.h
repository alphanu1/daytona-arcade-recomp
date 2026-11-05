#ifndef DAYTONA_NATIVE_SCENERY_CAMERA_H
#define DAYTONA_NATIVE_SCENERY_CAMERA_H
#include "geometry.h"
#include "meshpack.h"
typedef struct {
    nm_view_t view;
    nm_bounds_planes_t planes;
    int shared;
} nm_scenery_camera_t;
/* Only static world-space instances may share planes. Nonuniform input falls
 * back to individual model preparation; never borrow a car's local camera. */
void nm_scenery_camera_prepare(nm_scenery_camera_t *out,const nm_pack_t *pack,
    const nm_view_t *view);
#endif
