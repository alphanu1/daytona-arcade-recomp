#ifndef DAYTONA_NATIVE_VR_DEMO_H
#define DAYTONA_NATIVE_VR_DEMO_H
/* The attract's V.R. demo race (phases 6/7, include/attract.h): a race of the loop's course run by the race's own
 * objects under the phase flag 1<<7, with nothing driven by a pad.
 *   phase 6 (1e5c): 637c resets the camera, then the objects: the field pass 12068 (1210c), the player c940 (its
 *     updater c9c4; 501874, 5010c4, 5010c0, +1ba..+1c0 := 0; the grid reset e20c; 13e00: the road marks
 *     53b718/53b71c cleared), every opponent c9b0 (e20c, then cde0: the race's opponent), the camera 6484 (6510:
 *     the race's), the scenery 5890, the world objects 220000 and the demo's 2D object 224e64; the cars are the
 *     cinematic's last stage's (501084 + 1: 21 on Beginner, 10 on the others), gridded by entity on the race grid
 *     (e20c: 286fac0's rows, 234be8[course]; lap 255 for a car behind the half of the course).
 *   phase 7 (2028): 834 iterations (5010a8 from 832), then 2058 steps the course and phase 8 (the title).
 *     224ef4 counts its +6 and selects the view 501710 = (+6 >> 7) & 3: VR1, VR2, VR3, VR4, VR1 ... every 128
 *     iterations (the camera 8814 takes the preset's mode and words as for a held button). No timer, checkpoint,
 *     award or sound of the race runs; nothing is drawn of the race HUD.
 *   c9c4 (the player): +20d counts down, e6b8 (the wheel phases +5a/+250 by the speed index << 6), +ac := 1,
 *     11304/11438/10ccc (an opponent's resistance, speed controller and guidance, intro_math.h
 *     nm_opponent_motion_step), +58 := +2a, e72c (the drawn angles: ground + offsets), 16060 (boundary.h),
 *     15fc4/16014/15d60 (damage.h, the player's prefix), 15314 (the impact correction), 158c8 (the input inhibit),
 *     the centre ground query 11620 with the road frame (TGP 2a2a, 3030), 11bd4 (the wheel frame), 1365c (map
 *     progression, as an attract car), 16e10, caf4 (nothing in phases 6..9), 11aa4 (body angles), 13de0 (rank key),
 *     e078 (the circle).
 * The scene is an nm_race_scene_t (its cars, field, order and camera words), so the race's presentation draws it. */
#include "race_scene.h"
#include "race_camera.h"
#include "attract.h"
#include "frontend_pool.h"
#include "nm_mem.h"

enum { NM_DEMO_PHASE = 7, NM_DEMO_TICKS = 834 };
#define NM_DEMO_FLAGS (UINT32_C(1) << NM_DEMO_PHASE)

/* 224e64's counter +6 and its view. */
typedef struct {
    uint16_t counter;
} nm_demo_t;

/* What the attract's scenes read of the original's 44 entities and its globals (the entities are never cleared),
 * beyond the race entry's part: nm_entity_pool_t (frontend_pool.h). Measured on the attract trace (courses 0, 2, 1):
 *  - +60, the rank key: the cinematic updaters of Beginner and Advanced (d290, d230) do not write it; its cars keep
 *    the V.R. demo's before (power-on: 0), and the demo's grid reset (e3f4 clears +34.. below it) keeps the
 *    cinematic's;
 *  - +6e, the lap byte: each cinematic's opponents start with the one the course's grid reset gives their entity
 *    (e20c: 255 behind the half of the course, else 0; road->grid[].lap), the player (13e00) with 255;
 *  - 5014ba, the sort pass's leading lap: the demo's into the cinematic after it;
 *  - 501710, the view: the last demo's into the next demo's camera reset (637c).
 * The native cinematic counts laps from one more (intro_math.h): its lap byte is the original's + 1. */

/* After nm_intro_scene_start_attract (before its first tick). Its phase clock (503690/503692 = 5010b0 - ticks - 3)
 * is the caller's. */
