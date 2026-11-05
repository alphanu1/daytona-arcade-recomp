#ifndef DAYTONA_NATIVE_PIT_CREW_H
#define DAYTONA_NATIVE_PIT_CREW_H
#include "pit.h"
#include "pose.h"
#include "nm_mem.h"
/* NM_PIT_CREW_SHZ: the SH4ZAM chains (the Dreamcast's). A host build may select them (-DNM_PIT_CREW_SHZ=1, SH4ZAM's
 * include directory and its software back-end sources: tests/trace_race_native.c --parts then checks every crew
 * matrix against the scalar reference, nm_pose_verify). */
#if defined(__DREAMCAST__) && !defined(NM_PIT_CREW_SHZ)
#define NM_PIT_CREW_SHZ 1
#endif
#if defined(NM_PIT_CREW_SHZ) && !defined(__DREAMCAST__)
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
/* The SH4's FSCA truncates radians * 10430.38 to a 16-bit bin (hence pose.h's bin centre); sinf/cosf here take the
 * angle itself. */
static inline float nm_pose_radians(uint16_t angle) { return (float)angle*(1.0f/SHZ_FSCA_RAD_FACTOR); }
static int nm_pose_verify=1;
/* SH4ZAM 0.9.0's software back-end multiplied the translation column by its apply_rotation_* matrix too (rot[3]
 * was XMTRX's own column 3; 0.10.0's rotate_* keeps it as the SH4 does); the SH4 back-end (shz_xmtrx_sh4.inl.h: two
 * FTRVs into columns 0 and 1 for Z) transforms the inner 3x3 only, XMTRX = XMTRX R. The host check models the SH4's. */
static inline void nm_pit_crew_sw_rotation(unsigned axis,float radians) {
    alignas(32) shz_mat3x4_t m;
    shz_xmtrx_store_3x4(&m);
    const float c=cosf(radians),s=sinf(radians);
    const unsigned a=axis==0 ? 1u : 0u,b=axis==2 ? 1u : 2u; /* the two columns the turn mixes */
    float *u=m.elem+3*a,*v=m.elem+3*b,ou[3],ov[3];
    for(unsigned r=0;r<3;++r) { ou[r]=u[r];ov[r]=v[r]; }
    /* Rx: (y, z), Rz: (x, y) with +s into the first; Ry: (x, z) with -s into the first (right-handed). */
    const float sign=axis==1 ? -1.0f : 1.0f;
    for(unsigned r=0;r<3;++r) { u[r]=c*ou[r]+sign*s*ov[r];v[r]=c*ov[r]-sign*s*ou[r]; }
    shz_xmtrx_load_3x4(&m);
}
#undef shz_xmtrx_rotate_x
#undef shz_xmtrx_rotate_y
#undef shz_xmtrx_rotate_z
#define shz_xmtrx_rotate_x(r) nm_pit_crew_sw_rotation(0,(r))
#define shz_xmtrx_rotate_y(r) nm_pit_crew_sw_rotation(1,(r))
#define shz_xmtrx_rotate_z(r) nm_pit_crew_sw_rotation(2,(r))
#endif

/* The pit crew's draw (spec: build/dc-native-pit.xjs2cE/spec-pitstop.md section 3), presentation only: the six
 * skeletal figures of task rows 68..73 as the arcade's 1aca4 draws them on every tick the rows run after their
 * first (pit.h nm_pit_t.crew_drawn), from the state nm_pit_crew_tick leaves (nm_pit_crew_t.drawn_*) and the data
 * of the race header (nm_pit_crew_art_t). Per crew, in the arcade's order:
 *   1b2a4 -> 1b33c (crews 0..3): the held items by +68 bits 1, 2, 3: V T(position) Ry(heading) T(item) [Rz(item)];
 *   1ada8: the pose, keyframe (frame >> 1) of the motion; when the frame is odd the arcade moves HALF A STEP AWAY
 *          from the next keyframe b (1af50: TGP a - b, times 0.5, plus a; 1afcc: subi gives a - b, then
 *          a + (int16)(a - b) / 2, truncated): the log's matrices agree with this, not with a midpoint;
 *   1b2f0 -> 1b4c0: 16 parts, root V T(position) Ry(heading) T(pose) Ry Rx Rz (angles 0..2), every other part
 *          its parent T(23ac98[k-1]) then Rx Rz Ry or Rz alone (pit_crew_parts), the two feet (12, 15) the parent
 *          T(offset) with its rotation replaced by the identity (0x1111 read, 0x707 load of 501740), then
 *          Rx(angle) Ry(heading - camera heading 5014a8 + second heading +64).
 * Angles are 16-bit units with the source's rotation sign (pose.h). All matrices are view space (column-major
 * 3x4, the renderer's instance matrices). On the Dreamcast the chains stay resident in XMTRX through SH4ZAM; the
 * host (and nm_pose_verify) uses the scalar compositions of pose.h. */

