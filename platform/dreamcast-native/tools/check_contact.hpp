// Host-only command36 oracle. No board/runtime code enters the native target.
#pragma once
#include "../tests/contact_check.h"
#include "../tests/boundary_check.h"
#include "../include/wheels.h"
#include "../include/intro_math.h"
#include "../include/race_intents.h"
#include "../include/race_field.h"
#include "../include/render/ui/name_pose.h"
#include "../tests/race_intent_check.h"
#include "check_road.hpp"
#include "runtime/m2_board.h"
#include "refcore/i960_ref.h"
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <limits>
#include <stdexcept>

namespace daytona::native {
inline void check_race_announcements(rt::M2Board &board) {
    VehicleOracleBus bus(board);
    const uint32_t callbacks[]={0,0x193c4,0x193ec,0x19414,0x19438,0x1947c,0x194c8,0x19504,0x19534};
    uint32_t rank_signals[9];
    for(unsigned i=0;i<9;++i) rank_signals[i]=board.read_dword(0x1956c+4*i);
    unsigned cases=0,advanced=0,changed=0;
    uint32_t hash=2166136261u;
    for(unsigned course=0;course<3;++course) {
        const uint32_t base=board.read_dword(0x236260+4*course);
        std::vector<nm_race_announcement_row_t> rows;
        for(unsigned i=0;i<128;++i) {
            const uint32_t callback=board.read_dword(base+16*i+8);
            const auto tag=std::find(std::begin(callbacks),std::end(callbacks),callback);
            if(tag==std::end(callbacks)) throw std::runtime_error("unknown announcement predicate");
            rows.push_back({std::bit_cast<int32_t>(board.read_dword(base+16*i)),
                board.read_dword(base+16*i+4),unsigned(tag-std::begin(callbacks))});
            if(rows.back().threshold==100000) break;
        }
        if(rows.empty() || rows.back().threshold!=100000) throw std::runtime_error("missing announcement sentinel");
        for(unsigned cursor=0;cursor<rows.size();++cursor) for(unsigned i=0;i<512;++i) {
            bus.entity.fill(0xa5);bus.globals.fill(0x5a);bus.checkpoint_memory.fill(0x69);
            for(auto &n:bus.neighbors) n.fill(0xc3);
            const uint32_t progress[]={uint32_t(rows[cursor].threshold-1),uint32_t(rows[cursor].threshold),
                uint32_t(rows[cursor].threshold+1),0x80000000u,0x7fffffffu,0xffff8000u,0x7fff0000u,0};
            const uint32_t delta[]={0,1,2,3,0x7fffffff,0x80000000u,0xffffffff};
            const int32_t speeds[]={100,101,179,180,259,260,INT32_MIN,INT32_MAX};
            nm_race_announcement_input_t in={};
            in.flags=(i%4)<<20; if(i%4==2) in.flags=0x400000;
            in.gate=(i/4)%4;in.inhibit=(i/16)%2;in.progress=progress[(i/2)%8];
            in.next_progress=in.progress+delta[(i/3)%7];in.next_next_progress=in.progress+delta[(i/5)%7];
            in.speed=speeds[(i/7)%8];in.rpm=i&64 ? 7800 : 7799;
            in.condition=i&128 ? -128 : 0;in.passing=i&256 ? -1 : 0;
            in.rank=int8_t((i/32)%10);in.cars=uint8_t(in.rank+(i&16 ? 1 : 2));
            nm_race_announcement_state_t state={cursor,0x87654321,0x12345678};
            auto put=[](auto &memory,unsigned offset,auto value) { memcpy(memory.data()+offset,&value,sizeof(value)); };
            put(bus.globals,0x90,in.flags);put(bus.globals,0x80,in.cars);
            put(bus.entity,0x18,in.speed);put(bus.entity,0x60,in.progress);put(bus.entity,0x150,in.rpm);
            put(bus.entity,0x96,in.condition);put(bus.entity,0x15e,in.passing);put(bus.entity,0x6a,in.rank);
            put(bus.entity,0x88,uint32_t(0x620000));put(bus.neighbors[0],0x88,uint32_t(0x621000));
            put(bus.neighbors[0],0x60,in.next_progress);put(bus.neighbors[1],0x60,in.next_next_progress);
            put(bus.checkpoint_memory,0x60,uint8_t(course));put(bus.checkpoint_memory,0x454,in.gate);
            put(bus.checkpoint_memory,0x680,in.inhibit);put(bus.checkpoint_memory,0x622,state.cursor);
            put(bus.checkpoint_memory,0x504,state.repeats);put(bus.checkpoint_memory,0x602,state.pending);
            auto expected=bus.checkpoint_memory;
            const auto entity=bus.entity;const auto globals=bus.globals;const auto neighbors=bus.neighbors;
            nm_race_car_t player;memset(&player,0xa5,sizeof(player));
            player.car.boundary.pivot_signal=nm_yaw_angle(state.pending);
            player.car.boundary.damage.signal=nm_yaw_angle(state.pending>>16);
            nm_race_car_t next={},next_next={};
            player.car.rank_key=nm_yaw_angle(in.progress);player.car.rank_key_high=uint16_t(in.progress>>16);
            next.car.rank_key=nm_yaw_angle(in.next_progress);next.car.rank_key_high=uint16_t(in.next_progress>>16);
            next_next.car.rank_key=nm_yaw_angle(in.next_next_progress);next_next.car.rank_key_high=uint16_t(in.next_next_progress>>16);
            player.car.speed_index=in.speed;player.car.rpm=in.rpm;
            player.car.force.trigger_count[1]=in.condition;player.car.drag_state=in.passing;player.rank=uint8_t(in.rank);
            const auto view=nm_race_announcement_view(&player,&next,&next_next,in.flags,in.gate,in.inhibit,in.cars);
            nm_race_intents_t intents;memset(&intents,0x5a,sizeof(intents));
            intents.repeats=state.repeats;
            auto expected_player=player;auto expected_intents=intents;
            if(!nm_race_create_intent(&player,&intents,&state.cursor,rows.data(),unsigned(rows.size()),rank_signals,&view))
                throw std::runtime_error("native announcement rejected fixture");
            state.repeats=intents.repeats;
            state.pending=uint32_t(uint16_t(player.car.boundary.pivot_signal)) |
                uint32_t(uint16_t(player.car.boundary.damage.signal))<<16;
            expected_player.car.boundary.pivot_signal=player.car.boundary.pivot_signal;
            expected_player.car.boundary.damage.signal=player.car.boundary.damage.signal;
            expected_intents.repeats=intents.repeats;
            if(memcmp(&player,&expected_player,sizeof(player)) || memcmp(&intents,&expected_intents,sizeof(intents)))
                throw std::runtime_error("announcement changed unrelated native ownership");
            put(expected,0x622,state.cursor);put(expected,0x504,state.repeats);put(expected,0x602,state.pending);
            bus.run(0x19320,0x19568);
            if(bus.checkpoint_memory!=expected || bus.entity!=entity || bus.globals!=globals || bus.neighbors!=neighbors)
                throw std::runtime_error("announcement memory differs course="+std::to_string(course)+
                    " row="+std::to_string(cursor)+" case="+std::to_string(i));
            ++cases;advanced+=state.cursor==cursor+1;changed+=state.pending!=0x12345678;
            hash=nm_race_announcement_digest(hash,&state);
        }
    }
    if(hash!=NM_RACE_ANNOUNCEMENT_HASH) throw std::runtime_error("announcement source digest changed");
    std::printf("native_race_announcements PASS cases=%u advanced=%u changed=%u hash=%08x (whole scratch/shared memory)\n",cases,advanced,changed,hash);
}
inline void check_race_signals(rt::M2Board &board, const nm_drive_params_t &math) {
    check_race_announcements(board);
    VehicleOracleBus bus(board); uint32_t timer_hash = 2166136261u, pass_hash = 2166136261u;
    nm_race_intent_row_t rows[57];
    for(unsigned i=0;i<57;++i) {
        const uint32_t at=0x235ec0+16*i;
        rows[i]={{board.read_dword(at),board.read_dword(at+4)},
            std::bit_cast<int32_t>(board.read_dword(at+8)),std::bit_cast<int16_t>(board.read_word(at+12)),0};
    }
    unsigned intent_calls=0,retained=0,wrapped=0;
    uint32_t intent_hash=2166136261u,tail_hash=2166136261u;
    for(unsigned i=0;i<65536;++i) {
        bus.entity.fill(0xa5);bus.globals.fill(0x5a);bus.checkpoint_memory.fill(0x69);
        nm_race_intents_t state={};
        const uint32_t repeats[]={0,8,17,26,27,0x7fffffff,0xffffffff};
        state.repeats=repeats[(i/7)%7];
        for(unsigned lane=0;lane<2;++lane) {
            auto &s=state.lane[lane];
            s.pending=int16_t((i/8+lane)%57);s.active=int16_t((i/64+lane)%57);
            if(i&1) s.active=30;
            const int32_t times[]={0,1,-1,-32768,32767,(int32_t)rows[s.active].trigger+1};
            s.timer=nm_yaw_angle(uint32_t(times[(i/2+lane)%6]));
        }
        const int32_t warmup=(i&4) ? 256 : 255;
        const int8_t lap=(i&8) ? 0 : -1;
        bus.entity[0x6d]=uint8_t(lap);memcpy(bus.globals.data()+0xd0,&warmup,4);
        bus.checkpoint_memory[0x60]=uint8_t((i/16)%4);bus.damage_variant=uint8_t(i&1);
        const unsigned alternate=board.read_dword(0x236250+4*((i/16)%4))!=0;
        const uint32_t special[]={board.read_dword(0x191c8+4*(i&1)),board.read_dword(0x191d0+4*(i&1))};
        auto store=[&](auto &memory,const nm_race_intents_t &s) {
            for(unsigned lane=0;lane<2;++lane) {
                memcpy(memory.data()+0x602+2*lane,&s.lane[lane].pending,2);
                memcpy(memory.data()+0x612+2*lane,&s.lane[lane].timer,2);
                memcpy(memory.data()+0x61a+2*lane,&s.lane[lane].active,2);
            }
            memcpy(memory.data()+0x504,&s.repeats,4);
        };
        store(bus.checkpoint_memory,state);
        const auto old_entity=bus.entity;
        const auto old_globals=bus.globals;
        auto expected=bus.checkpoint_memory;
        nm_race_intent_output_t out;
        nm_race_car_t player={};
        player.car.boundary.pivot_signal=state.lane[0].pending;
        player.car.boundary.damage.signal=state.lane[1].pending;
        player.car.lap.completed=uint8_t(lap);
        player.car.lap.lap=uint8_t(~uint8_t(lap)); // Distinguish6d from6e.
        auto expected_player=player;
        state.lane[0].pending=state.lane[1].pending=-1; // Poison stale borrowed views.
        auto tail_player=player;auto tail_state=state;
        if(!nm_race_consume_intents(&player,&state,rows,57,alternate,special,warmup,&out))
            throw std::runtime_error("native race intent rejected fixture");
        expected_player.car.boundary.pivot_signal=state.lane[0].pending;
        expected_player.car.boundary.damage.signal=state.lane[1].pending;
        if(memcmp(&player,&expected_player,sizeof(player)))
            throw std::runtime_error("race intent changed unrelated player fields");
        store(expected,state);
        bus.run(0x1904c,0x1931c);auto cues=bus.award_cues;
        bus.run(0x191d8,0x1931c);cues.insert(cues.end(),bus.award_cues.begin(),bus.award_cues.end());
        if(bus.checkpoint_memory!=expected || bus.entity!=old_entity || bus.globals!=old_globals ||
           cues.size()!=out.count || !std::equal(cues.begin(),cues.end(),out.command)) {
            std::fprintf(stderr,"race intent mismatch case=%u source-cues=%zu native-cues=%u\n",i,cues.size(),out.count);
            throw std::runtime_error("native intent memory/calls differ from1904c/191d8");
        }
        intent_calls+=out.count;retained+=state.lane[0].pending!=0;wrapped+=state.lane[0].timer==32767;
        intent_hash=nm_race_intent_digest(intent_hash,&state,&out,&player);
        // Source19038 order: compare the combined native tail after the same consumers.
        const uint32_t base=board.read_dword(0x236260);
        if(board.read_dword(base+40)!=0) throw std::runtime_error("tail fixture callback changed");
        nm_race_announcement_row_t announcements[3]={};
        announcements[2]={std::bit_cast<int32_t>(board.read_dword(base+32)),board.read_dword(base+36),0};
        uint32_t ranks[9]={},cursor=2;
        nm_race_announcement_input_t input={};
        input.flags=i&1 ? 0x100000 : 0;input.gate=(i>>1)&1;input.inhibit=1;
        input.progress=uint32_t(announcements[2].threshold-(i&4 ? 1 : 0));
        nm_race_intent_output_t tail_out;
        if(!nm_race_event_tail(&tail_player,&tail_state,&cursor,rows,57,alternate,special,warmup,
                announcements,3,ranks,&input,&tail_out)) throw std::runtime_error("native combined tail rejected");
        memcpy(bus.globals.data()+0x90,&input.flags,4);
        memcpy(bus.entity.data()+0x60,&input.progress,4);
        memcpy(bus.checkpoint_memory.data()+0x454,&input.gate,4);
        bus.checkpoint_memory[0x60]=0;
        const uint32_t initial_cursor=2;
        memcpy(bus.checkpoint_memory.data()+0x622,&initial_cursor,4);
        auto tail_expected=bus.checkpoint_memory;
        auto tail_expected_state=tail_state;
        tail_expected_state.lane[0].pending=tail_player.car.boundary.pivot_signal;
        tail_expected_state.lane[1].pending=tail_player.car.boundary.damage.signal;
        store(tail_expected,tail_expected_state);
        memcpy(tail_expected.data()+0x622,&cursor,4);
        const auto tail_entity=bus.entity;const auto tail_globals=bus.globals;
        bus.run(0x19320,0x19568);
        if(bus.checkpoint_memory!=tail_expected || bus.entity!=tail_entity || bus.globals!=tail_globals ||
           tail_out.count!=out.count || !std::equal(out.command,out.command+out.count,tail_out.command))
            throw std::runtime_error("combined tail differs from sequential i960 consumers/creator");
        tail_hash=nm_race_intent_digest(tail_hash,&tail_state,&tail_out,&tail_player);
        const nm_race_announcement_state_t tail={cursor,tail_state.repeats,
            uint32_t(uint16_t(tail_player.car.boundary.pivot_signal)) |
            uint32_t(uint16_t(tail_player.car.boundary.damage.signal))<<16};
        tail_hash=nm_race_announcement_digest(tail_hash,&tail);
    }
    std::puts("native_race_event_tail PASS cases=65536 (consume then create, gates, full shared memory and command order)");
    std::printf("native_race_intents PASS cases=65536 calls=%u retained=%u wrapped=%u\n",intent_calls,retained,wrapped);
    uint8_t alternate[4];uint32_t special[2][2];
    for(unsigned i=0;i<4;++i) alternate[i]=board.read_dword(0x236250+4*i)!=0;
    for(unsigned v=0;v<2;++v) for(unsigned event=0;event<2;++event)
        special[v][event]=board.read_dword(0x191c8+8*event+4*v);
    if(nm_race_intent_target_hash(rows,alternate,special,NULL)!=intent_hash)
        throw std::runtime_error("target intent fixture differs from source-checked sweep");
    nm_race_announcement_row_t tail_rows[3]={};
    const uint32_t tail_base=board.read_dword(0x236260);
    tail_rows[2]={std::bit_cast<int32_t>(board.read_dword(tail_base+32)),board.read_dword(tail_base+36),0};
    if(nm_race_intent_target_hash(rows,alternate,special,tail_rows)!=tail_hash)
        throw std::runtime_error("target tail fixture differs from source-checked sweep");
    std::printf("native_race_event_tail_target hash=%08x PASS\n",tail_hash);
    std::printf("native_race_intent_target hash=%08x PASS\n",intent_hash);
    bus.entity.fill(0xa5); bus.globals.fill(0x5a); bus.auxiliary.fill(0x3c);
    const auto entity = bus.entity;
    const auto globals = bus.globals;
    const auto auxiliary = bus.auxiliary;
    for (unsigned i = 0; i < NM_RACE_TIMERS_CASES; ++i) {
        nm_race_timers_t s; nm_race_timers_test_input(i,&s);
        bus.race_short_word = 0xa55a0000u | uint16_t(s.short_ticks);
        bus.damage_control = uint32_t(s.control); for (unsigned j = 0; j < 2; ++j) bus.race_events[j] = uint32_t(s.event[j]);
        if (!nm_race_timers_step(&s)) throw std::runtime_error("native race timers rejected fixture");
        bus.run(0x12150,0x121c0);
        if (bus.race_short_word != (0xa55a0000u|uint16_t(s.short_ticks)) || bus.damage_control != uint32_t(s.control) ||
                bus.race_events[0] != uint32_t(s.event[0]) || bus.race_events[1] != uint32_t(s.event[1]) ||
                bus.entity != entity || bus.globals != globals || bus.auxiliary != auxiliary)
            throw std::runtime_error("race timer memory differs from original12150");
        for (unsigned j = 0; j < 14; ++j) timer_hash = (timer_hash^reinterpret_cast<const uint8_t *>(&s)[j])*16777619u;
    }
    unsigned signals[17]{};
    for (unsigned i = 0; i < NM_TRAFFIC_PASS_CASES; ++i) {
        nm_traffic_pass_body_t body[3]; nm_traffic_pass_t s; nm_traffic_pass_test_input(i,body,&s);
        bus.collision_pool.resize(3); bus.traffic_player = 0x640000;
        const auto pointer = [](unsigned index) { return index == UINT16_MAX ? 0x600000u : 0x640000u+0x300u*index; };
        bus.traffic_previous = {pointer(s.next),pointer(s.previous)}; bus.boundary_spin_signal = s.signal;
        for (unsigned j = 0; j < 3; ++j) {
            auto &memory = bus.collision_pool[j]; memory.fill(0x69); traffic_pass_fields(memory,body[j],true);
            const uint32_t next = pointer(body[j].next), previous = pointer(body[j].previous);
            memcpy(memory.data()+0x88,&next,4); memcpy(memory.data()+0x8c,&previous,4);
        }
        const auto pool = bus.collision_pool;
        if (!nm_traffic_pass(&math,body,3,0,&s)) throw std::runtime_error("native traffic pass rejected fixture");
        bus.run(0x125b0,0x127fc);
        if (bus.traffic_previous[0] != pointer(s.next) || bus.traffic_previous[1] != pointer(s.previous) ||
                bus.boundary_spin_signal != s.signal || bus.collision_pool != pool || bus.entity != entity ||
                bus.globals != globals || bus.auxiliary != auxiliary) {
            std::fprintf(stderr,"traffic pass case=%u native=%u source=%u\n",i,s.signal,bus.boundary_spin_signal);
            throw std::runtime_error("traffic pass memory differs from original125b0");
        }
        if (s.signal < 17) ++signals[s.signal];
        for (unsigned j = 0; j < 6; ++j) pass_hash = (pass_hash^reinterpret_cast<const uint8_t *>(&s)[j])*16777619u;
    }
    if (timer_hash != NM_RACE_TIMERS_HASH || pass_hash != NM_TRAFFIC_PASS_HASH ||
            timer_hash != nm_race_timers_test_hash() || pass_hash != nm_traffic_pass_test_hash(&math))
        throw std::runtime_error("race signal shared digest mismatch");
    std::printf("native_race_signals PASS timers=%u hash=%08x pass=%u hash=%08x signals3/4/7/8/9/10/11/12/13/14/15/16=%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u\n",
        NM_RACE_TIMERS_CASES,timer_hash,NM_TRAFFIC_PASS_CASES,pass_hash,signals[3],signals[4],signals[7],signals[8],signals[9],signals[10],signals[11],signals[12],signals[13],signals[14],signals[15],signals[16]);
}

inline void check_traffic_near(rt::M2Board &board, const nm_drive_params_t &math) {
    VehicleOracleBus bus(board); uint32_t hash = 2166136261u;
    unsigned holds = 0, outside = 0, distinct_subject = 0;
    if (board.read_dword(0x18) || board.read_dword(0x88)) throw std::runtime_error("unexpected immutable traffic inputs");
    for (unsigned i = 0; i < NM_TRAFFIC_NEAR_CASES+256; ++i) {
        const bool worker = i >= NM_TRAFFIC_NEAR_CASES;
        nm_traffic_near_body_t body[44]; unsigned count, reset, player, subject;
        nm_traffic_near_test_input(worker ? i*8+5 : i,body,&count,&reset,&player,&subject);
        if (worker) {
            subject = count++;
            body[subject] = {};
            body[subject].next = body[subject].previous = UINT16_MAX;
            const uint32_t progress[] = {0,127,128,129,0x7fffffff,0x80000000,0xffffffff};
            body[subject].progress = progress[i%7]; body[subject].hold = uint8_t(i);
        }
        bus.entity.fill(0x33); bus.globals.fill(0xa5); bus.auxiliary.fill(0x5a); bus.guided_profile_updates = 99;
        memcpy(bus.globals.data()+0x84,&reset,4); bus.collision_pool.resize(count);
        bus.traffic_player = 0x640000+0x300*player; bus.traffic_subject = 0x640000+0x300*subject;
        for (unsigned j = 0; j < count; ++j) {
            auto &memory = bus.collision_pool[j]; memory.fill(0x3c); traffic_near_fields(memory,body[j],true);
            const uint32_t next = worker && j == subject ? 0 : 0x640000+0x300*body[j].next;
            const uint32_t previous = worker && j == subject ? 0 : 0x640000+0x300*body[j].previous, stride = 0x300;
            memcpy(memory.data()+0x88,&next,4); memcpy(memory.data()+0x8c,&previous,4); memcpy(memory.data()+8,&stride,4);
        }
        auto expected = bus.collision_pool;
        const auto record = bus.entity; const auto globals = bus.globals; const auto auxiliary = bus.auxiliary;
        const auto old_hold = body[subject].hold; uint8_t updates = 99;
        uint32_t ahead[2] = {0,0};
        if (!worker) { const unsigned a = body[subject].next, b = body[a].next; ahead[0] = body[a].progress; ahead[1] = body[b].progress; }
        if (!nm_traffic_near(&math,body,count,reset,player,subject,ahead,&updates)) throw std::runtime_error("traffic proximity fixture rejected");
        for (unsigned j = 0; j < count; ++j) traffic_near_fields(expected[j],body[j],true);
        bus.run(0x12464,0x12588);
        if (expected != bus.collision_pool || record != bus.entity || globals != bus.globals || auxiliary != bus.auxiliary || bus.guided_profile_updates != updates) {
            std::fprintf(stderr,"traffic near case=%u player=%u subject=%u pool=%d\n",i,player,subject,expected==bus.collision_pool);
            for (unsigned j = 0; j < count; ++j) for (unsigned k = 0; k < 0x300; ++k) if (expected[j][k] != bus.collision_pool[j][k]) {
                std::fprintf(stderr,"body=%u offset=%x native=%02x source=%02x\n",j,k,expected[j][k],bus.collision_pool[j][k]); break;
            }
            throw std::runtime_error("native traffic proximity differs from original i960/TGP whole memory");
        }
        holds += body[subject].hold != old_hold; outside += player >= reset; distinct_subject += subject != player;
        if (!worker) for (unsigned j = 0; j < count; ++j) for (unsigned k = 0; k <= offsetof(nm_traffic_near_body_t,hold); ++k)
            hash = (hash^reinterpret_cast<const uint8_t *>(&body[j])[k])*16777619u;
    }
    if (hash != NM_TRAFFIC_NEAR_HASH || hash != nm_traffic_near_test_hash(&math)) throw std::runtime_error("traffic proximity shared hash mismatch");
    std::printf("native_traffic_near PASS cases=%u+256worker changed_hold=%u outside_player=%u distinct_subject=%u hash=%08x body=%zu (whole12464/139d8 memory, not autonomous race)\n",
        NM_TRAFFIC_NEAR_CASES,holds,outside,distinct_subject,hash,sizeof(nm_traffic_near_body_t));
}

inline void check_traffic_order(rt::M2Board &board) {
    VehicleOracleBus bus(board); uint32_t hash = 2166136261u;
    for (unsigned count = 1; count <= 44; ++count) {
        bus.entity.fill(0xa5); bus.globals.fill(0x5a);
        const uint32_t stride = 0x300;
        memcpy(bus.entity.data()+8,&stride,4);
        memcpy(bus.globals.data()+0x80,&count,4);
        bus.traffic_ranks.fill(0xcccccccc); bus.traffic_track.fill(0xdddddddd);
        bus.traffic_scratch.fill(0xeeeeeeee); bus.traffic_lap = -7;
        const auto entity = bus.entity; const auto globals = bus.globals;
        const auto scratch = bus.traffic_scratch;
        uint16_t ranks[44], track[44];
        std::fill_n(ranks,44,91); std::fill_n(track,44,92);
        if (!nm_traffic_order_init(ranks,track,count)) throw std::runtime_error("traffic init rejected");
        bus.run(0x121d4,0x1221c);
        if (entity != bus.entity || globals != bus.globals || scratch != bus.traffic_scratch || bus.traffic_lap != -7)
            throw std::runtime_error("traffic init changed unrelated state");
        for (unsigned j = 0; j < 44; ++j) {
            const uint32_t rank = j < count ? 0x500000+stride*ranks[j] : 0xcccccccc;
            const uint32_t track_ptr = j < count ? 0x500000+stride*track[j] : 0xdddddddd;
            if (bus.traffic_ranks[j] != rank || bus.traffic_track[j] != track_ptr)
                throw std::runtime_error("traffic init differs from original pointer lists");
        }
    }
    std::printf("native_traffic_init PASS counts=1..44 original121d4..1221c; tails/body/globals/lap preserved\n");
    unsigned rotations[4]{}, ranking = 0, profile_passes = 0;
    for (unsigned i = 0; i < NM_TRAFFIC_ORDER_CASES; ++i) {
        nm_traffic_order_body_t body[44]; uint16_t ranks[44], track[44];
        unsigned count, sort_count, ring_count; uint32_t flags; int8_t lap;
        nm_traffic_order_test_input(i,body,ranks,track,&count,&sort_count,&ring_count,&flags,&lap);
        bus.entity.fill(0xa5); bus.globals.fill(0x5a); bus.auxiliary.fill(0x3c);
        memcpy(bus.globals.data()+0x80,&sort_count,4); const unsigned reset = ring_count-1;
        memcpy(bus.globals.data()+0x84,&reset,4); memcpy(bus.globals.data()+0x90,&flags,4);
        bus.traffic_lap = lap; bus.traffic_scratch.fill(0xcccccccc); bus.collision_pool.resize(count);
        for (unsigned j = 0; j < count; ++j) {
            auto &memory = bus.collision_pool[j]; memory.fill(0x33); traffic_order_fields(memory,body[j],true);
            const uint32_t next = 0x640000+0x300*body[j].next, previous = 0x640000+0x300*body[j].previous;
            memcpy(memory.data()+0x88,&next,4); memcpy(memory.data()+0x8c,&previous,4);
        }
        for (unsigned j = 0; j < 44; ++j) {
            bus.traffic_ranks[j] = 0x640000+0x300*ranks[j]; bus.traffic_track[j] = 0x640000+0x300*track[j];
        }
        auto scratch = bus.traffic_scratch;
        // Only the reference uses this transient copy area. Validate its exact
        // writes as well, but do not retain a useless scratch array on target.
        if (body[track[0]].lap > lap) {
            unsigned n = 1;
            while (n < 4 && body[track[n]].lap == body[track[0]].lap && body[track[n]].profile < 8) ++n;
            for (unsigned j = 0; j < n; ++j) scratch[j] = bus.traffic_track[j];
            ++rotations[n-1];
        } else ++profile_passes;
        ranking += (flags&0x500000u)!=0;
        auto expected = bus.collision_pool;
        const auto record = bus.entity; const auto globals = bus.globals; const auto auxiliary = bus.auxiliary;
        if (!nm_traffic_order(body,count,ranks,track,sort_count,ring_count,flags,&lap)) throw std::runtime_error("native traffic fixture rejected");
        for (unsigned j = 0; j < count; ++j) {
            traffic_order_fields(expected[j],body[j],true);
            const uint32_t next = 0x640000+0x300*body[j].next, previous = 0x640000+0x300*body[j].previous;
            memcpy(expected[j].data()+0x88,&next,4); memcpy(expected[j].data()+0x8c,&previous,4);
        }
        bus.run(0x1225c,0x122d8); bus.run(0x122dc,0x12460);
        if (expected != bus.collision_pool || record != bus.entity || globals != bus.globals || auxiliary != bus.auxiliary ||
            scratch != bus.traffic_scratch || lap != bus.traffic_lap) {
            std::fprintf(stderr,"traffic order case=%u pool=%d scratch=%d lap=%d/%d\n",i,expected==bus.collision_pool,scratch==bus.traffic_scratch,lap,bus.traffic_lap);
            throw std::runtime_error("native traffic ordering differs from original whole memory");
        }
        for (unsigned j = 0; j < sizeof(body); ++j) hash = (hash^reinterpret_cast<const uint8_t *>(body)[j])*16777619u;
        for (unsigned j = 0; j < 44; ++j) {
            if (bus.traffic_ranks[j] != 0x640000+0x300*ranks[j] || bus.traffic_track[j] != 0x640000+0x300*track[j])
                throw std::runtime_error("traffic pointer list differs from native indices");
            hash = (hash^ranks[j])*16777619u; hash = (hash^track[j])*16777619u;
        }
        hash = (hash^uint8_t(lap))*16777619u;
    }
    if (hash != NM_TRAFFIC_ORDER_HASH || hash != nm_traffic_order_test_hash()) throw std::runtime_error("traffic ordering shared hash mismatch");
    for (unsigned n : rotations) if (!n) throw std::runtime_error("traffic rotation coverage missing");
    std::printf("native_traffic_order PASS cases=%u rank_pass=%u profile_pass=%u rotate1/2/3/4=%u/%u/%u/%u hash=%08x body=%zu (whole1225c/122dc, not12464 proximity flags)\n",
        NM_TRAFFIC_ORDER_CASES,ranking,profile_passes,rotations[0],rotations[1],rotations[2],rotations[3],hash,sizeof(nm_traffic_order_body_t));
}

// Execute the ORIGINAL pair loop and TGP gates, replacing only the response
// callee with explicit test mutations/charges. This isolates traversal, not
// whole-pass physics; full responses remain covered separately below.
inline void check_collision_walk(rt::M2Board &board, const nm_drive_params_t &math) {
    VehicleOracleBus bus(board); unsigned pairs = 0, candidates = 0, swaps = 0, exhausted = 0, empty = 0;
    uint32_t hash = 2166136261u;
    for (unsigned row = 0; row < 4; ++row) for (unsigned j = 0; j < 14; ++j)
        if (math.collision_schedule[row][j] != int(std::bit_cast<int8_t>(board.read_byte(0x13ec4+14*row+j)))+5)
            throw std::runtime_error("collision schedule differs from source");
    for (unsigned i = 0; i < 8192; ++i) {
        nm_collision_rank_t rank[64]; unsigned count, reset, primary; uint32_t wrap; uint16_t order[14];
        nm_collision_prepare_test_input(i,rank,&count,&reset,&primary,&wrap);
        if (!nm_collision_prepare(rank,count,reset,primary,wrap,order)) throw std::runtime_error("walk fixture preparation failed");
        nm_collision_walk_body_t body[64]{};
        bus.first_instruction = 0x1406c; bus.last_instruction = 0x1417c;
        bus.entity.fill(0x5a); bus.globals.fill(0xa5); bus.auxiliary.fill(0x3c);
        bus.collision_pool.resize(count); bus.collision_primary = 0x640000+0x300*primary; bus.collision_phase = i%4;
        for (unsigned j = 0; j < count; ++j) {
            body[j].next = rank[j].next; body[j].candidates = rank[j].candidates;
            // Include entirely empty, all-hit, distance/word misses and repeated links.
            if (i%7 == 0) body[j].candidates = 0;
            if (i%7 == 1) body[j].candidates = 13;
            body[j].probe.position[0] = float(int(j%7)-3)*(i&4 ? 2.0f : 0.0f);
            body[j].probe.position[1] = float(int(j/7)-3)*(i&8 ? 1.0f : 0.0f);
            body[j].probe.word5c = nm_force_bits(float((j+i)%5)*(i&16 ? 1.0f : 0.0f));
            auto &memory = bus.collision_pool[j]; memory.fill(0x11);
            const uint32_t next = 0x640000+0x300*body[j].next;
            memcpy(memory.data()+0x88,&next,4); memcpy(memory.data()+0x25a,&body[j].candidates,2);
            memcpy(memory.data()+0x1c,&body[j].probe.position[0],4); memcpy(memory.data()+0x24,&body[j].probe.position[1],4);
            memcpy(memory.data()+0x5c,&body[j].probe.word5c,4);
        }
        for (unsigned j = 0; j < 14; ++j) {
            const uint32_t at = 0x640000+0x300*order[j]; memcpy(bus.entity.data()+0x5c+4*j,&at,4);
        }
        auto expected = bus.collision_pool;
        const auto record = bus.entity; const auto globals = bus.globals; const auto auxiliary = bus.auxiliary;
        rt::I960Ref cpu(&bus); cpu.m_IP = 0x1406c;
        cpu.m_r[27] = 0x880000; cpu.m_r[28] = 0x4000; cpu.m_r[29] = 0x500000;
        const int budgets[] = {1000,5,10,15,160,310,315,1000};
        nm_collision_walk_t state{}; state.budget = budgets[(i/4)%8]; cpu.m_r[15] = uint32_t(state.budget);
        unsigned instructions = 0, cost = 0, hits = 0;
        while (cpu.m_IP != 0x1417c) {
            if (++instructions > 10000) throw std::runtime_error("collision traversal oracle instruction guard");
            if (cpu.m_IP == 0x140fc) ++candidates;
            if (cpu.m_IP == 0x1414c || cpu.m_IP == 0x14160) {
                uint16_t pair[2]{};
                const int found = nm_collision_walk_next(&math,body,count,order,primary,i%4,cost,&state,pair);
                if (found != 1 || cpu.m_r[24] != 0x640000+0x300*pair[0] || cpu.m_r[25] != 0x640000+0x300*pair[1] ||
                    cpu.m_r[15] != uint32_t(state.budget)) {
                    std::fprintf(stderr,"collision walk case=%u hit=%u native=%d pair=%u/%u budget=%d source=%x/%x/%d\n",
                        i,hits,found,pair[0],pair[1],state.budget,cpu.m_r[24],cpu.m_r[25],int32_t(cpu.m_r[15]));
                    throw std::runtime_error("native pair traversal differs from i960/TGP");
                }
                swaps += cpu.m_IP == 0x1414c; ++pairs; ++hits;
                hash = (hash^pair[0])*16777619u; hash = (hash^pair[1])*16777619u;
                cost = (i+hits)&1 ? 150 : 300;
                // Deterministic simulated responses move both bodies, including
                // aliases, and change the live raw word. Do NOT refresh from source.
                for (unsigned b = 0; b < 2; ++b) {
                    auto &probe = body[pair[b]].probe;
                    if (i&32) probe.position[b] += b ? -8.0f : 8.0f;
                    if (i&64) probe.word5c = nm_force_bits(float((hits+b)%4));
                    auto &memory = expected[pair[b]];
                    memcpy(memory.data()+0x1c,&probe.position[0],4); memcpy(memory.data()+0x24,&probe.position[1],4);
                    memcpy(memory.data()+0x5c,&probe.word5c,4);
                    const uint32_t address = cpu.m_r[24+b];
                    if (i&32) {
                        const uint32_t at = address+(b ? 0x24 : 0x1c);
                        const float moved = nm_force_float(bus.read_dword(at))+(b ? -8.0f : 8.0f);
                        bus.write_dword(at,nm_force_bits(moved));
                    }
                    if (i&64) bus.write_dword(address+0x5c,nm_force_bits(float((hits+b)%4)));
                }
                cpu.m_r[19] = cost; cpu.m_IP += 4; // Test callee returns; original caller swap/charges execute.
            } else cpu.execute_one();
        }
        uint16_t pair[2]{};
        if (nm_collision_walk_next(&math,body,count,order,primary,i%4,cost,&state,pair) != 0 ||
            cpu.m_r[15] != uint32_t(state.budget) || expected != bus.collision_pool || record != bus.entity ||
            globals != bus.globals || auxiliary != bus.auxiliary)
            throw std::runtime_error("collision traversal final budget/memory differs from i960/TGP");
        exhausted += state.budget <= 0; empty += hits == 0; hash = (hash^uint32_t(state.budget))*16777619u;
    }
    if (!pairs || candidates <= pairs || !swaps || !exhausted || !empty) throw std::runtime_error("collision walk coverage missing");
    if(hash!=NM_COLLISION_WALK_HASH || hash!=nm_collision_walk_test_hash(&math))
        throw std::runtime_error("collision walk target fixture differs from original loop oracle");
    std::printf("native_collision_walk PASS cases=8192 candidates=%u pairs=%u primary_swaps=%u exhausted=%u no_pair=%u hash=%08x state=%zu (original loop/TGP, synthetic response effects)\n",
        candidates,pairs,swaps,exhausted,empty,hash,sizeof(nm_collision_walk_t));
}

inline void check_collision_preparation(rt::M2Board &board) {
    VehicleOracleBus bus(board); uint32_t hash = 2166136261u; unsigned outside = 0, repeated = 0;
    for (unsigned i = 0; i < NM_COLLISION_PREPARE_CASES; ++i) {
        nm_collision_rank_t body[64]; unsigned count, reset, primary; uint32_t wrap; uint16_t order[14];
        nm_collision_prepare_test_input(i,body,&count,&reset,&primary,&wrap);
        bus.entity.fill(0xa5); bus.globals.fill(0x5a); bus.auxiliary.fill(0x11);
        bus.collision_pool.resize(count);
        for (unsigned j = 0; j < count; ++j) {
            auto &memory = bus.collision_pool[j]; memory.fill(0x3c);
            collision_rank_fields(memory,body[j],true);
            const uint32_t next = 0x640000+0x300*body[j].next, prev = 0x640000+0x300*body[j].previous, stride = 0x300;
            memcpy(memory.data()+0x88,&next,4); memcpy(memory.data()+0x8c,&prev,4); memcpy(memory.data()+8,&stride,4);
        }
        bus.collision_wrap = wrap; bus.collision_primary = 0x640000+0x300*primary;
        memcpy(bus.globals.data()+0x84,&reset,4);
        auto memory = bus.collision_pool; auto record = bus.entity;
        const auto globals = bus.globals; const auto auxiliary = bus.auxiliary;
        if (!nm_collision_prepare(body,count,reset,primary,wrap,order)) throw std::runtime_error("native collision preparation rejected scratch links");
        for (unsigned j = 0; j < count; ++j) collision_rank_fields(memory[j],body[j],true);
        for (unsigned j = 0; j < 14; ++j) {
            const uint32_t pointer = 0x640000+0x300*order[j]; memcpy(record.data()+0x5c+4*j,&pointer,4);
        }
        const auto cpu = bus.run(0x13f08,0x1406c);
        if (cpu.m_IP != 0x1406c || memory != bus.collision_pool || record != bus.entity ||
            globals != bus.globals || auxiliary != bus.auxiliary || cpu.m_r[15] != 1000) {
            std::fprintf(stderr,"collision preparation case=%u pool=%d record=%d\n",i,memory==bus.collision_pool,record==bus.entity);
            throw std::runtime_error("native collision preparation differs from i960/TGP whole memory");
        }
        outside += primary >= reset; repeated += count < 14 || i%11 == 0;
        for (unsigned j = 0; j < count; ++j) for (unsigned k = 0; k < offsetof(nm_collision_rank_t,contact_side)+1; ++k)
            hash = (hash^reinterpret_cast<const unsigned char *>(&body[j])[k])*16777619u;
        for (unsigned j = 0; j < 14; ++j) hash = (hash^order[j])*16777619u;
    }
    if (hash != NM_COLLISION_PREPARE_HASH || hash != nm_collision_prepare_test_hash())
        throw std::runtime_error("collision preparation shared digest mismatch");
    std::printf("native_collision_prepare PASS cases=%u primary_outside=%u repeated_window=%u hash=%08x body=%zu (whole13f08..14068, before pair traversal)\n",
        NM_COLLISION_PREPARE_CASES,outside,repeated,hash,sizeof(nm_collision_rank_t));
}

inline void check_collision_detection(rt::M2Board &board, const nm_drive_params_t &math) {
    VehicleOracleBus bus(board); unsigned hits[4]{}, signals = 0; uint32_t hash = 2166136261u;
    for (unsigned v = 0; v < 4; ++v) {
        const auto corner = board.read_dword(0x235af4+4*(4-v));
        if (corner >= 4 || math.collision_corner[corner] != v ||
            math.collision_signal[v] != board.read_word(0x235b40+4*(4-v)))
            throw std::runtime_error("collision detection tables differ from source");
    }
    for (unsigned i = 0; i < NM_COLLISION_DETECT_CASES; ++i) {
        bus.entity.fill(0xa5); bus.globals.fill(0x5a); bus.auxiliary.fill(0x11);
        for (auto &body : bus.neighbors) body.fill(0x3c);
        nm_collision_shape_t body[2]; nm_collision_contact_t contact;
        memset(&contact,0x5a,sizeof(contact)); uint16_t signal = 0x1234;
        if (!nm_collision_detect_test_input(&math,i,body)) throw std::runtime_error("bad detection test input");
        for (unsigned j = 0; j < 2; ++j) collision_shape_fields(bus.neighbors[j],body[j],true);
        collision_contact_fields(bus.entity,contact,true); memcpy(bus.boundary_signal.data(),&signal,2);
        auto memory = bus.neighbors; auto record = bus.entity;
        const auto globals = bus.globals; const auto auxiliary = bus.auxiliary;
        const unsigned second = i%8 != 0;
        std::array<uint32_t,10> args{}; args[8] = 0x620000; args[9] = 0x620000+0x1000*second;
        bus.collision_primary = i&8 ? args[9] : 0x622000;
        // Independently test the shared containment primitive against31/32.
        auto &tgp = board.tgp(); float perimeter[4][3];
        tgp.function_port_w(0x31*4,0x3131);
        for (unsigned v = 0; v < 4; ++v) for (unsigned a = 0; a < 3; ++a) {
            perimeter[v][a] = body[1].world[v^(v>>1)][a];
            tgp.fifo_w(nm_force_bits(perimeter[v][a]));
        }
        float x = body[0].world[i%4][0], z = body[0].world[i%4][2];
        if ((i&7) < 3) {
            x = perimeter[(i>>3)&3][0]; z = perimeter[(i>>3)&3][2];
            if ((i&7) != 0) {
                float &axis = i&32 ? x : z;
                axis = std::nextafter(axis,(i&7) == 1 ? -INFINITY : INFINITY);
            }
        }
        tgp.function_port_w(0x32*4,0x3232);
        tgp.fifo_w(nm_force_bits(x)); tgp.fifo_w(0); tgp.fifo_w(nm_force_bits(z)); tgp.sync();
        const auto raw = tgp.fifo_r();
        if ((raw != 0 && raw != UINT32_MAX) || nm_contact_contains(perimeter,x,z) != int(raw>>31) || !tgp.fifo_out_empty())
            throw std::runtime_error("native containment differs from command32");
        if (!nm_collision_detect(&math,&body[0],&body[second],(i&8)!=0,&contact,&signal))
            throw std::runtime_error("native detection rejected scratch pair");
        for (unsigned j = 0; j < 2; ++j) collision_shape_fields(memory[j],body[j],true);
        collision_contact_fields(record,contact,true);
        const auto cpu = bus.run(0x14180,0x148d4,&args);
        uint16_t expected_signal; memcpy(&expected_signal,bus.boundary_signal.data(),2);
        if (cpu.m_IP != 0x14630 || memory != bus.neighbors || record != bus.entity ||
            globals != bus.globals || auxiliary != bus.auxiliary || signal != expected_signal ||
            cpu.m_r[24] != args[8] || cpu.m_r[25] != args[9] || cpu.m_r[29] != 0x500000) {
            std::fprintf(stderr,"collision detection case=%u flags=%u reference=%u pair=%d record=%d signal=%x/%x\n",
                i,contact.flags,bus.read_dword(0x500034),memory==bus.neighbors,record==bus.entity,signal,expected_signal);
            for (unsigned j = 0; j < 2; ++j) for (unsigned k = 0; k < 0x300; ++k)
                if (memory[j][k] != bus.neighbors[j][k]) {
                    std::fprintf(stderr,"body%u offset=%x native=%02x reference=%02x\n",j,k,memory[j][k],bus.neighbors[j][k]); break;
                }
            for (unsigned k = 0; k < 12; ++k) {
                uint32_t n,r; memcpy(&n,memory[0].data()+0xf4+4*k,4); memcpy(&r,bus.neighbors[0].data()+0xf4+4*k,4);
                std::fprintf(stderr,"world%u native=%08x reference=%08x\n",k,n,r);
            }
            throw std::runtime_error("native detection differs from whole i960/TGP memory");
        }
        ++hits[contact.flags]; signals += signal != 0x1234;
        for (unsigned b = 0; b < 2; ++b)
            for (unsigned k = 0; k < offsetof(nm_collision_shape_t,side)+1; ++k)
                hash = (hash^reinterpret_cast<const unsigned char *>(&body[b])[k])*16777619u;
        for (const auto byte : std::bit_cast<std::array<unsigned char,sizeof(contact)>>(contact)) hash = (hash^byte)*16777619u;
        hash = (hash^signal)*16777619u;
    }
    if (hash != NM_COLLISION_DETECT_HASH || hash != nm_collision_detect_test_hash(&math))
        throw std::runtime_error("detection shared digest mismatch");
    std::printf("native_collision_detect PASS cases=%u hits0/1/2/3=%u/%u/%u/%u signals=%u hash=%08x shape=%zu contact=%zu (whole detection, before response)\n",
        NM_COLLISION_DETECT_CASES,hits[0],hits[1],hits[2],hits[3],signals,hash,sizeof(nm_collision_shape_t),sizeof(nm_collision_contact_t));
}

inline void check_collision_candidate(rt::M2Board &board, const nm_drive_params_t &math) {
    VehicleOracleBus bus(board); uint32_t hash = 2166136261u; unsigned hits = 0;
    // Verify the source immediates independently; no new asset pack is needed.
    if (board.read_dword(0x140b0) != 0x40a54413 || board.read_dword(0x14134) != 0x3fd9999a)
        throw std::runtime_error("collision broad-phase source constants changed");
    for (unsigned i = 0; i < NM_COLLISION_CANDIDATE_CASES; ++i) {
        bus.entity.fill(0xa5); bus.globals.fill(0x5a); bus.auxiliary.fill(0x11);
        for (auto &body : bus.neighbors) body.fill(0x3c);
        nm_collision_probe_t probe[2]; nm_collision_candidate_test_input(i,probe);
        for (unsigned body = 0; body < 2; ++body) {
            memcpy(bus.neighbors[body].data()+0x1c,&probe[body].position[0],4);
            memcpy(bus.neighbors[body].data()+0x24,&probe[body].position[1],4);
            memcpy(bus.neighbors[body].data()+0x5c,&probe[body].word5c,4);
        }
        auto memory = bus.neighbors; const auto record = bus.entity;
        const auto globals = bus.globals; const auto auxiliary = bus.auxiliary;
        unsigned hit = 99;
        if (!nm_collision_candidate(&math,&probe[0],&probe[1],&hit))
            throw std::runtime_error("collision broad-phase rejected scratch positions");
        std::array<uint32_t,10> args{}; args[8] = 0x620000; args[9] = 0x621000;
        bus.run(0x140a4,0x140d0,&args);
        // A preceding response can move g9 after command40. Its snapshot, not
        // this current position, must still be used by the next command41.
        if (i&1) for (unsigned axis = 0; axis < 2; ++axis) {
            const float moved = probe[1].position[axis]+(axis ? -64.0f : 32.0f);
            const unsigned at = axis ? 0x24 : 0x1c;
            memcpy(bus.neighbors[1].data()+at,&moved,4); memcpy(memory[1].data()+at,&moved,4);
        }
        const auto cpu = bus.run(0x140e0,0x14168,&args);
        if (hit != (cpu.m_IP == 0x1413c) || (cpu.m_IP != 0x1413c && cpu.m_IP != 0x14168) ||
            memory != bus.neighbors || record != bus.entity || globals != bus.globals || auxiliary != bus.auxiliary ||
            cpu.m_r[24] != args[8] || cpu.m_r[25] != args[9] || cpu.m_r[29] != 0x500000) {
            std::fprintf(stderr,"collision candidate case=%u native=%u ip=%x\n",i,hit,cpu.m_IP);
            throw std::runtime_error("collision broad-phase differs from i960/TGP branch or memory");
        }
        hits += hit; hash = (hash^hit)*16777619u;
    }
    if (hits != 44675 || hash != NM_COLLISION_CANDIDATE_HASH || hash != nm_collision_candidate_test_hash(&math))
        throw std::runtime_error("collision broad-phase coverage/digest regression");
    std::printf("native_collision_candidate PASS cases=196608 cached_center_cases=98304 hits=%u hash=%08x body=%zu (whole pair gate, not list traversal/contact detection)\n",
        hits,hash,sizeof(nm_collision_probe_t));
}

inline void check_collision_travel(rt::M2Board &board) {
    VehicleOracleBus bus(board); uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < 65536; ++i) {
        bus.entity.fill(0xa5); bus.globals.fill(0x5a); bus.auxiliary.fill(0x11);
        nm_collision_travel_t state{};
        float position[2]{};
        for (unsigned axis = 0; axis < 2; ++axis) {
            position[axis] = i%17 ? float(int(i*13+17*axis)-400000)*0.125f : axis ? -0.0f : 0.0f;
            state.previous[axis] = i%13 ? float(int(i*11+31*axis)-300000)*0.25f : axis ? 0.0f : -0.0f;
            state.travel[axis] = -123;
            memcpy(bus.entity.data()+(axis ? 0x24 : 0x1c),&position[axis],4);
        }
        memcpy(bus.entity.data()+0x28c,&state,sizeof(state));
        auto expected = bus.entity; const auto globals = bus.globals; const auto auxiliary = bus.auxiliary;
        if (!nm_collision_track(position,&state)) throw std::runtime_error("collision travel rejected finite input");
        memcpy(expected.data()+0x28c,&state,sizeof(state));
        bus.run(0x13f90,0x13fe0);
        if (bus.entity != expected || bus.globals != globals || bus.auxiliary != auxiliary)
            throw std::runtime_error("collision travel differs from whole i960/TGP memory");
        for (auto byte : expected) hash = (hash^byte)*16777619u;
    }
    if (hash != 0xd135ea61) throw std::runtime_error("collision travel digest regression");
    std::printf("native_collision_travel PASS cases=65536 hash=%08x state=%zu (one-body13f90..13fdc, not list traversal/live coverage)\n",hash,sizeof(nm_collision_travel_t));
}

