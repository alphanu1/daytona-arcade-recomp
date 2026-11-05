#ifndef DAYTONA_NATIVE_RACE_VIEW_H
#define DAYTONA_NATIVE_RACE_VIEW_H
#include "pose.h"
#include "nm_mem.h"

/* The race views (VR1..VR4, view presets 0..3 of the table 1d00220; build/dc-native-views.PLAN.md):
 *   0 VR1 driver's view  mode 2, eye (0, 1.05, +2.15), pitch 728,  centre -20, focus 240: no player car drawn
 *   1 VR2 bumper/hood    mode 2, eye (0, 1.10, +0.20), pitch 910,  centre -30, focus 250: the hood drawn
 *   2 VR3 behind (race)  mode 0, eye (0, 1.85, -3.98), pitch 3276, centre -50, focus 210
 *   3 VR4 behind, high   mode 0, eye (0, 3.74, -6.64), pitch 3094, centre -8,  focus 260
 * (the values are the ROM's; the table comes from the race-start header). The view matrix feeds the detail
 * levels whose count the opponents read on the next tick (car_detail.h), and the eye forward word the scenery
 * anchor: a view change changes the race, in the original too. Everything that reaches those is computed in
 * source order on the host's exact float operations; the presentation-only values (focus, window centre, sky
 * rows, the drawn parts) come from the same words. */

/* 53e120, the held inputs (1c070..1c084): START bit 4, VR1..VR3 bits 5..7, VR4 bit 16. */
enum { NM_VIEW_HELD_START=1u<<4, NM_VIEW_HELD_VR1=1u<<5, NM_VIEW_HELD_VR2=1u<<6, NM_VIEW_HELD_VR3=1u<<7,
       NM_VIEW_HELD_VR4=1u<<16 };
/* 8858..8870: the view buttons act in the phases 501090 & 0x7800c0 (6, 7, 19..22). */
enum { NM_VIEW_PHASES=0x7800c0u };

/* 8874..88b4: no change while START is held (the buttons zoom the CONDITION panel then, hud.h); else the
 * first held of VR1, VR2, VR3, VR4 (held, not pressed: holding a button keeps its view). */
static inline unsigned nm_race_view_pick(uint32_t held,unsigned view) {
    if(held&NM_VIEW_HELD_START) return view;
    const uint32_t buttons=held&(NM_VIEW_HELD_VR1|NM_VIEW_HELD_VR2|NM_VIEW_HELD_VR3|NM_VIEW_HELD_VR4);
    if(!buttons) return view;
    return buttons&NM_VIEW_HELD_VR1 ? 0u : buttons&NM_VIEW_HELD_VR2 ? 1u : buttons&NM_VIEW_HELD_VR3 ? 2u : 3u;
}

/* 8814..8a28, the end of the camera pass (6b74, after the view and the detail pass): in mode 0 or 2 during the
 * view phases, the held buttons pick the view 501710 and 5014a4 := its preset's mode at once; the camera words
 * move an eighth of the way to their targets every tick (TGP 1, scale by 2^-3, TGP 0; pitch: integer difference,
 * arithmetic shift 3): the preset's in those phases, else the reset record's 28b5714.., with 5014a4 unchanged
 * (the finish, results, keyed cameras). 501720 (x), 50172c and the yaw and roll angles are not moved. Then
 * 17fb0/17ffc copy the focus 501734 and 501730 into the window records (presentation). */
