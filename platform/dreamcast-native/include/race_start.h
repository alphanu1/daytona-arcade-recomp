#ifndef DAYTONA_NATIVE_RACE_START_H
#define DAYTONA_NATIVE_RACE_START_H
#include "intro_math.h"
#include "race_field.h"
#include "visibility.h"

/* ab78..ad30, first call of the phase22 handler (course0, stage501850<64): the
 * rolling-start sequence is keyed to the player's profile BEFORE this tick's
 * movement. 29/19/9 profiles before the wrap step stages0->1->2->3; stage1->2
 * also runs 62b4, which ends the start camera: selected view mode, transition0,
 * reset follow words/angles and the auxiliary object's bit31. From profile<=5a
 * the stage counts every tick, and leaving stage3 raises signal501a02=26.
 * The 18dec/18e2c UI rows and 19bdc sound commands ae2109..ae210c stay silent. */
static inline int nm_race_start_sequence(const nm_drive_params_t *p,int16_t *stage,
        nm_player_t *player,unsigned wrap,int8_t restored_camera_mode,const nm_race_view_preset_t *preset) {
    if(!p || !stage || !player || !wrap || wrap>4096) return 0;
    if(*stage>=64) return 1;
    const int remaining=(int)wrap-player->phase[0];
    if(remaining>=(int)wrap-0x5a) {
        int next=*stage;
        if(next==3) { ++next;player->boundary.pivot_signal=26; }
        *stage=(int16_t)(next+1);
    } else if(remaining==29) {
        if(*stage==0) *stage=1;
    } else if(remaining==19) {
        if(*stage==1) {
            *stage=2;
            nm_player_view_reset(p,player,restored_camera_mode,preset);
        }
    } else if(remaining==9 && *stage==2) *stage=3;
    return 1;
}

/* Start signal, object 501248 (enabled by the phase19 entry 31c4..31e0, init c010): the standing start
 * of every race whose lap count 5010ec is not 8 (c0a0..c0d0 disables it at once on course 0). Gameplay
 * part only, one call per tick in its task pass with this tick's 5010a8 (the phase20 countdown, after
 * the handler 3458 decremented it); its lights (c234, c6a4, c2c0, c4b4) and sounds (c594, 19bdc) are
 * presentation. While 5010a8 >= 0 (c0d4, then c114 from 5010a8 < 192):
 *  - c130 (start stage 501850 nonzero): the engine revs with the accelerator 501054:
 *    rpm += clamp((accel << 5) - rpm) >> 2 (arithmetic) to -192..750, then rpm in 500..8500, stored
 *    to +150 and +154;
 *  - c594 at 5010a8 == 192: 62b4, the view reset that ends the start camera;
 *  - c594 at 5010a8 == 0 (c1c0 -> c228): signal 501a02 := 26, as the rolling start's stage 3 exit. */
static inline int nm_race_start_signal(const nm_drive_params_t *p,nm_player_t *player,uint32_t timer,
        int16_t stage,uint8_t total_laps,uint32_t accel,int8_t restored_camera_mode,const nm_race_view_preset_t *preset) {
    if(!p || !player) return 0;
    if(total_laps==8 || (timer&UINT32_C(0x80000000))) return 1;
    if(stage) {
        int32_t step=(int32_t)((uint32_t)accel<<5)-player->rpm;
        step>>=2;
        if(step< -192) step=-192;
        else if(step>750) step=750;
        int32_t rpm=player->rpm+step;
        if(rpm<500) rpm=500;
        else if(rpm>8500) rpm=8500;
        player->rpm=player->filtered_rpm=rpm;
    }
    if(timer==192) nm_player_view_reset(p,player,restored_camera_mode,preset);
    else if(!timer) player->boundary.pivot_signal=26;
    return 1;
}

/* 5b88 selects the course key table 6474 and 5c70 starts it at tick0 in camera
 * mode1 (transition byte501713=1); 5cac..61fc then plays it once per camera
 * pass with the live cars as anchors (6200), exactly as the intro sequences.
 * Keys are prepared offline (race_start_data.h); no ROM pointers on target.
 * Scenery anchor 5970..5ae8 for mode1: the eye words 501720/501728 moved 10
 * units along the camera heading 5014a8 (the camera entity itself sits at the
 * origin with yaw0), same scalar order as the follow anchor. Caller runs car
 * detail/guided mode/bitmaps afterwards. */
