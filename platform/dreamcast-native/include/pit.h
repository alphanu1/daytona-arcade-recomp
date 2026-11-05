#ifndef DAYTONA_NATIVE_PIT_H
#define DAYTONA_NATIVE_PIT_H
#include "intro_math.h"
#include "tunnel_light.h"

/* Pit stop, course0 (spec: build/dc-native-pit.xjs2cE/spec-pitstop.md).
 * The player's handler dc18 (entry tick) / dd14 (every pit tick) / dfd4 (abort),
 * the crew task rows 68..73 (1abfc first tick, 1aca4 after) and the shared
 * words 501a84 (race_field.h nm_race_shared_t.timer_hold), 50112c, 501128,
 * 501a98. Data: the race header (tools/prepare_intro.py --race-start,
 * nm_race_pit), ROM-derived and local; no ROM address is read at run time
 * (motion and model words are identities for the presentation's binding). */

enum { NM_PIT_WAYPOINTS=7, NM_PIT_CREW=6, NM_PIT_SCRIPT_ENTRIES=128, NM_PIT_CUES=8,
       NM_PIT_STOP_TICKS=640, NM_PIT_CREW_TICKS=128 };
enum { NM_PIT_CAMERA_NONE=0, NM_PIT_CAMERA_ENTRY=1, NM_PIT_CAMERA_STOP=2 };

/* 234bf8[course] (course0 234c08): 16-byte rows x, z, km/h, 0. Seven waypoints and the row after them,
 * which the abort path's fall-through dfd4 -> df3c -> e0a8 reads with +c0 = 7 (the next course's table). */
typedef struct { float x, z, speed; uint32_t reserved; } nm_pit_waypoint_t;
/* A crew script entry (32 bytes in the ROM): crew time, action (1b274), arguments +8 +c +10. */
typedef struct { uint32_t frame, action, argument[3]; } nm_pit_script_t;
/* 239848[course] (course0 239858), 32 bytes a crew: motion +0 -> +54, model set +4 -> +50,
 * script +8 -> +58 (here: first entry and count in scripts[]), position +c..+14 -> +1c..+24,
 * headings +18/+1a -> +5c/+64, frame count +1c -> +60. */
typedef struct {
    uint32_t motion, models;
    float position[3];
    int16_t heading, heading2;
    uint32_t frames;
    uint16_t script, script_count;
} nm_pit_crew_record_t;
/* 23ad4c: the crew sound cues (1ad74), {crew time, 19bdc command}, the last one a terminator. */
typedef struct { uint32_t time, command; } nm_pit_cue_t;

/* The crew's presentation data (include/pit_crew.h draws it; tools/prepare_intro.py --race-start writes it).
 * A motion is a run of 76-byte keyframes, one every second motion frame: the root's offset and 32 int16 angles
 * (1ada8 copies or interpolates one into +74..+be). A model set (+50) names the object list of each of the 16
 * parts; with the manual transmission (501464) 1ad3c reads the set 64 bytes further: models[1] (in the car pack
 * since the gears branch, tools/export_course.hpp; an older pack draws models[0]). Each list is one model: its polygon-ROM
 * address, the car pack's catalogue identity. */
