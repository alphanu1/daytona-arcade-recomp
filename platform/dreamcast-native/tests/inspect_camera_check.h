#ifndef DAYTONA_INSPECT_CAMERA_CHECK_H
#define DAYTONA_INSPECT_CAMERA_CHECK_H
#include "render/scene/inspect_camera.h"
#include <math.h>
#include <stdio.h>
/* Startup only: independent scalar rotation/inverse oracle. */
static int nm_inspect_camera_check(void) {
    const float base[12]={0,0,-1,0,1,0,1,0,0,-3,2,4};
    nm_inspect_camera_t camera;float view[12];
    nm_inspect_camera_reset(&camera,base);
    for(unsigned i=0;i<128;++i) {
        nm_inspect_camera_step(&camera,0.017f,1.7f,-0.6f,shz_vec3_init(0,0,0),view);
        const float cy=cosf(camera.yaw),sy=sinf(camera.yaw);
        const float cx=cosf(camera.pitch),sx=sinf(camera.pitch);
        const float r[9]={cy,0,-sy,sy*sx,cx,cy*sx,sy*cx,-sx,cy*cx};
        for(unsigned c=0;c<4;++c) for(unsigned a=0;a<3;++a) {
            const float expected=r[a*3]*base[c*3]+r[a*3+1]*base[c*3+1]+r[a*3+2]*base[c*3+2];
            if(!isfinite(view[c*3+a]) || fabsf(view[c*3+a]-expected)>0.001f) return 0;
        }
        const shz_vec3_t position=nm_inspect_camera_position(view);
        for(unsigned a=0;a<3;++a) {
            const float residual=view[a]*position.x+view[3+a]*position.y+view[6+a]*position.z+view[9+a];
            if(!isfinite(residual) || fabsf(residual)>0.001f) return 0;
        }
    }
    const float identity[12]={1,0,0,0,1,0,0,0,1,0,0,0};
    nm_inspect_camera_reset(&camera,identity);
    nm_inspect_camera_step(&camera,0.05f,0,0,shz_vec3_init(0,0,20),view);
    if(fabsf(view[11]+1)>0.001f) return 0;
    puts("Native inspect camera: 128 rotation/inverse cases + translation PASS");
    return 1;
}
#endif