static inline int nm_race_start_camera(const nm_race_car_t *cars,unsigned count,
        const nm_drive_params_t *math,const nm_scenery_pack_t *scenery,
        const nm_intro_key_t *keys,const uint32_t *times,unsigned key_count,
        nm_intro_anchor_t anchors[44],nm_intro_cursor_t *cursor,nm_intro_camera_t *camera,
        float view[12],int16_t *heading,uint16_t *sky_scroll,nm_scenery_cells_t *cells) {
    if(!cars || !count || count>44 || !math || !scenery || !keys || !times || !anchors ||
       !cursor || !camera || !view || !heading || !sky_scroll || !cells) return 0;
    for(unsigned i=0;i<count;++i) {
        const nm_wheels_state_t *g=&cars[i].car.boundary.ground;
        for(unsigned a=0;a<3;++a) anchors[i].position[a]=g->position[a];
        anchors[i].angles[0]=g->pitch;anchors[i].angles[1]=cars[i].car.body_yaw;
        anchors[i].angles[2]=g->roll;
    }
    nm_intro_interval_t interval;
    if(!nm_intro_camera_step(times,key_count,cursor,&interval) ||
       !nm_intro_camera_sample(math,keys+interval.key,keys+interval.key+1,
            anchors,count,interval.elapsed,interval.duration,camera) ||
       !nm_intro_camera_view(math,camera,view,heading,sky_scroll)) return 0;
    const float dx=nm_drive_sine(math,(uint16_t)*heading)*10.0f;
    const float dz=nm_drive_sine(math,(uint16_t)(*heading+16384))*10.0f;
    const float x=dx+camera->position[0],z=camera->position[2]-dz;
    uint16_t cell;
    return nm_contact_scenery_cell(x,z,&cell) &&
        nm_scenery_select(scenery,math,cell,(uint16_t)cars[0].car.phase[0],x,z,*heading,cells);
}

/* Crash camera. 15d18..15d38 (crash start outside flags1f800000) re-arms the
 * auxiliary object 501270 with 5c14: transition byte501713=0, the car-relative
 * key table named at 5c20 and, through 5c70, tick0 and camera mode1. Its keys
 * have no anchors: 5cac leaves the eye in the offset words 501720.. and the
 * look-at angles in 501738.., and view 660c/669c hangs them on the target car
 * (full pitch2a-less angles 28/2a/2c and position, heading5014a8 = body yaw -
 * key yaw) through the same tail as the follow view. Scenery anchor 5970..5ae8
 * with the camera entity at the car. 15d94/637c (nm_damage_intro_step) ends it. */
static inline int nm_race_crash_camera(const nm_player_t *car,const nm_drive_params_t *math,
        const nm_scenery_pack_t *scenery,const nm_intro_key_t *keys,const uint32_t *times,
        unsigned key_count,nm_intro_cursor_t *cursor,nm_intro_camera_t *camera,
        float view[12],int16_t *heading,uint16_t *sky_scroll,nm_scenery_cells_t *cells) {
    if(!car || !math || !scenery || !keys || !times || !cursor || !camera || !view ||
       !heading || !sky_scroll || !cells) return 0;
    nm_intro_interval_t interval;
    int16_t authored[3];
    if(!nm_intro_camera_step(times,key_count,cursor,&interval) ||
       !nm_intro_camera_sample(math,keys+interval.key,keys+interval.key+1,
            NULL,0,interval.elapsed,interval.duration,camera) ||
       !nm_intro_camera_angles(math,camera,authored)) return 0;
    const nm_wheels_state_t *g=&car->boundary.ground;
    const int16_t angles[3]={g->pitch,car->body_yaw,g->roll};
    if(!nm_car_relative_view(math,g->position,angles,camera->position,authored,view)) return 0;
    const int16_t yaw=nm_yaw_angle((uint32_t)((int)car->body_yaw-authored[1]));
    /* Source order: rotate the negated eye offset by -yaw, subtract from the
     * car, then the 10-unit shift along the heading. */
    const uint16_t turn=(uint16_t)(0u-(uint16_t)car->body_yaw);
    const float sine=nm_drive_sine(math,turn),cosine=nm_drive_sine(math,(uint16_t)(turn+16384));
    const float ox=nm_force_float(nm_force_bits(camera->position[0])^UINT32_C(0x80000000));
    const float oz=nm_force_float(nm_force_bits(camera->position[2])^UINT32_C(0x80000000));
    const float rx=ox*cosine-oz*sine,rz=oz*cosine+ox*sine;
    const float bx=g->position[0]-rx,bz=g->position[2]-rz;
    const float dx=nm_drive_sine(math,(uint16_t)yaw)*10.0f;
    const float dz=nm_drive_sine(math,(uint16_t)(yaw+16384))*10.0f;
    const float x=dx+bx,z=bz-dz;
    uint16_t cell;
    if(!isfinite(x) || !isfinite(z) || !nm_contact_scenery_cell(x,z,&cell) ||
       !nm_scenery_select(scenery,math,cell,(uint16_t)car->phase[0],x,z,yaw,cells)) return 0;
    /* Background rows as the follow camera derives them (66e4..67f0), with the
     * key's eye height, pitch and focus. Presentation only. */
    if(!nm_camera_sky_scroll(math,camera->position[1]+g->position[1],authored[0]-g->pitch,
        camera->focus,nm_force_float(math->camera_reset_words[4]),sky_scroll)) return 0;
    *heading=yaw;
    return 1;
}
#endif
