#pragma once
#include "check_road.hpp"
#include "../include/intro_math.h"
#include "../tests/path_check.h"
#include "../tests/guided_check.h"
#include "../tests/contact_check.h"
#include <filesystem>

namespace daytona::native {
inline uint32_t check_path_source(rt::M2Board &board, const nm_path_pack_t &pack) {
    const uint32_t desc = board.read_dword(0x3a48+4*pack.slot);
    const uint32_t table = board.read_dword(desc+16);
    const uint32_t source = board.read_dword(table+8*pack.route);
    if (board.read_dword(table+8*pack.route+4) != pack.count) throw std::runtime_error("path count differs from source");
    for (unsigned i = 0; i < pack.records*28; ++i)
        if (reinterpret_cast<const uint8_t *>(pack.rows)[i] != board.read_byte(source+i))
            throw std::runtime_error("path row/guard bits differ from source");
    return source;
}
inline void check_guided(rt::M2Board &board, const nm_path_pack_t &path, const nm_drive_params_t &math) {
    VehicleOracleBus bus(board); bus.path = path;
    if(board.read_dword(0x137d0)!=0xc3) throw std::runtime_error("lane mask differs from original");
    for(unsigned i=0;i<32768;++i) {
        nm_guided_lane_car_t car{},player{},front{},saved{};
        car.id=1;player.id=i%31==0 ? 1 : 0;front.id=2;saved.id=3;
        car.flags=(i&7)<<10;car.flags|=(i&8) ? 16384u : 0;
        if(i&16) car.flags|=8192;
        car.speed=(int)(i%401)-100;player.speed=(int)((i*7)%501)-100;front.speed=(int)((i*13)%401)-100;
        car.lateral=(int16_t)(i*37%2048);player.lateral=(int16_t)(i*71%2048);
        front.lateral=(int16_t)(i*19%2048);saved.lateral=(int16_t)(i*43%2048);
        car.marker=(int16_t)((int)(i%9)-4);car.mode=(int8_t)(i%3);
        front.position[0]=(float)(i%25);saved.position[0]=(float)(i%9);
        front.position[1]=(float)(i%7);saved.position[1]=(float)(i%3);
        const bool remembered=i&32;
        nm_guided_lane_state_t state{123,(int16_t)((int)(i%451)-1),(uint8_t)(remembered ? 3 : 255)};
        const auto put=[](auto &bytes,const nm_guided_lane_car_t &c) {
            bytes.fill(0xa5);memcpy(bytes.data(),&c.flags,4);bytes[4]=c.id;
            memcpy(bytes.data()+0x18,&c.speed,4);memcpy(bytes.data()+0x1c,&c.position[0],4);
            memcpy(bytes.data()+0x24,&c.position[1],4);memcpy(bytes.data()+0x13e,&c.lateral,2);
            memcpy(bytes.data()+0x146,&c.marker,2);memcpy(bytes.data()+0x236,&c.mode,1);
        };
        put(bus.entity,car);put(bus.neighbors[0],player);put(bus.neighbors[1],front);put(bus.neighbors[2],saved);
        bus.write_word(0x500140,(uint16_t)state.target);bus.write_word(0x500142,(uint16_t)state.control);
        bus.write_dword(0x500088,0x621000);bus.write_dword(0x500244,remembered ? 0x622000 : 0);
        bus.guided_profile_updates=(uint8_t)(i%19);
        auto expected=bus.entity;const auto neighbors=bus.neighbors;
        if(!nm_guided_lane_step(&math,&player,&car,&front,remembered ? &saved : nullptr,
            (uint16_t)path.rows[0].lateral,bus.guided_profile_updates,&state)) throw std::runtime_error("native lane rejected input");
        memcpy(expected.data()+0x140,&state.target,2);memcpy(expected.data()+0x142,&state.control,2);
        const uint32_t pointer=state.remembered==255 ? 0 : state.remembered==3 ? 0x622000 : 0x621000;
        memcpy(expected.data()+0x244,&pointer,4);
        std::array<uint32_t,10> args{};args[0]=0x630000;args[9]=0x620000;
        bus.run(0x136e8,0x139d4,&args);
        if(bus.entity!=expected || bus.neighbors!=neighbors) {
            for(unsigned b=0;b<expected.size();++b) if(expected[b]!=bus.entity[b])
                std::fprintf(stderr,"lane case=%u offset=%03x native=%02x source=%02x\n",i,b,expected[b],bus.entity[b]);
            throw std::runtime_error("native lane differs from whole136e8..139d4");
        }
    }
    std::puts("native_guided_lane PASS cases=32768 whole136e8..139d4 with linked/saved neighbors");
    for(unsigned mode=0;mode<256;++mode) for(unsigned rank=0;rank<44;++rank) for(unsigned player_rank=0;player_rank<44;++player_rank) {
        const int16_t progress=nm_yaw_angle(mode*977+rank*8191), player_progress=nm_yaw_angle(mode*337+player_rank*1023);
        bus.entity.fill(0xa5);bus.neighbors[0].fill(0xc3);bus.guided_mode=(uint8_t)mode;
        memcpy(bus.entity.data()+0x60,&progress,2);bus.entity[0x6a]=(uint8_t)rank;
        memcpy(bus.neighbors[0].data()+0x60,&player_progress,2);bus.neighbors[0][0x6a]=(uint8_t)player_rank;
        auto expected=bus.entity;const auto neighbor=bus.neighbors[0];
        float target;
        if(!nm_guided_target(&math,mode,player_progress,progress,player_rank,rank,&target))
            throw std::runtime_error("guided target rejected native domain");
        memcpy(expected.data()+0x23c,&target,4);
        std::array<uint32_t,10> args{};args[9]=0x620000;
        bus.run(0x10dd8,0x10e6c,&args);
        if(bus.entity!=expected || bus.neighbors[0]!=neighbor)
            throw std::runtime_error("guided target differs from original10dd8..10e68");
    }
    for(unsigned mode=0;mode<256;++mode) {
        float target;
        if(!nm_guided_target(&math,mode,0,0,0,0,&target) || nm_force_bits(target)!=nm_force_bits(math.guided_player_target[mode]))
            throw std::runtime_error("general guided target differs from folded player table");
    }
    float invalid=123;
    if(nm_guided_target(&math,256,0,0,0,0,&invalid) || nm_guided_target(&math,0,0,0,44,0,&invalid) ||
       nm_guided_target(&math,0,0,0,0,44,&invalid) || invalid!=123)
        throw std::runtime_error("invalid guided target changed output");
    std::puts("native_guided_target PASS cases=495616 all256 modes/all44x44 ranks, plus self-table parity and invalid inputs");
    const int offsets[]={-32768,-6145,-6144,-3,-1,0,1,3,6144,6145,32767};
    unsigned biased=0;
    for(unsigned i=0;i<65536;++i) for(unsigned branch=0;branch<2;++branch) {
        const int16_t heading=nm_yaw_angle(i), other=nm_yaw_angle(i+(uint32_t)offsets[i%11]);
        const uint32_t flags=branch ? 4096 : 0;
        bus.entity.fill(0xa5);bus.globals.fill(0x5a);bus.neighbors[0].fill(0xc3);
        memcpy(bus.entity.data(),&flags,4);memcpy(bus.entity.data()+0x48,&heading,2);
        memcpy(bus.neighbors[0].data()+0x48,&other,2);
        auto expected=bus.entity;const auto neighbor=bus.neighbors[0];const auto globals=bus.globals;
        const int16_t result=nm_guided_heading_bias(heading,other,flags);
        memcpy(expected.data()+0x48,&result,2);biased+=result!=heading;
        std::array<uint32_t,10> args{};args[9]=0x620000;
        bus.run(0x10f68,0x10fa8,&args);
        if(bus.entity!=expected || bus.neighbors[0]!=neighbor || bus.globals!=globals)
            throw std::runtime_error("guided heading bias differs from original10f68..10fa4");
    }
    if(!biased) throw std::runtime_error("guided heading bias missing changes");
    std::printf("native_guided_heading_bias PASS cases=131072 changed=%u, all signed16 headings/directional limits/wrap edges\n",biased);
    uint32_t hash = 2166136261u;
    unsigned profiled = 0, adjusted = 0, rejected = 0;
    for (unsigned i = 0; i < NM_GUIDED_CHECK_CASES; ++i) {
        nm_guided_player_input_t in; nm_guided_player_state_t s;
        nm_guided_test_input(&path,i,&bus.profile,&in,&s);
        bus.entity.fill(0xa5); bus.guided_mode = in.mode; bus.guided_profile_updates = s.profile_updates;
        const auto put = [&](unsigned at, auto value) { memcpy(bus.entity.data()+at,&value,sizeof(value)); };
        put(0,in.flags); put(0x18,in.speed_index); put(0x34,in.speed); put(0x238,in.scale);
        put(0x138,in.path_flags); put(0x1b8,in.surface); put(0x20d,in.condition);
        put(0x128,uint16_t(0)); put(0x130,uint32_t(0x630000)); put(0x12c,uint16_t(path.count));
        const auto put_state = [&](auto &bytes, const nm_guided_player_state_t &v) {
            const auto p = [&](unsigned at, auto value) { memcpy(bytes.data()+at,&value,sizeof(value)); };
            for (unsigned a = 0; a < 3; ++a) p(0x1c+4*a,v.position[a]);
            p(0x9c,v.delta[0]); p(0xa4,v.delta[1]); p(0x44,v.desired_speed);
            p(0x23c,v.adjustment_target); p(0x240,v.adjustment); p(0x30,v.cursor.road);
            p(0x134,v.cursor.offset); p(0x136,v.ahead_offset);
            memcpy(bytes.data()+0x13e,&v.lateral,8);
            p(0x48,v.heading); p(0x2a,v.body_yaw); p(0x28,v.pitch); p(0x2c,v.roll);
            p(0x26e,v.road_yaw); p(0x18a,v.tire_angle);
        };
        put_state(bus.entity,s); auto expected = bus.entity;
        PlayerMemory adapter_memory;adapter_memory.entity=bus.entity;
        adapter_memory.profile_updates=s.profile_updates;
        nm_player_t player{};adapter_memory.fields(player,false);
        const auto player_before=player;
        nm_player_dynamics_input_t dynamics{};
        dynamics.speed_index=in.speed_index;dynamics.guided_scale=in.scale;dynamics.guided_mode=in.mode;
        const bool adapter_accepted=nm_player_guided_motion(&math,&path,&bus.profile,&dynamics,&player);
        const auto before = s;
        const bool accepted = nm_guided_player_step(&path,&math,&bus.profile,&in,&s);
        if(adapter_accepted!=accepted || (!accepted && memcmp(&player,&player_before,sizeof(player))))
            throw std::runtime_error("guided adapter acceptance/atomicity differs from operation");
        profiled += s.profile_updates != before.profile_updates;
        adjusted += s.adjustment != before.adjustment;
        put_state(expected,s);
        bus.run(0x10ccc,0x1114c);
        hash = (hash^unsigned(accepted))*16777619u;
        if (!bus.guided_conversion_valid) {
            if (!rejected) std::printf("guided first_invalid_conversion case=%u divisor=%08x\n",i,bus.guided_divisor);
            ++rejected;
            if (accepted || memcmp(&s,&before,sizeof(s))) throw std::runtime_error("guided conversion rejection is not atomic");
            hash = nm_guided_hash_state(hash,&s);
            continue;
        }
        if (!accepted) throw std::runtime_error("guided rejected valid reference conversion");
        auto actual=adapter_memory;actual.entity=bus.entity;actual.profile_updates=bus.guided_profile_updates;
        auto projected=actual;projected.fields(player,true);
        if(projected!=actual) throw std::runtime_error("guided player adapter differs from original mapped state");
        if (bus.entity != expected || bus.guided_profile_updates != s.profile_updates) {
            for (unsigned b = 0; b < expected.size(); ++b) if (bus.entity[b] != expected[b])
                std::fprintf(stderr,"guided mismatch case=%u offset=%03x native=%02x reference=%02x\n",i,b,expected[b],bus.entity[b]);
            throw std::runtime_error("guided player differs from whole i960/TGP memory");
        }
        hash = nm_guided_hash_state(hash,&s);
    }
    if (!profiled || !adjusted || hash != nm_guided_test_hash(&path,&math) ||
        hash != nm_guided_expected_hash(path.slot,path.route)) throw std::runtime_error("guided player coverage/hash mismatch");
    std::printf("native_guided_player cases=%u hash=%08x profiled=%u adjusted=%u rejected_conversion=%u state_bytes=%zu PASS (whole10ccc plus player adapter, scratch player identity only)\n",
        NM_GUIDED_CHECK_CASES,hash,profiled,adjusted,rejected,sizeof(nm_guided_player_state_t));
}
inline void check_rolling(rt::M2Board &board, const nm_path_pack_t &path, const nm_drive_params_t &math) {
    VehicleOracleBus bus(board); bus.path=path;
    for(unsigned test=0;test<512;++test) {
        const unsigned i=test%256, id=test<256 ? 0 : 1+i%43;
        nm_guided_player_input_t input; nm_guided_player_state_t guided;
        nm_guided_test_input(&path,i,&bus.profile,&input,&guided);
        nm_player_t player{};player.lateral=guided.lateral;
        player.render_roll_offset=nm_yaw_angle(i*997);
        PlayerMemory memory;memory.entity.fill(0xa5);memory.fields(player,true);
        memory.entity[4]=(uint8_t)id;
        bus.entity=memory.entity;
        float height=-999;
        if(!nm_car_rolling_entry_pose(&bus.profile,id,&player,&height))
            throw std::runtime_error("rolling entry rejected profile");
        memory.fields(player,true);memcpy(memory.entity.data()+0x288,&height,4);
        const uint16_t counter=0;const uint32_t updater=id ? 0xd6f8 : 0xd328;
        if(!id) memcpy(memory.entity.data()+6,&counter,2);
        memcpy(memory.entity.data()+12,&updater,4);
        bus.run(id ? 0xd684 : 0xd2c0,id ? 0xd6f4 : 0xd324);
        if(bus.entity!=memory.entity) throw std::runtime_error("rolling entry pose differs from whole player/opponent entry");
        const auto before=player;const float saved=height;
        if(nm_player_rolling_entry_pose(nullptr,&player,&height) || memcmp(&before,&player,sizeof(player)) || height!=saved)
            throw std::runtime_error("invalid rolling entry changed outputs");
        if(nm_car_rolling_entry_pose(&bus.profile,44,&player,&height) || memcmp(&before,&player,sizeof(player)) || height!=saved)
            throw std::runtime_error("invalid rolling ID changed outputs");
    }
    if(board.read_dword(0x28b574c)!=0x3fa6aaabu)
        throw std::runtime_error("rolling entry speed inventory differs");
    std::puts("native_rolling_entry_pose cases=512 PASS (whole d2c0/d684/e760, all44 IDs, both lanes; speed source028b574c=3fa6aaab)");
    for(unsigned test=0;test<512;++test) {
        const unsigned i=test%256;const bool spinning=test>=256;
        nm_player_t player{};
        nm_player_dynamics_input_t in{};
        in.speed_flags=1u<<22; in.speed_index=player.speed_index=100+(i%200);
        in.curve=(uint8_t)(i&1); player.gear=(i/2)%4;
        player.accel=(uint16_t)i;player.brake=i&8 ? 255 : 0;
        player.rpm=1000+(int)i*10;player.boundary.speed=1.0f+(float)i/256;
        player.boundary.ground.pitch=nm_yaw_angle(i*7);
        player.boundary.ground.roll=nm_yaw_angle(i*3);
        for(auto &mask:player.boundary.ground.mask) mask=2;
        nm_guided_player_input_t guided_input; nm_guided_player_state_t guided;
        nm_guided_test_input(&path,i,&bus.profile,&guided_input,&guided);
        memcpy(player.boundary.ground.position,guided.position,sizeof(guided.position));
        player.path_offset=guided.cursor.offset; player.boundary.ground.road=guided.cursor.road;
        player.lateral=guided.lateral; player.boundary.heading=guided.heading;
        player.boundary.damage.shake_ticks=std::bit_cast<int8_t>((uint8_t)i);
        if(spinning) {
            player.boundary.damage.spin_ticks=(uint8_t)(1+i%32);
            player.boundary.damage.spin_heading=nm_yaw_angle(i*109);
            player.boundary.damage.spin_rotation=nm_yaw_angle(i*997);
            player.boundary.damage.flags=i&1 ? 0x400000u : 0;
        }
        PlayerMemory memory; memory.entity.fill(0xa5);memory.fields(player,true);
        bus.entity=memory.entity;bus.globals.fill(0);bus.auxiliary.fill(0x35);
        bus.write_dword(0x500130,0x630000);bus.write_word(0x50012c,(uint16_t)path.count);
        memory.entity=bus.entity;
        bus.boundary_shake=player.boundary.damage.shake_ticks;bus.shake_angles={};
        bus.curve_selector=in.curve;bus.write_dword(0x501090,in.speed_flags);
        const auto before=player;
        if(!nm_player_rolling_motion_step(&math,&path,&bus.profile,&in,&player))
            throw std::runtime_error("rolling connected motion rejected valid input");
        memory.fields(player,true);
        auto auxiliary=bus.auxiliary;
        if(player.force.trigger_pair) for(unsigned a=0;a<2;++a) {
            auxiliary[16*a+3]|=0x80;const uint32_t target=a ? 0x998c : 0x9970;
            memcpy(auxiliary.data()+16*a+12,&target,4);
        }
        bus.run(0xecc8,0xef7c);bus.run(0xef80,0xf274);
        bus.run(0xf31c,0xf82c);bus.run(0x10338,0x1040c);
        bus.run(0x154d0,0x15508);bus.run(spinning ? 0xeafc : 0x11150,spinning ? 0xebc0 : 0x11300);
        if(bus.entity!=memory.entity || bus.auxiliary!=auxiliary ||
           bus.boundary_shake!=(uint8_t)player.boundary.damage.shake_ticks ||
           bus.shake_angles[0]!=player.shake_angles[0] || bus.shake_angles[1]!=player.shake_angles[1]) {
            for(unsigned b=0;b<memory.entity.size();++b) if(bus.entity[b]!=memory.entity[b])
                std::fprintf(stderr,"rolling chain case=%u offset=%03x native=%02x reference=%02x\n",i,b,memory.entity[b],bus.entity[b]);
            throw std::runtime_error("rolling connected motion differs from six original routines");
        }
        auto invalid=before;
        if(!spinning && (nm_player_rolling_motion_step(&math,&path,nullptr,&in,&invalid) || memcmp(&invalid,&before,sizeof(invalid))))
            throw std::runtime_error("rolling late failure must roll back powertrain and shake");
        invalid.boundary.damage.spin_ticks=33;const auto invalid_spin=invalid;
        if(nm_player_rolling_motion_step(&math,&path,&bus.profile,&in,&invalid) || memcmp(&invalid,&invalid_spin,sizeof(invalid)))
            throw std::runtime_error("rolling invalid spin must fail atomically");
    }
    std::puts("native_rolling_connected_motion cases=512 PASS (powertrain/shake plus256 rolling11150 and256 spineafc; atomic failure)");
    nm_intro_speed_params_t intro{};
    intro.desired_divisor=std::bit_cast<float>(board.read_dword(0x28b57a4));
    intro.drag_offset=std::bit_cast<float>(board.read_dword(0x28b5814));
    intro.minimum_delta=std::bit_cast<float>(board.read_dword(0x28b57b8));
    for(unsigned i=0;i<512;++i) intro.response[i]=std::bit_cast<float>(board.read_dword(0x28b85a0+4*i));
    for(unsigned i=0;i<512;++i) {
        nm_player_t player{};nm_guided_player_input_t in;nm_guided_player_state_t guided;
        nm_guided_test_input(&path,i%256,&bus.profile,&in,&guided);
        memcpy(player.boundary.ground.position,guided.position,sizeof(guided.position));
        player.path_offset=guided.cursor.offset;player.boundary.ground.road=guided.cursor.road;
        player.lateral=guided.lateral;player.boundary.heading=guided.heading;
        player.speed_index=80+i%200;player.boundary.speed=1+float(i%256)/256;
        player.desired_speed=player.boundary.speed+0.25f;player.resistance=float(i%8)/32;
        player.wheel_phase[0]=nm_yaw_angle(i*971);player.wheel_phase[1]=nm_yaw_angle(i*331);
        PlayerMemory memory;memory.entity.fill(0xa5);memory.fields(player,true);
        bus.entity=memory.entity;bus.globals.fill(0);bus.write_dword(0x501090,1u<<3);
        bus.write_dword(0x500130,0x630000);bus.write_word(0x50012c,(uint16_t)path.count);
        memory.entity=bus.entity;const auto globals=bus.globals;const auto before=player;
        const bool leader=i>=256;
        if(!nm_intro_motion_prefix(&math,&intro,&path,&bus.profile,1u<<3,leader,&player))
            throw std::runtime_error("intro connected motion rejected input");
        memory.fields(player,true);
        bus.run(0x11438,0x115e0);bus.run(leader ? 0xe6b8 : 0xe6f4,leader ? 0xe6f0 : 0xe728);
        bus.run(0x11150,0x11300);
        if(bus.entity!=memory.entity || bus.globals!=globals)
            throw std::runtime_error("intro motion prefix differs from original speed/wheels/path");
        auto invalid=before;
        if(nm_intro_motion_prefix(&math,&intro,&path,nullptr,1u<<3,leader,&invalid) || memcmp(&invalid,&before,sizeof(before)))
            throw std::runtime_error("intro late path failure mutated state");
    }
    std::puts("native_intro_motion PASS cases=512 leader/opponent (speed/wheels/path original11438/e6b8/e6f4/11150, full entity/global, atomic failure)");
    unsigned accepted=0,rejected=0;
    for(unsigned i=0;i<NM_GUIDED_CHECK_CASES;++i) {
        nm_guided_player_input_t in; nm_guided_player_state_t s;
        nm_guided_test_input(&path,i,&bus.profile,&in,&s);
        bus.entity.fill(0xa5);
        bus.write_word(0x500128,0);bus.write_dword(0x500130,0x630000);
        bus.write_word(0x50012c,(uint16_t)path.count);bus.write_dword(0x500034,nm_force_bits(in.speed));
        const auto fields=[](auto &bytes,const nm_guided_player_state_t &v) {
            const auto put=[&](unsigned at,auto value){memcpy(bytes.data()+at,&value,sizeof(value));};
            for(unsigned a=0;a<3;++a) put(0x1c+4*a,v.position[a]);
            put(0x9c,v.delta[0]);put(0xa4,v.delta[1]);put(0x30,v.cursor.road);put(0x134,v.cursor.offset);
            memcpy(bytes.data()+0x13e,&v.lateral,8);
            put(0x48,v.heading);put(0x2a,v.body_yaw);put(0x28,v.pitch);put(0x2c,v.roll);put(0x26e,v.road_yaw);
        };
        fields(bus.entity,s);auto expected=bus.entity;const auto before=s;
        const bool ok=nm_rolling_player_step(&path,&math,&bus.profile,in.speed,&s);
        fields(expected,s);bus.run(0x11150,0x11300);
        if(!bus.guided_conversion_valid) {
            if(ok || memcmp(&s,&before,sizeof(s))) throw std::runtime_error("rolling rejection not atomic");
            ++rejected;continue;
        }
        if(!ok || bus.entity!=expected) {
            for(unsigned b=0;b<expected.size();++b) if(bus.entity[b]!=expected[b])
                std::fprintf(stderr,"rolling mismatch case=%u offset=%03x native=%02x reference=%02x\n",i,b,expected[b],bus.entity[b]);
            throw std::runtime_error("rolling movement differs from whole11150");
        }
        ++accepted;
    }
    if(!accepted) throw std::runtime_error("rolling movement missing accepted coverage");
    std::printf("native_rolling_movement accepted=%u rejected=%u PASS (whole11150, host table math; SH4ZAM hardware pending)\n",accepted,rejected);
}
inline void check_path(rt::M2Board &board, const char *file, const char *math_file) {
    nm_path_pack_t pack{};
    if (!nm_path_load(&pack,file)) throw std::runtime_error("invalid native path pack");
    struct Cleanup { nm_path_pack_t &p; ~Cleanup() { nm_path_unload(&p); } } cleanup{pack};
    std::unique_ptr<nm_drive_params_t,decltype(&std::free)> math(nm_drive_load(math_file),&std::free);
    if (!math || !board.tgp().booted()) throw std::runtime_error("path oracle requires math and booted TGP");
    check_path_source(board,pack);
    VehicleOracleBus bus(board); bus.path = pack;
    uint32_t hash = 2166136261u;
    unsigned moved[2]{}, held[2]{};
    for (unsigned i = 0; i < nm_path_test_count(&pack); ++i) {
        nm_path_cursor_t s; float x,z; int reverse;
        nm_path_test_input(&pack,i,&x,&z,&reverse,&s);
        bus.entity.fill(0xa5);
        bus.write_dword(0x500130,0x630000); bus.write_word(0x50012c,pack.count);
        bus.write_word(0x500134,s.offset); bus.write_dword(0x500030,s.road);
        bus.write_dword(0x50001c,nm_force_bits(x)); bus.write_dword(0x500024,nm_force_bits(z));
        auto expected = bus.entity;
        const unsigned before = s.offset;
        if (!nm_path_cursor_step(&pack,math.get(),x,z,reverse,&s)) throw std::runtime_error("native path cursor rejected");
        if (before != s.offset) ++moved[reverse]; else ++held[reverse];
        memcpy(expected.data()+0x134,&s.offset,2); memcpy(expected.data()+0x30,&s.road,4);
        const auto cpu = bus.run(reverse ? 0x10c50 : 0x10bcc, reverse ? 0x10cc8 : 0x10c4c);
        if (expected != bus.entity || (!reverse && cpu.m_r[16] != 0x630000+28*s.matched)) {
            std::fprintf(stderr,"path mismatch slot=%u route=%u case=%u reverse=%d offset=%u native=%u reference=%u\n",
                pack.slot,pack.route,i,reverse,before,s.offset,bus.read_word(0x500134));
            throw std::runtime_error("native path cursor differs from whole i960/TGP operation");
        }
        hash = nm_path_hash_state(hash,&s);
    }
    if (!moved[0] || !moved[1] || !held[0] || !held[1] || hash != nm_path_expected_hash(pack.slot,pack.route) ||
        hash != nm_path_test_hash(&pack,math.get()))
        throw std::runtime_error("path oracle missing branches or startup hash mismatch");
    std::printf("native_path slot=%u route=%u cases=%u hash=%08x forward_move/hold=%u/%u reverse_move/hold=%u/%u payload=%u PASS (cursor only, not guided movement)\n",
        pack.slot,pack.route,nm_path_test_count(&pack),hash,moved[0],held[0],moved[1],held[1],pack.records*28);
    check_guided(board,pack,*math);
    check_rolling(board,pack,*math);
}

inline void check_checkpoints(rt::M2Board &board, const nm_road_pack_t &road) {
    const uint32_t source = board.read_dword(0x2872974+4*road.slot);
    if (board.read_dword(source) != road.checkpoint_count) throw std::runtime_error("checkpoint count mismatch");
    for (unsigned i = 0; i < road.checkpoint_count; ++i)
        if (board.read_word(source+4+4*i) != road.checkpoints[i]) throw std::runtime_error("checkpoint profile mismatch");
    VehicleOracleBus bus(board); bus.first_instruction = 0x132b8;
    unsigned times = 0, reverse = 0, visits = 0, finishes = 0, resets = 0;
    for (unsigned i = 0; i < NM_CHECKPOINT_CASES; ++i) {
        nm_checkpoint_case_t v; nm_checkpoint_test_case(i,&v);
        bus.entity.fill(0xa5); bus.globals.fill(0xa5); bus.checkpoint_memory.fill(0xa5);
        bus.write_word(0x500128,v.current); bus.write_word(0x50012a,v.previous); bus.write_dword(0x500000,v.flags);
        bus.write_dword(0x501434,v.profiles-1); bus.write_dword(0x50143c,0x650000);
        checkpoint_fields(bus.entity,bus.globals,bus.checkpoint_memory,v.lap,v.race,true);
        bus.checkpoint_data[0] = v.count;
        for (unsigned n = 0; n < v.count; ++n) bus.checkpoint_data[n+1] = 0xabcd0000u|v.points[n];
        auto entity = bus.entity; auto globals = bus.globals; auto extra = bus.checkpoint_memory;
        const auto auxiliary = bus.auxiliary; const auto data = bus.checkpoint_data;
        const auto before = v;
        if (!nm_checkpoint_test_step(&v)) throw std::runtime_error("checkpoint native rejected");
        checkpoint_fields(entity,globals,extra,v.lap,v.race,true);
        memcpy(entity.data(),&v.flags,4); memcpy(entity.data()+0x12a,&v.previous,2);
        if (v.write.written) memcpy(extra.data()+0x19c+4*v.write.index,&v.write.ticks,4);
        bus.run(0x132b8,0x134f4);
        if (bus.entity != entity || bus.globals != globals || bus.checkpoint_memory != extra ||
            bus.auxiliary != auxiliary || bus.checkpoint_data != data) {
            std::fprintf(stderr,"checkpoint mismatch case=%u delta=%d count=%u\n",i,(int)before.current-before.previous,before.count);
            throw std::runtime_error("checkpoint differs from whole original memory");
        }
        times += v.write.written; reverse += v.race.reverse != before.race.reverse;
        visits += v.lap.counter != before.lap.counter;
        finishes += v.race.finish_flags != before.race.finish_flags;
        resets += v.race.visited < before.race.visited;
    }
    if (!times || !reverse || !visits || !finishes || !resets || nm_checkpoint_test_hash() != NM_CHECKPOINT_HASH)
        throw std::runtime_error("checkpoint missing branches or changed digest");
    std::printf("native_checkpoints slot=%u count=%u cases=%u hash=%08x time_writes=%u reverse=%u visits=%u finishes=%u resets=%u PASS (whole132b8..134f4 memory)\n",
        road.slot,road.checkpoint_count,NM_CHECKPOINT_CASES,nm_checkpoint_test_hash(),times,reverse,visits,finishes,resets);
}

inline void check_player_road_tail(rt::M2Board &board, const nm_road_pack_t &road, const nm_drive_params_t &math) {
    VehicleOracleBus bus(board); bus.tail_road = &road; bus.collision_wrap = road.profile_count-1;
    uint32_t hash = 2166136261u; unsigned path_changes = 0;
    for (unsigned i = 0; i < 65536; ++i) {
        nm_player_t s; auto bytes = bus.road_marks; bytes.fill(0x6b);
        nm_road_marks_t marks{bytes.data(),0,0}; nm_road_marks_test_input(i,&marks,&s);
        marks.count = road.profile_count-1; s.phase[0] = (int16_t)(i%road.profile_count);
        const auto &lane = road.profiles[s.phase[0]].lanes[(i/road.profile_count)%8];
        s.boundary.ground.position[0] = lane.xyz[0]+float(int(i%129)-64)*0.25f;
        s.boundary.ground.position[2] = lane.xyz[2]-float(int(i%97)-48)*0.125f;
        s.lap.lap = (uint8_t)i; s.path_offset = (uint16_t)(i*137);
        s.boundary.ground.combined_mask = i&0xfff; s.rank_key = -1234;
        PlayerMemory expected; expected.entity.fill(0xa5); expected.globals.fill(0x5a);
        memcpy(&s.rank_key_high,expected.entity.data()+0x62,2);
        memcpy(&s.response_ticks,expected.entity.data()+0x98,1); // Preserve the prior untouched-byte canary/digest.
        memcpy(&s.spin_body_offset,expected.entity.data()+0xc0,2);
        expected.fields(s,true);
        bus.entity = expected.entity; bus.globals = expected.globals; bus.road_marks = bytes; bus.road_mark_timer = marks.timer;
        const auto auxiliary = bus.auxiliary; const auto previous = s.path_offset;
        if (!nm_player_road_tail(&math,&road,&marks,&s)) throw std::runtime_error("authored player road tail rejected");
        expected.fields(s,true); path_changes += s.path_offset != previous;
        bus.run(0x13a14,0x13b24);
        if (!bus.lateral_conversion_valid) throw std::runtime_error("authored lateral conversion invalid");
        bus.run(0x10bac,0x10bc8); bus.run(0x13de0,0x13dfc); bus.run(0x13e3c,0x13ec0);
        if (bus.entity != expected.entity || bus.globals != expected.globals || bus.auxiliary != auxiliary ||
            bus.road_mark_timer != marks.timer || bus.road_marks != bytes)
            throw std::runtime_error("player road tail differs from whole original memory");
        for (auto byte : expected.entity) hash = (hash^byte)*16777619u;
        for (unsigned b = 0; b < 4; ++b) hash = (hash^((marks.timer>>(8*b))&255))*16777619u;
        for (unsigned j = 0; j < marks.count; ++j) hash = (hash^bytes[j])*16777619u;
    }
    const uint32_t hashes[] = {0xfada81f8,0x961ae771,0x6093722e,0x94a53c3f};
    if (road.slot >= 4 || hash != hashes[road.slot] || path_changes != (road.slot == 3 ? 4094u : 4095u))
        throw std::runtime_error("player road tail coverage/digest changed");
    std::printf("native_player_road_tail slot=%u cases=65536 path_changes=%u hash=%08x PASS (whole13a14/10bac/13de0/13e3c)\n",
        road.slot,path_changes,hash);
}
inline void check_checkpoint_awards(rt::M2Board &board, const nm_road_pack_t &road) {
    const auto &p = *road.awards;
    const uint32_t directory = board.read_dword(0x2870304+4*road.slot);
    for (unsigned d = 0; d < 4; ++d) {
        const uint32_t source = board.read_dword(directory+4*d);
        for (unsigned i = 0; i < 257; ++i)
            if (p.ticks[d][i] != board.read_dword(source+4*i)) throw std::runtime_error("award tick table differs");
        for (unsigned a = 0; a < 2; ++a)
            if (p.adjustment[a][d] != board.read_dword(board.read_dword((a ? 0x234f70 : 0x234fb0)+4*road.slot)+4*d))
                throw std::runtime_error("award adjustment table differs");
    }
    for (unsigned i = 0; i < 16; ++i)
        if (p.cue[i] != board.read_dword(board.read_dword(0x234e38+4*road.slot)+4*i) ||
            p.signal[i] != board.read_byte(board.read_dword(0x234e28+4*road.slot)+2*i))
            throw std::runtime_error("award announcement table differs");
    VehicleOracleBus bus(board); bus.course_slot = (uint8_t)road.slot;
    unsigned awards = 0, special = 0; uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_CHECKPOINT_AWARD_CASES; ++i) {
        nm_checkpoint_award_t s; nm_checkpoint_award_input_t in; int16_t signal;
        nm_checkpoint_award_test_input(i,&in,&s,&signal);
        bus.entity.fill(0xa5); bus.globals.fill(0); bus.checkpoint_memory.fill(0x6b);
        bus.checkpoint_memory[0x60] = (uint8_t)road.slot;
        checkpoint_award_fields(bus.entity,bus.globals,bus.checkpoint_memory,s,signal,true);
        memcpy(bus.entity.data(),&in.player_flags,4); memcpy(bus.entity.data()+0x138,&in.path_flags,2);
        memcpy(bus.checkpoint_memory.data()+0x454,&in.blocked,4);
        memcpy(bus.checkpoint_memory.data()+0x680,&in.penalty,4);
        bus.globals[0x74] = in.suppression; bus.award_difficulty = in.difficulty;
        bus.award_table = board.read_dword(directory+4*in.difficulty);
        auto entity = bus.entity; auto globals = bus.globals; auto extra = bus.checkpoint_memory;
        const auto auxiliary = bus.auxiliary;
        if (!nm_checkpoint_award_step(&p,&in,&s,&signal)) throw std::runtime_error("award rejected valid scratch input");
        awards += s.cue_count != 0; special += s.cue_count == 3;
        checkpoint_award_fields(entity,globals,extra,s,signal,true);
        bus.run(0x134f8,0x135dc);
        if (entity != bus.entity || globals != bus.globals || extra != bus.checkpoint_memory || auxiliary != bus.auxiliary ||
            bus.award_cues.size() != s.cue_count || !std::equal(bus.award_cues.begin(),bus.award_cues.end(),s.cues))
            throw std::runtime_error("checkpoint award differs from original whole memory/intents");
        hash = nm_checkpoint_award_hash_state(hash,&s,signal);
    }
    if (awards != 28672 || special != 1792 || hash != nm_checkpoint_award_test_hash(&p) ||
        hash != nm_checkpoint_award_expected_hash(road.slot)) throw std::runtime_error("checkpoint award shared digest/coverage differs");
    std::printf("native_checkpoint_award slot=%u cases=%u awards=%u special=%u hash=%08x PASS (whole134f8/135e0, original ROM tables and silent ordered intents)\n",
        road.slot,NM_CHECKPOINT_AWARD_CASES,awards,special,hash);
}
inline void check_road_pack(rt::M2Board &board, const char *file, const char *math_file,
                           const char *path_directory, const std::vector<RoadSelectionCase> *actual = nullptr) {
    nm_road_pack_t road{}; std::array<nm_path_pack_t,8> paths{};
    struct Cleanup {
        nm_road_pack_t &r; std::array<nm_path_pack_t,8> &p;
        ~Cleanup() { nm_road_unload(&r); for (auto &path : p) nm_path_unload(&path); }
    } cleanup{road,paths};
    if (!nm_road_load(&road,file)) throw std::runtime_error("invalid road pack");
    check_checkpoints(board,road);
    check_checkpoint_awards(board,road);
    std::unique_ptr<nm_drive_params_t,decltype(&std::free)> math(nm_drive_load(math_file),&std::free);
    if (!math || !board.tgp().booted()) throw std::runtime_error("road selection oracle requires math and booted TGP");
    const uint32_t desc = board.read_dword(0x3a48+4*road.slot);
    const uint32_t source_profiles = board.read_dword(desc+20), source_mapping = board.read_dword(desc+4);
    VehicleOracleBus bus(board);
    bus.road_mapping = source_mapping+8; bus.road_flags = board.read_dword(0x2862444+4*road.slot);
    if (board.read_dword(source_profiles)+1 != road.profile_count || board.read_dword(source_profiles+4) != 8 ||
        board.read_dword(source_mapping)+1 != road.profile_count || board.read_dword(source_mapping+4) != road.map_count)
        throw std::runtime_error("road pack counts differ from source");
    for (unsigned i = 0; i < road.profile_count*164; ++i)
        if (reinterpret_cast<const uint8_t *>(road.profiles)[i] != board.read_byte(source_profiles+8+i))
            throw std::runtime_error("road profile bits differ from source");
    uint32_t lateral_hash = 2166136261u;
    unsigned lateral_cases = 0;
    for (unsigned i = 0; i < road.profile_count; ++i) for (const auto &lane : road.profiles[i].lanes)
        for (int offset : {-8,-1,0,1,8}) {
            bus.profile = road.profiles[i]; bus.entity.fill(0xa5);
            const float x = lane.xyz[0]+offset*0.125f, z = lane.xyz[2]-offset*0.25f;
            bus.write_word(0x500128,0); bus.write_dword(0x50001c,nm_force_bits(x)); bus.write_dword(0x500024,nm_force_bits(z));
            auto expected = bus.entity; const auto globals = bus.globals;
            int16_t lateral = -1234;
            if (!nm_road_measure_lateral(math.get(),&road.profiles[i],x,z,&lateral))
                throw std::runtime_error("authored road lateral rejected");
            memcpy(expected.data()+0x13e,&lateral,2);
            bus.run(0x13a14,0x13b24);
            if (!bus.lateral_conversion_valid || bus.entity != expected || bus.globals != globals)
                throw std::runtime_error("authored road lateral differs from whole i960/TGP memory");
            for (auto byte : expected) lateral_hash = (lateral_hash^byte)*16777619u;
            ++lateral_cases;
        }
    const uint32_t lateral_hashes[] = {0xf3be6cd6,0x58ecf2d7,0xd9d4755b,0x6bfe706e};
    if (lateral_hash != lateral_hashes[road.slot]) throw std::runtime_error("authored road lateral digest changed");
    std::printf("native_road_lateral slot=%u profiles=%u cases=%u hash=%08x PASS (all authored lanes/terminal profiles, whole13a14)\n",
        road.slot,road.profile_count,lateral_cases,lateral_hash);
    check_player_road_tail(board,road,*math);
    for (unsigned i = 0; i < road.map_count; ++i) {
        const unsigned index = board.read_word(bus.road_mapping+4*i);
        const unsigned flags = board.read_word(bus.road_mapping+4*i+2) | board.read_word(bus.road_flags+2*index);
        if (road.mapping[i].profile != index || road.mapping[i].flags != flags)
            throw std::runtime_error("road offline flag fold differs from original tables");
    }
    std::array<uint32_t,8> sources{};
    unsigned inhibit_starts=0;
    for(unsigned i=0;i<65536;++i) {
        nm_player_t car{};
        car.input_inhibit=std::bit_cast<int8_t>((uint8_t)(i>>8));
        car.inhibit_direction=std::bit_cast<int8_t>((uint8_t)i);
        car.boundary.speed=(float)((int)(i%17)-2)*0.125f;
        car.boundary.damage.flags=0x80000003u|(((i>>2)&3u)<<15);
        car.boundary.damage.inhibit_ticks=(int16_t)((i&32) ? 1 : (i&64) ? 256 : 0);
        const uint32_t clocks[]={0,1920,1921,0x7fffffffu,0xffffffffu};
        const uint32_t clock=clocks[(i>>4)%5];car.checkpoints.clock=clock;
        car.inhibit_angle=123;car.inhibit_scale=3.25f;car.boundary.damage.override_count=73;
        PlayerMemory memory;memory.entity.fill(0xa5);memory.fields(car,true);
        bus.entity=memory.entity;bus.globals=memory.globals;bus.inhibit_owner=0x12345678;
        const auto globals=bus.globals;const auto before=car;
        const int result=nm_opponent_inhibit_step(math.get(),clock,&car);
        if(!result) throw std::runtime_error("native opponent inhibit rejected valid inputs");
        inhibit_starts+=result==2;memory.fields(car,true);
        bus.run(0x15a04,0x15b8c);
        if(bus.entity!=memory.entity || bus.globals!=globals || bus.inhibit_owner!=(result==2 ? 0x500000u : 0x12345678u)) {
            for(unsigned b=0;b<memory.entity.size();++b) if(memory.entity[b]!=bus.entity[b])
                std::fprintf(stderr,"opponent inhibit case=%u offset=%03x native=%02x source=%02x\n",i,b,memory.entity[b],bus.entity[b]);
            throw std::runtime_error("opponent inhibit differs from original15a04 gameplay state");
        }
        if(before.input_inhibit) {
            auto invalid=before;invalid.boundary.speed=NAN;const auto saved=invalid;
            if(nm_opponent_inhibit_step(math.get(),clock,&invalid) || memcmp(&saved,&invalid,sizeof(saved)))
                throw std::runtime_error("opponent inhibit failure changed state");
        }
    }
    if(!inhibit_starts) throw std::runtime_error("opponent inhibit missing start coverage");
    std::printf("native_opponent_inhibit PASS cases=65536 starts=%u (whole entity/globals/owner; sound cooldown active)\n",inhibit_starts);
    bus.tail_road=&road;bus.collision_wrap=road.profile_count-1;
    for(unsigned i=0;i<4096;++i) {
        nm_player_t car{};const unsigned profile=i%road.profile_count;
        car.phase[0]=(int16_t)profile;car.phase[1]=-123;
        memcpy(car.boundary.ground.position,road.profiles[profile].lanes[4].xyz,12);
        car.boundary.ground.road=i%road.map_count;car.boundary.damage.initial_inhibit=(uint8_t)i;
        car.boundary.ground.pitch=nm_yaw_angle(i*97);car.boundary.ground.roll=nm_yaw_angle(i*179);
        car.body_yaw=nm_yaw_angle(i*773);car.render_roll_offset=321;
        car.lateral.current=-123;car.lateral.control=-99;car.path_offset=0xbeef;
        car.lap.lap=(uint8_t)(i/7);car.lap.start_ticks=(uint16_t)(i*113);car.checkpoints.clock=0xffff0000u+i;
        PlayerMemory memory;memory.entity.fill(0xa5);memory.fields(car,true);
        const uint16_t count=(uint16_t)(road.profile_count-1);memcpy(memory.entity.data()+0x12c,&count,2);
        bus.entity=memory.entity;bus.globals=memory.globals;const auto globals=bus.globals;
        const auto before=car;
        if(!nm_opponent_road_tail(math.get(),&road,&car)) throw std::runtime_error("opponent tail rejected valid state");
        memory.fields(car,true);
        bus.run(0xce74,0xceb0);bus.run(0x1365c,0x136e4);bus.run(0x16e10,0x16e54);bus.run(0x13de0,0x13dfc);
        if(bus.entity!=memory.entity || bus.globals!=globals) throw std::runtime_error("opponent tail differs from original timer/pose/road/scenery/rank");
        auto invalid=before;invalid.boundary.ground.road=road.map_count;const auto saved=invalid;
        if(nm_opponent_road_tail(math.get(),&road,&invalid) || memcmp(&saved,&invalid,sizeof(saved)))
            throw std::runtime_error("opponent tail late failure mutated state");
    }
    bus.tail_road=nullptr;
    std::puts("native_opponent_road_tail PASS cases=4096 all timer bytes, whole entity/globals, late failure atomic");
    unsigned intro_wraps=0;
    for(unsigned i=0;i<road.map_count;++i) for(unsigned variant=0;variant<8;++variant) {
        nm_player_t player{};
        player.boundary.ground.road=i;
        const unsigned current=road.mapping[i].profile;
        const unsigned previous[]={0,current,road.profile_count-1,current+1,current+7,current+8,65535,32768};
        player.phase[0]=nm_yaw_angle(previous[variant]);player.phase[1]=-123;
        player.path_flags=0x1234;player.previous_path_flags=0xabcd;
        player.lap.lap=(uint8_t)(i+variant);player.lap.completed=123;
        player.lap.start_ticks=(uint16_t)(i*997);player.lap.last_ticks=0xbeef;
        player.checkpoints.clock=0xffff0000u+i*137;
        PlayerMemory memory;memory.entity.fill(0xa5);memory.fields(player,true);
        const uint16_t count=(uint16_t)(road.profile_count-1);
        memcpy(memory.entity.data()+0x12c,&count,2);
        bus.entity=memory.entity;bus.globals=memory.globals;
        const auto globals=bus.globals;const auto before=player;
        if(!nm_intro_road_step(&road,&player)) throw std::runtime_error("intro road rejected valid map");
        intro_wraps+=player.lap.lap!=before.lap.lap;
        memory.fields(player,true);bus.run(0x1365c,0x136e4);
        if(bus.entity!=memory.entity || bus.globals!=globals)
            throw std::runtime_error("intro road/lap differs from original1365c");
        auto invalid=before;invalid.boundary.ground.road=road.map_count;const auto saved=invalid;
        if(nm_intro_road_step(&road,&invalid) || memcmp(&invalid,&saved,sizeof(saved)))
            throw std::runtime_error("intro invalid map mutated state");
    }
    if(!intro_wraps) throw std::runtime_error("intro road wrap coverage missing");
    std::printf("native_intro_road PASS cases=%u wraps=%u (whole1365c, full entity/globals, invalid map atomic)\n",road.map_count*8,intro_wraps);
    if (road.slot < 3) for (unsigned route = 0; route < paths.size(); ++route) {
        const auto filename = std::filesystem::path(path_directory)/
            ("course"+std::to_string(road.slot)+"-route"+std::to_string(route)+".dnp");
        if (!nm_path_load(&paths[route],filename.c_str()) || paths[route].slot != road.slot || paths[route].route != route)
            throw std::runtime_error("road selection needs all eight matching path packs");
        sources[route] = check_path_source(board,paths[route]);
    }
    if(road.slot==0) {
        constexpr uint32_t entity=0x5fc000;
        std::array<uint8_t,0x300> saved_entity;
        for(unsigned b=0;b<saved_entity.size();++b) saved_entity[b]=board.read_byte(entity+b);
        const uint32_t old_profiles=board.read_dword(0x501448),old_paths=board.read_dword(0x50144c);
        const auto restore=[&] {
            board.write_dword(0x501448,old_profiles);board.write_dword(0x50144c,old_paths);
            for(unsigned b=0;b<saved_entity.size();++b) board.write_byte(entity+b,saved_entity[b]);
        };
        unsigned cases=0;
        try {
            board.write_dword(0x501448,source_profiles+8);board.write_dword(0x50144c,board.read_dword(desc+16));
            for(uint32_t stage : {0x2865a80u,0x2865d58u}) {
                const unsigned count=board.read_dword(stage+4);
                if(count>44) throw std::runtime_error("intro spawn count invalid");
                for(unsigned car=0;car<count;++car) {
                    const uint32_t at=stage+12+16*car,route=board.read_dword(at),speed_index=board.read_dword(at+4);
                    const uint32_t packed=board.read_dword(at+8),updater=board.read_dword(at+12);
                    if(route>=8 || speed_index>=512 || (updater!=0xcf74 && updater!=0xcfc0 && updater!=0xd05c))
                        throw std::runtime_error("intro spawn source unsupported");
                    const float speed=std::bit_cast<float>(board.read_dword(0x28b7520+4*speed_index))*
                        std::bit_cast<float>(board.read_dword(0x28b573c));
                    const nm_intro_spawn_t spawn={route,speed,(uint16_t)packed,nm_yaw_angle(packed>>16),updater==0xcf74 ? 0u : updater==0xcfc0 ? 1u : 2u};
                    nm_player_t player{};
                    player.boundary.damage.flags=0x12345678;player.render_roll_offset=nm_yaw_angle(car*997);
                    player.resistance=7;player.filtered_pitch=123;player.render_yaw_offset=-456;player.bank=789;
                    player.boundary.damage.overlay_model=0x123456;player.boundary.damage.angle[0]=123;
                    player.inhibit_angle=123;player.input_inhibit=4;player.inhibit_direction=-3;
                    player.surface_ticks[0]=12;player.force.trigger_count[1]=13;player.response_ticks=14;player.boundary.condition=15;
                    PlayerMemory memory;memory.entity.fill(0xa5);memory.fields(player,true);
                    for(unsigned b=0;b<memory.entity.size();++b) board.write_byte(entity+b,memory.entity[b]);
                    const auto before=player;float height=-999;
                    if(!nm_intro_spawn_car(&spawn,&road,&paths[route],&player,&height))
                        throw std::runtime_error("intro spawn rejected authored record");
                    memory.fields(player,true);
                    const uint16_t length=(uint16_t)paths[route].count;
                    memcpy(memory.entity.data()+0x12c,&length,2);memcpy(memory.entity.data()+0x130,&sources[route],4);
                    memcpy(memory.entity.data()+12,&updater,4);memcpy(memory.entity.data()+0x288,&height,4);
                    memory.entity[0x97]=0; // Unrepresented source effect byte, cleared by cfd0 block.
                    for(uint32_t entry : {0x226468u,updater==0xd05c ? 0xd08cu : 0xcfd0u}) {
                        rt::I960Ref cpu(&board);cpu.m_IP=entry;cpu.m_r[16]=at;
                        cpu.m_r[25]=entity;cpu.m_r[29]=entity;cpu.m_r[27]=0x880000;cpu.m_r[28]=0x4000;
                        unsigned steps=0;
                        while(board.fetch(cpu.m_IP)!=0x0a000000 || cpu.m_rcache_pos) {
                            if(++steps>4096) throw std::runtime_error("intro spawn original instruction guard");
                            cpu.execute_one();
                        }
                    }
                    for(unsigned b=0;b<memory.entity.size();++b) if(board.read_byte(entity+b)!=memory.entity[b])
                        throw std::runtime_error("intro spawn/pose differs from original226468/cfd0");
                    auto invalid=before;auto wrong=spawn;wrong.route=8;const float prior=height;
                    if(nm_intro_spawn_car(&wrong,&road,&paths[route],&invalid,&height) ||
                       memcmp(&invalid,&before,sizeof(before)) || height!=prior)
                        throw std::runtime_error("invalid intro spawn changed outputs");
                    ++cases;
                }
            }
        } catch(...) { restore();throw; }
        restore();
        std::printf("native_intro_spawn PASS cars=%u (both authored stages, original226468+cfd0/d08c pose/reset, full entity, model binding separate)\n",cases);
    }
    if(road.slot<3) {
        bus.tail_road=&road;
        nm_intro_speed_params_t intro{};
        intro.desired_divisor=std::bit_cast<float>(board.read_dword(0x28b57a4));
        intro.drag_offset=std::bit_cast<float>(board.read_dword(0x28b5814));
        intro.minimum_delta=std::bit_cast<float>(board.read_dword(0x28b57b8));
        for(unsigned i=0;i<512;++i) intro.response[i]=std::bit_cast<float>(board.read_dword(0x28b85a0+4*i));
        unsigned changes=0;
        for(unsigned route=0;route<8;++route) for(unsigned leader=0;leader<2;++leader) {
            bus.path=paths[route];nm_player_t player{};
            const auto &row=bus.path.rows[0];
            memcpy(player.boundary.ground.position,row.xyz,sizeof(row.xyz));
            player.boundary.ground.road=row.road;player.phase[0]=nm_yaw_angle(road.mapping[row.road].profile);
            player.lateral.current=player.lateral.target=384;
            player.boundary.heading=nm_yaw_angle((uint32_t)(int32_t)row.yaw*4);
            player.boundary.speed=1;player.desired_speed=2;player.speed_index=100;
            for(unsigned tick=0;tick<1024;++tick) {
                player.checkpoints.clock=tick;
                bus.profile=road.profiles[(uint16_t)player.phase[0]];
                PlayerMemory memory;memory.entity.fill(0xa5);memory.fields(player,true);
                const uint16_t count=(uint16_t)(road.profile_count-1);
                memcpy(memory.entity.data()+0x12c,&count,2);
                bus.entity=memory.entity;bus.globals=memory.globals;
                bus.write_dword(0x500130,0x630000);bus.write_dword(0x501090,1u<<3);
                memory.entity=bus.entity;const auto globals=bus.globals;const auto before=player;
                if(!nm_intro_drive_step(math.get(),&intro,&road,&bus.path,1u<<3,leader,&player))
                    throw std::runtime_error("intro connected drive rejected authored path");
                changes+=player.phase[0]!=before.phase[0];memory.fields(player,true);
                bus.run(0x11438,0x115e0);bus.run(leader ? 0xe6b8 : 0xe6f4,leader ? 0xe6f0 : 0xe728);
                bus.run(0x11150,0x11300);bus.run(0x1365c,0x136e4);bus.run(0x16e10,0x16e54);
                if(bus.entity!=memory.entity || bus.globals!=globals)
                    throw std::runtime_error("intro connected drive differs from original five routines");
                auto invalid=before;auto broken=road;broken.map_count=0;
                if(nm_intro_drive_step(math.get(),&intro,&broken,&bus.path,1u<<3,leader,&invalid) || memcmp(&invalid,&before,sizeof(before)))
                    throw std::runtime_error("intro late road failure published motion");
            }
        }
        if(!changes) throw std::runtime_error("intro persistent drive never changed profile");
        std::printf("native_intro_drive PASS ticks=16384 profiles_changed=%u (eight routes, leader/opponent, persistent1024 ticks, full entity/globals)\n",changes);
        bus.tail_road=nullptr;
    }
    unsigned cases = 0, normal = 0, forward = 0, reverse = 0, moved = 0, actual_held = 0;
    uint32_t hash = 2166136261u;
    const auto run = [&](std::array<uint8_t,0x300> input, const nm_path_pack_t *path) {
        const auto get = [&]<class T>(unsigned at, T) { T value; memcpy(&value,input.data()+at,sizeof(value)); return value; };
        nm_road_selection_t s = {get(0x30,uint32_t{}),get(0x134,uint16_t{}),get(0x128,uint16_t{}),
            get(0x12a,uint16_t{}),get(0x138,uint16_t{}),get(0x13a,uint16_t{})};
        const uint16_t surface = get(0x1b8,uint16_t{});
        const uint32_t flags = get(0,uint32_t{}), source = get(0x130,uint32_t{});
        bus.path = path ? *path : nm_path_pack_t{};
        if (surface&0x50c) { const uint32_t remapped = 0x630000; memcpy(input.data()+0x130,&remapped,4); }
        bus.entity = input; const auto globals = bus.globals;
        const unsigned offset = s.offset;
        if (!nm_road_select(&road,path,math.get(),surface,flags,get(0x1c,float{}),get(0x24,float{}),&s))
            throw std::runtime_error("road selection rejected oracle input");
        auto expected = input;
        const auto put = [&](unsigned at, auto value) { memcpy(expected.data()+at,&value,sizeof(value)); };
        put(0x30,s.road); put(0x134,s.offset); put(0x128,s.phase); put(0x12a,s.previous_phase);
        put(0x138,s.flags); put(0x13a,s.previous_flags);
        bus.run(0x10980,0x10a10);
        if (bus.entity != expected || bus.globals != globals) {
            for (unsigned i = 0; i < expected.size(); ++i) if (bus.entity[i] != expected[i])
                std::fprintf(stderr,"road selection case=%u offset=%03x native=%02x reference=%02x\n",cases,i,expected[i],bus.entity[i]);
            throw std::runtime_error("road selection differs from whole i960/TGP memory");
        }
        ++cases; moved += s.offset != offset;
        if (!(surface&0x50c)) ++normal; else if (flags&(1u<<28)) ++reverse; else ++forward;
        for (unsigned at : {0x30u,0x31u,0x32u,0x33u,0x128u,0x129u,0x12au,0x12bu,0x134u,0x135u,0x138u,0x139u,0x13au,0x13bu})
            hash = (hash^expected[at])*16777619u;
        put(0x130,source); // Restore only the host oracle's relocated path pointer.
        return expected;
    };
    const auto synthetic = [&](unsigned i, uint16_t surface, const nm_path_pack_t *path, unsigned vector) {
        std::array<uint8_t,0x300> input; input.fill(0xa5);
        const auto put = [&](unsigned at, auto value) { memcpy(input.data()+at,&value,sizeof(value)); };
        nm_path_cursor_t s{}; float x = NAN, z = INFINITY; int reverse = i&1;
        if (surface&0x50c) {
            nm_path_test_input(path,vector,&x,&z,&reverse,&s); put(0x12c,uint16_t(path->count));
        } else s.road = i%road.map_count;
        put(0,uint32_t((i*2654435761u & ~(1u<<28)) | (uint32_t(reverse)<<28)));
        put(0x30,s.road); put(0x134,s.offset); put(0x1c,x); put(0x24,z); put(0x1b8,surface);
        put(0x128,uint16_t(i*73)); put(0x12a,uint16_t(~i)); put(0x138,uint16_t(i*151)); put(0x13a,uint16_t(i*17));
        run(input,path);
    };
    // Every map row in normal mode; then every surface mask and all held path
    // rows/endpoints/directions. Slot3 has no validated DNP1 layout: normal only.
    for (unsigned i = 0; i < road.map_count; ++i) synthetic(i,0,nullptr,0);
    for (unsigned i = 0; i < 65536; ++i) {
        const nm_path_pack_t *path = road.slot < 3 ? &paths[(i/20)%8] : nullptr;
        synthetic(i,uint16_t(path ? i : i&~0x50c),path,path ? i%nm_path_test_count(path) : 0);
    }
    for (const auto &path : paths) if (path.rows)
        for (unsigned i = 0; i < nm_path_test_count(&path); ++i) synthetic(i,0x50c,&path,i);
    if (actual) {
        if (actual->empty()) throw std::runtime_error("no actual road selections observed");
        for (const auto &entry : *actual) {
            if (entry.mapping != bus.road_mapping || entry.flags != bus.road_flags)
                throw std::runtime_error("actual road selection uses different source tables");
            const nm_path_pack_t *path = nullptr;
            uint16_t surface, count; uint32_t source;
            memcpy(&surface,entry.before.data()+0x1b8,2); memcpy(&source,entry.before.data()+0x130,4);
            memcpy(&count,entry.before.data()+0x12c,2);
            if (surface&0x50c) {
                ++actual_held;
                for (unsigned i = 0; i < paths.size(); ++i) if (sources[i] == source && paths[i].count == count) path = &paths[i];
                if (!path) throw std::runtime_error("actual held road selection has unknown path");
            }
            if (run(entry.before,path) != entry.after) throw std::runtime_error("native road selection differs from actual return");
        }
    }
    std::printf("native_road_selection slot=%u cases=%u hash=%08x normal/forward/reverse=%u/%u/%u moved=%u actual=%zu actual_held=%u payload=%u state=%zu PASS (whole10980 and original ROM tables)\n",
        road.slot,cases,hash,normal,forward,reverse,moved,actual ? actual->size() : 0,actual_held,road.payload_bytes,sizeof(nm_road_selection_t));
}
}