static inline int nm_race_view_update(const nm_drive_params_t *p,const nm_race_view_preset_t presets[4],
        uint32_t flags,uint32_t held,uint8_t *view,nm_player_t *player) {
    if(!p || !presets || !view || *view>3 || !player) return 0;
    uint32_t target[4]={p->camera_reset_words[1],p->camera_reset_words[2],p->camera_reset_words[4],
        p->camera_reset_words[5]};
    int16_t pitch=p->camera_reset_angles[0];
    unsigned next=*view;int32_t mode=player->camera_mode;
    if((mode==0 || mode==2) && (flags&NM_VIEW_PHASES)) {
        next=nm_race_view_pick(held,next);
        const nm_race_view_preset_t *preset=presets+next;
        nm_memcpy(target,preset->words,sizeof(target));pitch=preset->pitch;mode=preset->mode;
    }
    static const uint8_t word[4]={1,2,4,5};
    uint32_t eased[4];
    for(unsigned i=0;i<4;++i) {
        const float current=nm_force_float(player->camera_words[word[i]]),goal=nm_force_float(target[i]);
        const float difference=goal-current;
        const float step=difference*0.125f; /* scaler -3: exact */
        const float result=step+current;
        if(!isfinite(result)) return 0;
        eased[i]=nm_force_bits(result);
    }
    for(unsigned i=0;i<4;++i) player->camera_words[word[i]]=eased[i];
    player->camera_angles[0]=(int16_t)(uint16_t)((uint32_t)(int32_t)player->camera_angles[0]+
        (uint32_t)nm_guided_shift((uint32_t)((int32_t)pitch-(int32_t)player->camera_angles[0]),3));
    player->camera_mode=mode;*view=(uint8_t)next;
    return 1;
}

/* 6938..69b0, for every camera mode (also the keyed ones): in the driver's view (501710 = 0) the eye's forward
 * offset is scaled by 1 - |+d6 / 7300| (TGP 2 by 0x390fa7a8, sign set, TGP 0 with 1.0), and is 0 once that
 * is not above 0 (integer compare): a spinning car pulls the eye back to its centre. Input and result are the
 * eye word itself (the source scales its negation, the same bits but the sign). */
static inline float nm_race_view_eye_z(float z,unsigned view,int16_t spin) {
    if(view) return z;
    const float product=nm_force_float(0x390fa7a8u)*(float)spin;
    const float factor=1.0f+nm_force_float(nm_force_bits(product)|UINT32_C(0x80000000));
    const uint32_t bits=nm_force_bits(factor);
    const float negated=nm_force_float(nm_force_bits(z)^UINT32_C(0x80000000)); /* 692c notbit 31 */
    const float scaled=(bits&UINT32_C(0x80000000)) || !bits ? 0.0f : factor*negated;
    return nm_force_float(nm_force_bits(scaled)^UINT32_C(0x80000000));
}

/* 69b4..6a8c, after 6938, for every camera mode: off course 0 (501460), in all views but VR4 (501710 = 3), a camera
 * entity pitch +28 of -546 or less turns the eye about x by pitch + 546, clamped at -1092 (1d/1e: the sine table, TGP 1
 * and 0 for the sums). +28 is half the ground pitch in modes 0 and 2 (6598), the full one under the crash camera (669c)
 * and 0 under the start and pit tracks (667c, 501713 = 1). The source turns the negated words r5, r6 and keeps them in
 * registers: +b8, +bc and the scenery anchor keep the unturned eye. Returns 1 when it turned. */
static inline int nm_race_view_eye_turn(const nm_drive_params_t *p,unsigned course,unsigned view,int16_t pitch,float eye[3]) {
    if(!course || view==3 || pitch>-546) return 0;
    const int32_t angle=pitch+546<-1092 ? -1092 : pitch+546;
    const float sine=nm_drive_sine(p,(uint16_t)angle),cosine=nm_drive_sine(p,(uint16_t)(angle+16384));
    const float r5=nm_force_float(nm_force_bits(eye[1])^UINT32_C(0x80000000));
    const float r6=nm_force_float(nm_force_bits(eye[2])^UINT32_C(0x80000000));
    const float g0=cosine*r6,g1=sine*r5,g2=sine*r6,g3=cosine*r5;
    const float y=g3+g2,z=g0-g1;
    eye[1]=nm_force_float(nm_force_bits(y)^UINT32_C(0x80000000));
    eye[2]=nm_force_float(nm_force_bits(z)^UINT32_C(0x80000000));
    return 1;
}

/* The camera entity's angles and heading in modes 0 and 2 (6518..6590): mode 0 the body yaw +2a, mode 2 the drawn
 * yaw +4e, each plus +ee and the shake 501834; the heading 5014a8 adds +d6 in mode 2. Pitch is the ground
 * pitch shifted right once, roll the ground roll. */