inline void check_collision_route(rt::M2Board &board, const nm_drive_params_t &math) {
    VehicleOracleBus bus(board); uint32_t hash = 2166136261u;
    unsigned counts[3]{};
    const uint32_t flags[] = {0,1,2,3,4,UINT32_MAX};
    // Independently execute each source return-value store. Full live response
    // checks verify these after the actual response; scratch dispatch stops before it.
    if (bus.run(0x14640,0x14644).m_r[19] != 300 || bus.run(0x1467c,0x14680).m_r[19] != 300 ||
        bus.run(0x14684,0x14688).m_r[19] != 150) throw std::runtime_error("collision caller return constants differ");
    for (unsigned i = 0; i < 65536; ++i) for (unsigned f = 0; f < 6; ++f) {
        bus.entity.fill(0); bus.globals.fill(0x5a); bus.auxiliary.fill(0x11);
        for (auto &memory : bus.neighbors) memory.fill(0);
        const int16_t first_heading = nm_yaw_angle(i+32767), second_heading = 32767;
        memcpy(bus.neighbors[0].data()+0x58,&first_heading,2); memcpy(bus.neighbors[1].data()+0x58,&second_heading,2);
        memcpy(bus.entity.data()+0x34,&flags[f],4);
        const uint32_t angle = 0xffff0000u|i; memcpy(bus.entity.data()+0x38,&angle,4);
        const auto input = collision_route_input(bus.entity,bus.neighbors[0],bus.neighbors[1]);
        const auto first = collision_choice_body(bus.neighbors[0]), second = collision_choice_body(bus.neighbors[1]);
        const auto memory = bus.neighbors; const auto record = bus.entity; const auto globals = bus.globals; const auto auxiliary = bus.auxiliary;
        nm_collision_route_t out{};
        if (!nm_collision_route(&math,&input,&first,&second,&out)) throw std::runtime_error("native route rejected scratch input");
        std::array<uint32_t,10> args{}; args[8] = 0x620000; args[9] = 0x621000;
        const auto cpu = bus.run(0x14630,0x14688,&args);
        const unsigned branch = cpu.m_IP == 0x148dc ? NM_COLLISION_FIRST : cpu.m_IP == 0x148f0 ? NM_COLLISION_SECOND :
            cpu.m_IP == 0x14688 ? NM_COLLISION_NONE : 99;
        if (branch != out.branch || memory != bus.neighbors || record != bus.entity || globals != bus.globals || auxiliary != bus.auxiliary ||
            cpu.m_r[24] != args[8] || cpu.m_r[25] != args[9] || cpu.m_r[29] != 0x500000 ||
            out.caller_result != (branch == NM_COLLISION_NONE ? cpu.m_r[19] : 300u)) {
            std::fprintf(stderr,"collision route angle=%u flags=%x native/ref=%u/%u ip=%x\n",i,flags[f],out.branch,branch,cpu.m_IP);
            throw std::runtime_error("collision dispatch differs from i960/TGP branch/memory");
        }
        ++counts[branch]; hash = (hash^out.branch)*16777619u; hash = (hash^out.caller_result)*16777619u;
    }
    if (counts[0] != 228989 || counts[1] != 98175 || counts[2] != 66052 || hash != 0x7d75ffb0)
        throw std::runtime_error("collision route branch/digest regression");
    std::printf("native_collision_route PASS cases=393216 first/second/none=%u/%u/%u hash=%08x input=%zu result=%zu (dispatch prefix plus caller return constants; response checked separately)\n",
        counts[0],counts[1],counts[2],hash,sizeof(nm_collision_route_input_t),sizeof(nm_collision_route_t));
}

