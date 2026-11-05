#ifndef DAYTONA_NATIVE_POSE_H
#define DAYTONA_NATIVE_POSE_H
#include "player.h"
#include "nm_mem.h"
#ifdef __DREAMCAST__
#include <sh4zam/shz_sh4zam.h>
#include <float.h>
#include <stdio.h>
#ifdef NATIVE_DRIVING_TEST
static int nm_pose_verify=1; /* Finite diagnostic only. */
#else
static const int nm_pose_verify=0; /* Controller/disc: no duplicate reference math. */
#endif
#endif

/* Presentation matrices, column-major 3x4 as in DNM1. No machine state.
 * Normal body draw order at 6c40: translate, yaw, pitch, roll. Crash/model
 * offsets are separate work; this must not replace wheel/physics transforms. */
static inline int nm_body_pose_reference(const nm_drive_params_t *p, const float xyz[3],
                              const int16_t angles[3], float out[12]) {
    if (!p || !xyz || !angles || !out) return 0;
    for (unsigned a=0; a<3; ++a) if (!isfinite(xyz[a])) return 0;
    const float cp=nm_drive_sine(p,(uint16_t)(angles[0]+16384)), sp=nm_drive_sine(p,(uint16_t)angles[0]);
    const float cy=nm_drive_sine(p,(uint16_t)(angles[1]+16384)), sy=nm_drive_sine(p,(uint16_t)angles[1]);
    const float cr=nm_drive_sine(p,(uint16_t)(angles[2]+16384)), sr=nm_drive_sine(p,(uint16_t)angles[2]);
    const float m[12]={cy*cr-sy*sp*sr, -cp*sr, sy*cr+cy*sp*sr,
        cy*sr+sy*sp*cr, cp*cr, sy*sr-cy*sp*cr,
        -sy*cp, sp, cy*cp, xyz[0],xyz[1],xyz[2]};
    for (unsigned i=0; i<12; ++i) if (!isfinite(m[i])) return 0;
    nm_memcpy(out,m,sizeof(m)); return 1;
}

#ifdef __DREAMCAST__
/* The installed strict SH4 rotation API converts radians with FTRC. Select
 * the CENTER of the original unsigned 16-bit FSCA bin: a boundary conversion
 * may otherwise round down one angle unit. This is not a half-unit rotation. */
static inline float nm_pose_radians(uint16_t angle) {
    return ((float)angle+0.5f)*(1.0f/SHZ_FSCA_RAD_FACTOR);
}
#endif

static inline int nm_body_pose(const nm_drive_params_t *p, const float xyz[3],
                              const int16_t angles[3], float out[12]) {
#ifdef __DREAMCAST__
    if(!p || !xyz || !angles || !out) return 0;
    for(unsigned i=0;i<3;++i) if(!isfinite(xyz[i])) return 0;
    alignas(32) shz_mat3x4_t result;
    /* Source uses the opposite rotation sign to SH4ZAM, T * Ry * Rx * Rz. */
    shz_xmtrx_init_rotation_yxz(nm_pose_radians((uint16_t)(-(int)angles[1])),
        nm_pose_radians((uint16_t)(-(int)angles[0])),
        nm_pose_radians((uint16_t)(-(int)angles[2])));
    shz_xmtrx_set_translation(xyz[0],xyz[1],xyz[2]);
    shz_xmtrx_store_3x4(&result);
    for(unsigned i=0;i<12;++i) if(!isfinite(result.elem[i])) return 0;
    if(nm_pose_verify) {
    float reference[12];
    if(!nm_body_pose_reference(p,xyz,angles,reference)) return 0;
    for(unsigned i=0;i<12;++i) {
        /* Existing host pose test permits 1e-5 absolute rotation error.
         * Translation is copied exactly, never given this tolerance. */
        if(!isfinite(result.elem[i]) || (i<9 ?
            shz_fabsf(result.elem[i]-reference[i])>1e-5f : result.elem[i]!=reference[i])) {
            puts("FAIL: SH4ZAM body rotation/translation differs"); return 0;
        }
    }
    }
    shz_memcpy4(out,result.elem,sizeof(result)); return 1;
#else
    return nm_body_pose_reference(p,xyz,angles,out);
#endif
}

