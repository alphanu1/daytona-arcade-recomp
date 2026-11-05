#ifndef DAYTONA_NATIVE_PRACTICE_H
#define DAYTONA_NATIVE_PRACTICE_H
#include "player.h"
#include "contact_clear.h"
#include "pose.h"
#include "reflection.h"
#include "race.h"
#include "frame_clock.h"
#include "nm_mem.h"

/* First persistent single-car integration. Practice spawn, NOT the arcade
 * grid/countdown/race dispatcher. The physics itself is the imported player.
 * ponytail: course0/route0 only until native race selection/spawn is connected. */
/* The unit of one simulation tick (the arcade's 57.5 Hz) for the host harness and the updaters' elapsed time. The
 * console runs one tick per displayed frame, 59.94 Hz, counted from vblanks (frame_clock.h): a frame hands its
 * updater ticks * NM_PRACTICE_TICK_US, so the accumulators below step whole ticks and the game runs 4% faster. */
#define NM_PRACTICE_TICK_US 17384u
typedef struct {
    nm_player_t car;
    nm_checkpoint_award_t award;
    uint32_t ticks, remainder_us, dropped_us;
    uint16_t reflection_phase, reflection_u, reflection_v;
    /*132b8 writes full32-bit elapsed ticks at50159c+4*byte-index.
     * Keep native storage, including index0/reverse crossings and byte wrap. */
    uint32_t lap_times[256];
    uint8_t lap_recorded[32], last_lap_index;
    uint32_t start_flags, phase_timer;
    float reference_height;
    uint8_t rolling_active, rolling_enabled, race_phase;
} nm_practice_t;

static inline void nm_practice_record_lap(nm_practice_t *s) {
    const nm_lap_write_t *write=&s->car.lap_write;
    if(!write->written) return;
    s->lap_times[write->index]=write->ticks;
    s->lap_recorded[write->index>>3]|=(uint8_t)(1u<<(write->index&7));
    s->last_lap_index=write->index;
}

static inline int nm_practice_reset(nm_practice_t *out, const nm_drive_params_t *math,
        const nm_contact_pack_t *contact, const nm_road_pack_t *road,
        const nm_path_pack_t *path, nm_road_marks_t *marks) {
    if (!out || !math || !contact || !road || !path || !marks || !marks->values ||
        contact->slot || road->slot || path->slot || path->route || !path->rows ||
        !road->profiles || road->profile_count < 2 || marks->count != road->profile_count-1 ||
        road->checkpoint_count > 30) return 0;
    nm_practice_t s; nm_memset(&s,0,sizeof(s));
    /* Legacy practice packs use profile0. DNR5 supplies the authored ID0 grid
     * position; rolling-start speed/phase entry are not implied by this reset. */
    s.car.lateral.current = s.car.lateral.target = 1408;
    unsigned profile=0;
    if (road->grid_count) {
        if (!road->grid || road->grid[0].route != path->route ||
            road->grid[0].profile >= road->profile_count-1) return 0;
        const nm_road_grid_t *grid=road->grid;
        profile=grid->profile;
        s.car.phase[0]=nm_yaw_angle(profile); s.car.phase[1]=nm_yaw_angle(grid->previous);
        s.car.lap.furthest=(uint16_t)grid->previous;
        s.car.lap.lap=(uint8_t)grid->lap;
        s.car.path_offset=(uint16_t)grid->path_offset;
        s.car.lateral.current=s.car.lateral.target=(int16_t)grid->lateral;
    }
    nm_road_pose_t pose;
    if (!nm_road_pose_step(road->profiles+profile,&s.car.lateral,&pose)) return 0;
    nm_wheels_state_t *ground = &s.car.boundary.ground;
    nm_memcpy(ground->position,pose.xyz,sizeof(pose.xyz));
    ground->pitch = nm_yaw_angle((uint32_t)pose.angles[0]);
    ground->yaw = nm_yaw_angle((uint32_t)pose.angles[1]);
    ground->roll = nm_yaw_angle((uint32_t)pose.angles[2]);
    s.car.body_yaw = s.car.boundary.heading = ground->yaw;
    if (!nm_wheels_ground(contact,math,ground)) return 0;
    if (!nm_contact_scenery_cell(ground->position[0],ground->position[2],&s.car.scenery_cell)) return 0;
    s.car.render_angles[0] = ground->pitch;
    s.car.render_angles[1] = ground->yaw;
    s.car.render_angles[2] = ground->roll;
    s.car.checkpoints.visited = 1;
    s.car.checkpoints.required = (1u<<(road->checkpoint_count+1))-1u;
    s.car.checkpoints.total_laps = road->total_laps ? (uint8_t)road->total_laps : 8;
    /* Long diagnostic session; arcade countdown/finish dispatch is still open. */
    s.award.remaining = 60000;
    nm_memset(marks->values,0,marks->count); marks->timer = 0;
    *out = s;
    return 1;
}