inline void check_collision_choice(rt::M2Board &board, const nm_drive_params_t &math) {
    for (unsigned c = 0; c < 4; ++c)
        if (board.read_dword(0x235b08+4*c) != 0xf4+12u*math.collision_corner[c])
            throw std::runtime_error("collision corner differs from original byte-offset table");
    VehicleOracleBus bus(board);
    uint32_t hash = 2166136261u;
    unsigned branches[2]{}, angular = 0, alias_cases = 0;
    for (unsigned i = 0; i < 196608; ++i) {
        const bool alias = i >= 131072;
        bus.entity.fill(0xa5); bus.globals.fill(0x5a); bus.auxiliary.fill(0x11);
        for (auto &memory : bus.neighbors) memory.fill(0x3c);
        for (unsigned body = 0; body < 2; ++body) {
            const auto put = [&](unsigned at, auto value) { memcpy(bus.neighbors[body].data()+at,&value,sizeof(value)); };
            put(0x294,float(int((i+137*body)%501)-250)*0.125f);
            put(0x298,i%17 ? float(int((3*i+97*body)%431)-215)*0.0625f : body ? -0.0f : 0.0f);
            for (unsigned corner = 0; corner < 4; ++corner) for (unsigned axis = 0; axis < 3; ++axis)
                put(0xf4+12*corner+4*axis,float(int((i+13*corner+17*body+31*axis)%257)-128)*0.25f);
        }
        const auto first = collision_choice_body(bus.neighbors[0]);
        const auto second = collision_choice_body(bus.neighbors[alias ? 0 : 1]);
        const float dx = second.travel[0]-first.travel[0], dz = second.travel[1]-first.travel[1];
        const float distance = nm_force_length(&math,0.0f-dx,0.0f-dz);
        const float depths[] = {0,-0.0f,distance,-distance,std::nextafter(distance,INFINITY),
            std::nextafter(distance,0.0f),2*distance,-2*distance,0.25f,1000.0f};
        const auto put = [&](unsigned at, auto value) { memcpy(bus.entity.data()+at,&value,sizeof(value)); };
        put(0x9c,depths[(i/16)%10]); put(0x98,depths[(i/160)%10]);
        put(0x38,uint32_t(0xa5a50000u|(i&65535))); put(0x48,uint32_t(i&3)); put(0x54,uint32_t((i>>2)&3));
        const auto in = collision_choice_input(bus.entity);
        angular += std::max(fabsf(in.depth[0]),fabsf(in.depth[1])) <= distance;
        const auto memory = bus.neighbors; const auto record = bus.entity; const auto globals = bus.globals; const auto auxiliary = bus.auxiliary;
        unsigned branch = 77;
        if (!nm_collision_choose(&math,&in,&first,&second,&branch)) throw std::runtime_error("native choice rejected scratch input");
        std::array<uint32_t,10> args{}; args[8] = 0x620000; args[9] = alias ? 0x620000 : 0x621000;
        const auto cpu = bus.run(0x14904,0x14a20,&args);
        const unsigned actual = cpu.m_IP == 0x148dc ? 0 : cpu.m_IP == 0x148f0 ? 1 : 77;
        if (branch != actual || memory != bus.neighbors || record != bus.entity || globals != bus.globals || auxiliary != bus.auxiliary ||
            cpu.m_r[24] != args[8] || cpu.m_r[25] != args[9] || cpu.m_r[29] != 0x500000) {
            std::fprintf(stderr,"collision choice case=%u native/ref=%u/%u distance=%g depths=%g/%g corners=%u/%u\n",
                i,branch,actual,distance,in.depth[0],in.depth[1],in.corner[0],in.corner[1]);
            throw std::runtime_error("collision choice differs from i960/TGP branch or changed memory");
        }
        ++branches[branch]; alias_cases += alias;
        hash = (hash^branch)*16777619u;
    }
    if (branches[0] != 102885 || branches[1] != 93723 || angular != 79376 || alias_cases != 65536 || hash != 0x11c5844c)
        throw std::runtime_error("collision choice branch/alias/digest regression");
    std::printf("native_collision_choice PASS cases=196608 first/second=%u/%u angular=%u aliases=%u hash=%08x body=%zu input=%zu (whole14904 decision, memory unchanged, not contact detection)\n",
        branches[0],branches[1],angular,alias_cases,hash,sizeof(nm_collision_choice_body_t),sizeof(nm_collision_choice_input_t));
}

inline void check_collision_impulse(rt::M2Board &board, const nm_drive_params_t &math) {
    if (board.read_dword(0) || board.read_dword(0x28b573c) != nm_force_bits(math.collision_speed_step))
        throw std::runtime_error("collision impulse parameters differ from source");
    VehicleOracleBus bus(board);
    uint32_t hash = 2166136261u;
    unsigned skipped = 0, lateral = 0, longitudinal = 0, signals = 0;
    const int32_t severities[] = {INT32_MIN,-1,0,24,25,29,30,31,INT32_MAX};
    for (unsigned i = 0; i < 131072; ++i) {
        const bool alias = i >= 65536;
        bus.entity.fill(0xa5); bus.globals.fill(0x5a); bus.auxiliary.fill(0x11);
        for (auto &body : bus.neighbors) body.fill(0x3c);
        bus.boundary_spin_signal = 1234; bus.boundary_shake = i%3 ? 0 : 255;
        nm_collision_impulse_input_t in{};
        in.flags = i%11 == 0 ? 1u<<15 : i%13 == 0 ? 1u<<16 : 0;
        in.contact_flags = i&2; in.severity = severities[(i/4)%9];
        memcpy(bus.entity.data(),&in.flags,4); memcpy(bus.entity.data()+0x34,&in.contact_flags,4);
        nm_collision_impulse_body_t bodies[2]{};
        for (unsigned body = 0; body < 2; ++body) {
            auto &s = bodies[body];
            s.speed = i%17 ? float(int((i+79*body)%1001)-500)*0.25f : body ? -0.0f : 0.0f;
            s.delta[0] = float(int((i+13*body)%103)-50)*0.125f;
            s.delta[1] = float(int((i+23*body)%137)-60)*0.0625f;
            s.correction[0] = 12; s.correction[1] = -13;
            s.yaw = nm_yaw_angle(i+12345*body); s.own_yaw = -12; s.other_yaw = 13;
            s.inhibit_ticks = i&4 ? -1 : 0;
            s.input_inhibit = std::bit_cast<int8_t>(uint8_t(i));
            s.response_ticks = i%5 ? 0 : -128;
            s.entity_id = i&1 ? uint8_t(1+(i/2)%255) : 0;
            s.contact_side = uint8_t(i/8);
            collision_impulse_fields(bus.neighbors[body],s,true);
        }
        auto expected = bus.neighbors;
        const auto record = bus.entity; const auto globals = bus.globals; const auto auxiliary = bus.auxiliary;
        const auto &second = bodies[alias ? 0 : 1];
        const bool skip = !second.entity_id && (bodies[0].input_inhibit < -48 || bodies[0].input_inhibit > 48 || (in.flags&0x18000));
        skipped += skip;
        lateral += !skip && (second.contact_side == 1 || second.contact_side == 3);
        longitudinal += !skip && second.contact_side != 1 && second.contact_side != 3 && uint32_t(in.severity) >= 30;
        signals += !skip && !second.entity_id && in.severity >= 25;
        nm_collision_impulse_shared_t shared{bus.boundary_spin_signal,std::bit_cast<int8_t>(bus.boundary_shake)};
        if (!nm_collision_impulse(&math,&in,&bodies[0],&bodies[alias ? 0 : 1],&shared))
            throw std::runtime_error("collision impulse rejected finite scratch case");
        for (unsigned body = 0; body < (alias ? 1u : 2u); ++body) collision_impulse_fields(expected[body],bodies[body],true);
        std::array<uint32_t,10> args{}; args[4] = uint32_t(in.severity); args[8] = 0x620000; args[9] = alias ? 0x620000 : 0x621000;
        const auto cpu = bus.run(0x14c54,0x14e6c,&args);
        if (bus.neighbors != expected || bus.entity != record || bus.globals != globals || bus.auxiliary != auxiliary ||
            bus.boundary_spin_signal != shared.signal || bus.boundary_shake != uint8_t(shared.shake_ticks) ||
            cpu.m_r[20] != args[4] || cpu.m_r[24] != args[8] || cpu.m_r[25] != args[9] || cpu.m_r[29] != 0x500000) {
            for (unsigned body = 0; body < 3; ++body) for (unsigned at = 0; at < 0x300; ++at)
                if (bus.neighbors[body][at] != expected[body][at]) std::fprintf(stderr,"collision impulse case=%u body=%u offset=%03x native=%02x ref=%02x\n",
                    i,body,at,expected[body][at],bus.neighbors[body][at]);
            throw std::runtime_error("collision impulse differs from whole i960/TGP memory");
        }
        for (const auto &body : expected) for (auto byte : body) hash = (hash^byte)*16777619u;
        hash = (hash^shared.signal)*16777619u; hash = (hash^uint8_t(shared.shake_ticks))*16777619u;
    }
    if (skipped != 44485 || lateral != 943 || longitudinal != 47573 || signals != 11687 || hash != 0xa77137ab)
        throw std::runtime_error("collision impulse branch/digest regression");
    std::printf("native_collision_impulse PASS cases=131072 skipped=%u lateral=%u longitudinal=%u signals=%u hash=%08x body=%zu shared=%zu (whole14c54 memory/registers, signed/unsigned gates and aliases)\n",
        skipped,lateral,longitudinal,signals,hash,sizeof(nm_collision_impulse_body_t),sizeof(nm_collision_impulse_shared_t));
}

inline void check_collision_damage(rt::M2Board &board, const nm_drive_params_t &math) {
    for (unsigned t = 0; t < 3; ++t) for (unsigned i = 0; i < 4; ++i)
        if (math.collision_threshold[t][i] != board.read_word(0x14fb4+8*t+2*i))
            throw std::runtime_error("collision threshold differs from source");
    for (unsigned b = 0; b < 2; ++b) for (unsigned side = 0; side < 4; ++side)
        if (math.collision_flags[b][side] != (1u<<(board.read_dword(0x235000+16*b+4*side)&31)))
            throw std::runtime_error("collision flag mask differs from source");
    const uint32_t sequence = board.read_dword(board.read_dword(0x2896e58)+4);
    if (sequence != board.read_dword(board.read_dword(0x2896e5c)+4))
        throw std::runtime_error("collision crash source directories differ");
    VehicleOracleBus bus(board);
    uint32_t hash = 2166136261u;
    unsigned starts = 0, shakes[2]{}, selectors[4]{}, rejected = 0, cases = 0;
    const auto run = [&](unsigned alias) {
        nm_collision_damage_body_t bodies[2]{};
        for (unsigned b = 0; b < 2; ++b) {
            collision_damage_fields(bus.neighbors[b],bodies[b],sequence,false);
            bodies[b].damage.shake_ticks = std::bit_cast<int8_t>(bus.boundary_shake);
            bodies[b].damage.sequence_counter = bus.boundary_sequence_counter;
        }
        nm_collision_damage_input_t in{};
        memcpy(&in.global_flags,bus.globals.data()+0x90,4); in.control = bus.damage_control;
        in.player_mode = bus.entity[0x6d]; in.curve = bus.curve_selector;
        selectors[(bodies[alias ? 0 : 1].entity_id != 0)+2*(in.player_mode != 0)]++;
        const auto before = bus.neighbors; auto expected = before;
        const auto record = bus.entity; const auto globals = bus.globals; auto auxiliary = bus.auxiliary;
        const auto state_before = bodies[alias ? 0 : 1];
        int32_t strength = 12345;
        const bool accepted = nm_collision_damage(&math,&in,&bodies[0],&bodies[alias ? 0 : 1],&strength);
        const auto &s = bodies[alias ? 0 : 1].damage;
        for (unsigned b = 0; b < (alias ? 1u : 2u); ++b) collision_damage_fields(expected[b],bodies[b],sequence,true);
        if (accepted && s.auxiliary_request) {
            uint32_t flags; memcpy(&flags,auxiliary.data(),4); flags |= 0x80000000u;
            memcpy(auxiliary.data(),&flags,4); const uint32_t function = 0x5c14; memcpy(auxiliary.data()+12,&function,4);
        }
        std::array<uint32_t,10> args{}; args[8] = 0x620000; args[9] = alias ? 0x620000 : 0x621000;
        auto cpu = bus.run(0x14fcc,0x15234,&args);
        if (accepted != bus.collision_conversion_valid || bus.neighbors != expected ||
            bus.entity != record || bus.globals != globals || bus.auxiliary != auxiliary ||
            (accepted && (cpu.m_r[20] != uint32_t(strength) || cpu.m_r[29] != 0x500000 || cpu.m_r[25] != args[9] ||
                bus.boundary_shake != (uint8_t)s.shake_ticks || bus.boundary_sequence_counter != s.sequence_counter ||
                bus.boundary_sound_mask != s.sound_requests)) ||
            (!accepted && (expected != before || strength != 12345))) {
            for (unsigned b = 0; b < 2; ++b) for (unsigned at = 0; at < 0x300; ++at)
                if (bus.neighbors[b][at] != expected[b][at]) std::fprintf(stderr,"collision damage case=%u body=%u offset=%03x native=%02x ref=%02x\n",
                    cases,b,at,expected[b][at],bus.neighbors[b][at]);
            std::fprintf(stderr,"collision damage case=%u accepted=%u/%u strength=%d/%d shake=%d/%u sounds=%u/%u\n",
                cases,accepted,bus.collision_conversion_valid,strength,int32_t(cpu.m_r[20]),s.shake_ticks,bus.boundary_shake,s.sound_requests,bus.boundary_sound_mask);
            throw std::runtime_error("collision damage differs from whole i960/TGP memory/events/g4");
        }
        if (accepted) {
            starts += s.crash_count != state_before.damage.crash_count;
            shakes[0] += !state_before.damage.shake_ticks && s.shake_ticks == 16;
            shakes[1] += !state_before.damage.shake_ticks && s.shake_ticks == 30;
        } else ++rejected;
        for (const auto &b : expected) for (auto byte : b) hash = (hash^byte)*16777619u;
        for (auto byte : auxiliary) hash = (hash^byte)*16777619u;
        for (uint32_t value : {uint32_t(strength),uint32_t(accepted),uint32_t(bus.boundary_shake),
                uint32_t(bus.boundary_sequence_counter),uint32_t(bus.boundary_sound_mask)})
            for (unsigned byte = 0; byte < 4; ++byte) hash = (hash^((value>>(8*byte))&255))*16777619u;
        ++cases;
    };
    for (unsigned i = 0; i < 131072; ++i) {
        bus.entity.fill(0xa5); bus.globals.fill(0x5a); bus.auxiliary.fill(0x11);
        for (auto &body : bus.neighbors) body.fill(0x3c);
        bus.curve_selector = (i>>6)&1; bus.damage_variant = (uint8_t)(i>>7);
        bus.entity[0x6d] = (i>>1)&1 ? 255 : 0;
        bus.boundary_shake = i%3 ? 0 : 255; bus.boundary_sequence_counter = (uint8_t)i;
        bus.damage_control = i%5 ? 0 : 224;
        bus.write_dword(0x501090,i&2 ? 0x00500000 : 0x08000000);
        const unsigned selector = (i&1)+2*((i>>1)&1);
        const int lo = math.collision_threshold[0][selector], mid = math.collision_threshold[1][selector], hi = math.collision_threshold[2][selector];
        const int values[] = {0,lo-1,lo,lo+1,mid-1,mid,mid+1,hi-1,hi,hi+1,255,512};
        for (unsigned b = 0; b < 2; ++b) {
            auto &memory = bus.neighbors[b];
            const auto put = [&](unsigned at, auto v) { memcpy(memory.data()+at,&v,sizeof(v)); };
            put(0,uint32_t(0xa5a5a5a5u & ~64u));
            put(0x9c,b ? 0.0f : float(values[(i/4)%12])/math.speed_index_scale);
            put(0xa4,b ? -0.0f : (i%17 ? 0.0f : 2.5f));
            put(0xc8,int16_t((i>>8)%3 ? 0 : -1));
            memory[4] = (i&1) ? (uint8_t)(1+(i>>9)%255) : 0;
            memory[0xd4] = (i>>4)&1 ? (uint8_t)i : 0;
            memory[0x27d] = i%7 ? 3 : 255; memory[0xed] = (uint8_t)(i>>2);
            memory[0xc7] = (uint8_t)(i>>3);
        }
        run(i>>16);
    }
    for (float delta : {0.0f,1e10f,-1e10f,1e30f,INFINITY,NAN}) {
        const uint32_t bits = nm_force_bits(delta); memcpy(bus.neighbors[0].data()+0x9c,&bits,4);
        run(0);
    }
    for (unsigned count : selectors) if (!count) throw std::runtime_error("collision damage selector lacks coverage");
    // Owned player low=999 is ABOVE mid=120: its shake16 path is unreachable.
    // The ROM-free analytic check covers that branch with ordered test values.
    if (cases != 131078 || starts != 9001 || shakes[0] || shakes[1] != 7608 || rejected != 4 || hash != 0x3004e2a7) {
        std::fprintf(stderr,"collision damage coverage starts=%u shake16/30=%u/%u rejected=%u\n",starts,shakes[0],shakes[1],rejected);
        throw std::runtime_error("collision damage branch/conversion coverage missing");
    }
    std::printf("native_collision_damage PASS cases=%u starts=%u shake16/30=%u/%u rejected=%u hash=%08x state=%zu input=%zu (whole14fcc memory/events/g4, not14c54 velocity)\n",
        cases,starts,shakes[0],shakes[1],rejected,hash,sizeof(nm_collision_damage_body_t),sizeof(nm_collision_damage_input_t));
}