enum { NM_PIT_CREW_PARTS=16, NM_PIT_CREW_ANGLES=32, NM_PIT_CREW_ITEMS=3, NM_PIT_CREW_MOTIONS=8, NM_PIT_CREW_SETS=12 };
typedef struct { float xyz[3]; int16_t angle[NM_PIT_CREW_ANGLES]; } nm_pit_crew_key_t;
typedef struct { uint32_t address; uint16_t first, count; } nm_pit_crew_motion_t;
typedef struct { uint32_t address; uint32_t models[2][NM_PIT_CREW_PARTS]; } nm_pit_crew_set_t;
typedef struct {
    float offsets[NM_PIT_CREW_PARTS-1][3]; /* 23ac98: part k's translation from its parent (k = 1..15) */
    /* 1b33c, crews 0..3: the held items by +68 bits 1..3, in the root's frame: translation (code immediates),
     * a turn about Z (0: none) and the model (lists 288b730, 288b730, 288ddc8). */
    float item_offsets[NM_PIT_CREW_ITEMS][3];
    int16_t item_roll[NM_PIT_CREW_ITEMS];
    uint32_t item_models[NM_PIT_CREW_ITEMS];
    uint32_t motion_count, set_count, key_count;
    nm_pit_crew_motion_t motions[NM_PIT_CREW_MOTIONS];
    nm_pit_crew_set_t sets[NM_PIT_CREW_SETS];
    const nm_pit_crew_key_t *keys;
} nm_pit_crew_art_t;

typedef struct {
    nm_pit_waypoint_t waypoints[NM_PIT_WAYPOINTS+1];
    uint32_t clean_body[2]; /* 234bf0[501464]: the body list the repair de30..dedc writes into +14. */
    nm_pit_crew_record_t crew[NM_PIT_CREW];
    nm_pit_cue_t cues[NM_PIT_CUES];
    uint32_t script_count;
    nm_pit_script_t scripts[NM_PIT_SCRIPT_ENTRIES];
    const nm_pit_crew_art_t *art; /* presentation only (pit_crew.h); NULL: the crew is not drawn */
} nm_pit_data_t;

/* One crew object (task rows 68..73, 0x280 bytes; the fields the native needs). */
typedef struct {
    uint32_t time;              /* +38: crew time, stopped ticks since the car stood */
    uint32_t entry;             /* +3c: next script entry */
    uint32_t action, previous;  /* +40, +44 */
    uint32_t argument[2];       /* +48, +4c */
    int32_t frame;              /* +34: motion frame */
    uint32_t frames;            /* +60: frame count of the motion */
    uint32_t motion, models;    /* +54, +50: identities */
    uint32_t items;             /* +68: held-item bits */
    uint32_t cue;               /* +70: next sound cue */
    /* Presentation: what this tick's draws use (1aca4). The items draw 1b2a4 and the keyframe pointer g8 (motion
     * +54, frame +34 >> 1) and the model set g6 (+50) are taken BEFORE the script 1b00c; the pose 1ada8 runs
     * after it and interpolates when the frame is odd THEN (1af50/1afcc read +34 again). */
    uint32_t drawn_items, drawn_models, drawn_motion;
    int32_t drawn_frame;
    uint32_t drawn_odd;
} nm_pit_crew_t;

/* 7384..7664 / 1b168..1b210: the 501a98 bit of wheel k (front pair first). */
static const uint8_t nm_pit_wheel_bits[4]={1,0,3,2};
/* What the player's draw (camera row 60, before the crew rows) saw on a tick the crew ran: the crew's writes
 * reach the picture one tick later (spec-pitstop.md section 8). Presentation only. */
typedef struct {
    uint32_t damage, overlay_model;
    int16_t wheel_phase[2];
    uint8_t wheels_off, valid;
} nm_pit_drawn_t;

typedef struct {
    uint32_t ticks;      /* 50112c: pit ticks since the entry (dd48) */
    uint8_t armed;       /* 501128: the stop camera armed by the crew (1b00c) */
    uint8_t wheels_off;  /* 501a98: wheel bits the crew toggles (1, 0, 3, 2)[k]; 7384 hides those wheels */
    uint8_t crew;        /* rows 68..73 flags bit31 (dd68 on, de30 / dfd4 off) */
    uint8_t crew_start;  /* rows 68..73 handler 1abfc: their next run is the first */
    uint8_t suspended;   /* rows 74..138 (actor creator, world actors) suspended (dd9c; de58 / e000 resume) */
    uint8_t camera;      /* the keyed track the aux object 501270 plays: NM_PIT_CAMERA_* (0: the start's) */
    uint8_t camera_arm;  /* aux object re-armed: 5af8 (dc44, this tick) or 5b20 (1b00c, the next tick's pass) */
    uint8_t crew_drawn;  /* presentation: the crew rows drew this tick (1aca4; not on 1abfc's first run) */
    uint8_t manual;      /* presentation: 501464 as the pit ticks saw it (1ad3c: the crew's manual-car colours) */
    nm_pit_crew_t crews[NM_PIT_CREW];
    nm_pit_drawn_t drawn;
} nm_pit_t;