static inline int nm_attract_carry_into_cinematic(nm_intro_scene_t *s,const nm_entity_pool_t *pool,
        const nm_road_pack_t *road) {
    if(!s || !pool || !road || !road->grid) return 0;
    for(unsigned i=0;i<44;++i) { /* every entity: a later stage spawns more cars (their spawn keeps both words) */
        s->cars[i].rank_key=pool->rank_key[i];
        if(i && i<road->grid_count) s->cars[i].lap.lap=(uint8_t)(road->grid[i].lap+1);
        s->collision_motion[i]=pool->motion[i];s->collision_relative[i]=pool->relative[i];s->collision_side[i]=pool->side[i];
    }
    s->traffic_lap=(int8_t)(pool->leading_lap+1);
    s->cars[0].boundary.damage.sequence_counter=pool->crash_counter;
    for(unsigned i=0;i<44;++i) s->cars[i].checkpoints.clock=pool->clock;
    return 1;
}

/* The presentation half of the phase-6 iteration: the race view's state in VIEW (the race's presentation
 * scene, race_camera.h), its previous cells the cinematic's (POOL's), then the camera pass with the flags 1<<6
 * (6484 -> 6510). NULL or the step that refused. */
static inline const char *nm_demo_view_start(nm_race_scene_t *s,nm_intro_scene_t *view,const nm_drive_params_t *math,
        const nm_scenery_pack_t *scenery,const nm_race_cameras_t *cameras,const nm_entity_pool_t *pool) {
    if(!s || !view || !pool || s->count<3 || s->count>44) return "arguments";
    nm_memset(view,0,sizeof(*view));
    for(unsigned i=0;i<s->count;++i) { view->detail_order[i]=(uint8_t)i;view->detail_depth[i]=INT16_MAX; }
    view->count=s->count;
    view->cells=pool->cells; /* 501520 as the cinematic's scenery object left it */
    s->shared.flags=UINT32_C(1)<<6;
    const char *why=nm_race_camera_pass(s,view,math,scenery,cameras,0);
    s->shared.flags=NM_DEMO_FLAGS;
    return why;
}

/* phase 6: the field of COUNT cars on the race grid of the road's course, the camera reset with the view VIEW's
 * preset (637c), 501710 = VIEW. The race scene's marks are cleared (13e00). POOL: what the cinematic before left in
 * the entities (frontend_pool.h; the grid reset e3f4 keeps the collision words +28c..+2a4 and +27d) and the leading
 * lap 5014ba; then the phase-6 iteration's field pass 1210c (flags 1<<6, collision phase COLLISION_PHASE: 5010b0 & 3)
 * over the gridded cars. The caller's camera pass follows, with the cinematic's cells as the previous ones. */