enum { NM_PIT_CREW_DRAWS_MAX=NM_PIT_CREW*NM_PIT_CREW_PARTS+4*NM_PIT_CREW_ITEMS };
typedef struct { uint32_t model; alignas(16) float matrix[12]; } nm_pit_crew_draw_t;

/* 1b4c0's tree: parent part, rotation kind (3: Rx Rz Ry, 1: Rz, 2: the foot's Rx and camera-relative Ry) and
 * the first pose angle (int16 index after +80: X, then Y, then Z for kind 3). */
enum { NM_PIT_CREW_TURN=1, NM_PIT_CREW_FOOT=2, NM_PIT_CREW_JOINT=3 };
static const struct { uint8_t parent, kind, angle; } nm_pit_crew_tree[NM_PIT_CREW_PARTS]={
    {0,0,0},
    {0,NM_PIT_CREW_JOINT,3},{1,NM_PIT_CREW_JOINT,6},
    {1,NM_PIT_CREW_JOINT,9},{3,NM_PIT_CREW_TURN,12},{4,NM_PIT_CREW_TURN,13},
    {1,NM_PIT_CREW_JOINT,14},{6,NM_PIT_CREW_TURN,17},{7,NM_PIT_CREW_TURN,18},
    {0,NM_PIT_CREW_JOINT,19},{9,NM_PIT_CREW_JOINT,22},{10,NM_PIT_CREW_TURN,25},{11,NM_PIT_CREW_FOOT,26},
    {9,NM_PIT_CREW_JOINT,27},{13,NM_PIT_CREW_TURN,30},{14,NM_PIT_CREW_FOOT,31}};

static inline const nm_pit_crew_motion_t *nm_pit_crew_motion(const nm_pit_crew_art_t *art,uint32_t address) {
    for(unsigned m=0;m<art->motion_count && m<NM_PIT_CREW_MOTIONS;++m)
        if(art->motions[m].address==address) return art->motions+m;
    return NULL;
}
static inline const nm_pit_crew_set_t *nm_pit_crew_set(const nm_pit_crew_art_t *art,uint32_t address) {
    for(unsigned s=0;s<art->set_count && s<NM_PIT_CREW_SETS;++s)
        if(art->sets[s].address==address) return art->sets+s;
    return NULL;
}

/* 1ada8: the pose of this tick's draw. */
static inline int nm_pit_crew_pose(const nm_pit_crew_art_t *art,const nm_pit_crew_t *k,nm_pit_crew_key_t *out) {
    const nm_pit_crew_motion_t *motion=nm_pit_crew_motion(art,k->drawn_motion);
    if(!motion || k->drawn_frame<0) return 0;
    const unsigned index=(unsigned)k->drawn_frame>>1;
    if(index+(k->drawn_odd ? 1u : 0u)>=motion->count || motion->first+motion->count>art->key_count) return 0;
    const nm_pit_crew_key_t *a=art->keys+motion->first+index;
    if(!k->drawn_odd) { *out=*a;return 1; }
    const nm_pit_crew_key_t *b=a+1;
    for(unsigned i=0;i<3;++i) {
        out->xyz[i]=(a->xyz[i]-b->xyz[i])*0.5f+a->xyz[i];
        if(!isfinite(out->xyz[i])) return 0;
    }
    for(unsigned i=0;i<NM_PIT_CREW_ANGLES;++i) {
        const int32_t difference=(int16_t)(uint16_t)((uint32_t)(int32_t)a->angle[i]-(uint32_t)(int32_t)b->angle[i]);
        out->angle[i]=(int16_t)(uint16_t)(uint32_t)(a->angle[i]+difference/2);
    }
    return 1;
}