/* Enable the prepared rolling start in the existing single-car session.
 * ponytail: difficulty0 until the course/difficulty selection UI is imported. */
static inline int nm_practice_start_rolling(nm_practice_t *s, const nm_drive_params_t *math,
        const nm_contact_pack_t *contact, const nm_road_pack_t *road, const nm_path_pack_t *path) {
    if (!s || s->ticks || s->remainder_us) return 0;
    nm_race_settings_t settings;
    if (!nm_race_settings(road,0,&settings)) return 0;
    nm_player_t car; nm_memset(&car,0,sizeof(car));
    car.checkpoints=s->car.checkpoints;
    car.boundary.damage.flags=UINT32_C(0x80000000);
    if (!nm_player_rolling_start(math,contact,road,path,0,&car,&s->reference_height,&s->start_flags)) return 0;
    s->car=car;
    s->car.checkpoints.total_laps=settings.total_laps;
    s->award.remaining=settings.initial_ticks;
    s->race_phase=22;
    s->rolling_active=s->rolling_enabled=1;
    return 1;
}

/* Same portable path used by host continuation checks and enDjinn input.
 * Caller owns the loaded assets and marks; no allocations or reference refresh.
 * Bound catch-up to four ticks and expose discarded time instead of claiming
 * wall-clock fidelity after a stall. A rejection stops the session explicitly. */
