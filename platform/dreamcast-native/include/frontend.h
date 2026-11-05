#ifndef DAYTONA_NATIVE_FRONTEND_H
#define DAYTONA_NATIVE_FRONTEND_H
#include "intro_scene.h"
#include "race_scene.h"
#include "car_detail.h"
#include "select.h"

/* The state the original carries from scene to scene (build/dc-native-frontend.PLAN.md, "What reaches the race").
 * The original has one pool of 44 car entities (504e00 + id*0x300) shared by every scene, never cleared; the race
 * entry (3008, the grid resets) keeps each entity's collision tracking words from the scene before: the saved
 * position +28c/+290, the travel +294/+298, the contact side +27d and the relative +2a4; the neighbour link +244;
 * the player's words nm_player_carried keeps; and the sort pass's leading lap 5014ba (written only by 12458) and
 * the crash orientation counter 503680 (15e54). Natively each scene has its own cars; nm_entity_pool_t is the pool's
 * carried part, filled by each scene's exit, so a game after a race, or started from the demo or the title, enters
 * like the first one.
 *
 * The frame counter 5010b0 counts main-loop iterations since power-on (lag frames add none): the
 * cinematic attract's first tick is iteration 3 after power-on (intro_scene.h object_clock), and the
 * race inherits the count as nm_race_scene_t.counter_base. */
#include "frontend_pool.h" /* nm_entity_pool_t */
#include "nm_mem.h"

/* The cinematic's car laps count from 0 where the original's spawns (cf74/cfc0) write 255, so its
 * leading lap is the original's plus one (both orders compare laps only against each other).
 * Measured on five game starts: -1 four times, 0 for a start at frame 1090. */
static inline int8_t nm_frontend_leading_lap(const nm_intro_scene_t *s) {
    return (int8_t)(s->traffic_lap-1);
}

/* The end of a cinematic (a credit, the demo after it): its cars into the pool, the other entities stay. ROAD: the
 * course of an attract cinematic, whose player runs caf4 on each tick (d284): the low half of +b0 := -(the profile's
 * +2) of the profile it stands on. NULL for the podium: the next cinematic writes it. */
static inline void nm_frontend_pool_from_intro(nm_entity_pool_t *pool,const nm_intro_scene_t *s,const nm_road_pack_t *road) {
    for(unsigned i=0;i<s->count && i<44;++i) {
        pool->motion[i]=s->collision_motion[i];pool->relative[i]=s->collision_relative[i];pool->side[i]=s->collision_side[i];
        pool->rank_key[i]=s->cars[i].rank_key;
        pool->position[i][0]=s->cars[i].boundary.ground.position[0];pool->position[i][1]=s->cars[i].boundary.ground.position[2];
        pool->lateral[i]=s->cars[i].lateral.current;
    }
    nm_memcpy(pool->rank_order,s->rank_order,sizeof(pool->rank_order));nm_memcpy(pool->track_order,s->track_order,sizeof(pool->track_order));
    pool->leading_lap=nm_frontend_leading_lap(s);
    pool->crash_counter=s->cars[0].boundary.damage.sequence_counter;
    pool->cells=s->cells;
    if(road) {
        const unsigned profile=(uint16_t)s->cars[0].phase[0];
        const nm_road_profile_t *row=profile<road->profile_count ? road->profiles+profile : s->overrun;
        if(row) pool->steer=(int32_t)(((uint32_t)pool->steer&UINT32_C(0xffff0000))|(uint16_t)(0-(uint16_t)row->profile_control));
    }
}

/* The end of a scene on the race's objects (the V.R. demo, a race): its cars into the pool; CELLS: the visible cells
 * of its camera pass. */
static inline void nm_frontend_pool_from_race(nm_entity_pool_t *pool,const nm_race_scene_t *s,const nm_scenery_cells_t *cells) {
    for(unsigned i=0;i<s->count && i<44;++i) {
        const nm_race_car_t *c=s->cars+i;
        pool->motion[i]=c->collision_motion;pool->relative[i]=c->relative;pool->side[i]=c->collision_side;
        pool->remembered[i]=c->remembered;pool->rank_key[i]=c->car.rank_key;pool->rank[i]=c->rank;
        pool->position[i][0]=c->car.boundary.ground.position[0];pool->position[i][1]=c->car.boundary.ground.position[2];
        pool->lateral[i]=c->car.lateral.current;
    }
    nm_memcpy(pool->rank_order,s->order.rank,sizeof(pool->rank_order));nm_memcpy(pool->track_order,s->order.track,sizeof(pool->track_order));
    pool->leading_lap=s->order.lap;pool->view=s->view&3u;
    pool->crash_counter=s->shared.crash_counter;pool->clock=s->shared.clock;
    pool->cells=*cells;
}