static inline int nm_pit_crew_turn_reference(const nm_drive_params_t *p,const float m[12],unsigned axis,int32_t angle,
        float out[12]) {
    int16_t angles[3]={0};const float zero[3]={0};
    angles[axis]=(int16_t)(uint16_t)(uint32_t)angle;
    float r[12];
    return nm_body_pose_reference(p,zero,angles,r) && nm_pose_compose_reference(m,r,out);
}
static inline int nm_pit_crew_move_reference(const float m[12],const float xyz[3],float out[12]) {
    const float t[12]={1,0,0,0,1,0,0,0,1,xyz[0],xyz[1],xyz[2]};
    return nm_pose_compose_reference(m,t,out);
}
/* The scalar reference of nm_pit_crew_parts (host, and the finite test's check of the SH4ZAM chains). */
static inline int nm_pit_crew_parts_reference(const nm_drive_params_t *p,const nm_pit_crew_art_t *art,
        const float view[12],const float position[3],int16_t heading,int16_t heading2,int16_t camera_heading,
        const nm_pit_crew_key_t *pose,float root[12],float parts[NM_PIT_CREW_PARTS][12]) {
    const int16_t yaw[3]={0,heading,0};
    float placed[12];
    if(!nm_body_pose_reference(p,position,yaw,placed) || !nm_pose_compose_reference(view,placed,root) ||
       !nm_body_pose_reference(p,pose->xyz,pose->angle,placed) || !nm_pose_compose_reference(root,placed,parts[0])) return 0;
    const int32_t foot=(int32_t)heading-(int32_t)camera_heading+(int32_t)heading2;
    for(unsigned k=1;k<NM_PIT_CREW_PARTS;++k) {
        const unsigned a=nm_pit_crew_tree[k].angle;
        float m[12];
        if(!nm_pit_crew_move_reference(parts[nm_pit_crew_tree[k].parent],art->offsets[k-1],m)) return 0;
        switch(nm_pit_crew_tree[k].kind) {
        case NM_PIT_CREW_JOINT:
            if(!nm_pit_crew_turn_reference(p,m,0,pose->angle[a],m) || !nm_pit_crew_turn_reference(p,m,2,pose->angle[a+2],m) ||
               !nm_pit_crew_turn_reference(p,m,1,pose->angle[a+1],m)) return 0;
            break;
        case NM_PIT_CREW_TURN:
            if(!nm_pit_crew_turn_reference(p,m,2,pose->angle[a],m)) return 0;
            break;
        case NM_PIT_CREW_FOOT:
            m[0]=m[4]=m[8]=1;m[1]=m[2]=m[3]=m[5]=m[6]=m[7]=0;
            if(!nm_pit_crew_turn_reference(p,m,0,pose->angle[a],m) || !nm_pit_crew_turn_reference(p,m,1,foot,m)) return 0;
            break;
        default: return 0;
        }
        nm_memcpy(parts[k],m,sizeof(m));
    }
    return 1;
}

#ifdef NM_PIT_CREW_SHZ
/* |value| < 2^20 on its bits (finite.h: no float compare). */
static inline int nm_pit_crew_bounded(const float *values,unsigned count) {
    for(unsigned i=0;i<count;++i) {
        uint32_t bits; nm_memcpy(&bits,values+i,sizeof(bits));
        if((bits&UINT32_C(0x7fffffff))>UINT32_C(0x49800000)) return 0;
    }
    return 1;
}
#endif
/* The 16 parts' view-space matrices, part k's stored where out[k] points, and the root frame V T(position)
 * Ry(heading) (the items' parent). SH4ZAM: out[k] 4-byte aligned (SH4ZAM's stores are single moves). With every
 * input below 2^20 (view, position, pose, the offsets) the rotations keep the chain's 3x3 below 2^21 and its
 * translation below 2^46: every matrix is finite, nothing is tested element by element (otherwise, and before,
 * 204 tests a figure). */
