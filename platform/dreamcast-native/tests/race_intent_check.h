#ifndef DAYTONA_NATIVE_RACE_INTENT_CHECK_H
#define DAYTONA_NATIVE_RACE_INTENT_CHECK_H
#include "../include/race_field.h"
#define NM_RACE_ANNOUNCEMENT_HASH UINT32_C(0xa9ee4972)

static inline uint32_t nm_race_announcement_digest(uint32_t hash,const nm_race_announcement_state_t *s) {
    const uint32_t words[]={s->cursor,s->repeats,s->pending};
    for(unsigned i=0;i<3;++i) for(unsigned b=0;b<4;++b)
        hash=(hash^((words[i]>>(8*b))&255))*16777619u;
    return hash;
}

/* Match the independently source-executed domain; table data are exported offline. */
static inline uint32_t nm_race_announcement_target_hash(const nm_race_announcement_row_t rows[87],
        const uint16_t offsets[4],const uint16_t counts[4],const uint32_t ranks[9]) {
    uint32_t hash=2166136261u;
    for(unsigned course=0;course<3;++course) {
        if(!counts[course] || (unsigned)offsets[course]+counts[course]>87) return 0;
        const nm_race_announcement_row_t *table=rows+offsets[course];
        for(unsigned cursor=0;cursor<counts[course];++cursor) for(unsigned i=0;i<512;++i) {
            const uint32_t threshold=(uint32_t)table[cursor].threshold;
            const uint32_t progress[]={threshold-1,threshold,threshold+1,0x80000000u,
                0x7fffffffu,0xffff8000u,0x7fff0000u,0};
            const uint32_t delta[]={0,1,2,3,0x7fffffff,0x80000000u,0xffffffff};
            const int32_t speeds[]={100,101,179,180,259,260,INT32_MIN,INT32_MAX};
            nm_race_announcement_input_t in;memset(&in,0,sizeof(in));
            in.flags=(i%4)<<20;if(i%4==2) in.flags=0x400000;
            in.gate=(i/4)%4;in.inhibit=(i/16)%2;in.progress=progress[(i/2)%8];
            in.next_progress=in.progress+delta[(i/3)%7];in.next_next_progress=in.progress+delta[(i/5)%7];
            in.speed=speeds[(i/7)%8];in.rpm=i&64 ? 7800 : 7799;
            in.condition=i&128 ? -128 : 0;in.passing=i&256 ? -1 : 0;
            in.rank=(int8_t)((i/32)%10);in.cars=(uint8_t)(in.rank+(i&16 ? 1 : 2));
            nm_race_announcement_state_t state={cursor,0x87654321,0x12345678};
            nm_race_car_t player;memset(&player,0xa5,sizeof(player));
            player.car.boundary.pivot_signal=nm_yaw_angle(state.pending);
            player.car.boundary.damage.signal=nm_yaw_angle(state.pending>>16);
            nm_race_car_t next,next_next;
            memset(&next,0,sizeof(next));memset(&next_next,0,sizeof(next_next));
            player.car.rank_key=nm_yaw_angle(in.progress);player.car.rank_key_high=(uint16_t)(in.progress>>16);
            next.car.rank_key=nm_yaw_angle(in.next_progress);next.car.rank_key_high=(uint16_t)(in.next_progress>>16);
            next_next.car.rank_key=nm_yaw_angle(in.next_next_progress);next_next.car.rank_key_high=(uint16_t)(in.next_next_progress>>16);
            player.car.speed_index=in.speed;player.car.rpm=in.rpm;
            player.car.force.trigger_count[1]=in.condition;player.car.drag_state=in.passing;player.rank=(uint8_t)in.rank;
            const nm_race_announcement_input_t view=nm_race_announcement_view(&player,&next,&next_next,
                in.flags,in.gate,in.inhibit,in.cars);
            nm_race_intents_t intents;memset(&intents,0x5a,sizeof(intents));
            intents.repeats=state.repeats;
            if(!nm_race_create_intent(&player,&intents,&state.cursor,table,counts[course],ranks,&view)) return 0;
            state.repeats=intents.repeats;
            state.pending=(uint32_t)(uint16_t)player.car.boundary.pivot_signal |
                (uint32_t)(uint16_t)player.car.boundary.damage.signal<<16;
            hash=nm_race_announcement_digest(hash,&state);
        }
    }
    return hash;
}