/* The player as its draw sees it (the state before this tick's crew rows) and the 501a98 wheel bits it reads:
 * *copy holds the car when the crew ran this tick. Presentation (code/render/scene/practice.c, the trace). */
static inline const nm_player_t *nm_pit_drawn_player(const nm_pit_t *pit, const nm_player_t *car,
        nm_player_t *copy, unsigned *wheels_off) {
    if(!pit->drawn.valid) { *wheels_off=pit->wheels_off;return car; }
    NM_COPY(copy,car);
    copy->boundary.damage.damage=pit->drawn.damage;copy->boundary.damage.overlay_model=pit->drawn.overlay_model;
    copy->wheel_phase[0]=pit->drawn.wheel_phase[0];copy->wheel_phase[1]=pit->drawn.wheel_phase[1];
    *wheels_off=pit->drawn.wheels_off;
    return copy;
}

/* A keyed camera track of the aux object 501270 (6474 at the start, 234a9c / 234aac in the pit); the race
 * header lists them as nm_race_keyed_tracks[NM_PIT_CAMERA_*], indexed by nm_pit_t.camera. */
typedef struct { const nm_intro_key_t *keys; const uint32_t *times; unsigned count; } nm_race_keyed_track_t;
/* One course's race-start tables (race_start_data.h nm_race_course_starts[slot]): the keyed tracks by
 * nm_pit_t.camera (the start 6474[course], the pit entry and stop), the pit tracks by view, the pit stop.
 * tracks NULL: the header does not hold that course. */
typedef struct {
    const nm_race_keyed_track_t *tracks;
    const nm_race_keyed_track_t (*pit_views)[4];
    const nm_pit_data_t *pit;
    const nm_tunnel_lights_t *tunnel;   /* the scene light's tunnel ranges (tunnel_light.h) */
} nm_race_course_start_t;

/* The pit's dc18/dd14 flags gate 0x180000fc + 0x1f80000 (the race phases), as d7fc's. */
#define NM_PIT_FLAGS UINT32_C(0x19f800fc)

/* e0a8: steer toward waypoint +c0 (spin_body_offset, as the original reuses it). The TGP angle op 0f of
 * 10bcc with (w.z, x, z, w.x); the heading step is the WIDE difference clamped to +-128 (no wrap), the
 * desired speed the waypoint's km/h times 28b573c, and the index advances once the waypoint is more
 * than a quarter turn off the road heading +1c2 (signed 32-bit test of a - road + 0x3fff in 0..0x7fff). */
static inline int nm_pit_waypoint_step(const nm_drive_params_t *p, const nm_pit_data_t *data, nm_player_t *s) {
    const int index=s->spin_body_offset;
    if(index<0 || index>NM_PIT_WAYPOINTS) return 0;
    const nm_pit_waypoint_t *w=data->waypoints+index;
    const float dz=w->z-s->boundary.ground.position[2],dx=s->boundary.ground.position[0]-w->x;
    if(!isfinite(dz) || !isfinite(dx)) return 0;
    const int32_t angle=nm_force_angle(p,dz,dx);
    if(angle==INT32_MIN) return 0;
    int32_t step=angle-(int32_t)s->boundary.heading;
    if(step< -128) step=-128;
    else if(step>128) step=128;
    s->boundary.heading=s->boundary.ground.yaw=nm_yaw_angle((uint32_t)((int32_t)s->boundary.heading+step));
    s->desired_speed=p->collision_speed_step*w->speed;
    if(!isfinite(s->desired_speed)) return 0;
    const int32_t side=angle-(int32_t)s->boundary.ground.road_heading+0x3fff;
    if(side<0 || side>0x7fff) s->spin_body_offset=nm_yaw_angle((uint32_t)(index+1));
    return 1;
}