inline void check_collision_reaction(rt::M2Board &board, const nm_drive_params_t &math) {
    if (board.read_dword(0)) throw std::runtime_error("collision default selector differs");
    for (unsigned table = 0; table < 2; ++table) for (unsigned i = 0; i < 5; ++i)
        for (unsigned body = 0; body < 2; ++body)
            if (math.collision_ticks[table][i][body] != (uint8_t)board.read_word(0x235b18+20*table+4*i+2*body))
                throw std::runtime_error("collision timers differ from authored halfwords");
    VehicleOracleBus bus(board);
    uint32_t hash = 2166136261u;
    unsigned selectors[5]{}, lateral_cases = 0, rejected = 0;
    for (unsigned i = 0; i < 65536; ++i) {
        bus.entity.fill(0xa5); bus.globals.fill(0x5a); bus.profile = {};
        auto &lane = bus.profile.lanes[4];
        lane.xyz[0] = float(int(i%97)-48); lane.xyz[2] = float(int(i%73)-36);
        lane.angles[1] = nm_yaw_angle(i);
        const float x = lane.xyz[0]+float(int(i%257)-128)*0.125f;
        const float z = lane.xyz[2]+float(int(i%137)-68)*0.125f;
        bus.write_word(0x500128,0); bus.write_dword(0x50001c,nm_force_bits(x)); bus.write_dword(0x500024,nm_force_bits(z));
        auto expected = bus.entity; const auto globals = bus.globals;
        int16_t lateral = -1234;
        if (!nm_road_measure_lateral(&math,&bus.profile,x,z,&lateral)) throw std::runtime_error("lateral rejected oracle input");
        memcpy(expected.data()+0x13e,&lateral,2);
        bus.run(0x13a14,0x13b24);
        if (!bus.lateral_conversion_valid || bus.entity != expected || bus.globals != globals)
            throw std::runtime_error("lateral differs from complete i960/TGP memory");
        for (auto byte : expected) hash = (hash^byte)*16777619u;
        ++lateral_cases;
    }
    // Exercise conversion limits and wrapped+1024 before signed clamping.
    const float extremes[] = {-0.0f,0.0f,-1e7f,1e7f,-25264512.0f,-25264514.0f,25264512.0f,
        25264514.0f,-1e30f,1e30f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()};
    for (float x : extremes) {
        bus.profile = {}; bus.entity.fill(0xa5); bus.write_word(0x500128,0);
        bus.write_dword(0x50001c,nm_force_bits(x)); bus.write_dword(0x500024,0);
        const auto before = bus.entity; auto expected = before;
        int16_t lateral = -1234;
        const bool valid = nm_road_measure_lateral(&math,&bus.profile,x,0,&lateral);
        bus.run(0x13a14,0x13b24);
        if (valid) memcpy(expected.data()+0x13e,&lateral,2);
        if (valid != bus.lateral_conversion_valid || expected != bus.entity || (!valid && lateral != -1234))
            throw std::runtime_error("lateral conversion domain/atomic failure differs");
        rejected += !valid; ++lateral_cases;
    }
    for (unsigned alias = 0; alias < 2; ++alias) for (unsigned i = 0; i < 65536; ++i) {
        bus.entity.fill(0xa5); bus.globals.fill(0x5a);
        for (auto &body : bus.neighbors) body.fill(0x3c);
        nm_road_profile_t profiles[2]{};
        nm_collision_reaction_t bodies[2]{};
        for (unsigned body = 0; body < 2; ++body) {
            auto &p = profiles[body]; auto &s = bodies[body];
            p.profile_control = i%3 == 0 ? 0 : i%3 == 1 ? INT16_MIN : INT16_MAX;
            p.lanes[4].xyz[0] = float(int((i+31*body)%19)-9)*0.125f;
            p.lanes[4].xyz[2] = float(int((i+23*body)%17)-8)*0.125f;
            p.lanes[4].angles[1] = nm_yaw_angle(i+163*body);
            s.position[0] = float(int((i+17*body)%47)-23)*0.25f;
            s.position[1] = i%7 ? float(int((i+23*body)%37)-18)*0.5f : -0.0f;
            s.heading = nm_yaw_angle(i*73); s.profile = (uint16_t)body;
            s.lateral = -1234; s.correction_ticks = -45; s.initial_inhibit = 211;
            auto &memory = bus.neighbors[body];
            memcpy(memory.data()+0x1c,&s.position[0],4); memcpy(memory.data()+0x24,&s.position[1],4);
            memcpy(memory.data()+0x48,&s.heading,2); memcpy(memory.data()+0x128,&s.profile,2);
            memcpy(memory.data()+0x13e,&s.lateral,2); memory[0x214] = (uint8_t)s.correction_ticks;
            memory[0x20d] = s.initial_inhibit;
        }
        bus.profile = profiles[0]; bus.collision_profile = profiles[1];
        nm_road_pack_t road{}; road.profiles = profiles; road.profile_count = 2;
        auto expected = bus.neighbors; const auto record = bus.entity; const auto globals = bus.globals;
        int16_t relative = 1234;
        if (!nm_collision_react(&math,&road,&bodies[0],&bodies[alias ? 0 : 1],&relative))
            throw std::runtime_error("native collision reaction rejected oracle input");
        for (unsigned body = 0; body < (alias ? 1u : 2u); ++body) {
            memcpy(expected[body].data()+0x13e,&bodies[body].lateral,2);
            expected[body][0x214] = (uint8_t)bodies[body].correction_ticks;
            expected[body][0x20d] = bodies[body].initial_inhibit;
        }
        std::array<uint32_t,10> args{}; args[8] = 0x620000; args[9] = alias ? 0x620000 : 0x621000;
        auto cpu = bus.run(0x15238,0x1530c,&args);
        if (!bus.lateral_conversion_valid || bus.neighbors != expected || bus.entity != record ||
            bus.globals != globals || cpu.m_r[19] != uint32_t(int32_t(relative)) || cpu.m_r[29] != 0x500000) {
            std::fprintf(stderr,"collision reaction mismatch alias=%u case=%u relative=%d/%d index=%u\n",
                alias,i,relative,int32_t(cpu.m_r[19]),cpu.m_r[23]);
            throw std::runtime_error("collision reaction differs from complete i960/TGP memory/register output");
        }
        if (cpu.m_r[23] >= 5) throw std::runtime_error("collision selector out of range");
        ++selectors[cpu.m_r[23]];
        for (const auto &body : expected) for (auto byte : body) hash = (hash^byte)*16777619u;
        hash = (hash^uint16_t(relative))*16777619u;
    }
    for (unsigned count : selectors) if (!count) throw std::runtime_error("collision reaction selector lacks coverage");
    if (lateral_cases != 65548 || rejected != 5 || hash != 0xa9013475)
        throw std::runtime_error("collision reaction/lateral coverage or digest changed");
    std::printf("native_collision_reaction PASS pairs=131072 lateral=%u rejected=%u selectors=%u/%u/%u/%u/%u hash=%08x state=%zu bytes (whole memory/g3, aliases; not force/damage response)\n",
        lateral_cases,rejected,selectors[0],selectors[1],selectors[2],selectors[3],selectors[4],hash,sizeof(nm_collision_reaction_t));
}

inline void check_collision_separation(rt::M2Board &board, const nm_drive_params_t &math) {
    VehicleOracleBus bus(board);
    uint32_t hash = 2166136261u;
    unsigned cases = 0;
    for (unsigned mode = 0; mode < 4; ++mode) for (unsigned angle = 0; angle < 65536; ++angle) {
        const unsigned branch = mode&1, alias = mode>>1;
        bus.entity.fill(0xa5); bus.globals.fill(0x5a);
        for (auto &body : bus.neighbors) body.fill(0x3c);
        nm_collision_body_t bodies[2]{};
        for (unsigned body = 0; body < 2; ++body) {
            bodies[body].position[0] = float(int((angle+body*17)%97)-48)*0.25f;
            bodies[body].position[1] = angle%7 ? float(int((angle+body*23)%137)-68)*0.5f : -0.0f;
            bodies[body].side = 0x3c;
            memcpy(bus.neighbors[body].data()+0x1c,&bodies[body].position[0],4);
            memcpy(bus.neighbors[body].data()+0x24,&bodies[body].position[1],4);
        }
        if (alias) bodies[1] = bodies[0];
        const uint32_t full_angle = angle | ((angle*73u)<<16), side = angle*15485863u;
        const float depth = angle%8 == 0 ? -0.0f : float(int(angle%257)-128)*0.125f;
        bus.write_dword(0x500000+(branch ? 0x4c : 0x38),full_angle);
        bus.write_dword(0x500000+(branch ? 0x98 : 0x9c),nm_force_bits(depth));
        bus.write_dword(0x500000+(branch ? 0x50 : 0x3c),side);
        auto expected = bus.neighbors;
        const auto record = bus.entity;
        const auto globals = bus.globals;
        auto *plus = bodies+(alias ? 0 : branch), *minus = bodies+(alias ? 0 : 1-branch);
        if (!nm_collision_separate(&math,full_angle,depth,side,plus,minus))
            throw std::runtime_error("native collision separation rejected oracle input");
        for (unsigned body = 0; body < (alias ? 1u : 2u); ++body) {
            memcpy(expected[body].data()+0x1c,&bodies[body].position[0],4);
            memcpy(expected[body].data()+0x24,&bodies[body].position[1],4);
            expected[body][0x27d] = bodies[body].side;
        }
        std::array<uint32_t,10> args{}; args[8] = 0x620000; args[9] = alias ? 0x620000 : 0x621000;
        bus.run(branch ? 0x14b58 : 0x14a5c,branch ? 0x14c50 : 0x14b54,&args);
        if (bus.neighbors != expected || bus.entity != record || bus.globals != globals) {
            std::fprintf(stderr,"collision separation mismatch mode=%u angle=%u\n",mode,angle);
            throw std::runtime_error("collision separation differs from whole i960/TGP memory");
        }
        for (const auto &body : expected) for (auto byte : body) hash = (hash^byte)*16777619u;
        ++cases;
    }
    if (cases != 262144 || hash != 0xf620abb4)
        throw std::runtime_error("collision separation coverage/digest changed");
    std::printf("native_collision_separation PASS cases=%u hash=%08x (both branches/all16-bit angles/alias store ordering; not contact selection)\n",cases,hash);
}

inline void damage_fields(VehicleOracleBus &bus, nm_damage_state_t &s, uint32_t sequence, bool store) {
    const auto copy = [store](void *memory, auto &value) {
        if (store) memcpy(memory,&value,sizeof(value)); else memcpy(&value,memory,sizeof(value));
    };
    damage_entity_fields(bus.entity,s,sequence,store);
    copy(&bus.boundary_last_surface,s.last_surface); copy(&bus.boundary_sequence_counter,s.sequence_counter);
    copy(&bus.boundary_spin_signal,s.signal); copy(&bus.boundary_shake,s.shake_ticks);
}
inline uint32_t check_damage_case(VehicleOracleBus &bus, const nm_contact_pack_t &pack,
        const nm_drive_params_t &math, uint32_t sequence, uint32_t first, uint32_t selected,
        unsigned index, uint32_t hash) {
    nm_damage_state_t s{}; nm_damage_input_t in{};
    const auto get = [&](unsigned at, auto &value) { memcpy(&value,bus.entity.data()+at,sizeof(value)); };
    damage_fields(bus,s,sequence,false);
    get(0x1a8,in.wheel_surface); get(0x48,in.heading); get(0x18,in.speed_index); get(4,in.entity_id);
    in.curve = bus.curve_selector; in.selected = selected;
    memcpy(&in.global_flags,bus.globals.data()+0x90,4);
    VehicleOracleBus result = bus;
    auto auxiliary = bus.auxiliary; const auto globals = bus.globals;
    const int valid = first == 0x16bd4 ? nm_boundary_spin(&math,&pack,&in,&s) : nm_crash_advance(&math,in.entity_id,&s);
    if (!valid) throw std::runtime_error("native damage rejected oracle input");
    damage_fields(result,s,sequence,true);
    const auto &expected = result.entity;
    if (s.auxiliary_request) {
        uint32_t flags; memcpy(&flags,auxiliary.data(),4); flags |= 0x80000000u;
        memcpy(auxiliary.data(),&flags,4); const uint32_t function = 0x5c14; memcpy(auxiliary.data()+12,&function,4);
    }
    std::array<uint32_t,10> args{}; args[1] = selected;
    bus.run(first,first == 0x16bd4 ? 0x16d78 : 0x15fc0,&args);
    if (bus.entity != expected || bus.auxiliary != auxiliary || bus.globals != globals ||
        bus.boundary_last_surface != s.last_surface || bus.boundary_sequence_counter != s.sequence_counter ||
        bus.boundary_spin_signal != (uint16_t)s.signal || bus.boundary_shake != (uint8_t)s.shake_ticks ||
        bus.boundary_sound_mask != s.sound_requests) {
        for (unsigned at = 0; at < expected.size(); ++at) if (expected[at] != bus.entity[at])
            std::fprintf(stderr,"damage first=%x case=%u offset=%03x native=%02x ref=%02x\n",first,index,at,expected[at],bus.entity[at]);
        std::fprintf(stderr,"damage events native=%u ref=%u\n",s.sound_requests,bus.boundary_sound_mask);
        throw std::runtime_error("native damage differs from i960/TGP memory/events");
    }
    for (auto byte : expected) hash = (hash^byte)*16777619u;
    for (auto byte : auxiliary) hash = (hash^byte)*16777619u;
    const uint32_t shared[] = {s.last_surface,s.sequence_counter,(uint16_t)s.signal,(uint8_t)s.shake_ticks,s.sound_requests};
    for (auto value : shared) for (unsigned b = 0; b < 4; ++b) hash = (hash^((value>>(8*b))&255))*16777619u;
    return hash;
}
inline void damage_pre_fields(VehicleOracleBus &bus, nm_damage_pre_state_t &s, uint32_t sequence, bool store) {
    damage_fields(bus,s.damage,sequence,store);
    const auto copy = [store](void *memory, auto &value) {
        if (store) memcpy(memory,&value,sizeof(value)); else memcpy(&value,memory,sizeof(value));
    };
    copy(bus.entity.data()+0x48,s.heading); copy(bus.entity.data()+0xee,s.decay);
    copy(bus.entity.data()+0x134,s.path_offset); copy(bus.entity.data()+0x99,s.countdown);
    copy(bus.damage_camera.data()+16,s.camera_words); copy(bus.damage_camera.data()+40,s.camera_angles);
    copy(bus.damage_camera.data()+3,s.camera_transition); copy(&bus.damage_camera_mode,s.camera_mode);
    copy(&bus.damage_control,s.control); copy(bus.boundary_signal.data(),s.pivot_signal);
    copy(bus.auxiliary.data(),s.auxiliary_flags);
}
inline uint32_t check_damage_pre_case(VehicleOracleBus &bus, const nm_drive_params_t &math,
        uint32_t sequence, unsigned index, uint32_t hash, bool intro=false) {
    nm_damage_pre_state_t s{}; damage_pre_fields(bus,s,sequence,false);
    nm_damage_pre_input_t in{};
    memcpy(&in.global_flags,bus.globals.data()+0x90,4); memcpy(&in.path_flags,bus.entity.data()+0x138,4);
    memcpy(&in.phase,bus.entity.data()+0x128,2); memcpy(&in.yaw,bus.entity.data()+0x58,2);
    in.restored_camera_mode = std::bit_cast<int8_t>(bus.damage_restore_mode);
    in.variant = bus.damage_variant; in.entity_id = bus.entity[4];
    VehicleOracleBus expected = bus;
    if (!(intro ? nm_damage_intro_step(&math,&in,&s) : nm_damage_pre_step(&math,&in,&s)))
        throw std::runtime_error("native damage lifecycle rejected oracle input");
    damage_pre_fields(expected,s,sequence,true);
    if(!intro) bus.run(0x15fc4,0x16010);
    bus.run(0x16014,0x1605c); bus.run(0x15d60,0x15fc0);
    if (bus.entity != expected.entity || bus.globals != expected.globals || bus.auxiliary != expected.auxiliary ||
        bus.damage_camera != expected.damage_camera || bus.damage_camera_mode != expected.damage_camera_mode ||
        bus.damage_control != expected.damage_control || bus.boundary_signal != expected.boundary_signal ||
        bus.boundary_spin_signal != expected.boundary_spin_signal || bus.boundary_last_surface != expected.boundary_last_surface ||
        bus.boundary_sequence_counter != expected.boundary_sequence_counter || bus.boundary_shake != expected.boundary_shake ||
        bus.boundary_sound_mask != s.damage.sound_requests) {
        for (unsigned at = 0; at < expected.entity.size(); ++at) if (expected.entity[at] != bus.entity[at])
            std::fprintf(stderr,"damage lifecycle case=%u offset=%03x native=%02x ref=%02x\n",index,at,expected.entity[at],bus.entity[at]);
        throw std::runtime_error("damage lifecycle differs from complete i960 state");
    }
    const auto bytes = [&](const auto &array) { for (auto byte : array) hash = (hash^byte)*16777619u; };
    bytes(bus.entity); bytes(bus.globals); bytes(bus.auxiliary); bytes(bus.damage_camera); bytes(bus.boundary_signal);
    const uint32_t shared[] = {bus.damage_control,uint32_t(bus.damage_camera_mode),bus.boundary_last_surface,
        bus.boundary_spin_signal,bus.boundary_sound_mask,bus.boundary_shake,bus.boundary_sequence_counter};
    for (auto word : shared) for (unsigned b = 0; b < 4; ++b) hash = (hash^((word>>(8*b))&255))*16777619u;
    return hash;
}
inline void check_damage_pre(rt::M2Board &board, const nm_drive_params_t &math, uint32_t sequence,
        const std::vector<DamagePreCase> *live) {
    std::printf("native_camera_restore_snapshot selected=%d modes=%d/%d/%d/%d (backup RAM descriptors, not ROM constants)\n",
        int(std::bit_cast<int8_t>(board.read_byte(0x501710))),
        int(std::bit_cast<int8_t>(board.read_byte(0x1d00233))),
        int(std::bit_cast<int8_t>(board.read_byte(0x1d00247))),
        int(std::bit_cast<int8_t>(board.read_byte(0x1d0025b))),
        int(std::bit_cast<int8_t>(board.read_byte(0x1d0026f))));
    if (live && live->empty()) throw std::runtime_error("no actual damage lifecycle entries captured");
    const uint32_t word_addresses[] = {0x28b5710,0x28b5714,0x28b5718,0x28b5724,0x28b5728,0x28b5730};
    for (unsigned i = 0; i < 6; ++i) if (math.camera_reset_words[i] != board.read_dword(word_addresses[i]))
        throw std::runtime_error("camera reset words differ from source");
    for (unsigned i = 0; i < 3; ++i) if (uint16_t(math.camera_reset_angles[i]) != board.read_word(0x28b571c+2*i))
        throw std::runtime_error("camera reset angles differ from source");
    VehicleOracleBus bus(board);
    uint32_t hash = 2166136261u;
    unsigned stops = 0, restores = 0, animations = 0;
    uint32_t intro_hash=2166136261u;
    for (unsigned i = 0; i < 65536; ++i) {
        bus.entity.fill(0xa5); bus.globals.fill(0x5a); bus.auxiliary.fill(0xff); bus.damage_camera.fill(0x17);
        bus.boundary_last_surface = 123; bus.boundary_spin_signal = 65535; bus.boundary_signal.fill(255);
        bus.damage_variant = (uint8_t)(i>>8); bus.damage_restore_mode = (uint8_t)i;
        bus.damage_camera[0] = (uint8_t)(i>>4); bus.damage_camera_mode = 256;
        bus.damage_control = 12345;
        bus.write_byte(0x5000ec,(uint8_t)i); bus.write_byte(0x500099,(uint8_t)(i>>8));
        bus.write_word(0x5000ee,(uint16_t)i); bus.write_word(0x500058,(uint16_t)(i*31));
        bus.write_word(0x500128,(uint16_t)(i*73)); bus.write_dword(0x500138,i/2);
        const unsigned row = i%(NM_CRASH_ROWS+1);
        bus.write_word(0x500274,NM_CRASH_ROWS-row); bus.write_dword(0x500278,sequence+4+16*row);
        const int16_t inhibits[] = {-1,0,1,2,29,73,129,INT16_MIN};
        bus.write_word(0x5000c8,(uint16_t)inhibits[(i>>8)%8]);
        bus.write_word(0x500276,(uint16_t)i); bus.write_byte(0x50027c,(uint8_t)(i>>2));
        bus.write_byte(0x500004,(i>>1)&1); bus.write_dword(0x501090,i&1 ? 0x08000000 : 0x00500000);
        VehicleOracleBus intro_bus=bus;
        intro_hash=check_damage_pre_case(intro_bus,math,sequence,i,intro_hash,true);
        hash = check_damage_pre_case(bus,math,sequence,i,hash);
        stops += bus.boundary_last_surface == UINT32_MAX;
        restores += bus.damage_camera_mode != 256;
        animations += row != NM_CRASH_ROWS;
    }
    if (!stops || !restores || !animations) throw std::runtime_error("missing damage lifecycle coverage");
    if (hash != 0xe6a994d6) throw std::runtime_error("damage lifecycle digest mismatch");
    if (intro_hash != 0x4d9b2912) throw std::runtime_error("intro damage prefix digest mismatch");
    std::printf("native_damage_pre PASS cases=65536 stops=%u camera_resets=%u active=%u hash=%08x state=%zu input=%zu bytes\n",
        stops,restores,animations,hash,sizeof(nm_damage_pre_state_t),sizeof(nm_damage_pre_input_t));
    std::printf("native_damage_intro PASS cases=65536 hash=%08x whole16014/15d60, spin countdown preserved\n",intro_hash);
    unsigned actual_stops = 0, actual_active = 0;
    if (live) for (unsigned i = 0; i < live->size(); ++i) {
        const auto &c = (*live)[i];
        bus.entity = c.entity; bus.globals = c.globals; bus.auxiliary = c.auxiliary; bus.damage_camera = c.camera;
        bus.boundary_last_surface = c.last_surface; bus.damage_control = c.control; bus.damage_camera_mode = c.camera_mode;
        memcpy(bus.boundary_signal.data(),&c.pivot_signal,2); bus.boundary_spin_signal = c.spin_signal;
        bus.boundary_shake = c.shake; bus.boundary_sequence_counter = c.sequence_counter;
        bus.damage_variant = c.variant; bus.damage_restore_mode = c.restore_mode;
        (void)check_damage_pre_case(bus,math,sequence,i,2166136261u);
        actual_stops += c.entity[0xec] && !bus.entity[0xec];
        actual_active += c.entity[0x274] || c.entity[0x275];
    }
    if (live) std::printf("native_damage_pre_actual PASS inputs=%zu stops=%u active_animations=%u (post-run entry oracle, not persistent gameplay)\n",
        live->size(),actual_stops,actual_active);
}
inline uint32_t check_damage(rt::M2Board &board, const nm_contact_pack_t &pack, const nm_drive_params_t &math,
        const std::vector<DamagePreCase> *live) {
    const uint32_t directory = board.read_dword(0x2896e58), sequence = board.read_dword(directory+4);
    const uint32_t other = board.read_dword(board.read_dword(0x2896e5c)+4);
    if (!board.read_dword(directory) || board.read_dword(sequence) != NM_CRASH_ROWS || sequence != other)
        throw std::runtime_error("crash sequence selection differs from source");
    for (unsigned curve = 0; curve < 2; ++curve) for (unsigned i = 0; i < 16; ++i) {
        if (math.crash_model[curve][i] != board.read_dword(0x235b5c+4*curve+8*i))
            throw std::runtime_error("crash model catalog keys differ");
        for (unsigned t = 0; t < 2; ++t)
            if (std::bit_cast<uint32_t>(math.crash_threshold[curve][t][i]) != board.read_dword(board.read_dword(0x16d7c+8*t+4*curve)+4*i))
                throw std::runtime_error("crash threshold differs");
    }
    for (unsigned i = 0; i < NM_CRASH_ROWS; ++i) for (unsigned b = 0; b < sizeof(nm_crash_row_t); ++b)
        if (reinterpret_cast<const uint8_t *>(&math.crash_rows[i])[b] != board.read_byte(sequence+8+16*i+b))
            throw std::runtime_error("crash row differs");
    VehicleOracleBus bus(board);
    uint32_t hash = 2166136261u, animation_hash = 2166136261u;
    unsigned starts = 0, sounds[8]{}, animation_sounds[8]{}, duplicate = 0;
    for (unsigned i = 0; i < 65536; ++i) {
        const unsigned id = i%pack.quad_count;
        bus.entity.fill(0xa5); bus.globals.fill(0); bus.auxiliary.fill(0x11);
        bus.curve_selector = (i>>6)&1; bus.boundary_shake = i&1 ? 255 : 0;
        bus.boundary_sequence_counter = (uint8_t)(i>>4); bus.boundary_spin_signal = 65535;
        bus.boundary_last_surface = i&128 ? id : UINT32_MAX; duplicate += i&128 ? 1 : 0;
        bus.write_dword(0x500000,i*173); bus.write_dword(0x500208,0xa0000000u|((i*73)&511));
        bus.write_byte(0x500004,(i>>3)&1); bus.write_word(0x500048,(uint16_t)i);
        bus.write_word(0x5000c8,(uint16_t)((int)((i>>8)%3)-1)); bus.write_byte(0x5000c7,(uint8_t)(i>>8));
        bus.write_dword(0x500278,0x12345678);
        const int32_t high = math.crash_threshold[bus.curve_selector][0][(i>>8)&15];
        const int32_t low = math.crash_threshold[bus.curve_selector][1][(i>>8)&15];
        const int32_t speeds[] = {INT32_MIN,INT32_MAX,high,low,
            high == INT32_MIN ? high : high-1,low == INT32_MIN ? low : low-1};
        bus.write_dword(0x500018,std::bit_cast<uint32_t>(speeds[(i>>4)%6]));
        bus.write_dword(0x501090,i&32 ? 0x08000000 : 0x00500000);
        for (unsigned w = 0; w < 4; ++w) bus.write_dword(0x5001a8+4*w,(i/16)%5 == w ? id : id+pack.quad_count+1);
        const auto before = bus.boundary_sequence_counter;
        hash = check_damage_case(bus,pack,math,sequence,0x16bd4,id,i,hash);
        starts += before != bus.boundary_sequence_counter; ++sounds[bus.boundary_sound_mask];
    }
    bus.boundary_last_surface = UINT32_MAX; bus.boundary_sequence_counter = 0;
    bus.boundary_shake = 0; bus.boundary_spin_signal = 0;
    for (unsigned i = 0; i < (NM_CRASH_ROWS+1)*16; ++i) {
        const unsigned row = i%(NM_CRASH_ROWS+1);
        bus.entity.fill(0xa5); bus.globals.fill(0); bus.auxiliary.fill(0x11);
        bus.write_word(0x500274,NM_CRASH_ROWS-row); bus.write_dword(0x500278,sequence+4+16*row);
        bus.write_byte(0x500004,(i/(NM_CRASH_ROWS+1))&1); bus.write_byte(0x50027c,(uint8_t)(i/2));
        const int16_t counts[] = {39,79,INT16_MIN,INT16_MAX};
        bus.write_word(0x500276,(uint16_t)counts[(i/4)%4]);
        bus.write_word(0x5000c8,(uint16_t)(i%131));
        animation_hash = check_damage_case(bus,pack,math,sequence,0x15eb8,0,i,animation_hash);
        ++animation_sounds[bus.boundary_sound_mask];
    }
    if (!starts || !sounds[0] || !sounds[NM_CRASH_SOUND_START] ||
        !animation_sounds[NM_CRASH_SOUND_SMALL] || !animation_sounds[NM_CRASH_SOUND_LARGE])
        throw std::runtime_error("missing damage start/animation sound coverage");
    const uint32_t hashes[] = {0x25f9e231,0xabf8bf4b,0xb479d8a3,0xaead2051};
    if (pack.slot >= 4 || hash != hashes[pack.slot]) throw std::runtime_error("damage spin digest mismatch");
    if (animation_hash != 0xf9bd0345) throw std::runtime_error("damage animation digest mismatch");
    std::printf("native_damage PASS spin=65536 starts=%u duplicate=%u hash=%08x advance=%u hash=%08x state=%zu input=%zu bytes (complete16bd4/15eb8, not outer gameplay)\n",
        starts,duplicate,hash,(NM_CRASH_ROWS+1)*16,animation_hash,sizeof(nm_damage_state_t),sizeof(nm_damage_input_t));
    std::printf("native_damage_animation silent/small/large=%u/%u/%u (sound intents only)\n",
        animation_sounds[0],animation_sounds[NM_CRASH_SOUND_SMALL],animation_sounds[NM_CRASH_SOUND_LARGE]);
    check_damage_pre(board,math,sequence,live);
    return sequence;
}
inline uint32_t check_boundary_recovery_case(VehicleOracleBus &bus, const nm_contact_pack_t &pack,
        const nm_drive_params_t &math, unsigned index, uint32_t hash) {
    nm_wheels_state_t w{}; nm_boundary_recovery_t native{};
    const auto get = [&](unsigned at, auto &value) { memcpy(&value,bus.entity.data()+at,sizeof(value)); };
    get(0x1c,w.position); get(0x30,w.road); get(0x58,w.yaw); get(0x1c2,w.road_heading); get(0x34,native.speed);
    if (w.road >= pack.quad_count || !nm_boundary_recover(&math,pack.quads+w.road,&w,&native))
        throw std::runtime_error("boundary recovery rejected oracle input");
    auto expected = bus.entity; auto globals = bus.globals; auto heights = bus.ground_heights;
    memcpy(expected.data()+0x20,&native.height,4); memcpy(expected.data()+0x34,&native.speed,4);
    memcpy(expected.data()+0x1e0,native.displacement,12); memcpy(expected.data()+0x1ec,&native.rotation,2);
    memcpy(globals.data()+0xb8,&native.hold_ticks,4); memcpy(globals.data()+0xc4,&native.recovery_ticks,4);
    for (unsigned v = 0; v < 4; ++v) memcpy(heights.data()+4*v,&native.height,4);
    bus.run(0x161f8,0x1632c);
    if (bus.entity != expected || bus.globals != globals || bus.ground_heights != heights) {
        for (unsigned at = 0; at < expected.size(); ++at) if (expected[at] != bus.entity[at])
            std::fprintf(stderr,"recovery case=%u offset=%03x native=%02x ref=%02x\n",index,at,expected[at],bus.entity[at]);
        throw std::runtime_error("boundary recovery differs from complete i960/TGP state");
    }
    for (auto byte : expected) hash = (hash^byte)*16777619u;
    for (auto byte : globals) hash = (hash^byte)*16777619u;
    for (auto byte : heights) hash = (hash^byte)*16777619u;
    return hash;
}
inline void check_boundary_recovery(rt::M2Board &board, const nm_contact_pack_t &pack,
        const nm_drive_params_t &math) {
    VehicleOracleBus bus(board);
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < pack.quad_count*16; ++i) {
        const auto &q = pack.quads[i/16];
        bus.entity.fill(0xa5); bus.globals.fill(0x5a); bus.ground_heights.fill(0xff);
        for (unsigned a = 0; a < 3; ++a) {
            const float position = q.recovery_center[a]+float(int((i>>(2*a))%33)-16)*0.5f;
            bus.write_dword(0x50001c+4*a,nm_force_bits(position));
        }
        bus.write_dword(0x500030,i/16);
        bus.write_dword(0x500034,nm_force_bits(i%16 == 0 ? -0.0f : float(int(i%513)-256)*0.125f));
        bus.write_word(0x500058,(uint16_t)(i*4093)); bus.write_word(0x5001c2,(uint16_t)(i*73));
        hash = check_boundary_recovery_case(bus,pack,math,i,hash);
    }
    const uint32_t hashes[] = {0xd6477512,0x897bc980,0x1ff6bebe,0x073e32d8};
    if (pack.slot >= 4 || hash != hashes[pack.slot]) throw std::runtime_error("boundary recovery digest mismatch");
    std::printf("native_boundary_recovery PASS cases=%u hash=%08x state=%zu bytes (complete161f8, host)\n",
        pack.quad_count*16,hash,sizeof(nm_boundary_recovery_t));
}
inline void check_player_offcourse(rt::M2Board &board, const nm_contact_pack_t &pack,
        const nm_drive_params_t &math) {
    VehicleOracleBus bus(board);
    const int32_t timers[] = {INT32_MIN,-1,0,511,512,513,0x18c0,INT32_MAX};
    uint32_t hash = 2166136261u;
    unsigned recovered = 0, cleared = 0, advanced = 0;
    for (unsigned i = 0; i < pack.quad_count*32; ++i) {
        nm_player_t s{};
        const auto &q = pack.quads[i/32];
        for (unsigned a = 0; a < 3; ++a)
            s.boundary.ground.position[a] = q.recovery_center[a]+float(int((i>>(2*a))%33)-16)*0.5f;
        s.boundary.ground.road = i/32;
        s.boundary.ground.yaw = nm_yaw_angle(i*4093);
        s.boundary.ground.road_heading = nm_yaw_angle(i*73);
        s.boundary.ground.combined_mask = i&16 ? uint16_t(1u<<(i%3 == 0 ? 1 : i%3 == 1 ? 9 : 10)) : 0;
        s.boundary.speed = i%16 == 0 ? -0.0f : float(int(i%513)-256)*0.125f;
        s.boundary.hold_ticks = -123; s.boundary.recovery_ticks = 17;
        s.offcourse_ticks = timers[i%8];
        PlayerMemory expected;
        expected.entity.fill(0xa5); expected.globals.fill(0x5a); expected.heights.fill(0xff);
        checkpoint_fields(expected.entity,expected.globals,expected.checkpoint_extra,s.lap,s.checkpoints,false);
        body_angle_fields(expected.entity,s,false); // Preserve the untouched orientation canary.
        memcpy(&s.spin_body_offset,expected.entity.data()+0xc0,2);
        tilt_fields(expected.entity,expected.checkpoint_extra,s,false);
        memcpy(&s.rank_key,expected.entity.data()+0x60,2);
        memcpy(&s.rank_key_high,expected.entity.data()+0x62,2);
        memcpy(&s.response_ticks,expected.entity.data()+0x98,1);
        expected.fields(s,true);
        bus.entity = expected.entity; bus.globals = expected.globals; bus.ground_heights = expected.heights;
        bus.recovery_input_word = i&8 ? 0xfffffff0u : 0xffffffefu;
        if (!nm_player_offcourse_step(&math,&pack,(i&8)!=0,&s))
            throw std::runtime_error("off-course native rejected scratch input");
        expected.fields(s,true);
        bus.run(0x10a14,0x10a30); bus.run(0x10a34,0x10ba8);
        if (bus.entity != expected.entity || bus.globals != expected.globals || bus.ground_heights != expected.heights) {
            for (unsigned at = 0; at < expected.entity.size(); ++at) if (bus.entity[at] != expected.entity[at])
                std::fprintf(stderr,"offcourse case=%u offset=%03x native=%02x ref=%02x\n",i,at,expected.entity[at],bus.entity[at]);
            throw std::runtime_error("off-course differs from whole i960/TGP memory");
        }
        recovered += s.boundary.recovery_ticks == 64;
        cleared += (i&16) != 0;
        advanced += !(i&16) && s.boundary.recovery_ticks != 64;
        for (auto b : expected.entity) hash = (hash^b)*16777619u;
        for (auto b : expected.globals) hash = (hash^b)*16777619u;
        for (auto b : expected.heights) hash = (hash^b)*16777619u;
    }
    const uint32_t hashes[] = {0x646ef893,0xfa53f9df,0x7a3c2c7c,0x0e0f8085};
    if (pack.slot >= 4 || hash != hashes[pack.slot] || recovered != pack.quad_count*11 ||
        cleared != pack.quad_count*16 || advanced != pack.quad_count*5)
        throw std::runtime_error("off-course coverage changed");
    std::printf("native_player_offcourse PASS cases=%u recovered=%u cleared=%u advanced=%u hash=%08x (complete10a14/10a34, all authored quads)\n",
        pack.quad_count*32,recovered,cleared,advanced,hash);
}
inline void boundary_fields(VehicleOracleBus &bus, nm_boundary_state_t &s, uint32_t sequence, bool store) {
    damage_fields(bus,s.damage,sequence,store);
    const auto copy = [store](void *memory, auto &value) {
        if (store) memcpy(memory,&value,sizeof(value)); else memcpy(&value,memory,sizeof(value));
    };
    const auto field = [&](unsigned at, auto &value) { copy(bus.entity.data()+at,value); };
    auto &w = s.ground;
    field(0x1c,w.position); field(0xf4,w.world); field(0x30,w.road); field(0x1a8,w.surface);
    field(0x1a4,w.selected_surface); field(0x1ba,w.mask); field(0x1b8,w.combined_mask);
    field(0x58,w.yaw); field(0x2c,w.roll); field(0x28,w.pitch); field(0x1c2,w.road_heading);
    const unsigned frame_offsets[] = {0x1c8,0x1c4,0x1cc,0x1d0,0x1d4,0x1d8};
    for (unsigned i = 0; i < 6; ++i) field(frame_offsets[i],w.road_frame[i]);
    for (unsigned v = 0; v < 4; ++v) copy(bus.ground_heights.data()+4*(v^1),w.height[v]);
    field(0x34,s.speed); field(0xf0,s.impact_speed); field(0x1e0,s.displacement);
    field(0x48,s.heading); field(0xee,s.rotation); field(0x1ec,s.recovery_rotation); field(0x99,s.condition);
    copy(bus.globals.data()+0xb8,s.hold_ticks); copy(bus.globals.data()+0xc4,s.recovery_ticks);
    copy(bus.boundary_signal.data(),s.pivot_signal); copy(&bus.boundary_center_mask,s.center_mask);
}