static inline int nm_pit_crew_parts_to(const nm_drive_params_t *p,const nm_pit_crew_art_t *art,const float view[12],
        const float position[3],int16_t heading,int16_t heading2,int16_t camera_heading,const nm_pit_crew_key_t *pose,
        float root[12],float *const out[NM_PIT_CREW_PARTS]) {
    if(!p || !art || !view || !position || !pose || !root || !out) return 0;
#ifdef NM_PIT_CREW_SHZ
    for(unsigned i=0;i<3;++i) if(!isfinite(position[i]) || !isfinite(pose->xyz[i])) return 0;
    const int bounded=nm_pit_crew_bounded(view,12) && nm_pit_crew_bounded(position,3) && nm_pit_crew_bounded(pose->xyz,3) &&
        nm_pit_crew_bounded(art->offsets[0],3*(NM_PIT_CREW_PARTS-1));
    shz_mat3x4_t *const *const matrix=(shz_mat3x4_t *const *)out;
    alignas(32) shz_mat3x4_t frame;
    shz_memcpy4(frame.elem,view,sizeof(frame));
    shz_xmtrx_load_3x4(&frame);
    shz_xmtrx_translate(position[0],position[1],position[2]);
    shz_xmtrx_rotate_y(nm_pose_radians((uint16_t)(-(int)heading)));
    shz_xmtrx_store_3x4(&frame);
    /* Root: T(pose) Ry Rx Rz, resident after the frame. */
    shz_xmtrx_translate(pose->xyz[0],pose->xyz[1],pose->xyz[2]);
    shz_xmtrx_rotate_y(nm_pose_radians((uint16_t)(-(int)pose->angle[1])));
    shz_xmtrx_rotate_x(nm_pose_radians((uint16_t)(-(int)pose->angle[0])));
    shz_xmtrx_rotate_z(nm_pose_radians((uint16_t)(-(int)pose->angle[2])));
    shz_xmtrx_store_3x4(matrix[0]);
    const uint16_t foot=(uint16_t)(-((int)heading-(int)camera_heading+(int)heading2));
    unsigned resident=0;
    for(unsigned k=1;k<NM_PIT_CREW_PARTS;++k) {
        /* A chain (3 -> 4 -> 5, ...) continues in XMTRX; a new branch reloads its parent. */
        const unsigned parent=nm_pit_crew_tree[k].parent,a=nm_pit_crew_tree[k].angle;
        if(parent!=resident) shz_xmtrx_load_3x4(matrix[parent]);
        const float *offset=art->offsets[k-1];
        shz_xmtrx_translate(offset[0],offset[1],offset[2]);
        switch(nm_pit_crew_tree[k].kind) {
        case NM_PIT_CREW_JOINT:
            shz_xmtrx_rotate_x(nm_pose_radians((uint16_t)(-(int)pose->angle[a])));
            shz_xmtrx_rotate_z(nm_pose_radians((uint16_t)(-(int)pose->angle[a+2])));
            shz_xmtrx_rotate_y(nm_pose_radians((uint16_t)(-(int)pose->angle[a+1])));
            break;
        case NM_PIT_CREW_TURN:
            shz_xmtrx_rotate_z(nm_pose_radians((uint16_t)(-(int)pose->angle[a])));
            break;
        case NM_PIT_CREW_FOOT: {
            /* 0x1111/0x707: only the translation of the chain survives. */
            shz_xmtrx_store_3x4(matrix[k]);
            const float *t=matrix[k]->elem+9;
            shz_xmtrx_init_rotation_x(nm_pose_radians((uint16_t)(-(int)pose->angle[a])));
            shz_xmtrx_set_translation(t[0],t[1],t[2]);
            shz_xmtrx_rotate_y(nm_pose_radians(foot));
            break;
        }
        default: return 0;
        }
        shz_xmtrx_store_3x4(matrix[k]);
        resident=k;
    }
    if(!bounded) {
        for(unsigned i=0;i<12;++i) if(!isfinite(frame.elem[i])) return 0;
        for(unsigned k=0;k<NM_PIT_CREW_PARTS;++k) for(unsigned i=0;i<12;++i) if(!isfinite(out[k][i])) return 0;
    }
    if(nm_pose_verify) {
        float reference_root[12],reference[NM_PIT_CREW_PARTS][12];
        if(!nm_pit_crew_parts_reference(p,art,view,position,heading,heading2,camera_heading,pose,reference_root,reference)) return 0;
        for(unsigned k=0;k<=NM_PIT_CREW_PARTS;++k) {
            const float *got=k<NM_PIT_CREW_PARTS ? out[k] : frame.elem,*want=k<NM_PIT_CREW_PARTS ? reference[k] : reference_root;
            for(unsigned i=0;i<12;++i)
                if(shz_fabsf(got[i]-want[i])>(i<9 ? 1e-4f : 1e-4f*(1.0f+shz_fabsf(want[i])))) {
                    printf("FAIL: SH4ZAM pit crew chain differs part %u element %u: %g %g\n",k,i,(double)got[i],(double)want[i]); return 0;
                }
        }
    }
    shz_memcpy4(root,frame.elem,sizeof(frame));
    return 1;
#else
    float parts[NM_PIT_CREW_PARTS][12];
    if(!nm_pit_crew_parts_reference(p,art,view,position,heading,heading2,camera_heading,pose,root,parts)) return 0;
    for(unsigned k=0;k<NM_PIT_CREW_PARTS;++k) nm_memcpy(out[k],parts[k],sizeof(parts[k]));
    return 1;
#endif
}
/* The same into an array. */
static inline int nm_pit_crew_parts(const nm_drive_params_t *p,const nm_pit_crew_art_t *art,const float view[12],
        const float position[3],int16_t heading,int16_t heading2,int16_t camera_heading,const nm_pit_crew_key_t *pose,
        float root[12],float parts[NM_PIT_CREW_PARTS][12]) {
    if(!parts) return 0;
    float *out[NM_PIT_CREW_PARTS];
    for(unsigned k=0;k<NM_PIT_CREW_PARTS;++k) out[k]=parts[k];
    return nm_pit_crew_parts_to(p,art,view,position,heading,heading2,camera_heading,pose,root,out);
}