/* e194: unless spinning (+ec, then eafc: ea04's spin path), the TGP op 47 move by the speed along
 * -heading: delta +9c/+a4 and position. No motion scale, no lookahead (501864 untouched). */
static inline int nm_pit_motion_step(const nm_drive_params_t *p, const nm_player_dynamics_input_t *in,
        nm_player_t *s) {
    if(s->boundary.damage.spin_ticks) return nm_player_planar_motion(p,in,s);
    const float speed=s->boundary.speed;
    if(!isfinite(speed) || !isfinite(s->boundary.ground.position[0]) || !isfinite(s->boundary.ground.position[2])) return 0;
    float delta[2];
    nm_force_cartesian(p,(uint16_t)(-(int)s->boundary.heading),speed,delta);
    for(unsigned a=0;a<2;++a) {
        const float position=delta[a]+s->boundary.ground.position[2*a];
        if(!isfinite(delta[a]) || !isfinite(position)) return 0;
        s->delta[a]=delta[a];s->boundary.ground.position[2*a]=position;
    }
    return 1;
}

/* 10384: rpm +150 from the speed and gear (two TGP multiplies, cvtri), clamped 0..8500, the excess to +25c
 * (compensation): the rpm half of 10338 (drive.h nm_speed_step) without the speed update. */
static inline int nm_pit_rpm_step(const nm_drive_params_t *p, nm_player_t *s) {
    if(s->gear>=4 || !isfinite(s->boundary.speed) || !isfinite(p->gear[s->gear]) || !isfinite(p->rpm_scale)) return 0;
    float rpm=s->boundary.speed*p->gear[s->gear];
    rpm=p->rpm_scale*rpm;
    int32_t raw;
    if(!nm_drive_round_i32(rpm,&raw)) return 0;
    s->rpm=raw<0 ? 0 : raw>8500 ? 8500 : raw;
    s->compensation=raw>s->rpm ? raw-s->rpm : 0;
    return 1;
}

/* The pit tick, one call per player update while the handler is dc18 (entry=1) or dd14. All of it only
 * under the race phases (NM_PIT_FLAGS), else just the circle e078 (and dc18 stays pending).
 *  dc18..dd10 (entry): the aux object 501270 re-armed with 5af8 (the entry camera; pit->camera_arm,
 *    the scene applies it after this tick's interactions), 501a84 := 640, resistance, steer, steer angle
 *    +124, waypoint +c0, 501128, 50112c, tire angles +188/+18a, filtered pitch, +54, bank := 0, yaw :=
 *    heading := road heading, +1a0 := 0; speed := 270 * 28b573c when the speed index is >= 270, desired
 *    speed := the same; then falls into dd14.
 *  dd14..dfd0: +c0 >= 7 aborts (dfd4..e070, then df3c); 50112c += 1, at 128 the crew rows start (1abfc)
 *    and rows 74..138 are suspended; the stop count 501a84 -= 1 on ticks that start standing (+18 == 0,
 *    the previous tick's index), at 128 the crew stops, the actors resume and the repair +208 &= ~0x10f,
 *    +14 := clean body[501464]; at <= 0 the release: handler d7fc (*released), camera reset 62b4,
 *    501a84 := 1, +5c := 0. Then: +20d -= 1 when set, +24c := 0, e5f4, (9010: ret), 15fc4/16014/15d60,
 *    11438, 10384, 11d60 automatic (also with the manual transmission), e194, 116d0, e0a8, path offset
 *    (profile + 3) * 28, 16e10, 10980/16060/16da4, 11aa4, 132b8, +2a := +58, +15f := 4 at index >= 150.
 * Not called: the controls, forces, tyres, road tail (lateral, rank key, road marks, shared tilt) and the
 * entry test d920. No random number. All or nothing: nothing is published on a rejection. */