inline uint32_t check_boundary_response_case(VehicleOracleBus &bus, const nm_contact_pack_t &pack,
        const nm_drive_params_t &math, uint32_t sequence, unsigned index, bool synthetic,
        uint32_t hash, unsigned &rejected) {
    bus.boundary_complete = true; bus.course_slot = (uint8_t)pack.slot;
    if (synthetic) {
        // Vary inputs not consumed by the prefix, preserving its existing hash.
        bus.globals.fill(0); bus.globals[0x93] = index&16 ? 0x10 : 0;
        bus.curve_selector = index&1; bus.boundary_last_surface = index&8 ? UINT32_MAX : index%pack.quad_count;
        bus.boundary_sequence_counter = (uint8_t)index; bus.boundary_shake = index&4 ? 0 : 3;
        bus.write_dword(0x500018,index%513); bus.write_byte(0x50006d,(uint8_t)(index>>4));
        bus.write_dword(0x500034,nm_force_bits(float(int(index%257)-128)*0.125f));
        bus.write_byte(0x500099,index&2 ? 0 : 1); bus.write_byte(0x500004,index&1);
        bus.write_word(0x5000c8,index&1 ? 0 : 7); bus.write_byte(0x5000c7,(uint8_t)index);
        bus.write_word(0x500058,(uint16_t)(index*73));
        for (unsigned v = 0; v < 4; ++v) {
            const float x = nm_force_float(bus.read_dword(0x50001c))+(v&1 ? 4 : -4);
            const float z = nm_force_float(bus.read_dword(0x500024))+(v<2 ? 6 : -6);
            bus.write_dword(0x5000f4+12*v,nm_force_bits(x));
            bus.write_dword(0x5000f8+12*v,bus.read_dword(0x500020));
            bus.write_dword(0x5000fc+12*v,nm_force_bits(z));
        }
    }
    nm_boundary_state_t s{}; boundary_fields(bus,s,sequence,false);
    const nm_boundary_state_t before = s;
    nm_boundary_input_t in{};
    memcpy(&in.speed_index,bus.entity.data()+0x18,4); memcpy(&in.global_flags,bus.globals.data()+0x90,4);
    in.lap = bus.entity[0x6d]; in.entity_id = bus.entity[4]; in.curve = bus.curve_selector;
    const bool accepted = nm_boundary_step(&pack,&math,&in,&s);
    VehicleOracleBus expected = bus;
    if (accepted) {
        boundary_fields(expected,s,sequence,true);
        if (s.damage.auxiliary_request) {
            uint32_t flags; memcpy(&flags,expected.auxiliary.data(),4); flags |= 0x80000000u;
            memcpy(expected.auxiliary.data(),&flags,4); const uint32_t function = 0x5c14;
            memcpy(expected.auxiliary.data()+12,&function,4);
        }
    }
    bus.run(0x16060,0x16d78);
    hash = (hash^unsigned(accepted))*16777619u;
    if (!bus.boundary_division_valid) {
        if (accepted || memcmp(&before,&s,sizeof(s)) || !synthetic) throw std::runtime_error("boundary DIV0 rejection/actual domain mismatch");
        ++rejected; return hash;
    }
    if (!accepted || bus.entity != expected.entity || bus.globals != expected.globals || bus.auxiliary != expected.auxiliary ||
        bus.ground_heights != expected.ground_heights || bus.boundary_signal != expected.boundary_signal ||
        bus.boundary_center_mask != expected.boundary_center_mask || bus.boundary_last_surface != expected.boundary_last_surface ||
        bus.boundary_spin_signal != expected.boundary_spin_signal || bus.boundary_sequence_counter != expected.boundary_sequence_counter ||
        bus.boundary_shake != expected.boundary_shake || bus.boundary_sound_requests != s.pivot_sound ||
        bus.boundary_sound_mask != s.damage.sound_requests || bus.boundary_action != s.action) {
        for (unsigned at = 0; at < expected.entity.size(); ++at) if (bus.entity[at] != expected.entity[at])
            std::fprintf(stderr,"boundary response case=%u action=%u offset=%03x native=%02x reference=%02x\n",index,bus.boundary_action,at,expected.entity[at],bus.entity[at]);
        std::fprintf(stderr,"boundary response accepted=%u action=%u/%u pivot_sound=%u/%u crash_sound=%u/%u\n",
            accepted,s.action,bus.boundary_action,s.pivot_sound,bus.boundary_sound_requests,s.damage.sound_requests,bus.boundary_sound_mask);
        throw std::runtime_error("native boundary response differs from whole16060 memory/events");
    }
    for (auto v : expected.entity) hash = (hash^v)*16777619u;
    for (auto v : expected.globals) hash = (hash^v)*16777619u;
    for (auto v : expected.auxiliary) hash = (hash^v)*16777619u;
    for (auto v : expected.ground_heights) hash = (hash^v)*16777619u;
    const uint32_t shared[] = {s.center_mask,s.damage.last_surface,(uint16_t)s.pivot_signal,(uint16_t)s.damage.signal,
        (uint8_t)s.damage.shake_ticks,s.damage.sequence_counter,s.action,s.pivot_sound,s.damage.sound_requests};
    for (auto v : shared) for (unsigned b = 0; b < 4; ++b) hash = (hash^((v>>(8*b))&255))*16777619u;
    return hash;
}