static inline int nm_practice_advance(nm_practice_t *s, const nm_drive_params_t *math,
        const nm_contact_pack_t *contact, const nm_road_pack_t *road,
        const nm_path_pack_t *path, nm_road_marks_t *marks,
        const nm_controls_t *controls, uint32_t elapsed_us) {
    if (!s || !controls || controls->steer < -127 || controls->steer > 127) return 0;
    const uint32_t maximum = 4u*NM_PRACTICE_TICK_US;
    if (elapsed_us > maximum) { s->dropped_us += elapsed_us-maximum; elapsed_us = maximum; }
    s->remainder_us += elapsed_us;
    nm_player_tick_input_t in = {.active={.dynamics={.speed_flags=1u<<22,
        .motion_scale=1,.guided_scale=1},.proximity_disabled=1},.phase=22};
    in.active.controls = *controls;
    while (s->remainder_us >= NM_PRACTICE_TICK_US) {
        const uint8_t active_phase=s->rolling_enabled ? s->race_phase : 22;
        if (s->rolling_enabled) {
            /* Original03520 blocks the entire timer decision during rolling.
             * Phase22 precedes player physics: first normal entry sees clock1.
             * Single-player mode74/player204/hold are all zero in this session. */
            if (active_phase==22 && !(s->start_flags&1u))
                s->race_phase=nm_race_active_timer(0,0,0,&s->award.remaining,
                    &s->car.checkpoints.clock,&s->car.checkpoints.finish_flags);
            else if (active_phase==23) {
                s->car.camera_mode=0;
                s->phase_timer=384;
                if (!nm_player_finish_entry(&s->car)) return 0;
                s->race_phase=24;
            } else if (active_phase==24) {
                /* ponytail: rank0/mode0 for this one-car session. Replace with
                 * actual opponent ranking when multi-car gameplay is connected. */
                s->race_phase=nm_race_finish_timer(&s->phase_timer,0,0);
            } else if (active_phase==25) {
                s->phase_timer=nm_race_timeout_ticks(0);
                s->race_phase=26;
            } else if (active_phase==26) {
                uint32_t mode=0; /* Same single-car mode as finish selection. */
                s->race_phase=nm_race_timeout_timer(&s->phase_timer,&mode);
            }
            if (active_phase!=22 && active_phase!=23 && active_phase!=24) {
                /* ponytail: results/timeout presentation and following screen
                 * handlers remain to be imported; retain the last pose. */
                ++s->ticks; s->remainder_us-=NM_PRACTICE_TICK_US;
                continue;
            }
        }
        in.phase=s->rolling_enabled ? s->race_phase : 22;
        in.active.dynamics.speed_flags=1u<<active_phase;
        in.active.dynamics.lap = s->car.lap.completed;
        in.active.dynamics.guided_scale=1;
        if (active_phase==23 || active_phase==24) {
            if (!road || !road->grid_guided_scales) return 0;
            in.active.dynamics.guided_scale=road->grid_guided_scales[0];
            if (!nm_player_finish_prefix(math,contact,road,path,&in.active,&s->car)) return 0;
        } else if (s->rolling_active) {
            if (!road || !road->grid_guided_scales) return 0;
            in.active.dynamics.guided_scale=road->grid_guided_scales[0];
            uint8_t released=0;
            if (!nm_player_rolling_tick(math,contact,road,path,&in.active,&s->car,&s->start_flags,&released)) return 0;
            if (released) s->rolling_active=0;
        } else {
            if (!nm_player_tick_step(math,contact,road,path,&in,&s->car,&s->award,marks)) return 0;
            nm_practice_record_lap(s);
        }
        if (s->rolling_enabled) {
            nm_player_effect_timers(&s->car);  /* 6c18 -> 9014 */
            (void)nm_player_contact_clear(&s->car); /* 9088 -> 9148, contact_clear.h */
        }
        /* Course0 practice has no neighboring car or game-mode transition.
         * Phase starts at zero here; arcade grid/global-frame alignment remains
         * part of race startup, not a captured-frame phase baked into assets. */
        const nm_reflection_input_t reflection={.frame=s->ticks,.speed_index=(uint32_t)s->car.speed_index,
            .flags=s->car.path_flags,.previous_flags=s->car.previous_path_flags,
            .heading=nm_follow_heading(&s->car),.course=0,.next_mode=in.phase,.active_mode=active_phase};
        nm_reflection_update_t update;
        if(!nm_reflection_step(&reflection,&s->reflection_phase,&update)) return 0;
        if(update.action==NM_REFLECTION_UV) {
            s->reflection_u=update.u_offset;s->reflection_v=update.v_offset;
        }
        if (!s->rolling_enabled) {
            ++s->car.checkpoints.clock;
            if (s->award.remaining) --s->award.remaining;
        }
        ++s->ticks; s->remainder_us -= NM_PRACTICE_TICK_US;
    }
    return 1;
}
int nm_practice_run(const char *root);
/* Title owns no course data. After releasing its PVR assets, it calls this
 * to rebuild the existing live-race presentation and replace its mode. */
int nm_practice_restart_from_title(void);
/* The results' last iteration has run: the pool takes what the game leaves, the frame counter its count. */
void nm_practice_game_over(void);
int nm_practice_attract_start(void);
/* The next attract scene loads in the title's frames (scene_loader.h): started at the title's start, still
 * loading, and its end after the title's release (the rest at once, then its palettes). */
int nm_practice_attract_preload(void);
int nm_practice_attract_loading(void);
void nm_practice_attract_settle(void);
void nm_practice_attract_abandon(void);
/* The asset root of the running session (NULL after a failure), for the front-end modes. */
const char *nm_practice_root(void);
/* The race entered from the select screens (gameplay/select_mode.c): chosen course and transmission. */
int nm_practice_race_from_select(unsigned course,unsigned transmission);
/* The front end's course choice for the next race (code/practice.c): the slot 501460, 0 Beginner, 1 Expert,
 * 2 Advanced. 0: the build holds no race-start or podium tables for it (the race keeps the previous choice);
 * its packs are loaded, and checked, when that race starts (nm_practice_restart_from_title). */
int nm_practice_course_select(unsigned course);
#endif