static inline int nm_player_pit_tick(const nm_drive_params_t *p, const nm_intro_speed_params_t *speed,
        const nm_contact_pack_t *contact, const nm_road_pack_t *road, const nm_path_pack_t *path,
        const nm_pit_data_t *data, const nm_player_active_input_t *in, int entry,
        nm_player_t *state, nm_pit_t *pit, uint32_t *hold, uint8_t *released) {
    if(!p || !speed || !contact || !road || !path || !data || !in || !state || !pit || !hold || !released ||
       in->dynamics.course_slot!=contact->slot || road->slot!=contact->slot || path->slot!=road->slot ||
       !road->profiles || road->profile_count<2 || road->profile_count>4097 || in->dynamics.curve>1) return 0;
    nm_player_t s; NM_COPY(&s,state);
    nm_pit_t q=*pit;
    uint32_t h=*hold;
    uint8_t out=0;
    q.manual=in->dynamics.curve;
    if(in->dynamics.speed_flags&NM_PIT_FLAGS) {
        if(entry) {
            q.camera_arm=NM_PIT_CAMERA_ENTRY;
            h=NM_PIT_STOP_TICKS;
            s.resistance=0;s.steer=0;s.force.steer_angle=0;s.spin_body_offset=0;q.armed=0;q.ticks=0;
            s.force.tire_angle[0]=s.force.tire_angle[1]=0;
            s.filtered_pitch=s.render_yaw_offset=s.bank=0;
            s.boundary.ground.yaw=s.boundary.heading=s.boundary.ground.road_heading;
            s.force.rear_bank_force=0;
            const float limit=270.0f*p->collision_speed_step; /* TGP op 2 */
            if(s.speed_index>=270) s.boundary.speed=limit;
            s.desired_speed=limit;
        }
        if(s.spin_body_offset>=NM_PIT_WAYPOINTS) {
            /* dfd4..e070: past the last waypoint (not reached on course0, spec section 2). */
            q.crew=0;q.suspended=0;out=1;q.camera=NM_PIT_CAMERA_NONE;
            /* e058: 62b4 in every view (not reached on course 0; the race scene restores view 2 and up only). */
            nm_player_view_reset(p,&s,in->restored_camera_mode,NULL);
            h=1;s.boundary.damage.overlay_model=0;
        } else {
            ++q.ticks;
            if(q.ticks==NM_PIT_CREW_TICKS) { q.crew=q.crew_start=1;q.suspended=1; }
            uint32_t t=h;
            if(!s.speed_index) {
                --t;
                if(t==NM_PIT_CREW_TICKS) {
                    q.crew=0;q.suspended=0;
                    s.boundary.damage.damage&=~UINT32_C(0x10f);
                    s.boundary.damage.body_model=data->clean_body[in->dynamics.curve];
                    t=h-1u;
                }
            }
            h=t;
            if((int32_t)t<=0) {
                /* df08: view 501710 below 2: 637c, else 62b4 (the race scene applies the preset after the step). */
                out=1;q.camera=NM_PIT_CAMERA_NONE;
                nm_player_view_reset(p,&s,in->restored_camera_mode,NULL);
                h=1;s.boundary.damage.overlay_model=0;
            }
        }
        /* df3c.. */
        if(s.boundary.damage.initial_inhibit) --s.boundary.damage.initial_inhibit;
        s.force.rpm_boost=0;
        s.lap_write.written=0;s.early_sound_request=0;s.route_transition=0;
        s.force.trigger_pair=s.boundary.pivot_sound=s.pre_sound_requests=0;
        s.boundary.action=NM_BOUNDARY_NONE;
        if(!nm_player_speed_index_step(p,&s)) return 0;
        const nm_damage_pre_input_t pi={in->dynamics.speed_flags,
            (uint32_t)s.path_flags|((uint32_t)s.previous_path_flags<<16),s.phase[0],s.boundary.ground.yaw,
            in->restored_camera_mode,in->variant,in->dynamics.entity_id};
        if(!nm_player_damage_prefix(p,&pi,&s,0) ||
           !nm_intro_speed_step(p,speed,in->dynamics.speed_flags,&s) ||
           !nm_pit_rpm_step(p,&s)) return 0;
        const nm_gears_input_t gi={s.speed_index,nm_gears_signed(s.force.rpm_boost),0,0};
        nm_gears_state_t gear={s.gear,s.brake,s.rpm,s.filtered_rpm,s.downshift_rpm,s.shift_ticks};
        if(!nm_gears_step(p,&gi,&gear)) return 0;
        s.gear=gear.gear;s.brake=gear.brake;s.rpm=gear.rpm;
        s.filtered_rpm=gear.filtered_rpm;s.downshift_rpm=gear.downshift_rpm;s.shift_ticks=gear.shift_ticks;
        nm_player_dynamics_input_t dynamics=in->dynamics;dynamics.speed_index=s.speed_index;
        if(!nm_pit_motion_step(p,&dynamics,&s) ||
           !nm_wheels_ground(contact,p,&s.boundary.ground) ||
           !nm_pit_waypoint_step(p,data,&s)) return 0;
        s.path_offset=(uint16_t)(((int32_t)s.phase[0]+3)*28);
        dynamics.speed_index=s.speed_index;
        if(!nm_contact_scenery_cell(s.boundary.ground.position[0],s.boundary.ground.position[2],&s.scenery_cell) ||
           !nm_player_road_contact_step(p,contact,road,path,&dynamics,&s) ||
           !nm_player_body_angles_step(p,s.tilt[0],&s)) return 0;
        uint16_t previous=(uint16_t)s.phase[1];
        if(!nm_checkpoint_step(road,(uint16_t)s.phase[0],&previous,&s.boundary.damage.flags,
                &s.lap,&s.checkpoints,&s.lap_write)) return 0;
        s.phase[1]=nm_yaw_angle(previous);
        s.body_yaw=s.boundary.ground.yaw;
        if(s.speed_index>=150) s.boundary.damage.override_count=4;
    }
    nm_player_circle_step(&s);
    NM_COPY(state,&s);*pit=q;*hold=h;*released=out;
    return 1;
}