static inline int nm_pose_compose_reference(const float a[12], const float b[12], float out[12]) {
    float m[12];
    for (unsigned c=0; c<4; ++c) for (unsigned r=0; r<3; ++r) {
        float v=a[r]*b[c*3]+a[3+r]*b[c*3+1]+a[6+r]*b[c*3+2];
        if (c==3) v+=a[9+r];
        if (!isfinite(v)) return 0;
        m[c*3+r]=v;
    }
    nm_memcpy(out,m,sizeof(m)); return 1;
}

static inline int nm_pose_compose(const float a[12], const float b[12], float out[12]) {
#ifdef __DREAMCAST__
    /* DNM and SH4ZAM both use column-major affine 3x4. Local typed copies
     * preserve input/output aliasing and avoid alignment/type-punning claims.
     * XMTRX is clobbered; callers load their render matrix after pose setup. */
    alignas(32) shz_mat3x4_t left, right, result;
    shz_memcpy4(left.elem,a,sizeof(left));
    shz_memcpy4(right.elem,b,sizeof(right));
    shz_xmtrx_load_apply_store_3x4(&result,&left,&right);
    for(unsigned i=0;i<12;++i) if(!isfinite(result.elem[i])) return 0;
#ifdef NATIVE_DRIVING_TEST
    if(nm_pose_verify) {
    float reference[12];
    if(!nm_pose_compose_reference(a,b,reference)) return 0;
    for(unsigned c=0;c<4;++c) for(unsigned r=0;r<3;++r) {
        const unsigned i=c*3+r;
        const float magnitude=shz_fabsf(a[r]*b[c*3])+shz_fabsf(a[3+r]*b[c*3+1])+
            shz_fabsf(a[6+r]*b[c*3+2])+(c==3 ? shz_fabsf(a[9+r]) : 0.0f);
        if(!isfinite(magnitude) || shz_fabsf(result.elem[i]-reference[i])>
                8.0f*FLT_EPSILON*magnitude+FLT_MIN) {
            puts("FAIL: SH4ZAM pose composition exceeds cancellation-aware bound");
            return 0;
        }
    }
    }
#endif
    shz_memcpy4(out,result.elem,sizeof(result));
    return 1;
#else
    return nm_pose_compose_reference(a,b,out);
#endif
}

/* Intro ordinary body detail4/3/2/1 (7974/7c4c/7f20/81dc): road yaw, pitch, roll, residual yaw;
 * nonzero RAW5c adds crash Z/Y/X and local pivot translation. Not Euler-angle
 * addition. Mode 0 is d144's extra, 1 body, 2 shadow (ground height, no pivot). */
static inline int nm_car_part_pose_reference(const nm_drive_params_t *p,
        const nm_player_t *car,int crash_rotation,int player,float out[12]) {
    if(!p || !car || !out) return 0;
    const nm_damage_state_t *damage=&car->boundary.damage;
    const float xyz[3]={car->boundary.ground.position[0],
        car->boundary.ground.position[1]+(crash_rotation==2 ? 0 : nm_force_float(damage->overlay_model)),car->boundary.ground.position[2]};
    const float zero[3]={0};
    const int16_t yaw=player ? car->render_angles[1] : car->road_yaw;
    const int16_t base[3]={car->render_angles[0],yaw,car->render_angles[2]};
    const int16_t residual[3]={0,(int16_t)(player ? (car->input_inhibit ? car->inhibit_angle : 0) :
        nm_yaw_angle((uint32_t)((int)car->render_angles[1]+car->inhibit_angle-yaw))),0};
    float result[12],part[12];
    if(!nm_body_pose_reference(p,xyz,base,result) || !nm_body_pose_reference(p,zero,residual,part) ||
       !nm_pose_compose_reference(result,part,result)) return 0;
    if(crash_rotation && damage->overlay_model) {
        if(crash_rotation==2)
            for(unsigned r=0;r<3;++r) result[3+r]*=nm_force_float(0x3c23d70a);
        const unsigned order[3]={2,1,0};
        for(unsigned i=0;i<3;++i) {
            int16_t angle[3]={0};angle[order[i]]=damage->angle[order[i]];
            if(!nm_body_pose_reference(p,zero,angle,part) || !nm_pose_compose_reference(result,part,result)) return 0;
        }
        const float pivot[3]={0,nm_force_float(0xbf0a3d71),0};const int16_t angle[3]={0};
        if(crash_rotation==1 && (!nm_body_pose_reference(p,pivot,angle,part) || !nm_pose_compose_reference(result,part,result))) return 0;
    }
    nm_memcpy(out,result,sizeof(result));return 1;
}