/* An item: root T(item) [Rz(item)]. */
static inline int nm_pit_crew_item(const nm_drive_params_t *p,const nm_pit_crew_art_t *art,const float root[12],
        unsigned item,float out[12]) {
    if(item>=NM_PIT_CREW_ITEMS) return 0;
    const float *offset=art->item_offsets[item];
#ifdef NM_PIT_CREW_SHZ
    if(nm_pose_verify) {
        float m[12];
        if(!nm_pit_crew_move_reference(root,offset,m) ||
           (art->item_roll[item] && !nm_pit_crew_turn_reference(p,m,2,art->item_roll[item],m))) return 0;
        nm_memcpy(out,m,sizeof(m)); /* compared below */
    }
    alignas(32) shz_mat3x4_t matrix;
    shz_memcpy4(matrix.elem,root,sizeof(matrix));
    shz_xmtrx_load_3x4(&matrix);
    shz_xmtrx_translate(offset[0],offset[1],offset[2]);
    if(art->item_roll[item]) shz_xmtrx_rotate_z(nm_pose_radians((uint16_t)(-(int)art->item_roll[item])));
    shz_xmtrx_store_3x4(&matrix);
    for(unsigned i=0;i<12;++i) if(!isfinite(matrix.elem[i])) return 0;
    if(nm_pose_verify)
        for(unsigned i=0;i<12;++i)
            if(shz_fabsf(matrix.elem[i]-out[i])>(i<9 ? 1e-4f : 1e-4f*(1.0f+shz_fabsf(out[i])))) {
                puts("FAIL: SH4ZAM pit crew item differs"); return 0;
            }
    shz_memcpy4(out,matrix.elem,sizeof(matrix));
    return 1;
#else
    float m[12];
    if(!nm_pit_crew_move_reference(root,offset,m) ||
       (art->item_roll[item] && !nm_pit_crew_turn_reference(p,m,2,art->item_roll[item],m))) return 0;
    nm_memcpy(out,m,sizeof(m));
    return 1;
#endif
}

/* One figure's draws (nm_pit_crew_draws_figures): out[first, first+count) (its items, then its 16 parts: parts
 * from out[first+items]), its model set (index into art->sets), its root frame V T(position) Ry(heading) (the
 * parent of everything it draws) and the translation of this frame's pose in that frame (part 0's origin). */
typedef struct { uint16_t first, count, set, items; alignas(16) float root[12]; float pose[3]; } nm_pit_crew_figure_t;
/* What bounds a figure's parts around part 0's origin, from the crew data alone: per part the length of its
 * chain of offsets (part 0: 0). Rotations keep lengths, so part k's origin lies within chain[k] of part 0's.
 * Euclidean lengths, rounded up by one part in 2^16. */
