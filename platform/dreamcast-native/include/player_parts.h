#ifndef DAYTONA_NATIVE_PLAYER_PARTS_H
#define DAYTONA_NATIVE_PLAYER_PARTS_H
#include "pose.h"
#include "nm_mem.h"

/* The race player is not a detail-list car: 8480..84dc lists the pool at
 * 501224 only, so entity 501220 is never level-selected or budgeted. Its draw
 * submits one fixed list in this order every frame (display-list log of the
 * original, every race frame): body 95ff9d (6c40/6dc0), glass 13b (polygon
 * RAM, reflection UVs), cutout shell 961bbf, the four wheels 7384..7664
 * (8d7b13/8d7c9a alternating, front pair first), ground shadow 8d7ab7.
 * Glass and shell reuse the body matrix. The shadow has two source paths:
 * the crash block 6dc0..6ec4 rebuilds the flattened ground pose
 * (nm_player_part_pose shadow), while the normal block 6ecc..6f0c draws it
 * with the matrix the wheel routine left: body times the pitch/bank undo. */
enum {
    NM_PLAYER_PART_BODY, NM_PLAYER_PART_GLASS, NM_PLAYER_PART_SHELL,
    NM_PLAYER_PART_WHEEL, NM_PLAYER_PART_SHADOW=NM_PLAYER_PART_WHEEL+4, NM_PLAYER_PARTS
};
static const uint32_t nm_player_part_models[NM_PLAYER_PARTS]={
    0x95ff9d,0x13b,0x961bbf,0x8d7b13,0x8d7c9a,0x8d7b13,0x8d7c9a,0x8d7ab7};
/* Hood models of the automatic car, 231eb0[0][i] (lists 288d9b8, 288ebc4, 288ebb0, 288ebd8). */
static const uint32_t nm_player_hood_models[4]={0x94fff9,0xa0092b,0xa0027c,0xa00fda};
/* The manual transmission's car (501464 = 1): 7358..7374 index the body list 286fab0[501464] (the grid
 * reset e5ac writes it to +14: 288eb4c, model 960dae) and the shell 286fab8[501464] (288eb74, 961cb0), 7340 the
 * hoods 231eb0[1][i] (288d9cc, 288ec00, 288ebec, 288ec14). Body and shell have the automatic car's vertices,
 * u,v and faces (car pack and race export: the same 584 vertices, the same corners of every face) with other
 * texture headers (header word 3 of 156 of the 325 body faces): the colours. Glass, wheels and shadow are
 * shared. Damaged bodies crash_model[1][d] (235b60+8d) are drawn as car-damage.dnb's variants of the
 * automatic ones in the manual body's face order (render/scene/practice.c). */
static const uint32_t nm_player_manual_models[2]={0x960dae,0x961cb0}; /* body, shell */
static const uint32_t nm_player_manual_hood_models[4]={0x9506a8,0xa01d38,0xa01689,0xa023e7};
/* The model of part `part` for transmission `manual` (0 automatic, 1 manual). */
static inline uint32_t nm_player_part_model(unsigned part,unsigned manual) {
    if(manual && part==NM_PLAYER_PART_BODY) return nm_player_manual_models[0];
    if(manual && part==NM_PLAYER_PART_SHELL) return nm_player_manual_models[1];
    return part<NM_PLAYER_PARTS ? nm_player_part_models[part] : 0;
}

/* The body model +14 the draw takes (7358), from the damage word +208: the
 * writers 16d08..16d28 (corner hit: crash_model[v][d&15] unless crashed),
 * 15cc8/15cd4..15cfc (crash start, bit 8: crash_model[v][0]) and de04..dee0
 * (pit repair: bits 0-3 and 8 cleared, clean body) leave exactly
 *   bit 8 ? crash_model[v][0] : d&15 ? crash_model[v][d&15] : clean.
 * Result: the crash_model index (word 235b5c+4v+8d), or -1 for the clean body. */
static inline int nm_player_damage_body(uint32_t damage) {
    return damage&256u ? 0 : damage&15u ? (int)(damage&15u) : -1;
}
/* 7328..7350: hood list 231eb0[v][i], i = 3 once crashed, else the two front
 * corners (bit 2: wheel 1, bit 3: wheel 0): 231ea0[d&15]/4 = (d>>2)&3. */
static inline unsigned nm_player_hood(uint32_t damage) {
    return damage&256u ? 3u : (damage>>2)&3u;
}
/* What 6c0c..6f0c draws for the view: view mode 5014a4 (byte +13 of the view
 * preset 501710, 637c/88e8; 1 for keyed cameras, 5ca4) and the eye word 501728
 * as raw bits (the view updater moves it toward the preset's word +4: VR1
 * 2.15, VR2 0.2). Mode 2, sign clear and bits above 2.1 (0x40066666, integer
 * compare, 6d70..6d98): the driver's view, neither body nor wheels. Else in
 * mode 2 the body routine draws the hood unless -0.45 > eye (7300..7324), and
 * the wheel routine draws nothing when -0.45 < eye (7384..73ac; at exactly
 * -0.45 both the hood and the wheels). */
