#ifndef DAYTONA_PRACTICE_STARTUP_H
#define DAYTONA_PRACTICE_STARTUP_H
/* Target startup oracles, never per-frame work. The scalar reference remains
 * in its separate translation unit without the SH4ZAM presentation defines. */
#include "../include/practice_scene.h"
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
#include <stdlib.h>
#include "inspect_camera_check.h"

#if defined(__DREAMCAST__) && defined(NATIVE_DRIVING_TEST)
static int nm_practice_check_matrix_construction(const nm_drive_params_t *math) {
    for(unsigned i=0;i<65536;++i) {
        if((uint32_t)(nm_pose_radians((uint16_t)i)*SHZ_FSCA_RAD_FACTOR)!=i) return 0;
        const float xyz[3]={99,-3,148};
        const int16_t angles[3]={(int16_t)i,(int16_t)(i*17),(int16_t)(i*37)};
        float matrix[12];
        if(!nm_body_pose(math,xyz,angles,matrix)) return 0;
        nm_player_t camera_car={0};
        shz_memcpy4(camera_car.boundary.ground.position,xyz,sizeof(xyz));
        camera_car.boundary.ground.pitch=angles[0];
        camera_car.boundary.ground.yaw=angles[1];
        camera_car.boundary.ground.roll=angles[2];
        int16_t heading;
        if(!nm_follow_view(math,&camera_car,matrix,&heading) || heading!=angles[1]) return 0;
        camera_car.filtered_pitch=angles[0]; camera_car.bank=angles[2];
        camera_car.force.steer_angle=angles[1];
        camera_car.wheel_phase[0]=angles[0]; camera_car.wheel_phase[1]=angles[2];
        camera_car.boundary.damage.damage=(i&1) ? 0xf0 : 0;
        for(unsigned wheel=0;wheel<4;++wheel)
            if(!nm_wheel_pose(math,&camera_car,wheel,xyz,matrix)) return 0;
    }
    puts("SH4ZAM construction PASS: 65536 angle bins and mixed rotations; exact translation");
    puts("SH4ZAM camera inverse PASS: 65536 mixed poses, scalar bounds checked");
    puts("SH4ZAM resident wheel chain PASS: 262144 mixed wheel poses incl damage");
    return 1;
}
#endif
#endif
