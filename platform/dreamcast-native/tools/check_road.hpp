// Host-only oracle: run actual i960 routines on scratch controls/entity/profile data
// and the already booted reference TGP. Never linked into the Dreamcast target.
#pragma once
#include "../include/road.h"
#include "../include/vehicle.h"
#include "../include/wheels.h"
#include "../include/damage.h"
#include "../include/boundary.h"
#include "../include/path.h"
#include "../include/intro_math.h"
#include "../include/race_field.h"
#include "../tests/road_check.h"
#include "../tests/drive_check.h"
#include "check_player.hpp"
#include "refcore/i960_ref.h"
#include "runtime/m2_board.h"
#include <array>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <memory>
#include <cstdlib>
#include <vector>
#include <map>

namespace daytona::native {
static_assert(alignof(nm_race_car_t)==32 && sizeof(nm_race_car_t)%32==0);
static_assert(sizeof(nm_road_profile_t) == 164 && sizeof(nm_road_lane_t) == 20);
static_assert(sizeof(nm_road_lateral_t) == 8);
static_assert(sizeof(nm_axle_state_t) == 8);
static_assert(sizeof(nm_forces_state_t) == 48);
static_assert(sizeof(nm_forces_input_t) == 52);
static_assert(sizeof(nm_traction_state_t) == 24 && sizeof(nm_speed_state_t) == 16);
static_assert(sizeof(nm_tires_state_t) == 24 && sizeof(nm_tires_input_t) == 28);
static_assert(sizeof(nm_gears_state_t) == 24 && sizeof(nm_gears_input_t) == 12);
static_assert(sizeof(nm_resistance_state_t) == 8 && sizeof(nm_proximity_input_t) == 36);
static_assert(sizeof(nm_motion_state_t) == 36 && sizeof(nm_motion_input_t) == 36);

class VehicleOracleBus final : public rt::Bus {
public:
    rt::M2Board &board;
    nm_road_profile_t profile{};
    nm_road_profile_t collision_profile{}; // Second authored profile in pair-response checks.
    nm_path_pack_t path{}; // Borrowed immutable rows for the path oracle only.
    uint32_t road_mapping = 0, road_flags = 0; // Original ROM tables, NOT pre-folded DNR1 data.
    uint8_t guided_mode = 0, guided_profile_updates = 0;
    std::array<uint8_t, 0x300> entity{};
    std::array<uint8_t, 0x100> globals{};
    std::array<uint8_t, 32> auxiliary{};
    std::array<uint8_t, 8> yaw_globals{}; // 5018a4 offset, 5018a8 switch.
    std::array<uint8_t, 8> lookahead{}; // 501864/501868, normal motion only.
    std::array<uint8_t, 16> ground_heights{};
    std::array<uint8_t, 2> render_shadow_count{}; // 501120, player render oracle.
    uint32_t contact_grid = 0, contact_surface = 0;
    std::array<std::array<uint8_t,0x300>,3> neighbors{};
    uint32_t proximity_disabled = 0;
    uint8_t curve_selector = 0;
    uint8_t course_slot = 0;
    uint32_t gear_input_word = 0;
    uint32_t recovery_input_word = 0;
    bool yaw_conversion_valid = true;
    bool guided_conversion_valid = true;
    uint32_t guided_divisor = 0;
    uint32_t yaw_rotation = 0;
    std::array<uint8_t,2> boundary_signal{};
    uint8_t boundary_shake = 0;
    unsigned boundary_sound_requests = 0;
    uint16_t boundary_center_mask = 0;
    bool boundary_complete = false, boundary_division_valid = true;
    unsigned boundary_action = NM_BOUNDARY_NONE;
    uint32_t boundary_last_surface = 0;
    uint16_t boundary_spin_signal = 0;
    uint8_t boundary_sequence_counter = 0;
    unsigned boundary_sound_mask = 0;
    std::array<uint8_t,48> damage_camera{}; //501710..50173f, preserve holes.
    uint32_t damage_control = 0;
    int32_t damage_camera_mode = 0;
    uint8_t damage_variant = 0, damage_restore_mode = 0;
    std::array<int16_t,2> shake_angles{};
    unsigned early_sound_requests = 0;
    bool early_conversion_valid = true;
    bool lateral_conversion_valid = true;
    bool collision_conversion_valid = true;
    bool body_conversion_valid = true;
    uint32_t collision_primary = 0x621000;
    std::vector<std::array<uint8_t,0x300>> collision_pool;
    uint32_t collision_wrap = 0;
    uint8_t collision_phase = 0;
    std::array<uint32_t,44> traffic_ranks{}, traffic_track{}, traffic_scratch{};
    int8_t traffic_lap = 0;
    uint32_t traffic_player = 0x640000, traffic_subject = 0x640000;
    uint32_t race_short_word = 0;
    std::array<uint32_t,2> race_events{}, traffic_previous{};
    std::array<uint8_t,0x700> checkpoint_memory{}; //501400..501aff, including lap-table aliases.
    std::array<uint32_t,32> checkpoint_data{}; // Count followed by original4-byte rows.
    std::array<uint8_t,4096> road_marks{};
    uint32_t road_mark_timer = 0;
    uint32_t inhibit_owner=0;
    uint32_t award_table = 0;
    uint16_t award_difficulty = 0;
    int16_t scenery_heading = 0;
    std::vector<uint32_t> award_cues;
    const nm_road_pack_t *tail_road = nullptr;
    uint32_t first_instruction = 0x13b28, last_instruction = 0x13ddc;
    explicit VehicleOracleBus(rt::M2Board &b) : board(b) {}
    uint16_t flags(uint32_t at) override {
        // Synthetic profiles/auxiliary/pair/path storage is ordinary RAM.
        // Inherit real bus flags elsewhere, especially non-burst TGP FIFO.
        // LDT/STT must advance through RAM instead of rereading one word.
        return at >= 0x600000 && at < 0x650000 ? rt::Cpu::BURST : board.flags(at);
    }
    uint32_t fetch(uint32_t at) override {
        if ((at < first_instruction || at > last_instruction) &&
            !(first_instruction==0xcb60 && at>=0x115e4 && at<=0x116cc) &&
            !(first_instruction==0x157e8 && at>=0x15678 && at<=0x157e4) &&
            !(first_instruction==0x15a04 && at>=0x15b90 && at<=0x15c08) &&
            !(first_instruction==0xce74 && at>=0x13a14 && at<=0x13b24) &&
            !(first_instruction == 0x1727c && at >= 0x17668 && at <= 0x17754) &&
            !((first_instruction==0x1904c || first_instruction==0x191d8) && at>=0x19bdc && at<=0x19c54) &&
            !(first_instruction == 0x134f8 && ((at >= 0x135e0 && at <= 0x13654) || (at >= 0x19bdc && at <= 0x19c54))) &&
            !((first_instruction == 0xd920 || first_instruction == 0xd9f8) && at >= 0xe078 && at <= 0xe0a4) &&
            !(first_instruction == 0x12464 && at >= 0x139d8 && at <= 0x13a10) &&
            !((first_instruction == 0x14904 || first_instruction == 0x14630) && (at == 0x148dc || at == 0x148f0)) &&
            !(first_instruction == 0x14630 && at >= 0x14904 && at <= 0x14a20) &&
            !(first_instruction == 0x15238 && at >= 0x13a14 && at <= 0x13b24) &&
            !((first_instruction == 0x14fcc || first_instruction == 0x158c8 || first_instruction == 0x1638c || first_instruction == 0x16bd4 || first_instruction == 0x15eb8 || first_instruction == 0x15d60 ||
                (first_instruction == 0x16060 && boundary_complete)) &&
                at >= 0x19bdc && at <= 0x19c54) &&
            !(first_instruction == 0x15d60 && at >= 0x637c && at <= 0x6454) &&
            !(first_instruction == 0x15de0 && at >= 0x15c0c && at <= 0x15c84) &&
            !(first_instruction == 0x10980 && at >= 0x10bcc && at <= 0x10cc8) &&
            !((first_instruction == 0x14fcc || first_instruction == 0x16bd4 || (first_instruction == 0x16060 && boundary_complete)) &&
                ((at >= 0x15cc8 && at <= 0x15d5c) || (at >= 0x15e54 && at <= 0x15fc0))) &&
            !(first_instruction == 0xecc8 && at >= 0x10410 && at <= 0x1047c) &&
            !(first_instruction == 0xef80 && at >= 0xf278 && at <= 0xf318) &&
            !(first_instruction == 0xfdf8 && at >= 0x10480 && at <= 0x104e8) &&
            !(first_instruction == 0xd394 && at >= 0x116d0 && at <= 0x11d5c) &&
            !((first_instruction == 0xd2c0 || first_instruction == 0xd684) &&
              ((at >= 0x13b28 && at <= 0x13ddc) || (at >= 0xe760 && at <= 0xe788))) &&
            !((first_instruction == 0x116d0 || first_instruction == 0xd394 || first_instruction == 0x16060 || first_instruction == 0xe78c) && at >= 0x115e4 && at <= 0x116cc) &&
            !(first_instruction == 0x11150 && ((at >= 0x10bcc && at <= 0x10c4c) ||
                (at >= 0x13b28 && at <= 0x13ddc))) &&
            !(first_instruction == 0x10ccc && ((at >= 0x10bcc && at <= 0x10c4c) ||
                (at >= 0x136e8 && at <= 0x13740) || (at >= 0x13b28 && at <= 0x13ddc))) &&
            !(first_instruction == 0xf31c && ((at >= 0xf830 && at <= 0xf9bc) ||
                                            (at >= 0x10480 && at <= 0x10564))))
            throw std::runtime_error("vehicle oracle left routine");
        return board.fetch(at);
    }
    uint8_t read_byte(uint32_t at) override {
        if(first_instruction==0x19320) {
            if(at>=0x501400 && at<0x501b00) return checkpoint_memory[at-0x501400];
            if((at>=0x236260 && at<0x2367e0) || (at>=0x1956c && at<0x19590)) return board.read_byte(at);
        }
        if(first_instruction==0x1904c || first_instruction==0x191d8) {
            if(at>=0x501400 && at<0x501b00) return checkpoint_memory[at-0x501400];
            if((at>=0x235ec0 && at<0x236260) || (at>=0x191c8 && at<0x191d8)) return board.read_byte(at);
            if(at==0x53e59a) return damage_variant;
            if(at==0x501092) return globals[0x92]&~0x50u; // Execute/count intent, suppress only the sound queue.
        }
        if(first_instruction==0xcb60 && ((at>=0xcd24 && at<0xcde0) || (at>=0x2000000 && at<0x4000000)))
            return board.read_byte(at);
        if(first_instruction==0x15678 || first_instruction==0x157e8) {
            if(at>=0x235090 && at<0x235684) return board.read_byte(at);
            if(at>=0x28b573c && at<0x28b5740) return board.read_byte(at);
            if(at>=0x501220 && at<0x501224) return uint8_t(0x620000u>>(8*(at-0x501220)));
            if(at>=0x501894 && at<0x501898) return uint8_t(race_short_word>>(8*(at-0x501894)));
        }
        if(first_instruction==0x15a04) {
            if(at>=0x23523c && at<0x23563c) return board.read_byte(at);
            if(at>=0x28b573c && at<0x28b5740) return board.read_byte(at);
            if(at>=0x501a9c && at<0x501aa0) return at==0x501a9c; // Active sound-only cooldown, no audio effect.
        }
        if (first_instruction == 0xe23c) {
            if (at >= 0x501434 && at < 0x501438)
                return uint8_t(collision_wrap>>(8*(at-0x501434)));
            if (at == 0x501460) return course_slot;
            if (at >= 0x234be8 && at < 0x234bf0) return board.read_byte(at);
        }
        if (first_instruction == 0x3560 && at >= 0x501a84 && at < 0x501a88)
            return checkpoint_memory[at-0x501400];
        if (first_instruction == 0xd5a8 && at >= 0x501400 && at < 0x501b00)
            return checkpoint_memory[at-0x501400];
        if (first_instruction == 0x171f8) {
            if (at == 0x501460) return course_slot;
            if (at >= 0x5012f0 && at < 0x5012f4) return uint8_t(0x500000u>>(8*(at-0x5012f0)));
            if (at >= 0x17150 && at < 0x171f8) return board.read_byte(at);
        }
        if (first_instruction == 0x16ed0) {
            if (at == 0x501460) return course_slot;
            if (at >= 0x17104 && at < 0x17150) return board.read_byte(at);
            if (at >= 0x501500 && at < 0x501700) return checkpoint_memory[at-0x501400];
        }
        if (first_instruction == 0x1727c || first_instruction == 0x17668) {
            if (at >= 0x5014a8 && at < 0x5014aa) return uint8_t(uint16_t(scenery_heading)>>(8*(at-0x5014a8)));
            if ((at >= 0x17658 && at < 0x17668) || (at >= 0x28b5880 && at < 0x28b5b00)) return board.read_byte(at);
        }
        if (first_instruction == 0x134f8) {
            if (at >= 0x501440 && at < 0x501444) return uint8_t(award_table>>(8*(at-0x501440)));
            if (at >= 0x501220 && at < 0x501224) return uint8_t(0x500000u>>(8*(at-0x501220)));
            if (at >= 0x53e59c && at < 0x53e59e) return uint8_t(award_difficulty>>(8*(at-0x53e59c)));
            if (at >= 0x501400 && at < 0x501b00) return checkpoint_memory[at-0x501400];
            if ((at >= 0x234e28 && at < 0x235000) || (at >= 0x2000000 && at < 0x4000000)) return board.read_byte(at);
        }
        if ((first_instruction == 0x13a14 || first_instruction == 0x11150 || first_instruction==0xce74) && tail_road && at >= 0x600000 &&
                at-0x600000 < 164*tail_road->profile_count)
            return reinterpret_cast<const uint8_t *>(tail_road->profiles)[at-0x600000];
        if (first_instruction == 0x13de0 || first_instruction == 0x13e3c || first_instruction == 0x11304) {
            if (at >= 0x501434 && at < 0x501438) return uint8_t(collision_wrap>>(8*(at-0x501434)));
            if (at >= 0x53b718 && at < 0x53b71c) return uint8_t(road_mark_timer>>(8*(at-0x53b718)));
            if (at >= 0x53b71c && at < 0x53c71c) return road_marks[at-0x53b71c];
        }
        // Silent crash oracle: a live sound cooldown bypasses sound-only15c0c.
        if(first_instruction==0x15de0 && at>=0x501aa4 && at<0x501aa8) return at==0x501aa4;
        if ((first_instruction == 0x11aa4 || first_instruction == 0x1550c) && at >= 0x501844 && at < 0x50184c)
            return checkpoint_memory[at-0x501400];
        if (first_instruction == 0x1550c && ((at >= 0x235144 && at < 0x235284) ||
                (at >= 0x235b54 && at < 0x235b5c))) return board.read_byte(at);
        if (first_instruction == 0x132b8) {
            if (at >= 0x501400 && at < 0x501b00) return checkpoint_memory[at-0x501400];
            if (at >= 0x650000 && at < 0x650080)
                return uint8_t(checkpoint_data[(at-0x650000)/4]>>(8*(at%4)));
        }
        if (first_instruction == 0x12150) {
            if (at >= 0x501894 && at < 0x501898) return uint8_t(race_short_word>>(8*(at-0x501894)));
            if ((at >= 0x501a9c && at < 0x501aa0) || (at >= 0x501aa4 && at < 0x501aa8))
                return uint8_t(race_events[at >= 0x501aa4]>>(8*(at%4)));
        }
        if (first_instruction == 0x125b0) {
            if (at >= 0x50183c && at < 0x501844) return uint8_t(traffic_previous[(at-0x50183c)/4]>>(8*(at%4)));
            if (at >= 0x501a04 && at < 0x501a06) return uint8_t(boundary_spin_signal>>(8*(at-0x501a04)));
            if (at >= 0x501220 && at < 0x501224) return uint8_t(traffic_player>>(8*(at-0x501220)));
            if (at >= 0x640000 && at-0x640000 < collision_pool.size()*0x300)
                return collision_pool[(at-0x640000)/0x300][(at-0x640000)%0x300];
        }
        if (first_instruction == 0x12464 && ((at >= 0x18 && at < 0x1c) || (at >= 0x88 && at < 0x8c)))
            return board.read_byte(at); // Original worker's null-link progress inputs, not a synthetic car.
        if (first_instruction == 0x121d4 && at >= 0x501220 && at < 0x501224)
            return uint8_t(0x500000u>>(8*(at-0x501220)));
        if (first_instruction == 0x121d4 || first_instruction == 0x1225c || first_instruction == 0x122dc) {
            if (at == 0x5014ba) return uint8_t(traffic_lap);
            if (at >= 0x503080 && at < 0x503290) {
                const unsigned offset = at-0x503080;
                const auto &array = offset < 176 ? traffic_ranks : offset < 352 ? traffic_track : traffic_scratch;
                return uint8_t(array[(offset%176)/4]>>(8*(offset%4)));
            }
        }
        if (first_instruction == 0x13f08 || first_instruction == 0x1406c || first_instruction == 0x1225c || first_instruction == 0x122dc || first_instruction == 0x12464) {
            if (first_instruction == 0x12464 && at >= 0x501220 && at < 0x501224) return uint8_t(traffic_player>>(8*(at-0x501220)));
            if (at >= 0x13ec4 && at < 0x13efc) return board.read_byte(at);
            if (at == 0x503692) return collision_phase;
            if (at >= 0x640000 && at-0x640000 < collision_pool.size()*0x300)
                return collision_pool[(at-0x640000)/0x300][(at-0x640000)%0x300];
            if (at >= 0x501224 && at < 0x501228) return uint8_t(0x640000u>>(8*(at-0x501224)));
            if (at >= 0x5012f0 && at < 0x5012f4) return uint8_t(collision_primary>>(8*(at-0x5012f0)));
            if (at >= 0x501434 && at < 0x501438) return uint8_t(collision_wrap>>(8*(at-0x501434)));
        }
        if (first_instruction == 0x14180) {
            if (at >= 0x235af4 && at < 0x235b54) return board.read_byte(at);
            if (at >= 0x5012f0 && at < 0x5012f4) return uint8_t(collision_primary>>(8*(at-0x5012f0)));
        }
        if ((first_instruction == 0x14904 || first_instruction == 0x14630) && at >= 0x235b08 && at < 0x235b18) return board.read_byte(at);
        if (first_instruction == 0x14c54) {
            if (at < 4 || (at >= 0x28b573c && at < 0x28b5740)) return board.read_byte(at);
            if (at == 0x501830) return boundary_shake;
        }
        if (first_instruction == 0x14fcc && at >= 0x14fb4 && at < 0x14fcc) return board.read_byte(at);
        if (first_instruction == 0x15238 && (at < 4 || (at >= 0x235b18 && at < 0x235b40)))
            return board.read_byte(at);
        if (at >= 0x53e584 && at < 0x53e588) return uint8_t(recovery_input_word>>(8*(at-0x53e584)));
        if (first_instruction == 0xe5f4 || first_instruction == 0x154d0 || first_instruction == 0x158c8) {
            if (at >= 0x200000 && at < 0x240000) return board.read_byte(at);
            if (at >= 0x28b573c && at < 0x28b5744) return board.read_byte(at);
            if (at == 0x501830) return boundary_shake;
            if (at == 0x501092) return globals[0x92]&~0x50u; // Silent source sound gate.
        }
        if (at >= 0x501834 && at < 0x501836) return uint8_t(uint16_t(shake_angles[0])>>(8*(at-0x501834)));
        if (at >= 0x501300 && at < 0x501302) return uint8_t(uint16_t(shake_angles[1])>>(8*(at-0x501300)));
        if (first_instruction == 0x10980 || first_instruction == 0x1365c) {
            if (at >= 0x501430 && at < 0x501434) return uint8_t(road_mapping>>(8*(at-0x501430)));
            if (at >= 0x501450 && at < 0x501454) return uint8_t(road_flags>>(8*(at-0x501450)));
            if (at >= 0x02000000 && at < 0x04000000) return board.read_byte(at);
        }
        if (first_instruction == 0x15de0 || first_instruction == 0x14fcc || first_instruction == 0x16bd4 || first_instruction == 0x15eb8 || first_instruction == 0x15d60 || first_instruction == 0x15fc4 ||
            (first_instruction == 0x16060 && boundary_complete)) {
            if (at == 0x501830) return boundary_shake;
            if (at == 0x503680) return boundary_sequence_counter;
            if (at >= 0x5018ac && at < 0x5018b0) return uint8_t(boundary_last_surface>>(8*(at-0x5018ac)));
            if (at >= 0x501a04 && at < 0x501a06) return uint8_t(boundary_spin_signal>>(8*(at-0x501a04)));
            if (at >= 0x501270 && at < 0x501274) return uint8_t(0x610000u>>(8*(at-0x501270)));
            // Mute only sound-enable bits20/22. Neither participates in the
            // damage auxiliary gate1f800000; still execute/count real19bdc.
            if (at == 0x501092) return globals[0x92]&~0x50u;
            if ((at >= 0x200000 && at < 0x240000) || (at >= 0x2000000 && at < 0x4000000) ||
                (at >= 0x16d7c && at < 0x16d8c)) return board.read_byte(at);
        }
        if (at >= 0x501710 && at < 0x501740) return damage_camera[at-0x501710];
        if (at >= 0x501124 && at < 0x501128) return uint8_t(damage_control>>(8*(at-0x501124)));
        if (at >= 0x5014a4 && at < 0x5014a8) return uint8_t(uint32_t(damage_camera_mode)>>(8*(at-0x5014a4)));
        if (at == 0x503690) return damage_variant;
        const int camera_index = std::bit_cast<int8_t>(damage_camera[0]);
        if (at == uint32_t(0x1d00233+20*camera_index)) return damage_restore_mode;
        if (at >= 0x5018b0 && at < 0x5018b2) return uint8_t(boundary_center_mask>>(8*(at-0x5018b0)));
        if (first_instruction == 0x1638c || (first_instruction == 0x16060 && boundary_complete)) {
            if (at == 0x501830) return boundary_shake;
            if (at >= 0x501a02 && at < 0x501a04) return boundary_signal[at-0x501a02];
            if (at >= 0x200000 && at < 0x240000) return board.read_byte(at);
        }
        if (at >= 0x501220 && at < 0x501224) return uint8_t(0x500000u>>(8*(at-0x501220)));
        if(first_instruction==0x11438 && ((at>=0x28b85a0 && at<0x28b8da0) ||
            (at>=0x28b5740 && at<0x28b5744))) return board.read_byte(at);
        if(first_instruction==0x10dd8 && at>=0x234b2b && at<0x234d88) return board.read_byte(at);
        if(first_instruction==0x136e8 && at>=0x137d0 && at<0x137d4) return board.read_byte(at);
        if (at == 0x5014e0) return guided_profile_updates;
        if(at>=0x5014e4 && at<0x5014e8) return uint8_t(inhibit_owner>>(8*(at-0x5014e4)));
        if (at == 0x5014f7) return guided_mode;
        if ((at >= 0x234b54 && at < 0x234d88) || (at >= 0x28b5b00 && at < 0x28b5d14)) return board.read_byte(at);
        if (path.rows && at >= 0x630000 && at-0x630000 < path.records*sizeof(nm_path_row_t))
            return reinterpret_cast<const uint8_t *>(path.rows)[at-0x630000];
        if (at >= 0x500000 && at < 0x500300) return entity[at-0x500000];
        if (at >= 0x501000 && at < 0x501100) return globals[at-0x501000];
        if (at >= 0x501100 && at < 0x501110) return ground_heights[at-0x501100];
        if (first_instruction==0x6c40 && at>=0x501120 && at<0x501122) return render_shadow_count[at-0x501120];
        if (at >= 0x501428 && at < 0x50142c) return uint8_t(contact_grid>>(8*(at-0x501428)));
        if (at >= 0x50142c && at < 0x501430) return uint8_t(contact_surface>>(8*(at-0x50142c)));
        if ((first_instruction == 0x116d0 || first_instruction == 0xd394 || first_instruction == 0x16060 || first_instruction == 0xe78c) &&
            at >= 0x02000000 && at < 0x04000000) return board.read_byte(at);
        if (at >= 0x234ac4 && at < 0x234af4) return board.read_byte(at);
        if (at >= 0x610000 && at < 0x610020) return auxiliary[at-0x610000];
        if (at >= 0x5018a4 && at < 0x5018ac) return yaw_globals[at-0x5018a4];
        if (at >= 0x501864 && at < 0x50186c) return lookahead[at-0x501864];
        if (at >= 0x620000 && at < 0x623000 && (at&0xfff) < 0x300)
            return neighbors[(at-0x620000)/0x1000][at&0xfff];
        if (at >= 0x501a80 && at < 0x501a84) return uint8_t(proximity_disabled >> (8*(at-0x501a80)));
        if (at >= 0x501274 && at < 0x50127c) {
            const uint32_t pointer = at < 0x501278 ? 0x610000 : 0x610010;
            return uint8_t(pointer >> (8*(at%4)));
        }
        if (at >= 0x600000 && at < 0x600000 + sizeof(profile))
            return reinterpret_cast<const uint8_t *>(&profile)[at-0x600000];
        if (first_instruction == 0x15238 && at >= 0x600000+sizeof(profile) && at < 0x600000+2*sizeof(profile))
            return reinterpret_cast<const uint8_t *>(&collision_profile)[at-0x600000-sizeof(profile)];
        if (at >= 0x501448 && at < 0x50144c) return uint8_t(0x600000u >> (8*(at-0x501448)));
        if (at == 0x501464) return curve_selector;
        if (at == 0x501460) return course_slot;
        if (at >= 0x53e120 && at < 0x53e124) return uint8_t(gear_input_word >> (8*(at-0x53e120)));
        if ((at >= 0x234abc && at < 0x234ac4) || (at >= 0x234bc8 && at < 0x234be8) ||
            (at >= 0xebc4 && at < 0xec44) || (at >= 0xf9c0 && at < 0xf9d0) ||
            (at >= 0x028b5758 && at < 0x028b5860) ||
            (at >= 0x028b5d20 && at < 0x028b854c)) return board.read_byte(at);
        std::fprintf(stderr,"vehicle oracle unexpected read address=%08x routine=%08x\n",at,first_instruction);
        throw std::runtime_error("unexpected road oracle read");
    }
    uint16_t read_word(uint32_t at) override { return read_byte(at) | uint16_t(read_byte(at+1)) << 8; }
    uint32_t read_dword(uint32_t at) override {
        if (at >= 0x91fff0 && at <= 0x91fff8) return board.read_dword(at);
        if (at == 0x884000) return board.read_dword(at);
        return read_word(at) | uint32_t(read_word(at+2)) << 16;
    }
    void write_byte(uint32_t at, uint8_t v) override {
        if(first_instruction==0x6c40 && at>=0x501120 && at<0x501122) {
            render_shadow_count[at-0x501120]=v;return;
        }
        if(first_instruction==0x19320 && at>=0x501400 && at<0x501b00) {
            checkpoint_memory[at-0x501400]=v;return;
        }
        if((first_instruction==0x1904c || first_instruction==0x191d8) && at>=0x501400 && at<0x501b00) {
            checkpoint_memory[at-0x501400]=v;return;
        }
        if(first_instruction==0x157e8) {
            if(at==0x501898) { checkpoint_memory[0x498]=v;return; }
            if(at>=0x501894 && at<0x501896) {
                const unsigned shift=8*(at-0x501894);
                race_short_word=(race_short_word&~(255u<<shift))|(uint32_t(v)<<shift);return;
            }
        }
        if(at>=0x5014e4 && at<0x5014e8) {
            const unsigned shift=8*(at-0x5014e4);
            inhibit_owner=(inhibit_owner&~(255u<<shift))|(uint32_t(v)<<shift);return;
        }
        if (first_instruction==0x42e8 && at>=0x501a80 && at<0x501a84) {
            const unsigned shift=8*(at-0x501a80);
            proximity_disabled=(proximity_disabled&~(255u<<shift)) | (uint32_t(v)<<shift);
            return;
        }
        if (first_instruction == 0xe3f4 && at >= 0x501844 && at < 0x501846) {
            checkpoint_memory[at-0x501400]=v; return;
        }
        if (first_instruction == 0x16ed0 && at >= 0x501500 && at < 0x501700) {
            checkpoint_memory[at-0x501400]=v; return;
        }
        if (first_instruction == 0x134f8 && at >= 0x501400 && at < 0x501b00) {
            checkpoint_memory[at-0x501400] = v; return;
        }
        if (first_instruction == 0x13e3c) {
            if (at >= 0x53b718 && at < 0x53b71c) {
                const unsigned shift = 8*(at-0x53b718);
                road_mark_timer = (road_mark_timer&~(255u<<shift)) | (uint32_t(v)<<shift); return;
            }
            if (at >= 0x53b71c && at < 0x53c71c) { road_marks[at-0x53b71c] = v; return; }
        }
        if (first_instruction == 0x1550c && at >= 0x501844 && at < 0x50184c) {
            checkpoint_memory[at-0x501400] = v; return;
        }
        if ((first_instruction == 0x132b8 || first_instruction == 0xd5a8) && at >= 0x501400 && at < 0x501b00) {
            checkpoint_memory[at-0x501400] = v; return;
        }
        if (first_instruction == 0x12150 && ((at >= 0x501894 && at < 0x501898) ||
                (at >= 0x501a9c && at < 0x501aa0) || (at >= 0x501aa4 && at < 0x501aa8))) {
            auto &word = at < 0x501898 ? race_short_word : race_events[at >= 0x501aa4];
            const unsigned shift = 8*(at%4); word = (word&~(255u<<shift)) | (uint32_t(v)<<shift); return;
        }
        if (first_instruction == 0x125b0 && at >= 0x50183c && at < 0x501844) {
            auto &word = traffic_previous[(at-0x50183c)/4]; const unsigned shift = 8*(at%4);
            word = (word&~(255u<<shift)) | (uint32_t(v)<<shift); return;
        }
        if (first_instruction == 0x121d4 || first_instruction == 0x1225c || first_instruction == 0x122dc) {
            if (at == 0x5014ba) { traffic_lap = std::bit_cast<int8_t>(v); return; }
            if (at >= 0x503080 && at < 0x503290) {
                const unsigned offset = at-0x503080, shift = 8*(offset%4);
                auto &array = offset < 176 ? traffic_ranks : offset < 352 ? traffic_track : traffic_scratch;
                auto &word = array[(offset%176)/4]; word = (word&~(255u<<shift)) | (uint32_t(v)<<shift); return;
            }
        }
        if ((first_instruction == 0x13f08 || first_instruction == 0x1406c || first_instruction == 0x1225c || first_instruction == 0x122dc || first_instruction == 0x12464) && at >= 0x640000 && at-0x640000 < collision_pool.size()*0x300) {
            collision_pool[(at-0x640000)/0x300][(at-0x640000)%0x300] = v; return;
        }
        if ((first_instruction == 0x14180 || first_instruction == 0x14c54 || first_instruction == 0x14fcc || first_instruction == 0x14a5c || first_instruction == 0x14b58 || first_instruction == 0x15238) &&
            at >= 0x620000 && at < 0x623000 && (at&0xfff) < 0x300) {
            neighbors[(at-0x620000)/0x1000][at&0xfff] = v; return;
        }
        if ((at >= 0x501834 && at < 0x501836) || (at >= 0x501300 && at < 0x501302)) {
            const unsigned index = at >= 0x501834 ? 0 : 1, shift = 8*(at&1);
            shake_angles[index] = nm_yaw_angle((uint16_t(shake_angles[index])&~(255u<<shift)) | (unsigned(v)<<shift));
        }
        else if ((first_instruction == 0x14c54 || first_instruction == 0x154d0) && at == 0x501830) boundary_shake = v;
        else if (at >= 0x501710 && at < 0x501740) damage_camera[at-0x501710] = v;
        else if (at >= 0x501124 && at < 0x501128) {
            const unsigned shift = 8*(at-0x501124);
            damage_control = (damage_control&~(255u<<shift)) | (uint32_t(v)<<shift);
        }
        else if (at >= 0x5014a4 && at < 0x5014a8) {
            const unsigned shift = 8*(at-0x5014a4);
            damage_camera_mode = std::bit_cast<int32_t>((uint32_t(damage_camera_mode)&~(255u<<shift)) | (uint32_t(v)<<shift));
        }
        else if ((first_instruction == 0x14fcc || first_instruction == 0x16bd4 || first_instruction == 0x15eb8 || (first_instruction == 0x16060 && boundary_complete)) && at == 0x501830) boundary_shake = v;
        else if ((first_instruction == 0x15de0 || first_instruction == 0x14fcc || first_instruction == 0x16bd4 || first_instruction == 0x15eb8 || (first_instruction == 0x16060 && boundary_complete)) && at == 0x503680) boundary_sequence_counter = v;
        else if (at >= 0x5018ac && at < 0x5018b0) {
            const unsigned shift = 8*(at-0x5018ac);
            boundary_last_surface = (boundary_last_surface&~(255u<<shift)) | (uint32_t(v)<<shift);
        }
        else if (at >= 0x501a04 && at < 0x501a06) {
            const unsigned shift = 8*(at-0x501a04);
            boundary_spin_signal = uint16_t((boundary_spin_signal&~(255u<<shift)) | (uint32_t(v)<<shift));
        }
        else if (at >= 0x5018b0 && at < 0x5018b2) {
            const unsigned shift = 8*(at-0x5018b0);
            boundary_center_mask = uint16_t((boundary_center_mask&~(255u<<shift)) | (uint32_t(v)<<shift));
        }
        else if (first_instruction == 0x1638c && at == 0x501830) boundary_shake = v;
        else if ((first_instruction == 0x14180 || first_instruction == 0x1638c || first_instruction == 0x15d60 || (first_instruction == 0x16060 && boundary_complete)) && at >= 0x501a02 && at < 0x501a04) boundary_signal[at-0x501a02] = v;
        else if (first_instruction == 0xda18 && at >= 0x501a84 && at < 0x501a88)
            checkpoint_memory[at-0x501400] = v;
        else if (at == 0x5014e0) guided_profile_updates = v;
        else if (at >= 0x500000 && at < 0x500300) entity[at-0x500000] = v;
        else if (at >= 0x501000 && at < 0x501100) globals[at-0x501000] = v;
        else if (at >= 0x501100 && at < 0x501110) ground_heights[at-0x501100] = v;
        else if (at >= 0x610000 && at < 0x610020) auxiliary[at-0x610000] = v;
        else if (at >= 0x5018a4 && at < 0x5018ac) yaw_globals[at-0x5018a4] = v;
        else if (at >= 0x501864 && at < 0x50186c) lookahead[at-0x501864] = v;
        else {
            std::fprintf(stderr,"vehicle oracle unexpected write address=%08x value=%02x routine=%08x\n",at,v,first_instruction);
            throw std::runtime_error("unexpected vehicle oracle write");
        }
    }
    void write_word(uint32_t at, uint16_t v) override {
        if (at == 0x884000) board.write_word(at,v);
        else { write_byte(at,uint8_t(v)); write_byte(at+1,uint8_t(v>>8)); }
    }
    void write_dword(uint32_t at, uint32_t v) override {
        if ((at >= 0x880000 && at <= 0x883ffc && at % 4 == 0) || at == 0x884000 || at == 0x91fff0)
            board.write_dword(at, v);
        else { write_word(at,uint16_t(v)); write_word(at+2,uint16_t(v>>16)); }
    }
    rt::I960Ref run(uint32_t begin, uint32_t end, const std::array<uint32_t,10> *arguments = nullptr) {
        first_instruction = begin; last_instruction = end;
        award_cues.clear();
        rt::I960Ref cpu(this);
        cpu.m_IP = begin;
        cpu.m_r[27] = 0x880000; // g11 command port
        cpu.m_r[28] = 0x4000;   // g12 FIFO displacement
        cpu.m_r[29] = 0x500000; // g13 scratch entity
        if (begin == 0x12464) cpu.m_r[29] = traffic_subject;
        if (begin == 0x13f90) cpu.m_r[4] = 0x500000; // r4 body in the travel preparation loop.
        if (arguments) for (unsigned i = 0; i < arguments->size(); ++i) cpu.m_r[16+i] = (*arguments)[i];
        yaw_conversion_valid = true;
        guided_conversion_valid = true;
        boundary_sound_requests = 0;
        boundary_sound_mask = 0;
        early_sound_requests = 0;
        early_conversion_valid = true;
        lateral_conversion_valid = true;
        collision_conversion_valid = true;
        body_conversion_valid = true;
        boundary_action = NM_BOUNDARY_NONE; boundary_division_valid = true;
        unsigned instructions = 0;
        while (fetch(cpu.m_IP) != 0x0a000000 || cpu.m_rcache_pos != 0) {
            if(begin==0xce74 && cpu.m_IP==0xceb0) break; // Timer/angles, before separately executed road/scenery/rank calls.
            if(begin==0x10dd8 && cpu.m_IP==0x10e6c) break; // Before speed product, exact target-adjustment block.
            if(begin==0x10f68 && cpu.m_IP==0x10fa8) break; // Before surface/path steering, exact heading-bias block.
            if (begin == 0xd394 && cpu.m_IP == 0xd3c8) break; // Before scenery; retain actual caller saves/restores.
            if ((begin == 0x134f8 || begin==0x1904c || begin==0x191d8) && cpu.m_IP == 0x19bdc) {
                if (award_cues.size() >= 3) throw std::runtime_error("checkpoint sound intent overflow");
                award_cues.push_back(cpu.m_r[16]);
            }
            if (begin == 0x11aa4 && (cpu.m_IP == 0x11aa8 || cpu.m_IP == 0x11b24)) {
                const double value = std::round(double(nm_force_float(cpu.m_r[16])));
                if (std::isnan(value)) {
                    cpu.m_r[16] = 0x80000000u; cpu.m_IP += 4; continue; // Existing80960KB masked NaN rule.
                }
                body_conversion_valid = std::isfinite(value) && value >= -2147483648.0 && value < 2147483648.0;
                if (!body_conversion_valid) break; // BEFORE undefined overflow cast; no reference mutation.
            }
            if (begin == 0x13f08 && cpu.m_IP == 0x1406c) break; // Preparation, before pair traversal.
            if (begin == 0x14180 && cpu.m_IP == 0x14630) break; // Before response dispatch.
            if (begin == 0x140a4 && cpu.m_IP == 0x140d0) break; // Command40 setup only.
            if (begin == 0x140e0 && (cpu.m_IP == 0x1413c || cpu.m_IP == 0x14168)) break; // Before pair swap/call or skip charge.
            if ((begin == 0x14904 || begin == 0x14630) && (cpu.m_IP == 0x148dc || cpu.m_IP == 0x148f0)) break;
            if (begin == 0x13f90 && cpu.m_IP == 0x13fe0) break;
            if (begin == 0x14fcc && cpu.m_IP == 0x15040) {
                const double value = std::round(double(nm_force_float(cpu.m_r[20])));
                if (std::isnan(value)) { // Existing80960KB masked CVTRI rule, no undefined host cast.
                    cpu.m_r[20] = 0x80000000u; cpu.m_IP = 0x15044; continue;
                }
                collision_conversion_valid = std::isfinite(value) && value >= -2147483648.0 && value < 2147483648.0;
                if (!collision_conversion_valid) break;
            }
            if ((begin == 0x13a14 || begin == 0x15238) && cpu.m_IP == 0x13af4) {
                const float value = nm_force_float(cpu.m_r[5]);
                lateral_conversion_valid = std::isfinite(value) && value >= -2147483648.0f && value < 2147483648.0f;
                if (!lateral_conversion_valid) break; // BEFORE the undefined reference CVTZRI cast.
            }
            // Held caller's timer/stores, stopping before its first motion call.
            if (begin == 0xd9c0 && cpu.m_IP == 0xd9e8) break;
            if (begin == 0x158c8 && cpu.m_IP == 0x19bdc) {
                if (cpu.m_r[16] != 0x9e067f || ++early_sound_requests > 1)
                    throw std::runtime_error("unexpected early-player sound intent");
            }
            if (begin == 0xe5f4 && cpu.m_IP == 0xe618) {
                const double value = std::round(double(nm_force_float(cpu.m_r[16])));
                if (std::isnan(value)) { // Same80960KB masked CVTRI rule as yaw; avoid host UB.
                    cpu.m_r[16] = 0x80000000u; cpu.m_IP = 0xe61c; continue;
                }
                if (!std::isfinite(value) || value < -2147483648.0 || value >= 2147483648.0) {
                    early_conversion_valid = false; break; // BEFORE undefined reference cast or stores.
                }
            }
            // Prefix oracle stops BEFORE executing the selected response.
            if (begin == 0x16060 && (cpu.m_IP == 0x161f8 || cpu.m_IP == 0x1638c || cpu.m_IP == 0x16bd4)) {
                boundary_action = cpu.m_IP == 0x161f8 ? NM_BOUNDARY_RECOVER : cpu.m_IP == 0x1638c ? NM_BOUNDARY_PIVOT : NM_BOUNDARY_SPIN;
                if (!boundary_complete) break;
            }
            if (++instructions > (begin == 0x1727c || begin == 0x132b8 || begin == 0x1225c || begin == 0x122dc || begin == 0x12464 || begin == 0x13f08 || begin == 0x1638c || begin == 0x16060 ? 2048u : begin == 0x116d0 || begin == 0x10ccc ? 1024u : 512u)) throw std::runtime_error("vehicle oracle instruction guard");
            if ((begin == 0x1638c || (begin == 0x16060 && boundary_complete)) && cpu.m_IP == 0x16734 && !cpu.m_r[7]) {
                boundary_division_valid = false; break; // Do not adopt I960Ref's DIV0 hack.
            }
            if ((begin == 0x1638c || (begin == 0x16060 && cpu.m_r[16] == 0x9e077f)) && cpu.m_IP == 0x19bdc) {
                if (cpu.m_r[16] != 0x9e077f) throw std::runtime_error("unexpected boundary sound request");
                ++boundary_sound_requests;
                // Execute the actual source sound gate with globals[90]=0.
                // Its early return emits no sound or device writes.
            }
            if ((begin == 0x14fcc || begin == 0x16bd4 || begin == 0x15eb8 || begin == 0x15d60 ||
                 (begin == 0x16060 && cpu.m_r[16] != 0x9e077f)) && cpu.m_IP == 0x19bdc) {
                const uint32_t sound = cpu.m_r[16];
                const unsigned bit = sound == 0x9e0a7f ? NM_CRASH_SOUND_START : sound == 0x9e087f ? NM_CRASH_SOUND_SMALL :
                                     sound == 0x9e1b7f ? NM_CRASH_SOUND_LARGE : 0;
                if (!bit || (boundary_sound_mask&bit)) throw std::runtime_error("unexpected or repeated damage sound intent");
                boundary_sound_mask |= bit;
            }
            if (begin == 0x10ccc && cpu.m_IP == 0x10f2c) {
                guided_divisor = cpu.m_r[9];
                const float value = std::bit_cast<float>(guided_divisor);
                // Stop before the reference's undefined out-of-range C++ cast.
                guided_conversion_valid = std::isfinite(value) && value >= -2147483648.0f && value < 2147483648.0f;
                if (!guided_conversion_valid) break;
            }
            if (begin == 0x11150 && cpu.m_IP == 0x111b8) {
                guided_divisor=cpu.m_r[9];
                const double value=std::round(double(std::bit_cast<float>(guided_divisor)));
                guided_conversion_valid=std::isfinite(value) && value>=-2147483648.0 && value<2147483648.0;
                if(!guided_conversion_valid) break;
            }
            if ((begin == 0x11d60 || begin == 0x11e80) &&
                (cpu.m_IP == 0x11de4 || cpu.m_IP == 0x11e2c || cpu.m_IP == 0x11f4c || cpu.m_IP == 0x11fb8)) {
                const double rounded = std::round(double(std::bit_cast<float>(cpu.m_r[3])));
                if (!std::isfinite(rounded) || rounded < -2147483648.0 || rounded >= 2147483648.0)
                    throw std::runtime_error("gear oracle conversion outside defined host domain");
            }
            if (begin == 0xfdf8 && cpu.m_IP == 0x101c4) {
                // Reject before I960Ref's signed C++ MULI could overflow.
                // Normal cases execute the actual instruction, unmodified.
                const int64_t product = int64_t(std::bit_cast<int32_t>(cpu.m_r[6])) *
                                        std::bit_cast<int32_t>(cpu.m_r[3]);
                if (product < INT32_MIN || product > INT32_MAX)
                    throw std::runtime_error("tire oracle MULI outside defined host domain");
            }
            if (begin == 0xf9d0 && cpu.m_IP == 0xfbd0) {
                // Avoid the reference's undefined host float-to-int cast.
                if (cpu.m_AC >> 30) throw std::runtime_error("yaw oracle requires rounding mode0");
                yaw_rotation = cpu.m_r[5];
                const double rounded = std::round(double(std::bit_cast<float>(cpu.m_r[5]))); // AC mode0.
                if (std::isnan(rounded)) {
                    // Intel80960KB PRM12-23 masked conversion result. Bypass
                    // the host's undefined cast; AC flags are outside this
                    // persistent-state oracle (the routine does not read them).
                    cpu.m_r[5] = 0x80000000u;
                    cpu.m_IP = 0xfbd4;
                    continue;
                }
                yaw_conversion_valid = std::isfinite(rounded) && rounded >= -2147483648.0 && rounded < 2147483648.0;
                if (!yaw_conversion_valid) break;
            }
            cpu.execute_one();
        }
        return cpu;
    }
    void check_drive(const nm_drive_params_t &params, const nm_drive_input_t &v) {
        entity.fill(0xa5); globals.fill(0x5a); curve_selector = v.curve;
        write_word(0x5000ac,v.throttle); write_byte(0x5000a8,v.brake);
        write_word(0x500150,v.rpm); write_dword(0x500148,v.gear);
        write_dword(0x50025c,uint32_t(v.compensation)); write_word(0x500028,uint16_t(v.pitch));
        write_dword(0x500018,v.speed_index); write_byte(0x50015e,uint8_t(v.drag_state));
        for (unsigned n = 0; n < 4; ++n) write_word(0x5001ba+2*n,v.contact[n]);
        auto expected = entity;
        const auto before_globals = globals;
        nm_drive_terms_t terms;
        if (!nm_drive_terms(&params,&v,&terms)) throw std::runtime_error("native drive rejected oracle input");
        const float values[] = {terms.drive,terms.brake[0],terms.brake[1],terms.slope,terms.drag};
        const unsigned offsets[] = {0x168,0x174,0x178,0x29c,0x160};
        for (unsigned n = 0; n < 5; ++n) std::memcpy(expected.data()+offsets[n],&values[n],4);
        run(0xecc8,0xef7c);
        if (expected != entity || before_globals != globals || curve_selector != v.curve) {
            for (unsigned n = 0; n < 5; ++n)
                std::fprintf(stderr,"drive offset=%03x native=%08x reference=%08x throttle=%u rpm=%u gear=%u pitch=%d\n",
                    offsets[n],std::bit_cast<uint32_t>(values[n]),read_dword(0x500000+offsets[n]),v.throttle,v.rpm,v.gear,v.pitch);
            throw std::runtime_error("native/reference drive memory mismatch");
        }
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("drive oracle left unread TGP output");
    }
    void check_axle(const nm_drive_params_t &params, const nm_axle_input_t &v, nm_axle_state_t state) {
        entity.fill(0xa5); globals.fill(0x5a);
        write_word(0x500018,uint16_t(v.speed_index));
        write_word(0x500028,uint16_t(v.pitch)); write_word(0x50002c,uint16_t(v.bank));
        write_dword(0x50003c,std::bit_cast<uint32_t>(v.longitudinal_delta));
        for (unsigned a = 0; a < 2; ++a) {
            write_dword(0x50016c+4*a,std::bit_cast<uint32_t>(v.inertial[a]));
            write_dword(0x500194+4*a,std::bit_cast<uint32_t>(state.load[a]));
        }
        for (unsigned n = 0; n < 4; ++n) write_word(0x5001ba+2*n,v.contact[n]);
        auto expected = entity;
        const auto before_globals = globals;
        if (!nm_axle_load_step(&params,&v,&state)) throw std::runtime_error("native axle rejected valid input");
        std::memcpy(expected.data()+0x194,&state,sizeof(state));
        run(0xef80,0xf274);
        if (expected != entity || before_globals != globals) {
            for (unsigned a = 0; a < 2; ++a)
                std::fprintf(stderr,"axle=%u native=%08x reference=%08x pitch=%d bank=%d speed=%d masks=%04x/%04x\n",
                    a,std::bit_cast<uint32_t>(state.load[a]),read_dword(0x500194+4*a),v.pitch,v.bank,
                    v.speed_index,v.contact[2*a],v.contact[2*a+1]);
            throw std::runtime_error("native/reference axle memory mismatch");
        }
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("axle oracle unread TGP output");
    }
    void check_forces(const nm_drive_params_t &params, const nm_forces_input_t &v, nm_forces_state_t state, unsigned index) {
        entity.fill(0xa5); globals.fill(0x5a); auxiliary.fill(0x35);
        const auto put_float = [&](unsigned offset, float value) { write_dword(0x500000+offset,std::bit_cast<uint32_t>(value)); };
        write_dword(0x500018,uint32_t(v.speed_index)); write_dword(0x500150,uint32_t(v.rpm));
        write_dword(0x500148,v.gear); write_word(0x50002c,uint16_t(v.bank)); write_word(0x5000b0,uint16_t(v.steer));
        write_byte(0x500099,uint8_t(v.condition_99)); write_byte(0x5000ec,uint8_t(v.condition_ec));
        write_byte(0x50015c,uint8_t(v.condition_15c)); write_byte(0x500265,uint8_t(v.condition_265));
        put_float(0x164,v.resistance); put_float(0x168,v.terms.drive);
        put_float(0x160,v.terms.drag); put_float(0x29c,v.terms.slope);
        for (unsigned a = 0; a < 2; ++a) {
            put_float(0x174+4*a,v.terms.brake[a]); put_float(0x194+4*a,v.axle.load[a]);
        }
        const auto put_state = [&](const nm_forces_state_t &s) {
            for (unsigned a = 0; a < 2; ++a) {
                put_float(0x16c+4*a,s.inertial[a]); put_float(0x17c+4*a,s.grip[a]);
                write_word(0x500188+2*a,uint16_t(s.tire_angle[a])); write_byte(0x500095+a,uint8_t(s.trigger_count[a]));
            }
            put_float(0x1a0,s.rear_bank_force); put_float(0x260,s.slip_decay);
            put_float(0x2a0,s.slip_ratio); put_float(0x3c,s.longitudinal_delta);
            write_word(0x500124,uint16_t(s.steer_angle)); write_dword(0x50024c,s.rpm_boost);
        };
        put_state(state);
        const auto initial = entity;
        const auto before_globals = globals;
        if (!nm_forces_step(&params,&v,&state)) {
            std::fprintf(stderr,"native forces rejected case=%u\n",index);
            throw std::runtime_error("native forces rejected oracle input");
        }
        put_state(state);
        const auto expected = entity;
        auto expected_auxiliary = auxiliary;
        if (state.trigger_pair) {
            for (unsigned a = 0; a < 2; ++a) {
                expected_auxiliary[16*a+3] |= 0x80;
                const uint32_t function = a ? 0x998c : 0x9970;
                std::memcpy(expected_auxiliary.data()+16*a+12,&function,4);
            }
        }
        entity = initial;
        run(0xf31c,0xf82c);
        if (entity != expected || globals != before_globals || auxiliary != expected_auxiliary) {
            for (unsigned at = 0; at < entity.size(); ++at) if (entity[at] != expected[at])
                std::fprintf(stderr,"forces case=%u offset=%03x native=%02x reference=%02x\n",index,at,expected[at],entity[at]);
            throw std::runtime_error("native/reference forces state or auxiliary event mismatch");
        }
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("forces oracle unread TGP output");
    }
    bool check_traction(const nm_drive_params_t &params, const nm_traction_input_t &v,
                        nm_traction_state_t state, unsigned index) {
        entity.fill(0xa5); globals.fill(0x5a); course_slot = v.course_slot;
        const auto put_float = [&](unsigned offset, float value) { write_dword(0x500000+offset,std::bit_cast<uint32_t>(value)); };
        write_dword(0x500148,v.gear);
        write_word(0x500124,uint16_t(v.steer_angle)); write_word(0x50018a,uint16_t(v.tire_angle));
        write_word(0x500128,uint16_t(v.phase[0])); write_word(0x50012a,uint16_t(v.phase[1]));
        write_word(0x500018,uint16_t(v.speed_index)); write_word(0x5000b0,uint16_t(v.steer));
        write_word(0x500028,uint16_t(v.pitch)); write_word(0x5001b8,v.surface);
        write_word(0x5000ac,v.accel); write_word(0x5000a8,v.brake); write_byte(0x50015f,v.override_count);
        for (unsigned a = 0; a < 4; ++a) write_word(0x5001ba+2*a,v.contact[a]);
        put_float(0x1cc,v.lateral); put_float(0x16c,v.inertial[0]); put_float(0x170,v.inertial[1]);
        /* Reference computes then discards these distance conversions. */
        put_float(0x1c4,float(index%32)*0.5f); put_float(0x1c8,-float(index%32)*0.5f);
        const auto put_state = [&](const nm_traction_state_t &s) {
            put_float(0x270,s.previous_lateral); put_float(0x168,s.drive);
            put_float(0x198,s.rear_load); put_float(0x29c,s.slope);
            write_word(0x50026c,uint16_t(s.condition_26c));
            for (unsigned a = 0; a < 6; ++a) write_byte(0x500264+a,uint8_t(s.countdown[a]));
        };
        put_state(state);
        const auto initial = entity;
        const auto before_globals = globals;
        const auto before_auxiliary = auxiliary;
        const bool accepted = nm_traction_step(&params,&v,&state);
        put_state(state);
        const auto expected = entity;
        entity = initial;
        run(0x10568,0x1097c);
        bool reference_finite = true;
        for (unsigned offset : {0x270,0x168,0x198,0x29c})
            reference_finite &= std::isfinite(std::bit_cast<float>(read_dword(0x500000+offset)));
        if (accepted != reference_finite || (accepted ? entity != expected : initial != expected) ||
            globals != before_globals || auxiliary != before_auxiliary) {
            for (unsigned at = 0; at < entity.size(); ++at) if (entity[at] != expected[at])
                std::fprintf(stderr,"traction case=%u offset=%03x native=%02x reference=%02x\n",index,at,expected[at],entity[at]);
            throw std::runtime_error("native/reference traction memory mismatch");
        }
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("traction oracle unread output");
        return accepted;
    }
    void check_speed(const nm_drive_params_t &params, uint32_t flags, unsigned gear,
                      nm_speed_state_t state, unsigned index) {
        entity.fill(0xa5); globals.fill(0x5a);
        write_dword(0x501090,flags); write_dword(0x500148,gear);
        const auto put_state = [&](const nm_speed_state_t &s) {
            write_dword(0x500034,std::bit_cast<uint32_t>(s.speed));
            write_dword(0x50003c,std::bit_cast<uint32_t>(s.delta));
            write_dword(0x500150,uint32_t(s.rpm)); write_dword(0x50025c,uint32_t(s.compensation));
        };
        put_state(state);
        const auto initial = entity;
        const auto before_globals = globals;
        if (!nm_speed_step(&params,flags,gear,&state)) throw std::runtime_error("native speed rejected oracle input");
        put_state(state);
        const auto expected = entity;
        entity = initial;
        run(0x10338,0x1040c);
        if (entity != expected || globals != before_globals) {
            for (unsigned at = 0; at < entity.size(); ++at) if (entity[at] != expected[at])
                std::fprintf(stderr,"speed case=%u offset=%03x native=%02x reference=%02x\n",index,at,expected[at],entity[at]);
            throw std::runtime_error("native/reference speed memory mismatch");
        }
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("speed oracle unread output");
    }
    bool check_yaw(const nm_drive_params_t &params, const nm_yaw_input_t &v,
                   nm_yaw_state_t state, unsigned index) {
        entity.fill(0xa5); globals.fill(0x5a); auxiliary.fill(0x35); yaw_globals.fill(0x53);
        course_slot = v.course_slot;
        write_dword(0x500034,std::bit_cast<uint32_t>(v.speed));
        write_dword(0x50003c,std::bit_cast<uint32_t>(float(int(index%32)-16)));
        write_word(0x500124,uint16_t(v.steer_angle)); write_word(0x5000b0,uint16_t(v.steer));
        write_word(0x500018,uint16_t(v.speed_index)); write_word(0x5001c2,uint16_t(v.road_heading));
        for (unsigned a = 0; a < 2; ++a) write_word(0x500188+2*a,uint16_t(v.tire_angle[a]));
        write_byte(0x5018a8,v.fixed_offset);
        const auto put_state = [&](const nm_yaw_state_t &s) {
            for (unsigned a = 0; a < 2; ++a) write_dword(0x50016c+4*a,std::bit_cast<uint32_t>(s.inertial[a]));
            write_dword(0x5001f0,s.alignment_frames); write_word(0x500058,uint16_t(s.yaw));
            write_word(0x500048,uint16_t(s.heading)); write_word(0x50002a,uint16_t(s.body_yaw));
            write_word(0x50024a,uint16_t(s.yaw_delta)); write_word(0x5018a4,uint16_t(s.offset));
        };
        put_state(state);
        const auto initial = entity;
        const auto initial_yaw = yaw_globals;
        const auto before_globals = globals;
        const auto before_auxiliary = auxiliary;
        const bool accepted = nm_yaw_step(&params,&v,&state);
        put_state(state);
        const auto expected = entity;
        const auto expected_yaw = yaw_globals;
        entity = initial; yaw_globals = initial_yaw;
        run(0xf9d0,0xfdf4);
        const bool valid = yaw_conversion_valid &&
            std::isfinite(std::bit_cast<float>(read_dword(0x50016c))) &&
            std::isfinite(std::bit_cast<float>(read_dword(0x500170)));
        if (accepted != valid || (accepted ? entity != expected || yaw_globals != expected_yaw :
                                            initial != expected || initial_yaw != expected_yaw) || globals != before_globals ||
            auxiliary != before_auxiliary || course_slot != v.course_slot) {
            for (unsigned at = 0; at < entity.size(); ++at) if (entity[at] != expected[at])
                std::fprintf(stderr,"yaw case=%u offset=%03x native=%02x reference=%02x\n",index,at,expected[at],entity[at]);
            for (unsigned at = 0; at < yaw_globals.size(); ++at) if (yaw_globals[at] != expected_yaw[at])
                std::fprintf(stderr,"yaw case=%u global=%08x native=%02x reference=%02x\n",index,0x5018a4+at,expected_yaw[at],yaw_globals[at]);
            throw std::runtime_error("native/reference yaw memory mismatch");
        }
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("yaw oracle unread output");
        return accepted;
    }
    void check_motion(const nm_drive_params_t &params, const nm_motion_input_t &v,
                      nm_motion_state_t s, unsigned index) {
        entity.fill(0xa5); globals.fill(0x5a); auxiliary.fill(0x35); yaw_globals.fill(0x53);
        const auto scalar = [&](uint32_t at, float value) { write_dword(at,std::bit_cast<uint32_t>(value)); };
        scalar(0x5001f8,v.scale); write_dword(0x500000,v.flags);
        write_dword(0x500018,uint32_t(v.speed_index)); write_word(0x5001f4,uint16_t(v.guided_ticks));
        write_word(0x5000e8,uint16_t(v.spin_heading)); write_word(0x5000ea,uint16_t(v.spin_rotation));
        write_word(0x5001ec,uint16_t(v.hold_rotation)); write_byte(0x5000ec,v.spin_ticks);
        for (unsigned a = 0; a < 3; ++a) scalar(0x5001e0+4*a,v.displacement[a]);
        const auto put = [&](const nm_motion_state_t &pose) {
            for (unsigned a = 0; a < 3; ++a) scalar(0x50001c+4*a,pose.position[a]);
            for (unsigned a = 0; a < 2; ++a) {
                scalar(0x50009c+8*a,pose.delta[a]); scalar(0x501864+4*a,pose.lookahead[a]);
            }
            scalar(0x500034,pose.speed); write_word(0x500048,uint16_t(pose.heading)); write_word(0x500058,uint16_t(pose.yaw));
        };
        put(s);
        const auto initial = entity; const auto initial_ahead = lookahead;
        const auto before_globals = globals; const auto before_yaw = yaw_globals; const auto before_auxiliary = auxiliary;
        if (!nm_motion_step(&params,&v,&s)) throw std::runtime_error("native motion rejected scratch input");
        put(s);
        const auto expected = entity; const auto expected_ahead = lookahead;
        entity = initial; lookahead = initial_ahead;
        run(v.held ? 0xec44 : 0xea04,v.held ? 0xecc4 : 0xebc0);
        if (entity != expected || lookahead != expected_ahead || globals != before_globals ||
            yaw_globals != before_yaw || auxiliary != before_auxiliary) {
            for (unsigned at = 0; at < entity.size(); ++at) if (entity[at] != expected[at])
                std::fprintf(stderr,"motion case=%u offset=%03x native=%02x reference=%02x\n",index,at,expected[at],entity[at]);
            for (unsigned at = 0; at < lookahead.size(); ++at) if (lookahead[at] != expected_ahead[at])
                std::fprintf(stderr,"motion case=%u lookahead=%u native=%02x reference=%02x\n",index,at,expected_ahead[at],lookahead[at]);
            throw std::runtime_error("native/reference motion memory mismatch");
        }
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("motion oracle unread output");
    }
    void check_resistance(const nm_drive_params_t &params, unsigned count, int mark=-1, uint32_t timer=0) {
        entity.fill(0xa5); globals.fill(0x5a);
        write_byte(0x50015f,uint8_t(count));
        write_word(0x500128,37);
        road_marks[37]=(uint8_t)mark;road_mark_timer=timer;
        auto expected = entity;
        const auto before_globals = globals;
        nm_resistance_state_t s{123,uint8_t(count)};
        if (!(mark<0 ? nm_resistance_step(&params,&s) : nm_resistance_marked_step(&params,timer,(int8_t)mark,&s)))
            throw std::runtime_error("native resistance rejected input");
        std::memcpy(expected.data()+0x164,&s.resistance,4); expected[0x15f] = s.override_count;
        const auto before_marks=road_marks;
        run(mark<0 ? 0x1132c : 0x11304,0x1136c);
        if (expected != entity || before_globals != globals || before_marks!=road_marks || road_mark_timer!=timer)
            throw std::runtime_error("native/reference resistance memory mismatch");
    }
    unsigned check_proximity(const nm_drive_params_t &params, const nm_proximity_input_t &v,
                         int8_t count, unsigned index) {
        // Check both constituent TGP commands even if the i960's distance
        // branch would skip the angle: whole-function parity alone can mask it.
        for (unsigned n = 0; n < 3; ++n) {
            const auto scalar = [&](float value) { write_dword(0x884000,std::bit_cast<uint32_t>(value)); };
            write_dword(0x880430,0x4343);
            scalar(v.position[0]); scalar(v.position[1]); scalar(v.neighbor[n][0]); scalar(v.neighbor[n][1]);
            const uint32_t distance = read_dword(0x884000);
            const float x = v.position[0]-v.neighbor[n][0], z = v.neighbor[n][1]-v.position[1];
            const uint32_t actual_distance = std::bit_cast<uint32_t>(nm_force_length(&params,x,z));
            write_dword(0x8800f0,0x0f0f);
            scalar(v.neighbor[n][1]); scalar(v.position[0]); scalar(v.position[1]); scalar(v.neighbor[n][0]);
            const uint32_t angle = read_dword(0x884000), actual_angle = uint32_t(nm_force_angle(&params,z,x));
            if (distance != actual_distance || angle != actual_angle) {
                std::fprintf(stderr,"proximity math case=%u neighbor=%u distance=%08x/%08x angle=%08x/%08x xz=%g/%g\n",
                    index,n,actual_distance,distance,actual_angle,angle,x,z);
                throw std::runtime_error("native/reference proximity command mismatch");
            }
        }
        entity.fill(0xa5); globals.fill(0x5a); auxiliary.fill(0x35); yaw_globals.fill(0x53);
        write_dword(0x50001c,std::bit_cast<uint32_t>(v.position[0]));
        write_dword(0x500024,std::bit_cast<uint32_t>(v.position[1]));
        write_word(0x500048,uint16_t(v.heading)); write_byte(0x50015e,uint8_t(count));
        write_dword(0x500088,0x620000);
        proximity_disabled = v.disabled ? 0x10000 : 0;
        for (unsigned n = 0; n < 3; ++n) {
            neighbors[n].fill(0xa5);
            std::memcpy(neighbors[n].data()+0x1c,&v.neighbor[n][0],4);
            std::memcpy(neighbors[n].data()+0x24,&v.neighbor[n][1],4);
            const uint32_t next = 0x620000+((n+1)%3)*0x1000;
            std::memcpy(neighbors[n].data()+0x88,&next,4);
        }
        auto expected = entity;
        const auto before_globals = globals;
        const auto before_auxiliary = auxiliary;
        const auto before_yaw = yaw_globals;
        if (!nm_proximity_step(&params,&v,&count)) throw std::runtime_error("native proximity rejected input");
        expected[0x15e] = uint8_t(count);
        const auto cpu = run(0x11370,0x11434);
        if (entity != expected || globals != before_globals || auxiliary != before_auxiliary || yaw_globals != before_yaw) {
            std::fprintf(stderr,"proximity case=%u native_count=%u reference=%u\n",index,expected[0x15e],entity[0x15e]);
            throw std::runtime_error("native/reference proximity memory mismatch");
        }
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("proximity oracle unread output");
        return !v.disabled && cpu.m_r[5] >= 1 && cpu.m_r[5] <= 3 ? 3-cpu.m_r[5] : 3;
    }
    void check_gears(const nm_drive_params_t &params, const nm_gears_input_t &v,
                     nm_gears_state_t state, unsigned index) {
        entity.fill(0xa5); globals.fill(0x5a); auxiliary.fill(0x35); yaw_globals.fill(0x53);
        gear_input_word = (0xa58fffffu & ~0x700000u) | uint32_t(v.selector)<<20;
        write_dword(0x500018,uint32_t(v.speed_index)); write_dword(0x50024c,uint32_t(v.rpm_bias));
        const auto put_state = [&](const nm_gears_state_t &s) {
            write_dword(0x500148,s.gear); write_dword(0x5000a8,s.brake);
            write_dword(0x500150,uint32_t(s.rpm)); write_dword(0x500154,uint32_t(s.filtered_rpm));
            write_dword(0x500158,uint32_t(s.downshift_rpm)); write_byte(0x50015c,uint8_t(s.shift_ticks));
        };
        put_state(state);
        const auto initial = entity;
        const auto before_globals = globals;
        const auto before_yaw = yaw_globals;
        const auto before_auxiliary = auxiliary;
        if (!nm_gears_step(&params,&v,&state)) {
            std::fprintf(stderr,"native gears rejected case=%u mode=%u gear=%u rpm=%d\n",index,v.manual,state.gear,state.rpm);
            throw std::runtime_error("native gears rejected oracle input");
        }
        put_state(state);
        const auto expected = entity;
        entity = initial;
        run(v.manual ? 0x11e80 : 0x11d60,v.manual ? 0x12064 : 0x11e7c);
        if (entity != expected || globals != before_globals || auxiliary != before_auxiliary || yaw_globals != before_yaw) {
            for (unsigned at = 0; at < entity.size(); ++at) if (entity[at] != expected[at])
                std::fprintf(stderr,"gears case=%u mode=%u offset=%03x native=%02x reference=%02x\n",index,v.manual,at,expected[at],entity[at]);
            throw std::runtime_error("native/reference gears memory mismatch");
        }
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("gears oracle unread output");
    }
    void check_tires(const nm_drive_params_t &params, const nm_tires_input_t &v,
                     nm_tires_state_t state, unsigned index) {
        entity.fill(0xa5); globals.fill(0x5a); auxiliary.fill(0x35); yaw_globals.fill(0x53);
        write_word(0x50024a,uint16_t(v.yaw_delta)); write_word(0x500018,uint16_t(v.speed_index));
        write_word(0x500124,uint16_t(v.steer_angle));
        write_dword(0x500168,std::bit_cast<uint32_t>(v.drive));
        write_dword(0x500034,std::bit_cast<uint32_t>(v.speed));
        write_dword(0x50017c,std::bit_cast<uint32_t>(v.front_grip));
        for (unsigned a = 0; a < 2; ++a) write_dword(0x500194+4*a,std::bit_cast<uint32_t>(v.load[a]));
        const auto put_state = [&](const nm_tires_state_t &s) {
            write_dword(0x500180,std::bit_cast<uint32_t>(s.rear_grip)); write_dword(0x500000,s.flags);
            write_word(0x500188,uint16_t(s.angle[0])); write_word(0x50018a,uint16_t(s.angle[1]));
            write_word(0x500248,uint16_t(s.filter)); write_word(0x50026c,uint16_t(s.condition));
            write_byte(0x500096,uint8_t(s.trigger_count)); write_byte(0x50015f,s.override_count);
        };
        put_state(state);
        const auto initial = entity;
        const auto before_globals = globals;
        const auto before_yaw = yaw_globals;
        if (!nm_tires_step(&params,&v,&state)) throw std::runtime_error("native tires rejected oracle input");
        put_state(state);
        const auto expected = entity;
        auto expected_auxiliary = auxiliary;
        if (state.trigger_pair) for (unsigned a = 0; a < 2; ++a) {
            expected_auxiliary[16*a+3] |= 0x80;
            const uint32_t function = a ? 0x998c : 0x9970;
            std::memcpy(expected_auxiliary.data()+16*a+12,&function,4);
        }
        entity = initial;
        run(0xfdf8,0x10268);
        if (entity != expected || globals != before_globals || auxiliary != expected_auxiliary || yaw_globals != before_yaw) {
            for (unsigned at = 0; at < entity.size(); ++at) if (entity[at] != expected[at])
                std::fprintf(stderr,"tires case=%u offset=%03x native=%02x reference=%02x\n",index,at,expected[at],entity[at]);
            throw std::runtime_error("native/reference tires memory or event mismatch");
        }
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("tires oracle unread output");
    }
    void check(nm_road_lateral_t initial) {
        const auto input = initial;
        entity.fill(0);
        std::memcpy(entity.data()+0x13e, &initial, sizeof(initial));
        auto expected_entity = entity;
        const auto expected_globals = globals;
        const auto cpu = run(0x13b28,0x13ddc);
        nm_road_lateral_t expected;
        std::memcpy(&expected, entity.data()+0x13e, sizeof(expected));
        nm_road_pose_t actual{};
        if (!nm_road_pose_step(&profile, &initial, &actual) ||
            std::memcmp(&expected, &initial, sizeof(initial))) {
            std::fprintf(stderr, "road input current/target/control/step=%d/%d/%d/%d native current/step=%d/%d reference=%d/%d\n",
                input.current,input.target,input.control,input.step,initial.current,initial.step,expected.current,expected.step);
            throw std::runtime_error("native/reference road lateral state mismatch");
        }
        std::memcpy(expected_entity.data()+0x13e,&initial,sizeof(initial));
        if (expected_entity != entity || expected_globals != globals)
            throw std::runtime_error("unexpected road routine memory write");
        const unsigned xyz_regs[] = {17,21,18}, angle_regs[] = {19,22,20};
        for (unsigned a = 0; a < 3; ++a)
            if (std::bit_cast<uint32_t>(actual.xyz[a]) != cpu.m_r[xyz_regs[a]] ||
                uint32_t(actual.angles[a]) != cpu.m_r[angle_regs[a]]) {
                std::fprintf(stderr,"road pose axis=%u current=%d native xyz=%08x reference=%08x angles=%d/%d\n",a,
                    initial.current,std::bit_cast<uint32_t>(actual.xyz[a]),cpu.m_r[xyz_regs[a]],actual.angles[a],int32_t(cpu.m_r[angle_regs[a]]));
                throw std::runtime_error("native/reference road pose mismatch");
            }
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("road oracle left unread TGP output");
    }
    void check_controls(nm_vehicle_test_t v) {
        entity.fill(0); globals.fill(0);
        write_byte(0x501000,v.adc[0]); write_byte(0x501004,v.adc[1]); write_byte(0x501008,v.adc[2]);
        write_dword(0x501050,0x12345678); // Previous steering is copied, not consumed.
        write_dword(0x501054,v.previous[0]); write_dword(0x501058,v.previous[1]);
        const auto before_entity = entity;
        auto expected_globals = globals;
        const auto input = nm_controls_decode(v.adc[0],v.adc[1],v.adc[2],v.previous[0],v.previous[1]);
        const uint32_t decoded[] = {uint32_t(input.steer),input.accel,input.brake,0x12345678,
            v.previous[0],v.previous[1],input.accel_high_edge,input.brake_high_edge};
        std::memcpy(expected_globals.data()+0x50,decoded,sizeof(decoded));
        run(0x4e5c,0x4fe4);
        if (globals != expected_globals || entity != before_entity) {
            std::fprintf(stderr,"ADC=%u/%u/%u native steering=%d reference=%d accel=%u/%u brake=%u/%u\n",
                v.adc[0],v.adc[1],v.adc[2],input.steer,int32_t(read_dword(0x501050)),
                input.accel,read_dword(0x501054),input.brake,read_dword(0x501058));
            throw std::runtime_error("native/reference decoded controls mismatch");
        }
        write_dword(0x5010d0,uint32_t(v.counter));
        write_dword(0x5000b0,uint32_t(v.state.steer));
        write_word(0x5000a8,v.state.brake); write_word(0x5000ac,v.state.accel);
        write_byte(0x50015f,v.state.override_count);
        write_byte(0x5000d4,uint8_t(v.condition_d4)); write_word(0x5000c8,uint16_t(v.condition_c8));
        write_dword(0x500018,uint32_t(v.speed));
        expected_globals = globals;
        auto expected_entity = entity;
        run(0xe93c,0xea00);
        if (!nm_vehicle_input_step(&v.state,&input,v.condition_d4+v.condition_c8 != 0,v.counter,v.speed))
            throw std::runtime_error("native input step rejected valid controls");
        std::memcpy(expected_entity.data()+0xb0,&v.state.steer,4);
        std::memcpy(expected_entity.data()+0xa8,&v.state.brake,2);
        std::memcpy(expected_entity.data()+0xac,&v.state.accel,2);
        expected_entity[0x15f] = v.state.override_count;
        if (entity != expected_entity || globals != expected_globals) {
            std::fprintf(stderr,"controls native/reference steer=%d/%d accel=%u/%u brake=%u/%u count=%u/%u\n",
                v.state.steer,int32_t(read_dword(0x5000b0)),v.state.accel,read_word(0x5000ac),
                v.state.brake,read_word(0x5000a8),v.state.override_count,read_byte(0x50015f));
            throw std::runtime_error("native/reference vehicle input state mismatch");
        }
    }
};

inline void check_drive(rt::M2Board &board, const char *path) {
    std::unique_ptr<nm_drive_params_t,decltype(&std::free)> params(nm_drive_load(path),&std::free);
    if (!params) throw std::runtime_error("invalid native drive pack");
    if (!board.tgp().booted() || !board.tgp().fifo_out_empty())
        throw std::runtime_error("drive oracle requires booted/idle TGP");
    VehicleOracleBus bus(board);
    for (unsigned i = 0; i < NM_DIVIDE_CHECK_CASES; ++i) {
        float a,b; nm_divide_test_case(i,&a,&b);
        bus.write_dword(0x880030,0x303);
        bus.write_dword(0x884000,std::bit_cast<uint32_t>(a));
        bus.write_dword(0x884000,std::bit_cast<uint32_t>(b));
        const uint32_t expected = bus.read_dword(0x884000);
        const uint32_t actual = std::bit_cast<uint32_t>(nm_force_divide(params.get(),a,b));
        if (expected != actual) {
            std::fprintf(stderr,"divide i=%u numerator=%08x denominator=%08x expected=%08x actual=%08x\n",
                i,std::bit_cast<uint32_t>(a),std::bit_cast<uint32_t>(b),expected,actual);
            throw std::runtime_error("native/reference full-table division mismatch");
        }
        bus.write_dword(0x880240,0x2424); bus.write_dword(0x884000,std::bit_cast<uint32_t>(a));
        bus.write_dword(0x880290,0x2929); bus.write_dword(0x884000,std::bit_cast<uint32_t>(b));
        bus.write_dword(0x880250,0x2525);
        if (bus.read_dword(0x884000) != actual)
            throw std::runtime_error("native/reference accumulator division mismatch");
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("division oracle unread output");
    }
    std::printf("native_divide PASS cases=%u hash=%08x exact_tgp_all_reciprocal_indices\n",
                NM_DIVIDE_CHECK_CASES,nm_divide_test_hash(params.get()));
    if (nm_divide_test_hash(params.get()) != NM_DIVIDE_CHECK_HASH)
        throw std::runtime_error("native division regression digest changed");
    for (unsigned i = 0; i < NM_FORCE_CHECK_CASES; ++i) {
        nm_force_polar_t a,b,result{}; nm_force_test_case(i,&a,&b);
        const auto command = [&](unsigned cmd) { bus.write_dword(0x880000+16*cmd,cmd*257); };
        const auto scalar = [&](float value) { bus.write_dword(0x884000,std::bit_cast<uint32_t>(value)); };
        command(3); scalar(a.magnitude); scalar(1+b.magnitude);
        const uint32_t division = bus.read_dword(0x884000);
        if (division != std::bit_cast<uint32_t>(nm_force_divide(params.get(),a.magnitude,1+b.magnitude)))
            throw std::runtime_error("native/reference force division mismatch");
        command(0x44); bus.write_dword(0x884000,uint32_t(a.angle)); scalar(a.magnitude);
        float cart[2]; nm_force_cartesian(params.get(),uint16_t(a.angle),a.magnitude,cart);
        for (unsigned n = 0; n < 2; ++n)
            if (bus.read_dword(0x884000) != std::bit_cast<uint32_t>(cart[n]))
                throw std::runtime_error("native/reference force Cartesian mismatch");
        command(0x0a); scalar(cart[1]); scalar(cart[0]);
        if (bus.read_dword(0x884000) != uint32_t(nm_force_angle(params.get(),cart[1],cart[0])))
            throw std::runtime_error("native/reference direct atan mismatch");
        command(0x46); bus.write_dword(0x884000,uint32_t(a.angle)); scalar(a.magnitude);
        bus.write_dword(0x884000,uint32_t(b.angle)); scalar(b.magnitude);
        const uint32_t angle = bus.read_dword(0x884000), magnitude = bus.read_dword(0x884000);
        if (!nm_force_combine(params.get(),&a,&b,&result) || angle != uint32_t(result.angle) ||
            magnitude != std::bit_cast<uint32_t>(result.magnitude)) {
            std::fprintf(stderr,"force case=%u angles=%d/%d native=%08x/%08x reference=%08x/%08x\n",
                i,a.angle,b.angle,uint32_t(result.angle),std::bit_cast<uint32_t>(result.magnitude),angle,magnitude);
            throw std::runtime_error("native/reference polar combination mismatch");
        }
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("force oracle unread output");
    }
    std::printf("native_force_math PASS cases=%u hash=%08x exact_divide_cartesian_polar\n",
                NM_FORCE_CHECK_CASES,nm_force_test_hash(params.get()));
    if (nm_force_test_hash(params.get()) != NM_FORCE_CHECK_HASH)
        throw std::runtime_error("native force math regression digest changed");
    for (unsigned angle = 0; angle < 65536; ++angle) {
        bus.write_dword(0x8801d0,0x1d1d);
        bus.write_dword(0x884000,angle);
        bus.write_dword(0x884000,std::bit_cast<uint32_t>(1.0f));
        if (bus.read_dword(0x884000) != std::bit_cast<uint32_t>(nm_drive_sine(params.get(),uint16_t(angle))))
            throw std::runtime_error("native/reference drive sine mismatch");
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("sine oracle unread output");
    }
    for (unsigned i = 0; i < NM_DRIVE_CHECK_CASES; ++i) bus.check_drive(*params,nm_drive_test_case(i));
    if (nm_drive_test_hash(params.get()) != NM_DRIVE_CHECK_HASH)
        throw std::runtime_error("native drive regression digest changed");
    std::printf("native_drive PASS cases=%u sine_angles=65536 hash=%08x exact_i960_tgp_memory; force terms only\n",
                NM_DRIVE_CHECK_CASES,nm_drive_test_hash(params.get()));
    for (unsigned i = 0; i < NM_AXLE_CHECK_CASES; ++i) {
        nm_axle_input_t v; nm_axle_state_t s; nm_axle_test_case(i,&v,&s);
        bus.check_axle(*params,v,s);
    }
    if (nm_axle_test_hash(params.get()) != NM_AXLE_CHECK_HASH)
        throw std::runtime_error("native axle regression digest changed");
    std::printf("native_axle PASS cases=%u hash=%08x exact_i960_tgp_memory; load filtering only\n",
                NM_AXLE_CHECK_CASES,nm_axle_test_hash(params.get()));
    unsigned triggers[2] = {}, boosts = 0;
    for (unsigned i = 0; i < NM_FORCES_CHECK_CASES; ++i) {
        nm_forces_input_t v; nm_forces_state_t s; nm_forces_test_case(i,&v,&s);
        bus.check_forces(*params,v,s,i);
        for (unsigned a = 0; a < 2; ++a) triggers[a] += bus.entity[0x95+a] != uint8_t(s.trigger_count[a]);
        boosts += bus.read_dword(0x50024c) != 0;
    }
    if (!triggers[0] || !triggers[1] || !boosts) throw std::runtime_error("forces sweep missed auxiliary/boost branches");
    std::printf("native_forces PASS cases=%u hash=%08x exact_i960_tgp_memory_and_events\n",
                NM_FORCES_CHECK_CASES,nm_forces_test_hash(params.get()));
    std::printf("native_forces coverage trigger95=%u trigger96=%u rpm_boost=%u\n",triggers[0],triggers[1],boosts);
    if (nm_forces_test_hash(params.get()) != NM_FORCES_CHECK_HASH)
        throw std::runtime_error("native forces regression digest changed");
    unsigned traction_rejected = 0;
    for (unsigned i = 0; i < NM_TRACTION_CHECK_CASES; ++i) {
        nm_traction_input_t v; nm_traction_state_t s; nm_traction_test_case(i,&v,&s);
        traction_rejected += !bus.check_traction(*params,v,s,i);
    }
    std::printf("native_traction PASS cases=%u hash=%08x exact_finite_memory nonfinite_reference_rejected=%u\n",
                NM_TRACTION_CHECK_CASES,nm_traction_test_hash(params.get()),traction_rejected);
    if (nm_traction_test_hash(params.get()) != NM_TRACTION_CHECK_HASH)
        throw std::runtime_error("native traction regression digest changed");
    /* Overflowed intermediates are allowed if the reference publishes finite
     * state after its comparisons/clamps. Do not reject those prematurely. */
    nm_traction_input_t extreme{}; nm_traction_state_t extreme_state{};
    extreme.gear = 1; extreme.lateral = std::bit_cast<float>(0x7f7fffffu);
    extreme.tire_angle = -4096; extreme.speed_index = 150; extreme.surface = 2;
    extreme_state.previous_lateral = -extreme.lateral; extreme_state.rear_load = 0.5f;
    bus.check_traction(*params,extreme,extreme_state,NM_TRACTION_CHECK_CASES);
    extreme = {}; extreme.gear = 1; extreme_state = {};
    extreme_state.drive = 1; extreme_state.rear_load = std::bit_cast<float>(0x7f7fffffu);
    extreme_state.countdown[0] = 1;
    bus.check_traction(*params,extreme,extreme_state,NM_TRACTION_CHECK_CASES+1);
    for (unsigned i = 0; i < NM_SPEED_CHECK_CASES; ++i) {
        nm_speed_state_t s; uint32_t flags; unsigned gear; nm_speed_test_case(i,&flags,&gear,&s);
        bus.check_speed(*params,flags,gear,s,i);
    }
    std::printf("native_speed PASS cases=%u hash=%08x exact_i960_tgp_memory\n",
                NM_SPEED_CHECK_CASES,nm_speed_test_hash(params.get()));
    if (nm_speed_test_hash(params.get()) != NM_SPEED_CHECK_HASH)
        throw std::runtime_error("native speed regression digest changed");
    unsigned yaw_rejected = 0, yaw_invalid_conversion = 0, yaw_nan = 0, yaw_aligned = 0, yaw_reset = 0;
    for (unsigned i = 0; i < NM_YAW_CHECK_CASES; ++i) {
        nm_yaw_input_t v; nm_yaw_state_t s; nm_yaw_test_case(i,&v,&s);
        if (!bus.check_yaw(*params,v,s,i)) {
            if (!yaw_rejected) std::printf("native_yaw first_invalid_case=%u rotation_bits=%08x conversion_valid=%d\n",
                                           i,bus.yaw_rotation,bus.yaw_conversion_valid);
            ++yaw_rejected; yaw_invalid_conversion += !bus.yaw_conversion_valid;
        } else {
            yaw_aligned += bus.read_word(0x500058) != uint16_t(uint32_t(int32_t(s.yaw))+uint32_t(int32_t(int16_t(bus.read_word(0x50024a)))));
            yaw_reset += s.alignment_frames && !bus.read_dword(0x5001f0);
        }
        yaw_nan += std::isnan(std::bit_cast<float>(bus.yaw_rotation));
    }
    std::printf("native_yaw PASS cases=%u hash=%08x exact_valid_memory invalid_reference_rejected=%u\n",
                NM_YAW_CHECK_CASES,nm_yaw_test_hash(params.get()),yaw_rejected);
    std::printf("native_yaw coverage invalid_conversion=%u masked_nan=%u aligned=%u counter_cleared=%u\n",
                yaw_invalid_conversion,yaw_nan,yaw_aligned,yaw_reset);
    if (!yaw_aligned || !yaw_reset || !yaw_nan || nm_yaw_test_hash(params.get()) != NM_YAW_CHECK_HASH)
        throw std::runtime_error("native yaw coverage or digest changed");
    unsigned tire_events = 0, tire_lower = 0, tire_upper = 0;
    for (unsigned i = 0; i < NM_TIRES_CHECK_CASES; ++i) {
        nm_tires_input_t v; nm_tires_state_t s; nm_tires_test_case(i,&v,&s);
        bus.check_tires(*params,v,s,i);
        tire_events += bus.auxiliary[3] != 0x35;
        tire_lower += bus.read_word(0x50018a) == uint16_t(-13824);
        tire_upper += bus.read_word(0x50018a) == 13824;
    }
    std::printf("native_tires PASS cases=%u hash=%08x exact_i960_tgp_memory_and_events\n",
                NM_TIRES_CHECK_CASES,nm_tires_test_hash(params.get()));
    std::printf("native_tires coverage events=%u rear_lower=%u rear_upper=%u\n",tire_events,tire_lower,tire_upper);
    if (!tire_events || !tire_lower || !tire_upper || nm_tires_test_hash(params.get()) != NM_TIRES_CHECK_HASH)
        throw std::runtime_error("native tires coverage or digest changed");
    unsigned gear_up[2]{}, gear_down[2]{}, gear_brake = 0;
    for (unsigned i = 0; i < NM_GEARS_CHECK_CASES; ++i) {
        nm_gears_input_t v; nm_gears_state_t s; nm_gears_test_case(i,params.get(),&v,&s);
        bus.check_gears(*params,v,s,i);
        gear_up[v.manual] += bus.read_dword(0x500148) > s.gear;
        gear_down[v.manual] += bus.read_dword(0x500148) < s.gear;
        gear_brake += bus.read_dword(0x5000a8) != s.brake;
    }
    std::printf("native_gears PASS cases=%u hash=%08x exact_i960_tgp_memory auto_up/down=%u/%u manual_up/down=%u/%u brake=%u\n",
        NM_GEARS_CHECK_CASES,nm_gears_test_hash(params.get()),gear_up[0],gear_down[0],gear_up[1],gear_down[1],gear_brake);
    if (!gear_up[0] || !gear_down[0] || !gear_up[1] || !gear_down[1] || !gear_brake ||
        nm_gears_test_hash(params.get()) != NM_GEARS_CHECK_HASH)
        throw std::runtime_error("native gears missing branch coverage");
    for (unsigned i = 0; i < 256; ++i) bus.check_resistance(*params,i);
    std::printf("native_resistance PASS cases=256 hash=%08x exact_i960_memory\n",nm_resistance_test_hash(params.get()));
    for(unsigned timer=0;timer<2;++timer) for(int mark=0;mark<5;++mark)
        for(unsigned count=0;count<256;++count) bus.check_resistance(*params,count,mark,timer ? UINT32_MAX : 0);
    std::puts("native_opponent_resistance PASS cases=2560 whole11304..1136c, road marks/timer unchanged");
    unsigned proximity_hits[4]{}, proximity_disabled = 0;
    for (unsigned i = 0; i < NM_PROXIMITY_CHECK_CASES; ++i) {
        nm_proximity_input_t v; int8_t count; nm_proximity_test_case(i,params.get(),&v,&count);
        ++proximity_hits[bus.check_proximity(*params,v,count,i)];
        proximity_disabled += v.disabled != 0;
    }
    std::printf("native_proximity PASS cases=%u hash=%08x exact_i960_tgp_memory hit1/2/3=%u/%u/%u no_hit=%u disabled=%u\n",
        NM_PROXIMITY_CHECK_CASES,nm_proximity_test_hash(params.get()),proximity_hits[0],proximity_hits[1],proximity_hits[2],proximity_hits[3],proximity_disabled);
    if (!proximity_hits[0] || !proximity_hits[1] || !proximity_hits[2] || !proximity_disabled ||
        nm_resistance_test_hash(params.get()) != NM_RESISTANCE_CHECK_HASH ||
        nm_proximity_test_hash(params.get()) != NM_PROXIMITY_CHECK_HASH)
        throw std::runtime_error("missing proximity coverage");
    for (unsigned i = 0; i < NM_MOTION_CHECK_CASES; ++i) {
        nm_motion_input_t v; nm_motion_state_t s; nm_motion_test_case(i,&v,&s);
        bus.check_motion(*params,v,s,i);
    }
    std::printf("native_motion PASS cases=%u hash=%08x exact_i960_tgp_memory normal/spin/held; guided10ccc NOT included\n",
        NM_MOTION_CHECK_CASES,nm_motion_test_hash(params.get()));
    if (nm_motion_test_hash(params.get()) != NM_MOTION_CHECK_HASH)
        throw std::runtime_error("native motion regression digest changed");
}

// Actual player-call inputs, not end-of-frame snapshots. Read RAM only; do not
// run a second TGP, touch FIFOs or replace the reference's published state.
struct BoundaryPivotCase {
    std::array<uint8_t,0x300> entity;
    int16_t signal;
    uint8_t shake;
};
struct BoundarySnapshot {
    std::array<uint8_t,0x300> entity;
    std::array<uint8_t,0x100> globals;
    std::array<uint8_t,32> auxiliary;
    std::array<uint8_t,16> heights;
    uint32_t last_surface;
    uint16_t center_mask, spin_signal, pivot_signal;
    uint8_t shake, sequence_counter, curve;
};
struct BoundarySelectionCase { BoundarySnapshot before, after; bool complete = false; };
inline BoundarySnapshot capture_boundary(rt::M2Board &board, uint32_t at) {
    const uint32_t auxiliary = board.read_dword(0x501270);
    if (at < 0x500000 || at > 0x5ffd00 || at%16 || auxiliary < 0x500000 || auxiliary > 0x5fffe0 || auxiliary%16)
        throw std::runtime_error("invalid boundary capture pointer");
    BoundarySnapshot input{};
    for (unsigned i = 0; i < input.entity.size(); ++i) input.entity[i] = board.read_byte(at+i);
    for (unsigned i = 0; i < input.heights.size(); ++i) input.heights[i] = board.read_byte(0x501100+i);
    for (unsigned i = 0; i < input.globals.size(); ++i) input.globals[i] = board.read_byte(0x501000+i);
    for (unsigned i = 0; i < input.auxiliary.size(); ++i) input.auxiliary[i] = board.read_byte(auxiliary+i);
    input.center_mask = board.read_word(0x5018b0); input.last_surface = board.read_dword(0x5018ac);
    input.spin_signal = board.read_word(0x501a04); input.pivot_signal = board.read_word(0x501a02);
    input.shake = board.read_byte(0x501830); input.sequence_counter = board.read_byte(0x503680);
    input.curve = board.read_byte(0x501464);
    return input;
}
struct DamagePreCase {
    std::array<uint8_t,0x300> entity;
    std::array<uint8_t,0x100> globals;
    std::array<uint8_t,32> auxiliary;
    std::array<uint8_t,48> camera;
    uint32_t last_surface, control;
    int32_t camera_mode;
    uint16_t pivot_signal, spin_signal;
    uint8_t shake, sequence_counter, variant, restore_mode;
};
struct RoadSelectionCase {
    std::array<uint8_t,0x300> before, after;
    uint32_t mapping, flags;
};
struct LiveDriveCheck {
    LivePlayerCheck player_chain;
    LivePlayerCheck rolling_chain;
    PlayerMemory grid_expected;
    uint32_t grid_entity = 0;
    uint64_t grid_ids = 0;
    unsigned grid_checked = 0;
    uint32_t marked_entity=0;
    std::array<uint8_t,0x300> marked_expected{};
    uint64_t marked_checked=0, marked_ids=0;
    PlayerMemory opponent_expected;
    uint32_t opponent_entity=0;
    uint64_t opponent_checked=0, opponent_ids=0, opponent_released=0;
    uint32_t lane_entity=0;
    std::array<uint8_t,0x300> lane_expected{};
    uint64_t lane_checked=0,lane_ids=0;
    PlayerMemory guided_expected;
    uint32_t guided_entity=0;
    uint64_t guided_checked=0,guided_ids=0;
    PlayerMemory motion_expected;
    uint32_t motion_entity=0;
    uint64_t motion_checked=0,motion_ids=0;
    PlayerMemory finish_expected;
    uint32_t finish_entity=0,finish_owner=0;
    uint16_t finish_timer=0;
    uint8_t finish_event=0;
    uint64_t finish_checked=0,finish_visible=0,finish_height=0,finish_blocked_trigger=0;
    nm_player_t grid_reset_state{};
    float grid_reset_height = 0;
    bool grid_reset_pending = false;
    std::array<nm_race_car_t,44> native_grid_cars{};
    uint64_t native_grid_ids=0;
    unsigned native_grid_resets=0;
    std::array<nm_race_car_t,44> field_input{};
    nm_race_shared_t field_shared{};
    nm_intro_speed_params_t field_speed{};
    const nm_path_pack_t *field_path=nullptr;
    std::array<uint8_t,4096> field_marks{};
    uint32_t field_mark_timer=0;
    uint64_t field_checked=0;
    nm_player_t rolling_start_state{};
    float rolling_start_height=0;
    bool rolling_start_ready=false;
    std::vector<std::array<float,10>> boundary_inputs; // Host oracle inputs only; never target assets.
    std::vector<BoundaryPivotCase> boundary_pivots;
    std::vector<BoundarySelectionCase> boundary_selections;
    std::vector<DamagePreCase> damage_pre;
    std::vector<RoadSelectionCase> road_selections;
    bool capture_road = false, road_pending = false, boundary_pending = false;
    rt::M2Board &board;
    std::unique_ptr<nm_drive_params_t,decltype(&std::free)> params{nullptr,&std::free};
    std::array<uint8_t,0x300> expected{};
    std::array<uint8_t,8> expected_yaw{};
    std::array<uint8_t,8> expected_ahead{};
    std::array<uint8_t,16> expected_heights{};
    nm_contact_pack_t contact{};
    std::array<uint8_t,32> expected_auxiliary{};
    uint32_t auxiliary_address[2]{};
    uint32_t entity = 0;
    unsigned pending = 0;
    LiveCollisionCheck collisions;
    // Host-only observed normal-field order; never replay this as target gameplay.
    std::vector<unsigned> field_turns;
    std::map<std::vector<unsigned>,uint64_t> field_orders;
    std::map<uint32_t,uint64_t> field_modes;
    std::vector<unsigned> scene_turns;
    std::map<std::vector<unsigned>,uint64_t> scene_orders;
    std::array<nm_race_car_t,44> movement_cars{};
    std::array<PlayerMemory,44> movement_expected{};
    nm_race_shared_t movement_shared{};
    nm_checkpoint_award_t movement_award{};
    std::vector<uint8_t> movement_marks;
    uint32_t movement_mark_timer=0;
    uint32_t movement_base=0,movement_stride=0;
    unsigned movement_count=0;
    uint64_t movement_checked=0;
    void begin_movement() {
        const auto &chain=player_chain;
        if(chain.movement_input.phase!=22) return;
        if(movement_count) throw std::runtime_error("nested whole-field movement check");
        const uint32_t base=board.read_dword(0x501220),stride=board.read_dword(base+8);
        const unsigned count=board.read_dword(0x501080);
        if(count<3 || count>44 || stride<0x300 || base<0x500000 || uint64_t(base)+uint64_t(count)*stride>0x600000)
            throw std::runtime_error("invalid whole-field source pool");
        // This gate covers normal-player cycles with ALL opponents normal.
        // Rolling/other phases require their own full-cycle coverage, not coercion.
        for(unsigned id=1;id<count;++id) if(board.read_dword(base+id*stride+12)!=0xcde0) return;
        const auto index=[&](uint32_t ptr) -> uint16_t {
            if(ptr<base || (ptr-base)%stride || (ptr-base)/stride>=count)
                throw std::runtime_error("invalid whole-field native link");
            return (uint16_t)((ptr-base)/stride);
        };
        const auto &road=chain.road;
        const uint32_t table=board.read_dword(board.read_dword(0x3a48+4*road.slot)+16);
        const unsigned variant=board.read_byte(0x503690);
        if(variant>=3) throw std::runtime_error("whole-field crash variant outside checked domain");
        const uint32_t sequence=board.read_dword(board.read_dword(0x2896e58+4*variant)+4);
        const unsigned row_count=board.read_dword(sequence);
        if(!row_count || row_count>2048)
            throw std::runtime_error("whole-field crash variant outside checked domain");
        std::vector<nm_crash_row_t> rows(row_count);
        for(unsigned r=0;r<row_count;++r) for(unsigned b=0;b<sizeof(rows[r]);++b)
            reinterpret_cast<uint8_t *>(&rows[r])[b]=board.read_byte(sequence+8+16*r+b);
        for(unsigned id=0;id<count;++id) {
            const uint32_t at=base+id*stride;
            auto &memory=movement_expected[id];memory=chain.movement_memory;
            if(id) memory.sequence=sequence;
            for(unsigned b=0;b<memory.entity.size();++b) memory.entity[b]=board.read_byte(at+b);
            auto &n=movement_cars[id];n={};memory.fields(n.car,false);
            n.next=index(board.read_dword(at+0x88));n.previous=index(board.read_dword(at+0x8c));
            const uint32_t saved=board.read_dword(at+0x244);
            n.remembered=saved ? (uint8_t)index(saved) : 255;
            n.route=255;
            for(unsigned r=0;r<8;++r) if(board.read_dword(table+8*r)==board.read_dword(at+0x130)) n.route=(uint8_t)r;
            n.mode=board.read_byte(at+0x236);n.rank=board.read_byte(at+0x6a);
            n.near_marker=nm_yaw_angle(board.read_word(at+0x146));
            n.guided_scale=nm_force_float(board.read_dword(at+0x238));
            n.other_yaw=nm_yaw_angle(board.read_word(at+0x212));n.relative=board.read_dword(at+0x2a4);
            n.effects.height={board.read_word(at+0x27e),nm_force_float(board.read_dword(at+0x280)),
                nm_force_float(board.read_dword(at+0x284)),nm_force_float(board.read_dword(at+0x288)),nm_force_float(board.read_dword(at+0xa0))};
            n.effects.correction_yaw=nm_yaw_angle(board.read_word(at+0x216));n.effects.contact_age=board.read_byte(at+0x235);
        }
        auto &s=movement_shared;s={};
        s.flags=board.read_dword(0x501090);s.clock=board.read_dword(0x5010cc);s.tick=board.read_word(0x5010b0);
        s.timer_hold=board.read_dword(0x501a84);
        s.guided_mode=board.read_byte(0x5014f7);s.profile_updates=board.read_byte(0x5014e0);
        s.crash_counter=board.read_byte(0x503680);s.event_timer=nm_yaw_angle(board.read_word(0x501894));s.event=board.read_byte(0x501898);
        const uint32_t owner=board.read_dword(0x5014e4);s.inhibit_owner=owner ? (uint8_t)index(owner) : 255;
        for(unsigned c=0;c<16;++c) s.visible_cells[c]=board.read_dword(0x501520+4*c);
        nm_intro_speed_params_t speed{};
        speed.desired_divisor=nm_force_float(board.read_dword(0x28b57a4));speed.drag_offset=nm_force_float(board.read_dword(0x28b5814));
        speed.minimum_delta=nm_force_float(board.read_dword(0x28b57b8));
        for(unsigned i=0;i<512;++i) speed.response[i]=nm_force_float(board.read_dword(0x28b85a0+4*i));
        std::vector<uint8_t> values(road.profile_count);
        for(unsigned i=0;i<values.size();++i) values[i]=board.read_byte(0x53b71c+i);
        nm_road_marks_t marks{values.data(),road.profile_count-1,board.read_dword(0x53b718)};
        auto award=chain.movement_award;uint8_t rolling[44]={};uint32_t start_flags=0;
        if(!nm_race_movement_step(movement_cars.data(),count,&s,rolling,&start_flags,params.get(),&speed,
            &contact,&road,chain.paths.data(),&chain.movement_input,&award,&marks,road.profile_count,rows.data(),row_count))
            throw std::runtime_error("whole-field movement rejected live inputs");
        movement_award=award;movement_mark_timer=marks.timer;movement_marks=std::move(values);
        movement_base=base;movement_stride=stride;movement_count=count;
    }
    void finish_movement() {
        if(!movement_count) return;
        const auto &s=movement_shared;
        const uint32_t owner=s.inhibit_owner==255 ? 0 : movement_base+s.inhibit_owner*movement_stride;
        if(s.clock!=board.read_dword(0x5010cc) || s.timer_hold!=board.read_dword(0x501a84) || s.profile_updates!=board.read_byte(0x5014e0) ||
           s.crash_counter!=board.read_byte(0x503680) || owner!=board.read_dword(0x5014e4) ||
           (uint16_t)s.event_timer!=board.read_word(0x501894) || s.event!=board.read_byte(0x501898))
            throw std::runtime_error("whole-field shared movement outputs differ from source");
        if(movement_mark_timer!=board.read_dword(0x53b718))
            throw std::runtime_error("whole-field road mark timer differs from source");
        for(unsigned i=0;i<movement_marks.size();++i) if(movement_marks[i]!=board.read_byte(0x53b71c+i))
            throw std::runtime_error("whole-field road mark differs from source");
        for(unsigned id=0;id<movement_count;++id) {
            auto &n=movement_cars[id];
            const uint32_t at=movement_base+id*movement_stride;
            // Compare mapped car fields; independent per-turn oracles still
            // validate unmapped source bytes, side effects and shared memory.
            auto actual=movement_expected[id];
            for(unsigned b=0;b<actual.entity.size();++b) actual.entity[b]=board.read_byte(at+b);
            auto overlay=actual;overlay.fields(n.car,true);
            if(id) {
                const auto &h=n.effects.height;
                memcpy(overlay.entity.data()+0x27e,&h.sampled_tick,2);
                memcpy(overlay.entity.data()+0x280,&h.sampled_height,4);
                memcpy(overlay.entity.data()+0x284,&h.sampled_velocity,4);
                memcpy(overlay.entity.data()+0x288,&h.reference_height,4);
                memcpy(overlay.entity.data()+0xa0,&h.velocity,4);
                memcpy(overlay.entity.data()+0x216,&n.effects.correction_yaw,2);
                overlay.entity[0x235]=n.effects.contact_age;
                const uint32_t remembered=n.remembered==255 ? 0 : movement_base+n.remembered*movement_stride;
                memcpy(overlay.entity.data()+0x244,&remembered,4);
            } else {
                for(unsigned b=0;b<actual.globals.size();++b) actual.globals[b]=board.read_byte(0x501000+b);
                for(unsigned b=0;b<actual.checkpoint_extra.size();++b) actual.checkpoint_extra[b]=board.read_byte(0x501400+b);
                overlay.globals=actual.globals;overlay.checkpoint_extra=actual.checkpoint_extra;
                auto award=movement_award;int16_t signal=n.car.boundary.pivot_signal;
                checkpoint_award_fields(overlay.entity,overlay.globals,overlay.checkpoint_extra,award,signal,true);
                if(overlay.globals!=actual.globals || overlay.checkpoint_extra!=actual.checkpoint_extra)
                    throw std::runtime_error("whole-field checkpoint award outputs differ from source");
            }
            if(overlay.entity!=actual.entity) {
                for(unsigned b=0;b<actual.entity.size();++b) if(overlay.entity[b]!=actual.entity[b])
                    std::fprintf(stderr,"whole-field frame=%llu id=%u offset=%03x native=%02x reference=%02x\n",
                        (unsigned long long)board.frame(),id,b,overlay.entity[b],actual.entity[b]);
                throw std::runtime_error("whole-field movement differs from source");
            }
        }
        ++movement_checked;movement_count=0;
    }
    void finish_field_order() {
        if(!field_turns.empty()) { ++field_orders[field_turns];field_turns.clear(); }
    }
    uint64_t entered[10]{}, checked[10]{}, rejected[10]{}, zero_speed = 0, straight = 0, rejected_turn = 0;
    uint64_t motion_normal = 0, motion_spin = 0, motion_guided = 0;
    uint64_t gear_up[2]{}, gear_down[2]{}, gear_hold_calls = 0;
    uint64_t proximity_disabled_calls = 0, proximity_refreshes = 0;
    int32_t gear_hold_expected = 0;
    bool accepted = false;
    explicit LiveDriveCheck(rt::M2Board &b, const char *path, const char *contact_path = nullptr) : board(b) {
        params.reset(nm_drive_load(path));
        if (!params) throw std::runtime_error("invalid live native drive pack");
        if (contact_path && !nm_contact_load(&contact,contact_path))
            throw std::runtime_error("invalid live native contact pack");
    }
    ~LiveDriveCheck() { nm_contact_unload(&contact); }
    void predict_opponent_finish(uint32_t entity) {
        if(finish_entity) throw std::runtime_error("nested opponent finish");
        finish_expected=motion_expected; // Carry native movement output, not recaptured car state.
        for(unsigned i=0;i<finish_expected.globals.size();++i) finish_expected.globals[i]=board.read_byte(0x501000+i);
        for(unsigned i=0;i<finish_expected.checkpoint_extra.size();++i) finish_expected.checkpoint_extra[i]=board.read_byte(0x501400+i);
        finish_expected.sequence_counter=board.read_byte(0x503680);
        const unsigned variant=board.read_dword(0x501090)&0x10000000 ? 2 : board.read_byte(0x503690);
        if(variant>=3) throw std::runtime_error("opponent crash variant outside native domain");
        uint32_t sequence=board.read_dword(board.read_dword(0x2896e58+4*variant)+4);
        const uint32_t pointer=board.read_dword(entity+0x278);
        //15df4..15e00: a trigger does NOT replace an active sequence while
        //d4/c8 inhibit it. Keep the actual sequence behind the retained row.
        const bool trigger=(board.read_dword(entity)&64)!=0;
        const bool restart=trigger && !board.read_byte(entity+0xd4) && !board.read_word(entity+0xc8);
        finish_blocked_trigger+=trigger && !restart;
        if(board.read_word(entity+0x274) && !restart) {
            for(unsigned v=0;v<3;++v) {
                const auto start=board.read_dword(board.read_dword(0x2896e58+4*v)+4);
                if(pointer>=start+4 && pointer<start+4+16*board.read_dword(start)) sequence=start;
            }
        }
        finish_expected.sequence=sequence;
        const unsigned count=board.read_dword(sequence);
        if(!count || count>2048) throw std::runtime_error("invalid opponent crash rows");
        std::vector<nm_crash_row_t> rows(count);
        // ROM rows have16-byte stride; native rows retain only12 payload bytes.
        for(unsigned row=0;row<count;++row) for(unsigned byte=0;byte<sizeof(rows[row]);++byte)
            reinterpret_cast<uint8_t *>(&rows[row])[byte]=board.read_byte(sequence+8+16*row+byte);
        nm_player_t state{},player{};finish_expected.fields(state,false);
        if(pointer>=sequence+4 && (pointer-sequence-4)%16==0) state.boundary.damage.row=(uint16_t)((pointer-sequence-4)/16);
        const uint32_t player_at=board.read_dword(0x501220);
        player.boundary.speed=nm_force_float(board.read_dword(player_at+0x34));
        player.phase[0]=std::bit_cast<int16_t>(board.read_word(player_at+0x128));
        nm_opponent_finish_state_t effects{};
        effects.height={board.read_word(entity+0x27e),nm_force_float(board.read_dword(entity+0x280)),
            nm_force_float(board.read_dword(entity+0x284)),nm_force_float(board.read_dword(entity+0x288)),nm_force_float(board.read_dword(entity+0xa0))};
        effects.correction_yaw=std::bit_cast<int16_t>(board.read_word(entity+0x216));effects.contact_age=board.read_byte(entity+0x235);
        const unsigned cell=state.scenery_cell;
        if(cell>=512) throw std::runtime_error("opponent visibility cell outside domain");
        nm_opponent_finish_input_t in{&player,rows.data(),count,board.read_word(0x5010b0),
            std::bit_cast<int16_t>(board.read_word(0x501894)),std::bit_cast<int16_t>(board.read_word(entity+0x212)),
            board.read_byte(entity+4),board.read_byte(entity+0x236),board.read_byte(0x5014f7),
            (uint8_t)((board.read_dword(0x501520+4*(cell>>5))>>(cell&31))&1),std::bit_cast<int8_t>(board.read_byte(entity+0x2a4))};
        const auto before=state;const auto old_effects=effects;
        auto bad_road=rolling_chain.road;bad_road.profiles=nullptr;
        if(nm_opponent_finish_step(params.get(),&contact,&bad_road,&in,&effects,&state) ||
           memcmp(&before,&state,sizeof(state)) || memcmp(&old_effects,&effects,sizeof(effects)))
            throw std::runtime_error("opponent finish late failure changed state");
        if(!nm_opponent_finish_step(params.get(),&contact,&rolling_chain.road,&in,&effects,&state))
            throw std::runtime_error("opponent finish rejected live input");
        // Independent field adapter starts BEFORE motion and owns both halves.
        const unsigned id=in.id;
        field_input[id].effects=old_effects;
        field_input[id].car.boundary.damage.row=before.boundary.damage.row;
        field_input[id].other_yaw=in.other_yaw;field_input[id].relative=in.relative;
        const auto old_car=field_input[id];const auto old_shared=field_shared;
        nm_road_marks_t marks{field_marks.data(),rolling_chain.road.profile_count,field_mark_timer};
        if(nm_race_opponent_step(field_input.data(),44,id,&field_shared,params.get(),&field_speed,&contact,
            &rolling_chain.road,nullptr,&marks,rows.data(),count) ||
           memcmp(&old_car,&field_input[id],sizeof(old_car)) || memcmp(&old_shared,&field_shared,sizeof(old_shared)))
            throw std::runtime_error("native field rejection changed car/shared state");
        if(!nm_race_opponent_step(field_input.data(),44,id,&field_shared,params.get(),&field_speed,&contact,
            &rolling_chain.road,field_path,&marks,rows.data(),count))
            throw std::runtime_error("native field adapter rejected live inputs");
        PlayerMemory field_memory=finish_expected;
        field_memory.fields(field_input[id].car,true);
        PlayerMemory composed=finish_expected;composed.fields(state,true);
        uint32_t remembered_pointer;memcpy(&remembered_pointer,finish_expected.entity.data()+0x244,4);
        if(field_memory.entity!=composed.entity || memcmp(&effects,&field_input[id].effects,sizeof(effects)) ||
           field_input[id].remembered!=(remembered_pointer ? board.read_byte(remembered_pointer+4) : 255) ||
           field_shared.profile_updates!=state.profile_updates || field_shared.crash_counter!=state.boundary.damage.sequence_counter ||
           field_shared.inhibit_owner!=((effects.requests&1) ? id : old_shared.inhibit_owner) ||
           field_shared.event_timer!=((effects.requests&2) ? 320 : old_shared.event_timer) ||
           field_shared.event!=((effects.requests&2) ? 4 : old_shared.event))
            throw std::runtime_error("native field adapter differs from connected source-checked update");
        ++field_checked;
        finish_visible+=in.visible;
        finish_height+=in.visible && ((uint32_t)(int32_t)in.relative<=4 || (state.boundary.damage.flags&1024));
        finish_expected.fields(state,true);
        if(state.boundary.damage.row<=count && (before.boundary.damage.remaining || restart)) {
            const uint32_t next=sequence+4+16*state.boundary.damage.row;
            memcpy(finish_expected.entity.data()+0x278,&next,4);
        }
        const auto &h=effects.height;
        memcpy(finish_expected.entity.data()+0x27e,&h.sampled_tick,2);
        memcpy(finish_expected.entity.data()+0x280,&h.sampled_height,4);
        memcpy(finish_expected.entity.data()+0x284,&h.sampled_velocity,4);
        memcpy(finish_expected.entity.data()+0x288,&h.reference_height,4);
        memcpy(finish_expected.entity.data()+0xa0,&h.velocity,4);
        memcpy(finish_expected.entity.data()+0x216,&effects.correction_yaw,2);
        finish_expected.entity[0x235]=effects.contact_age;
        finish_owner=effects.requests&1 ? entity : board.read_dword(0x5014e4);
        finish_timer=effects.requests&2 ? 320 : board.read_word(0x501894);
        finish_event=effects.requests&2 ? 4 : board.read_byte(0x501898);
        finish_entity=entity;
    }
    nm_guided_lane_car_t lane_car(uint32_t at) const {
        nm_guided_lane_car_t c{};c.id=board.read_byte(at+4);c.flags=board.read_dword(at);
        c.speed=std::bit_cast<int32_t>(board.read_dword(at+0x18));
        c.lateral=std::bit_cast<int16_t>(board.read_word(at+0x13e));
        c.marker=std::bit_cast<int16_t>(board.read_word(at+0x146));
        c.mode=std::bit_cast<int8_t>(board.read_byte(at+0x236));
        c.position[0]=nm_force_float(board.read_dword(at+0x1c));
        c.position[1]=nm_force_float(board.read_dword(at+0x24));return c;
    }
    static void observe_return(void *context, const rt::Cpu &cpu, uint32_t source) {
        auto &self = *static_cast<LiveDriveCheck *>(context);
        if(source==0xcebc && self.finish_entity) {
            if(cpu.m_r[29]!=self.finish_entity) throw std::runtime_error("opponent finish entity mismatch");
            for(unsigned i=0;i<self.finish_expected.entity.size();++i)
                if(self.board.read_byte(self.finish_entity+i)!=self.finish_expected.entity[i]) {
                    std::fprintf(stderr,"opponent finish frame=%llu id=%u case=%llu offset=%03x native=%02x source=%02x\n",
                        (unsigned long long)self.board.frame(),self.board.read_byte(self.finish_entity+4),
                        (unsigned long long)self.finish_checked,i,self.finish_expected.entity[i],self.board.read_byte(self.finish_entity+i));
                    for(unsigned at:{0u,0x5cu,0xc8u,0xd4u,0x274u,0x278u}) {
                        uint32_t before,native;memcpy(&before,self.motion_expected.entity.data()+at,4);
                        memcpy(&native,self.finish_expected.entity.data()+at,4);
                        std::fprintf(stderr,"opponent finish word%03x before=%08x native=%08x source=%08x\n",
                            at,before,native,self.board.read_dword(self.finish_entity+at));
                    }
                    throw std::runtime_error("opponent finish differs from originalcde0..cebc");
                }
            if(self.finish_owner!=self.board.read_dword(0x5014e4) || self.finish_timer!=self.board.read_word(0x501894) ||
               self.finish_event!=self.board.read_byte(0x501898) || self.finish_expected.sequence_counter!=self.board.read_byte(0x503680))
                throw std::runtime_error("opponent finish shared effects differ");
            ++self.finish_checked;self.finish_entity=0;
        }
        if(source==0x1114c && self.motion_entity) {
            if(cpu.m_r[29]!=self.motion_entity) throw std::runtime_error("opponent motion return entity mismatch");
            for(unsigned i=0;i<self.motion_expected.entity.size();++i)
                if(self.board.read_byte(self.motion_entity+i)!=self.motion_expected.entity[i]) {
                    std::fprintf(stderr,"opponent motion offset=%03x native=%02x source=%02x\n",i,
                        self.motion_expected.entity[i],self.board.read_byte(self.motion_entity+i));
                    throw std::runtime_error("opponent motion differs from originalcde0..cdec");
                }
            if(self.motion_expected.profile_updates!=self.board.read_byte(0x5014e0))
                throw std::runtime_error("opponent motion shared count differs");
            self.predict_opponent_finish(self.motion_entity);
            ++self.motion_checked;self.motion_entity=0;
        }
        if(source==0x1114c && self.guided_entity) {
            if(cpu.m_r[29]!=self.guided_entity) throw std::runtime_error("guided return entity mismatch");
            for(unsigned i=0;i<self.guided_expected.entity.size();++i)
                if(self.board.read_byte(self.guided_entity+i)!=self.guided_expected.entity[i]) {
                    std::fprintf(stderr,"opponent guided offset=%03x native=%02x source=%02x\n",i,
                        self.guided_expected.entity[i],self.board.read_byte(self.guided_entity+i));
                    throw std::runtime_error("opponent guided differs from whole original10ccc");
                }
            if(self.guided_expected.profile_updates!=self.board.read_byte(0x5014e0))
                throw std::runtime_error("opponent guided shared profile count differs");
            ++self.guided_checked;self.guided_entity=0;
        }
        if(self.lane_entity && (source==0x13740 || source==0x13760 || source==0x137cc ||
           source==0x13858 || source==0x13870 || source==0x138c8 || source==0x139d4)) {
            if(cpu.m_r[29]!=self.lane_entity) throw std::runtime_error("lane return entity mismatch");
            for(unsigned i=0;i<self.lane_expected.size();++i)
                if(self.board.read_byte(self.lane_entity+i)!=self.lane_expected[i])
                    throw std::runtime_error("native lane differs from live original entity");
            ++self.lane_checked;self.lane_entity=0;
        }
        if(source==0xd7f8 && self.opponent_entity) {
            if(cpu.m_r[29]!=self.opponent_entity) throw std::runtime_error("opponent rolling return entity mismatch");
            for(unsigned i=0;i<self.opponent_expected.entity.size();++i)
                if(self.board.read_byte(self.opponent_entity+i)!=self.opponent_expected.entity[i]) {
                    std::fprintf(stderr,"opponent rolling offset=%03x native=%02x reference=%02x\n",i,
                        self.opponent_expected.entity[i],self.board.read_byte(self.opponent_entity+i));
                    throw std::runtime_error("opponent rolling differs from whole original update");
                }
            ++self.opponent_checked;self.opponent_entity=0;
        }
        if(source==0x1136c && self.marked_entity) {
            if(cpu.m_r[29]!=self.marked_entity) throw std::runtime_error("marked resistance return entity mismatch");
            for(unsigned i=0;i<self.marked_expected.size();++i)
                if(self.board.read_byte(self.marked_entity+i)!=self.marked_expected[i])
                    throw std::runtime_error("marked resistance differs from live original entity");
            ++self.marked_checked;
            self.marked_ids|=UINT64_C(1)<<self.board.read_byte(self.marked_entity+4);
            self.marked_entity=0;
        }
        if (source == 0xe3f0 && self.grid_entity) {
            if (cpu.m_r[29] != self.grid_entity) throw std::runtime_error("grid reset return entity mismatch");
            for (unsigned i=0;i<self.grid_expected.entity.size();++i)
                if (self.board.read_byte(self.grid_entity+i) != self.grid_expected.entity[i]) {
                    std::fprintf(stderr,"grid reset mismatch offset=%03x native=%02x reference=%02x\n",i,
                        self.grid_expected.entity[i],self.board.read_byte(self.grid_entity+i));
                    throw std::runtime_error("native grid ground differs from original reset tail");
                }
            if (self.grid_reset_pending) {
                const auto &road=self.rolling_chain.road;
                const unsigned id=self.board.read_byte(self.grid_entity+4);
                if(road.grid_guided_scales && (id>=road.grid_count ||
                   nm_force_bits(road.grid_guided_scales[id])!=self.board.read_dword(self.grid_entity+0x238)))
                    throw std::runtime_error("offline guided scale differs from original TGP reset division");
                auto reset=self.grid_expected;
                reset.fields(self.grid_reset_state,true);
                memcpy(reset.entity.data()+0x288,&self.grid_reset_height,4);
                if (reset.entity!=self.grid_expected.entity) {
                    for(unsigned i=0;i<reset.entity.size();++i) if(reset.entity[i]!=self.grid_expected.entity[i])
                        std::fprintf(stderr,"combined reset offset=%03x native=%02x reference=%02x\n",i,reset.entity[i],self.grid_expected.entity[i]);
                    throw std::runtime_error("combined native base/grid/pose reset differs from original");
                }
                if ((uint16_t)self.grid_reset_state.tilt[0]!=self.board.read_word(0x501844))
                    throw std::runtime_error("combined reset shared tilt differs");
                const auto &native=self.native_grid_cars[id];
                const uint32_t remembered=self.board.read_dword(self.grid_entity+0x244);
                if(nm_force_bits(native.grid_speed)!=self.board.read_dword(self.grid_entity+0x38) ||
                   nm_force_bits(native.guided_scale)!=self.board.read_dword(self.grid_entity+0x238) ||
                   native.mode!=self.board.read_byte(self.grid_entity+0x236) ||
                   native.effects.contact_age!=self.board.read_byte(self.grid_entity+0x235) ||
                   native.remembered!=(remembered ? self.board.read_byte(remembered+4) : 255) ||
                   native.other_yaw!=std::bit_cast<int16_t>(self.board.read_word(self.grid_entity+0x212)) ||
                   native.effects.correction_yaw!=std::bit_cast<int16_t>(self.board.read_word(self.grid_entity+0x216)) ||
                   native.effects.height.sampled_tick!=self.board.read_word(self.grid_entity+0x27e) ||
                   nm_force_bits(native.effects.height.sampled_height)!=self.board.read_dword(self.grid_entity+0x280) ||
                   nm_force_bits(native.effects.height.sampled_velocity)!=self.board.read_dword(self.grid_entity+0x284) ||
                   nm_force_bits(native.effects.height.velocity)!=self.board.read_dword(self.grid_entity+0xa0) ||
                   native.next!=id || native.previous!=id ||
                   self.board.read_dword(self.grid_entity+0x88)!=self.grid_entity ||
                   self.board.read_dword(self.grid_entity+0x8c)!=self.grid_entity)
                    throw std::runtime_error("native field grid metadata differs from original");
                ++self.native_grid_resets;self.native_grid_ids|=UINT64_C(1)<<id;
                self.grid_reset_pending=false;
                std::puts("native_grid_reset_live PASS (persistent from before base reset; mapped entity+reference height+tilt, metadata excluded)");
            }
            const unsigned id=self.board.read_byte(self.grid_entity+4);
            if(id>=44) throw std::runtime_error("grid entity ID outside native field bound");
            self.grid_ids|=UINT64_C(1)<<id;
            self.grid_entity=0;
            std::printf("native_grid_pose_live PASS checked=%u id=%u ids=%llx (authored pose through reset road selection, full entity)\n",
                ++self.grid_checked,id,(unsigned long long)self.grid_ids);
        }
        if (self.rolling_chain.road.storage && !self.rolling_chain.entered && source == 0xe3f0 &&
            cpu.m_r[29] == self.board.read_dword(0x501220))
            std::printf("native_player_reset_exit frame=%llu profile=%u previous=%u path=%u x=%08x z=%08x\n",
                (unsigned long long)self.board.frame(),self.board.read_word(cpu.m_r[29]+0x128),
                self.board.read_word(cpu.m_r[29]+0x12a),self.board.read_word(cpu.m_r[29]+0x134),
                self.board.read_dword(cpu.m_r[29]+0x1c),self.board.read_dword(cpu.m_r[29]+0x24));
        self.collisions.observe_return(self.board,cpu,source,self.params.get());
        if (self.rolling_chain.pending && source == 0xe0a4)
            self.rolling_chain.finish(self.board,cpu.m_r[29]);
        if (self.contact.storage && self.player_chain.road.storage && source == 0xe938 && cpu.m_IP == 0xd834) {
            self.player_chain.begin(self.board,cpu.m_r[29],self.params.get(),&self.contact);
            self.begin_movement();
        }
        if (self.contact.storage && self.player_chain.road.storage &&
            self.player_chain.pending && source == 0xe0a4)
            self.player_chain.finish(self.board,cpu.m_r[29]);
    }
    static void observe(void *context, const rt::Cpu &cpu, uint32_t target, int type) {
        if (type) return;
        auto &self = *static_cast<LiveDriveCheck *>(context);
        // Unlike field_turns, retain camera/scenery calls after the event tail.
        // Close at the next object pass, never at19038 before scenery runs.
        if(target==0x1838 && !self.scene_turns.empty()) {
            ++self.scene_orders[self.scene_turns];self.scene_turns.clear();
        }
        const uint32_t scene_flags=self.board.read_dword(0x501090);
        if(!(scene_flags&0x10000000) && (scene_flags&0x500000)) {
            // Camera6484 is initialization; its installed updater changes.
            // Identify the dispatched owner rather than guessing that function.
            if(cpu.m_IP==0x1864 && cpu.m_r[29]==self.board.read_dword(0x501240)) self.scene_turns.push_back(258);
            if(cpu.m_IP==0x1864 && cpu.m_r[29]==self.board.read_dword(0x501284)) self.scene_turns.push_back(259);
            if(target==0x16e58) self.scene_turns.push_back(260);
        }
        if(target==0x1224c) self.finish_movement();
        if(cpu.m_IP==0x1864 || target==0x1224c || target==0x19038) {
            const uint32_t flags=self.board.read_dword(0x501090);
            if(!(flags&0x10000000) && (flags&0x500000)) {
                if(cpu.m_IP==0x1864) {
                    const uint32_t base=self.board.read_dword(0x501220),count=self.board.read_dword(0x501080);
                    if(base>=0x500000 && base<=0x5ffd00 && count && count<=44) {
                        const uint32_t stride=self.board.read_dword(base+8),at=cpu.m_r[29];
                        if(stride>=0x300 && at>=base && uint64_t(at)<uint64_t(base)+uint64_t(count)*stride &&
                           (at-base)%stride==0) {
                            const unsigned id=self.board.read_byte(at+4);
                            if(id!=(at-base)/stride || target!=self.board.read_dword(at+12))
                                throw std::runtime_error("normal field dispatch identity mismatch");
                            self.field_turns.push_back(id);++self.field_modes[target];
                            self.scene_turns.push_back(id);
                        }
                    }
                } else {
                    self.field_turns.push_back(target==0x1224c ? 256 : 257);
                    self.scene_turns.push_back(target==0x1224c ? 256 : 257);
                    if(target==0x19038) self.finish_field_order();
                }
            }
        }
        if(target==0x136e8 && self.rolling_chain.road.storage) {
            if(self.lane_entity) throw std::runtime_error("nested lane check");
            const auto read=[&](uint32_t at) { return self.lane_car(at); };
            const uint32_t entity=cpu.m_r[29], front_ptr=self.board.read_dword(entity+0x88), saved_ptr=self.board.read_dword(entity+0x244);
            const auto car=read(entity),player=read(cpu.m_r[25]);
            const auto front=front_ptr ? read(front_ptr) : nm_guided_lane_car_t{};
            const auto saved=saved_ptr ? read(saved_ptr) : nm_guided_lane_car_t{};
            nm_guided_lane_state_t state{std::bit_cast<int16_t>(self.board.read_word(entity+0x140)),
                std::bit_cast<int16_t>(self.board.read_word(entity+0x142)),(uint8_t)(saved_ptr ? saved.id : 255)};
            for(unsigned i=0;i<self.lane_expected.size();++i) self.lane_expected[i]=self.board.read_byte(entity+i);
            if(!nm_guided_lane_step(self.params.get(),&player,&car,front_ptr ? &front : nullptr,saved_ptr ? &saved : nullptr,
                self.board.read_word(cpu.m_r[16]+0x18),self.board.read_byte(0x5014e0),&state))
                throw std::runtime_error("native lane rejected live input");
            const uint32_t selected=state.remembered==255 ? 0 : saved_ptr && state.remembered==saved.id ? saved_ptr : front_ptr;
            memcpy(self.lane_expected.data()+0x140,&state.target,2);memcpy(self.lane_expected.data()+0x142,&state.control,2);
            memcpy(self.lane_expected.data()+0x244,&selected,4);
            self.lane_entity=entity;self.lane_ids|=UINT64_C(1)<<car.id;
        }
        if((target==0x10ccc || target==0xcde0) && self.rolling_chain.road.storage && cpu.m_r[29]!=self.board.read_dword(0x501220)) {
            const bool motion=target==0xcde0;
            auto &expected=motion ? self.motion_expected : self.guided_expected;
            if(motion ? self.motion_entity : self.guided_entity) throw std::runtime_error("nested opponent movement update");
            const uint32_t entity=cpu.m_r[29], player_ptr=self.board.read_dword(0x501220);
            const uint32_t front_ptr=self.board.read_dword(entity+0x88),saved_ptr=self.board.read_dword(entity+0x244);
            const auto car=self.lane_car(entity),player=self.lane_car(player_ptr);
            const auto front=front_ptr ? self.lane_car(front_ptr) : nm_guided_lane_car_t{};
            const auto saved=saved_ptr ? self.lane_car(saved_ptr) : nm_guided_lane_car_t{};
            const auto &road=self.rolling_chain.road;
            const unsigned profile=self.board.read_word(entity+0x128);
            if(car.id>=44 || profile>=road.profile_count) throw std::runtime_error("guided ID/profile outside native domain");
            for(unsigned i=0;i<expected.entity.size();++i) expected.entity[i]=self.board.read_byte(entity+i);
            expected.profile_updates=self.board.read_byte(0x5014e0);
            nm_player_t state{};expected.fields(state,false);
            const uint32_t desc=self.board.read_dword(0x3a48+4*road.slot),table=self.board.read_dword(desc+16);
            const nm_path_pack_t *path=nullptr;
            for(auto &candidate:self.rolling_chain.paths)
                if(self.board.read_dword(table+8*candidate.route)==self.board.read_dword(entity+0x130)) { path=&candidate;break; }
            nm_player_dynamics_input_t in{};in.speed_index=car.speed;
            in.guided_scale=nm_force_float(self.board.read_dword(entity+0x238));in.guided_mode=self.board.read_byte(0x5014f7);
            nm_guided_race_input_t race{&player,&car,front_ptr ? &front : nullptr,saved_ptr ? &saved : nullptr,
                std::bit_cast<int16_t>(self.board.read_word(player_ptr+0x60)),state.rank_key,
                std::bit_cast<int16_t>(self.board.read_word(player_ptr+0x48)),self.board.read_byte(player_ptr+0x6a),self.board.read_byte(entity+0x6a),{}};
            uint8_t remembered=saved_ptr ? saved.id : 255;
            race.heading_circle[0]=nm_force_float(self.board.tgp().diagnostic_data_word(0x220));
            race.heading_circle[1]=nm_force_float(self.board.tgp().diagnostic_data_word(0x221));
            race.heading_circle[2]=nm_force_float(self.board.tgp().diagnostic_data_word(0x223));
            if(motion) {
                nm_intro_speed_params_t speed{};
                speed.desired_divisor=nm_force_float(self.board.read_dword(0x28b57a4));
                speed.drag_offset=nm_force_float(self.board.read_dword(0x28b5814));
                speed.minimum_delta=nm_force_float(self.board.read_dword(0x28b57b8));
                for(unsigned i=0;i<512;++i) speed.response[i]=nm_force_float(self.board.read_dword(0x28b85a0+4*i));
                in.speed_flags=self.board.read_dword(0x501090);
                // Only linked bodies are consumed by one opponent turn.
                const auto capture=[&](uint32_t at) {
                    const auto lane=self.lane_car(at);
                    if(lane.id>=44) throw std::runtime_error("native field ID outside44");
                    auto &n=self.field_input[lane.id];
                    PlayerMemory memory;
                    for(unsigned b=0;b<memory.entity.size();++b) memory.entity[b]=self.board.read_byte(at+b);
                    memory.fields(n.car,false);
                    const uint32_t front_at=self.board.read_dword(at+0x88),remembered_at=self.board.read_dword(at+0x244);
                    n.next=front_at ? self.board.read_byte(front_at+4) : lane.id;
                    n.remembered=remembered_at ? self.board.read_byte(remembered_at+4) : 255;
                    n.near_marker=lane.marker;n.mode=(uint8_t)lane.mode;n.rank=self.board.read_byte(at+0x6a);
                    n.guided_scale=nm_force_float(self.board.read_dword(at+0x238));
                };
                capture(player_ptr);capture(entity);if(front_ptr) capture(front_ptr);if(saved_ptr) capture(saved_ptr);
                self.field_path=path;
                if(!path) throw std::runtime_error("native field missing authored route");
                self.field_input[car.id].route=(uint8_t)path->route;
                auto &shared=self.field_shared;shared={};
                shared.flags=in.speed_flags;shared.clock=self.board.read_dword(0x5010cc);
                shared.tick=self.board.read_word(0x5010b0);shared.guided_mode=in.guided_mode;
                shared.profile_updates=expected.profile_updates;shared.crash_counter=self.board.read_byte(0x503680);
                shared.event_timer=std::bit_cast<int16_t>(self.board.read_word(0x501894));shared.event=self.board.read_byte(0x501898);
                const uint32_t owner=self.board.read_dword(0x5014e4);
                shared.inhibit_owner=owner ? self.board.read_byte(owner+4) : 255;
                memcpy(shared.heading_circle,race.heading_circle,sizeof(shared.heading_circle));
                for(unsigned c=0;c<16;++c) shared.visible_cells[c]=self.board.read_dword(0x501520+4*c);
                self.field_marks[profile]=self.board.read_byte(0x53b71c+profile);
                self.field_mark_timer=self.board.read_dword(0x53b718);self.field_speed=speed;
                const auto before=state;const auto old_remembered=remembered;
                if(nm_opponent_motion_step(self.params.get(),&speed,nullptr,road.profiles+profile,&in,&race,0,0,&remembered,&state) ||
                   memcmp(&before,&state,sizeof(state)) || remembered!=old_remembered)
                    throw std::runtime_error("opponent motion late failure changed state");
                if(!nm_opponent_motion_step(self.params.get(),&speed,path,road.profiles+profile,&in,&race,
                    self.board.read_dword(0x53b718),std::bit_cast<int8_t>(self.board.read_byte(0x53b71c+profile)),&remembered,&state))
                    throw std::runtime_error("native opponent motion rejected actual input");
            } else if(!nm_car_guided_motion(self.params.get(),path,road.profiles+profile,&in,&race,&remembered,&state))
                throw std::runtime_error("native opponent guidance rejected actual input");
            expected.fields(state,true);
            const uint32_t selected=remembered==255 ? 0 : saved_ptr && remembered==saved.id ? saved_ptr : front_ptr;
            memcpy(expected.entity.data()+0x244,&selected,4);
            if(motion) { self.motion_entity=entity;self.motion_ids|=UINT64_C(1)<<car.id; }
            else { self.guided_entity=entity;self.guided_ids|=UINT64_C(1)<<car.id; }
        }
        if(target==0xd6f8 && self.rolling_chain.road.storage) {
            if(self.opponent_entity) throw std::runtime_error("nested opponent rolling update");
            const uint32_t entity=cpu.m_r[29];
            const auto &road=self.rolling_chain.road;
            const unsigned id=self.board.read_byte(entity+4), profile=self.board.read_word(entity+0x128);
            const unsigned cell=self.board.read_word(entity+0x2e);
            if(!id || id>=44 || profile>=road.profile_count || cell>=512)
                throw std::runtime_error("opponent rolling input outside native bounds");
            if(self.board.read_dword(0x28b575c)!=0x3cd55555u)
                throw std::runtime_error("opponent rolling speed constant differs");
            for(unsigned i=0;i<self.opponent_expected.entity.size();++i)
                self.opponent_expected.entity[i]=self.board.read_byte(entity+i);
            nm_player_t state{};self.opponent_expected.fields(state,false);
            state.checkpoints.clock=self.board.read_dword(0x5010cc);
            const uint32_t desc=self.board.read_dword(0x3a48+4*road.slot);
            const uint32_t table=self.board.read_dword(desc+16), source=self.board.read_dword(entity+0x130);
            const nm_path_pack_t *path=nullptr;
            for(auto &candidate:self.rolling_chain.paths)
                if(self.board.read_dword(table+8*candidate.route)==source) { path=&candidate;break; }
            const int visible=(self.board.read_dword(0x501520+4*(cell>>5))>>(cell&31))&1;
            const float speed=nm_force_float(self.board.read_dword(self.board.read_dword(0x501220)+0x34));
            const auto before=state;
            if(nm_opponent_rolling_step(self.params.get(),&road,nullptr,id,speed,0,0,visible,&state) ||
               memcmp(&before,&state,sizeof(state))) throw std::runtime_error("invalid rolling path mutated state");
            const int result=nm_opponent_rolling_step(self.params.get(),&road,path,id,speed,
                self.board.read_dword(0x53b718),std::bit_cast<int8_t>(self.board.read_byte(0x53b71c+profile)),visible,&state);
            if(!result) throw std::runtime_error("native opponent rolling rejected live input");
            self.opponent_expected.fields(state,true);
            if(result==2) {
                const uint32_t updater=0xcde0;
                memcpy(self.opponent_expected.entity.data()+12,&updater,4);++self.opponent_released;
            }
            self.opponent_ids|=UINT64_C(1)<<id;self.opponent_entity=entity;
        }
        if(target==0x11304 && self.rolling_chain.road.storage) {
            if(self.marked_entity) throw std::runtime_error("nested marked resistance check");
            const uint32_t entity=cpu.m_r[29];
            const unsigned id=self.board.read_byte(entity+4), profile=self.board.read_word(entity+0x128);
            if(id>=44 || profile>=self.rolling_chain.road.profile_count)
                throw std::runtime_error("marked resistance ID/profile outside native bounds");
            for(unsigned i=0;i<self.marked_expected.size();++i)
                self.marked_expected[i]=self.board.read_byte(entity+i);
            nm_resistance_state_t state{nm_force_float(self.board.read_dword(entity+0x164)),self.board.read_byte(entity+0x15f)};
            if(!nm_resistance_marked_step(self.params.get(),self.board.read_dword(0x53b718),
                std::bit_cast<int8_t>(self.board.read_byte(0x53b71c+profile)),&state))
                throw std::runtime_error("marked resistance rejected live original input");
            memcpy(self.marked_expected.data()+0x164,&state.resistance,4);
            self.marked_expected[0x15f]=state.override_count;
            self.marked_entity=entity;
        }
        if (self.contact.storage && self.rolling_chain.road.storage && target==0xe3f4 && cpu.m_IP==0xe210) {
            if(self.grid_reset_pending) throw std::runtime_error("nested combined reset");
            PlayerMemory initial;
            for(unsigned i=0;i<initial.entity.size();++i) initial.entity[i]=self.board.read_byte(cpu.m_r[29]+i);
            initial.fields(self.grid_reset_state,false);
            const auto &road=self.rolling_chain.road;
            const unsigned id=self.board.read_byte(cpu.m_r[29]+4);
            if(id>=road.grid_count) throw std::runtime_error("missing prepared grid record");
            const auto &grid=road.grid[id];
            if(road.start) {
                for(unsigned i=0;i<road.grid_count;++i) {
                    const float raw=nm_force_float(self.board.read_dword(0x286fac0+48*i+0x28));
                    if(nm_force_bits(road.grid_speeds[i])!=nm_force_bits(raw*self.params->collision_speed_step))
                        throw std::runtime_error("prepared grid speed table differs from original constants");
                    if(road.grid_guided_scales && nm_force_bits(road.grid_guided_scales[i])!=
                       nm_force_bits(nm_force_divide(self.params.get(),road.grid_speeds[i],road.start->default_speed)))
                        throw std::runtime_error("offline guided scale differs from independent native reference math");
                }
                for(unsigned i=0;i<4;++i)
                    if(road.start->body_model[i]!=self.board.read_dword(0x286fab0+4*i))
                        throw std::runtime_error("prepared model table differs from original");
                if(nm_force_bits(road.start->default_speed)!=self.board.read_dword(0x28b57a4) ||
                   nm_force_bits(road.start->rolling_speed)!=self.board.read_dword(0x28b574c))
                    throw std::runtime_error("prepared startup speeds differ from original");
                std::printf("native_prepared_start PASS grid_speeds=%u guided_scales=%u models=4 startup_speeds=2\n",
                    road.grid_count,road.grid_guided_scales ? road.grid_count : 0);
            }
            const nm_path_pack_t *path=nullptr;
            for(auto &candidate:self.rolling_chain.paths) if(candidate.route==grid.route) { path=&candidate;break; }
            // Host reference preparation only; production must export this constant offline.
            const float reference_speed=nm_force_float(grid.speed_parameter)*self.params->collision_speed_step;
            const unsigned variant=self.board.read_byte(0x501464);
            if(variant>=4) throw std::runtime_error("invalid grid model selector");
            const uint32_t reference_model=self.board.read_dword(0x286fab0+4*variant);
            const float speed=road.grid_speeds ? road.grid_speeds[id] : reference_speed;
            const uint32_t model=road.start ? road.start->body_model[variant] : reference_model;
            if(nm_force_bits(speed)!=nm_force_bits(reference_speed) || model!=reference_model)
                throw std::runtime_error("prepared grid speed/model differs from ROM source");
            if(!road.slot && road.grid_guided_scales && !id) {
                self.rolling_start_state=self.grid_reset_state;
                uint32_t flags=self.board.read_dword(0x501854);
                if(!nm_player_rolling_start(self.params.get(),&self.contact,&road,path,variant,
                    &self.rolling_start_state,&self.rolling_start_height,&flags))
                    throw std::runtime_error("prepared rolling start rejected actual initial state");
                self.rolling_start_ready=true;
            }
            auto &native=self.native_grid_cars[id];native.car=self.grid_reset_state;
            const uint32_t remembered=self.board.read_dword(cpu.m_r[29]+0x244);
            native.remembered=remembered ? self.board.read_byte(remembered+4) : 255;
            native.effects.height.sampled_tick=self.board.read_word(cpu.m_r[29]+0x27e);
            native.effects.height.sampled_height=nm_force_float(self.board.read_dword(cpu.m_r[29]+0x280));
            native.effects.height.sampled_velocity=nm_force_float(self.board.read_dword(cpu.m_r[29]+0x284));
            native.effects.height.velocity=nm_force_float(self.board.read_dword(cpu.m_r[29]+0xa0));
            const auto before=native;
            if(nm_race_car_grid_reset(&native,self.params.get(),&self.contact,&road,nullptr,id,variant) ||
               memcmp(&before,&native,sizeof(native))) throw std::runtime_error("native grid late failure changed car");
            if(!nm_race_car_grid_reset(&native,self.params.get(),&self.contact,&road,path,id,variant))
                throw std::runtime_error("combined native reset rejected prepared grid");
            self.grid_reset_state=native.car;self.grid_reset_height=native.effects.height.reference_height;
            if(native.effects.height.sampled_tick!=before.effects.height.sampled_tick ||
               nm_force_bits(native.effects.height.sampled_height)!=nm_force_bits(before.effects.height.sampled_height) ||
               nm_force_bits(native.effects.height.sampled_velocity)!=nm_force_bits(before.effects.height.sampled_velocity) ||
               nm_force_bits(native.effects.height.velocity)!=0 ||
               native.remembered!=before.remembered) throw std::runtime_error("grid reset lost preserved field history");
            self.grid_reset_pending=true;
        }
        if (self.contact.storage && self.rolling_chain.road.storage && target == 0x13b28 && cpu.m_IP == 0xe308) {
            if (self.grid_entity) throw std::runtime_error("nested grid reset check");
            self.grid_entity=cpu.m_r[29];
            for (unsigned i=0;i<self.grid_expected.entity.size();++i)
                self.grid_expected.entity[i]=self.board.read_byte(self.grid_entity+i);
            nm_player_t state{};self.grid_expected.fields(state,false);
            const auto &road=self.rolling_chain.road;
            const uint32_t desc=self.board.read_dword(0x3a48+4*road.slot);
            const uint32_t table=self.board.read_dword(desc+16), source=self.board.read_dword(self.grid_entity+0x130);
            const nm_path_pack_t *path=nullptr;
            for (auto &candidate:self.rolling_chain.paths)
                if(self.board.read_dword(table+8*candidate.route)==source) { path=&candidate;break; }
            float reference_height;
            if (!nm_player_grid_pose(self.params.get(),&self.contact,&road,path,&state,&reference_height))
                throw std::runtime_error("native grid pose rejected actual reset");
            self.grid_expected.fields(state,true);
            memcpy(self.grid_expected.entity.data()+0x288,&reference_height,4);
        }
        if (self.rolling_chain.road.storage && !self.rolling_chain.entered &&
            cpu.m_r[29] == self.board.read_dword(0x501220) &&
            ((target == 0x13b28 && (cpu.m_IP == 0xe308 || cpu.m_IP == 0xd2e0)) ||
             (target == 0x10980 && cpu.m_IP == 0xe3f0)))
            std::printf("native_player_reset_stage frame=%llu call=%05x return=%05x profile=%u previous=%u path=%u lane=%u x=%08x z=%08x\n",
                (unsigned long long)self.board.frame(),target,cpu.m_IP,self.board.read_word(cpu.m_r[29]+0x128),
                self.board.read_word(cpu.m_r[29]+0x12a),self.board.read_word(cpu.m_r[29]+0x134),
                self.board.read_word(cpu.m_r[29]+0x13e),self.board.read_dword(cpu.m_r[29]+0x1c),self.board.read_dword(cpu.m_r[29]+0x24));
        if (self.rolling_start_ready && !self.rolling_chain.entered && target==0xd3f4 && cpu.m_IP==0xd354) {
            PlayerMemory actual;
            for(unsigned i=0;i<actual.entity.size();++i) actual.entity[i]=self.board.read_byte(cpu.m_r[29]+i);
            auto predicted=actual;predicted.fields(self.rolling_start_state,true);
            memcpy(predicted.entity.data()+0x288,&self.rolling_start_height,4);
            if(predicted.entity!=actual.entity) {
                for(unsigned i=0;i<actual.entity.size();++i) if(predicted.entity[i]!=actual.entity[i])
                    std::fprintf(stderr,"rolling start offset=%03x native=%02x reference=%02x\n",i,predicted.entity[i],actual.entity[i]);
                throw std::runtime_error("prepared rolling start differs at first physics entry");
            }
            std::puts("native_rolling_start_live PASS (mapped entity and reference height, continuous from pre-reset; shared state/UI excluded)");
            self.rolling_start_ready=false;
        }
        if (self.contact.storage && self.rolling_chain.road.storage && target == 0xd3f4 && cpu.m_IP == 0xd354)
            self.rolling_chain.begin(self.board,cpu.m_r[29],self.params.get(),&self.contact,true);
        if (target == 0x9014 && self.rolling_chain.checked && !self.rolling_chain.pending &&
            cpu.m_r[29] == self.rolling_chain.entity)
            ++self.rolling_chain.pending_effect_calls;
        if (self.rolling_chain.pending && target == 0xd438 && cpu.m_IP == 0xd3e8)
            self.rolling_chain.finish(self.board,cpu.m_r[29]); // BEFORE UI changes shared shake/scroll memory.
        if (self.player_chain.pending && self.player_chain.road.storage && target == 0x19bdc &&
            (cpu.m_IP == 0x13548 || cpu.m_IP == 0x13618 || cpu.m_IP == 0x1363c || cpu.m_IP == 0x13648)) {
            auto &chain = self.player_chain;
            if (chain.award_cues_seen >= chain.award.cue_count || cpu.m_r[16] != chain.award.cues[chain.award_cues_seen++])
                throw std::runtime_error("live checkpoint announcement order differs");
        }
        self.collisions.observe(self.board,cpu,target,self.params.get(),&self.player_chain.road);
        if (self.contact.storage) self.player_chain.observe_external_pose(self.board,cpu,target);
        if (self.capture_road && target == 0x10980 && cpu.m_IP == 0xd8f4) {
            // ponytail: bounded host evidence; stream checks if diagnostics need
            // more than65536 entries. Never ship these snapshots or truncate.
            if (self.road_pending || self.road_selections.size() >= 65536 || cpu.m_r[29] != self.entity)
                throw std::runtime_error("invalid road selection capture");
            RoadSelectionCase input{};
            for (unsigned i = 0; i < input.before.size(); ++i) input.before[i] = self.board.read_byte(self.entity+i);
            input.mapping = self.board.read_dword(0x501430); input.flags = self.board.read_dword(0x501450);
            self.road_selections.push_back(input); self.road_pending = true;
        }
        if (self.capture_road && target == 0x16060 && cpu.m_IP == 0xd8f8) {
            if (!self.road_pending || cpu.m_r[29] != self.entity) throw std::runtime_error("road return without matching entry");
            auto &result = self.road_selections.back().after;
            for (unsigned i = 0; i < result.size(); ++i) result[i] = self.board.read_byte(self.entity+i);
            self.road_pending = false;
        }
        if (self.contact.storage) self.player_chain.observe_pose(self.board,cpu);
        if (self.contact.storage && target == 0x15fc4 && cpu.m_IP == 0xd88c) {
            const uint32_t at = cpu.m_r[29], auxiliary = self.board.read_dword(0x501270);
            if (at < 0x500000 || at > 0x5ffd00 || at%16 || self.board.read_byte(at+4) != 0 ||
                auxiliary < 0x500000 || auxiliary > 0x5fffe0 || auxiliary%16 || self.damage_pre.size() >= 65536)
                throw std::runtime_error("invalid live player lifecycle capture");
            DamagePreCase input;
            for (unsigned i = 0; i < input.entity.size(); ++i) input.entity[i] = self.board.read_byte(at+i);
            for (unsigned i = 0; i < input.globals.size(); ++i) input.globals[i] = self.board.read_byte(0x501000+i);
            for (unsigned i = 0; i < input.auxiliary.size(); ++i) input.auxiliary[i] = self.board.read_byte(auxiliary+i);
            for (unsigned i = 0; i < input.camera.size(); ++i) input.camera[i] = self.board.read_byte(0x501710+i);
            input.last_surface = self.board.read_dword(0x5018ac); input.control = self.board.read_dword(0x501124);
            input.camera_mode = std::bit_cast<int32_t>(self.board.read_dword(0x5014a4));
            input.pivot_signal = self.board.read_word(0x501a02); input.spin_signal = self.board.read_word(0x501a04);
            input.shake = self.board.read_byte(0x501830); input.sequence_counter = self.board.read_byte(0x503680);
            input.variant = self.board.read_byte(0x503690);
            const int view = std::bit_cast<int8_t>(input.camera[0]);
            // Selected descriptor byte in backup RAM, never a device read.
            const int offset = 0x233+20*view;
            if (offset < 0 || offset >= 0x4000) throw std::runtime_error("camera descriptor outside backup RAM");
            input.restore_mode = self.board.read_byte(0x1d00000+offset);
            self.damage_pre.push_back(input);
        }
        if (self.contact.storage && target == 0x16060 && cpu.m_IP == 0xd8f8 &&
            cpu.m_r[29] == self.player_chain.entity) {
            // Same bounded host-only capture as the pivot inputs, not a replay asset.
            if (self.boundary_pending || self.boundary_selections.size() >= 65536) throw std::runtime_error("boundary selection capture guard");
            self.boundary_selections.push_back({capture_boundary(self.board,cpu.m_r[29]),{},false});
            self.boundary_pending = true;
        }
        if (self.contact.storage && target == 0x16da4 && cpu.m_IP == 0xd8fc && cpu.m_r[29] == self.player_chain.entity) {
            if (!self.boundary_pending) throw std::runtime_error("boundary return without entry");
            auto &input = self.boundary_selections.back();
            input.after = capture_boundary(self.board,cpu.m_r[29]); input.complete = true;
            self.boundary_pending = false;
        }
        if (self.contact.storage && target == 0x167b4 && cpu.m_r[29] == self.player_chain.entity &&
            (cpu.m_IP == 0x16538 || cpu.m_IP == 0x16558 || cpu.m_IP == 0x16568 || cpu.m_IP == 0x16578)) {
            // ponytail: bounded host capture; stream into a separate oracle if
            // very long diagnostics exceed one million calls. Never truncate.
            if (self.boundary_inputs.size() >= 1000000) throw std::runtime_error("boundary capture guard");
            std::array<float,10> input;
            for (unsigned i = 0; i < input.size(); ++i) input[i] = std::bit_cast<float>(cpu.m_r[16+i]);
            self.boundary_inputs.push_back(input);
            if (cpu.m_IP == 0x16538) {
                if (self.boundary_pivots.size() >= 65536) throw std::runtime_error("pivot capture guard");
                BoundaryPivotCase pivot;
                for (unsigned i = 0; i < pivot.entity.size(); ++i) pivot.entity[i] = self.board.read_byte(cpu.m_r[29]+i);
                pivot.signal = std::bit_cast<int16_t>(self.board.read_word(0x501a02));
                pivot.shake = self.board.read_byte(0x501830);
                self.boundary_pivots.push_back(pivot);
            }
        }
        const bool traction_in = target == 0x10568 && cpu.m_IP == 0xd8d4;
        const bool yaw_in = target == 0xf9d0 && cpu.m_IP == 0xd8dc;
        const bool traction_out = target == 0xf31c && cpu.m_IP == 0xd8d8;
        const bool yaw_out = target == 0xfdf8 && cpu.m_IP == 0xd8e0;
        const bool tires_out = target == 0x10338 && cpu.m_IP == 0xd8e4;
        const bool auto_in = target == 0x11d60 && cpu.m_IP == 0xd8b4;
        const bool manual_in = target == 0x11e80 && cpu.m_IP == 0xd8ac;
        const bool gears_out = (target == 0x1132c && cpu.m_IP == 0xd8c4) ||
                               (target == 0xec44 && cpu.m_IP == 0xd9ec);
        const bool resistance_in = target == 0x1132c && cpu.m_IP == 0xd8c4;
        const bool proximity_in = target == 0x11370 && cpu.m_IP == 0xd8c8;
        const bool proximity_out = target == 0xecc8 && cpu.m_IP == 0xd8cc;
        const bool motion_in = target == 0xea04 && cpu.m_IP == 0xd8e8;
        const bool held_in = target == 0xec44 && cpu.m_IP == 0xd9ec;
        const bool motion_out = target == 0x116d0 && cpu.m_IP == 0xd8ec;
        const bool held_out = target == 0x116d0 && cpu.m_IP == 0xd9f0;
        const bool ground_out = target == 0x16e10 && (cpu.m_IP == 0xd8f0 || cpu.m_IP == 0xd9f4);
        if (gears_out) {
            if (self.pending != 4 && self.pending != 5) throw std::runtime_error("live gear return without entry");
            self.finish(cpu.m_r[29],self.pending);
        }
        if (traction_out || yaw_out) self.finish(cpu.m_r[29],traction_out ? 1 : 2);
        if (tires_out) self.finish(cpu.m_r[29],3);
        if (traction_in || yaw_in) self.begin(cpu,traction_in ? 1 : 2);
        if (yaw_out) self.begin(cpu,3);
        if (auto_in || manual_in) self.begin(cpu,auto_in ? 4 : 5);
        if (proximity_in) self.finish(cpu.m_r[29],6);
        if (proximity_out) self.finish(cpu.m_r[29],7);
        if (resistance_in || proximity_in) self.begin(cpu,resistance_in ? 6 : 7);
        if (motion_out || held_out) self.finish(cpu.m_r[29],motion_out ? 8 : 9);
        if (motion_in || held_in) self.begin(cpu,motion_in ? 8 : 9);
        if (self.contact.storage) {
            if (!self.player_chain.road.storage && proximity_out)
                self.player_chain.begin(self.board,cpu.m_r[29],self.params.get(),&self.contact);
            if (!self.player_chain.road.storage && target == 0x10980 && cpu.m_IP == 0xd8f4)
                self.player_chain.finish(self.board,cpu.m_r[29]);
            if (ground_out) self.finish(cpu.m_r[29],10);
            if (motion_out || held_out) self.begin(cpu,10);
        }
    }
    void finish(uint32_t at, unsigned operation) {
        if (pending != operation || at != entity) throw std::runtime_error("live native drive call pairing mismatch");
        if (!accepted) {
            if (rejected[operation-1] == 1) std::printf("live_drive first_rejected_return frame=%llu operation=%u yaw=%04x heading=%04x body=%04x delta=%04x offset=%04x inertia=%08x/%08x drive=%08x\n",
                (unsigned long long)board.frame(),operation,board.read_word(entity+0x58),board.read_word(entity+0x48),
                board.read_word(entity+0x2a),board.read_word(entity+0x24a),board.read_word(0x5018a4),
                board.read_dword(entity+0x16c),board.read_dword(entity+0x170),board.read_dword(entity+0x168));
            pending = 0; return;
        }
        for (unsigned i = 0; i < expected.size(); ++i) if (board.read_byte(entity+i) != expected[i]) {
            std::fprintf(stderr,"live_drive frame=%llu operation=%u entity=%08x offset=%03x native=%02x reference=%02x\n",
                (unsigned long long)board.frame(),operation,entity,i,expected[i],board.read_byte(entity+i));
            throw std::runtime_error("live native/reference entity mismatch");
        }
        for (unsigned i = 0; i < expected_yaw.size(); ++i)
            if (board.read_byte(0x5018a4+i) != expected_yaw[i]) throw std::runtime_error("live native/reference yaw globals mismatch");
        if (operation == 8 || operation == 9) for (unsigned i = 0; i < expected_ahead.size(); ++i)
            if (board.read_byte(0x501864+i) != expected_ahead[i]) throw std::runtime_error("live native/reference motion lookahead mismatch");
        if (operation == 10) for (unsigned i = 0; i < expected_heights.size(); ++i)
            if (board.read_byte(0x501100+i) != expected_heights[i]) throw std::runtime_error("live native/reference ground heights mismatch");
        if (operation == 3) for (unsigned a = 0; a < 2; ++a) for (unsigned i = 0; i < 16; ++i)
            if (board.read_byte(auxiliary_address[a]+i) != expected_auxiliary[a*16+i])
                throw std::runtime_error("live native/reference tire auxiliary event mismatch");
        if ((operation == 4 || operation == 5) && board.read_dword(0x5010c4) != uint32_t(gear_hold_expected))
            throw std::runtime_error("live gear caller hold mismatch");
        ++checked[operation-1]; pending = 0;
    }
    void begin(const rt::Cpu &cpu, unsigned operation) {
        if (pending) throw std::runtime_error("nested live native drive entry");
        entity = cpu.m_r[29];
        if (entity < 0x500000 || entity > 0x5ffd00 || entity%16 || cpu.m_AC>>30)
            throw std::runtime_error("invalid live player pointer or rounding mode");
        for (unsigned i = 0; i < expected.size(); ++i) expected[i] = board.read_byte(entity+i);
        for (unsigned i = 0; i < expected_yaw.size(); ++i) expected_yaw[i] = board.read_byte(0x5018a4+i);
        const auto u16 = [&](unsigned offset) { return board.read_word(entity+offset); };
        const auto s16 = [&](unsigned offset) { return std::bit_cast<int16_t>(u16(offset)); };
        const auto u32 = [&](unsigned offset) { return board.read_dword(entity+offset); };
        const auto f32 = [&](unsigned offset) { return std::bit_cast<float>(u32(offset)); };
        const auto put = [&](unsigned offset, auto value) { std::memcpy(expected.data()+offset,&value,sizeof(value)); };
        if (operation == 1) {
            nm_traction_input_t v{}; nm_traction_state_t s{};
            v.gear = u32(0x148); v.course_slot = board.read_byte(0x501460);
            v.steer_angle = s16(0x124); v.tire_angle = s16(0x18a); v.steer = s16(0xb0);
            v.phase[0] = s16(0x128); v.phase[1] = s16(0x12a);
            v.speed_index = s16(0x18); v.pitch = s16(0x28); v.surface = u16(0x1b8);
            v.accel = u16(0xac); v.brake = u16(0xa8); v.override_count = board.read_byte(entity+0x15f);
            v.lateral = f32(0x1cc); v.inertial[0] = f32(0x16c); v.inertial[1] = f32(0x170);
            for (unsigned a = 0; a < 4; ++a) v.contact[a] = u16(0x1ba+2*a);
            s.previous_lateral = f32(0x270); s.drive = f32(0x168); s.rear_load = f32(0x198); s.slope = f32(0x29c);
            s.condition_26c = s16(0x26c);
            for (unsigned a = 0; a < 6; ++a) s.countdown[a] = std::bit_cast<int8_t>(board.read_byte(entity+0x264+a));
            accepted = nm_traction_step(params.get(),&v,&s);
            if (!accepted && !rejected[0]) std::fprintf(stderr,"live_traction rejected drive=%08x rear_load=%08x rear_inertial=%08x timer=%d\n",
                u32(0x168),u32(0x198),u32(0x170),s.countdown[0]);
            put(0x270,s.previous_lateral); put(0x168,s.drive); put(0x198,s.rear_load); put(0x29c,s.slope);
            put(0x26c,s.condition_26c);
            std::memcpy(expected.data()+0x264,s.countdown,sizeof(s.countdown));
        } else if (operation == 2) {
            nm_yaw_input_t v{}; nm_yaw_state_t s{};
            v.speed = f32(0x34); v.steer_angle = s16(0x124); v.steer = s16(0xb0);
            v.speed_index = s16(0x18); v.road_heading = s16(0x1c2);
            v.course_slot = board.read_byte(0x501460); v.fixed_offset = expected_yaw[4];
            for (unsigned a = 0; a < 2; ++a) { v.tire_angle[a] = s16(0x188+2*a); s.inertial[a] = f32(0x16c+4*a); }
            s.yaw = s16(0x58); s.heading = s16(0x48); s.body_yaw = s16(0x2a); s.yaw_delta = s16(0x24a);
            s.alignment_frames = u32(0x1f0); s.offset = std::bit_cast<int16_t>(board.read_word(0x5018a4));
            zero_speed += v.speed == 0;
            straight += int(v.steer_angle)+v.tire_angle[0] == v.tire_angle[1];
            accepted = nm_yaw_step(params.get(),&v,&s);
            if (!accepted) rejected_turn += int(v.steer_angle)+v.tire_angle[0] != v.tire_angle[1];
            if (!accepted && !rejected[1]) std::fprintf(stderr,"live_yaw rejected speed=%08x steer=%d tires=%d/%d yaw=%d heading=%d offset=%d fixed=%u inertia=%08x/%08x\n",
                u32(0x34),v.steer_angle,v.tire_angle[0],v.tire_angle[1],s.yaw,s.heading,s.offset,v.fixed_offset,
                u32(0x16c),u32(0x170));
            put(0x58,s.yaw); put(0x48,s.heading); put(0x2a,s.body_yaw); put(0x24a,s.yaw_delta);
            put(0x1f0,s.alignment_frames); put(0x16c,s.inertial[0]); put(0x170,s.inertial[1]);
            std::memcpy(expected_yaw.data(),&s.offset,2);
        } else if (operation == 3) {
            nm_tires_input_t v{}; nm_tires_state_t s{};
            v.drive = f32(0x168); v.speed = f32(0x34); v.front_grip = f32(0x17c);
            v.yaw_delta = s16(0x24a); v.speed_index = s16(0x18); v.steer_angle = s16(0x124);
            for (unsigned a = 0; a < 2; ++a) { v.load[a] = f32(0x194+4*a); s.angle[a] = s16(0x188+2*a); }
            s.rear_grip = f32(0x180); s.flags = u32(0); s.filter = s16(0x248); s.condition = s16(0x26c);
            s.trigger_count = std::bit_cast<int8_t>(board.read_byte(entity+0x96));
            s.override_count = board.read_byte(entity+0x15f);
            accepted = nm_tires_step(params.get(),&v,&s);
            put(0x180,s.rear_grip); put(0,s.flags); put(0x248,s.filter); put(0x26c,s.condition);
            put(0x188,s.angle[0]); put(0x18a,s.angle[1]); put(0x96,s.trigger_count); put(0x15f,s.override_count);
            for (unsigned a = 0; a < 2; ++a) {
                const uint32_t at = board.read_dword(0x501274+4*a);
                if (at < 0x500000 || at > 0x5ffff0 || at%16)
                    throw std::runtime_error("invalid live tire auxiliary pointer");
                auxiliary_address[a] = at;
                for (unsigned i = 0; i < 16; ++i) expected_auxiliary[16*a+i] = board.read_byte(at+i);
                if (s.trigger_pair) {
                    expected_auxiliary[16*a+3] |= 0x80;
                    const uint32_t function = a ? 0x998c : 0x9970;
                    std::memcpy(expected_auxiliary.data()+16*a+12,&function,4);
                }
            }
        } else if (operation == 4 || operation == 5) {
            nm_gears_input_t v{}; nm_gears_state_t s{};
            v.manual = operation == 5; v.selector = (board.read_dword(0x53e120)>>20)&7;
            v.speed_index = std::bit_cast<int32_t>(u32(0x18)); v.rpm_bias = std::bit_cast<int32_t>(u32(0x24c));
            s.gear = u32(0x148); s.brake = u32(0xa8); s.rpm = std::bit_cast<int32_t>(u32(0x150));
            s.filtered_rpm = std::bit_cast<int32_t>(u32(0x154)); s.downshift_rpm = std::bit_cast<int32_t>(u32(0x158));
            s.shift_ticks = std::bit_cast<int8_t>(board.read_byte(entity+0x15c));
            const unsigned before_gear = s.gear;
            accepted = nm_gears_step(params.get(),&v,&s);
            gear_up[v.manual] += s.gear > before_gear; gear_down[v.manual] += s.gear < before_gear;
            put(0x148,s.gear); put(0xa8,s.brake); put(0x150,s.rpm); put(0x154,s.filtered_rpm);
            put(0x158,s.downshift_rpm); put(0x15c,s.shift_ticks);
            // The next call follows either the normal branch or d9c0's
            // hold decrement. Include that caller effect, never omit bytes.
            gear_hold_expected = std::bit_cast<int32_t>(board.read_dword(0x5010c4));
            if (gear_hold_expected > 0) {
                ++gear_hold_calls;
                if (!--gear_hold_expected) put(0x18a,int16_t(0));
            }
        } else if (operation == 6) {
            nm_resistance_state_t s{f32(0x164),board.read_byte(entity+0x15f)};
            accepted = nm_resistance_step(params.get(),&s);
            put(0x164,s.resistance); put(0x15f,s.override_count);
        } else if (operation == 7) {
            nm_proximity_input_t v{};
            v.disabled = board.read_dword(0x501a80) != 0;
            v.heading = s16(0x48); v.position[0] = f32(0x1c); v.position[1] = f32(0x24);
            if (!v.disabled) {
                uint32_t at = u32(0x88);
                for (unsigned n = 0; n < 3; ++n) {
                    if (at < 0x500000 || at > 0x5ffd00 || at%16)
                        throw std::runtime_error("invalid live neighbor entity pointer");
                    v.neighbor[n][0] = std::bit_cast<float>(board.read_dword(at+0x1c));
                    v.neighbor[n][1] = std::bit_cast<float>(board.read_dword(at+0x24));
                    at = board.read_dword(at+0x88);
                }
            }
            int8_t count = std::bit_cast<int8_t>(board.read_byte(entity+0x15e));
            const int before = count;
            accepted = nm_proximity_step(params.get(),&v,&count);
            proximity_disabled_calls += v.disabled != 0;
            proximity_refreshes += !v.disabled && count == 64 && before != 65;
            put(0x15e,count);
        } else if (operation == 10) {
            if (board.read_byte(0x501460) != contact.slot)
                throw std::runtime_error("live ground course does not match prepared contact pack");
            nm_wheels_state_t s{};
            s.yaw = s16(0x58); s.roll = s16(0x2c); s.pitch = s16(0x28); s.road = u32(0x30);
            for (unsigned a = 0; a < 3; ++a) s.position[a] = f32(0x1c+4*a);
            for (unsigned v = 0; v < 4; ++v) {
                s.height[v] = nm_force_float(board.read_dword(0x501100+4*(v^1)));
                s.surface[v] = u32(0x1a8+4*v);
            }
            accepted = nm_wheels_ground(&contact,params.get(),&s);
            put(0x20,s.position[1]); put(0x28,s.pitch); put(0x2c,s.roll);
            put(0x30,s.road); put(0x1a4,s.selected_surface); put(0x1b8,s.combined_mask); put(0x1c2,s.road_heading);
            std::memcpy(expected.data()+0xf4,s.world,48);
            const unsigned offsets[] = {0x1c8,0x1c4,0x1cc,0x1d0,0x1d4,0x1d8};
            for (unsigned i = 0; i < 6; ++i) put(offsets[i],s.road_frame[i]);
            for (unsigned v = 0; v < 4; ++v) {
                put(0x1a8+4*v,s.surface[v]); put(0x1ba+2*v,s.mask[v]);
                std::memcpy(expected_heights.data()+4*(v^1),s.height+v,4);
            }
        } else {
            nm_motion_input_t v{}; nm_motion_state_t s{};
            v.held = operation == 9; v.flags = u32(0); v.scale = f32(0x1f8);
            v.speed_index = std::bit_cast<int32_t>(u32(0x18)); v.guided_ticks = s16(0x1f4);
            v.spin_ticks = board.read_byte(entity+0xec); v.spin_heading = s16(0xe8);
            v.spin_rotation = s16(0xea); v.hold_rotation = s16(0x1ec);
            s.heading = s16(0x48); s.yaw = s16(0x58); s.speed = f32(0x34);
            for (unsigned a = 0; a < 3; ++a) { s.position[a] = f32(0x1c+4*a); v.displacement[a] = f32(0x1e0+4*a); }
            for (unsigned a = 0; a < 2; ++a) {
                s.delta[a] = f32(0x9c+8*a); s.lookahead[a] = std::bit_cast<float>(board.read_dword(0x501864+4*a));
            }
            if (!v.held) {
                motion_spin += v.spin_ticks != 0;
                motion_guided += !v.spin_ticks && v.guided_ticks > 0;
                motion_normal += !v.spin_ticks && v.guided_ticks <= 0;
            }
            accepted = nm_motion_step(params.get(),&v,&s);
            put(0x48,s.heading); put(0x58,s.yaw); put(0x34,s.speed);
            for (unsigned a = 0; a < 3; ++a) put(0x1c+4*a,s.position[a]);
            for (unsigned a = 0; a < 2; ++a) put(0x9c+8*a,s.delta[a]);
            std::memcpy(expected_ahead.data(),s.lookahead,8);
        }
        ++entered[operation-1];
        if (!accepted) {
            if (!rejected[operation-1]) std::fprintf(stderr,"live_drive rejected frame=%llu operation=%u entity=%08x traction_calls=%llu yaw_calls=%llu\n",
                (unsigned long long)board.frame(),operation,entity,(unsigned long long)entered[0],(unsigned long long)entered[1]);
            ++rejected[operation-1];
        }
        pending = operation;
    }
    void report() const {
        for(const auto &[order,cycles]:field_orders) {
            std::printf("native_normal_field_order cycles=%llu events=",(unsigned long long)cycles);
            for(unsigned event:order) std::printf("%u,",event);
            std::puts(" (0..43=car,256=traffic,257=event-tail; delimited by tail entry, not video frames/replay)");
        }
        std::printf("native_normal_field_order unfinished_events=%zu (last partial cycle excluded)\n",field_turns.size());
        for(const auto &[order,cycles]:scene_orders) {
            std::printf("native_normal_scene_order cycles=%llu events=",(unsigned long long)cycles);
            for(unsigned event:order) std::printf("%u,",event);
            std::puts(" (258=camera,259=scenery,260=bitmap selection)");
        }
        std::printf("native_normal_scene_order unfinished_events=%zu (last pass excluded)\n",scene_turns.size());
        for(const auto &[mode,calls]:field_modes)
            std::printf("native_normal_field_mode entry=%05x calls=%llu (actual car dispatch)\n",mode,(unsigned long long)calls);
        collisions.report();
        std::printf("native_field_opponent_live checked=%llu (native IDs/neighbor records/shared state, one-turn adapter)\n",
            (unsigned long long)field_checked);
        std::printf("native_field_movement_live checked=%llu pending=%u (normal phase22 complete movement pass, mapped cars/effects/shared owners/marks/award)\n",
            (unsigned long long)movement_checked,movement_count);
        if(player_chain.road.storage && (!movement_checked || movement_count))
            throw std::runtime_error("missing or unfinished whole-field movement coverage");
        if(grid_reset_pending) throw std::runtime_error("unfinished native field grid reset");
        std::printf("native_field_grid_live resets=%u ids=%llx car_bytes=%zu stride_align=%zu\n",
            native_grid_resets,(unsigned long long)native_grid_ids,sizeof(nm_race_car_t),alignof(nm_race_car_t));
        if(finish_entity) throw std::runtime_error("unfinished opponent final callbacks");
        std::printf("native_opponent_complete_live checked=%llu visible=%llu height=%llu blocked_trigger_inputs=%llu (whole entity/shared intents, connected cde0..cebc)\n",
            (unsigned long long)finish_checked,(unsigned long long)finish_visible,(unsigned long long)finish_height,
            (unsigned long long)finish_blocked_trigger);
        if(motion_entity) throw std::runtime_error("unfinished opponent motion return");
        std::printf("native_opponent_motion_live checked=%llu ids=%llx (whole entity/shared count, cde0..cdec)\n",
            (unsigned long long)motion_checked,(unsigned long long)motion_ids);
        if(guided_entity) throw std::runtime_error("unfinished opponent guided return");
        std::printf("native_opponent_guided_live checked=%llu ids=%llx (whole entity/shared count)\n",
            (unsigned long long)guided_checked,(unsigned long long)guided_ids);
        if(lane_entity) throw std::runtime_error("unfinished lane return");
        std::printf("native_guided_lane_live checked=%llu ids=%llx (whole entity)\n",
            (unsigned long long)lane_checked,(unsigned long long)lane_ids);
        if(opponent_entity) throw std::runtime_error("unfinished opponent rolling update");
        std::printf("native_opponent_rolling_live checked=%llu ids=%llx released=%llu (whole entity)\n",
            (unsigned long long)opponent_checked,(unsigned long long)opponent_ids,(unsigned long long)opponent_released);
        if(marked_entity) throw std::runtime_error("unfinished marked resistance return");
        std::printf("native_opponent_resistance_live checked=%llu ids=%llx (whole entity; zero means no coverage)\n",
            (unsigned long long)marked_checked,(unsigned long long)marked_ids);
        if (boundary_pending) throw std::runtime_error("unfinished boundary return");
        if (contact.storage) player_chain.report();
        if (rolling_chain.entered) rolling_chain.report();
        else if (rolling_chain.road.storage) std::puts("native_rolling_prefix_live NO COVERAGE");
        const bool complete = checked[0] && checked[1] && checked[2] && (checked[3] || checked[4]) && checked[5] && checked[6] && checked[7] &&
            (!contact.storage || checked[9]) && !pending;
        bool failed = !complete;
        for (unsigned i = 0; i < 10; ++i) failed |= rejected[i] != 0;
        std::printf("native_drive_live %s traction=%llu/%llu yaw=%llu/%llu tires=%llu/%llu zero_speed=%llu straight=%llu pending=%u; actual call entries/returns, no target replay\n",
            failed ? "FAIL" : "PASS",
            (unsigned long long)checked[0],(unsigned long long)entered[0],(unsigned long long)checked[1],(unsigned long long)entered[1],
            (unsigned long long)checked[2],(unsigned long long)entered[2],
            (unsigned long long)zero_speed,(unsigned long long)straight,pending);
        std::printf("native_drive_live rejected_traction=%llu rejected_yaw=%llu rejected_nonparallel_yaw=%llu\n",
            (unsigned long long)rejected[0],(unsigned long long)rejected[1],(unsigned long long)rejected_turn);
        std::printf("native_drive_live rejected_tires=%llu\n",(unsigned long long)rejected[2]);
        std::printf("native_gears_live auto=%llu/%llu manual=%llu/%llu auto_up/down=%llu/%llu manual_up/down=%llu/%llu caller_hold=%llu rejected=%llu/%llu\n",
            (unsigned long long)checked[3],(unsigned long long)entered[3],(unsigned long long)checked[4],(unsigned long long)entered[4],
            (unsigned long long)gear_up[0],(unsigned long long)gear_down[0],(unsigned long long)gear_up[1],(unsigned long long)gear_down[1],
            (unsigned long long)gear_hold_calls,(unsigned long long)rejected[3],(unsigned long long)rejected[4]);
        std::printf("native_force_inputs_live resistance=%llu/%llu proximity=%llu/%llu disabled=%llu refreshed=%llu rejected=%llu/%llu\n",
            (unsigned long long)checked[5],(unsigned long long)entered[5],(unsigned long long)checked[6],(unsigned long long)entered[6],
            (unsigned long long)proximity_disabled_calls,(unsigned long long)proximity_refreshes,
            (unsigned long long)rejected[5],(unsigned long long)rejected[6]);
        std::printf("native_motion_live movement=%llu/%llu held=%llu/%llu normal=%llu spin=%llu guided=%llu rejected=%llu/%llu\n",
            (unsigned long long)checked[7],(unsigned long long)entered[7],(unsigned long long)checked[8],(unsigned long long)entered[8],
            (unsigned long long)motion_normal,(unsigned long long)motion_spin,(unsigned long long)motion_guided,
            (unsigned long long)rejected[7],(unsigned long long)rejected[8]);
        if (contact.storage) std::printf("native_ground_live complete116d0=%llu/%llu rejected=%llu course=%u\n",
            (unsigned long long)checked[9],(unsigned long long)entered[9],(unsigned long long)rejected[9],contact.slot);
        for (unsigned i = 0; i < 10; ++i) if (rejected[i]) throw std::runtime_error("native operations reject actual player inputs");
        if (!complete) throw std::runtime_error("live drive check has missing or incomplete player operations");
    }
};

inline void check_road(rt::M2Board &board) {
    if (!board.tgp().booted() || !board.tgp().fifo_out_empty())
        throw std::runtime_error("road oracle requires booted/idle reference TGP");
    for (unsigned i = 0; i < 256; ++i)
        if (board.read_dword(0x028b7d20 + i*4) != std::bit_cast<uint32_t>(float(i)/255.0f))
            throw std::runtime_error("road fraction table is not float32(i/255)");
    VehicleOracleBus bus(board);
    for (unsigned i = 0; i < NM_CONTROLS_CHECK_CASES; ++i) {
        nm_vehicle_test_t v; nm_vehicle_test_case(i,&v); bus.check_controls(v);
    }
    std::printf("native_controls PASS cases=%u hash=%08x exact_i960\n",NM_CONTROLS_CHECK_CASES,nm_controls_test_hash());
    for (unsigned i = 0; i < NM_ROAD_CHECK_CASES; ++i) {
        nm_road_lateral_t state;
        nm_road_test_case(i, &bus.profile, &state);
        bus.check(state);
    }
    std::printf("native_road_synthetic PASS cases=%u hash=%08x exact_i960_tgp\n",
                NM_ROAD_CHECK_CASES, nm_road_test_hash());
    unsigned total = 0;
    for (unsigned slot = 0; slot < 4; ++slot) {
        const uint32_t descriptor = board.read_dword(0x3a48 + slot*4);
        const auto rom = [&](uint32_t at) {
            if (at % 4 || at < 0x02000000 || at > 0x03fffffc)
                throw std::runtime_error("invalid road ROM address");
            return board.read_dword(at);
        };
        rom(descriptor); // Validate before adding field offsets.
        const auto begin = rom(descriptor+20), end = rom(descriptor+24);
        const auto logical = rom(begin);
        if (!logical || logical > 4096 || rom(begin+4) != 8 || end != begin+8+(logical+1)*164)
            throw std::runtime_error("unexpected authored road profile layout");
        unsigned cases = 0;
        for (unsigned row = 0; row <= logical; ++row) {
            for (unsigned w = 0; w < 41; ++w) {
                const auto value = rom(begin+8+row*164+w*4);
                std::memcpy(reinterpret_cast<uint8_t *>(&bus.profile)+w*4, &value, 4);
            }
            // Every profile, lane boundary and interpolation endpoint. Includes
            // the final distinct record; do not mistake logical count for length.
            for (int lane = 0; lane < 8; ++lane) for (int fraction : {0,1,127,128,254,255}) {
                const int16_t current = int16_t(lane*256+fraction);
                bus.check({current, current, 0, 0});
                ++cases;
            }
            for (int16_t delta : {-17,-16,-15,0,15,16,17})
                for (int16_t velocity : {-17,-16,-1,0,1,16,17})
                    for (int16_t control : {224,225}) {
                        bus.check({384,int16_t(384+delta),control,velocity});
                        ++cases;
                    }
        }
        std::printf("native_road_pose PASS slot=%u logical=%u records=%u cases=%u exact_xyz_angles_state\n",
                    slot, logical, logical+1, cases);
        total += cases;
    }
    std::printf("native_road_pose total=%u; authored lateral/spawn routine only, not driving/collision\n", total);
}
} // namespace daytona::native