static inline int nm_race_view_angles(const nm_player_t *car,int16_t angles[3],int16_t *heading) {
    if(!car || !angles || !heading || (car->camera_mode!=0 && car->camera_mode!=2)) return 0;
    const int inside=car->camera_mode==2;
    const int16_t yaw=nm_yaw_angle((uint32_t)((int)(inside ? car->render_angles[1] : car->body_yaw)+
        car->boundary.rotation+car->shake_angles[0]));
    const int pitch=car->boundary.ground.pitch;
    angles[0]=(int16_t)(pitch<0 ? -((-pitch+1)/2) : pitch/2);angles[1]=yaw;angles[2]=car->boundary.ground.roll;
    *heading=inside ? nm_yaw_angle((uint32_t)((int)yaw+car->inhibit_angle)) : yaw;
    return 1;
}

/* 6510..6b58 in modes 0 and 2 for the selected view: Rz Rx Ry(501738..) T(-eye) [Ry(-+d6) in mode 2 with +d6 nonzero,
 * 6b90..6bb0] Rz Rx Ry(-angles) T(-position), with the live eye words (6938, 69b4) and angles. Mode 0 in view 2 is
 * nm_follow_view on course 0 and off it above the turn's pitch. */
static inline int nm_race_view_camera(const nm_drive_params_t *p,const nm_player_t *car,unsigned course,unsigned view,
        float out[12],int16_t *heading) {
    if(!p || !car || !out || !heading || view>3) return 0;
    int16_t angles[3],yaw;
    if(!nm_race_view_angles(car,angles,&yaw)) return 0;
    float eye[3];
    for(unsigned a=0;a<3;++a) eye[a]=nm_force_float(car->camera_words[a]);
    eye[2]=nm_race_view_eye_z(eye[2],view,car->inhibit_angle);
    (void)nm_race_view_eye_turn(p,course,view,angles[0],eye);
    const int16_t spin=car->camera_mode==2 ? car->inhibit_angle : 0;
    if(!nm_car_relative_view_spin(p,car->boundary.ground.position,angles,spin,eye,car->camera_angles,out)) return 0;
    *heading=yaw;return 1;
}

/* 5890..596c with the live eye forward word (the camera entity's +bc, 65e4) and heading 5014a8. */
static inline int nm_race_view_scenery_origin(const nm_drive_params_t *p,const nm_player_t *car,int16_t heading,
        float out[2],uint16_t *cell) {
    if(!p || !car || !out || !cell) return 0;
    const float offset_z=nm_force_float(car->camera_words[2]);
    if(!isfinite(offset_z)) return 0;
    const float distance=-offset_z+10.0f;
    const float dx=nm_drive_sine(p,(uint16_t)heading)*distance;
    const float dz=nm_drive_sine(p,(uint16_t)(heading+16384))*distance;
    const float x=dx+car->boundary.ground.position[0];
    const float z=car->boundary.ground.position[2]-dz;
    uint16_t next_cell;
    if(!nm_contact_scenery_cell(x,z,&next_cell)) return 0;
    out[0]=x;out[1]=z;*cell=next_cell;return 1;
}

/* 66e4..67f0 with the live words: eye height 501724 plus the car's, the camera pitch 501738 less half the ground
 * pitch, the focus 501734 and the centre offset 501730. Presentation (the background rows). */
static inline int nm_race_view_sky_scroll(const nm_drive_params_t *p,const nm_player_t *car,uint16_t *out) {
    if(!p || !car || !out) return 0;
    const int pitch=car->boundary.ground.pitch;
    const int half=pitch<0 ? -((-pitch+1)/2) : pitch/2;
    return nm_camera_sky_scroll(p,nm_force_float(car->camera_words[1])+car->boundary.ground.position[1],
        car->camera_angles[0]-half,nm_force_float(car->camera_words[5]),nm_force_float(car->camera_words[4]),out);
}

#endif