static inline int nm_demo_start(nm_race_scene_t *s, nm_demo_t *demo, const nm_drive_params_t *math,
        const nm_contact_pack_t *contact, const nm_road_pack_t *road, const nm_path_pack_t paths[8],
        unsigned count, unsigned view, const nm_race_view_preset_t presets[4], nm_road_marks_t *marks,
        unsigned mark_capacity, const nm_entity_pool_t *pool, unsigned collision_phase) {
    if(!s || !demo || !math || !contact || !road || !paths || !presets || !marks || !marks->values ||
       road->slot>2 || contact->slot!=road->slot || !road->grid || count<3 || count>road->grid_count || count>44 ||
       view>3 || marks->count!=road->profile_count-1 || mark_capacity<road->profile_count) return 0;
    for(unsigned id=0;id<count;++id) if(road->grid[id].route>=8) return 0;
    nm_memset(s,0,sizeof(*s));nm_memset(marks->values,0,mark_capacity);marks->timer=0;
    nm_memset(demo,0,sizeof(*demo));
    s->course=(uint8_t)road->slot;s->count=(uint8_t)count;
    s->phase=s->active_phase=NM_DEMO_PHASE;s->shared.flags=NM_DEMO_FLAGS;
    s->start_stage=-1;s->view=(uint8_t)view;s->view_presets=presets;
    nm_tunnel_light_reset(&s->scene_light,nm_race_light_normal_default);
    for(unsigned id=0;id<count;++id) {
        nm_race_car_t *c=s->cars+id;c->remembered=255;
        if(!id && pool && pool->raced) nm_player_carried(&c->car,&pool->player);
        c->car.boundary.damage.flags=0x80000000;
        if(!nm_race_car_grid_reset(c,math,contact,road,paths+road->grid[id].route,id,0)) return 0;
        c->effects.height.sampled_tick=(uint16_t)(0u-5u);
        c->next=(uint16_t)((id+1)%count);c->previous=(uint16_t)((id+count-1)%count);
    }
    nm_player_t *player=&s->cars[0].car;
    player->checkpoints.total_laps=(uint8_t)road->total_laps; /* 5010ec, the lap board's count (presentation) */
    nm_memcpy(player->camera_words,math->camera_reset_words,sizeof(player->camera_words));
    nm_memcpy(player->camera_angles,math->camera_reset_angles,sizeof(player->camera_angles));
    nm_player_view_reset(math,player,(int8_t)presets[view].mode,NULL); /* 637c */
    s->passing=(nm_traffic_pass_t){s->cars[0].next,s->cars[0].previous,0};
    if(!nm_traffic_order_init(s->order.rank,s->order.track,s->count)) return 0;
    if(pool) {
        for(unsigned id=0;id<count;++id) {
            nm_race_car_t *c=s->cars+id;
            c->collision_motion=pool->motion[id];c->relative=pool->relative[id];c->collision_side=pool->side[id];
            c->car.rank_key=pool->rank_key[id];
            c->remembered=pool->remembered[id];c->rank=pool->rank[id];
        }
        for(unsigned id=count;id<44;++id) { /* as nm_frontend_race_entry */
            nm_player_t *car=&s->cars[id].car;
            car->boundary.ground.position[0]=pool->position[id][0];car->boundary.ground.position[2]=pool->position[id][1];
            car->lateral.current=pool->lateral[id];
        }
        nm_memcpy(s->order.rank,pool->rank_order,sizeof(s->order.rank));nm_memcpy(s->order.track,pool->track_order,sizeof(s->order.track));
        s->order.lap=pool->leading_lap;
        s->shared.crash_counter=pool->crash_counter;s->shared.clock=pool->clock;
    }
    s->shared.flags=UINT32_C(1)<<6;
    if(collision_phase>=4 || !nm_race_interactions_step(s->cars,s->count,&s->shared,&s->order,s->cooldowns,&s->passing,
        &s->collisions,math,road,0,collision_phase,0,0)) return 0;
    s->shared.flags=NM_DEMO_FLAGS;
    return 1;
}

/* 15314: the impact correction (+214 ticks, +218/+21c added to x/z, then three quarters of it). */
static inline int nm_demo_correction(nm_player_t *s) {
    if(!s->correction_ticks) return 1;
    const int ticks=(int)s->correction_ticks-1;
    s->correction_ticks=ticks < -128 ? 127 : (int8_t)ticks;
    if(ticks==1) s->correction[0]=s->correction[1]=0;
    for(unsigned a=0;a<2;++a) {
        const float position=s->correction[a]+s->boundary.ground.position[2*a];
        const float correction=0.75f*s->correction[a];
        if(!isfinite(position) || !isfinite(correction)) return 0;
        s->boundary.ground.position[2*a]=position;s->correction[a]=correction;
    }
    return 1;
}

/* 158c8: the input inhibit (as nm_player_early_mode's). */
static inline int nm_demo_inhibit(const nm_drive_params_t *p, nm_player_t *s) {
    if(s->input_inhibit) {
        s->control=224;
        const unsigned timer=(uint8_t)s->input_inhibit;
        s->input_inhibit=timer>128 ? (int8_t)((int)timer-257) : (int8_t)(timer-1);
        s->early_sound_request=timer==81;
        int angle=p->inhibit_angles[(s->boundary.damage.flags>>1)&1][timer];
        if(s->inhibit_direction>=0) angle=-angle;
        s->inhibit_angle=nm_yaw_angle((uint32_t)angle);
        angle=s->inhibit_angle<0 ? -(int)s->inhibit_angle : s->inhibit_angle;
        const float product=nm_force_float(0x384cccaa)*(float)angle;
        s->inhibit_scale=product-nm_force_float(0x3f59999a);
        const float speed=s->boundary.speed-p->inhibit_speed_drop;
        if(!isfinite(s->inhibit_scale) || !isfinite(speed)) return 0;
        if(!(nm_force_bits(speed)&0x80000000u)) s->boundary.speed=speed;
        s->boundary.damage.flags&=~0x18000u;
    } else if(s->speed_index<20 || (uint8_t)s->boundary.damage.inhibit_ticks) {
        s->boundary.damage.flags&=~0x18000u;
    } else if(s->boundary.damage.flags&0x18000u) {
        s->inhibit_direction=s->boundary.damage.flags&0x10000u ? -1 : 1;
        s->input_inhibit=-112;
        s->boundary.damage.flags&=~0x18000u;
        s->early_sound_request=1;
    }
    return 1;
}

