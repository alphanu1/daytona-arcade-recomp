#pragma once
#include "../include/player.h"
#include "../include/collision.h"
#include "../include/practice.h"
#include "../include/race_field.h"
#include "runtime/m2_board.h"
#include <array>
#include <bit>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <filesystem>
#include <algorithm>
#include <vector>

namespace daytona::native {
inline void checkpoint_award_fields(std::array<uint8_t,0x300> &entity,
        std::array<uint8_t,0x100> &globals, std::array<uint8_t,0x700> &extra,
        nm_checkpoint_award_t &s, int16_t &signal, bool store) {
    const auto copy = [store](uint8_t *at, auto &value) {
        if (store) memcpy(at,&value,sizeof(value)); else memcpy(&value,at,sizeof(value));
    };
    copy(entity.data()+0x6f,s.counter); copy(entity.data()+0x202,s.condition);
    copy(globals.data()+0xd0,s.remaining); copy(globals.data()+0xd4,s.display);
    copy(globals.data()+0xe0,s.calls); copy(extra.data()+0x612,s.announcement_ticks);
    copy(extra.data()+0x602,signal);
}
inline void tilt_fields(std::array<uint8_t,0x300> &entity,
        std::array<uint8_t,0x700> &extra, nm_player_t &s, bool store) {
    const auto copy = [store](uint8_t *at, auto &value) {
        if (store) memcpy(at,&value,sizeof(value)); else memcpy(&value,at,sizeof(value));
    };
    copy(entity.data()+0x215,s.surface_phase); copy(entity.data()+0x22c,s.tilt_phase);
    copy(extra.data()+0x444,s.tilt[0]); copy(extra.data()+0x448,s.tilt[1]);
}
inline void body_angle_fields(std::array<uint8_t,0x300> &entity, nm_player_t &s, bool store) {
    const auto field = [store,&entity](unsigned at, auto &value) {
        if (store) memcpy(entity.data()+at,&value,sizeof(value)); else memcpy(&value,entity.data()+at,sizeof(value));
    };
    field(0x4c,s.render_angles); field(0x52,s.filtered_pitch); field(0x54,s.render_yaw_offset);
    field(0x56,s.bank); field(0x20e,s.render_roll_offset);
}
inline void checkpoint_fields(std::array<uint8_t,0x300> &entity,
        std::array<uint8_t,0x100> &globals, std::array<uint8_t,0x700> &extra,
        nm_lap_t &lap, nm_checkpoints_t &race, bool store) {
    const auto copy = [store](uint8_t *at, auto &value) {
        if (store) memcpy(at,&value,sizeof(value)); else memcpy(&value,at,sizeof(value));
    };
    copy(entity.data()+0x12e,lap.furthest); copy(entity.data()+0x64,lap.best_ticks);
    copy(entity.data()+0x66,lap.last_ticks); copy(entity.data()+0x68,lap.start_ticks);
    copy(entity.data()+0x6c,lap.counter); copy(entity.data()+0x6d,lap.completed); copy(entity.data()+0x6e,lap.lap);
    copy(extra.data()+0xb0,race.visited); copy(extra.data()+0xb4,race.required);
    copy(extra.data()+0xb8,race.last_lap); copy(extra.data()+0xb9,race.completed);
    copy(extra.data()+0x68c,race.reverse); copy(globals.data()+0xcc,race.clock);
    copy(globals.data()+0xd8,race.finish_flags); copy(globals.data()+0xdc,race.display_ticks);
    copy(globals.data()+0xec,race.total_laps);
}
inline void traffic_pass_fields(std::array<uint8_t,0x300> &entity, nm_traffic_pass_body_t &s, bool store) {
    const auto field = [&](unsigned at, auto &value) {
        if (store) memcpy(entity.data()+at,&value,sizeof(value)); else memcpy(&value,entity.data()+at,sizeof(value));
    };
    field(0x18,s.progress); field(0x1c,s.position[0]); field(0x24,s.position[1]); field(0x13e,s.lateral);
}
inline void traffic_near_fields(std::array<uint8_t,0x300> &entity, nm_traffic_near_body_t &s, bool store) {
    const auto field = [&](unsigned at, auto &value) {
        if (store) memcpy(entity.data()+at,&value,sizeof(value)); else memcpy(&value,entity.data()+at,sizeof(value));
    };
    field(0,s.flags); field(0x146,s.marker146); field(0x18,s.progress); field(0x1c,s.position[0]); field(0x24,s.position[1]); field(0x15f,s.hold);
}
inline void traffic_order_fields(std::array<uint8_t,0x300> &entity, nm_traffic_order_body_t &s, bool store) {
    const auto field = [&](unsigned at, auto &value) {
        if (store) memcpy(entity.data()+at,&value,sizeof(value)); else memcpy(&value,entity.data()+at,sizeof(value));
    };
    field(0x60,s.rank_key); field(0x128,s.profile); field(0x6e,s.lap); field(0x6a,s.rank);
    // next/previous are native indices; map guest pointers in the caller.
}
inline void collision_rank_fields(std::array<uint8_t,0x300> &entity, nm_collision_rank_t &s, bool store) {
    const auto field = [&](unsigned at, auto &value) {
        if (store) memcpy(entity.data()+at,&value,sizeof(value)); else memcpy(&value,entity.data()+at,sizeof(value));
    };
    field(0,s.flags); field(0x2a4,s.relative); field(0x1c,s.position[0]); field(0x24,s.position[1]);
    field(0x28c,s.motion.previous); field(0x294,s.motion.travel); field(0x128,s.profile);
    field(0x258,s.adjusted); field(0x25a,s.candidates); field(0x27d,s.contact_side);
}
inline void collision_shape_fields(std::array<uint8_t,0x300> &entity, nm_collision_shape_t &s, bool store) {
    const auto field = [&](unsigned at, auto &value) {
        if (store) memcpy(entity.data()+at,&value,sizeof(value)); else memcpy(&value,entity.data()+at,sizeof(value));
    };
    field(0,s.flags); field(0x1c,s.position); field(0xf4,s.world); field(0x58,s.heading);
    field(0x2c,s.roll); field(0xc8,s.inhibit_ticks); field(0xed,s.side);
}
inline void collision_contact_fields(std::array<uint8_t,0x300> &record, nm_collision_contact_t &s, bool store) {
    const auto field = [&](unsigned at, auto &value) {
        if (store) memcpy(record.data()+at,&value,sizeof(value)); else memcpy(&value,record.data()+at,sizeof(value));
    };
    field(0x34,s.flags); field(0x44,s.quarter); field(0x38,s.angle[0]); field(0x4c,s.angle[1]);
    field(0x3c,s.side[0]); field(0x50,s.side[1]); field(0x48,s.corner[0]); field(0x54,s.corner[1]);
    field(0x58,s.countdown); field(0x9c,s.depth[0]); field(0x98,s.depth[1]);
}
inline void damage_entity_fields(std::array<uint8_t,0x300> &entity, nm_damage_state_t &s,
                                 uint32_t sequence, bool store) {
    const auto field = [&](unsigned at, auto &value) {
        if (store) memcpy(entity.data()+at,&value,sizeof(value)); else memcpy(&value,entity.data()+at,sizeof(value));
    };
    field(0,s.flags); field(0x208,s.damage); field(0x14,s.body_model); field(0x5c,s.overlay_model);
    field(0xe8,s.spin_heading); field(0xea,s.spin_rotation); field(0x274,s.remaining); field(0xc8,s.inhibit_ticks);
    field(0xca,s.angle); field(0x276,s.sound_ticks); field(0xec,s.spin_ticks); field(0xed,s.side);
    field(0x15f,s.override_count); field(0xc7,s.crash_count); field(0x20d,s.initial_inhibit); field(0x27c,s.orientation);
    if (store) {
        if (s.row <= NM_CRASH_ROWS) {
            const uint32_t pointer = sequence+4+16*s.row; memcpy(entity.data()+0x278,&pointer,4);
        }
    } else {
        uint32_t pointer; memcpy(&pointer,entity.data()+0x278,4);
        s.row = pointer >= sequence+4 && pointer <= sequence+4+16*NM_CRASH_ROWS && (pointer-sequence-4)%16 == 0 ?
            (uint16_t)((pointer-sequence-4)/16) : UINT16_MAX;
    }
}
inline void collision_damage_fields(std::array<uint8_t,0x300> &entity, nm_collision_damage_body_t &s,
                                    uint32_t sequence, bool store) {
    damage_entity_fields(entity,s.damage,sequence,store);
    const auto field = [&](unsigned at, auto &value) {
        if (store) memcpy(entity.data()+at,&value,sizeof(value)); else memcpy(&value,entity.data()+at,sizeof(value));
    };
    field(0x9c,s.delta[0]); field(0xa4,s.delta[1]); field(0xd4,s.input_inhibit);
    field(4,s.entity_id); field(0x27d,s.contact_side);
}

inline void collision_impulse_fields(std::array<uint8_t,0x300> &entity, nm_collision_impulse_body_t &s, bool store) {
    const auto field = [&](unsigned at, auto &value) {
        if (store) memcpy(entity.data()+at,&value,sizeof(value)); else memcpy(&value,entity.data()+at,sizeof(value));
    };
    field(0x34,s.speed); field(0x9c,s.delta[0]); field(0xa4,s.delta[1]); field(0x218,s.correction);
    field(0x58,s.yaw); field(0x216,s.own_yaw); field(0x212,s.other_yaw); field(0xc8,s.inhibit_ticks);
    field(0xd4,s.input_inhibit); field(0x98,s.response_ticks); field(4,s.entity_id); field(0x27d,s.contact_side);
}
inline nm_collision_choice_body_t collision_choice_body(const std::array<uint8_t,0x300> &entity) {
    nm_collision_choice_body_t body{};
    memcpy(body.travel,entity.data()+0x294,8); memcpy(body.world,entity.data()+0xf4,48);
    return body;
}
inline nm_collision_choice_input_t collision_choice_input(const std::array<uint8_t,0x300> &record) {
    nm_collision_choice_input_t in{};
    memcpy(&in.depth[0],record.data()+0x9c,4); memcpy(&in.depth[1],record.data()+0x98,4);
    memcpy(&in.corner[0],record.data()+0x48,4); memcpy(&in.corner[1],record.data()+0x54,4);
    memcpy(&in.angle,record.data()+0x38,4);
    return in;
}
inline nm_collision_route_input_t collision_route_input(const std::array<uint8_t,0x300> &record,
        const std::array<uint8_t,0x300> &first, const std::array<uint8_t,0x300> &second) {
    nm_collision_route_input_t in{}; in.contact = collision_choice_input(record);
    memcpy(&in.flags,record.data()+0x34,4); memcpy(&in.heading[0],first.data()+0x58,2);
    memcpy(&in.heading[1],second.data()+0x58,2); return in;
}

// Host-only field boundary. Whole entity/global arrays retain all untouched
// bytes, so omitted stores cannot be hidden by comparing only known outputs.
struct PlayerMemory {
    std::array<uint8_t,0x300> entity{};
    std::array<uint8_t,8> yaw{}, ahead{};
    std::array<uint8_t,16> heights{};
    std::array<uint8_t,32> auxiliary{};
    std::array<uint8_t,32> crash_auxiliary{};
    std::array<uint8_t,0x100> globals{};
    std::array<uint8_t,4> signals{};
    std::array<uint8_t,48> camera{};
    uint32_t last_surface = 0, sequence = 0;
    uint32_t control = 0;
    int32_t camera_mode = 0;
    uint16_t center_mask = 0;
    uint8_t profile_updates = 0, shake = 0, sequence_counter = 0;
    std::array<int16_t,2> shake_angles{};
    std::array<uint8_t,0x700> checkpoint_extra{};
    bool operator==(const PlayerMemory &) const = default;
    void fields(nm_player_t &s, bool store) {
        const auto copy = [store](void *memory, auto &value) {
            if (store) std::memcpy(memory,&value,sizeof(value)); else std::memcpy(&value,memory,sizeof(value));
        };
        const auto field = [&](unsigned at, auto &v) { copy(entity.data()+at,v); };
        field(0,s.boundary.damage.flags); field(0x1c,s.boundary.ground.position); field(0x28,s.boundary.ground.pitch);
        field(0x2a,s.body_yaw); field(0x2c,s.boundary.ground.roll); field(0x2e,s.scenery_cell); field(0x30,s.boundary.ground.road);
        field(0x34,s.boundary.speed); field(0x3c,s.force.longitudinal_delta); field(0x44,s.desired_speed);
        field(0x48,s.boundary.heading); field(0x58,s.boundary.ground.yaw); field(0x95,s.force.trigger_count);
        field(0x9c,s.delta[0]); field(0xa4,s.delta[1]); field(0xf4,s.boundary.ground.world);
        field(0x124,s.force.steer_angle); field(0x134,s.path_offset); field(0x136,s.ahead_offset);
        field(0x13e,s.lateral); field(0x150,s.rpm); field(0x15f,s.boundary.damage.override_count);
        field(0x160,s.terms.drag); field(0x168,s.terms.drive); field(0x16c,s.force.inertial);
        field(0x174,s.terms.brake); field(0x17c,s.force.grip); field(0x188,s.force.tire_angle);
        field(0x194,s.axle.load); field(0x1a0,s.force.rear_bank_force);
        field(0x1a4,s.boundary.ground.selected_surface); field(0x1a8,s.boundary.ground.surface);
        field(0x1b8,s.boundary.ground.combined_mask); field(0x1ba,s.boundary.ground.mask); field(0x1c2,s.boundary.ground.road_heading);
        const unsigned road[] = {0x1c8,0x1c4,0x1cc,0x1d0,0x1d4,0x1d8};
        for (unsigned i = 0; i < 6; ++i) field(road[i],s.boundary.ground.road_frame[i]);
        field(0x1f0,s.alignment_frames); field(0x1f4,s.guided_ticks);
        field(0x23c,s.adjustment_target); field(0x240,s.adjustment);
        field(0x248,s.tire_filter); field(0x24a,s.yaw_delta); field(0x24c,s.force.rpm_boost);
        field(0x25c,s.compensation); field(0x260,s.force.slip_decay); field(0x264,s.countdown);
        field(0x26c,s.condition); field(0x26e,s.road_yaw); field(0x270,s.previous_lateral);
        field(0x29c,s.terms.slope); field(0x2a0,s.force.slip_ratio);
        copy(yaw.data(),s.yaw_offset); copy(ahead.data(),s.lookahead);
        for (unsigned v = 0; v < 4; ++v) copy(heights.data()+4*(v^1),s.boundary.ground.height[v]);
        copy(&profile_updates,s.profile_updates);
        field(0x128,s.phase); field(0x138,s.path_flags); field(0x13a,s.previous_path_flags);
        field(0x91,s.surface_ticks);
        field(0x148,s.gear); field(0xa8,s.brake); field(0xac,s.accel); field(0xb0,s.steer);
        field(0x154,s.filtered_rpm); field(0x158,s.downshift_rpm); field(0x15c,s.shift_ticks);
        field(0x15e,s.drag_state); field(0x164,s.resistance); field(0xd4,s.input_inhibit);
        copy(camera.data()+16,s.camera_words); copy(camera.data()+40,s.camera_angles);
        copy(camera.data()+3,s.camera_transition); copy(&camera_mode,s.camera_mode);
        copy(&control,s.control); copy(crash_auxiliary.data(),s.auxiliary_flags);
        field(0x18,s.speed_index); field(0x5a,s.wheel_phase[0]); field(0x250,s.wheel_phase[1]);
        field(0xc0,s.spin_body_offset);
        field(0x218,s.correction); field(0x214,s.correction_ticks);
        field(0xdc,s.inhibit_scale); field(0xd6,s.inhibit_angle); field(0xd5,s.inhibit_direction);
        copy(shake_angles.data(),s.shake_angles);
        copy(globals.data()+0xc0,s.offcourse_ticks);
        checkpoint_fields(entity,globals,checkpoint_extra,s.lap,s.checkpoints,store);
        body_angle_fields(entity,s,store);
        tilt_fields(entity,checkpoint_extra,s,store);
        field(0x60,s.rank_key);
        field(0x62,s.rank_key_high);
        field(0x98,s.response_ticks);
        if (!store) s.route_transition = 0;
        else if (s.route_transition) {
            const uint32_t mode = 0xdc18;
            memcpy(entity.data()+12,&mode,4);
        }
        auto &v = s.boundary; auto &d = v.damage;
        field(0xf0,v.impact_speed); field(0x1e0,v.displacement); field(0xee,v.rotation);
        field(0x1ec,v.recovery_rotation); field(0x99,v.condition);
        copy(globals.data()+0xb8,v.hold_ticks); copy(globals.data()+0xc4,v.recovery_ticks);
        copy(signals.data(),v.pivot_signal); copy(&center_mask,v.center_mask);
        field(0x208,d.damage); field(0x14,d.body_model); field(0x5c,d.overlay_model);
        field(0xe8,d.spin_heading); field(0xea,d.spin_rotation); field(0x274,d.remaining); field(0xc8,d.inhibit_ticks);
        field(0xca,d.angle); field(0x276,d.sound_ticks); field(0xec,d.spin_ticks); field(0xed,d.side);
        field(0xc7,d.crash_count); field(0x20d,d.initial_inhibit); field(0x27c,d.orientation);
        copy(&last_surface,d.last_surface); copy(&sequence_counter,d.sequence_counter);
        copy(signals.data()+2,d.signal); copy(&shake,d.shake_ticks);
        if (store) {
            if (d.row <= NM_CRASH_ROWS) {
                const uint32_t pointer = sequence+4+16*d.row;
                memcpy(entity.data()+0x278,&pointer,4);
            }
        } else {
            uint32_t pointer; memcpy(&pointer,entity.data()+0x278,4);
            d.row = pointer >= sequence+4 && pointer <= sequence+4+16*NM_CRASH_ROWS && (pointer-sequence-4)%16 == 0 ?
                (uint16_t)((pointer-sequence-4)/16) : UINT16_MAX;
        }
    }
};

struct LiveTrafficOrderCheck {
    std::array<nm_race_car_t,44> field_cars{};
    std::array<nm_race_car_t,44> interaction_cars{};
    nm_race_order_t interaction_order{};
    nm_race_shared_t interaction_shared{};
    int32_t interaction_events[2]{};
    nm_traffic_pass_t interaction_passing{};
    uint32_t field_sequence=0;
    nm_race_order_t field_order{};
    nm_race_shared_t field_shared{};
    unsigned field_count=0;
    uint64_t field_checked=0;
    unsigned field_signal_count=0;
    uint64_t field_signals_checked=0;
    int32_t field_events[2]{};
    nm_traffic_pass_t field_passing{};
    std::vector<uint32_t> addresses;
    std::vector<std::array<uint8_t,0x300>> memory;
    std::array<uint32_t,44> ranks{}, track{}, scratch{};
    int8_t lap = 0;
    bool pending = false, latest = false;
    uint64_t entered = 0, checked = 0, rotations = 0, ranking = 0, fed_links = 0;
    bool near_pending = false;
    unsigned reset_count = 0, player_index = 0, subject_index = 0;
    uint32_t ahead_progress[2]{};
    uint64_t near_rom_inputs = 0;
    uint64_t fed_bodies = 0;
    nm_race_timers_t timers{};
    nm_traffic_pass_t passing{};
    bool timers_pending = false, pass_pending = false;
    uint64_t timers_entered = 0, timers_checked = 0, pass_entered = 0, pass_checked = 0, pass_signals = 0;
    void check_timers(rt::M2Board &b) const {
        if (uint16_t(timers.short_ticks) != b.read_word(0x501894) || uint32_t(timers.control) != b.read_dword(0x501124) ||
                uint32_t(timers.event[0]) != b.read_dword(0x501a9c) || uint32_t(timers.event[1]) != b.read_dword(0x501aa4))
            throw std::runtime_error("native race timers differ from source");
    }
    void finish_timers(rt::M2Board &b, const rt::Cpu &cpu) {
        if (!timers_pending || cpu.m_IP != 0x1213c) throw std::runtime_error("unexpected race timer return");
        check_timers(b); timers_pending = false; ++timers_checked;
    }
    void finish_pass(rt::M2Board &b, const rt::Cpu &cpu) {
        if (!pass_pending || cpu.m_IP != 0x12140 || b.read_dword(0x50183c) != addresses[passing.next] ||
                b.read_dword(0x501840) != addresses[passing.previous] || b.read_word(0x501a04) != passing.signal)
            throw std::runtime_error("native traffic pass differs from source");
        if(field_signal_count) {
            if(field_cars[0].car.control!=b.read_dword(0x501124) ||
               (uint16_t)field_cars[0].car.boundary.damage.signal!=b.read_word(0x501a04) ||
               (uint16_t)field_shared.event_timer!=b.read_word(0x501894) ||
               (uint32_t)field_events[0]!=b.read_dword(0x501a9c) || (uint32_t)field_events[1]!=b.read_dword(0x501aa4) ||
               field_passing.next!=b.read_byte(b.read_dword(0x50183c)+4) ||
               field_passing.previous!=b.read_byte(b.read_dword(0x501840)+4) || field_passing.signal!=b.read_word(0x501a04))
                throw std::runtime_error("native field signals differ after native traffic/timers/passing");
            ++field_signals_checked;field_signal_count=0;
        }
        pass_pending = false; ++pass_checked;
    }
    uint64_t near_entered = 0, near_checked = 0, near_distinct_subject = 0, near_hold_changed = 0;
    uint16_t index(uint32_t address) const {
        // ponytail: linear lookup in the bounded host-only list check; add a map
        // only if larger diagnostic pools make this measurable.
        const auto at = std::find(addresses.begin(),addresses.end(),address);
        return at == addresses.end() ? UINT16_MAX : uint16_t(at-addresses.begin());
    }
    uint32_t link(rt::M2Board &b, uint32_t address, unsigned offset) {
        const unsigned i = index(address);
        if (!latest || pending || i >= memory.size()) throw std::runtime_error("collision preparation lacks native traffic links");
        uint32_t value; memcpy(&value,memory[i].data()+offset,4);
        if (value != b.read_dword(address+offset)) throw std::runtime_error("native traffic link changed before collision preparation");
        ++fed_links; return value;
    }
    const std::array<uint8_t,0x300> &body(rt::M2Board &b, uint32_t address) {
        const unsigned i = index(address);
        if (!latest || pending || near_pending || i >= memory.size()) throw std::runtime_error("collision preparation lacks native traffic body");
        for (unsigned j = 0; j < 0x300; ++j)
            if (memory[i][j] != b.read_byte(address+j)) throw std::runtime_error("traffic body changed before collision preparation");
        ++fed_bodies; return memory[i];
    }
    void finish_near(rt::M2Board &b, const rt::Cpu &cpu) {
        if (!near_pending || cpu.m_IP != 0x12258) throw std::runtime_error("unexpected traffic proximity return");
        for (unsigned i = 0; i < memory.size(); ++i) for (unsigned j = 0; j < 0x300; ++j)
            if (memory[i][j] != b.read_byte(addresses[i]+j)) throw std::runtime_error("traffic proximity whole body mismatch");
        for (unsigned j = 0; j < 44; ++j)
            if (ranks[j] != b.read_dword(0x503080+4*j) || track[j] != b.read_dword(0x503130+4*j) || scratch[j] != b.read_dword(0x5031e0+4*j))
                throw std::runtime_error("traffic proximity changed pointer arrays");
        if (uint8_t(lap) != b.read_byte(0x5014ba) || b.read_byte(0x5014e0)) throw std::runtime_error("traffic proximity global mismatch");
        if(field_count) {
            for(unsigned i=0;i<field_count;++i) {
                const unsigned body=index(track[i]);
                if(body>=memory.size()) throw std::runtime_error("native field traffic lost body");
                const unsigned id=memory[body][4];const auto &native=field_cars[id];
                const uint32_t at=addresses[body];
                if(native.car.boundary.damage.flags!=b.read_dword(at) || native.car.gear!=b.read_dword(at+0x148) ||
                   (uint16_t)native.near_marker!=b.read_word(at+0x146) || native.rank!=b.read_byte(at+0x6a) ||
                   native.next!=b.read_byte(b.read_dword(at+0x88)+4) ||
                   native.previous!=b.read_byte(b.read_dword(at+0x8c)+4) ||
                   field_order.track[i]!=id || ((field_shared.flags&0x500000) && field_order.rank[i]!=b.read_byte(ranks[i]+4)))
                    throw std::runtime_error("native field traffic differs from full original1224c car effects");
            }
            if(field_order.lap!=lap || field_shared.profile_updates)
                throw std::runtime_error("native field traffic shared state differs");
            ++field_checked;field_count=0;
        }
        near_pending = false; latest = true; ++near_checked;
    }
    void observe(rt::M2Board &b, const rt::Cpu &cpu, uint32_t target, const nm_drive_params_t *math) {
        if (target == 0x12150) {
            if (!latest || timers_pending || cpu.m_IP != 0x1213c) throw std::runtime_error("unexpected race timer entry");
            check_timers(b);
            if(field_signal_count) {
                // Native cars/lists already contain the previous phase's outputs.
                const auto prior_shared=field_shared;const auto prior_passing=field_passing;
                const auto control=field_cars[0].car.control;
                const int32_t prior_events[]={field_events[0],field_events[1]};
                const auto next=field_cars[0].next;field_cars[0].next=(uint16_t)field_signal_count;
                if(nm_race_signals_step(field_cars.data(),field_signal_count,&field_shared,field_events,&field_passing,math) ||
                   field_cars[0].car.control!=control || memcmp(&prior_shared,&field_shared,sizeof(field_shared)) ||
                   memcmp(&prior_passing,&field_passing,sizeof(field_passing)) || memcmp(prior_events,field_events,sizeof(field_events)))
                    throw std::runtime_error("native field signals late failure changed state");
                field_cars[0].next=next;
                if(!nm_race_signals_step(field_cars.data(),field_signal_count,&field_shared,field_events,&field_passing,math))
                    throw std::runtime_error("native field signals rejected prepared field");
            }
            if (!nm_race_timers_step(&timers)) throw std::runtime_error("native race timers rejected actual input");
            timers_pending = true; ++timers_entered;
        }
        if (target == 0x125b0) {
            if (!latest || timers_pending || pass_pending || cpu.m_IP != 0x12140) throw std::runtime_error("unexpected traffic pass entry");
            std::vector<nm_traffic_pass_body_t> bodies(memory.size());
            for (unsigned i = 0; i < memory.size(); ++i) {
                traffic_pass_fields(memory[i],bodies[i],false);
                uint32_t next, previous; memcpy(&next,memory[i].data()+0x88,4); memcpy(&previous,memory[i].data()+0x8c,4);
                bodies[i].next = index(next); bodies[i].previous = index(previous);
            }
            const uint16_t signal = passing.signal;
            if (!nm_traffic_pass(math,bodies.data(),bodies.size(),player_index,&passing)) throw std::runtime_error("native traffic pass rejected actual input");
            pass_signals += signal != passing.signal; pass_pending = true; ++pass_entered;
        }
        if (target == 0x12464) {
            if (!pending || cpu.m_IP != 0x12258) throw std::runtime_error("traffic return without matching entry");
            for (unsigned i = 0; i < memory.size(); ++i) for (unsigned j = 0; j < 0x300; ++j)
                if (memory[i][j] != b.read_byte(addresses[i]+j)) throw std::runtime_error("traffic order whole body memory mismatch");
            for (unsigned j = 0; j < 44; ++j)
                if (ranks[j] != b.read_dword(0x503080+4*j) || track[j] != b.read_dword(0x503130+4*j) || scratch[j] != b.read_dword(0x5031e0+4*j))
                    throw std::runtime_error("traffic order pointer/scratch array mismatch");
            if (uint8_t(lap) != b.read_byte(0x5014ba)) throw std::runtime_error("traffic order last lap mismatch");
            pending = false; ++checked;
            if (near_pending || index(cpu.m_r[29]) != subject_index) throw std::runtime_error("unexpected traffic proximity subject");
            std::vector<nm_traffic_near_body_t> bodies(memory.size());
            for (unsigned i = 0; i < memory.size(); ++i) {
                traffic_near_fields(memory[i],bodies[i],false);
                uint32_t next, previous; memcpy(&next,memory[i].data()+0x88,4); memcpy(&previous,memory[i].data()+0x8c,4);
                bodies[i].next = index(next); bodies[i].previous = index(previous);
            }
            const uint8_t hold = bodies[subject_index].hold; uint8_t updates = 99;
            if (!nm_traffic_near(math,bodies.data(),bodies.size(),reset_count,player_index,subject_index,ahead_progress,&updates) || updates)
                throw std::runtime_error("native traffic proximity rejected actual inputs");
            for (unsigned i = 0; i < memory.size(); ++i) traffic_near_fields(memory[i],bodies[i],true);
            near_distinct_subject += subject_index != player_index; near_hold_changed += bodies[subject_index].hold != hold;
            near_pending = true; ++near_entered;
        }
        if (target != 0x1225c) return;
        if (pending || near_pending || timers_pending || pass_pending || cpu.m_IP != 0x12250) throw std::runtime_error("unexpected traffic order entry");
        timers.short_ticks = std::bit_cast<int16_t>(b.read_word(0x501894));
        timers.control = std::bit_cast<int32_t>(b.read_dword(0x501124));
        timers.event[0] = std::bit_cast<int32_t>(b.read_dword(0x501a9c)); timers.event[1] = std::bit_cast<int32_t>(b.read_dword(0x501aa4));
        const unsigned sort_count = b.read_dword(0x501080), reset = b.read_dword(0x501084);
        if (sort_count < 3 || sort_count > 44 || reset < 2 || reset >= sort_count) throw std::runtime_error("invalid live traffic counts");
        const uint32_t flags = b.read_dword(0x501090); lap = std::bit_cast<int8_t>(b.read_byte(0x5014ba));
        addresses.clear();
        const auto include = [&](uint32_t at) {
            if (at < 0x500000 || at > 0x5ffd00 || at%16) {
                std::fprintf(stderr,"traffic snapshot frame=%llu pointer=%08x subject=%08x player=%08x subject_next=%08x\n",
                    (unsigned long long)b.frame(),at,cpu.m_r[29],b.read_dword(0x501220),b.read_dword(cpu.m_r[29]+0x88));
                throw std::runtime_error("invalid traffic body pointer");
            }
            if (index(at) == UINT16_MAX) addresses.push_back(at);
        };
        const uint32_t base = b.read_dword(0x501224), stride = b.read_dword(base+8);
        if (stride < 0x300 || uint64_t(base)+uint64_t(stride)*reset > 0x600000) throw std::runtime_error("invalid traffic reset pool");
        for (unsigned i = 0; i < reset; ++i) include(base+stride*i);
        reset_count = reset;
        for (unsigned j = 0; j < 44; ++j) {
            ranks[j] = b.read_dword(0x503080+4*j); track[j] = b.read_dword(0x503130+4*j); scratch[j] = b.read_dword(0x5031e0+4*j);
            if (j >= sort_count) continue;
            for (uint32_t at : {track[j], flags&0x500000u ? ranks[j] : track[j]}) {
                include(at);
            }
        }
        const uint32_t player = b.read_dword(0x501220), subject = cpu.m_r[29];
        include(player); include(subject);
        player_index = index(player); subject_index = index(subject);
        passing = {index(b.read_dword(0x50183c)),index(b.read_dword(0x501840)),b.read_word(0x501a04)};
        memory.resize(addresses.size()); std::vector<nm_traffic_order_body_t> bodies(addresses.size());
        for (unsigned i = 0; i < addresses.size(); ++i) {
            for (unsigned j = 0; j < 0x300; ++j) memory[i][j] = b.read_byte(addresses[i]+j);
            traffic_order_fields(memory[i],bodies[i],false);
            bodies[i].next = index(b.read_dword(addresses[i]+0x88)); bodies[i].previous = index(b.read_dword(addresses[i]+0x8c));
        }
        uint16_t rank_indices[44], track_indices[44];
        for (unsigned j = 0; j < 44; ++j) { rank_indices[j] = index(ranks[j]); track_indices[j] = index(track[j]); }
        if(sort_count==40 && reset+1==sort_count) {
            field_count=sort_count;field_order.lap=lap;field_shared.flags=flags;
            field_shared.profile_updates=b.read_byte(0x5014e0);
            field_shared.crash_counter=b.read_byte(0x503680);
            field_sequence=b.read_dword(b.read_dword(b.read_byte(0x503690) ? 0x2896e58 : 0x2896e5c)+4);
            uint64_t seen=0;
            for(unsigned i=0;i<field_count;++i) {
                const unsigned at=track_indices[i],id=memory[at][4];
                if(id>=field_count || (seen&(UINT64_C(1)<<id))) throw std::runtime_error("native field traffic IDs not dense");
                seen|=UINT64_C(1)<<id;
                auto &native=field_cars[id];
                PlayerMemory initial;initial.entity=memory[at];initial.sequence=field_sequence;
                for(unsigned j=0;j<4;++j) initial.signals[j]=b.read_byte(0x501a02+j);
                initial.shake=b.read_byte(0x501830);initial.sequence_counter=field_shared.crash_counter;
                const uint32_t auxiliary=b.read_dword(b.read_dword(0x501270));
                memcpy(initial.crash_auxiliary.data(),&auxiliary,4);
                initial.fields(native.car,false);
                native.effects.correction_yaw=std::bit_cast<int16_t>(b.read_word(addresses[at]+0x216));
                native.other_yaw=std::bit_cast<int16_t>(b.read_word(addresses[at]+0x212));
                nm_collision_rank_t collision{};collision_rank_fields(memory[at],collision,false);
                native.relative=collision.relative;native.collision_motion=collision.motion;
                native.collision_adjusted=collision.adjusted;native.collision_candidates=collision.candidates;
                native.collision_side=collision.contact_side;
                native.next=b.read_byte(b.read_dword(addresses[at]+0x88)+4);
                native.previous=b.read_byte(b.read_dword(addresses[at]+0x8c)+4);
                native.rank=bodies[at].rank;native.near_marker=std::bit_cast<int16_t>(b.read_word(addresses[at]+0x146));
                field_order.track[i]=(uint16_t)id;
                field_order.rank[i]=rank_indices[i]<memory.size() ? memory[rank_indices[i]][4] : UINT16_MAX;
            }
            const auto before=field_cars;const auto prior_order=field_order;const auto prior_shared=field_shared;
            field_order.track[0]=44;const auto invalid_order=field_order;
            if(nm_race_traffic_step(field_cars.data(),field_count,&field_order,&field_shared,math) ||
               memcmp(before.data(),field_cars.data(),sizeof(field_cars)) ||
               memcmp(&invalid_order,&field_order,sizeof(field_order)) || memcmp(&prior_shared,&field_shared,sizeof(field_shared)))
                throw std::runtime_error("native field traffic invalid index changed state");
            field_order=prior_order;
            field_cars[0].car.control=(uint32_t)timers.control;field_shared.event_timer=timers.short_ticks;
            field_cars[0].car.boundary.damage.signal=nm_yaw_angle(passing.signal);
            field_events[0]=timers.event[0];field_events[1]=timers.event[1];
            field_passing={(uint16_t)(passing.next<memory.size() ? memory[passing.next][4] : UINT16_MAX),
                (uint16_t)(passing.previous<memory.size() ? memory[passing.previous][4] : UINT16_MAX),passing.signal};
            interaction_cars=field_cars;interaction_order=field_order;interaction_shared=field_shared;
            std::copy_n(field_events,2,interaction_events);interaction_passing=field_passing;
            if(!nm_race_traffic_step(field_cars.data(),field_count,&field_order,&field_shared,math))
                throw std::runtime_error("native field traffic rejected actual field");
            field_signal_count=field_count;
        }
        if (bodies[track_indices[0]].lap > lap) {
            unsigned prefix = 1;
            while (prefix < 4 && prefix < sort_count && bodies[track_indices[prefix]].lap == bodies[track_indices[0]].lap && bodies[track_indices[prefix]].profile < 8) ++prefix;
            for (unsigned j = 0; j < prefix; ++j) scratch[j] = track[j];
            ++rotations;
        }
        ranking += (flags&0x500000u)!=0;
        if (!nm_traffic_order(bodies.data(),bodies.size(),rank_indices,track_indices,sort_count,reset+1,flags,&lap))
            throw std::runtime_error("native traffic order rejected actual inputs");
        for (unsigned i = 0; i < addresses.size(); ++i) {
            traffic_order_fields(memory[i],bodies[i],true);
            if (bodies[i].next != UINT16_MAX) memcpy(memory[i].data()+0x88,&addresses[bodies[i].next],4);
            if (bodies[i].previous != UINT16_MAX) memcpy(memory[i].data()+0x8c,&addresses[bodies[i].previous],4);
        }
        for (unsigned j = 0; j < sort_count; ++j) {
            if (flags&0x500000u) ranks[j] = addresses[rank_indices[j]];
            track[j] = addresses[track_indices[j]];
        }
        // Resolve from the retained native POST-order links, not stale source
        // links before122dc. A worker's null chain reads ROM[88]=ROM[18]=0;
        // validate/fold those immutable inputs here, never add a fake car or
        // a source-address/bus lookup to the Dreamcast operation.
        uint32_t following = subject;
        for (unsigned i = 0; i < 2; ++i) {
            if (following) {
                const unsigned at = index(following);
                if (at >= memory.size()) throw std::runtime_error("unmapped traffic progress subject");
                memcpy(&following,memory[at].data()+0x88,4);
            }
            if (!following) {
                if (b.read_dword(0x88) || b.read_dword(0x18)) throw std::runtime_error("unexpected traffic immutable zero chain");
                ahead_progress[i] = 0; ++near_rom_inputs;
            } else {
                const unsigned at = index(following);
                if (at >= memory.size()) throw std::runtime_error("unmapped traffic progress neighbor");
                memcpy(&ahead_progress[i],memory[at].data()+0x18,4);
            }
        }
        pending = true; latest = false; ++entered;
    }
    void report() const {
        if(field_signal_count) throw std::runtime_error("unfinished native field signals");
        std::printf("native_field_signals_live checked=%llu (native traffic outputs feed timers/passing without refresh)\n",
            (unsigned long long)field_signals_checked);
        if(field_count) throw std::runtime_error("unfinished native field traffic");
        std::printf("native_field_traffic_live checked=%llu (40 cars, rank/ring/near flags/marker-gear alias/shared quota)\n",
            (unsigned long long)field_checked);
        if (timers_pending || pass_pending || timers_entered != timers_checked || pass_entered != pass_checked)
            throw std::runtime_error("unfinished race signal checks");
        std::printf("native_race_signals_live timers=%llu/%llu passing=%llu/%llu signal_changes=%llu (native ordered bodies/shared inputs, not autonomous ticks)\n",
            (unsigned long long)timers_checked,(unsigned long long)timers_entered,(unsigned long long)pass_checked,(unsigned long long)pass_entered,(unsigned long long)pass_signals);
        if (pending || entered != checked) throw std::runtime_error("unfinished traffic ordering");
        std::printf("native_traffic_order_live checked=%llu/%llu ranking=%llu rotations=%llu native_links_fed=%llu (whole1225c/122dc; native outputs feed proximity and collision preparation)\n",
            (unsigned long long)checked,(unsigned long long)entered,(unsigned long long)ranking,(unsigned long long)rotations,(unsigned long long)fed_links);
        if (near_pending || near_entered != near_checked || entered != near_entered) throw std::runtime_error("unfinished traffic proximity");
        std::printf("native_traffic_near_live checked=%llu/%llu distinct_subject=%llu hold_changes=%llu immutable_inputs=%llu native_bodies_fed=%llu (whole12464/139d8 memory after native ordering, no refresh; not autonomous race)\n",
            (unsigned long long)near_checked,(unsigned long long)near_entered,(unsigned long long)near_distinct_subject,(unsigned long long)near_hold_changed,(unsigned long long)near_rom_inputs,(unsigned long long)fed_bodies);
    }
};

struct LiveCollisionCheck {
    LiveTrafficOrderCheck traffic;
    std::vector<std::array<uint8_t,0x300>> preparation_memory;
    std::vector<uint32_t> preparation_addresses;
    std::array<uint8_t,0x300> preparation_record{};
    uint32_t preparation_record_address = 0;
    bool preparation_pending = false;
    uint64_t preparation_entered = 0, preparation_checked = 0, preparation_no_pair = 0, preparation_outside = 0;
    uint64_t field_preparation_checked=0;
    uint16_t field_collision_order[14]{};
    bool field_collision_pending=false;
    nm_race_collisions_t field_collisions{};
    std::vector<std::array<uint8_t,0x300>> field_collision_memory;
    std::array<uint8_t,0x300> field_collision_record{};
    uint64_t field_collision_checked=0,field_collision_pairs=0,field_collision_responses=0;
    std::vector<nm_collision_walk_body_t> walk_bodies;
    nm_collision_walk_t walk{};
    uint16_t walk_order[14]{}, walk_pair[2]{};
    unsigned walk_primary = 0, walk_phase = 0, walk_cost = 0;
    bool walk_active = false;
    uint64_t walk_entered = 0, walk_finished = 0, walk_pairs = 0;
    uint64_t walk_circle_checked=0,walk_circle_queued=0;
    uint16_t walk_signal = 0;
    std::vector<std::array<uint32_t,5>> scene_collision_contexts;
    uint64_t scene_identity_mismatches=0,scene_phase_mismatches=0;
    void update_walk_probe(unsigned i) {
        const auto &memory = preparation_memory[i]; auto &probe = walk_bodies[i].probe;
        memcpy(&probe.position[0],memory.data()+0x1c,4); memcpy(&probe.position[1],memory.data()+0x24,4);
        memcpy(&probe.word5c,memory.data()+0x5c,4);
    }
    void finish_walk_pair(unsigned cost) {
        if (!walk_active) return;
        if (!walk.pending || walk_cost) throw std::runtime_error("unexpected native walk response boundary");
        for (unsigned j = 0; j < 2; ++j) {
            preparation_memory[walk_pair[j]] = expected[j]; update_walk_probe(walk_pair[j]);
        }
        preparation_record = record; walk_cost = cost;
    }
    void begin_preparation(rt::M2Board &b, const rt::Cpu &cpu,const nm_drive_params_t *math,const nm_road_pack_t *road) {
        if (preparation_pending || detection_pending || route_pending || walk_active)
            throw std::runtime_error("collision preparation overlaps previous pass");
        if (traffic.timers_pending || traffic.pass_pending || traffic.passing.signal != b.read_word(0x501a04))
            throw std::runtime_error("collision preparation lacks native race signal");
        traffic.check_timers(b); walk_signal = traffic.passing.signal;
        const uint32_t base = b.read_dword(0x501224), reset = b.read_dword(0x501084);
        const uint32_t stride = b.read_dword(base+8), primary_address = b.read_dword(0x5012f0);
        if (!reset || reset >= UINT16_MAX || stride < 0x300 || uint64_t(base)+uint64_t(stride)*reset > 0x600000)
            throw std::runtime_error("invalid live collision pool");
        const std::array<uint32_t,5> context={b.read_dword(0x501090),b.read_dword(cpu.m_r[29]),
            b.read_byte(b.read_dword(0x501220)+0x6d),b.read_byte(0x501464),b.read_byte(0x503692)};
        if(std::find(scene_collision_contexts.begin(),scene_collision_contexts.end(),context)==scene_collision_contexts.end())
            scene_collision_contexts.push_back(context);
        scene_phase_mismatches+=context[4]!=(b.read_dword(0x5010b0)&3);
        scene_identity_mismatches+=b.read_byte(primary_address+4)!=0;
        for(unsigned i=0;i<reset;++i) scene_identity_mismatches+=b.read_byte(base+stride*i+4)!=i+1;
        preparation_addresses.clear();
        for (unsigned i = 0; i < reset; ++i) preparation_addresses.push_back(base+stride*i);
        if (std::find(preparation_addresses.begin(),preparation_addresses.end(),primary_address) == preparation_addresses.end()) {
            preparation_addresses.push_back(primary_address); ++preparation_outside;
        }
        // ponytail: linear pointer-to-index lookup in a bounded host diagnostic;
        // use an address map only if much larger reference pools make it costly.
        const auto index = [&](uint32_t pointer) -> uint16_t {
            auto it = std::find(preparation_addresses.begin(),preparation_addresses.end(),pointer);
            return it == preparation_addresses.end() ? UINT16_MAX : uint16_t(it-preparation_addresses.begin());
        };
        const unsigned count = preparation_addresses.size(); preparation_memory.resize(count);
        std::vector<nm_collision_rank_t> bodies(count);
        for (unsigned i = 0; i < count; ++i) {
            const uint32_t at = preparation_addresses[i];
            if (at < 0x500000 || at > 0x5ffd00 || at%16) throw std::runtime_error("invalid preparation body address");
            preparation_memory[i] = traffic.body(b,at);
            collision_rank_fields(preparation_memory[i],bodies[i],false);
            bodies[i].next = index(traffic.link(b,at,0x88)); bodies[i].previous = index(traffic.link(b,at,0x8c));
        }
        uint16_t order[14];
        if (!nm_collision_prepare(bodies.data(),count,reset,index(primary_address),b.read_dword(0x501434),order))
            throw std::runtime_error("native preparation rejected actual linked pool");
        for (unsigned i = 0; i < count; ++i) collision_rank_fields(preparation_memory[i],bodies[i],true);
        if(count==40 && reset==39 && b.read_byte(primary_address+4)==0) {
            uint16_t native_order[14];std::fill_n(native_order,14,UINT16_MAX);
            const auto before=traffic.field_cars;
            traffic.field_cars[0].previous=40;const auto invalid=traffic.field_cars;
            if(nm_race_collision_prepare(traffic.field_cars.data(),count,b.read_dword(0x501434),native_order) ||
               memcmp(invalid.data(),traffic.field_cars.data(),sizeof(invalid)) ||
               std::any_of(native_order,native_order+14,[](uint16_t v){return v!=UINT16_MAX;}))
                throw std::runtime_error("native field collision invalid link changed state/window");
            traffic.field_cars=before;
            if(!nm_race_collision_prepare(traffic.field_cars.data(),count,b.read_dword(0x501434),native_order))
                throw std::runtime_error("native field collision preparation rejected current field");
            for(unsigned i=0;i<count;++i) {
                const unsigned id=preparation_memory[i][4];
                if(id>=count) throw std::runtime_error("native collision car ID outside field");
                const auto &car=traffic.field_cars[id];
                auto native=bodies[i];native.flags=car.car.boundary.damage.flags;native.relative=car.relative;
                native.motion=car.collision_motion;native.adjusted=car.collision_adjusted;
                native.candidates=car.collision_candidates;native.contact_side=car.collision_side;
                auto expected=preparation_memory[i];collision_rank_fields(expected,native,true);
                if(expected!=preparation_memory[i])
                    throw std::runtime_error("native field collision preparation differs from source-checked body");
            }
            for(unsigned i=0;i<14;++i) if(native_order[i]!=preparation_memory[order[i]][4])
                throw std::runtime_error("native field collision window order differs");
            if(road->storage) {
                memcpy(field_collision_order,native_order,sizeof(native_order));field_collision_pending=true;
            }
            ++field_preparation_checked;
        }
        preparation_record_address = cpu.m_r[29];
        if (preparation_record_address < 0x500000 || preparation_record_address > 0x5ffd00 || preparation_record_address%16)
            throw std::runtime_error("invalid preparation record address");
        preparation_record = traffic.body(b,preparation_record_address);
        for (unsigned i = 0; i < 14; ++i) memcpy(preparation_record.data()+0x5c+4*i,&preparation_addresses[order[i]],4);
        if(field_collision_pending) {
            auto &shared=traffic.field_shared;
            shared.heading_circle[0]=nm_force_float(b.tgp().diagnostic_data_word(0x220));
            shared.heading_circle[1]=nm_force_float(b.tgp().diagnostic_data_word(0x221));
            shared.heading_circle[2]=nm_force_float(b.tgp().diagnostic_data_word(0x223));
            field_collisions={};collision_contact_fields(preparation_record,field_collisions.contact,false);
            auto interaction_collisions=field_collisions;
            std::copy_n(shared.heading_circle,3,traffic.interaction_shared.heading_circle);
            if(b.read_dword(0x15310)!=0x0a000000 || b.read_dword(0x501434)!=road->profile_count-1)
                throw std::runtime_error("race interaction source no-op/profile wrap contract changed");
            if(!nm_race_interactions_step(traffic.interaction_cars.data(),count,&traffic.interaction_shared,
                &traffic.interaction_order,traffic.interaction_events,&traffic.interaction_passing,
                &interaction_collisions,math,road,b.read_dword(0x501a80),context[4],context[1],b.read_byte(0x501464)))
                throw std::runtime_error("native race interaction sequence rejected live input");
            const nm_collision_damage_input_t in={shared.flags,traffic.field_cars[0].car.control,
                traffic.field_cars[0].car.lap.completed,b.read_byte(0x501464)};
            if(!nm_race_collide(traffic.field_cars.data(),count,&shared,math,road,
                field_collision_order,context[4],context[1],&in,&field_collisions))
                throw std::runtime_error("native field rejected complete live collision pass");
            if(memcmp(traffic.interaction_cars.data(),traffic.field_cars.data(),sizeof(traffic.field_cars)) ||
               memcmp(&traffic.interaction_shared,&shared,sizeof(shared)) ||
               memcmp(&traffic.interaction_order,&traffic.field_order,sizeof(traffic.field_order)) ||
               memcmp(traffic.interaction_events,traffic.field_events,sizeof(traffic.field_events)) ||
               memcmp(&traffic.interaction_passing,&traffic.field_passing,sizeof(traffic.field_passing)) ||
               memcmp(&interaction_collisions,&field_collisions,sizeof(field_collisions)))
                throw std::runtime_error("native race interaction sequence differs from source-checked phases");
            field_collision_memory=preparation_memory;field_collision_record=preparation_record;
            collision_contact_fields(field_collision_record,field_collisions.contact,true);
            for(unsigned i=0;i<count;++i) {
                auto &car=traffic.field_cars[preparation_memory[i][4]];
                PlayerMemory out;out.entity=preparation_memory[i];out.sequence=traffic.field_sequence;
                out.fields(car.car,true);field_collision_memory[i]=out.entity;
                field_collision_memory[i][0x27d]=car.collision_side;
                memcpy(field_collision_memory[i].data()+0x216,&car.effects.correction_yaw,2);
                memcpy(field_collision_memory[i].data()+0x212,&car.other_yaw,2);
            }
        }
        // Retain the native pool/record across EVERY pair when all response
        // operations are available. Partial checks must not claim traversal.
        if (road->storage) {
            walk_bodies.resize(count); memcpy(walk_order,order,sizeof(order));
            walk = {}; walk.budget = 1000; walk_cost = 0;
            walk.centre[0]=nm_force_float(b.tgp().diagnostic_data_word(0x220));
            walk.centre[1]=nm_force_float(b.tgp().diagnostic_data_word(0x221));
            walk.circle_radius=nm_force_float(b.tgp().diagnostic_data_word(0x223));
            walk_primary = index(primary_address); walk_phase = b.read_byte(0x503692);
            for (unsigned i = 0; i < count; ++i) {
                walk_bodies[i].next = bodies[i].next; walk_bodies[i].candidates = bodies[i].candidates; update_walk_probe(i);
            }
            walk_active = true; ++walk_entered;
        }
        preparation_pending = true; ++preparation_entered;
    }
    void finish_preparation(rt::M2Board &b, const rt::Cpu &cpu) {
        if (!preparation_pending || cpu.m_r[29] != preparation_record_address)
            throw std::runtime_error("unexpected preparation observation boundary");
        for (unsigned i = 0; i < preparation_memory.size(); ++i) for (unsigned k = 0; k < 0x300; ++k)
            if (preparation_memory[i][k] != b.read_byte(preparation_addresses[i]+k))
                throw std::runtime_error("live collision preparation differs from whole pool memory");
        for (unsigned k = 0; k < 0x300; ++k)
            if (preparation_record[k] != b.read_byte(preparation_record_address+k))
                throw std::runtime_error("live collision preparation differs from whole record memory");
        preparation_pending = false; ++preparation_checked;
    }
    std::array<std::array<uint8_t,0x300>,2> expected{};
    std::array<uint8_t,0x300> record{};
    uint32_t address[2]{}, record_address = 0, return_ip = 0;
    uint64_t entered[2]{}, checked[2]{}, player_pairs = 0;
    bool pending = false;
    bool reaction_pending = false;
    uint64_t reaction_entered = 0, reaction_checked = 0;
    int16_t relative_angle = 0;
    nm_collision_damage_input_t damage_input{};
    std::array<uint8_t,32> damage_auxiliary{};
    std::array<uint8_t,0x100> damage_globals{};
    uint32_t damage_auxiliary_address = 0, damage_sequence = 0;
    int32_t severity = 0;
    int8_t damage_shake = 0;
    uint8_t damage_counter = 0;
    uint64_t damage_entered = 0, damage_checked = 0, crash_starts = 0, sound_intents = 0;
    bool damage_pending = false;
    bool impulse_pending = false;
    uint64_t impulse_entered = 0, impulse_checked = 0;
    nm_collision_impulse_shared_t impulse_shared{};
    bool choice_pending = false;
    uint64_t choice_entered = 0, choice_checked[2]{};
    bool route_pending = false, route_dispatch_pending = false, route_full_response = false;
    nm_collision_route_t route{};
    uint64_t route_entered = 0, route_returned = 0, route_memory_checked = 0, route_skipped[2]{};
    uint64_t candidate_checked = 0, candidate_stale = 0, candidate_changed_decisions = 0;
    bool detection_pending = false;
    uint64_t detection_entered = 0, detection_checked[4]{}, detection_cached[2]{}, detection_signals = 0;
    uint16_t detection_signal = 0;
    unsigned detection_flags = 0;
    void finish_detection(rt::M2Board &b, const rt::Cpu &cpu) {
        if (!detection_pending || cpu.m_r[24] != address[0] || cpu.m_r[25] != address[1] ||
            cpu.m_r[29] != record_address || b.read_word(0x501a02) != detection_signal)
            throw std::runtime_error("collision detection boundary/shared signal mismatch");
        compare(b); ++detection_checked[detection_flags]; detection_pending = false;
    }
    void capture_pair(rt::M2Board &b, const rt::Cpu &cpu) {
        address[0] = cpu.m_r[24]; address[1] = cpu.m_r[25]; record_address = cpu.m_r[29];
        const auto valid = [](uint32_t at) { return at >= 0x500000 && at <= 0x5ffd00 && at%16 == 0; };
        if (!valid(address[0]) || !valid(address[1]) || !valid(record_address) ||
            record_address == address[0] || record_address == address[1])
            throw std::runtime_error("invalid collision pair/record address");
        if (walk_active) {
            for (unsigned j = 0; j < 2; ++j) expected[j] = preparation_memory[walk_pair[j]];
            record = preparation_record; compare(b); // Native outputs, never a fresh reference pair snapshot.
        } else {
            for (unsigned body = 0; body < 2; ++body) for (unsigned i = 0; i < expected[body].size(); ++i)
                expected[body][i] = b.read_byte(address[body]+i);
            for (unsigned i = 0; i < record.size(); ++i) record[i] = b.read_byte(record_address+i);
        }
    }
    void observe_return(rt::M2Board &b, const rt::Cpu &cpu, uint32_t source, const nm_drive_params_t *math) {
        if (source == 0x121c0) traffic.finish_timers(b,cpu);
        if (source == 0x126bc) traffic.finish_pass(b,cpu);
        if (source == 0x12588) traffic.finish_near(b,cpu);
        if (source == 0x1417c && preparation_pending) { finish_preparation(b,cpu); ++preparation_no_pair; }
        if (source == 0x1417c && walk_active) {
            uint16_t pair[2]{};
            if (nm_collision_walk_next(math,walk_bodies.data(),walk_bodies.size(),walk_order,walk_primary,walk_phase,walk_cost,&walk,pair) != 0)
                throw std::runtime_error("native traversal has an extra pair after reference return");
            std::array<uint32_t,3> final_circle={b.tgp().diagnostic_data_word(0x220),
                b.tgp().diagnostic_data_word(0x221),b.tgp().diagnostic_data_word(0x223)};
            const auto &queued=b.tgp().diagnostic_pending_input();
            if(!queued.empty()) {
                //140a4..140d8 may enqueue40 then return on budget without a
                //FIFO read. Compare the exact command/arguments, not stale
                //TGP registers. Never sync/consume or alter reference timing.
                if(walk.budget>0 || queued.size()!=4 || queued[0]!=0x20004040)
                    throw std::runtime_error("unexpected pending TGP work at collision return");
                std::copy_n(queued.begin()+1,3,final_circle.begin());++walk_circle_queued;
            }
            if(nm_force_bits(walk.centre[0])!=final_circle[0] ||
               nm_force_bits(walk.centre[1])!=final_circle[1] ||
               nm_force_bits(walk.circle_radius)!=final_circle[2]) {
                std::fprintf(stderr,"collision circle frame=%llu centre_id=%u outer=%u remaining=%u budget=%d native=%08x/%08x/%08x source=%08x/%08x/%08x\n",
                    (unsigned long long)b.frame(),walk.centre_id,walk.outer,walk.remaining,walk.budget,
                    nm_force_bits(walk.centre[0]),nm_force_bits(walk.centre[1]),nm_force_bits(walk.circle_radius),
                    b.tgp().diagnostic_data_word(0x220),b.tgp().diagnostic_data_word(0x221),b.tgp().diagnostic_data_word(0x223));
                std::fprintf(stderr,"collision queued input words=%zu:",queued.size());
                for(uint32_t word:queued) std::fprintf(stderr," %08x",word);
                std::fprintf(stderr,"\n");
                throw std::runtime_error("native collision final circle owner differs from original TGP");
            }
            ++walk_circle_checked;
            if(field_collision_pending) {
                const auto &player=traffic.field_cars[0].car;
                if(field_collision_memory!=preparation_memory) {
                    for(unsigned i=0;i<preparation_memory.size();++i) for(unsigned j=0;j<0x300;++j)
                        if(field_collision_memory[i][j]!=preparation_memory[i][j]) {
                            std::fprintf(stderr,"field collision mismatch frame=%llu car=%u offset=%x native=%02x reference=%02x pairs=%u responses=%u\n",
                                (unsigned long long)b.frame(),preparation_memory[i][4],j,field_collision_memory[i][j],
                                preparation_memory[i][j],field_collisions.pairs,field_collisions.responses);
                            throw std::runtime_error("native field collision body differs");
                        }
                }
                if(field_collision_memory!=preparation_memory || field_collision_record!=preparation_record ||
                   (uint16_t)player.boundary.pivot_signal!=b.read_word(0x501a02) ||
                   (uint16_t)player.boundary.damage.signal!=b.read_word(0x501a04) ||
                   (uint8_t)player.boundary.damage.shake_ticks!=b.read_byte(0x501830) ||
                   traffic.field_shared.crash_counter!=b.read_byte(0x503680) ||
                   player.auxiliary_flags!=b.read_dword(b.read_dword(0x501270)) ||
                   nm_force_bits(traffic.field_shared.heading_circle[0])!=final_circle[0] ||
                   nm_force_bits(traffic.field_shared.heading_circle[1])!=final_circle[1] ||
                   nm_force_bits(traffic.field_shared.heading_circle[2])!=final_circle[2])
                    throw std::runtime_error("native field complete collision pass differs from source-checked memory/shared state");
                field_collision_pending=false;++field_collision_checked;
                field_collision_pairs+=field_collisions.pairs;field_collision_responses+=field_collisions.responses;
            }
            for (unsigned i = 0; i < preparation_memory.size(); ++i) for (unsigned j = 0; j < 0x300; ++j)
                if (preparation_memory[i][j] != b.read_byte(preparation_addresses[i]+j))
                    throw std::runtime_error("native traversal final whole pool mismatch");
            for (unsigned j = 0; j < 0x300; ++j) if (preparation_record[j] != b.read_byte(preparation_record_address+j))
                throw std::runtime_error("native traversal final record mismatch");
            walk_active = false; ++walk_finished;
        }
        if (source == 0x14644 || source == 0x14680) {
            if (!route_pending || route_dispatch_pending || pending || reaction_pending || damage_pending || impulse_pending ||
                cpu.m_r[19] != route.caller_result || cpu.m_r[24] != address[0] || cpu.m_r[25] != address[1] || cpu.m_r[29] != record_address)
                throw std::runtime_error("collision dispatch return differs from native result");
            // Without road data only separation was imported by this checker.
            // Still check the dispatch/g3 return, but do not compare stale
            // separation-only memory after the unobserved response effects.
            if (route_full_response) { compare(b); ++route_memory_checked; }
            finish_walk_pair(route.caller_result);
            route_pending = false; ++route_returned;
        }
        if (source == 0x14688) {
            if (route_pending || pending || reaction_pending || damage_pending || impulse_pending)
                throw std::runtime_error("collision no-response return while response is pending");
            finish_detection(b,cpu);
            const auto in = collision_route_input(record,expected[0],expected[1]);
            nm_collision_route_t skipped{};
            if (!nm_collision_route(math,&in,nullptr,nullptr,&skipped) || skipped.branch != NM_COLLISION_NONE ||
                cpu.m_r[19] != skipped.caller_result) throw std::runtime_error("collision no-response rule/g3 differs");
            ++route_skipped[in.flags != 0]; // Detection memory already checked above; now validate dispatch/g3.
            finish_walk_pair(skipped.caller_result);
        }
        if (source != 0x14e6c || !impulse_pending) return;
        if (cpu.m_IP != return_ip+8 || cpu.m_r[24] != address[0] || cpu.m_r[25] != address[1] ||
            cpu.m_r[29] != record_address || cpu.m_r[20] != uint32_t(severity))
            throw std::runtime_error("collision impulse return/register mismatch");
        compare(b);
        if (b.read_word(0x501a04) != impulse_shared.signal || b.read_byte(0x501830) != uint8_t(impulse_shared.shake_ticks))
            throw std::runtime_error("collision impulse shared state differs");
        for (unsigned i = 0; i < damage_globals.size(); ++i)
            if (damage_globals[i] != b.read_byte(0x501000+i)) throw std::runtime_error("collision impulse changed global memory");
        for (unsigned i = 0; i < damage_auxiliary.size(); ++i)
            if (damage_auxiliary[i] != b.read_byte(damage_auxiliary_address+i)) throw std::runtime_error("collision impulse changed auxiliary memory");
        if (damage_counter != b.read_byte(0x503680)) throw std::runtime_error("collision impulse changed sequence counter");
        walk_signal = impulse_shared.signal;
        ++impulse_checked; impulse_pending = false;
    }
    void compare(rt::M2Board &b) const {
        for (unsigned body = 0; body < 2; ++body) for (unsigned i = 0; i < expected[body].size(); ++i)
            if (expected[body][i] != b.read_byte(address[body]+i))
                throw std::runtime_error("live collision chain differs from whole entity memory");
        for (unsigned i = 0; i < record.size(); ++i) if (record[i] != b.read_byte(record_address+i))
            throw std::runtime_error("collision chain unexpectedly changed contact record");
    }
    void observe(rt::M2Board &b, const rt::Cpu &cpu, uint32_t target, const nm_drive_params_t *math,
                 const nm_road_pack_t *road) {
        traffic.observe(b,cpu,target,math);
        if (target == 0x13f08) begin_preparation(b,cpu,math,road);
        if (target == 0x14180) {
            if (preparation_pending) finish_preparation(b,cpu);
            if (cpu.m_IP != 0x14150 && cpu.m_IP != 0x14164)
                throw std::runtime_error("unexpected collision broad-phase caller");
            if (walk_active) {
                if (nm_collision_walk_next(math,walk_bodies.data(),walk_bodies.size(),walk_order,walk_primary,walk_phase,walk_cost,&walk,walk_pair) != 1 ||
                    cpu.m_r[24] != preparation_addresses[walk_pair[0]] || cpu.m_r[25] != preparation_addresses[walk_pair[1]] ||
                    cpu.m_r[15] != uint32_t(walk.budget))
                    throw std::runtime_error("native traversal actual pair/order/work-budget mismatch");
                walk_cost = 0; ++walk_pairs;
            }
            nm_collision_probe_t probes[2]{};
            for (unsigned body = 0; body < 2; ++body) {
                //14140..14148 puts the player in g9 AFTER the broad-phase test.
                const uint32_t at = cpu.m_r[24+(body^(cpu.m_IP == 0x14150))];
                probes[body].position[0] = nm_force_float(b.read_dword(at+0x1c));
                probes[body].position[1] = nm_force_float(b.read_dword(at+0x24));
                probes[body].word5c = b.read_dword(at+0x5c);
            }
            const nm_collision_probe_t current_circle = probes[1];
            probes[1].position[0] = nm_force_float(b.tgp().diagnostic_data_word(0x220));
            probes[1].position[1] = nm_force_float(b.tgp().diagnostic_data_word(0x221));
            if (b.tgp().diagnostic_data_word(0x223) != 0x40a54413)
                throw std::runtime_error("live collision circle radius changed");
            // Read-only reference snapshot, NOT target TGP state. The native
            // outer loop must retain its own centre across pair responses.
            if (memcmp(probes[1].position,current_circle.position,sizeof(current_circle.position))) {
                ++candidate_stale;
                unsigned fresh;
                if (!nm_collision_candidate(math,&probes[0],&current_circle,&fresh))
                    throw std::runtime_error("invalid live current-circle diagnostic");
                if (!fresh && !candidate_changed_decisions)
                    std::printf("native_collision_cached_center_counterexample frame=%llu point=%08x/%08x current=%08x/%08x cached=%08x/%08x\n",
                        (unsigned long long)b.frame(),nm_force_bits(probes[0].position[0]),nm_force_bits(probes[0].position[1]),
                        nm_force_bits(current_circle.position[0]),nm_force_bits(current_circle.position[1]),
                        nm_force_bits(probes[1].position[0]),nm_force_bits(probes[1].position[1]));
                candidate_changed_decisions += !fresh;
            }
            unsigned candidate;
            if (!nm_collision_candidate(math,&probes[0],&probes[1],&candidate) || !candidate) {
                std::fprintf(stderr,"candidate mismatch frame=%llu ip=%x g8=%x g9=%x current=%08x/%08x,%08x/%08x words=%08x/%08x TGPcircle=%08x/%08x radius=%08x\n",
                    (unsigned long long)b.frame(),cpu.m_IP,cpu.m_r[24],cpu.m_r[25],
                    nm_force_bits(probes[0].position[0]),nm_force_bits(probes[0].position[1]),
                    nm_force_bits(probes[1].position[0]),nm_force_bits(probes[1].position[1]),
                    probes[0].word5c,probes[1].word5c,b.tgp().diagnostic_data_word(0x220),
                    b.tgp().diagnostic_data_word(0x221),b.tgp().diagnostic_data_word(0x223));
                throw std::runtime_error("native broad-phase rejected actual contact-test pair");
            }
            ++candidate_checked;
            if (detection_pending || route_pending || pending || reaction_pending || damage_pending || impulse_pending)
                throw std::runtime_error("collision detection began while previous pair pending");
            capture_pair(b,cpu);
            nm_collision_shape_t shape[2]{}; nm_collision_contact_t contact{};
            for (unsigned j = 0; j < 2; ++j) {
                collision_shape_fields(expected[j],shape[j],false);
                detection_cached[j] += (shape[j].flags&32)!=0;
            }
            collision_contact_fields(record,contact,false);
            const uint16_t prior_signal = b.read_word(0x501a02); detection_signal = prior_signal;
            if (!nm_collision_detect(math,&shape[0],&shape[address[0] == address[1] ? 0 : 1],
                    address[1] == b.read_dword(0x5012f0),&contact,&detection_signal))
                throw std::runtime_error("native detection rejected live pair");
            if (address[0] == address[1]) shape[1] = shape[0];
            for (unsigned j = 0; j < 2; ++j) collision_shape_fields(expected[j],shape[j],true);
            collision_contact_fields(record,contact,true);
            detection_flags = contact.flags; detection_signals += detection_signal != prior_signal;
            detection_pending = true; ++detection_entered;
        }
        if (target == 0x14904 || target == 0x148dc || target == 0x148f0) {
            const uint32_t expected_ip = target == 0x14904 ? 0x14640 : target == 0x148dc ? 0x14674 : 0x1467c;
            if (cpu.m_IP != expected_ip || route_pending || choice_pending || pending || reaction_pending || damage_pending || impulse_pending)
                throw std::runtime_error("unexpected collision-route caller/pending state");
            finish_detection(b,cpu);
            const auto first = collision_choice_body(expected[0]), second = collision_choice_body(expected[1]);
            const auto in = collision_route_input(record,expected[0],expected[1]);
            for (unsigned corner = 0; corner < 4; ++corner)
                if (b.read_dword(0x235b08+4*corner) != 0xf4+12u*math->collision_corner[corner])
                    throw std::runtime_error("collision corner map differs from original offsets");
            if (!nm_collision_route(math,&in,&first,&second,&route) || route.branch == NM_COLLISION_NONE ||
                (target != 0x14904 && route.branch != (target == 0x148f0)))
                throw std::runtime_error("collision route rejected/differs from actual response");
            route_pending = route_dispatch_pending = true; ++route_entered;
            route_full_response = road->storage != nullptr;
            if (target == 0x14904) { ++choice_entered; choice_pending = true; }
        }
        if (target == 0x14c54 && (cpu.m_IP == 0x148ec || cpu.m_IP == 0x14900) && road->storage) {
            if (!damage_pending || cpu.m_IP != return_ip+8 || cpu.m_r[24] != address[0] || cpu.m_r[25] != address[1] ||
                cpu.m_r[29] != record_address || cpu.m_r[20] != uint32_t(severity))
                throw std::runtime_error("collision damage return/register mismatch");
            compare(b);
            for (unsigned i = 0; i < damage_auxiliary.size(); ++i)
                if (damage_auxiliary[i] != b.read_byte(damage_auxiliary_address+i))
                    throw std::runtime_error("collision damage auxiliary memory differs");
            for (unsigned i = 0; i < damage_globals.size(); ++i)
                if (damage_globals[i] != b.read_byte(0x501000+i)) throw std::runtime_error("collision damage changed global memory");
            if ((uint8_t)damage_shake != b.read_byte(0x501830) || damage_counter != b.read_byte(0x503680))
                throw std::runtime_error("collision damage shared state differs");
            ++damage_checked; damage_pending = false;
            nm_collision_impulse_body_t bodies[2]{};
            for (unsigned body = 0; body < 2; ++body) collision_impulse_fields(expected[body],bodies[body],false);
            nm_collision_impulse_input_t in{};
            memcpy(&in.flags,record.data(),4); memcpy(&in.contact_flags,record.data()+0x34,4); in.severity = severity;
            impulse_shared.shake_ticks = damage_shake;
            if (!nm_collision_impulse(math,&in,&bodies[0],&bodies[address[0] == address[1] ? 0 : 1],&impulse_shared))
                throw std::runtime_error("native impulse rejected actual collision input");
            if (address[0] == address[1]) bodies[1] = bodies[0];
            for (unsigned body = 0; body < 2; ++body) collision_impulse_fields(expected[body],bodies[body],true);
            ++impulse_entered; impulse_pending = true;
        }
        if (target == 0x14fcc && (cpu.m_IP == 0x148e8 || cpu.m_IP == 0x148fc) && road->storage) {
            if (!reaction_pending || cpu.m_IP != return_ip+4 || cpu.m_r[24] != address[0] ||
                cpu.m_r[25] != address[1] || cpu.m_r[29] != record_address ||
                cpu.m_r[19] != uint32_t(int32_t(relative_angle)))
                throw std::runtime_error("collision reaction return/register mismatch");
            compare(b); ++reaction_checked; reaction_pending = false;
            nm_collision_damage_body_t bodies[2]{};
            for (unsigned body = 0; body < 2; ++body) {
                collision_damage_fields(expected[body],bodies[body],damage_sequence,false);
                bodies[body].damage.shake_ticks = damage_shake;
                bodies[body].damage.sequence_counter = damage_counter;
            }
            if (!nm_collision_damage(math,&damage_input,&bodies[0],&bodies[address[0] == address[1] ? 0 : 1],&severity))
                throw std::runtime_error("native collision damage rejected actual input");
            if (address[0] == address[1]) bodies[1] = bodies[0];
            const auto &s = bodies[1].damage;
            crash_starts += s.sequence_counter != damage_counter;
            sound_intents += s.sound_requests != 0;
            damage_shake = s.shake_ticks; damage_counter = s.sequence_counter;
            for (unsigned body = 0; body < 2; ++body) collision_damage_fields(expected[body],bodies[body],damage_sequence,true);
            if (s.auxiliary_request) {
                uint32_t flags; memcpy(&flags,damage_auxiliary.data(),4); flags |= 0x80000000u;
                memcpy(damage_auxiliary.data(),&flags,4);
                const uint32_t function = 0x5c14; memcpy(damage_auxiliary.data()+12,&function,4);
            }
            ++damage_entered; damage_pending = true;
        }
        if (target == 0x15238 && (cpu.m_IP == 0x148e4 || cpu.m_IP == 0x148f8)) {
            if (!pending || cpu.m_IP != return_ip) throw std::runtime_error("collision return without entry");
            compare(b);
            ++checked[return_ip == 0x148f8]; pending = false;
            if (road->storage) {
                // Feed the native separation result forward, never refresh pair
                // state from the reference between these two operations.
                nm_collision_reaction_t bodies[2]{};
                for (unsigned body = 0; body < 2; ++body) {
                    auto &s = bodies[body]; const auto &memory = expected[body];
                    memcpy(&s.position[0],memory.data()+0x1c,4); memcpy(&s.position[1],memory.data()+0x24,4);
                    memcpy(&s.heading,memory.data()+0x48,2); memcpy(&s.profile,memory.data()+0x128,2);
                    memcpy(&s.lateral,memory.data()+0x13e,2); memcpy(&s.correction_ticks,memory.data()+0x214,1);
                    s.initial_inhibit = memory[0x20d];
                    if (s.profile >= road->profile_count) throw std::runtime_error("collision profile outside native road pack");
                    const uint32_t at = b.read_dword(0x501448)+164*s.profile;
                    const auto *bytes = reinterpret_cast<const uint8_t *>(road->profiles+s.profile);
                    for (unsigned i = 0; i < 164; ++i) if (bytes[i] != b.read_byte(at+i))
                        throw std::runtime_error("collision profile differs from immutable native road pack");
                }
                if (!nm_collision_react(math,road,&bodies[0],&bodies[address[0] == address[1] ? 0 : 1],&relative_angle))
                    throw std::runtime_error("native reaction rejected actual collision input");
                if (address[0] == address[1]) bodies[1] = bodies[0];
                for (unsigned body = 0; body < 2; ++body) {
                    memcpy(expected[body].data()+0x13e,&bodies[body].lateral,2);
                    expected[body][0x214] = (uint8_t)bodies[body].correction_ticks;
                    expected[body][0x20d] = bodies[body].initial_inhibit;
                }
                ++reaction_entered; reaction_pending = true;
            }
        }
        if (target != 0x14a5c && target != 0x14b58) return;
        const unsigned branch = target == 0x14b58;
        if (pending || reaction_pending || damage_pending || impulse_pending || cpu.m_IP != (branch ? 0x148f4u : 0x148e0u))
            throw std::runtime_error("unhandled collision separation caller");
        if (route_dispatch_pending) {
            if (route.branch != branch || cpu.m_r[24] != address[0] || cpu.m_r[25] != address[1] || cpu.m_r[29] != record_address)
                throw std::runtime_error("native collision route differs from selected response");
            compare(b); route_dispatch_pending = false;
            if (choice_pending) { ++choice_checked[branch]; choice_pending = false; }
            // Preserve the native entry snapshot into separation, no refresh.
        } else capture_pair(b,cpu);
        if (road->storage) {
            traffic.check_timers(b);
            damage_input.global_flags = b.read_dword(0x501090); damage_input.control = uint32_t(traffic.timers.control);
            damage_input.player_mode = b.read_byte(b.read_dword(0x501220)+0x6d); damage_input.curve = b.read_byte(0x501464);
            damage_shake = std::bit_cast<int8_t>(b.read_byte(0x501830)); damage_counter = b.read_byte(0x503680);
            if (walk_signal != b.read_word(0x501a04)) throw std::runtime_error("native collision signal changed outside imported chain");
            impulse_shared.signal = walk_signal;
            damage_sequence = b.read_dword(b.read_dword(b.read_byte(0x503690) ? 0x2896e58 : 0x2896e5c)+4);
            damage_auxiliary_address = b.read_dword(0x501270);
            if (damage_auxiliary_address < 0x500000 || damage_auxiliary_address > 0x5fffe0 || damage_auxiliary_address%16)
                throw std::runtime_error("invalid collision damage auxiliary address");
            for (unsigned i = 0; i < damage_auxiliary.size(); ++i) damage_auxiliary[i] = b.read_byte(damage_auxiliary_address+i);
            for (unsigned i = 0; i < damage_globals.size(); ++i) damage_globals[i] = b.read_byte(0x501000+i);
        }
        nm_collision_body_t bodies[2]{};
        for (unsigned body = 0; body < 2; ++body) {
            memcpy(&bodies[body].position[0],expected[body].data()+0x1c,4);
            memcpy(&bodies[body].position[1],expected[body].data()+0x24,4);
            bodies[body].side = expected[body][0x27d];
        }
        auto *plus = bodies+branch, *minus = address[0] == address[1] ? plus : bodies+(1-branch);
        if (!nm_collision_separate(math,b.read_dword(record_address+(branch ? 0x4c : 0x38)),
            nm_force_float(b.read_dword(record_address+(branch ? 0x98 : 0x9c))),
            b.read_dword(record_address+(branch ? 0x50 : 0x3c)),plus,minus))
            throw std::runtime_error("native separation rejected live collision");
        if (address[0] == address[1]) bodies[1-branch] = *plus;
        for (unsigned body = 0; body < 2; ++body) {
            memcpy(expected[body].data()+0x1c,&bodies[body].position[0],4);
            memcpy(expected[body].data()+0x24,&bodies[body].position[1],4);
            expected[body][0x27d] = bodies[body].side;
        }
        const uint32_t player = b.read_dword(0x501220);
        player_pairs += address[0] == player || address[1] == player;
        ++entered[branch]; return_ip = branch ? 0x148f8 : 0x148e4; pending = true;
    }
    void report() const {
        traffic.report();
        if(field_collision_pending) throw std::runtime_error("unfinished native field collision pass");
        std::printf("native_field_collide_live checked=%llu pairs=%llu responses=%llu (native interactions sequence and traffic/signals/preparation feed full pass, no car refresh)\n",
            (unsigned long long)field_collision_checked,(unsigned long long)field_collision_pairs,
            (unsigned long long)field_collision_responses);
        for(const auto &c:scene_collision_contexts)
            std::printf("native_collision_scene_context global=%08x record=%08x player_mode=%u curve=%u phase=%u\n",c[0],c[1],c[2],c[3],c[4]);
        std::printf("native_collision_scene_identity mismatches=%llu phase_frame_mismatches=%llu\n",
            (unsigned long long)scene_identity_mismatches,(unsigned long long)scene_phase_mismatches);
        if (walk_active || walk_entered != walk_finished) throw std::runtime_error("unfinished native collision walk");
        std::printf("native_collision_circle_live checked=%llu queued=%llu (last source circle registers or exact pending command40, including empty passes; no forced sync)\n",
            (unsigned long long)walk_circle_checked,(unsigned long long)walk_circle_queued);
        std::printf("native_collision_walk_live checked=%llu/%llu pairs=%llu (all accepted-pair order/entry budgets, native pool retained through skipped pairs and full responses; final whole pool/record; zero means NO full-response coverage)\n",
            (unsigned long long)walk_finished,(unsigned long long)walk_entered,(unsigned long long)walk_pairs);
        if (preparation_pending || preparation_entered != preparation_checked)
            throw std::runtime_error("unfinished collision preparation check");
        std::printf("native_field_collision_prepare_live checked=%llu (native traffic/signals feed14-car window, no field refresh)\n",
            (unsigned long long)field_preparation_checked);
        std::printf("native_collision_prepare_live checked=%llu/%llu primary_outside=%llu no_pair=%llu (whole pool/record before first contact or at empty-pass return; not traversal proof)\n",
            (unsigned long long)preparation_checked,(unsigned long long)preparation_entered,
            (unsigned long long)preparation_outside,(unsigned long long)preparation_no_pair);
        if (detection_pending || detection_entered != detection_checked[0]+detection_checked[1]+detection_checked[2]+detection_checked[3])
            throw std::runtime_error("unfinished collision detection check");
        std::printf("native_collision_detect_live checked=%llu flags0/1/2/3=%llu/%llu/%llu/%llu cached_first/second=%llu/%llu signal_changes=%llu (whole pair/record to dispatch or no-response return)\n",
            (unsigned long long)detection_entered,(unsigned long long)detection_checked[0],
            (unsigned long long)detection_checked[1],(unsigned long long)detection_checked[2],(unsigned long long)detection_checked[3],
            (unsigned long long)detection_cached[0],(unsigned long long)detection_cached[1],(unsigned long long)detection_signals);
        std::printf("native_collision_candidate_live checked=%llu cached_center_changed=%llu decision_changed=%llu (actual14180 accepted pairs only; no skipped-pair/list coverage)\n",
            (unsigned long long)candidate_checked,(unsigned long long)candidate_stale,(unsigned long long)candidate_changed_decisions);
        if (route_pending || route_dispatch_pending || route_entered != route_returned)
            throw std::runtime_error("unfinished collision route check");
        std::printf("native_collision_route_live responses=%llu/%llu whole_memory=%llu skipped_empty/aligned=%llu/%llu (parent return/g3; whole response memory requires road data; detection checked separately)\n",
            (unsigned long long)route_returned,(unsigned long long)route_entered,
            (unsigned long long)route_memory_checked,
            (unsigned long long)route_skipped[0],(unsigned long long)route_skipped[1]);
        if (choice_pending || choice_entered != choice_checked[0]+choice_checked[1])
            throw std::runtime_error("unfinished collision choice check");
        std::printf("native_collision_choice_live checked=%llu first/second=%llu/%llu (actual14904 entry to selected separation, unchanged whole pair/record; zero means NO coverage)\n",
            (unsigned long long)choice_entered,(unsigned long long)choice_checked[0],(unsigned long long)choice_checked[1]);
        if (pending || reaction_pending || damage_pending || impulse_pending || impulse_entered != impulse_checked || damage_entered != damage_checked || reaction_entered != reaction_checked || entered[0] != checked[0] || entered[1] != checked[1])
            throw std::runtime_error("unfinished collision separation check");
        std::printf("native_collision_live branch14a5c=%llu/%llu branch14b58=%llu/%llu player_pairs=%llu (whole pair/record memory; zero means NO coverage, not collision selection)\n",
            (unsigned long long)checked[0],(unsigned long long)entered[0],(unsigned long long)checked[1],
            (unsigned long long)entered[1],(unsigned long long)player_pairs);
        std::printf("native_collision_reaction_live checked=%llu/%llu (native separation fed into lateral/timers/g3; zero means NO coverage, not full response)\n",
            (unsigned long long)reaction_checked,(unsigned long long)reaction_entered);
        std::printf("native_collision_damage_live checked=%llu/%llu crashes=%llu sound_intents=%llu (native three-operation chain, whole pair/record/shared state/g4; not14c54 or autonomous gameplay)\n",
            (unsigned long long)damage_checked,(unsigned long long)damage_entered,(unsigned long long)crash_starts,(unsigned long long)sound_intents);
        std::printf("native_collision_impulse_live checked=%llu/%llu (native four-operation chain to actual14c54 return; whole pair/record/shared state, not contact selection or autonomous gameplay)\n",
            (unsigned long long)impulse_checked,(unsigned long long)impulse_entered);
    }
};

struct LivePlayerCheck {
    PlayerMemory movement_memory;
    nm_player_tick_input_t movement_input{};
    nm_checkpoint_award_t movement_award{};
    bool rolling_mode = false, finish_mode = false;
    unsigned observed_race_phase = 256;
    PlayerMemory expected;
    uint32_t entity = 0, auxiliary_address[2]{}, crash_auxiliary_address = 0;
    uint64_t entered = 0, checked = 0, spins = 0, guided = 0, events = 0;
    std::array<uint64_t,0x300/4> between_words{};
    std::array<uint64_t,0x300> between_bytes{};
    uint64_t effect_calls = 0, effect_transitions = 0, effect_max_calls = 0;
    unsigned pending_effect_calls = 0;
    uint64_t transitions = 0, between_yaw = 0, between_ahead = 0, between_heights = 0, between_profiles = 0;
    bool pending = false;
    nm_player_t player{};
    nm_road_pack_t road{};
    std::vector<uint8_t> road_marks;
    nm_road_marks_t marks{};
    std::array<nm_path_pack_t,8> paths{};
    uint64_t responses[4]{}, crash_events = 0, surface_updates = 0;
    uint64_t held_steps = 0, manual_steps = 0, active_animations = 0, pre_sounds = 0;
    uint64_t corrections = 0, shakes = 0, inhibits = 0, early_sounds = 0;
    uint64_t offcourse_starts = 0, offcourse_advanced = 0;
    uint64_t checkpoint_steps = 0, checkpoint_visits = 0, lap_writes = 0;
    uint64_t route_transitions = 0;
    uint64_t field_player_checked=0,field_player_neighbors=0;
    nm_checkpoint_award_t award{};
    unsigned award_cues_seen = 0;
    uint64_t awards = 0, award_sounds = 0;
    std::array<uint32_t,3> outside_pose{};
    uint32_t previous_call = 0, previous_return = 0;
    uint64_t outside_pose_changes = 0;
    ~LivePlayerCheck() {
        nm_road_unload(&road);
        for (auto &path : paths) nm_path_unload(&path);
    }
    void load_road(const char *file, const char *directory) {
        if (!nm_road_load(&road,file) || road.slot >= 3) throw std::runtime_error("invalid player road course");
        road_marks.resize(road.profile_count-1);
        marks.values = road_marks.data(); marks.count = (uint32_t)road_marks.size();
        for (unsigned route = 0; route < paths.size(); ++route) {
            const auto filename = std::filesystem::path(directory)/
                ("course"+std::to_string(road.slot)+"-route"+std::to_string(route)+".dnp");
            if (!nm_path_load(&paths[route],filename.c_str()) || paths[route].slot != road.slot || paths[route].route != route)
                throw std::runtime_error("player needs all eight matching paths");
        }
    }
    std::array<std::array<uint64_t,4>,128> pose_intervals{};
    std::array<uint32_t,4> last_pose{};
    uint32_t pose_entity = 0, pose_return = 0;
    uint64_t pose_frame = 0;
    void observe_external_pose(rt::M2Board &b, const rt::Cpu &cpu, uint32_t target) {
        // Observe our player even while another entity owns g13. Attribute only
        // to the interval after a call, not an unobserved individual store.
        if (checked && !pending) {
            const std::array<uint32_t,3> pose = {b.read_dword(entity+0x1c),b.read_dword(entity+0x20),b.read_dword(entity+0x24)};
            if (pose != outside_pose) {
                if (++outside_pose_changes <= 16)
                    std::printf("native_player_external_pose frame=%llu after_target=%05x return=%05x before_target=%05x return=%05x active_entity=%08x\n",
                        (unsigned long long)b.frame(),previous_call,previous_return,target,cpu.m_IP,cpu.m_r[29]);
                outside_pose = pose;
            }
        }
        previous_call = target; previous_return = cpu.m_IP;
    }
    void observe_pose(rt::M2Board &b, const rt::Cpu &cpu) {
        // Top-level active-player calls only. Attribute changes to intervals,
        // not individual stores: a callee and intervening caller code both
        // belong to the interval. Slot 0 covers other frames/entity lifetimes.
        if (cpu.m_IP < 0xd834 || cpu.m_IP > 0xd9f8) return;
        const uint32_t at = cpu.m_r[29];
        if (at < 0x500000 || at > 0x5ffd00 || at%16)
            throw std::runtime_error("invalid player pose audit address");
        const std::array<uint32_t,4> pose = {b.read_dword(at+0x1c),
            b.read_dword(at+0x20),b.read_dword(at+0x24),b.read_word(at+0x58)};
        if (pose_entity == at) {
            const unsigned slot = pose_frame == b.frame() ? (pose_return-0xd830)/4 : 0;
            for (unsigned i = 0; i < pose.size(); ++i) pose_intervals[slot][i] += pose[i] != last_pose[i];
        }
        last_pose = pose; pose_entity = at; pose_return = cpu.m_IP; pose_frame = b.frame();
    }
    void begin(rt::M2Board &b, uint32_t at, const nm_drive_params_t *math, const nm_contact_pack_t *contact, bool rolling = false, bool finishing = false) {
        finish_mode=finishing;
        rolling_mode = rolling || finishing;
        if (pending || at < 0x500000 || at > 0x5ffd00 || at%16) throw std::runtime_error("invalid player chain entry");
        const bool continuous = checked && entity == at;
        entity = at;
        const unsigned race_phase=b.read_byte(0x5010a4);
        if (race_phase!=observed_race_phase) {
            std::printf("native_player_clock_entry rolling=%u frame=%llu phase=%u clock=%u remaining=%u gate204=%u mode74=%u hold=%u blocked=%u\n",
                rolling_mode,(unsigned long long)b.frame(),race_phase,
                b.read_dword(0x5010cc),b.read_dword(0x5010d0),b.read_word(at+0x204),
                b.read_byte(0x501074),b.read_dword(0x501a84),b.read_dword(0x501854));
            observed_race_phase=race_phase;
        }
        if (road.storage) {
            if (b.read_dword(0x501434) != marks.count) throw std::runtime_error("player mark count differs from DNR2");
            marks.timer = b.read_dword(0x53b718);
            for (unsigned i = 0; i < marks.count; ++i) marks.values[i] = b.read_byte(0x53b71c+i);
        }
        for (unsigned i = 0; i < expected.entity.size(); ++i) expected.entity[i] = b.read_byte(at+i);
        for (unsigned i = 0; i < 8; ++i) {
            expected.yaw[i] = b.read_byte(0x5018a4+i); expected.ahead[i] = b.read_byte(0x501864+i);
        }
        for (unsigned i = 0; i < 16; ++i) expected.heights[i] = b.read_byte(0x501100+i);
        expected.profile_updates = b.read_byte(0x5014e0);
        for (unsigned i = 0; i < expected.globals.size(); ++i) expected.globals[i] = b.read_byte(0x501000+i);
        for (unsigned i = 0; i < expected.checkpoint_extra.size(); ++i) expected.checkpoint_extra[i] = b.read_byte(0x501400+i);
        for (unsigned i = 0; i < expected.signals.size(); ++i) expected.signals[i] = b.read_byte(0x501a02+i);
        expected.last_surface = b.read_dword(0x5018ac); expected.center_mask = b.read_word(0x5018b0);
        expected.shake = b.read_byte(0x501830); expected.sequence_counter = b.read_byte(0x503680);
        expected.shake_angles[0] = nm_yaw_angle(b.read_word(0x501834));
        expected.shake_angles[1] = nm_yaw_angle(b.read_word(0x501300));
        expected.sequence = b.read_dword(b.read_dword(0x2896e58)+4);
        for (unsigned i = 0; i < expected.camera.size(); ++i) expected.camera[i] = b.read_byte(0x501710+i);
        expected.control = b.read_dword(0x501124);
        expected.camera_mode = std::bit_cast<int32_t>(b.read_dword(0x5014a4));
        crash_auxiliary_address = b.read_dword(0x501270);
        if (crash_auxiliary_address < 0x500000 || crash_auxiliary_address > 0x5fffe0 || crash_auxiliary_address%16)
            throw std::runtime_error("invalid player crash event pointer");
        for (unsigned i = 0; i < expected.crash_auxiliary.size(); ++i)
            expected.crash_auxiliary[i] = b.read_byte(crash_auxiliary_address+i);
        for (unsigned a = 0; a < 2; ++a) {
            const uint32_t address = b.read_dword(0x501274+4*a);
            if (address < 0x500000 || address > 0x5ffff0 || address%16) throw std::runtime_error("invalid player event pointer");
            auxiliary_address[a] = address;
            for (unsigned i = 0; i < 16; ++i) expected.auxiliary[16*a+i] = b.read_byte(address+i);
        }
        if (continuous) {
            // Compare last native outputs to the NEXT real entry. Only mapped
            // fields are overlaid; unrelated guest bytes cannot inflate counts.
            // This locates remaining caller/tail work, never repairs native state.
            auto carried = expected;
            carried.fields(player,true);
            if (std::memcmp(carried.entity.data()+0x1c,expected.entity.data()+0x1c,12))
                std::printf("native_player_between_pose frame=%llu entity=%08x (outside the checked block)\n",
                    (unsigned long long)b.frame(),at);
            for (unsigned i = 0; i < between_words.size(); ++i)
                between_words[i] += std::memcmp(carried.entity.data()+4*i,expected.entity.data()+4*i,4) != 0;
            if (rolling_mode)
                for (unsigned i = 0; i < between_bytes.size(); ++i)
                    between_bytes[i] += carried.entity[i] != expected.entity[i];
            between_yaw += carried.yaw != expected.yaw;
            between_ahead += carried.ahead != expected.ahead;
            between_heights += carried.heights != expected.heights;
            between_profiles += carried.profile_updates != expected.profile_updates;
            if (rolling_mode) {
                auto advanced = player;
                for (unsigned i=0;i<pending_effect_calls;++i) { // 6c0c: 9014, then 9088 -> 9148
                    nm_player_effect_timers(&advanced); (void)nm_player_contact_clear(&advanced);
                }
                auto after_effects = expected;
                after_effects.fields(advanced,true);
                if (after_effects.entity != expected.entity)
                    throw std::runtime_error("rolling between-tick entity changes not explained by observed effect calls");
                effect_calls += pending_effect_calls;
                effect_max_calls = std::max(effect_max_calls,(uint64_t)pending_effect_calls);
                ++effect_transitions;
                player = advanced;
            }
            ++transitions;
        }
        pending_effect_calls = 0;
        if (!rolling_mode || !continuous) {
            player = {}; expected.fields(player,false);
        } else {
            // The separate camera updater owns these shared inputs. Keep the
            // car state persistent; do not disguise a full per-tick reseed.
            memcpy(player.camera_words,expected.camera.data()+16,sizeof(player.camera_words));
            memcpy(player.camera_angles,expected.camera.data()+40,sizeof(player.camera_angles));
            player.camera_transition=expected.camera[3];
            player.camera_mode=expected.camera_mode;
            // d564 UI writes501300, also mapped as the second shake angle.
            player.shake_angles[1]=expected.shake_angles[1];
            // Effect entity can retire independently (observed at frame3028).
            memcpy(&player.auxiliary_flags,expected.crash_auxiliary.data(),sizeof(player.auxiliary_flags));
        }
        int16_t award_signal = player.boundary.pivot_signal;
        award = {}; checkpoint_award_fields(expected.entity,expected.globals,expected.checkpoint_extra,award,award_signal,false);
        award_cues_seen = 0;
        nm_player_active_input_t active{}; auto &in = active.dynamics;
        active.force_recovery = (b.read_dword(0x53e584)>>4)&1;
        const auto f32 = [&](unsigned offset) { return std::bit_cast<float>(b.read_dword(at+offset)); };
        in.speed_flags = b.read_dword(0x501090);
        in.speed_index = std::bit_cast<int32_t>(b.read_dword(at+0x18));
        in.motion_scale = f32(0x1f8); in.guided_scale = f32(0x238);
        in.curve = b.read_byte(0x501464); in.course_slot = b.read_byte(0x501460);
        in.fixed_offset = b.read_byte(0x5018a8); in.lap = b.read_byte(at+0x6d); in.entity_id = b.read_byte(at+4);
        in.guided_mode = b.read_byte(0x5014f7);
        ++entered; spins += player.boundary.damage.spin_ticks != 0;
        guided += !player.boundary.damage.spin_ticks && player.guided_ticks > 0;
        const nm_path_pack_t *path = nullptr;
        if (road.storage && (rolling_mode || player.boundary.recovery_ticks <= 0)) {
            const uint32_t desc = b.read_dword(0x3a48+4*road.slot);
            const uint32_t table = b.read_dword(desc+16), source = b.read_dword(at+0x130);
            for (auto &candidate : paths) if (b.read_dword(table+8*candidate.route) == source) {
                if (b.read_word(at+0x12c) != candidate.count) throw std::runtime_error("player path count mismatch");
                path = &candidate; break;
            }
        }
        if (road.storage) {
            if (b.read_word(0x501434) != road.profile_count-1)
                throw std::runtime_error("player checkpoint wrap differs from road pack");
            const uint32_t checkpoints = b.read_dword(0x50143c);
            if (b.read_dword(checkpoints) != road.checkpoint_count)
                throw std::runtime_error("player checkpoint count differs from road pack");
            for (unsigned i = 0; i < road.checkpoint_count; ++i)
                if (b.read_word(checkpoints+4+4*i) != road.checkpoints[i])
                    throw std::runtime_error("player checkpoint differs from road pack");
            active.controls.steer = std::bit_cast<int32_t>(b.read_dword(0x501050));
            const unsigned accel = b.read_word(0x501054), brake = b.read_word(0x501058);
            if (accel > 255 || brake > 255) throw std::runtime_error("decoded player pedals outside byte domain");
            active.controls.accel = (uint8_t)accel; active.controls.brake = (uint8_t)brake;
            active.input_counter = std::bit_cast<int32_t>(b.read_dword(0x5010d0));
            active.gear_selector = (b.read_dword(0x53e120)>>20)&7;
            active.proximity_disabled = b.read_dword(0x501a80) != 0;
            active.variant = b.read_byte(0x503690);
            const int view = std::bit_cast<int8_t>(expected.camera[0]), offset = 0x233+20*view;
            if (offset < 0 || offset >= 0x4000) throw std::runtime_error("player camera descriptor outside backup RAM");
            active.restored_camera_mode = std::bit_cast<int8_t>(b.read_byte(0x1d00000+offset));
            if (!rolling_mode && !active.proximity_disabled && player.boundary.recovery_ticks <= 0) {
                uint32_t neighbor = b.read_dword(at+0x88);
                for (unsigned n = 0; n < 3; ++n) {
                    if (neighbor < 0x500000 || neighbor > 0x5ffd00 || neighbor%16)
                        throw std::runtime_error("invalid player neighbor pointer");
                    active.neighbor[n][0] = std::bit_cast<float>(b.read_dword(neighbor+0x1c));
                    active.neighbor[n][1] = std::bit_cast<float>(b.read_dword(neighbor+0x24));
                    neighbor = b.read_dword(neighbor+0x88);
                }
            }
            manual_steps += in.curve != 0; active_animations += player.boundary.damage.remaining != 0;
            corrections += player.correction_ticks != 0; shakes += player.boundary.damage.shake_ticks != 0;
            inhibits += player.input_inhibit != 0;
        }
        if (rolling_mode && !finish_mode && entered==1 && !road.slot && path && !path->route && road.grid_count) {
            auto values=road_marks;
            nm_road_marks_t initial_marks{values.data(),marks.count,0};
            nm_practice_t initial{};float height;
            if (!nm_practice_reset(&initial,math,contact,&road,path,&initial_marks) ||
                !nm_player_rolling_entry_pose(road.profiles+(uint16_t)initial.car.phase[0],&initial.car,&height))
                throw std::runtime_error("native rolling reset diagnostic rejected assets");
            initial.car.boundary.speed=nm_force_float(b.read_dword(0x28b574c));
            auto candidate=expected;candidate.fields(initial.car,true);
            unsigned differences=0;
            for(unsigned offset=0;offset<expected.entity.size();offset+=4) {
                uint32_t native,reference;
                memcpy(&native,candidate.entity.data()+offset,4);memcpy(&reference,expected.entity.data()+offset,4);
                if(native!=reference) {
                    ++differences;
                    std::printf("native_rolling_initial_difference offset=%03x native=%08x reference=%08x\n",offset,native,reference);
                }
            }
            std::printf("native_rolling_initial_inventory frame=%llu differences=%u native/profile/rpm/gear=%d/%d/%u reference=%d/%d/%u (diagnostic, NOT equivalence)\n",
                (unsigned long long)b.frame(),differences,initial.car.phase[0],initial.car.rpm,initial.car.gear,
                player.phase[0],player.rpm,player.gear);
        }
        // Missing guided path/profile must fail, not silently use planar motion.
        nm_player_tick_input_t tick{}; tick.active = active;
        tick.phase = b.read_byte(0x5010a4); tick.award.blocked = b.read_dword(0x501854);
        tick.award.penalty = b.read_dword(0x501a80); tick.award.suppression = b.read_byte(0x501074);
        const unsigned difficulty = b.read_word(0x53e59c);
        if (difficulty > 255) throw std::runtime_error("checkpoint difficulty outside byte domain");
        tick.award.difficulty = (uint8_t)difficulty;
        movement_memory=expected;movement_input=tick;movement_award=award;
        const bool check_field=road.storage && !rolling_mode && !finish_mode;
        std::array<nm_race_car_t,44> player_field{},player_field_before{};
        nm_race_shared_t player_shared{};
        nm_checkpoint_award_t field_award=award;
        auto field_marks_values=road_marks;
        nm_road_marks_t field_marks{field_marks_values.data(),marks.count,marks.timer};
        if(check_field) {
            const unsigned count=b.read_dword(0x501080);
            if(!count || count>44 || b.read_byte(at+4)) throw std::runtime_error("invalid native player field");
            player_field[0].car=player;player_field[0].guided_scale=in.guided_scale;
            player_field[0].route=path ? path->route : 0;
            player_shared.flags=in.speed_flags;player_shared.guided_mode=in.guided_mode;
            player_shared.clock=player.checkpoints.clock;player_shared.profile_updates=player.profile_updates;
            player_shared.crash_counter=player.boundary.damage.sequence_counter;
            if(!active.proximity_disabled && player.boundary.recovery_ticks<=0) {
                uint32_t address=at;unsigned id=0;
                for(unsigned n=0;n<3;++n) {
                    address=b.read_dword(address+0x88);
                    if(address<0x500000 || address>0x5ffd00 || address%16)
                        throw std::runtime_error("native player field has invalid neighbor address");
                    const unsigned next=b.read_byte(address+4);
                    if(next>=count) throw std::runtime_error("native player field has invalid neighbor ID");
                    player_field[id].next=(uint16_t)next;id=next;
                    player_field[id].car.boundary.ground.position[0]=nm_force_float(b.read_dword(address+0x1c));
                    player_field[id].car.boundary.ground.position[2]=nm_force_float(b.read_dword(address+0x24));
                    ++field_player_neighbors;
                }
            }
            // Poison duplicate views: the native field/shared owners must win.
            player_field[0].car.checkpoints.clock^=0x1234;
            player_field[0].car.profile_updates^=0x7f;
            player_field[0].car.boundary.damage.sequence_counter^=0x5a;
            auto field_tick=tick;
            field_tick.active.dynamics.speed_flags=0;field_tick.active.dynamics.guided_mode=255;
            field_tick.active.dynamics.guided_scale=-99;field_tick.active.dynamics.course_slot=255;
            field_tick.active.dynamics.entity_id=255;field_tick.active.dynamics.lap=255;
            for(auto &neighbor:field_tick.active.neighbor) for(float &axis:neighbor) axis=nm_force_float(0x7fc00000);
            player_field_before=player_field;
            if(!active.proximity_disabled && player.boundary.recovery_ticks<=0) {
                player_field[0].next=(uint16_t)count;
                const auto invalid=player_field;const auto prior_shared=player_shared;
                if(nm_race_player_step(player_field.data(),count,&player_shared,math,contact,&road,path,
                    &field_tick,&field_award,&field_marks) ||
                   memcmp(player_field.data(),invalid.data(),sizeof(invalid)) ||
                   memcmp(&player_shared,&prior_shared,sizeof(prior_shared)) ||
                   memcmp(&field_award,&award,sizeof(award)) || field_marks_values!=road_marks || field_marks.timer!=marks.timer)
                    throw std::runtime_error("native field player invalid neighbor changed state");
                player_field=player_field_before;
            }
            if(!nm_race_player_step(player_field.data(),count,&player_shared,math,contact,&road,path,
                &field_tick,&field_award,&field_marks)) throw std::runtime_error("native field player rejected live turn");
        }
        const uint8_t entry_spin = player.boundary.damage.spin_ticks;
        const int accepted = finish_mode ? nm_player_finish_prefix(math,contact,&road,path,&active,&player) :
            rolling_mode ? nm_player_rolling_prefix(math,contact,&road,path,&active,&player) :
            road.storage ? nm_player_tick_step(math,contact,&road,path,&tick,&player,&award,&marks) :
            nm_player_dynamics_step(math,contact,nullptr,nullptr,&in,&player);
        if (!accepted) {
            std::fprintf(stderr,"player chain rejected frame=%llu spin=%u guided=%d speed=%g\n",
                (unsigned long long)b.frame(),player.boundary.damage.spin_ticks,player.guided_ticks,player.boundary.speed);
            throw std::runtime_error("native player block rejected actual entry");
        }
        if(check_field) {
            player_field_before[0].car=player;
            if(memcmp(player_field.data(),player_field_before.data(),sizeof(player_field)) ||
               memcmp(&field_award,&award,sizeof(award)) || field_marks_values!=road_marks || field_marks.timer!=marks.timer ||
               player_shared.clock!=player.checkpoints.clock || player_shared.profile_updates!=player.profile_updates ||
               player_shared.crash_counter!=player.boundary.damage.sequence_counter ||
               memcmp(player_shared.heading_circle,player.proximity_circle,sizeof(player.proximity_circle)))
                throw std::runtime_error("native field player differs from source-checked tick/shared outputs");
            ++field_player_checked;
        }
        if (road.storage && !rolling_mode) {
            held_steps += player.held;
            route_transitions += player.route_transition;
            checkpoint_steps += !player.held;
            checkpoint_visits += player.lap.counter != expected.entity[0x6c];
            lap_writes += player.lap_write.written;
            offcourse_starts += !player.held && player.boundary.recovery_ticks == 64;
            offcourse_advanced += !player.held && player.offcourse_ticks != 0;
            if (!player.held) ++responses[player.boundary.action];
            pre_sounds += player.pre_sound_requests != 0;
            early_sounds += player.early_sound_request != 0;
            surface_updates += std::memcmp(expected.entity.data()+0x91,player.surface_ticks,4) != 0;
        }
        expected.fields(player,true);
        if (road.storage && !rolling_mode) {
            checkpoint_award_fields(expected.entity,expected.globals,expected.checkpoint_extra,award,player.boundary.pivot_signal,true);
            expected.yaw[4] = entry_spin;
            awards += award.cue_count != 0; award_sounds += award.cue_count;
        }
        if (road.storage && player.boundary.damage.auxiliary_request) {
            ++crash_events;
            expected.crash_auxiliary[3] |= 0x80;
            const uint32_t function = 0x5c14;
            std::memcpy(expected.crash_auxiliary.data()+12,&function,4);
        }
        if (player.force.trigger_pair) {
            ++events;
            for (unsigned a = 0; a < 2; ++a) {
                expected.auxiliary[16*a+3] |= 0x80;
                const uint32_t function = a ? 0x998c : 0x9970;
                std::memcpy(expected.auxiliary.data()+16*a+12,&function,4);
            }
        }
        pending = true;
    }
    void finish(rt::M2Board &b, uint32_t at) {
        if (!pending || at != entity) throw std::runtime_error("player chain return without entry");
        const auto compare = [&](const auto &bytes, uint32_t address) {
            for (unsigned i = 0; i < bytes.size(); ++i) if (b.read_byte(address+i) != bytes[i]) {
                std::fprintf(stderr,"player chain mismatch frame=%llu address=%08x native=%02x reference=%02x\n",
                    (unsigned long long)b.frame(),address+i,bytes[i],b.read_byte(address+i));
                throw std::runtime_error("native player chain differs from reference");
            }
        };
        compare(expected.entity,entity); compare(expected.yaw,0x5018a4);
        compare(expected.ahead,0x501864); compare(expected.heights,0x501100);
        compare(expected.globals,0x501000); compare(expected.signals,0x501a02);
        compare(expected.crash_auxiliary,crash_auxiliary_address);
        compare(expected.camera,0x501710);
        if (road.storage) {
            if (!rolling_mode && award_cues_seen != award.cue_count) throw std::runtime_error("missing player checkpoint announcement calls");
            if (marks.timer != b.read_dword(0x53b718)) throw std::runtime_error("player road mark timer differs");
            compare(road_marks,0x53b71c);
            for (unsigned offset : {0xb0u,0xb4u,0xb8u,0x444u,0x448u,0x612u,0x68cu})
                for (unsigned i = 0; i < 4; ++i)
                    if (expected.checkpoint_extra[offset+i] != b.read_byte(0x501400+offset+i))
                        throw std::runtime_error("player checkpoint/tilt shared state mismatch");
            if (player.lap_write.written && b.read_dword(0x50159c+4*player.lap_write.index) != player.lap_write.ticks)
                throw std::runtime_error("player lap-time write mismatch");
        }
        if (expected.control != b.read_dword(0x501124) || uint32_t(expected.camera_mode) != b.read_dword(0x5014a4))
            throw std::runtime_error("player pre-dynamics shared state mismatch");
        if (expected.last_surface != b.read_dword(0x5018ac) || expected.center_mask != b.read_word(0x5018b0) ||
            expected.shake != b.read_byte(0x501830) || expected.sequence_counter != b.read_byte(0x503680) ||
            uint16_t(expected.shake_angles[0]) != b.read_word(0x501834) || uint16_t(expected.shake_angles[1]) != b.read_word(0x501300)) {
            std::fprintf(stderr,"boundary shared frame=%llu native/reference surface=%08x/%08x mask=%04x/%04x shake=%u/%u sequence=%u/%u angles=%04x/%04x,%04x/%04x\n",
                (unsigned long long)b.frame(),expected.last_surface,b.read_dword(0x5018ac),expected.center_mask,b.read_word(0x5018b0),
                expected.shake,b.read_byte(0x501830),expected.sequence_counter,b.read_byte(0x503680),
                (uint16_t)expected.shake_angles[0],b.read_word(0x501834),(uint16_t)expected.shake_angles[1],b.read_word(0x501300));
            throw std::runtime_error("player boundary shared state mismatch");
        }
        for (unsigned a = 0; a < 2; ++a) for (unsigned i = 0; i < 16; ++i)
            if (expected.auxiliary[16*a+i] != b.read_byte(auxiliary_address[a]+i))
                throw std::runtime_error("player chain auxiliary effect mismatch");
        if (expected.profile_updates != b.read_byte(0x5014e0)) throw std::runtime_error("player profile update mismatch");
        ++checked; pending = false;
        for (unsigned a = 0; a < 3; ++a) outside_pose[a] = b.read_dword(entity+0x1c+4*a);
    }
    void report() const {
        if (!checked || entered != checked || pending) throw std::runtime_error("missing player chain coverage");
        if (rolling_mode) {
            std::printf("native_rolling_effect_schedule PASS transitions=%llu calls=%llu max_calls=%llu (mapped entity only, observed scheduling, not autonomous loop)\n",
                (unsigned long long)effect_transitions,(unsigned long long)effect_calls,(unsigned long long)effect_max_calls);
            std::printf("native_rolling_prefix_live PASS checked=%llu entry_spin=%llu (persistent player; external camera/UI angle/effect flags and observed effect scheduling; UI/release excluded)\n",
                (unsigned long long)checked,(unsigned long long)spins);
            for(unsigned word=0;word<between_words.size();++word) if(between_words[word])
                std::printf("native_rolling_between offset=%03x ticks=%llu (outside checked prefix)\n",
                    word*4,(unsigned long long)between_words[word]);
            for(unsigned byte=0;byte<between_bytes.size();++byte) if(between_bytes[byte])
                std::printf("native_rolling_between_byte offset=%03x ticks=%llu (outside checked prefix)\n",
                    byte,(unsigned long long)between_bytes[byte]);
            return;
        }
        std::printf("native_player_chain PASS active=%llu/%llu entry_spin=%llu entry_guided=%llu auxiliary=%llu state=%zu dynamics_input=%zu bytes (entry counts precede lifecycle when enabled; one snapshot per tick, not full race loop)\n",
            (unsigned long long)checked,(unsigned long long)entered,(unsigned long long)spins,
            (unsigned long long)guided,(unsigned long long)events,sizeof(player),sizeof(nm_player_dynamics_input_t));
        if(road.storage) {
            if(field_player_checked!=checked) throw std::runtime_error("missing native field player coverage");
            std::printf("native_field_player PASS turns=%llu neighbor_reads=%llu (live native links/shared owners, poisoned duplicate views, source-seeded turns)\n",
                (unsigned long long)field_player_checked,(unsigned long long)field_player_neighbors);
        }
        if (road.storage) std::printf("native_player_surface_chain PASS through16da4 none/recover/pivot/spin=%llu/%llu/%llu/%llu crash_events=%llu surface_updates=%llu (no refresh between dynamics/road/boundary)\n",
            (unsigned long long)responses[0],(unsigned long long)responses[1],(unsigned long long)responses[2],
            (unsigned long long)responses[3],(unsigned long long)crash_events,(unsigned long long)surface_updates);
        if (road.storage) std::printf("native_player_active_chain PASS frome5f4 held=%llu auto/manual=%llu/%llu active_animations=%llu pre_sounds=%llu input=%zu (no per-operation refresh; remaining caller updates still outside)\n",
            (unsigned long long)held_steps,(unsigned long long)(checked-manual_steps),(unsigned long long)manual_steps,
            (unsigned long long)active_animations,(unsigned long long)pre_sounds,sizeof(nm_player_active_input_t));
        if (road.storage) std::printf("native_player_early_chain PASS corrections=%llu shake=%llu inhibit=%llu sound_intents=%llu (frome5f4 through158c8)\n",
            (unsigned long long)corrections,(unsigned long long)shakes,(unsigned long long)inhibits,(unsigned long long)early_sounds);
        if (road.storage) std::printf("native_player_offcourse_chain PASS starts=%llu nonzero_timer=%llu (normal block now through10a34; zero starts means NO recovery coverage)\n",
            (unsigned long long)offcourse_starts,(unsigned long long)offcourse_advanced);
        if (road.storage) std::printf("native_player_checkpoint_chain PASS calls=%llu visits=%llu lap_writes=%llu (normal block through132b8; retained native road/profile/flags)\n",
            (unsigned long long)checkpoint_steps,(unsigned long long)checkpoint_visits,(unsigned long long)lap_writes);
        if (road.storage) std::printf("native_player_body_angles_chain PASS calls=%llu held=%llu (normal and held block through11aa4 return; native force/ground inputs retained, no model binding yet)\n",
            (unsigned long long)checked,(unsigned long long)held_steps);
        if (road.storage) std::printf("native_player_tilt_chain PASS calls=%llu (normal block through1550c return; retained speed/surface/boost, shared halfwords checked)\n",
            (unsigned long long)(checked-held_steps));
        if (road.storage) std::printf("native_player_road_tail_chain PASS calls=%llu (through13e3c; lateral/path/rank and all %u shared course bytes/timer checked)\n",
            (unsigned long long)(checked-held_steps),marks.count);
        if (road.storage) std::printf("native_player_end_chain PASS calls=%llu route_transitions=%llu held=%llu (whole entity through e0a4; circle verified by scratch oracle, no live TGP sync)\n",
            (unsigned long long)checked,(unsigned long long)route_transitions,(unsigned long long)held_steps);
        if (road.storage) std::printf("native_player_award_chain PASS fromd834 calls=%llu awards=%llu cue_intents=%llu (native time feeds controls; checkpoint bit consumed before physics, not autonomous ticks)\n",
            (unsigned long long)checked,(unsigned long long)awards,(unsigned long long)award_sounds);
        std::printf("native_player_between entries=%llu yaw=%llu ahead=%llu heights=%llu profiles=%llu (remaining reference writes, NOT autonomous ticks)\n",
            (unsigned long long)transitions,(unsigned long long)between_yaw,(unsigned long long)between_ahead,
            (unsigned long long)between_heights,(unsigned long long)between_profiles);
        std::printf("native_player_external_pose intervals=%llu (first16 logged; between checked blocks only)\n",
            (unsigned long long)outside_pose_changes);
        for (unsigned i = 0; i < between_words.size(); ++i) if (between_words[i])
            std::printf("native_player_between offset=%03x frames=%llu\n",4*i,(unsigned long long)between_words[i]);
        for (unsigned i = 0; i < pose_intervals.size(); ++i) {
            const auto &counts = pose_intervals[i];
            if (counts[0] || counts[1] || counts[2] || counts[3])
                std::printf("native_player_pose interval_after_call=%05x x/y/z/yaw=%llu/%llu/%llu/%llu\n",
                    i ? 0xd830+4*i-4 : 0,(unsigned long long)counts[0],(unsigned long long)counts[1],
                    (unsigned long long)counts[2],(unsigned long long)counts[3]);
        }
    }
};
}