inline void check_boundary_selection(rt::M2Board &board, const nm_contact_pack_t &pack,
        const nm_drive_params_t &math, uint32_t grid, uint32_t surface, uint32_t sequence,
        const std::vector<BoundarySelectionCase> *live) {
    if (live && live->empty()) throw std::runtime_error("no actual boundary selection entries captured");
    VehicleOracleBus bus(board);
    bus.contact_grid = grid; bus.contact_surface = surface;
    const uint16_t masks[] = {0,1,2,4,8,16,32,3,6,10,33,0x8000,0x8001,0x8002,0xffff,0x2c};
    unsigned counts[4]{}, actual[4]{};
    unsigned actual_crashes = 0;
    uint32_t hash = 2166136261u;
    uint32_t response_hash = 2166136261u; unsigned response_rejected = 0, response_counts[4]{};
    for (unsigned i = 0; i < 131072+(live ? live->size() : 0); ++i) {
        nm_wheels_state_t w{};
        const unsigned id = i%pack.quad_count;
        const auto &q = pack.quads[id];
        for (unsigned a = 0; a < 3; ++a) w.position[a] = ((q.xyz[0][a]+q.xyz[1][a])+q.xyz[2][a])*0.33333334f;
        int16_t heading = nm_yaw_angle(i);
        w.road = id; w.selected_surface = id;
        bus.entity.fill(0xa5); bus.ground_heights.fill(0); bus.boundary_center_mask = 0xcafe;
        memcpy(bus.entity.data()+0x1c,w.position,12);
        bus.write_dword(0x500030,w.road); bus.write_dword(0x5001a4,w.selected_surface);
        bus.write_word(0x500048,(uint16_t)heading);
        for (unsigned v = 0; v < 4; ++v) {
            w.mask[v] = i < 65536 ? masks[(i>>(4*v))&15] : v == 0 ? 1 : 0;
            w.surface[v] = (id+v)%pack.quad_count;
            w.height[v] = float(int((i>>(2*v))%33)-16)*0.25f;
            bus.write_word(0x5001ba+2*v,w.mask[v]); bus.write_dword(0x5001a8+4*v,w.surface[v]);
            bus.write_dword(0x501100+4*(v^1),nm_force_bits(w.height[v]));
        }
        if (i >= 131072) {
            const auto &input = (*live)[i-131072].before;
            bus.entity = input.entity; bus.ground_heights = input.heights; bus.boundary_center_mask = input.center_mask;
            bus.globals = input.globals; bus.auxiliary = input.auxiliary;
            bus.boundary_last_surface = input.last_surface; bus.boundary_spin_signal = input.spin_signal;
            memcpy(bus.boundary_signal.data(),&input.pivot_signal,2);
            bus.boundary_shake = input.shake; bus.boundary_sequence_counter = input.sequence_counter;
            bus.curve_selector = input.curve;
            memcpy(w.position,bus.entity.data()+0x1c,12); memcpy(w.surface,bus.entity.data()+0x1a8,16);
            memcpy(w.mask,bus.entity.data()+0x1ba,8); memcpy(&heading,bus.entity.data()+0x48,2);
            memcpy(&w.road,bus.entity.data()+0x30,4); memcpy(&w.selected_surface,bus.entity.data()+0x1a4,4);
            for (unsigned v = 0; v < 4; ++v) memcpy(w.height+v,bus.ground_heights.data()+4*(v^1),4);
        }
        nm_boundary_selection_t native{w.position[1],0x12345678,bus.boundary_center_mask,99};
        VehicleOracleBus response = bus;
        auto expected = bus.entity;
        const auto heights = bus.ground_heights;
        if (!nm_boundary_select(&pack,&math,&w,heading,&native)) throw std::runtime_error("boundary selection rejected oracle input");
        memcpy(expected.data()+0x20,&native.height,4);
        const auto cpu = bus.run(0x16060,0x16bd4);
        const unsigned action = cpu.m_IP == 0x161f8 ? NM_BOUNDARY_RECOVER : cpu.m_IP == 0x1638c ? NM_BOUNDARY_PIVOT :
            cpu.m_IP == 0x16bd4 ? NM_BOUNDARY_SPIN : NM_BOUNDARY_NONE;
        if (native.action != action || bus.entity != expected || bus.ground_heights != heights ||
            native.center_mask != bus.boundary_center_mask ||
            ((action == NM_BOUNDARY_PIVOT || action == NM_BOUNDARY_SPIN) && native.selected != cpu.m_r[17])) {
            std::fprintf(stderr,"boundary selection case=%u action=%u/%u selected=%x/%x center=%x/%x height=%08x/%08x pc=%x\n",
                i,native.action,action,native.selected,cpu.m_r[17],native.center_mask,bus.boundary_center_mask,
                nm_force_bits(native.height),bus.read_dword(0x500020),cpu.m_IP);
            throw std::runtime_error("boundary selection differs from i960/TGP prefix");
        }
        response_hash = check_boundary_response_case(response,pack,math,sequence,i,i < 131072,response_hash,response_rejected);
        ++response_counts[response.boundary_action];
        if (i == 131071) {
            const uint32_t hashes[] = {0x8fd254db,0x231712ca,0x3b0b867b,0xecb777f8};
            const unsigned rejected[] = {384,326,266,0};
            if (pack.slot >= 4 || response_hash != hashes[pack.slot] || response_rejected != rejected[pack.slot])
                throw std::runtime_error("whole boundary response digest/domain mismatch");
        }
        if (i >= 131072) {
            const auto &entry = (*live)[i-131072]; const auto &after = entry.after;
            if (!entry.complete || response.entity != after.entity || response.globals != after.globals ||
                response.auxiliary != after.auxiliary || response.ground_heights != after.heights ||
                response.boundary_center_mask != after.center_mask || response.boundary_last_surface != after.last_surface ||
                response.boundary_spin_signal != after.spin_signal || memcmp(response.boundary_signal.data(),&after.pivot_signal,2) ||
                response.boundary_shake != after.shake || response.boundary_sequence_counter != after.sequence_counter ||
                response.curve_selector != after.curve)
                throw std::runtime_error("whole native boundary differs from actual entry/return state");
            ++actual[action];
            if (action == NM_BOUNDARY_RECOVER) (void)check_boundary_recovery_case(bus,pack,math,i,2166136261u);
            if (action == NM_BOUNDARY_SPIN) {
                const auto counter = bus.boundary_sequence_counter;
                (void)check_damage_case(bus,pack,math,sequence,0x16bd4,native.selected,i,2166136261u);
                actual_crashes += counter != bus.boundary_sequence_counter;
            }
            continue;
        }
        ++counts[action];
        for (auto byte : expected) hash = (hash^byte)*16777619u;
        const uint32_t values[] = {native.center_mask,action,
            action == NM_BOUNDARY_PIVOT || action == NM_BOUNDARY_SPIN ? native.selected : 0};
        for (auto value : values) for (unsigned b = 0; b < 4; ++b) hash = (hash^((value>>(8*b))&255))*16777619u;
    }
    for (auto n : counts) if (!n) throw std::runtime_error("boundary selection missing branch coverage");
    const uint32_t expected_hashes[] = {0x517b46a3,0x3f6ebbfc,0xc0250fc8,0xec0463e9};
    if (pack.slot >= 4 || hash != expected_hashes[pack.slot]) throw std::runtime_error("boundary selection digest mismatch");
    std::printf("native_boundary_selection PASS cases=131072 none/recover/pivot/spin=%u/%u/%u/%u hash=%08x state=%zu bytes (prefix only)\n",
        counts[0],counts[1],counts[2],counts[3],hash,sizeof(nm_boundary_selection_t));
    if (live) std::printf("native_boundary_selection_actual inputs=%zu none/recover/pivot/spin=%u/%u/%u/%u (post-run entry oracle)\n",
        live->size(),actual[0],actual[1],actual[2],actual[3]);
    if (live) std::printf("native_boundary_recovery_actual inputs=%u (complete branch after checked prefix; zero means NO COVERAGE)\n",actual[NM_BOUNDARY_RECOVER]);
    if (live) std::printf("native_damage_actual spin=%u crash_starts=%u (complete branch after checked prefix; zero means NO COVERAGE)\n",
        actual[NM_BOUNDARY_SPIN],actual_crashes);
    std::printf("native_boundary_response cases=%zu hash=%08x none/recover/pivot/spin=%u/%u/%u/%u DIV0_rejected=%u actual=%zu state=%zu input=%zu PASS (whole16060; actual entries/returns, not persistent gameplay)\n",
        131072+(live ? live->size() : 0),response_hash,response_counts[0],response_counts[1],response_counts[2],response_counts[3],
        response_rejected,live ? live->size() : 0,sizeof(nm_boundary_state_t),sizeof(nm_boundary_input_t));
}
inline void check_boundary_pivot(rt::M2Board &board, const nm_contact_pack_t &pack, const nm_drive_params_t &math,
                                 const std::vector<BoundaryPivotCase> *live) {
    VehicleOracleBus bus(board);
    bus.course_slot = (uint8_t)pack.slot;
    uint8_t sides[4];
    for (unsigned i = 0; i < 4; ++i) sides[i] = board.read_byte(0x235a34+i);
    if (memcmp(sides,math.pivot_sides,4)) throw std::runtime_error("packed pivot sides differ from source");
    for (unsigned lap = 0; lap < 256; ++lap)
        if (math.pivot_divisor[pack.slot][lap] != board.read_dword(board.read_dword(0x235cfc+4*pack.slot)+4*lap))
            throw std::runtime_error("packed pivot divisor differs from source");
    uint32_t hash = 2166136261u;
    unsigned moved = 0, sounds = 0, side_counts[4]{}, actual_moved = 0;
    const unsigned synthetic = pack.quad_count*16;
    for (unsigned i = 0; i < synthetic+(live ? live->size() : 0); ++i) {
        unsigned id = i < synthetic ? i/16 : 0, side = i%4;
        const auto &q = pack.quads[id];
        nm_wheels_state_t wheels{};
        nm_boundary_pivot_t native{};
        for (unsigned a = 0; a < 3; ++a) native.position[a] = ((q.xyz[0][a]+q.xyz[1][a])+q.xyz[2][a])*0.33333334f;
        for (unsigned v = 0; v < 4; ++v) {
            wheels.surface[v] = 10000+v;
            wheels.world[v][0] = native.position[0] + (v&1 ? 4.0f : -4.0f);
            wheels.world[v][1] = native.position[1];
            wheels.world[v][2] = native.position[2] + (v<2 ? 6.0f : -6.0f);
        }
        wheels.surface[side == 0 ? 0 : side == 1 ? 2 : side == 2 ? 3 : 1] = id;
        native.yaw = nm_yaw_angle(i*73); native.rotation = nm_yaw_angle(i*103);
        native.speed = 0.5f+(i%256)*0.03125f; native.impact_speed = -17;
        native.condition = i&4 ? 1 : 0; native.shake_ticks = i&8 ? -1 : 0;
        native.side = 99; native.override_count = 3; native.signal = -9;
        uint32_t speed_index = i%64;
        bus.entity.fill(0xa5); bus.globals.fill(0); bus.boundary_sound_requests = 0;
        bus.boundary_shake = (uint8_t)native.shake_ticks;
        memcpy(bus.boundary_signal.data(),&native.signal,2);
        memcpy(bus.entity.data()+0x1c,native.position,12); memcpy(bus.entity.data()+0xf4,wheels.world,48);
        memcpy(bus.entity.data()+0x1a8,wheels.surface,16);
        bus.write_dword(0x500018,speed_index); bus.write_dword(0x500034,nm_force_bits(native.speed));
        bus.write_dword(0x5000f0,nm_force_bits(native.impact_speed));
        bus.write_byte(0x50006d,0); bus.write_byte(0x500099,(uint8_t)native.condition);
        bus.write_byte(0x5000ed,native.side); bus.write_byte(0x50015f,native.override_count);
        bus.write_word(0x500058,(uint16_t)native.yaw); bus.write_word(0x5000ee,(uint16_t)native.rotation);
        if (i >= synthetic) {
            const auto &input = (*live)[i-synthetic];
            bus.entity = input.entity; bus.boundary_shake = input.shake;
            memcpy(bus.boundary_signal.data(),&input.signal,2);
            const auto get = [&](unsigned at, auto &value) { memcpy(&value,bus.entity.data()+at,sizeof(value)); };
            get(0x1c,native.position); get(0xf4,wheels.world); get(0x1a8,wheels.surface);
            get(0x34,native.speed); get(0xf0,native.impact_speed); get(0x58,native.yaw); get(0xee,native.rotation);
            get(0x99,native.condition); get(0xed,native.side); get(0x15f,native.override_count); get(0x18,speed_index);
            native.signal = input.signal; memcpy(&native.shake_ticks,&input.shake,1);
            side = native.side;
            if (side >= 4) throw std::runtime_error("invalid pivot side in actual call");
            id = wheels.surface[side == 0 ? 0 : side == 1 ? 2 : side == 2 ? 3 : 1];
            if (id >= pack.quad_count) throw std::runtime_error("invalid actual pivot quad");
        }
        const auto before = native;
        auto expected = bus.entity;
        if (!nm_boundary_pivot_course(&math,&pack,id,&wheels,speed_index,bus.entity[0x6d],&native))
            throw std::runtime_error("native boundary pivot rejected scratch input");
        const auto put = [&](unsigned at, auto value) { memcpy(expected.data()+at,&value,sizeof(value)); };
        memcpy(expected.data()+0x1c,native.position,12);
        put(0x34,native.speed); put(0xf0,native.impact_speed); put(0x58,native.yaw); put(0xee,native.rotation);
        put(0x99,native.condition); put(0xed,native.side); put(0x15f,native.override_count);
        std::array<uint32_t,10> args{}; args[1] = id;
        bus.run(0x1638c,0x16ba8,&args);
        if (bus.entity != expected || memcmp(bus.boundary_signal.data(),&native.signal,2) ||
            bus.boundary_shake != (uint8_t)native.shake_ticks || bus.boundary_sound_requests != native.sound_request) {
            for (unsigned at = 0; at < expected.size(); ++at) if (expected[at] != bus.entity[at])
                std::fprintf(stderr,"pivot case=%u side=%u offset=%03x native=%02x ref=%02x\n",i,side,at,expected[at],bus.entity[at]);
            throw std::runtime_error("native pivot differs from complete i960/TGP state");
        }
        if (i >= synthetic) { actual_moved += memcmp(native.position,before.position,12) != 0; continue; }
        moved += memcmp(native.position,before.position,12) != 0; sounds += native.sound_request; ++side_counts[native.side];
        for (auto byte : expected) hash = (hash^byte)*16777619u;
        for (auto byte : bus.boundary_signal) hash = (hash^byte)*16777619u;
        hash = (hash^bus.boundary_shake)*16777619u; hash = (hash^native.sound_request)*16777619u;
    }
    const uint32_t expected_hashes[4] = {0xd9ae92c1,0x75f05fec,0xcff28fd8,0x584a8b1a};
    if (pack.slot >= 4 || hash != expected_hashes[pack.slot]) throw std::runtime_error("pivot regression digest mismatch");
    std::printf("native_boundary_pivot PASS cases=%u moved=%u sounds=%u sides=%u/%u/%u/%u hash=%08x state=%zu bytes (1638c branch, not full16060)\n",
        synthetic,moved,sounds,side_counts[0],side_counts[1],side_counts[2],side_counts[3],hash,sizeof(nm_boundary_pivot_t));
    if (live && !live->empty()) std::printf("native_boundary_pivot_actual PASS inputs=%zu moved=%u (snapshot at first intersection; post-run1638c oracle, not full16060)\n",live->size(),actual_moved);
}
inline void check_boundary(rt::M2Board &board, const nm_drive_params_t &math,
                           const std::vector<std::array<float,10>> *live) {
    VehicleOracleBus bus(board);
    unsigned hits = 0, rejected = 0, live_hits = 0, nonfinite_hits = 0, nonfinite_misses = 0;
    uint32_t hash = 2166136261u,observable_hash=2166136261u;
    for (unsigned i = 0; i < 65536+(live ? live->size() : 0); ++i) {
        float points[8]; nm_boundary_test_input(i,points);
        nm_boundary_hit_t actual = {{17,-19},0};
        if (i >= 65536) {
            const auto &input = (*live)[i-65536];
            memcpy(points,input.data(),sizeof(points)); memcpy(actual.point,input.data()+8,sizeof(actual.point));
        }
        std::array<uint32_t,10> arguments;
        for (unsigned v = 0; v < 8; ++v) arguments[v] = nm_force_bits(points[v]);
        arguments[8] = nm_force_bits(actual.point[0]); arguments[9] = nm_force_bits(actual.point[1]);
        const auto cpu = bus.run(0x167b4,0x16ba8,&arguments);
        const auto before = actual;
        const int accepted = nm_boundary_intersection(&math,points,&actual);
        const bool finite = std::isfinite(nm_force_float(cpu.m_r[24])) && std::isfinite(nm_force_float(cpu.m_r[25]));
        nonfinite_hits += !finite && bool(cpu.m_AC&2);
        nonfinite_misses += !finite && !(cpu.m_AC&2);
        if (!accepted && memcmp(&actual,&before,sizeof(actual))) throw std::runtime_error("boundary rejection changed output");
        const bool valid = finite || !(cpu.m_AC&2);
        if (accepted != valid || (accepted && (nm_force_bits(actual.point[0]) != cpu.m_r[24] ||
            nm_force_bits(actual.point[1]) != cpu.m_r[25] || actual.hit != bool(cpu.m_AC&2)))) {
            std::fprintf(stderr,"boundary case=%u native=%08x/%08x/%d ref=%08x/%08x/%u valid=%d/%d\n",
                i,nm_force_bits(actual.point[0]),nm_force_bits(actual.point[1]),actual.hit,
                cpu.m_r[24],cpu.m_r[25],bool(cpu.m_AC&2),accepted,valid);
            throw std::runtime_error("native boundary differs from i960/TGP");
        }
        hits += accepted && actual.hit; rejected += !accepted;
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("boundary oracle left output");
        if (i >= 65536) {
            if (!accepted) throw std::runtime_error("boundary rejected actual player inputs");
            live_hits += actual.hit;
            continue;
        }
        const uint32_t values[] = {(uint32_t)accepted,nm_force_bits(actual.point[0]),
            nm_force_bits(actual.point[1]),(uint32_t)actual.hit};
        for (auto value : values) for (unsigned b = 0; b < 4; ++b) hash = (hash^((value>>(8*b))&255))*16777619u;
        const bool source_hit=valid && bool(cpu.m_AC&2);
        const uint32_t observed[]={(uint32_t)valid,(uint32_t)source_hit,
            source_hit ? cpu.m_r[24] : 0,source_hit ? cpu.m_r[25] : 0};
        for(auto value:observed) for(unsigned b=0;b<4;++b) observable_hash=(observable_hash^((value>>(8*b))&255))*16777619u;
    }
    if (!hits || hash != nm_boundary_test_hash(&math) || hash != NM_BOUNDARY_EXPECTED_HASH)
        throw std::runtime_error("boundary digest mismatch");
    std::printf("native_boundary PASS cases=65536 hits=%u rejected=%u hash=%08x exact_i960_tgp_point_and_flag\n",hits-live_hits,rejected,hash);
    std::printf("native_boundary nonfinite_reference_hits=%u nonfinite_misses=%u\n",nonfinite_hits,nonfinite_misses);
    if(observable_hash!=NM_BOUNDARY_OBSERVABLE_HASH || observable_hash!=nm_boundary_observable_test_hash(&math)) throw std::runtime_error("boundary observable/poisoned-state mismatch");
    std::printf("native_boundary_observable PASS source_hash=%08x cases=65536 poisoned_states=131072; raw comparison retained\n",observable_hash);
    if (live && !live->empty()) std::printf("native_boundary_live PASS actual_inputs=%zu hits=%u rejected=0 (post-run oracle, not autonomous gameplay)\n",live->size(),live_hits);
    else if (live) std::puts("native_boundary_live NO COVERAGE (no boundary calls)");
}
inline void check_wheels(rt::M2Board &board, const nm_contact_pack_t &pack, const nm_drive_params_t &math,
                         uint32_t grid, uint32_t surface) {
    VehicleOracleBus bus(board);
    bus.contact_grid = grid; bus.contact_surface = surface;
    for (unsigned v = 0; v < 4; ++v) for (unsigned a = 0; a < 3; ++a)
        if (nm_force_bits(math.wheel_local[v][a]) != board.read_dword(0x234ac4+12*v+4*a))
            throw std::runtime_error("wheel local data differs from source");
    uint32_t transform_hash = 2166136261u, ground_hash = 2166136261u, state_hash = 2166136261u;
    unsigned branches[4] = {};
    auto compare = [&](const auto &before, uint32_t first, const nm_wheels_state_t &native, bool ground) {
        auto expected = before;
        const auto put = [&](unsigned offset, auto value) { memcpy(expected.data()+offset,&value,sizeof(value)); };
        memcpy(expected.data()+0xf4,native.world,48);
        auto heights = bus.ground_heights;
        if (ground) {
            put(0x20,native.position[1]); put(0x28,native.pitch); put(0x2c,native.roll);
            put(0x30,native.road); put(0x1a4,native.selected_surface); put(0x1b8,native.combined_mask);
            put(0x1c2,native.road_heading);
            const unsigned offsets[] = {0x1c8,0x1c4,0x1cc,0x1d0,0x1d4,0x1d8};
            for (unsigned i = 0; i < 6; ++i) put(offsets[i],native.road_frame[i]);
            for (unsigned v = 0; v < 4; ++v) {
                put(0x1a8+4*v,native.surface[v]); put(0x1ba+2*v,native.mask[v]);
                memcpy(heights.data()+4*(v^1),native.height+v,4);
            }
            ++branches[(((native.mask[0]|native.mask[1])&33)!=0) |
                       ((((native.mask[1]|native.mask[3])&33)!=0)<<1)];
        }
        bus.run(first,0x11d5c);
        if (bus.entity != expected || bus.ground_heights != heights) {
            for (unsigned i = 0; i < expected.size(); ++i) if (expected[i] != bus.entity[i])
                std::fprintf(stderr,"wheel mismatch first=%x offset=%x actual=%02x expected=%02x\n",first,i,expected[i],bus.entity[i]);
            throw std::runtime_error("native wheel/ground memory differs from i960/TGP");
        }
        uint32_t &hash = ground ? ground_hash : transform_hash;
        for (auto byte : expected) hash = (hash^byte)*16777619u;
        for (auto byte : heights) hash = (hash^byte)*16777619u;
    };
    auto seed = [&](const nm_wheels_state_t &s) {
        bus.entity.fill(0xa5); bus.ground_heights.fill(0);
        for (unsigned a = 0; a < 3; ++a) bus.write_dword(0x50001c+4*a,nm_force_bits(s.position[a]));
        bus.write_word(0x500058,uint16_t(s.yaw)); bus.write_word(0x50002c,uint16_t(s.roll));
        bus.write_word(0x500028,uint16_t(s.pitch)); bus.write_dword(0x500030,s.road);
        for (unsigned v = 0; v < 4; ++v) {
            bus.write_dword(0x501100+4*(v^1),nm_force_bits(s.height[v]));
            bus.write_dword(0x5001a8+4*v,s.surface[v]);
        }
    };
    for (unsigned i = 0; i < 65536; ++i) {
        nm_wheels_state_t s{};
        s.yaw = nm_yaw_angle(i); s.roll = nm_yaw_angle(i*73);
        s.position[0] = (int(i%2048)-1024)*0.5f;
        s.position[1] = i&1 ? -0.0f : float(int(i%100)-50);
        s.position[2] = (int(i%4096)-2048)*0.25f;
        seed(s); const auto before = bus.entity;
        if (!nm_wheels_transform(&math,s.position,s.yaw,s.roll,s.world)) throw std::runtime_error("wheel transform rejected");
        compare(before,0x11bd4,s,false);
    }
    for (unsigned i = 0; i < pack.quad_count*4; ++i) {
        nm_wheels_state_t s; nm_wheels_test_input(&pack,i,&s);
        const auto original = s;
        seed(s); const auto before = bus.entity;
        if (!nm_wheels_ground(&pack,&math,&s)) throw std::runtime_error("wheel ground update rejected");
        compare(before,0x116d0,s,true);
        state_hash = nm_wheels_hash_state(state_hash,&s);
        seed(original);
        bus.write_word(0x500048,(uint16_t)(i*73));
        PlayerMemory memory; memory.entity=bus.entity; memory.heights=bus.ground_heights;
        nm_player_t player{}; memory.fields(player,false);
        if (!nm_player_rolling_ground_step(&math,&pack,&player))
            throw std::runtime_error("rolling ground rejected valid contact");
        memory.fields(player,true);
        bus.run(0xd394,0xd3c8);
        if (bus.entity!=memory.entity || bus.ground_heights!=memory.heights)
            throw std::runtime_error("rolling ground differs from actual caller including saved pitch/roll");
    }
    if (state_hash != nm_wheels_expected_hash(pack.slot) || state_hash != nm_wheels_test_hash(&pack,&math))
        throw std::runtime_error("ground startup digest differs from oracle");
    std::printf("native_wheels state_bytes=%zu hash=%08x\n",sizeof(nm_wheels_state_t),state_hash);
    std::printf("native_rolling_ground cases=%u PASS (d394..d3c8 including whole116d0 and caller angle restoration)\n",pack.quad_count*4);
    std::printf("native_wheels transforms=65536 hash=%08x ground=%u hash=%08x branches=%u/%u/%u/%u PASS (complete116d0, host)\n",
        transform_hash,pack.quad_count*4,ground_hash,branches[0],branches[1],branches[2],branches[3]);
}
inline void check_player_prepare(rt::M2Board &board, const nm_contact_pack_t &pack,
        const nm_drive_params_t &math, uint32_t sequence, uint32_t grid, uint32_t surface) {
    VehicleOracleBus bus(board);
    bus.contact_grid = grid; bus.contact_surface = surface; bus.course_slot = pack.slot;
    if (nm_force_bits(math.speed_index_scale) != board.read_dword(0x28b5740) ||
        nm_force_bits(math.inhibit_speed_drop) != nm_force_bits(0.25f*nm_force_float(board.read_dword(0x28b573c))))
        throw std::runtime_error("early-player coefficients differ from source");
    for (unsigned timer = 0; timer < 256; ++timer) for (unsigned side = 0; side < 2; ++side) {
        const int signed_timer = timer < 128 ? (int)timer : (int)timer-256;
        if (uint16_t(math.shake_angles[side][timer]) != board.read_word(0x235140+64*side+2*signed_timer) ||
            uint16_t(math.inhibit_angles[side][timer]) != board.read_word((side ? 0x23561c : 0x23573c)+2*(144-int(timer))))
            throw std::runtime_error("early-player byte-indexed table differs from source");
    }
    const auto snapshot = [&]() {
        PlayerMemory m;
        m.entity = bus.entity; m.globals = bus.globals; m.crash_auxiliary = bus.auxiliary;
        m.yaw = bus.yaw_globals; m.ahead = bus.lookahead; m.heights = bus.ground_heights;
        m.camera = bus.damage_camera; m.camera_mode = bus.damage_camera_mode; m.control = bus.damage_control;
        m.last_surface = bus.boundary_last_surface; m.center_mask = bus.boundary_center_mask;
        m.profile_updates = bus.guided_profile_updates; m.shake = bus.boundary_shake;
        m.shake_angles = bus.shake_angles;
        m.sequence_counter = bus.boundary_sequence_counter; m.sequence = sequence;
        memcpy(m.signals.data(),bus.boundary_signal.data(),2);
        memcpy(m.signals.data()+2,&bus.boundary_spin_signal,2);
        return m;
    };
    uint32_t hash = 2166136261u;
    unsigned held = 0, animations = 0, sounds = 0;
    for (unsigned test = 0; test < 16384; ++test) {
        const unsigned i = test%8192;
        const bool rolling = test >= 8192;
        nm_player_t s{};
        s.spin_body_offset=nm_yaw_angle(0xa5a5); // Preserve historical scratch canary; now explicitly round-tripped.
        s.offcourse_ticks = 0x5a5a5a5a; // Preserve the pre-existing untouched-global canary.
        nm_wheels_test_input(&pack,i%(pack.quad_count*4),&s.boundary.ground);
        s.boundary.heading = s.boundary.ground.yaw;
        s.boundary.speed = (int(i%601)-100)*0.01f;
        s.speed_index = -123; s.wheel_phase[0] = nm_yaw_angle(i*71); s.wheel_phase[1] = nm_yaw_angle(i*29);
        s.correction_ticks = std::bit_cast<int8_t>((uint8_t)i);
        s.correction[0] = (int(i%13)-6)*0.125f; s.correction[1] = (int(i%17)-8)*0.25f;
        s.boundary.damage.shake_ticks = std::bit_cast<int8_t>((uint8_t)(i>>3));
        s.shake_angles[0] = -300; s.shake_angles[1] = 600;
        s.inhibit_direction = std::bit_cast<int8_t>((uint8_t)(i>>5));
        s.inhibit_angle = nm_yaw_angle(i*37); s.inhibit_scale = 9;
        s.boundary.damage.flags = ((i>>2)&3)<<15 | ((i>>6)&2);
        s.boundary.recovery_ticks = i&1 ? 1+i%21 : 0;
        s.boundary.recovery_rotation = nm_yaw_angle(i*73);
        for (unsigned a = 0; a < 3; ++a) s.boundary.displacement[a] = (int((i>>(3*a))%9)-4)*0.125f;
        s.gear = (i>>2)%4; s.rpm = 1000+int(i%8000); s.filtered_rpm = int(i)-4096;
        s.downshift_rpm = 9000; s.shift_ticks = std::bit_cast<int8_t>((uint8_t)i);
        s.force.rpm_boost = i&32 ? uint32_t(-100) : 100;
        s.brake = 0xa5a5ffff; s.steer = std::bit_cast<int32_t>(i*15485863u);
        s.accel = 65535; s.resistance = 42; s.drag_state = std::bit_cast<int8_t>((uint8_t)(i>>4));
        s.boundary.damage.row = i%(NM_CRASH_ROWS+1);
        s.boundary.damage.remaining = NM_CRASH_ROWS-s.boundary.damage.row;
        const int16_t inhibits[] = {-1,0,1,2,29,73,129,INT16_MIN};
        s.boundary.damage.inhibit_ticks = inhibits[(i>>4)%8];
        s.input_inhibit = std::bit_cast<int8_t>((uint8_t)(i>>3));
        s.boundary.damage.sound_ticks = int16_t(i%128); s.boundary.damage.orientation = (i>>6)&3;
        s.boundary.damage.spin_ticks = (uint8_t)i; s.boundary.damage.override_count = (uint8_t)(i>>5);
        s.boundary.damage.last_surface = UINT32_MAX; s.boundary.rotation = nm_yaw_angle(i*19);
        s.boundary.condition = std::bit_cast<int8_t>((uint8_t)i);
        s.phase[0] = nm_yaw_angle(rolling ? i%256 : i*31); s.path_flags = (uint16_t)(i>>1); s.previous_path_flags = (uint16_t)i;
        s.auxiliary_flags = UINT32_MAX; s.camera_mode = 99; s.camera_transition = 7;
        s.force.tire_angle[1] = nm_yaw_angle(i*37); s.delta[0] = 12; s.lookahead[1] = -15;
        nm_player_active_input_t in{};
        in.dynamics.course_slot = pack.slot; in.dynamics.curve = (i>>1)&1;
        in.dynamics.entity_id = (i>>10)&1; in.dynamics.speed_flags = i&16 ? 0x08000000u : 0;
        in.dynamics.speed_index = i%513;
        in.controls.steer = int(i%255)-127; in.controls.accel = (uint8_t)i; in.controls.brake = (uint8_t)(i>>3);
        in.input_counter = i&4 ? 65 : 64; in.gear_selector = (i>>7)&7;
        in.proximity_disabled = (i>>8)&1; in.variant = (uint8_t)(i>>5);
        in.restored_camera_mode = std::bit_cast<int8_t>((uint8_t)i);
        for (unsigned n = 0; n < 3; ++n) {
            in.neighbor[n][0] = s.boundary.ground.position[0]+float(int((i+7*n)%65)-32);
            in.neighbor[n][1] = s.boundary.ground.position[2]+float(int((i+11*n)%65)-32);
        }
        PlayerMemory initial; initial.sequence = sequence;
        initial.entity.fill(0xa5); initial.globals.fill(0x5a); initial.crash_auxiliary.fill(0x35);
        initial.camera.fill(0x17);
        checkpoint_fields(initial.entity,initial.globals,initial.checkpoint_extra,s.lap,s.checkpoints,false);
        body_angle_fields(initial.entity,s,false);
        tilt_fields(initial.entity,initial.checkpoint_extra,s,false);
        memcpy(&s.rank_key,initial.entity.data()+0x60,2);
        memcpy(&s.rank_key_high,initial.entity.data()+0x62,2);
        memcpy(&s.response_ticks,initial.entity.data()+0x98,1);
        initial.fields(s,true);
        bus.entity = initial.entity; bus.globals = initial.globals; bus.auxiliary = initial.crash_auxiliary;
        bus.yaw_globals = initial.yaw; bus.lookahead = initial.ahead; bus.ground_heights = initial.heights;
        bus.damage_camera = initial.camera; bus.damage_camera_mode = initial.camera_mode; bus.damage_control = initial.control;
        bus.boundary_last_surface = initial.last_surface; bus.boundary_center_mask = initial.center_mask;
        bus.guided_profile_updates = initial.profile_updates; bus.boundary_shake = initial.shake;
        bus.shake_angles = initial.shake_angles;
        bus.checkpoint_memory = initial.checkpoint_extra;
        bus.boundary_sequence_counter = initial.sequence_counter;
        memcpy(bus.boundary_signal.data(),initial.signals.data(),2); memcpy(&bus.boundary_spin_signal,initial.signals.data()+2,2);
        bus.damage_restore_mode = (uint8_t)in.restored_camera_mode; bus.damage_variant = in.variant;
        bus.curve_selector = in.dynamics.curve; bus.gear_input_word = uint32_t(in.gear_selector)<<20;
        bus.proximity_disabled = in.proximity_disabled;
        bus.write_byte(0x500004,in.dynamics.entity_id); bus.write_dword(0x500018,in.dynamics.speed_index);
        bus.write_dword(0x501090,in.dynamics.speed_flags); bus.write_dword(0x5010d0,in.input_counter);
        bus.write_dword(0x501050,in.controls.steer); bus.write_word(0x501054,in.controls.accel);
        bus.write_word(0x501058,in.controls.brake); bus.write_dword(0x500088,0x620000);
        for (unsigned n = 0; n < 3; ++n) {
            bus.neighbors[n].fill(0xa5);
            memcpy(bus.neighbors[n].data()+0x1c,&in.neighbor[n][0],4);
            memcpy(bus.neighbors[n].data()+0x24,&in.neighbor[n][1],4);
            const uint32_t next = 0x620000+((n+1)%3)*0x1000;
            memcpy(bus.neighbors[n].data()+0x88,&next,4);
        }
        auto expected = snapshot(); const auto neighbors = bus.neighbors;
        const bool is_held = !rolling && s.boundary.recovery_ticks > 0;
        if (!rolling) { animations += s.boundary.damage.remaining != 0; held += is_held; }
        int accepted;
        if (is_held) accepted = nm_player_active_step(&math,&pack,nullptr,nullptr,&in,&s,nullptr);
        else {
            if (rolling) nm_player_rolling_controls(&s);
            accepted = nm_player_early_mode(&math,&s,rolling);
            in.dynamics.speed_index = s.speed_index;
            if (accepted) accepted = nm_player_prepare_mode(&math,&in,&s,rolling);
        }
        if (!accepted)
            throw std::runtime_error("native player preparation rejected scratch case");
        expected.fields(s,true);
        if (rolling) bus.run(0xd3f4,0xd434);
        bus.run(0xe5f4,0xe6b4); bus.run(0x15314,0x153c0);
        if (!rolling) bus.run(0x154d0,0x15508);
        bus.run(0x158c8,0x15ae0);
        const unsigned early_sound = bus.early_sound_requests;
        bus.run(0x15fc4,0x16010); bus.run(0x16014,0x1605c); bus.run(0x15d60,0x15fc0);
        const unsigned sound_mask = bus.boundary_sound_mask;
        if (!rolling) bus.run(0xe93c,0xea00);
        const bool manual = !rolling && in.dynamics.curve;
        bus.run(manual ? 0x11e80 : 0x11d60,manual ? 0x12064 : 0x11e7c);
        if (is_held) {
            bus.run(0xd9c0,0xd9e8); bus.run(0xec44,0xecc4);
            bus.run(0x116d0,0x11d5c); bus.run(0x16e10,0x16e54);
        } else if (!rolling) {
            bus.run(0x1132c,0x1136c); bus.run(0x11370,0x11434);
        }
        const auto legacy = snapshot(); // Keep the existing prefix/held-through16e10 digest.
        if (is_held) {
            bus.run(0x11aa4,0x11bd0); bus.run(0xd9f8,0xda14); board.tgp().sync();
            for (unsigned a = 0; a < 3; ++a)
                if (nm_force_bits(s.proximity_circle[a]) != board.tgp().diagnostic_data_word(a == 2 ? 0x223 : 0x220+a))
                    throw std::runtime_error("held chain final circle differs");
        }
        const auto actual = snapshot();
        if (actual != expected || bus.neighbors != neighbors || sound_mask != s.pre_sound_requests || early_sound != s.early_sound_request) {
            for (unsigned at = 0; at < expected.entity.size(); ++at) if (actual.entity[at] != expected.entity[at])
                std::fprintf(stderr,"player prepare case=%u held=%d offset=%03x native=%02x ref=%02x\n",
                    i,is_held,at,expected.entity[at],actual.entity[at]);
            throw std::runtime_error("player preparation/held chain differs from complete i960 memory/events");
        }
        if (rolling) continue;
        sounds += sound_mask != 0;
        const auto bytes = [&](const auto &array) { for (auto b : array) hash = (hash^b)*16777619u; };
        bytes(legacy.entity); bytes(legacy.globals); bytes(legacy.crash_auxiliary); bytes(legacy.camera);
        bytes(legacy.heights); bytes(legacy.signals);
        for (auto angle : legacy.shake_angles) hash = (hash^uint16_t(angle))*16777619u;
        hash = (hash^sound_mask)*16777619u;
        hash = (hash^early_sound)*16777619u;
    }
    const uint32_t expected_hash[] = {0x4f13fb84,0x67498c7b,0xf406c199,0x3ca4c72a};
    if (pack.slot >= 4 || hash != expected_hash[pack.slot] || held != 4096 || animations != 8161 || sounds != 930)
        throw std::runtime_error("player preparation/held coverage digest changed");
    std::printf("native_player_prepare PASS frome5f4 cases=8192 held=%u animations=%u sounds=%u hash=%08x (normal prefix; held through e0a4 including circle, legacy hash through16e10)\n",
        held,animations,sounds,hash);
    std::puts("native_rolling_prepare PASS cases=8192 (d3f4/e5f4/15314/158c8/15fc4/16014/15d60/11d60; shake deferred)");
    const uint32_t speed_bits[] = {0,0x80000000,0x3f000000,0xbf000000,0x3f800000,0xbf800000,
        0x4b000000,0xcb000000,0x4c000000,0xcc000000,0x4f000000,0xcf000000,
        0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc00000,0xffc00000};
    unsigned accepted_edges = 0, rejected_edges = 0;
    for (uint32_t bits : speed_bits) {
        auto initial = snapshot(); nm_player_t s{}; initial.fields(s,false);
        s.boundary.speed = nm_force_float(bits);
        s.input_inhibit = s.correction_ticks = s.boundary.damage.shake_ticks = 0;
        s.boundary.damage.inhibit_ticks = 0; s.boundary.damage.flags = 0;
        initial.fields(s,true); bus.entity = initial.entity; bus.boundary_shake = 0;
        auto expected = snapshot();
        const int accepted = nm_player_early_step(&math,&s);
        if (accepted) { ++accepted_edges; expected.fields(s,true); } else ++rejected_edges;
        bus.run(0xe5f4,0xe6b4);
        if (accepted != bus.early_conversion_valid) throw std::runtime_error("speed-index conversion acceptance differs");
        if (accepted) { bus.run(0x15314,0x153c0); bus.run(0x154d0,0x15508); bus.run(0x158c8,0x15ae0); }
        if (snapshot() != expected) throw std::runtime_error("early-player conversion-edge memory differs");
    }
    std::printf("native_player_early_conversion PASS accepted=%u overflow_rejected=%u (NaN masked, stop before undefined overflow cast)\n",
        accepted_edges,rejected_edges);
}
inline void check_surface_timers(rt::M2Board &board) {
    VehicleOracleBus bus(board);
    uint32_t hash = 2166136261u;
    unsigned changed = 0;
    for (unsigned i = 0; i < 524288; ++i) {
        bus.entity.fill(0xa5);
        const int32_t speed_index = i&0x40000 ? (i&1 ? -1 : 400) : 0;
        const int8_t a = std::bit_cast<int8_t>((uint8_t)i), b = std::bit_cast<int8_t>((uint8_t)(i>>8));
        int8_t ticks[] = {a,b,b,a};
        const uint16_t masks[] = {0xffff,0xffff,uint16_t(0xfff3|((i>>14)&12)),uint16_t(0xfff3|((~i>>14)&12))};
        memcpy(bus.entity.data()+0x18,&speed_index,4); memcpy(bus.entity.data()+0x91,ticks,4);
        memcpy(bus.entity.data()+0x1ba,masks,8);
        auto expected = bus.entity;
        nm_player_surface_timers(speed_index,masks,ticks);
        changed += memcmp(ticks,expected.data()+0x91,4) != 0;
        memcpy(expected.data()+0x91,ticks,4);
        const auto globals = bus.globals;
        bus.run(0x16da4,0x16e0c);
        if (bus.entity != expected || bus.globals != globals) throw std::runtime_error("rear surface timers differ from i960");
        for (auto tick : ticks) hash = (hash^uint8_t(tick))*16777619u;
    }
    if (changed != 1016 || hash != 0x41aa9ac5) throw std::runtime_error("rear surface timers coverage/digest mismatch");
    std::printf("native_surface_timers PASS cases=524288 changed=%u hash=%08x (whole16da4, signed pairs and mask precedence)\n",changed,hash);
}
inline void check_player_end(rt::M2Board &board) {
    VehicleOracleBus bus(board); unsigned transitions = 0, yaw_changes = 0;
    for (unsigned i = 0; i < NM_PLAYER_END_CASES; ++i) {
        nm_player_t s; nm_player_end_test_input(i,&s);
        PlayerMemory expected; expected.entity.fill(0xa5); expected.globals.fill(0x5a); expected.fields(s,true);
        bus.entity = expected.entity; bus.globals = expected.globals;
        const auto auxiliary = bus.auxiliary; const auto old_yaw = s.body_yaw;
        nm_player_end_step(&s); expected.fields(s,true);
        transitions += s.route_transition; yaw_changes += old_yaw != s.body_yaw;
        bus.run(s.held ? 0xd9f8 : 0xd920,s.held ? 0xda14 : 0xd9bc);
        board.tgp().sync(); // Scratch-only, after captured frames; never alter live reference timing.
        if (bus.entity != expected.entity || bus.globals != expected.globals || bus.auxiliary != auxiliary)
            throw std::runtime_error("player final branch differs from whole original memory");
        for (unsigned a = 0; a < 3; ++a)
            if (nm_force_bits(s.proximity_circle[a]) != board.tgp().diagnostic_data_word(a == 2 ? 0x223 : 0x220+a))
                throw std::runtime_error("player final circle differs from original TGP");
    }
    if (transitions != 50673 || yaw_changes != 65532 || nm_player_end_test_hash() != NM_PLAYER_END_HASH)
        throw std::runtime_error("player final branch coverage/digest changed");
    std::printf("native_player_end cases=%u transitions=%u yaw_changes=%u hash=%08x PASS (whole d920/d9f8 through e0a4 and TGP circle)\n",
        NM_PLAYER_END_CASES,transitions,yaw_changes,nm_player_end_test_hash());
}
inline void check_road_marks(rt::M2Board &board) {
    VehicleOracleBus bus(board); unsigned ramps = 0, resets = 0;
    for (unsigned i = 0; i < NM_ROAD_MARKS_CASES; ++i) {
        nm_player_t s; auto expected = bus.road_marks; expected.fill(0xa5);
        nm_road_marks_t marks{expected.data(),0,0}; nm_road_marks_test_input(i,&marks,&s);
        PlayerMemory memory; memory.entity.fill(0xa5); memory.globals.fill(0x5a); memory.fields(s,true);
        bus.entity = memory.entity; bus.globals = memory.globals; bus.road_marks = expected;
        bus.collision_wrap = marks.count; bus.road_mark_timer = marks.timer;
        const auto auxiliary = bus.auxiliary;
        if (!nm_road_marks_step(&marks,(uint16_t)s.phase[0],s.speed_index,s.input_inhibit,s.boundary.damage.inhibit_ticks))
            throw std::runtime_error("road marks rejected scratch input");
        ramps += (uint32_t)s.speed_index < 128; resets += s.input_inhibit || s.boundary.damage.inhibit_ticks;
        bus.run(0x13e3c,0x13ec0);
        if (bus.entity != memory.entity || bus.globals != memory.globals || bus.auxiliary != auxiliary ||
            bus.road_mark_timer != marks.timer || bus.road_marks != expected)
            throw std::runtime_error("road marks differ from whole original memory");
    }
    if (ramps != 24576 || resets != 49152 || nm_road_marks_test_hash() != NM_ROAD_MARKS_HASH)
        throw std::runtime_error("road marks coverage/digest changed");
    std::printf("native_road_marks cases=%u ramps=%u resets=%u hash=%08x PASS (whole13e3c; unsigned speed gate, wrap and aliases)\n",
        NM_ROAD_MARKS_CASES,ramps,resets,nm_road_marks_test_hash());
}
inline void check_tilt(rt::M2Board &board, const nm_drive_params_t &math) {
    for (int phase = -128; phase < 128; ++phase) {
        const int half = phase >= 0 ? phase/2 : -((-phase+1)/2);
        if (uint16_t(nm_player_tilt_sample(&math,34+half)) != board.read_word(0x2351c4+2*half) ||
            uint16_t(nm_player_tilt_sample(&math,66+half)) != board.read_word(0x235204+2*half))
            throw std::runtime_error("surface tilt tables differ from source");
    }
    for (unsigned i = 0; i < 32; ++i)
        if (uint16_t(nm_player_tilt_sample(&math,98+i)) != board.read_word(0x235244+2*i))
            throw std::runtime_error("speed tilt table differs from source");
    for (unsigned i = 0; i < 4; ++i)
        if (uint16_t(math.boost_tilt[i]) != board.read_word(0x235b54+2*i))
            throw std::runtime_error("boost tilt table differs from source");
    VehicleOracleBus bus(board);
    unsigned changed = 0, secondary = 0, phases = 0;
    for (unsigned i = 0; i < NM_TILT_CASES; ++i) {
        nm_player_t s; const uint32_t flags = nm_tilt_test_input(i,&s);
        PlayerMemory expected; expected.entity.fill(0xa5); expected.globals.fill(0x5a);
        expected.checkpoint_extra.fill(0x6b); expected.fields(s,true);
        memcpy(expected.globals.data()+0x90,&flags,4);
        bus.entity = expected.entity; bus.globals = expected.globals;
        bus.checkpoint_memory = expected.checkpoint_extra;
        const auto before = s; const auto auxiliary = bus.auxiliary;
        if (!nm_player_tilt_step(&math,flags,&s)) throw std::runtime_error("tilt rejected scratch input");
        changed += s.tilt[0] != before.tilt[0]; secondary += s.tilt[1] != before.tilt[1];
        phases += s.tilt_phase != before.tilt_phase || s.surface_phase != before.surface_phase;
        expected.fields(s,true);
        bus.run(0x1550c,0x15674);
        if (bus.entity != expected.entity || bus.globals != expected.globals ||
            bus.checkpoint_memory != expected.checkpoint_extra || bus.auxiliary != auxiliary) {
            std::fprintf(stderr,"tilt mismatch case=%u\n",i);
            throw std::runtime_error("tilt differs from whole original memory");
        }
    }
    if (changed != 59069 || secondary != 16760 || phases != 57392 || nm_tilt_test_hash(&math) != NM_TILT_HASH)
        throw std::runtime_error("tilt coverage/digest changed");
    std::printf("native_tilt cases=%u changed=%u secondary=%u phases=%u hash=%08x PASS (whole1550c..15674, signed indices and preserved shared high halfwords)\n",
        NM_TILT_CASES,changed,secondary,phases,nm_tilt_test_hash(&math));
}
inline void check_name_ring_pose(rt::M2Board &board,const nm_drive_params_t &math) {
    constexpr uint32_t actor=0x510000;
    const uint32_t saved[]={board.read_dword(actor+0x38),board.read_dword(actor+0x3c),
        board.read_dword(0x50139c),board.read_dword(0x5010b0)};
    const auto restore=[&] {
        board.write_dword(actor+0x38,saved[0]);board.write_dword(actor+0x3c,saved[1]);
        board.write_dword(0x50139c,saved[2]);board.write_dword(0x5010b0,saved[3]);
    };
    unsigned cases=0;
    try {
        for(unsigned glyph=0;glyph<29;++glyph) for(unsigned timer=0;timer<32;++timer) {
            const uint32_t record=0x23b8f2+32*glyph;
            const float radius=nm_force_float((board.read_dword(record&~3u)>>16)|
                (board.read_dword((record&~3u)+4)<<16));
            const int16_t angle=nm_yaw_angle(board.read_word(record+24));
            board.write_word(actor+0x38,(uint16_t)timer);
            board.tgp().function_port_w(0x10*4,0x1010);
            rt::I960Ref cpu(&board);cpu.m_IP=0x226c30;
            cpu.m_r[11]=record;cpu.m_r[27]=0x880000;cpu.m_r[28]=0x4000;cpu.m_r[29]=actor;
            unsigned steps=0;
            while(cpu.m_IP!=0x226d30) {
                if(++steps>128) throw std::runtime_error("name ring instruction bound");
                cpu.execute_one();
            }
            board.tgp().function_port_w(0x11*4,0x1111);
            float expected[12],native[12];
            for(float &v:expected) v=nm_force_float(board.tgp().fifo_r());
            const float expanded=(float)timer*nm_force_float(0x3cf5c28f)+radius;
            if(!nm_name_ring_pose(&math,expanded,angle,(uint16_t)(timer<<11),native))
                throw std::runtime_error("name ring rejected authored pose");
            for(unsigned i=0;i<12;++i)
                if(!std::isfinite(expected[i]) || std::fabs(expected[i]-native[i])>1e-5f) {
                    std::fprintf(stderr,"name ring glyph=%u timer=%u angle=%d radius=%g component=%u source=%g native=%g\n",
                        glyph,timer,angle,(double)expanded,i,(double)expected[i],(double)native[i]);
                    throw std::runtime_error("name ring differs from original226c30..226d30");
                }
            nm_name_marker_t marker{((int)timer-16)*0.125f,nm_yaw_angle(timer<<11)};
            const uint32_t frame=0xffffffe0u+timer;
            board.write_word(actor+0x3a,(uint16_t)marker.angle);
            board.write_dword(actor+0x3c,nm_force_bits(marker.radius));
            board.write_dword(0x50139c,glyph);board.write_dword(0x5010b0,frame);
            board.tgp().function_port_w(0x10*4,0x1010);
            cpu.m_IP=0x227388;steps=0;
            while(cpu.m_IP!=0x22750c) {
                if(++steps>128) throw std::runtime_error("name marker instruction bound");
                cpu.execute_one();
            }
            if(!nm_name_marker_step(&marker,radius,angle) ||
               (uint16_t)marker.angle!=board.read_word(actor+0x3a) ||
               nm_force_bits(marker.radius)!=board.read_dword(actor+0x3c))
                throw std::runtime_error("name marker smoothing differs from original");
            board.tgp().function_port_w(0x11*4,0x1111);
            for(float &v:expected) v=nm_force_float(board.tgp().fifo_r());
            if(!nm_name_marker_pose(&math,&marker,frame,native))
                throw std::runtime_error("name marker rejected pose");
            for(unsigned i=0;i<12;++i)
                if(!std::isfinite(expected[i]) || std::fabs(expected[i]-native[i])>1e-5f)
                    throw std::runtime_error("name marker matrix differs from original227388..22750c");
            ++cases;
        }
    } catch(...) { restore();throw; }
    restore();
    std::printf("native_name_ring_pose PASS original226c30..226d30 and marker227388..22750c cases=%u each (29 glyphs x32 ticks; not a live name scene)\n",cases);
}