/* c9c4, the demo's player (see above). variant: 503690 (5010b0 & 1); restored: the view preset's mode. */
static inline int nm_demo_player_step(nm_race_car_t *cars, unsigned count, nm_race_shared_t *shared,
        const nm_drive_params_t *math, const nm_intro_speed_params_t *speed, const nm_contact_pack_t *contact,
        const nm_road_pack_t *road, const nm_path_pack_t *path, const nm_road_marks_t *marks,
        uint8_t variant, int8_t restored) {
    if(!cars || !shared || !math || !speed || !contact || !road || !road->profiles || !path || !marks ||
       !marks->values || count<3 || count>44 || cars[0].next>=count ||
       (cars[0].remembered!=255 && cars[0].remembered>=44) ||
       (uint16_t)cars[0].car.phase[0]>=road->profile_count || (uint16_t)cars[0].car.phase[0]>=marks->count ||
       cars[0].route!=path->route || path->slot!=road->slot || contact->slot!=road->slot) return 0;
    nm_race_car_t *const me=cars;
    nm_player_t *const s=&me->car;
    if(shared->flags&UINT32_C(0x19f800fc)) {
        s->checkpoints.clock=shared->clock;s->profile_updates=shared->profile_updates;
        s->boundary.damage.sequence_counter=shared->crash_counter;
        /* +20d */
        if(s->boundary.damage.initial_inhibit) --s->boundary.damage.initial_inhibit;
        /* e6b8 */
        int32_t phase=nm_gears_signed((uint32_t)s->speed_index<<6);
        if(phase<0) phase=0;
        else if(phase>0xaaa) phase=0xaaa;
        s->wheel_phase[0]=nm_yaw_angle((uint32_t)(int32_t)s->wheel_phase[0]-(uint32_t)phase);
        s->wheel_phase[1]=s->wheel_phase[0];
        /* 11304, 11438, 10ccc: the race input as an opponent's, of entity 0 */
        const unsigned profile=(uint16_t)s->phase[0];
        const nm_guided_lane_car_t player=nm_race_lane_car(cars,0),car=player;
        const nm_guided_lane_car_t front=nm_race_lane_car(cars+me->next,me->next);
        const nm_guided_lane_car_t saved=me->remembered==255 ? car : nm_race_lane_car(cars+me->remembered,me->remembered);
        nm_guided_race_input_t race={&player,&car,&front,me->remembered==255 ? NULL : &saved,
            cars[0].car.rank_key,s->rank_key,cars[0].car.boundary.heading,cars[0].rank,me->rank,
            {shared->heading_circle[0],shared->heading_circle[1],shared->heading_circle[2]}};
        nm_player_dynamics_input_t dynamics;nm_memset(&dynamics,0,sizeof(dynamics));
        dynamics.speed_flags=shared->flags;dynamics.guided_scale=me->guided_scale;dynamics.guided_mode=shared->guided_mode;
        const unsigned mark=marks->values[profile];
        if(!nm_opponent_motion_step_inplace(math,speed,path,road->profiles+profile,&dynamics,&race,marks->timer,
            (int8_t)(mark<128 ? (int)mark : (int)mark-256),&me->remembered,s)) return 0;
        /* +58 := +2a; e72c */
        s->boundary.ground.yaw=s->body_yaw;
        s->render_angles[0]=nm_yaw_angle((uint32_t)((int)s->boundary.ground.pitch+s->filtered_pitch));
        s->render_angles[1]=nm_yaw_angle((uint32_t)((int)s->boundary.ground.yaw+s->render_yaw_offset));
        s->render_angles[2]=nm_yaw_angle((uint32_t)((int)s->boundary.ground.roll+s->render_roll_offset));
        /* 16060 */
        const nm_boundary_input_t boundary={s->speed_index,shared->flags,s->lap.completed,0,0};
        if(!nm_boundary_step(contact,math,&boundary,&s->boundary)) return 0;
        /* 15fc4, 16014, 15d60 */
        const nm_damage_pre_input_t pre={shared->flags,(uint32_t)s->path_flags|((uint32_t)s->previous_path_flags<<16),
            s->phase[0],s->boundary.ground.yaw,restored,variant,0};
        if(!nm_player_damage_prefix(math,&pre,s,0)) return 0;
        /* 15314, 158c8 */
        if(!nm_demo_correction(s) || !nm_demo_inhibit(math,s)) return 0;
        /* 11620 and the road frame, 11bd4 */
        nm_wheels_state_t *g=&s->boundary.ground;
        nm_contact_result_t center={g->position[1],g->road,0,0};
        if(!nm_contact_query(contact,math,g->position[0],g->position[2],&center)) return 0;
        g->position[1]=center.height;g->road=center.road;g->combined_mask=(uint16_t)center.mask;
        if(!nm_wheels_road_frame(contact,math,g) || !nm_wheels_transform(math,g->position,g->yaw,g->roll,g->world)) return 0;
        /* 1365c, 16e10, (caf4), 11aa4, 13de0 */
        if(!nm_intro_road_step(road,s) ||
           !nm_contact_scenery_cell(g->position[0],g->position[2],&s->scenery_cell) ||
           !nm_player_body_angles_step(math,s->tilt[0],s)) return 0;
        const int lap=s->lap.lap<128 ? s->lap.lap : (int)s->lap.lap-256;
        s->rank_key=nm_yaw_angle((uint32_t)(lap*(int)(road->profile_count-1)+s->phase[0]));
        shared->profile_updates=s->profile_updates;shared->crash_counter=s->boundary.damage.sequence_counter;
    }
    nm_player_circle_step(s); /* e078 */
    nm_memcpy(shared->heading_circle,s->proximity_circle,sizeof(shared->heading_circle));
    return 1;
}