static inline uint32_t nm_race_intent_digest(uint32_t hash,const nm_race_intents_t *s,
        const nm_race_intent_output_t *out,const nm_race_car_t *player) {
    const uint32_t words[]={(uint16_t)s->lane[0].pending,(uint16_t)s->lane[0].timer,
        (uint16_t)s->lane[0].active,(uint16_t)s->lane[1].pending,(uint16_t)s->lane[1].timer,
        (uint16_t)s->lane[1].active,s->repeats,(uint16_t)player->car.boundary.pivot_signal,
        (uint16_t)player->car.boundary.damage.signal,out->count};
    for(unsigned i=0;i<sizeof(words)/sizeof(*words);++i) for(unsigned b=0;b<4;++b)
        hash=(hash^((words[i]>>(8*b))&255))*16777619u;
    for(unsigned i=0;i<out->count;++i) for(unsigned b=0;b<4;++b)
        hash=(hash^((out->command[i]>>(8*b))&255))*16777619u;
    return hash;
}

/* Same deterministic fixture domain as the independently executed i960
 * consumer pair in check_contact.hpp. No ROM reads on the target. */
static inline uint32_t nm_race_intent_target_hash(const nm_race_intent_row_t rows[57],
        const uint8_t alternate[4],const uint32_t special[2][2],
        const nm_race_announcement_row_t *announcements) {
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<65536;++i) {
        nm_race_intents_t state;memset(&state,0,sizeof(state));
        const uint32_t repeats[]={0,8,17,26,27,0x7fffffff,0xffffffff};
        state.repeats=repeats[(i/7)%7];
        for(unsigned lane=0;lane<2;++lane) {
            nm_race_intent_lane_t *s=state.lane+lane;
            s->pending=(int16_t)((i/8+lane)%57);s->active=(int16_t)((i/64+lane)%57);
            if(i&1) s->active=30;
            const int32_t times[]={0,1,-1,-32768,32767,(int32_t)rows[s->active].trigger+1};
            s->timer=nm_yaw_angle((uint32_t)times[(i/2+lane)%6]);
        }
        nm_race_car_t player;memset(&player,0,sizeof(player));
        player.car.boundary.pivot_signal=state.lane[0].pending;
        player.car.boundary.damage.signal=state.lane[1].pending;
        player.car.lap.completed=i&8 ? 0 : 255;
        player.car.lap.lap=(uint8_t)~player.car.lap.completed;
        state.lane[0].pending=state.lane[1].pending=-1;
        nm_race_intent_output_t out;
        uint32_t cursor=2;
        if(announcements) {
            const uint32_t ranks[9]={0};
            nm_race_announcement_input_t input;memset(&input,0,sizeof(input));
            input.flags=i&1 ? 0x100000 : 0;input.gate=(i>>1)&1;input.inhibit=1;
            input.progress=(uint32_t)announcements[2].threshold-(i&4 ? 1u : 0u);
            if(!nm_race_event_tail(&player,&state,&cursor,rows,57,alternate[(i/16)%4],special[i&1],
                i&4 ? 256 : 255,announcements,3,ranks,&input,&out)) return 0;
        } else if(!nm_race_consume_intents(&player,&state,rows,57,alternate[(i/16)%4],
                special[i&1],i&4 ? 256 : 255,&out)) return 0;
        hash=nm_race_intent_digest(hash,&state,&out,&player);
        if(announcements) {
            const nm_race_announcement_state_t tail={cursor,state.repeats,
                (uint32_t)(uint16_t)player.car.boundary.pivot_signal |
                (uint32_t)(uint16_t)player.car.boundary.damage.signal<<16};
            hash=nm_race_announcement_digest(hash,&tail);
        }
    }
    return hash;
}
#endif