static inline int nm_car_part_pose(const nm_drive_params_t *p,const nm_player_t *car,int crash_rotation,int player,float out[12]) {
#ifdef __DREAMCAST__
    if(!p || !car || !out) return 0;
    const nm_damage_state_t *damage=&car->boundary.damage;
    const shz_vec3_t xyz=shz_vec3_add(shz_vec3_init(car->boundary.ground.position[0],
        car->boundary.ground.position[1],car->boundary.ground.position[2]),
        shz_vec3_init(0,crash_rotation==2 ? 0 : nm_force_float(damage->overlay_model),0));
    if(!isfinite(xyz.x) || !isfinite(xyz.y) || !isfinite(xyz.z)) return 0;
    alignas(32) shz_mat3x4_t result;
    const int yaw=player ? car->render_angles[1] : car->road_yaw;
    const int residual=player ? (car->input_inhibit ? car->inhibit_angle : 0) :
        car->render_angles[1]+car->inhibit_angle-yaw;
    shz_xmtrx_init_rotation_yxz(nm_pose_radians((uint16_t)(-yaw)),
        nm_pose_radians((uint16_t)(-(int)car->render_angles[0])),
        nm_pose_radians((uint16_t)(-(int)car->render_angles[2])));
    shz_xmtrx_set_translation(xyz.x,xyz.y,xyz.z);
    shz_xmtrx_rotate_y(nm_pose_radians((uint16_t)(-residual)));
    if(crash_rotation && damage->overlay_model) {
        if(crash_rotation==2) shz_xmtrx_scale(1,nm_force_float(0x3c23d70a),1);
        shz_xmtrx_rotate_z(nm_pose_radians((uint16_t)(-(int)damage->angle[2])));
        shz_xmtrx_rotate_y(nm_pose_radians((uint16_t)(-(int)damage->angle[1])));
        shz_xmtrx_rotate_x(nm_pose_radians((uint16_t)(-(int)damage->angle[0])));
        if(crash_rotation==1) shz_xmtrx_translate(0,nm_force_float(0xbf0a3d71),0); // Local pivot, not world-space addition.
    }
    shz_xmtrx_store_3x4(&result); // One resident chain, no intermediate bank reloads.
    for(unsigned i=0;i<12;++i) if(!isfinite(result.elem[i])) return 0;
    shz_memcpy4(out,result.elem,sizeof(result));return 1;
#else
    return nm_car_part_pose_reference(p,car,crash_rotation,player,out);
#endif
}
/* Player6c40/6dc0 uses body yaw BEFORE pitch/roll; opponents use road yaw
 * followed by residual yaw. Sharing crash handling must not merge these orders. */
static inline int nm_player_part_pose(const nm_drive_params_t *p,const nm_player_t *car,int shadow,float out[12]) {
    return nm_car_part_pose(p,car,shadow ? 2 : 1,1,out);
}
static inline int nm_intro_part_pose_reference(const nm_drive_params_t *p,const nm_player_t *car,int mode,float out[12]) {
    return nm_car_part_pose_reference(p,car,mode,0,out);
}
static inline int nm_intro_part_pose(const nm_drive_params_t *p,const nm_player_t *car,int mode,float out[12]) {
    return nm_car_part_pose(p,car,mode,0,out);
}
static inline int nm_intro_body_pose_reference(const nm_drive_params_t *p,const nm_player_t *car,float out[12]) {
    return nm_intro_part_pose_reference(p,car,1,out);
}
static inline int nm_intro_body_pose(const nm_drive_params_t *p,const nm_player_t *car,float out[12]) {
    return nm_intro_part_pose(p,car,1,out);
}
/* d144..d21c extra model: same road/residual rotation and raw5c height,
 * but NO crash-axis rotations or local pivot. Not the special body matrix. */
static inline int nm_intro_special_pose(const nm_drive_params_t *p,const nm_player_t *car,float out[12]) {
    return nm_intro_part_pose(p,car,0,out);
}
/* 7b04/7ddc/8098 shadow blocks; caller owns detail/quota selection. */
static inline int nm_intro_shadow_pose(const nm_drive_params_t *p,const nm_player_t *car,float out[12]) {
    return nm_intro_part_pose(p,car,2,out);
}