/* The crew rows 68..73 (task table 22f1f0, crew id +4 = 0..5), once per tick after the cameras, the HUD
 * and the spin smoke (rows 58, 60, 61, 66), before the actor rows 74..138, while flags bit31 is set.
 * 1abfc (the first run, the tick the player's 50112c reached 128): the crew record into the object, the
 * counters zero, items 4. 1aca4 after: the sound cue 1ad74 (crew time == cue time: queued, next cue),
 * then 1b00c while the player stands (+18 == 0): the first such tick arms the stop camera (aux object
 * 501270 flags bit31, handler 5b20, 501128 := 1); an entry due (crew time >= its frame) is loaded by
 * 1b0b4 (previous := action, action and two arguments) and the entry index advances; then the current
 * action of 1b274 runs, with g9 the entry at the index before that advance; crew time += 1. The pose
 * (1ada8) and the draw are presentation (from frame, motion and items). Actions: 0/1 none, 2 frame + 1
 * (to the count), 3 frame - 1 (to 0), 4 motion, count, frame from the entry and action := previous, 5
 * model set from the entry, action := previous, item bit toggled (entry argument 1 when nonzero), 6..9
 * wheel k = 0..3: +208 bit 7-k cleared, action := previous, 501a98 bit (1, 0, 3, 2)[k] toggled, k = 0
 * also the wheel phases +5a/+250 := 0; 10 +5c := 0.64 (jack up), 11 +5c := 0 (jack down).
 * Sound commands go to sounds[] in queue order (each cue six times, once per crew). */