inline void check_player_render_attitude(rt::M2Board &board,const nm_drive_params_t &math) {
    VehicleOracleBus bus(board);
    unsigned cases=0,different=0,opposite_up=0;
    for(int yaw : {0,16384,32768}) for(int road : {0,8192,32768})
    for(int pitch : {0,512}) for(int roll : {-1024,0,1024})
    for(int inhibit : {0,-112}) for(uint32_t overlay : {0u,0x80000000u,0x3e800000u}) {
        nm_player_t car{};
        car.render_angles[0]=int16_t(pitch);car.render_angles[1]=nm_yaw_angle(yaw);
        car.render_angles[2]=int16_t(roll);car.road_yaw=nm_yaw_angle(road);
        car.input_inhibit=int8_t(inhibit);car.inhibit_angle=721;
        car.boundary.damage.overlay_model=overlay;
        car.boundary.damage.angle[0]=321;car.boundary.damage.angle[1]=-514;car.boundary.damage.angle[2]=1023;
        bus.entity.fill(0);
        memcpy(bus.entity.data()+0x4c,car.render_angles,sizeof(car.render_angles));
        memcpy(bus.entity.data()+0x5c,&overlay,4);
        memcpy(bus.entity.data()+0xca,car.boundary.damage.angle,6);
        memcpy(bus.entity.data()+0xd4,&car.input_inhibit,1);
        memcpy(bus.entity.data()+0xd6,&car.inhibit_angle,2);
        board.tgp().function_port_w(0x10*4,0x1010); // Original identity command.
        bus.first_instruction=0x6c40;bus.last_instruction=0x6ed0;
        rt::I960Ref cpu(&bus);
        cpu.m_r[25]=0x500000;cpu.m_r[27]=0x880000;cpu.m_r[28]=0x4000;
        cpu.m_IP=0x6c40;
        unsigned steps=0;
        while(cpu.m_IP!=(overlay ? 0x6d70u : 0x6eccu)) {
            if(++steps>128) throw std::runtime_error("player attitude instruction bound");
            cpu.execute_one();
        }
        float expected[12],native[12],intro[12];
        board.tgp().function_port_w(0x11*4,0x1111);
        for(float &v:expected) v=nm_force_float(board.tgp().fifo_r());
        if(!nm_player_part_pose(&math,&car,0,native) ||
           !nm_intro_body_pose_reference(&math,&car,intro)) throw std::runtime_error("attitude fixture rejected");
        bool differs=false;
        for(unsigned i=0;i<12;++i) {
            if(!std::isfinite(expected[i]) || std::fabs(expected[i]-native[i])>1e-5f)
                throw std::runtime_error("player pose differs from original6c40..6d70/6ecc");
            differs|=std::fabs(expected[i]-intro[i])>1e-5f;
        }
        ++cases;different+=differs;
        // Local +X (one side of the body) tilts upward vs downward.
        opposite_up+=expected[1]*intro[1]<-1e-8f;
        if(overlay) {
            board.tgp().function_port_w(0x10*4,0x1010);
            cpu.m_IP=0x6dc0;steps=0;
            while(cpu.m_IP!=0x6ebc) {
                if(++steps>128) throw std::runtime_error("player shadow instruction bound");
                cpu.execute_one();
            }
            board.tgp().function_port_w(0x11*4,0x1111);
            for(float &v:expected) v=nm_force_float(board.tgp().fifo_r());
            if(!nm_player_part_pose(&math,&car,1,native)) throw std::runtime_error("player shadow fixture rejected");
            for(unsigned i=0;i<12;++i)
                if(!std::isfinite(expected[i]) || std::fabs(expected[i]-native[i])>1e-5f)
                    throw std::runtime_error("player shadow differs from original6dc0..6ebc");
        }
    }
    if(cases!=324 || !different || !opposite_up) throw std::runtime_error("attitude fixture lacks divergence");
    std::printf("native_player_render_attitude PASS original6c40..6d70/6ecc and shadow6dc0..6ebc cases=%u intro-different=%u opposite-side-height=%u (synthetic curb/crash attitude, not observed hardware)\n",
        cases,different,opposite_up);
}

inline void check_body_angles(rt::M2Board &board, const nm_drive_params_t &math) {
    VehicleOracleBus bus(board);
    if (nm_force_bits(math.inertial_scale[0]) != board.read_dword(0x28b576c))
        throw std::runtime_error("body angle scale differs from source");
    unsigned accepted_count = 0, rejected_count = 0;
    for (unsigned i = 0; i < NM_BODY_ANGLES_CASES; ++i) {
        nm_player_t s; int16_t offset; nm_body_angles_test_input(i,&s,&offset);
        PlayerMemory m; m.entity.fill(0xa5); m.globals.fill(0x5a); m.fields(s,true);
        bus.entity = m.entity; bus.globals = m.globals;
        memcpy(bus.checkpoint_memory.data()+0x444,&offset,2);
        const auto before = s; const auto globals = bus.globals; const auto auxiliary = bus.auxiliary;
        const int accepted = nm_player_body_angles_step(&math,offset,&s);
        if (!accepted && memcmp(&s,&before,sizeof(s))) throw std::runtime_error("body angle rejection mutated state");
        m.fields(s,true);
        bus.run(0x11aa4,0x11bd0);
        if (accepted != bus.body_conversion_valid) throw std::runtime_error("body angle conversion acceptance differs");
        if (accepted && (bus.entity != m.entity || bus.globals != globals || bus.auxiliary != auxiliary)) {
            std::fprintf(stderr,"body angle mismatch case=%u\n",i);
            throw std::runtime_error("body angles differ from full original memory");
        }
        if (accepted) ++accepted_count; else ++rejected_count;
    }
    if (accepted_count != 44917 || rejected_count != 20619 || nm_body_angles_test_hash(&math) != NM_BODY_ANGLES_HASH)
        throw std::runtime_error("body angle conversion coverage/digest changed");
    std::printf("native_body_angles cases=%u accepted=%u overflow_rejected=%u hash=%08x PASS (whole11aa4..11bd0, original i960/TGP, not model binding)\n",
        NM_BODY_ANGLES_CASES,accepted_count,rejected_count,nm_body_angles_test_hash(&math));
}