static inline void nm_pit_crew_reach(const nm_pit_crew_art_t *art,float chain[NM_PIT_CREW_PARTS]) {
    chain[0]=0;
    for(unsigned k=1;k<NM_PIT_CREW_PARTS;++k) {
        const float *o=art->offsets[k-1];
#ifdef NM_PIT_CREW_SHZ
        /* SH4ZAM's root (FSRRA, within 2^-21 relative): a bound of the crew's art, presentation; the margin covers it. */
        const float length=shz_sqrtf(o[0]*o[0]+o[1]*o[1]+o[2]*o[2]);
#else
        const float length=sqrtf(o[0]*o[0]+o[1]*o[1]+o[2]*o[2]);
#endif
        chain[k]=chain[nm_pit_crew_tree[k].parent]+length*(1.0f+0x1p-16f);
    }
}
/* Every draw of the crew this tick, in the arcade's order (crew 0..5: items, then parts 0..15): model and
 * view-space matrix; with the manual transmission (pit->manual, 501464) the sets' manual twins. Nothing when the
 * rows did not draw. camera_heading: 5014a8 of this tick (the view's heading). figures (may be NULL): each drawn
 * figure's range of out, set and root frame, *figure_count of them. */
static inline int nm_pit_crew_draws_figures(const nm_drive_params_t *p,const nm_pit_data_t *data,const nm_pit_t *pit,
        const float view[12],int16_t camera_heading,nm_pit_crew_draw_t out[NM_PIT_CREW_DRAWS_MAX],unsigned *count,
        nm_pit_crew_figure_t figures[NM_PIT_CREW],unsigned *figure_count) {
    if(!p || !data || !pit || !view || !out || !count || pit->manual>1) return 0;
    const unsigned manual=pit->manual;
    *count=0;
    if(figure_count) *figure_count=0;
    const nm_pit_crew_art_t *art=data->art;
    if(!pit->crew_drawn || !art) return 1;
    for(unsigned c=0;c<NM_PIT_CREW;++c) {
        const nm_pit_crew_t *k=pit->crews+c;
        const nm_pit_crew_record_t *record=data->crew+c;
        const nm_pit_crew_set_t *set=nm_pit_crew_set(art,k->drawn_models);
        nm_pit_crew_key_t pose;
        alignas(32) float root[12];
        if(!set || !nm_pit_crew_pose(art,k,&pose)) return 0;
        /* The parts' matrices straight into their draws, after the items (which need the root). */
        unsigned items=0;
        if(c<4) for(unsigned item=0;item<NM_PIT_CREW_ITEMS;++item) items+=(k->drawn_items&(2u<<item))!=0;
        float *parts[NM_PIT_CREW_PARTS];
        for(unsigned part=0;part<NM_PIT_CREW_PARTS;++part) {
            out[*count+items+part].model=set->models[manual][part];
            parts[part]=out[*count+items+part].matrix;
        }
        if(!nm_pit_crew_parts_to(p,art,view,record->position,record->heading,record->heading2,camera_heading,&pose,root,parts))
            return 0;
        if(figures && figure_count) {
            nm_pit_crew_figure_t *const f=figures+(*figure_count)++;
            f->first=(uint16_t)*count; f->count=(uint16_t)(items+NM_PIT_CREW_PARTS); f->set=(uint16_t)(set-art->sets);
            f->items=(uint16_t)items; nm_memcpy(f->root,root,sizeof(root)); nm_memcpy(f->pose,pose.xyz,sizeof(f->pose));
        }
        if(c<4) for(unsigned item=0;item<NM_PIT_CREW_ITEMS;++item) {
            if(!(k->drawn_items&(2u<<item))) continue;
            out[*count].model=art->item_models[item];
            if(!nm_pit_crew_item(p,art,root,item,out[*count].matrix)) return 0;
            ++*count;
        }
        *count+=NM_PIT_CREW_PARTS;
    }
    return 1;
}
static inline int nm_pit_crew_draws(const nm_drive_params_t *p,const nm_pit_data_t *data,const nm_pit_t *pit,
        const float view[12],int16_t camera_heading,nm_pit_crew_draw_t out[NM_PIT_CREW_DRAWS_MAX],unsigned *count) {
    return nm_pit_crew_draws_figures(p,data,pit,view,camera_heading,out,count,NULL,NULL);
}
#endif