/* The podium scene runs on the race's entities: the crash counter 503680 and the race clock 5010cc go on in it. */
static inline void nm_frontend_podium_start(nm_intro_scene_t *podium,const nm_race_scene_t *race) {
    podium->cars[0].boundary.damage.sequence_counter=race->shared.crash_counter;
    for(unsigned i=0;i<44;++i) podium->cars[i].checkpoints.clock=race->shared.clock;
}

/* The end of a game: what the race leaves, entity 0 whole with it (only the race's player block writes the words the
 * next race's player reads first). PODIUM: the podium scene when one ran, else NULL: it re-spawns the entities of its
 * largest stage, and its field pass leaves their tracking words. The rank key, the lap and the sort lists stay the
 * race's: the original's podium cars keep those words, and 5014ba only has to stay above every lap of the cinematic
 * after it (122fc), as the race's does (the original's: 9 after the 8 laps of Beginner). */
static inline void nm_frontend_pool_from_game(nm_entity_pool_t *pool,const nm_race_scene_t *s,
        const nm_scenery_cells_t *cells,const nm_intro_scene_t *podium) {
    nm_frontend_pool_from_race(pool,s,cells);
    NM_COPY(&pool->player,&s->cars[0].car);pool->raced=1;pool->steer=s->cars[0].car.steer;
    if(!podium) return;
    unsigned count=0;
    for(unsigned k=0;k<podium->stage_count;++k) if(podium->stages[k].cars>count) count=podium->stages[k].cars;
    for(unsigned i=0;i<count && i<44;++i) {
        pool->motion[i]=podium->collision_motion[i];pool->relative[i]=podium->collision_relative[i];
        pool->side[i]=podium->collision_side[i];
        pool->position[i][0]=podium->cars[i].boundary.ground.position[0];pool->position[i][1]=podium->cars[i].boundary.ground.position[2];
        pool->lateral[i]=podium->cars[i].lateral.current;
    }
    pool->crash_counter=podium->cars[0].boundary.damage.sequence_counter;
}

/* The 1210c pass of a cinematic iteration (sort 1224c, timers 12150, passing 125b0, collision 13f08) with
 * the latched flags of `phase`. collision phase 503692 = (phase_clock + ticks before the iteration) & 3. */
static inline int nm_frontend_field_pass(nm_intro_scene_t *s,const nm_drive_params_t *math,
        const nm_road_pack_t *road,unsigned phase) {
    if(!nm_intro_scene_traffic(s) || !nm_intro_scene_near(s,math) || !nm_intro_scene_signals(s,math) ||
       !nm_intro_scene_collision_prepare(s,road)) return 0;
    const nm_collision_damage_input_t input={.global_flags=1u<<phase,.control=s->cars[0].control,
        .player_mode=s->cars[0].lap.completed,.curve=0};
    return nm_intro_scene_collide(s,math,road,(s->phase_clock+s->ticks)&3,UINT32_C(0x80000000),&input);
}

/* The two iterations after the one in which the phase 3 handler saw the credit (2818: next phase 12;
 * that iteration is an ordinary cinematic tick, nm_intro_scene_step):
 *  - phase 12 (2834): every attract object still runs, with the flags 1<<12. The opponents' updaters
 *    (d290/d118) move their cars as in the cinematic, the player's d230 only draws (its phase mask
 *    19f800fc excludes bit 12), the stage events run, then 1210c: the player's travel becomes zero.
 *  - phase 17 (2b0c): the handler switches every attract object off (flags bit 31) and falls into
 *    phase 18's substate 0 (select.h nm_select_enter); 1210c stays on and runs once more over the
 *    switched-off field: the travel of the cars in its 14-car window becomes zero.
 * Then the pool holds what the race inherits (every select iteration leaves it as it is).
 * Verified against the original's pool on five game starts (frames 390, 690, 1090, 1290, 1690):
 * every word of the 20/21 cinematic cars equal after each of the two iterations; the other
 * entities are untouched since power-on (zero) in those runs. */
