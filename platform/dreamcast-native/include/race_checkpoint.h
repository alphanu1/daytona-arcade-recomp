#ifndef DAYTONA_NATIVE_RACE_CHECKPOINT_H
#define DAYTONA_NATIVE_RACE_CHECKPOINT_H
#include "race_scene.h"
#include <stdio.h>

/* Diagnostic only: one comparison row of the whole race flow, shared by
 * code/practice.c (NATIVE_RACE_CHECKPOINTS) and tests/trace_race_native.c so
 * the console and host print the same text. No gameplay state is changed.
 * tick counts every simulated tick from race start through podium and results.
 * timer is the active owner's phase timer: finish (phase24), podium (27/28),
 * results (25/26), otherwise0. cars/count are the live scene's cars: the race
 * field, or the podium scene's cars once they have spawned. */
enum { NM_RACE_CHECKPOINT_WORDS=13 };

static inline int nm_race_checkpoint_due(uint32_t tick,unsigned phase,uint8_t *last_phase) {
    const int changed=phase!=*last_phase;
    *last_phase=(uint8_t)phase;
    return tick<=3 || tick%50==0 || changed;
}

static inline void nm_race_checkpoint_row(uint32_t row[NM_RACE_CHECKPOINT_WORDS],uint32_t tick,
        const nm_race_scene_t *race,int32_t timer,const nm_player_t *const cars[],unsigned count) {
    uint32_t hash=UINT32_C(2166136261);
    for(unsigned id=0;id<count;++id) {
        const uint32_t words[3]={nm_force_bits(cars[id]->boundary.ground.position[0]),
            nm_force_bits(cars[id]->boundary.ground.position[2]),nm_force_bits(cars[id]->boundary.speed)};
        for(unsigned w=0;w<3;++w) hash=(hash^words[w])*UINT32_C(16777619);
    }
    const nm_player_t *car=&race->cars[0].car;
    row[0]=tick;row[1]=race->phase;row[2]=(uint32_t)timer;row[3]=(uint16_t)car->phase[0];
    row[4]=nm_force_bits(car->boundary.speed);row[5]=nm_force_bits(car->boundary.ground.position[0]);
    row[6]=nm_force_bits(car->boundary.ground.position[2]);row[7]=race->cars[0].rank;row[8]=car->lap.completed;
    row[9]=car->checkpoints.finish_flags;row[10]=race->shared.guided_mode;row[11]=(uint8_t)car->camera_mode;
    row[12]=hash;
}

static inline void nm_race_checkpoint_print(FILE *out,const uint32_t row[NM_RACE_CHECKPOINT_WORDS]) {
    fprintf(out,"CHECKPOINT %5lu  %2lu %5ld | %3lu %08lx %08lx %08lx %2lu %lu %lx | %lu %lu | %08lx\n",
        (unsigned long)row[0],(unsigned long)row[1],(long)(int32_t)row[2],(unsigned long)row[3],
        (unsigned long)row[4],(unsigned long)row[5],(unsigned long)row[6],(unsigned long)row[7],
        (unsigned long)row[8],(unsigned long)row[9],(unsigned long)row[10],(unsigned long)row[11],
        (unsigned long)row[12]);
}
#endif
