#include "render/scene/inspect_camera.h"

void nm_inspect_camera_reset(nm_inspect_camera_t *camera,const float view[12]) {
    shz_memcpy4(camera->base.elem,view,sizeof(camera->base));
    camera->position=shz_vec3_init(0,0,0);
    camera->yaw=camera->pitch=0;
}

void nm_inspect_camera_step(nm_inspect_camera_t *camera,float dt,
        float yaw,float pitch,shz_vec3_t movement,float view[12]) {
    dt=shz_clampf(dt,0,0.05f);
    camera->yaw=shz_wrapf(camera->yaw+yaw*dt,-SHZ_F_PI,SHZ_F_PI);
    camera->pitch=shz_clampf(camera->pitch+pitch*dt,-1.5f,1.5f);
    /* Rebuild from angles, not repeated matrix multiplication: no accumulated
     * scale/shear drift during long inspection sessions. */
    shz_xmtrx_init_rotation_yxz(camera->yaw,camera->pitch,0);
    camera->position=shz_vec3_add(camera->position,
        shz_xmtrx_transform_vec3(shz_vec3_scale(movement,dt)));
    shz_xmtrx_transpose();
    const shz_vec3_t translation=shz_xmtrx_transform_vec3(shz_vec3_neg(camera->position));
    shz_xmtrx_set_translation(translation.x,translation.y,translation.z);
    shz_xmtrx_apply_3x4(&camera->base);
    alignas(32) shz_mat3x4_t result;
    shz_xmtrx_store_3x4(&result);
    shz_memcpy4(view,result.elem,sizeof(result));
}

shz_vec3_t nm_inspect_camera_position(const float view[12]) {
    alignas(32) shz_mat3x4_t matrix;
    shz_memcpy4(matrix.elem,view,sizeof(matrix));
    shz_xmtrx_load_3x4(&matrix);
    shz_xmtrx_set_translation(0,0,0);
    shz_xmtrx_transpose();
    return shz_xmtrx_transform_vec3(shz_vec3_init(-view[9],-view[10],-view[11]));
}