static inline int nm_frontend_game_start(nm_intro_scene_t *s,const nm_drive_params_t *math,
        const nm_contact_pack_t *contact,const nm_road_pack_t *road,const nm_path_pack_t paths[8],
        nm_entity_pool_t *pool) {
    if(!s || !math || !contact || !road || !paths || !pool || !s->stages || s->stage>=s->stage_count ||
       s->pending_stage!=UINT32_MAX) return 0;
    const uint32_t flags=s->flags;
    const nm_intro_stage_data_t *stage=s->stages+s->stage;
    s->flags=1u<<12;
    if(s->count!=stage->cars || !nm_intro_events_step(stage->events,stage->event_count,
            s->cars,s->count,&s->events,&s->pending_stage)) return 0;
    for(unsigned i=1;i<s->count;++i) {
        const nm_intro_spawn_t *spawn=stage->spawn+i;
        const unsigned profile=(uint16_t)s->cars[i].phase[0];
        if(!road->profiles || profile>road->profile_count || (profile==road->profile_count && !s->overrun) ||
           !nm_intro_drive_step_profile_inplace(math,&nm_intro_speeds,road,paths+spawn->route,
               profile==road->profile_count ? s->overrun : road->profiles+profile,s->flags,0,s->cars+i) ||
           !nm_intro_pose_step_inplace(contact,math,s->cars+i,s->collision_other_yaw[i],s->correction_yaw+i) ||
           !nm_intro_scene_crash(s,i)) return 0;
    }
    if(!nm_frontend_field_pass(s,math,road,12)) return 0;
    ++s->ticks;
    for(unsigned i=0;i<44;++i) s->cars[i].boundary.damage.flags&=UINT32_C(0x7fffffff);
    s->flags=1u<<17;
    if(!nm_frontend_field_pass(s,math,road,17)) return 0;
    ++s->ticks;
    s->flags=flags;
    nm_frontend_pool_from_intro(pool,s,road);
    return 1;
}

/* After nm_race_scene_start_carried (3008's grid resets): the pool's words into the race cars (entity id = car
 * id), the leading lap into the first sort pass, and the frame counter of the phase20 tick. */
static inline int nm_frontend_race_entry(nm_race_scene_t *race,const nm_entity_pool_t *pool,uint32_t counter_base) {
    if(!race || !pool || race->count>44) return 0;
    for(unsigned id=0;id<race->count;++id) {
        nm_race_car_t *c=race->cars+id;
        c->collision_motion=pool->motion[id];c->relative=pool->relative[id];c->collision_side=pool->side[id];
        c->remembered=pool->remembered[id];c->car.rank_key=pool->rank_key[id];
        if(id) c->rank=pool->rank[id]; /* the player's: c894, the race entry's */
    }
    /* A grid of fewer cars than the last scene's: +244 may name an entity this race does not run. 136e8 measures the
     * distance to it (6 or more clears the link) and reads its lateral; those words are the old scene's. */
    for(unsigned id=race->count;id<44;++id) {
        nm_player_t *car=&race->cars[id].car;
        car->boundary.ground.position[0]=pool->position[id][0];car->boundary.ground.position[2]=pool->position[id][1];
        car->lateral.current=pool->lateral[id];
    }
    race->cars[0].car.steer=pool->steer;
    race->order.lap=pool->leading_lap;
    race->shared.crash_counter=pool->crash_counter;
    race->counter_base=counter_base;
    return 1;
}

/* The drawn-car count 5014f7 the opponents read on the race's first tick (10de4: their guided target).
 * The race entry rebuilds the detail list (8480: the 39 opponents, depth 0x7fff); the first pass
 * requests levels (84e0) with the race's own first view but the cell bitmap 501520 the previous scene's
 * scenery object wrote (5890 runs after the detail pass and is off during the select screens), and
 * 7698 counts the requests: (count >> 2) under the phase flags. `view` is the race's initial view
 * (after its own initial camera/detail pass, which keeps its presentation results). Measured on
 * game starts at frames 1290, 390 and 1690: 0, 2 and 2 at the first tick; the races equal to the
 * original's on every row only with these values (the race's own cells give 0 for all three). */
static inline int nm_frontend_race_guided(nm_race_scene_t *race,const float view[12],const nm_entity_pool_t *pool) {
    if(!race || !view || !pool || race->count<2 || race->count>44) return 0;
    const nm_player_t *cars[44];int16_t depth[44];uint8_t order[44],level[44]={0};
    for(unsigned i=0;i<race->count;++i) { cars[i]=&race->cars[i].car;depth[i]=INT16_MAX;order[i]=(uint8_t)i; }
    return nm_race_detail_step(view,&pool->cells,cars,race->count,depth,order,level) &&
        nm_car_detail_guided_mode(level,race->count,1u<<19,0,race->course,&race->shared.guided_mode);
}

#endif