/* One phase-7 iteration: 224ef4's view, the player, the opponents (cde0), the field pass 1210c, as the race scene
 * orders them. collision_phase: 5010b0 & 3; variant: 5010b0 & 1. The caller runs the camera pass after it.
 * course: 501460 in this iteration, the road's course but on the demo's last (2028 -> 2058 steps it before the
 * objects run): the opponents' height filter (cd30 by 501460) and the camera's drawn-car count (77d0[501460]) of
 * that iteration take the next course's (attract trace, all three demos: the heights of the cars that correct,
 * 5014f7). The scene keeps it as s->course for the camera pass after. */
static inline int nm_demo_step(nm_race_scene_t *s, nm_demo_t *demo, const nm_drive_params_t *math,
        const nm_intro_speed_params_t *speed, const nm_contact_pack_t *contact, const nm_road_pack_t *road,
        const nm_path_pack_t paths[8], nm_road_marks_t *marks, unsigned mark_capacity, unsigned collision_phase,
        uint8_t variant, const nm_crash_row_t *rows, unsigned row_count, unsigned course) {
    if(!s || !demo || !math || !speed || !contact || !road || !paths || !marks || !marks->values || s->count<3 ||
       s->count>44 || collision_phase>=4 || !s->view_presets || mark_capacity<road->profile_count ||
       course>2 || road->slot>2) return 0;
    if(course!=road->slot) {
        static nm_drive_params_t stepped; /* the drive table with the next course's height pairs in the road's */
        nm_memcpy(&stepped,math,sizeof(stepped));
        nm_memcpy(stepped.opponent_height_coefficients[road->slot*7u],math->opponent_height_coefficients[course*7u],
            7u*sizeof(stepped.opponent_height_coefficients[0]));
        math=&stepped;
    }
    s->active_phase=s->phase;s->shared.flags=UINT32_C(1)<<s->active_phase;
    /* 224ef4 */
    ++demo->counter;
    s->view=(uint8_t)((demo->counter>>7)&3u);
    const int8_t restored=(int8_t)s->view_presets[s->view].mode;
    s->shared.tick=(uint16_t)s->ticks;s->shared.updating=0;
    /* The cars read the marks of the last authored profile too (count = capacity: its byte stays 0). */
    const nm_road_marks_t opponent_marks={marks->values,mark_capacity,marks->timer};
    if(!nm_demo_player_step(s->cars,s->count,&s->shared,math,speed,contact,road,paths+s->cars[0].route,
        &opponent_marks,variant,restored)) return 0;
    for(unsigned id=1;id<s->count;++id) {
        s->shared.updating=(uint8_t)id;
        if(!nm_race_opponent_step_inplace(s->cars,s->count,id,&s->shared,math,speed,contact,road,
            paths+s->cars[id].route,&opponent_marks,rows,row_count)) return 0;
    }
    s->shared.updating=(uint8_t)s->count;
    if(!nm_race_interactions_step(s->cars,s->count,&s->shared,&s->order,s->cooldowns,&s->passing,&s->collisions,
        math,road,0,collision_phase,0,0)) return 0;
    s->course=(uint8_t)course;
    ++s->ticks;
    return 1;
}