/* Local wheel draw7384..7664, relative to the already transformed body.
 * xyz is the authored RENDER offset234af4, not collision wheel_local234ac4.
 * Visibility/model selection and crash-body transforms belong to the caller.
 * Reuse live steering/phase/damage; never bake a captured wheel matrix. */
static inline int nm_wheel_pose(const nm_drive_params_t *p, const nm_player_t *car,
        unsigned wheel, const float xyz[3], float out[12]) {
    if (!p || !car || wheel>=4 || !xyz || !out) return 0;
    const float zero[3]={0};
    const int16_t undo[3]={nm_yaw_angle(0u-(uint32_t)(int32_t)car->filtered_pitch),0,
        nm_yaw_angle(0u-(uint32_t)(int32_t)car->bank)};
    const int16_t steer[3]={0,(int16_t)(wheel<2 ? car->force.steer_angle : 0),0};
    const int16_t phase=car->wheel_phase[wheel/2];
    int32_t wobble=0;
    if ((car->boundary.damage.damage & (1u<<(7-wheel))) &&
        !nm_drive_round_i32(nm_drive_sine(p,(uint16_t)((uint32_t)(int32_t)phase*2))*1541.0f,&wobble)) return 0;
    const int16_t roll[3]={0,0,nm_yaw_angle((uint32_t)wobble)};
    const int16_t spin[3]={phase,0,0};
    float result[12],part[12];
#ifdef __DREAMCAST__
    if(nm_pose_verify) {
#endif
    if (!nm_body_pose(p,zero,undo,result) || !nm_body_pose(p,xyz,steer,part) ||
        !nm_pose_compose(result,part,result) || !nm_body_pose(p,zero,roll,part) ||
        !nm_pose_compose(result,part,result) || !nm_body_pose(p,zero,spin,part) ||
        !nm_pose_compose(result,part,result)) return 0;
#ifdef __DREAMCAST__
    }
    /* Keep undo * T(offset) * Ry(steer) * Rz(wobble) * Rx(spin)
     * resident. Translation changes once; later rotations touch only 3x3.
     * No helper which owns/clobbers XMTRX may run until the final store. */
    alignas(32) shz_mat3x4_t chained;
    shz_xmtrx_init_rotation_yxz(nm_pose_radians(0),
        nm_pose_radians((uint16_t)(-(int)undo[0])),
        nm_pose_radians((uint16_t)(-(int)undo[2])));
    const shz_vec3_t offset=shz_xmtrx_transform_vec3(shz_vec3_init(xyz[0],xyz[1],xyz[2]));
    shz_xmtrx_set_translation(offset.x,offset.y,offset.z);
    shz_xmtrx_rotate_y(nm_pose_radians((uint16_t)(-(int)steer[1])));
    shz_xmtrx_rotate_z(nm_pose_radians((uint16_t)(-(int)roll[2])));
    shz_xmtrx_rotate_x(nm_pose_radians((uint16_t)(-(int)phase)));
    shz_xmtrx_store_3x4(&chained);
    for(unsigned i=0;i<12;++i) if(!isfinite(chained.elem[i])) return 0;
    if(nm_pose_verify) {
    const float translation_bound=8.0f*FLT_EPSILON*
        (shz_fabsf(xyz[0])+shz_fabsf(xyz[1])+shz_fabsf(xyz[2]))+FLT_MIN;
    for(unsigned i=0;i<12;++i) {
        if(!isfinite(chained.elem[i]) || shz_fabsf(chained.elem[i]-result[i])>
                (i<9 ? 1e-5f : translation_bound)) {
            puts("FAIL: resident SH4ZAM wheel chain differs"); return 0;
        }
    }
    }
    shz_memcpy4(out,chained.elem,sizeof(chained)); return 1;
#else
    nm_memcpy(out,result,sizeof(result)); return 1;
#endif
}

/* Normal mode0 reset follow view: source6510/6590/67f8/6914/6aac.
 * Authored DND camera reset offsets/angles; ground roll and floor(pitch/2),
 * not suspension angles. Rz(camera)*Rx(camera)*Ry(camera)*T(-offset)*
 * Rz(-ground)*Rx(-ground)*Ry(-ground)*T(-position).
 * Other VR modes, view transitions and projection/UI remain separate work. */