enum { NM_PLAYER_SHOW_BODY=1u, NM_PLAYER_SHOW_HOOD=2u, NM_PLAYER_SHOW_WHEELS=4u };
static inline unsigned nm_player_show(uint32_t mode,uint32_t eye) {
    if(mode!=2) return NM_PLAYER_SHOW_BODY|NM_PLAYER_SHOW_WHEELS;
    if(!(eye>>31) && (int32_t)eye>0x40066666) return 0;
    float w; nm_memcpy(&w,&eye,sizeof(w));
    const float limit=-0.45f; /* 0xbee66666 */
    return (limit>w ? NM_PLAYER_SHOW_BODY : NM_PLAYER_SHOW_HOOD)|(limit<w ? 0u : NM_PLAYER_SHOW_WHEELS);
}
/* The parts drawn, bit NM_PLAYER_PART_* each, in part order: the body slot
 * holds the body or the hood (7350: no glass, no shell); the shadow 288b6f4
 * (6ebc/6f04) is drawn in every view. */
static inline unsigned nm_player_part_mask(unsigned show) {
    unsigned mask=1u<<NM_PLAYER_PART_SHADOW;
    if(show&NM_PLAYER_SHOW_BODY) mask|=1u<<NM_PLAYER_PART_BODY|1u<<NM_PLAYER_PART_GLASS|1u<<NM_PLAYER_PART_SHELL;
    if(show&NM_PLAYER_SHOW_HOOD) mask|=1u<<NM_PLAYER_PART_BODY;
    if(show&NM_PLAYER_SHOW_WHEELS) mask|=15u<<NM_PLAYER_PART_WHEEL;
    return mask;
}

/* View-space matrices of every part for the view's show bits. wheel_offsets
 * are the authored render offsets 234af4 carried by the car pack, never
 * captured wheel matrices. The normal shadow takes the matrix the wheel
 * routine leaves: its pitch/bank undo (7420..) comes after the hood/driver's
 * view exit 73ac, so without wheels the shadow keeps the body matrix. */
static inline int nm_player_parts_shown(const nm_drive_params_t *p,const nm_player_t *car,
        const float view[12],const float wheel_offsets[4][3],unsigned show,float out[NM_PLAYER_PARTS][12]) {
    if(!p || !car || !view || !wheel_offsets || !out) return 0;
    float body[12],part[12],result[NM_PLAYER_PARTS][12];
    if(!nm_player_part_pose(p,car,0,body) || !nm_pose_compose(view,body,result[NM_PLAYER_PART_BODY])) return 0;
    nm_memcpy(result[NM_PLAYER_PART_GLASS],result[NM_PLAYER_PART_BODY],sizeof(result[0]));
    nm_memcpy(result[NM_PLAYER_PART_SHELL],result[NM_PLAYER_PART_BODY],sizeof(result[0]));
    for(unsigned wheel=0;wheel<4;++wheel)
        if(!nm_wheel_pose(p,car,wheel,wheel_offsets[wheel],part) ||
           !nm_pose_compose(result[NM_PLAYER_PART_BODY],part,result[NM_PLAYER_PART_WHEEL+wheel])) return 0;
    if(car->boundary.damage.overlay_model) {
        if(!nm_player_part_pose(p,car,1,part) || !nm_pose_compose(view,part,result[NM_PLAYER_PART_SHADOW])) return 0;
    } else if(!(show&NM_PLAYER_SHOW_WHEELS)) {
        nm_memcpy(result[NM_PLAYER_PART_SHADOW],result[NM_PLAYER_PART_BODY],sizeof(result[0]));
    } else {
        const float zero[3]={0};
        const int16_t undo[3]={nm_yaw_angle(0u-(uint32_t)(int32_t)car->filtered_pitch),0,
            nm_yaw_angle(0u-(uint32_t)(int32_t)car->bank)};
        if(!nm_body_pose(p,zero,undo,part) ||
           !nm_pose_compose(result[NM_PLAYER_PART_BODY],part,result[NM_PLAYER_PART_SHADOW])) return 0;
    }
    nm_memcpy(out,result,sizeof(result));
    return 1;
}
static inline int nm_player_parts(const nm_drive_params_t *p,const nm_player_t *car,
        const float view[12],const float wheel_offsets[4][3],float out[NM_PLAYER_PARTS][12]) {
    return nm_player_parts_shown(p,car,view,wheel_offsets,NM_PLAYER_SHOW_BODY|NM_PLAYER_SHOW_WHEELS,out);
}
#endif
