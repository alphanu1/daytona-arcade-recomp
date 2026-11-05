#include "render/models/scenery_camera.h"
#include <string.h>

void nm_scenery_camera_prepare(nm_scenery_camera_t *out,const nm_pack_t *pack,
        const nm_view_t *view) {
    out->shared=0;
    if(!pack || !pack->header || !pack->instances || !pack->header->instances || !view || !nm_view_valid(view)) return;
    const nm_instance_t *first=pack->instances;
    for(unsigned i=1;i<pack->header->instances;++i) {
        const nm_instance_t *instance=pack->instances+i;
        if(memcmp(first->matrix,instance->matrix,sizeof(first->matrix)) ||
           memcmp(first->focus,instance->focus,sizeof(first->focus)) ||
           memcmp(first->viewport,instance->viewport,sizeof(first->viewport)) ||
           memcmp(first->center,instance->center,sizeof(first->center))) return;
    }
    out->view=*view;
    nm_bounds_planes_prepare(&out->planes,view,first->matrix,first->focus);
    out->shared=1;
}