static inline int16_t nm_follow_heading(const nm_player_t *car) {
    /* 6544..655c: body yaw byte2a, NOT ground yaw58; they differ while the car slides. */
    return nm_yaw_angle((uint32_t)((int)car->body_yaw+car->boundary.rotation+car->shake_angles[0]));
}
/* Normal reset follow camera's sky offset, original66e4..67f0. Keep the
 * source table arithmetic/rounding: this selects discrete background rows.
 * Output is the source's low12 scroll bits, without any tile-controller mode.
 * The layer2/3 vertical pair wraps every1024 rows; layout/fill is separate. */
static inline int nm_camera_sky_scroll(const nm_drive_params_t *p,
        float height,int32_t pitch,float focus,float center,uint16_t *out) {
    if (!p || !out || !isfinite(height) || !isfinite(focus) || !isfinite(center)) return 0;
    const int32_t elevation=nm_force_angle(p,2048.0f,height);
    if (elevation==INT32_MIN) return 0;
    const uint16_t angle=(uint16_t)((uint32_t)pitch-(uint32_t)elevation);
    const float product=focus*nm_drive_sine(p,angle);
    const float quotient=nm_force_divide(p,product,nm_drive_sine(p,(uint16_t)(angle+16384u)));
    const float offset=quotient+center;
    int32_t rounded;
    if (!isfinite(offset) || !nm_drive_round_i32(offset,&rounded)) return 0;
    *out=(uint16_t)((uint32_t)rounded&4095u); return 1;
}
static inline int nm_follow_sky_scroll(const nm_drive_params_t *p,
        const nm_player_t *car, uint16_t *out) {
    if (!p || !car || !out) return 0;
    const int pitch=car->boundary.ground.pitch;
    const int half=pitch<0 ? -((-pitch+1)/2) : pitch/2;
    return nm_camera_sky_scroll(p,nm_force_float(p->camera_reset_words[1])+car->boundary.ground.position[1],
        p->camera_reset_angles[0]-half,nm_force_float(p->camera_reset_words[5]),
        nm_force_float(p->camera_reset_words[4]),out);
}
/* Shared tail 66e4/67f8/6914/6aac of every car-relative view: the camera
 * entity's angles/position (copied from the target car) and the offset words
 * 501720.. / angle words 501738.. chosen by the active camera mode. */
/* spin: the in-car views' extra Ry(-spin) between the eye and the car rotations (6b90..6bb0, mode 2 with +d6
 * nonzero), 0 for none: the car frame becomes body * Ry(spin) before its rigid inverse. */
