#ifndef DAYTONA_NATIVE_HUD_SCENE_H
#define DAYTONA_NATIVE_HUD_SCENE_H
#include "hud.h"
#include "race_scene.h"

/* The native owners of the HUD's source fields. Read-only on the scene; `cars`
 * keeps the lap-time table between calls (the original's 50159c, written by
 * 13430 inside the player's update) and the award counter last seen, so pass
 * the same object every tick and clear it at scene entry (then set its math).
 * This tick's events: the lap line (lap_write, cleared by every active step of
 * the player: 13410 sets 5010dc) and a time award (award.counter moved: 13534
 * sets 5010d4, the native award.display). */
static inline void nm_hud_observe(const nm_race_scene_t *s, nm_hud_race_t *race, nm_hud_cars_t *cars) {
    const nm_player_t *p=&s->cars[0].car;
    race->active_phase=s->active_phase;race->phase=s->phase;
    race->remaining=(int32_t)s->award.remaining;race->clock=s->shared.clock;
    /* After a tick, ticks counts it: 5010b0 = base + ticks - 1 (slot_machine.h). */
    race->counter=s->counter_base+s->ticks-1u;race->reverse=s->wrong_way;
    race->hold=s->shared.timer_hold;race->stage=s->start_stage;race->accel=s->accel;
    race->lap_last=p->lap.last_ticks;race->finish_flags=p->checkpoints.finish_flags;race->held=0;
    cars->start_flags=s->start_flags;
    cars->completed=p->lap.completed;cars->total_laps=p->checkpoints.total_laps;
    cars->rank=s->cars[0].rank;cars->count=s->count;
    cars->best_ticks=p->lap.best_ticks;cars->start_ticks=p->lap.start_ticks;
    cars->speed_index=p->speed_index;cars->filtered_rpm=p->filtered_rpm;cars->gear=p->gear;
    cars->x=p->boundary.ground.position[0];cars->z=p->boundary.ground.position[2];
    cars->yaw=(uint16_t)p->boundary.ground.yaw;
    if(p->lap_write.written && p->lap_write.index>=1 && p->lap_write.index<=8)
        cars->lap_times[p->lap_write.index-1]=p->lap_write.ticks;
    race->lap_line=p->lap_write.written;
    race->award=s->award.counter!=cars->award_counter;cars->award_counter=s->award.counter;
    /* 12ec8 and 131d8: ldis +58 + ldis +d6 (inhibit_angle) of the player and of every opponent. */
    cars->angle=(uint16_t)(p->boundary.ground.yaw+p->inhibit_angle);
    cars->others=0;
    for(unsigned id=1;id<s->count && cars->others<NM_HUD_RADAR_CARS;++id) {
        const nm_player_t *o=&s->cars[id].car;
        nm_hud_radar_car_t *r=cars->other+cars->others++;
        r->x=o->boundary.ground.position[0];r->z=o->boundary.ground.position[2];
        r->angle=(uint16_t)(o->boundary.ground.yaw+o->inhibit_angle);r->id=(uint8_t)id;
    }
    /* 12b34..12be0: first opponent in list order with the wanted rank byte. */
    const unsigned wanted=cars->rank==0;
    cars->leader_valid=0;cars->leader_second=(uint8_t)wanted;
    for(unsigned id=1;id<s->count;++id) if(s->cars[id].rank==wanted) {
        const nm_player_t *o=&s->cars[id].car;
        cars->leader_valid=1;
        cars->leader_x=o->boundary.ground.position[0];cars->leader_z=o->boundary.ground.position[2];
        cars->leader_yaw=(uint16_t)o->boundary.ground.yaw;
        break;
    }
}
#endif
