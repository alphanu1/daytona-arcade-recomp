#ifndef DAYTONA_NATIVE_PODIUM_H
#define DAYTONA_NATIVE_PODIUM_H
#include <stdint.h>

/* 03b84 phase27 entry falling into 03f3c phase28: control timer only. The
 * cinematic tasks it gates (stage graph 2265a4, cars cf74/c9b0, camera 5b68)
 * are separate owners and are NOT implemented or approximated here.
 * 01758 runs this handler BEFORE the 01838 task loop, so a tick's flags
 * describe the tasks of that same tick. Intents are valid for one tick:
 *  entered: 1a240/1a2b4 blank tile layers A/B and the cinematic tasks start;
 *  cleanup: timer==384 disables every cinematic task and loads the 2800014
 *           character/palette overlay (5460), no audio.
 * cinematic stays 1 from entry through the last tick before cleanup.
 *  spawn: the tick AFTER entry. Entry only arms the stage graph and the cars'
 *         spawn updaters (cf74/cfc0); their spawn poses and the first camera
 *         key appear on the following object pass, and movement one pass later. */
typedef struct {
    int32_t timer;
    uint8_t phase,cinematic,entered,cleanup,spawn;
} nm_podium_t;

/* One fixed tick. Returns 0 without mutation unless phase is 27 or 28.
 * Phase25 is selected only after the decrement goes below zero. */
static inline int nm_podium_step(nm_podium_t *s) {
    if(!s || (s->phase!=27 && s->phase!=28)) return 0;
    s->entered=s->cleanup=s->spawn=0;
    if(s->phase==27) { s->timer=1600;s->phase=28;s->cinematic=s->entered=1; }
    --s->timer;
    if(s->timer==1598) s->spawn=1;
    if(s->timer==384) { s->cinematic=0;s->cleanup=1; }
    else if(s->timer<0) s->phase=25;
    return 1;
}
#endif