static inline int nm_car_relative_view_spin(const nm_drive_params_t *p,const float position[3],
        const int16_t angles[3],int16_t spin,const float offset[3],const int16_t authored_angles[3],float out[12]) {
    if (!p || !position || !angles || !offset || !authored_angles || !out) return 0;
    int16_t camera_angles[3];
    for (unsigned a=0;a<3;++a) camera_angles[a]=nm_yaw_angle(0u-(uint32_t)(int32_t)authored_angles[a]);
    const float zero[3]={0};
    float body[12],camera[12],inverse_body[12]={0},inverse_camera[12]={0},result[12];
    if (!nm_body_pose(p,zero,angles,body) || !nm_body_pose(p,zero,camera_angles,camera)) return 0;
    if (spin) {
        const int16_t turn[3]={0,spin,0};
        float rotation[12],turned[12];
        if (!nm_body_pose(p,zero,turn,rotation) || !nm_pose_compose(body,rotation,turned)) return 0;
        nm_memcpy(body,turned,sizeof(body));
    }
#ifdef __DREAMCAST__
    if(nm_pose_verify) {
#endif
    for (unsigned r=0;r<3;++r) for (unsigned c=0;c<3;++c) {
        inverse_body[c*3+r]=body[r*3+c];
        inverse_camera[c*3+r]=camera[r*3+c];
    }
    for (unsigned r=0;r<3;++r) {
        inverse_body[9+r]=-(inverse_body[r]*position[0]+inverse_body[3+r]*position[1]+inverse_body[6+r]*position[2]);
        inverse_camera[9+r]=-(inverse_camera[r]*offset[0]+inverse_camera[3+r]*offset[1]+inverse_camera[6+r]*offset[2]);
    }
#ifdef __DREAMCAST__
    }
    /* Both inputs are pure rotations. Transpose is their rigid inverse;
     * general matrix inversion would change the source semantics and cost. */
    for(unsigned which=0;which<2;++which) {
        const float *rotation=which ? camera : body;
        const float *translation_source=which ? offset : position;
        float *reference=which ? inverse_camera : inverse_body;
        alignas(32) shz_mat3x4_t input, inverse;
        shz_memcpy4(input.elem,rotation,sizeof(input));
        const shz_vec3_t translation=shz_vec3_neg(shz_vec3_init(translation_source[0],translation_source[1],translation_source[2]));
        shz_xmtrx_load_3x4(&input);
        shz_xmtrx_transpose();
        const shz_vec3_t rotated=shz_xmtrx_transform_vec3(translation);
        shz_xmtrx_set_translation(rotated.x,rotated.y,rotated.z);
        shz_xmtrx_store_3x4(&inverse);
        for(unsigned i=0;i<12;++i) if(!isfinite(inverse.elem[i])) return 0;
        /* Finite diagnostic retains the scalar computation above as oracle. */
        if(nm_pose_verify) {
        if(memcmp(inverse.elem,reference,9*sizeof(float))) {
            puts("FAIL: SH4ZAM camera inverse rotation differs"); return 0;
        }
        for(unsigned r=0;r<3;++r) {
            const float magnitude=shz_fabsf(reference[r]*translation_source[0])+
                shz_fabsf(reference[3+r]*translation_source[1])+shz_fabsf(reference[6+r]*translation_source[2]);
            if(!isfinite(magnitude) || shz_fabsf(inverse.elem[9+r]-reference[9+r])>
                    8.0f*FLT_EPSILON*magnitude+FLT_MIN) {
                puts("FAIL: SH4ZAM camera inverse translation exceeds bound"); return 0;
            }
        }
        }
        shz_memcpy4(reference,inverse.elem,sizeof(inverse));
    }
#endif
    if (!nm_pose_compose(inverse_camera,inverse_body,result)) return 0;
    nm_memcpy(out,result,sizeof(result)); return 1;
}
static inline int nm_car_relative_view(const nm_drive_params_t *p,const float position[3],
        const int16_t angles[3],const float offset[3],const int16_t authored_angles[3],float out[12]) {
    return nm_car_relative_view_spin(p,position,angles,0,offset,authored_angles,out);
}
static inline int nm_follow_view(const nm_drive_params_t *p, const nm_player_t *car,
                                float out[12], int16_t *heading) {
    if (!p || !car || !out || !heading) return 0;
    const nm_wheels_state_t *ground=&car->boundary.ground;
    const int pitch=ground->pitch;
    const int16_t yaw=nm_follow_heading(car);
    const int16_t angles[3]={(int16_t)(pitch<0 ? -((-pitch+1)/2) : pitch/2),yaw,ground->roll};
    float offset[3];
    nm_memcpy(offset,p->camera_reset_words,sizeof(offset));
    if (!nm_car_relative_view(p,ground->position,angles,offset,p->camera_reset_angles,out)) return 0;
    *heading=yaw; return 1;
}

/* Normal follow scenery anchor, source5890..596c: the camera entity's XZ
 * shifted backwards by (10 - camera_offset_z), NOT the player's cell/XYZ.
 * 10 is the fixed daytona93 word at28b5734 (also immediate at5a68); the host
 * original-instruction oracle checks it. Keep scalar source-order rounding:
 * this anchor feeds discrete cell/wedge decisions, not presentation FTRV. */
static inline int nm_follow_scenery_origin(const nm_drive_params_t *p,
        const nm_player_t *car, int16_t heading, float out[2], uint16_t *cell) {
    if (!p || !car || !out || !cell) return 0;
    float offset_z; nm_memcpy(&offset_z,p->camera_reset_words+2,sizeof(offset_z));
    if (!isfinite(offset_z)) return 0;
    const float distance=-offset_z+10.0f;
    const float dx=nm_drive_sine(p,(uint16_t)heading)*distance;
    const float dz=nm_drive_sine(p,(uint16_t)(heading+16384))*distance;
    const float x=dx+car->boundary.ground.position[0];
    const float z=car->boundary.ground.position[2]-dz;
    uint16_t next_cell;
    if (!nm_contact_scenery_cell(x,z,&next_cell)) return 0;
    out[0]=x; out[1]=z; *cell=next_cell; return 1;
}
#endif
