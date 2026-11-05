#ifndef DAYTONA_NATIVE_RACE_INTENTS_H
#define DAYTONA_NATIVE_RACE_INTENTS_H
#include "yaw.h"
#include "gears.h"

/* Offline callback tags: none,193c4,193ec,19414,19438,1947c,194c8,19504,19534.
 * No guest function pointer is retained. Fields are the source integer words,
 * not geometric distances or floating-point approximations. */
typedef struct { int32_t threshold; uint32_t signal, predicate; } nm_race_announcement_row_t;
typedef struct {
    uint32_t flags, gate, inhibit, progress, next_progress, next_next_progress;
    int32_t speed, rpm;
    int8_t condition, passing, rank;
    uint8_t cars;
} nm_race_announcement_input_t;
typedef struct { uint32_t cursor, repeats, pending; } nm_race_announcement_state_t;

/*19320, AFTER both consumers. pending is the FULL word at501a02 and therefore
 *includes501a04 in its high half. No state is published on invalid input. */
static inline int nm_race_announcement_step(const nm_race_announcement_row_t *rows,unsigned count,
        const uint32_t rank_signals[9],const nm_race_announcement_input_t *in,
        nm_race_announcement_state_t *state) {
    if(!rows || !count || count>65536 || !rank_signals || !in || !state) return 0;
    if(!(in->flags&UINT32_C(0x00500000)) || (in->gate&1)) {
        state->cursor=state->repeats=0;return 1;
    }
    if(state->cursor>=count) return 0;
    const nm_race_announcement_row_t *row=rows+state->cursor;
    if(row->threshold>nm_yaw_angle(in->progress)) return 1;
    int emit;uint32_t signal=row->signal;
    switch(row->predicate) {
        case 0: emit=1;break;
        case 1: emit=in->speed>=260;break;
        case 2: emit=in->speed<=100;break;
        case 3: emit=in->condition!=0;break;
        case 4: emit=!in->inhibit && nm_gears_signed(in->next_next_progress-in->progress)<=2;break;
        case 5: emit=!in->inhibit && in->passing && nm_gears_signed(in->next_progress-in->progress)<=1;break;
        case 6: emit=in->speed>=180 && in->rpm>=7800;break;
        case 7: emit=in->rank==(int)in->cars-1;break;
        case 8:
            emit=!in->inhibit && in->rank<=8;
            if(emit) {
                if(in->rank<0) return 0; /* Source would read before its nine-row table. */
                signal=rank_signals[(unsigned)in->rank];
            }
            break;
        default: return 0;
    }
    ++state->cursor; /* Advances even when the selected predicate declines. */
    if(emit) state->pending=signal;
    return 1;
}

/* Offline rows from235ec0. Commands are numeric intents, never device writes. */
typedef struct {
    uint32_t command[2];
    int32_t duration;
    int16_t trigger;
    uint16_t reserved;
} nm_race_intent_row_t;
/* 191c8/191d0 select the two special commands (row 30's repeats 9 and 18/27) by 53e59a & 1, the board's
 * unit setting; this board has it set (hud.h: the wrong-way message and the speed in km/h use the same
 * bit), so callers pass nm_race_intent_special[NM_RACE_INTENT_UNIT]: ae216c, ae216d. */
#define NM_RACE_INTENT_UNIT 1
typedef struct { int16_t pending, timer, active; } nm_race_intent_lane_t;
typedef struct {
    nm_race_intent_lane_t lane[2];
    uint32_t repeats;
} nm_race_intents_t;
typedef struct { uint32_t command[4]; unsigned count; } nm_race_intent_output_t;

/*1904c then191d8. This is signal consumption, NOT19320 announcement creation
 *or19590 positional audio. Caller owns the shared pending words and must
 *round-trip them even in silent mode. special[0/1] are the two cabinet-selected
 *commands at191c8/191d0. No allocations, guest pointers or audio runtime.
 *Integer-width bookkeeping has no SH4ZAM floating-point substitute. */
static inline int nm_race_intents_step(const nm_race_intent_row_t *rows,unsigned count,
        unsigned alternate,const uint32_t special[2],int32_t warmup,int8_t lap,
        nm_race_intents_t *state,nm_race_intent_output_t *output) {
    if(!rows || !count || count>32768 || alternate>1 || !special || !state || !output) return 0;
    nm_race_intents_t next=*state;
    nm_race_intent_output_t out={{0},0};
    for(unsigned i=0;i<2;++i) {
        nm_race_intent_lane_t *s=next.lane+i;
        if(s->timer) {
            /*Compare the wide decrement, NOT its wrapped16-bit store. */
            const int32_t remaining=(int32_t)s->timer-1;
            s->timer=nm_yaw_angle((uint32_t)remaining);
            if(s->active) {
                if(s->active<0 || (unsigned)s->active>=count) return 0;
                const nm_race_intent_row_t *row=rows+s->active;
                if(row->trigger==remaining) {
                    unsigned copies=1;
                    uint32_t command=row->command[alternate];
                    if(i==0 && s->active==30) {
                        ++next.repeats;
                        if(next.repeats==9) command=special[0];
                        else if(next.repeats==18 || next.repeats==27) {
                            command=special[1];copies=next.repeats==27 ? 3 : 1;
                        } else if(next.repeats>27 && next.repeats<UINT32_C(0x80000000)) copies=0;
                    }
                    for(unsigned j=0;j<copies;++j) out.command[out.count++]=command;
                    s->active=0;
                }
            }
        } else if((i || (warmup>=256 && lap>=0)) && s->pending) {
            if(s->pending<0 || (unsigned)s->pending>=count) return 0;
            s->active=s->pending;
            const int32_t duration=rows[s->active].duration;
            /*192b8 returns with pending intact for the FIRST lane only. */
            if(i==0 && duration<0) continue;
            if(duration) s->timer=nm_yaw_angle((uint32_t)duration);
        }
        s->pending=0;
    }
    *state=next;*output=out;return 1;
}
#endif