static inline int nm_pit_crew_tick(const nm_pit_data_t *data, nm_pit_t *pit, nm_player_t *player,
        uint32_t sounds[NM_PIT_CREW], unsigned *sound_count) {
    if(!data || !pit || !player || !sounds || !sound_count || data->script_count>NM_PIT_SCRIPT_ENTRIES) return 0;
    *sound_count=0;
    pit->drawn=(nm_pit_drawn_t){player->boundary.damage.damage,player->boundary.damage.overlay_model,
        {player->wheel_phase[0],player->wheel_phase[1]},pit->wheels_off,(uint8_t)pit->crew};
    pit->crew_drawn=0;
    if(!pit->crew) return 1;
    if(pit->crew_start) {
        for(unsigned c=0;c<NM_PIT_CREW;++c) {
            const nm_pit_crew_record_t *r=data->crew+c;
            if(!r->script_count || (uint32_t)r->script+r->script_count>data->script_count) return 0;
            pit->crews[c]=(nm_pit_crew_t){.frames=r->frames,.motion=r->motion,.models=r->models,.items=4};
        }
        pit->crew_start=0;
        return 1;
    }
    pit->crew_drawn=1;
    for(unsigned c=0;c<NM_PIT_CREW;++c) {
        nm_pit_crew_t *k=pit->crews+c;
        const nm_pit_crew_record_t *r=data->crew+c;
        if(k->cue>=NM_PIT_CUES) return 0;
        if(data->cues[k->cue].time==k->time) { sounds[(*sound_count)++]=data->cues[k->cue].command;++k->cue; }
        k->drawn_items=k->items;k->drawn_models=k->models;k->drawn_motion=k->motion;k->drawn_frame=k->frame;
        k->drawn_odd=(uint32_t)k->frame&1u;
        if(player->speed_index) continue;
        if(!pit->armed) { pit->armed=1;pit->camera_arm=NM_PIT_CAMERA_STOP;player->auxiliary_flags|=UINT32_C(0x80000000); }
        if(k->entry>=r->script_count) return 0;
        const nm_pit_script_t *e=data->scripts+r->script+k->entry;
        if(k->time>=e->frame) {
            k->previous=k->action;k->action=e->action;
            k->argument[0]=e->argument[0];k->argument[1]=e->argument[1];
            ++k->entry;
        }
        switch(k->action) {
        case 0: case 1: break;
        case 2: { const int32_t next=k->frame+1;k->frame=next<(int32_t)k->frames ? next : (int32_t)k->frames;break; }
        case 3: { const int32_t next=k->frame-1;k->frame=next>0 ? next : 0;break; }
        case 4: k->action=k->previous;k->motion=e->argument[0];k->frames=e->argument[1];k->frame=(int32_t)e->argument[2];break;
        case 5:
            k->models=e->argument[0];k->action=k->previous;
            if(e->argument[1]) k->items^=UINT32_C(1)<<(e->argument[1]&31u);
            break;
        case 6: case 7: case 8: case 9: {
            const unsigned wheel=k->action-6u;
            if(!wheel) player->wheel_phase[0]=player->wheel_phase[1]=0;
            player->boundary.damage.damage&=~(UINT32_C(0x80)>>wheel);
            k->action=k->previous;
            pit->wheels_off^=(uint8_t)(1u<<nm_pit_wheel_bits[wheel]);
            break;
        }
        case 10: player->boundary.damage.overlay_model=UINT32_C(0x3f23d70a);break;
        case 11: player->boundary.damage.overlay_model=0;break;
        default: return 0;
        }
        ++k->time;
        k->drawn_odd=(uint32_t)k->frame&1u; /* 1af50 remi 2: the frame after the script */
    }
    return 1;
}
#endif