/* Phases 8 (20d4, the demo's natural end) and 17 (2b0c, a credit in it): the handler switches every object off (flags
 * bit 31); the field object 1210c stays on and runs once more over the switched-off field: the travel of the cars in
 * its 14-car window becomes zero. Its later passes change nothing (the original's pool over phases 8 to 18). */
static inline int nm_demo_field_off(nm_race_scene_t *s, const nm_drive_params_t *math, const nm_road_pack_t *road,
        unsigned phase, uint32_t iteration) {
    for(unsigned id=0;id<44;++id) s->cars[id].car.boundary.damage.flags&=UINT32_C(0x7fffffff);
    s->shared.flags=UINT32_C(1)<<phase;
    if(!nm_race_interactions_step(s->cars,s->count,&s->shared,&s->order,s->cooldowns,&s->passing,&s->collisions,
        math,road,0,iteration&3u,0,0)) return 0;
    ++s->ticks;
    return 1;
}

/* The two iterations after the one in which phase 7's handler saw the credit (2028 -> 20bc: that iteration is an
 * ordinary demo tick, nm_demo_step): the game start over the demo, as frontend.h's over the cinematic.
 *  - phase 12 (2834): every object still runs, with the flags 1<<12: the player's c9c4 takes only its e078 (its mask
 *    19f800fc excludes bit 12), the opponents' cde0 has no mask and moves them as before; then 1210c.
 *  - phase 17: nm_demo_field_off.
 * ITERATION: 5010b0 of the phase-12 iteration. The race entry then reads the pool as the demo's cars left it. */
static inline int nm_demo_game_start(nm_race_scene_t *s, const nm_drive_params_t *math,
        const nm_intro_speed_params_t *speed, const nm_contact_pack_t *contact, const nm_road_pack_t *road,
        const nm_path_pack_t paths[8], nm_road_marks_t *marks, unsigned mark_capacity, uint32_t iteration,
        const nm_crash_row_t *rows, unsigned row_count) {
    if(!s || !math || !speed || !contact || !road || !paths || !marks || !marks->values || s->count<3 || s->count>44 ||
       !s->view_presets || mark_capacity<road->profile_count) return 0;
    const nm_road_marks_t opponent_marks={marks->values,mark_capacity,marks->timer};
    s->shared.flags=UINT32_C(1)<<12;
    s->shared.tick=(uint16_t)s->ticks;
    if(!nm_demo_player_step(s->cars,s->count,&s->shared,math,speed,contact,road,paths+s->cars[0].route,
        &opponent_marks,(uint8_t)(iteration&1u),(int8_t)s->view_presets[s->view].mode)) return 0;
    for(unsigned id=1;id<s->count;++id) {
        s->shared.updating=(uint8_t)id;
        if(!nm_race_opponent_step_inplace(s->cars,s->count,id,&s->shared,math,speed,contact,road,
            paths+s->cars[id].route,&opponent_marks,rows,row_count)) return 0;
    }
    s->shared.updating=(uint8_t)s->count;
    if(!nm_race_interactions_step(s->cars,s->count,&s->shared,&s->order,s->cooldowns,&s->passing,&s->collisions,
        math,road,0,iteration&3u,0,0)) return 0;
    ++s->ticks;
    return nm_demo_field_off(s,math,road,17,iteration+1u);
}
#endif