inline void check_opponent_height(rt::M2Board &board, const nm_contact_pack_t &pack,
        const nm_drive_params_t &math, uint32_t grid, uint32_t surface) {
    VehicleOracleBus bus(board);bus.contact_grid=grid;bus.contact_surface=surface;
    for(unsigned i=0;i<44;++i)
        if(nm_force_bits(math.opponent_height_coefficients[i/2][i%2])!=board.read_dword(0xcd30+4*i))
            throw std::runtime_error("opponent height coefficients differ from ROM");
    unsigned snaps=0;
    for(unsigned i=0;i<4096;++i) {
        nm_player_t car{};
        const auto &q=pack.quads[i%pack.quad_count];
        memcpy(car.boundary.ground.position,q.recovery_center,sizeof(q.recovery_center));
        car.boundary.ground.road=i%pack.quad_count;
        car.correction_ticks=(int8_t)(i%32);car.surface_phase=(i&64) ? 1 : 0;
        const uint16_t elapsed[]={1,2,3,4,5,6,65535,0};
        const uint16_t tick=(uint16_t)(i*71);
        nm_opponent_height_t h={(uint16_t)(tick-elapsed[i%8]),q.recovery_center[1],
            0.125f,q.recovery_center[1]+float(int(i%9)-4)*0.25f,float(int(i%7)-3)*0.0625f};
        auto contact=pack;contact.slot=(i/32)%3;bus.course_slot=(uint8_t)contact.slot;
        PlayerMemory expected;expected.entity.fill(0xa5);expected.globals.fill(0x5a);
        expected.fields(car,true);
        const auto fields=[&](const nm_opponent_height_t &v) {
            memcpy(expected.entity.data()+0x27e,&v.sampled_tick,2);
            memcpy(expected.entity.data()+0x280,&v.sampled_height,4);
            memcpy(expected.entity.data()+0x284,&v.sampled_velocity,4);
            memcpy(expected.entity.data()+0x288,&v.reference_height,4);
            memcpy(expected.entity.data()+0xa0,&v.velocity,4);
        };
        fields(h);memcpy(expected.globals.data()+0xb0,&tick,2);
        bus.entity=expected.entity;bus.globals=expected.globals;
        const auto before=car;const auto old_height=h;
        if(!nm_opponent_height_step(&math,&contact,tick,&h,&car))
            throw std::runtime_error("opponent height rejected valid case="+std::to_string(i));
        snaps+=h.reference_height==h.sampled_height;
        expected.fields(car,true);fields(h);
        std::array<uint32_t,10> arguments{};arguments[9]=0x500000;
        bus.run(0xcb60,0xcd20,&arguments);
        if(bus.entity!=expected.entity || bus.globals!=expected.globals) {
            for(unsigned at=0;at<expected.entity.size();++at) if(bus.entity[at]!=expected.entity[at])
                std::fprintf(stderr,"opponent height case=%u offset=%03x native=%02x source=%02x\n",i,at,expected.entity[at],bus.entity[at]);
            throw std::runtime_error("opponent height differs from originalcb60");
        }
        auto invalid=before;auto invalid_height=old_height;invalid.boundary.ground.position[0]=NAN;
        const auto saved=invalid;
        if(nm_opponent_height_step(&math,&contact,tick,&invalid_height,&invalid) ||
           memcmp(&saved,&invalid,sizeof(saved)) || memcmp(&old_height,&invalid_height,sizeof(old_height)))
            throw std::runtime_error("opponent height failure changed state");
    }
    if(!snaps || snaps==4096) throw std::runtime_error("opponent height branch coverage missing");
    std::printf("native_opponent_height PASS cases=4096 snaps=%u (whole entity/globals; originalcb60/contact11620)\n",snaps);
}
inline void check_opponent_road_trigger(rt::M2Board &board, const nm_contact_pack_t &pack,
        const nm_drive_params_t &math) {
    VehicleOracleBus bus(board);
    unsigned branches[4]={};
    const float speeds[]={-1.0f,-0.0f,0.0f,math.collision_speed_step*0.5f,
        math.collision_speed_step,math.collision_speed_step*50.0f,4.0f,16.0f};
    for(unsigned mode=0;mode<256;++mode) {
        const int signed_mode=mode<128 ? (int)mode : (int)mode-256;
        if((uint32_t)math.opponent_road_threshold[0][mode]!=board.read_dword(0x235290+4*signed_mode) ||
           (uint32_t)math.opponent_road_threshold[1][mode]!=board.read_dword(0x235284+4*mode))
            throw std::runtime_error("opponent road thresholds differ from ROM");
        for(unsigned variant=0;variant<128;++variant) {
            const unsigned i=mode*128+variant;
            nm_player_t car{};
            const int32_t a=math.opponent_road_threshold[0][mode],b=math.opponent_road_threshold[1][mode];
            const int32_t indices[]={INT32_MIN,INT32_MAX,a,a==INT32_MAX ? a : a+1,
                b,b==INT32_MAX ? b : b+1,0,260};
            car.speed_index=indices[variant%8];
            car.boundary.ground.combined_mask=variant&8 ? 0xffff : 0xffdf;
            car.boundary.ground.road=i%pack.quad_count;
            car.boundary.ground.road_heading=nm_yaw_angle(i*73);
            car.boundary.heading=nm_yaw_angle(i*4093);
            car.boundary.speed=7.25f;
            car.boundary.damage.flags=0x80000003u|((i&3u)<<15);
            PlayerMemory expected;expected.entity.fill(0xa5);expected.globals.fill(0x5a);
            expected.fields(car,true);expected.entity[0x236]=(uint8_t)mode;
            bus.entity=expected.entity;bus.globals=expected.globals;
            const float player_speed=speeds[variant/16];
            memcpy(bus.neighbors[0].data()+0x34,&player_speed,sizeof(player_speed));
            const auto neighbors=bus.neighbors;
            const bool first=car.speed_index>a && (car.boundary.ground.combined_mask&32);
            const bool second=car.speed_index>b;
            ++branches[first+2*second];
            const auto before=car;
            if(!nm_opponent_road_trigger(&math,&pack,(uint8_t)mode,player_speed,&car))
                throw std::runtime_error("opponent road trigger rejected valid inputs");
            expected.fields(car,true);
            bus.run(0x15678,0x157e4);
            if(bus.entity!=expected.entity || bus.globals!=expected.globals || bus.neighbors!=neighbors) {
                for(unsigned at=0;at<expected.entity.size();++at) if(bus.entity[at]!=expected.entity[at])
                    std::fprintf(stderr,"opponent road case=%u offset=%03x native=%02x source=%02x\n",i,at,expected.entity[at],bus.entity[at]);
                throw std::runtime_error("opponent road trigger differs from original15678");
            }
            if(first || second) {
                auto invalid=before;invalid.boundary.ground.road=pack.quad_count;const auto saved=invalid;
                if(nm_opponent_road_trigger(&math,&pack,(uint8_t)mode,player_speed,&invalid) || memcmp(&saved,&invalid,sizeof(saved)))
                    throw std::runtime_error("invalid opponent road changed state");
            }
            if(second) {
                auto invalid=before;
                if(nm_opponent_road_trigger(&math,&pack,(uint8_t)mode,NAN,&invalid) || memcmp(&before,&invalid,sizeof(before)))
                    throw std::runtime_error("invalid opponent player speed changed state");
            }
        }
    }
    for(auto count:branches) if(!count) throw std::runtime_error("opponent road trigger missing branch coverage");
    unsigned events=0,contacts=0;
    for(unsigned i=0;i<4096;++i) {
        const unsigned variant=i%16;
        nm_player_t car{},player{};
        car.lap.completed=4;car.phase[0]=6;player.phase[0]=1;
        car.speed_index=INT32_MAX;car.boundary.speed=7.25f;player.boundary.speed=4.0f;
        car.boundary.damage.flags=0x80000003;
        car.boundary.ground.road=0;
        car.boundary.heading=nm_yaw_angle(i*4093);
        car.boundary.ground.road_heading=73;
        uint8_t age=(uint8_t)(i/16),mode=0,guided=0;
        const uint8_t slot=(i/32)%4;
        int16_t timer=0;
        switch(variant) {
            case 1: car.boundary.ground.combined_mask=44;break;
            case 2: timer=1;break;
            case 3: guided=3;break;
            case 4: mode=1;break;
            case 5: car.lap.completed=slot ? 1 : 3;break;
            case 6: car.input_inhibit=-1;break;
            case 7: car.boundary.ground.road=2;break;
            case 8: car.phase[0]=1;break;
            case 9: car.phase[0]=13;break;
            case 10: car.boundary.damage.inhibit_ticks=-1;break;
            case 11: car.boundary.ground.combined_mask=44;car.boundary.damage.inhibit_ticks=1;break;
            case 12: car.lap.completed=255;break;
            case 13: timer=INT16_MIN;break;
            case 14: car.phase[0]=2;guided=2;break;
            case 15: car.phase[0]=12;break;
        }
        PlayerMemory expected;expected.entity.fill(0xa5);expected.globals.fill(0x5a);
        expected.fields(car,true);expected.entity[0x235]=age;expected.entity[0x236]=mode;
        bus.entity=expected.entity;bus.globals=expected.globals;
        memcpy(bus.neighbors[0].data()+0x34,&player.boundary.speed,4);
        memcpy(bus.neighbors[0].data()+0x128,&player.phase[0],2);
        const auto neighbors=bus.neighbors;
        bus.guided_mode=guided;bus.course_slot=slot;
        bus.race_short_word=0xa5a50000u|(uint16_t)timer;
        bus.checkpoint_memory.fill(0x5a);
        auto extra=bus.checkpoint_memory;
        auto event_word=bus.race_short_word;
        const auto before=car;const auto old_age=age;
        const int result=nm_opponent_road_gate(&math,&pack,mode,guided,slot,timer,&player,&age,&car);
        if(!result) throw std::runtime_error("opponent road gate rejected valid inputs");
        if(result==2) { ++events;event_word=0xa5a50140u;extra[0x498]=4; }
        contacts+=variant==1;
        expected.fields(car,true);expected.entity[0x235]=age;
        bus.run(0x157e8,0x158c4);
        if(bus.entity!=expected.entity || bus.globals!=expected.globals || bus.neighbors!=neighbors ||
           bus.race_short_word!=event_word || bus.checkpoint_memory!=extra)
            throw std::runtime_error("opponent road gate differs from original157e8 case="+std::to_string(i));
        if(variant==1) {
            auto invalid=before;auto invalid_age=old_age;player.boundary.speed=NAN;
            if(nm_opponent_road_gate(&math,&pack,mode,guided,slot,timer,&player,&invalid_age,&invalid) ||
               memcmp(&before,&invalid,sizeof(before)) || invalid_age!=old_age)
                throw std::runtime_error("opponent road gate late failure changed state/age");
        }
    }
    if(!events || !contacts) throw std::runtime_error("opponent road gate missing branch coverage");
    std::printf("native_opponent_road_gate PASS cases=4096 events=%u contacts=%u (whole entity/globals/neighbors/shared event; original157e8)\n",events,contacts);
    std::printf("native_opponent_road_trigger PASS cases=32768 branches=%u/%u/%u/%u (whole entity/globals/neighbors; original15678)\n",
        branches[0],branches[1],branches[2],branches[3]);
}
inline void check_contact(rt::M2Board &board, const char *path, const char *drive_path,
                          const std::vector<std::array<float,10>> *live_boundary = nullptr,
                          const std::vector<BoundaryPivotCase> *live_pivots = nullptr,
                          const std::vector<BoundarySelectionCase> *live_selections = nullptr,
                          const std::vector<DamagePreCase> *live_damage_pre = nullptr) {
    nm_contact_pack_t pack{};
    if (!nm_contact_load(&pack,path)) throw std::runtime_error("invalid native contact pack");
    struct Cleanup { nm_contact_pack_t &p; ~Cleanup() { nm_contact_unload(&p); } } cleanup{pack};
    std::unique_ptr<nm_drive_params_t,decltype(&std::free)> math(nm_drive_load(drive_path),&std::free);
    if (!math || !board.tgp().booted()) throw std::runtime_error("contact oracle requires math pack and booted TGP");
    check_name_ring_pose(board,*math);
    check_player_render_attitude(board,*math);
    check_body_angles(board,*math);
    check_tilt(board,*math);
    check_road_marks(board);
    check_player_end(board);
    check_collision_separation(board,*math);
    check_collision_reaction(board,*math);
    check_collision_damage(board,*math);
    check_collision_impulse(board,*math);
    check_collision_choice(board,*math);
    check_collision_travel(board);
    check_collision_route(board,*math);
    check_collision_candidate(board,*math);
    check_collision_detection(board,*math);
    check_collision_preparation(board);
    check_collision_walk(board,*math);
    check_traffic_order(board);
    check_traffic_near(board,*math);
    check_race_signals(board,*math);
    auto &tgp = board.tgp();
    const uint32_t desc = board.read_dword(0x3a48+4*pack.slot);
    const uint32_t grid = board.read_dword(board.read_dword(desc+8));
    const uint32_t surface = board.read_dword(board.read_dword(desc+12));
    const uint32_t lists = tgp.mem_r(0x800010+pack.slot);
    const uint32_t quads = tgp.mem_r(0x800020+pack.slot);
    for (unsigned cell = 0; cell < 16384; ++cell) {
        const uint32_t at = lists+board.read_word(grid+2*cell);
        const uint16_t *list = pack.lists+pack.grid[cell];
        for (unsigned i = 0; i <= list[0]; ++i)
            if (list[i] != tgp.mem_r(0x800000+at+i)) throw std::runtime_error("contact candidate order/count differs from source");
    }
    for (unsigned i = 0; i < pack.quad_count; ++i) {
        for (unsigned v = 0; v < 4; ++v) for (unsigned a = 0; a < 3; ++a)
            if (nm_force_bits(pack.quads[i].xyz[v][a]) != tgp.mem_r(0x800000+quads+16*i+3*v+a))
                throw std::runtime_error("contact vertex bits differ from source");
        if (pack.quads[i].surface != board.read_byte(surface+i)) throw std::runtime_error("contact surface differs from source");
        if (pack.quads[i].heading != tgp.mem_r(0x800000+quads+16*i+15))
            throw std::runtime_error("contact heading differs from source");
        if (nm_force_bits(pack.quads[i].recovery_height) != tgp.mem_r(0x800000+quads+16*i+13))
            throw std::runtime_error("contact recovery height differs from source");
        for (unsigned a = 0; a < 3; ++a) {
            const auto &q = pack.quads[i];
            float center = q.xyz[0][a]+q.xyz[1][a]; center += q.xyz[2][a]; center += q.xyz[3][a]; center *= 0.25f;
            if (nm_force_bits(q.recovery_center[a]) != nm_force_bits(center))
                throw std::runtime_error("offline recovery center differs from ordered float sum");
        }
    }
    // Command0d selects the authored course tables. This runs AFTER all frames.
    tgp.function_port_w(0x0d*4,0x0d0d); tgp.fifo_w(pack.slot); tgp.sync();
    check_opponent_height(board,pack,*math,grid,surface);
    check_opponent_road_trigger(board,pack,*math);
    check_wheels(board,pack,*math,grid,surface);
    check_boundary(board,*math,live_boundary);
    check_boundary_pivot(board,pack,*math,live_pivots);
    const uint32_t sequence = check_damage(board,pack,*math,live_damage_pre);
    check_boundary_selection(board,pack,*math,grid,surface,sequence,live_selections);
    check_boundary_recovery(board,pack,*math);
    check_player_offcourse(board,pack,*math);
    check_surface_timers(board);
    check_player_prepare(board,pack,*math,sequence,grid,surface);
    for(unsigned variant=0;variant<3;++variant) {
        const uint32_t intro_sequence=board.read_dword(board.read_dword(0x2896e58+4*variant)+4);
        const unsigned count=board.read_dword(intro_sequence);
        if(count!=(variant==2 ? 347u : 257u)) throw std::runtime_error("intro crash inventory changed");
        std::vector<nm_crash_row_t> rows(count);
        for(unsigned row=0;row<count;++row) for(unsigned b=0;b<sizeof(rows[row]);++b)
            reinterpret_cast<uint8_t *>(&rows[row])[b]=board.read_byte(intro_sequence+8+16*row+b);
        VehicleOracleBus bus(board);
        for(unsigned orientation=0;orientation<4;++orientation) for(unsigned row=0;row<=count;++row) {
            nm_damage_state_t state{};state.remaining=(int16_t)(count-row);state.row=(uint16_t)row;
            state.orientation=(uint8_t)orientation;state.inhibit_ticks=(int16_t)(row%131);
            state.overlay_model=0x12345678;state.flags=0xa5a5a5a5;
            bus.entity.fill(0x5a);bus.globals.fill(0);bus.entity[4]=1;
            damage_entity_fields(bus.entity,state,intro_sequence,true);
            bus.write_dword(0x500278,intro_sequence+4+16*row);
            auto expected=bus.entity;const auto globals=bus.globals;
            if(!nm_crash_advance_rows(rows.data(),count,1,&state)) throw std::runtime_error("intro crash rejected authored row");
            damage_entity_fields(expected,state,intro_sequence,true);
            const uint32_t pointer=intro_sequence+4+16*state.row;memcpy(expected.data()+0x278,&pointer,4);
            bus.run(0x15eb8,0x15fc0);
            if(bus.entity!=expected || bus.globals!=globals)
                throw std::runtime_error("intro crash differs from original15eb8");
            state.remaining=1;state.row=(uint16_t)count;const auto saved=state;
            if(nm_crash_advance_rows(rows.data(),count,1,&state) || memcmp(&saved,&state,sizeof(saved)))
                throw std::runtime_error("intro crash invalid row mutated state");
        }
        std::printf("native_intro_crash PASS variant=%u cases=%u (four orientations, original15eb8 whole entity/globals, invalid row atomic)\n",variant,4*(count+1));
        for(unsigned counter=0;counter<256;++counter)
        for(unsigned trigger=0;trigger<2;++trigger)
        for(unsigned blocked=0;blocked<3;++blocked) {
            nm_damage_state_t state{};
            state.flags=0x80000000u|(trigger<<6);state.sequence_counter=(uint8_t)counter;
            state.inhibit_ticks=blocked==2 ? -1 : 0;
            bus.entity.fill(0x5a);bus.globals.fill(0);bus.entity[4]=1;
            bus.entity[0xd4]=blocked==1 ? 128 : 0;
            bus.boundary_sequence_counter=(uint8_t)counter;
            const uint32_t flags=variant==2 ? 0x10000000 : 8;memcpy(bus.globals.data()+0x90,&flags,4);
            bus.damage_variant=(uint8_t)(variant&1);
            damage_entity_fields(bus.entity,state,intro_sequence,true);
            auto expected=bus.entity;const auto globals=bus.globals;
            if(!nm_intro_crash_step(rows.data(),count,1,blocked==1 ? -128 : 0,&state))
                throw std::runtime_error("intro crash gate rejected source input");
            damage_entity_fields(expected,state,intro_sequence,true);
            bus.run(0x15de0,0x15fc0);
            if(bus.entity!=expected || bus.globals!=globals || bus.boundary_sequence_counter!=state.sequence_counter)
                throw std::runtime_error("intro crash gate differs from original15de0");
        }
        std::printf("native_intro_crash_gate PASS variant=%u cases=1536 original15de0 whole entity/globals/counter, sound cooldown active\n",variant);
    }
    {
        VehicleOracleBus bus(board);bus.contact_grid=grid;bus.contact_surface=surface;
        for(unsigned i=0;i<2048;++i) {
            float input[3];nm_contact_test_input(&pack,i%nm_contact_test_count(&pack),input);
            nm_player_t player{};
            player.boundary.ground.position[0]=input[0];player.boundary.ground.position[1]=input[2];
            player.boundary.ground.position[2]=input[1];player.boundary.ground.road=0x1234;
            player.boundary.ground.combined_mask=0xbeef;player.boundary.damage.initial_inhibit=(uint8_t)i;
            player.correction_ticks=std::bit_cast<int8_t>((uint8_t)(i*37));
            player.correction[0]=float(i%7)*0.125f;player.correction[1]=-player.correction[0];
            player.body_yaw=nm_yaw_angle(i*177);player.boundary.heading=nm_yaw_angle(i*101);
            player.render_roll_offset=nm_yaw_angle(i*997);
            const int16_t other=nm_yaw_angle(i*401);int16_t own=nm_yaw_angle(i*79);
            PlayerMemory memory;memory.entity.fill(0xa5);memory.fields(player,true);
            memcpy(memory.entity.data()+0x212,&other,2);memcpy(memory.entity.data()+0x216,&own,2);
            bus.entity=memory.entity;bus.globals.fill(0x5a);const auto globals=bus.globals;
            if(!nm_intro_pose_step(&pack,math.get(),&player,other,&own)) throw std::runtime_error("intro pose rejected input");
            memory.fields(player,true);memcpy(memory.entity.data()+0x216,&own,2);
            bus.run(0xe78c,0xe7bc);bus.run(0x153c4,0x154cc);bus.run(0xe760,0xe788);
            if(bus.entity!=memory.entity || bus.globals!=globals)
                throw std::runtime_error("intro pose differs from whole e78c/153c4/e760");
            player.boundary.damage.initial_inhibit=1;const auto saved=player;
            if(nm_intro_contact_step(nullptr,math.get(),&player) || memcmp(&saved,&player,sizeof(saved)))
                throw std::runtime_error("intro invalid contact changed timer/state");
            player.correction_ticks=3;player.correction[1]=INFINITY;
            const auto broken=player;const auto old_own=own;
            if(nm_intro_pose_step(&pack,math.get(),&player,other,&own) || memcmp(&broken,&player,sizeof(broken)) || own!=old_own)
                throw std::runtime_error("intro late correction failure published contact");
        }
        std::puts("native_intro_pose PASS cases=2048 (all timer bytes, whole e78c/153c4/e760 entity/globals, atomic late failure)");
    }
    unsigned cases = 0, hits = 0, misses = 0;
    uint32_t hash = 2166136261u;
    auto check = [&](float x, float z, float previous) {
        unsigned cell;
        if (!nm_contact_cell(x,z,&cell)) throw std::runtime_error("invalid contact oracle input");
        // Independent spatial-index oracle, including cvtri and wrapping.
        rt::I960Ref index(&board);
        index.m_IP = 0x115e4; index.m_r[21] = nm_force_bits(x); index.m_r[22] = nm_force_bits(z);
        unsigned instructions = 0;
        while (index.m_IP != 0x1161c) {
            if (++instructions > 14) throw std::runtime_error("contact cell oracle instruction guard");
            index.execute_one();
        }
        if (index.m_r[21] != cell) throw std::runtime_error("native contact cell differs from i960");
        nm_contact_result_t actual{previous,0x1234,0x5678,0xabcdef}, expected = actual;
        tgp.buffer_w(0x1fff0,0xffffffff,0xffffffff);
        tgp.function_port_w(0x36*4,0x3636);
        for (uint32_t word : {0x17ffcu,nm_force_bits(previous),uint32_t(board.read_word(grid+2*cell)),nm_force_bits(x),nm_force_bits(z)})
            tgp.fifo_w(word);
        tgp.sync();
        if (tgp.buffer_r(0x1fff0)) throw std::runtime_error("contact oracle did not complete");
        const uint32_t h = tgp.buffer_r(0x1fff4), id = tgp.buffer_r(0x1fff8);
        if (h) {
            ++hits;
            expected.height = nm_force_float(h); expected.surface = id;
            expected.mask = 1u<<(board.read_byte(surface+id)&15);
            if (expected.mask == 2) expected.road = id;
        } else { ++misses; expected.height = -0.1f; expected.mask = 1; }
        if (!nm_contact_query(&pack,math.get(),x,z,&actual) || memcmp(&actual,&expected,sizeof(actual))) {
            std::fprintf(stderr,"contact mismatch slot=%u case=%u x=%a z=%a previous=%a actual=%08x/%u/%u/%x expected=%08x/%u/%u/%x\n",
                pack.slot,cases,x,z,previous,nm_force_bits(actual.height),actual.road,actual.surface,actual.mask,
                nm_force_bits(expected.height),expected.road,expected.surface,expected.mask);
            throw std::runtime_error("native contact differs from command36");
        }
        for (unsigned char byte : std::bit_cast<std::array<unsigned char,16>>(actual)) hash = (hash^byte)*16777619u;
        ++cases;
    };
    for (unsigned i = 0; i < nm_contact_test_count(&pack); ++i) {
        float input[3]; nm_contact_test_input(&pack,i,input);
        check(input[0],input[1],input[2]);
    }
    if (!hits || !misses) throw std::runtime_error("contact oracle lacks hit/miss coverage");
    if (hash != nm_contact_expected_hash(pack.slot) || hash != nm_contact_test_hash(&pack,math.get()))
        throw std::runtime_error("contact startup digest differs from oracle");
    std::printf("native_contact slot=%u cases=%u hits=%u misses=%u hash=%08x payload=%u max_candidates=%u PASS (host, not suspension/frame timing)\n",
        pack.slot,cases,hits,misses,hash,pack.payload_bytes,pack.max_candidates);
}
}
