// Host-only original i960/TGP presentation-pose oracles, after scenario completion.
#pragma once
#include "../include/pose.h"
#include "../include/intro_math.h"
#include "../include/reflection.h"
#include "../tests/sky_cases.h"
#include "check_road.hpp"
#include "export_models.hpp"
#include "runtime/m2_board.h"
#include "refcore/i960_ref.h"
#include <bit>
#include <cfloat>
#include <memory>
#include <map>
#include <random>
#include <stdexcept>

namespace daytona::native {
struct ReflectionGeometryCheck {
    rt::M2Board *board=nullptr;
    decltype(rt::Geo::object_observer) next=nullptr;
    void *next_data=nullptr;
    unsigned samples[3]={}, words=0;
    rt::Cpu::CallObserver next_call=nullptr;
    void *next_call_data=nullptr;
    uint32_t actor=0;
    uint16_t expected_phase=0;
    nm_reflection_update_t expected_update{};
    unsigned live_controls=0,live_uploads=0;
    unsigned player_controls=0,leader_controls=0;
    uint32_t minimum_speed=UINT32_MAX,maximum_speed=0;
    std::map<uint32_t,unsigned> player_updaters;
    unsigned intro_spawn_rows=0,intro_player_spawns=0;
    bool previous_intro_control=false;
    unsigned retained_intro_phases=0;
    unsigned intro_body_samples=0,intro_body_changed=0;
    std::unique_ptr<nm_drive_params_t> body_math;
    nm_player_t expected_body{};
    uint32_t body_actor=0;
    bool body_spawn_pending=false;
    unsigned body_verified=0,body_tilt_enabled=0,body_tilt_nonzero=0,body_bank_nonzero=0;
    unsigned crash_calls=0,crash_triggered=0,crash_started=0,crash_active=0;
    unsigned special_crash_calls=0;
    unsigned special_draw_calls=0;
    std::map<uint32_t,unsigned> special_draw_models,special_draw_contexts;
    std::map<std::pair<uint32_t,uint32_t>,unsigned> special_body_draws;
    unsigned shadow_samples=0,shadow_faces=0;
    std::set<std::array<uint16_t,4>> shadow_headers;
    std::set<uint32_t> shadow_colors;
    std::map<uint32_t,unsigned> intro_collision_calls;
    std::map<uint32_t,unsigned> intro_traffic_flags;
    std::map<uint32_t,unsigned> intro_worker_speed;
    unsigned intro_worker_linked=0,intro_worker_other=0,intro_worker_hold_requests=0;
    static void observe_call(void *data,const rt::Cpu &cpu,uint32_t target,int type) {
        auto &self=*static_cast<ReflectionGeometryCheck *>(data);
        auto &board=*self.board;
        const uint32_t player=board.read_dword(0x501220);
        if((target==0x1786c || target==0x17a78) && cpu.m_IP>=0x7668 && cpu.m_IP<0x832c) {
            const uint32_t subject=cpu.m_r[24]; // Ordinary detail renderer's g8, not draw-context g13.
            if(subject>=0x500000 && subject<=0x5ffd00 && board.read_dword(subject+0xc)==0xd118)
                ++self.special_body_draws[{cpu.m_IP,cpu.m_r[16]}];
        }
        if(target==0x1786c && cpu.m_IP==0xd220) {
            const uint32_t subject=cpu.m_r[29],phase=board.read_dword(0x5010b0)&7;
            if(subject<0x500000 || subject>0x5ffd00 || board.read_dword(subject+0xc)!=0xd118 ||
               cpu.m_r[16]!=board.read_dword(0x234d58+4*phase))
                throw std::runtime_error("special draw subject/phase model mismatch");
            ++self.special_draw_calls;
            ++self.special_draw_models[cpu.m_r[16]];
            ++self.special_draw_contexts[board.read_dword(subject+0x10)];
        }
        if(board.frame()==300 && (target==0xd230 || target==0xd290 || target==0xd118 ||
                target==0x1225c || target==0x13f08 || target==0x6f24))
            std::printf("intro_tick_order frame=300 target=%08x subject=%08x player=%08x\n",target,cpu.m_r[29],player);
        if(player>=0x500000 && player<=0x5ffd00 && board.read_dword(player+0xc)==0xd230) {
            if(target==0x1225c) ++self.intro_traffic_flags[board.read_dword(0x501090)];
            if(target==0x12464) {
                const uint32_t worker=cpu.m_r[29];
                if(worker<0x500000 || worker>0x5ffd00) throw std::runtime_error("traffic worker outside RAM");
                ++self.intro_worker_speed[board.read_dword(worker+0x18)];
                self.intro_worker_linked+=board.read_dword(worker+0x88)!=0;
                self.intro_worker_other+=worker!=board.read_dword(0x501204);
                uint32_t at=worker;bool hold=false;
                for(unsigned i=0;i<2;++i) {
                    at=board.read_dword(at+0x88);
                    if(at && (at<0x500000 || at>0x5ffd00)) throw std::runtime_error("traffic worker link outside RAM");
                    hold|=std::bit_cast<int32_t>(board.read_dword(worker+0x18)-board.read_dword(at+0x18))>128;
                }
                self.intro_worker_hold_requests+=hold;
            }
            if(target==0x13f08 || target==0x14fcc || target==0x15678)
                ++self.intro_collision_calls[target];
            if(target==0x15de0 && (cpu.m_IP==0xd2b4 || cpu.m_IP==0xd13c)) {
                const uint32_t subject=cpu.m_r[29];
                if(subject<0x500000 || subject>0x5ffd00)
                    throw std::runtime_error("intro crash subject outside entity RAM");
                const bool trigger=board.read_dword(subject)&64;
                const bool start=trigger && !board.read_byte(subject+0xd4) && !board.read_word(subject+0xc8);
                ++self.crash_calls;self.crash_triggered+=trigger;self.crash_started+=start;
                self.special_crash_calls+=cpu.m_IP==0xd13c;
                self.crash_active+=board.read_word(subject+0x274)!=0;
                if(start) std::printf("intro_crash_start frame=%llu entity=%08x flags=%08x shared_counter=%u\n",
                    (unsigned long long)board.frame(),subject,board.read_dword(0x501090),board.read_byte(0x503680));
            }
        }
        if(target==0x11aa4 && cpu.m_IP==0xd28c) { // observer sees return PC
            if(self.body_actor) throw std::runtime_error("unverified intro body update");
            const uint32_t actor=cpu.m_r[29]; // g13, unlike camera g9.
            if(actor!=board.read_dword(0x501220))
                throw std::runtime_error("intro body update is not player");
            if(!self.body_math) self.body_math=std::make_unique<nm_drive_params_t>();
            self.body_math->inertial_scale[0]=std::bit_cast<float>(board.read_dword(0x28b576c));
            auto &s=self.expected_body;s={};
            s.force.rear_bank_force=std::bit_cast<float>(board.read_dword(actor+0x1a0));
            s.force.longitudinal_delta=std::bit_cast<float>(board.read_dword(actor+0x3c));
            s.filtered_pitch=std::bit_cast<int16_t>(board.read_word(actor+0x52));
            s.boundary.ground.pitch=std::bit_cast<int16_t>(board.read_word(actor+0x28));
            s.boundary.ground.yaw=std::bit_cast<int16_t>(board.read_word(actor+0x58));
            s.boundary.ground.roll=std::bit_cast<int16_t>(board.read_word(actor+0x2c));
            s.render_yaw_offset=std::bit_cast<int16_t>(board.read_word(actor+0x54));
            s.render_roll_offset=std::bit_cast<int16_t>(board.read_word(actor+0x20e));
            const int16_t tilt=std::bit_cast<int16_t>(board.read_word(0x501844));
            self.body_tilt_enabled+=board.read_byte(0x501874)!=0;
            self.body_tilt_nonzero+=tilt!=0;self.body_bank_nonzero+=s.force.rear_bank_force!=0;
            if(!nm_player_body_angles_step(self.body_math.get(),tilt,&s))
                throw std::runtime_error("live intro body input rejected");
            self.body_actor=actor;
        } else if(target==0x226468 || target==0x26468) {
            const uint32_t stage=board.read_dword(0x501858);
            if(stage<0x2800000 || stage>0x3fff000)
                throw std::runtime_error("intro stage outside authored ROM");
            const uint32_t count=board.read_dword(stage+4),row=cpu.m_r[16];
            if(!count || count>44 || row<stage+12 || (row-stage-12)%16 || (row-stage-12)/16>=count)
                throw std::runtime_error("intro initializer not an authored spawn row");
            const unsigned index=(row-stage-12)/16;
            const uint32_t player=board.read_dword(0x501220);
            if(player<0x500000 || player>0x5f0000 || board.read_dword(player+8)!=768 ||
               cpu.m_r[25]!=player+768*index)
                throw std::runtime_error("intro spawn ordering differs from native car array");
            if(!index) {
                if(board.read_dword(row+12)!=0xcf74)
                    throw std::runtime_error("intro player initializer changed");
                ++self.intro_player_spawns;
                self.body_spawn_pending=true;
            }
            ++self.intro_spawn_rows;
        } else if(target==0x6f24) {
            self.actor=cpu.m_r[25];
            if(self.actor<0x500000 || self.actor>0x5ffd00 || self.actor!=board.read_dword(0x5012f0))
                throw std::runtime_error("reflection actor is not the current camera entity");
            const uint32_t neighbor=board.read_dword(self.actor+0x88);
            if(neighbor<0x500000 || neighbor>0x5ffd00)
                throw std::runtime_error("reflection neighbor outside entity RAM");
            nm_reflection_input_t in{};
            in.frame=board.read_dword(0x5010b0);in.speed_index=board.read_dword(self.actor+0x18);
            self.player_controls+=self.actor==board.read_dword(0x501220);
            self.leader_controls+=self.actor==board.read_dword(0x501224);
            self.minimum_speed=std::min(self.minimum_speed,in.speed_index);
            self.maximum_speed=std::max(self.maximum_speed,in.speed_index);
            const uint32_t updater=board.read_dword(self.actor+0xc);
            ++self.player_updaters[updater];
            if(updater==0xd230) {
                if(!self.body_actor && self.body_spawn_pending) {
                    self.body_spawn_pending=false; // cf74 initializes e760; d230 starts next tick.
                } else {
                    if(self.body_actor!=self.actor) throw std::runtime_error("missing intro body observation");
                    for(unsigned a=0;a<3;++a)
                        if(board.read_word(self.actor+0x4c+2*a)!=(uint16_t)self.expected_body.render_angles[a])
                            throw std::runtime_error("live intro body angles differ from native helper");
                    if(board.read_word(self.actor+0x52)!=(uint16_t)self.expected_body.filtered_pitch ||
                       board.read_word(self.actor+0x56)!=(uint16_t)self.expected_body.bank)
                        throw std::runtime_error("live intro body filters differ from native helper");
                    self.body_actor=0;++self.body_verified;
                    self.body_spawn_pending=false;
                }
                /* Observe the final source pose: e760 alone (the current
                 * native intro) omits the later 11aa4 body-angle update. */
                const uint16_t baseline[3]={board.read_word(self.actor+0x28),
                    board.read_word(self.actor+0x2a),
                    (uint16_t)(board.read_word(self.actor+0x2c)+board.read_word(self.actor+0x20e))};
                bool changed=false;
                for(unsigned a=0;a<3;++a)
                    changed|=board.read_word(self.actor+0x4c+2*a)!=baseline[a];
                ++self.intro_body_samples;self.intro_body_changed+=changed;
            }
            in.flags=board.read_word(self.actor+0x138);in.previous_flags=board.read_word(self.actor+0x13a);
            in.neighbor_flags=board.read_word(neighbor+0x138);in.neighbor_previous_flags=board.read_word(neighbor+0x13a);
            in.heading=std::bit_cast<int16_t>(board.read_word(0x5014a8));
            in.course=board.read_byte(0x501460);in.next_mode=board.read_byte(0x5010a4);in.active_mode=board.read_byte(0x5010a0);
            const uint16_t phase=board.read_word(self.actor+6);
            if(self.previous_intro_control && updater==0xd230) {
                if(phase!=self.expected_phase) throw std::runtime_error("intro reflection phase reset outside control");
                ++self.retained_intro_phases;
            }
            self.previous_intro_control=updater==0xd230;
            self.expected_phase=phase;
            if(!nm_reflection_step(&in,&self.expected_phase,&self.expected_update))
                throw std::runtime_error("live reflection input rejected");
            ++self.live_controls;
        } else if(target==0x19ee0 && cpu.m_IP==0x6fdc) {
            if(!self.live_controls || self.expected_update.action!=NM_REFLECTION_UV ||
               cpu.m_r[16]!=board.read_dword(0x503588) || cpu.m_r[17]!=self.expected_update.v_offset ||
               cpu.m_r[18]!=self.expected_update.u_offset || board.read_word(self.actor+6)!=self.expected_phase)
                throw std::runtime_error("live shared reflection phase/UV differs from native control");
            ++self.live_uploads;
        }
        if(self.next_call) self.next_call(self.next_call_data,cpu,target,type);
    }
    static void observe(void *data,const rt::Geo::geo_state &state,
                        const rt::Geo::ObjectSource &source,std::span<const uint32_t> raster_words) {
        auto &self=*static_cast<ReflectionGeometryCheck *>(data);
        if(source.address==0x8d9e42) { // Model list288b7a8, ordinary body shadow.
            const auto faces=decode_model(state,source,raster_words);
            if(faces.size()!=4) throw std::runtime_error("shadow source face count changed");
            for(const auto &face:faces) {
                self.shadow_headers.insert(face.header);
                rt::GeoPoly poly{};std::copy_n(face.header.begin(),4,poly.texheader);
                poly.luma=uint8_t(face.reference_light);
                self.shadow_colors.insert(rt::material_solid_color(poly,self.board->video().gpu_mem()));
                if(!self.shadow_samples) {
                    std::printf("native_shadow_face flags=%08x vertices=%zu header=%04x/%04x/%04x/%04x normal=%g/%g/%g\n",
                        face.attribute,face.vertices.size(),face.header[0],face.header[1],face.header[2],face.header[3],
                        double(face.normal[0]),double(face.normal[1]),double(face.normal[2]));
                    for(const auto &v:face.vertices)
                        std::printf("native_shadow_vertex %g/%g/%g\n",double(v.position[0]),double(v.position[1]),double(v.position[2]));
                }
                ++self.shadow_faces;
            }
            ++self.shadow_samples;
        }
        const uint32_t addresses[3]={0x13b,0x1c8,0x241};
        const uint32_t points[3]={0x8050f8,0x805168,0x8051c8};
        const uint32_t headers[3]={0x800078,0x80009c,0x8000b8};
        for(unsigned model=0;model<3;++model) if(source.address==addresses[model]) {
            if(source.texture_points!=points[model] || source.texture_headers!=headers[model]) {
                std::fprintf(stderr,"window source points=%08x headers=%08x count=%u\n",source.texture_points,source.texture_headers,source.count);
                throw std::runtime_error("player window source layout changed");
            }
            // Two initial XYZs,13/11/9 ten-word strip records, terminator.
            // source.count is the remaining draw budget, NOT the model length.
            // Compare RAW positions/normals/attributes, not one projected view.
            for(unsigned i=0;i<137-20*model;++i) {
                if(state.polygon_ram0[addresses[model]+i]!=self.board->read_dword(0x29c0000+4*(addresses[model]+i)))
                    throw std::runtime_error("player window geometry differs from authored ROM baseline");
                ++self.words;
            }
            ++self.samples[model];
        }
        if(self.next) self.next(self.next_data,state,source,raster_words);
    }
    void report() const {
        std::printf("native_shadow_observation models=%u faces=%u headers=%zu colors=%zu\n",
            shadow_samples,shadow_faces,shadow_headers.size(),shadow_colors.size());
        for(uint32_t color:shadow_colors) std::printf("native_shadow_color argb=%08x\n",color);
        // Preserve the existing short player-window check; counts expose which
        // intro detail models were actually observed, not assumed covered.
        if(!samples[0]) throw std::runtime_error("reflection geometry has no observed player windows");
        std::printf("native_reflection_geometry PASS samples13b/1c8/241=%u/%u/%u raw_words=%u; all observed RAM windows equal canonical ROM, observed scenarios only\n",samples[0],samples[1],samples[2],words);
        if(!live_uploads) throw std::runtime_error("no live shared reflection uploads verified");
        std::printf("native_reflection_live PASS controls=%u UVuploads=%u; actor=5012f0 camera entity, shared UV phase, observed scenarios only\n",live_controls,live_uploads);
        std::printf("native_reflection_actor player=%u opponent0=%u speed_index=%u..%u; don't infer opponent0 phase\n",
            player_controls,leader_controls,minimum_speed,maximum_speed);
        for(const auto &[updater,count]:player_updaters)
            std::printf("native_reflection_actor_updater %08x samples=%u\n",updater,count);
        std::printf("native_intro_player_mapping rows=%u player_spawns=%u; spawn0=player501220, subsequent rows use768stride\n",
            intro_spawn_rows,intro_player_spawns);
        std::printf("native_intro_reflection_retained phases=%u; no reset between consecutive d230 controls\n",retained_intro_phases);
        std::printf("native_intro_body_observation samples=%u differs_from_e760=%u; final source pose, not native integration proof\n",
            intro_body_samples,intro_body_changed);
        std::printf("native_intro_body_live verified=%u tilt_enabled=%u tilt_nonzero=%u bank_nonzero=%u; same-input helper parity\n",
            body_verified,body_tilt_enabled,body_tilt_nonzero,body_bank_nonzero);
        std::printf("native_intro_crash_observation calls=%u special=%u triggered=%u starts=%u active=%u; original d290/d118 entries, no synthetic triggers\n",
            crash_calls,special_crash_calls,crash_triggered,crash_started,crash_active);
        for(const auto &[target,count]:intro_collision_calls)
            std::printf("native_intro_collision_calls target=%08x calls=%u\n",target,count);
        std::printf("native_intro_special_draw calls=%u model_choices=%zu contexts=%zu (d144->1786c, own g13; source10 is render context)\n",
            special_draw_calls,special_draw_models.size(),special_draw_contexts.size());
        for(const auto &[model,count]:special_draw_models)
            std::printf("native_intro_special_model list=%08x calls=%u\n",model,count);
        for(const auto &[key,count]:special_body_draws)
            std::printf("native_intro_special_body return=%08x list=%08x calls=%u\n",key.first,key.second,count);
        for(const auto &[flags,count]:intro_traffic_flags)
            std::printf("native_intro_traffic_flags flags=%08x calls=%u\n",flags,count);
        for(const auto &[speed,count]:intro_worker_speed)
            std::printf("native_intro_worker speed=%08x calls=%u linked=%u other=%u hold_requests=%u\n",
                speed,count,intro_worker_linked,intro_worker_other,intro_worker_hold_requests);
    }
};
inline void check_reflection_upload(rt::M2Board &board) {
    std::mt19937 random(0x55565749);
    unsigned descriptors=0, checked=0, gaps=0;
    for(uint32_t pointer:{0x503588u,0x503584u}) {
        const uint32_t table=board.read_dword(pointer);
        if(table<0x503380 || table>=0x503578 || table%2)
            throw std::runtime_error("reflection upload requires initialized authored descriptors");
        std::vector<bool> animated;
        const unsigned first=board.read_word(table+6);
        unsigned end=first;
        for(uint32_t at=table;;at+=8) {
            if(at>=0x503578) throw std::runtime_error("reflection descriptors unterminated");
            const unsigned count=board.read_word(at);
            if(count==65535) break;
            const unsigned start=board.read_word(at+6);
            if(count<3 || count>4 || start<end || ((start-first)&1) || start+2*count>0x8000)
                throw std::runtime_error("unexpected authored reflection descriptor");
            gaps+=start-end;animated.resize(start-first,false);
            animated.resize(start-first+2*count,true);end=start+2*count;++descriptors;
        }
        if(animated.empty()) throw std::runtime_error("empty authored reflection descriptors");
        // Verify that each drawable vertex in the two shared intro windows
        // receives an offset; strip-link-only UV gaps must not become vertices.
        unsigned window_vertices=0;
        for(unsigned model=0;model<2;++model) {
            uint32_t at=model ? 0x29c0904 : 0x29c0720;
            unsigned uv=model ? 0x1c8 : 0x168;
            at+=24;
            bool terminated=false;
            for(unsigned strip=0;strip<12;++strip,at+=40) {
                const uint32_t attr=board.read_dword(at);
                if(!(attr&3)) {terminated=true;break;}
                const unsigned vertices=(attr&1) ? 4 : 3;
                if(attr&(3u<<8)) for(unsigned half=0;half<2*vertices;++half) {
                    if(uv+half<first || uv+half-first>=animated.size() || !animated[uv+half-first])
                        throw std::runtime_error("intro window vertex outside shared reflection upload");
                    ++window_vertices;
                }
                uv+=2*vertices;
            }
            if(!terminated) throw std::runtime_error("intro window strip bound");
        }
        if(window_vertices!=96) throw std::runtime_error("intro reflection vertex inventory changed");
        for(unsigned i=0;i<1024;++i) {
            const uint16_t edges[4]={0,32767,32768,65535};
            const uint16_t offsets[2]={i<16 ? edges[i&3] : uint16_t(random()),
                                       i<16 ? edges[i>>2] : uint16_t(random())};
            board.write_dword(0x801008,0);
            rt::I960Ref cpu(&board);cpu.m_IP=0x19ee0;
            cpu.m_r[16]=table;cpu.m_r[17]=offsets[0];cpu.m_r[18]=offsets[1];
            cpu.m_r[26]=0x800000;cpu.m_r[28]=0x4000;
            unsigned instructions=0;
            while(cpu.m_IP!=0x19fa8) {
                if(++instructions>16384) throw std::runtime_error("reflection upload instruction guard");
                cpu.execute_one();
            }
            if(board.read_dword(0x802008)!=4*(animated.size()+3) ||
                board.read_dword(0x900000)!=0x02000000 || board.read_dword(0x900004)!=0x805000+first ||
                board.read_dword(0x900008)!=animated.size())
                throw std::runtime_error("reflection upload header/length differs");
            for(unsigned word=0;word<animated.size();++word) {
                const uint16_t base=board.read_word(0x29ea000+2*(first+word));
                const uint16_t offset=animated[word] ? offsets[word&1] : 0;
                const uint32_t actual=board.read_dword(0x90000c+4*word);
                if(actual!=uint32_t(base)+offset || uint16_t(actual)!=nm_reflection_uv(base,offset))
                    throw std::runtime_error("native reflection UV differs from original19ee0 upload");
                ++checked;
            }
        }
    }
    std::printf("native_reflection_upload PASS cases=2048 descriptors=%u gap_halfwords=%u checked_halfwords=%u; original19ee0..19fa8, V then U, low16 wrap\n",
        descriptors,gaps,checked);
}
inline void check_reflection(rt::M2Board &board) {
    check_reflection_upload(board); // Before the control oracle substitutes scratch descriptor tables.
    constexpr uint32_t entity=0x5fc000, neighbor=0x5fd000, all=0x5fe000, others=0x5fe100;
    std::mt19937 random(0x57494e44);
    unsigned actions[4]={}, neighbor_restores=0;
    uint32_t digest=2166136261u;
    for(unsigned i=0;i<65536;++i) {
        const auto flags=[&](unsigned bit) {return uint16_t((random()&~256u)|((i>>bit&1)<<8));};
        nm_reflection_input_t in{};
        in.frame=i;in.speed_index=random();
        if(i<4) {const uint32_t edges[4]={0,0xffffffff,0x80000000,0x7fffffff};in.speed_index=edges[i];}
        in.flags=flags(0);in.previous_flags=flags(1);in.neighbor_flags=flags(2);in.neighbor_previous_flags=flags(3);
        in.heading=std::bit_cast<int16_t>(uint16_t(i));in.course=(i>>4)&3;
        in.next_mode=(i>>6)&1;in.active_mode=0;
        uint16_t phase=uint16_t(i);
        board.write_word(entity+6,phase);board.write_dword(entity+0x18,in.speed_index);
        board.write_word(entity+0x138,in.flags);board.write_word(entity+0x13a,in.previous_flags);
        board.write_dword(entity+0x88,neighbor);
        board.write_word(neighbor+0x138,in.neighbor_flags);board.write_word(neighbor+0x13a,in.neighbor_previous_flags);
        board.write_word(0x5014a8,uint16_t(in.heading));board.write_dword(0x5010b0,in.frame);
        board.write_byte(0x501460,in.course);board.write_byte(0x5010a4,in.next_mode);board.write_byte(0x5010a0,in.active_mode);
        board.write_dword(0x503588,all);board.write_dword(0x503584,others);
        nm_reflection_update_t expected{};
        rt::I960Ref cpu(&board);cpu.m_IP=0x6f24;cpu.m_r[25]=entity;
        unsigned count=0,uploads=0;
        while(cpu.m_IP!=0x6fdc && cpu.m_IP!=0x7008 && cpu.m_IP!=0x7020) {
            if(++count>64) throw std::runtime_error("reflection original instruction guard");
            if(cpu.m_IP==0x6fd8 || cpu.m_IP==0x7004 || cpu.m_IP==0x701c) {
                if(++uploads>1 || (cpu.m_r[16]!=all && cpu.m_r[16]!=others))
                    throw std::runtime_error("reflection unexpected table/upload");
                expected.others_only=cpu.m_r[16]==others;
                if(cpu.m_IP==0x6fd8) {
                    if(cpu.m_r[17]>32767 || cpu.m_r[18]>32767) throw std::runtime_error("reflection UV mask");
                    expected.action=NM_REFLECTION_UV;expected.v_offset=uint16_t(cpu.m_r[17]);expected.u_offset=uint16_t(cpu.m_r[18]);
                } else {
                    expected.action=cpu.m_IP==0x7004 ? NM_REFLECTION_TUNNEL : NM_REFLECTION_RESTORE;
                    const uint32_t first=expected.action==NM_REFLECTION_TUNNEL ? 0x03c003c0 : 0;
                    const uint32_t third=expected.action==NM_REFLECTION_TUNNEL ? 0x1fff0c0c : 0;
                    if(cpu.m_r[17]!=first || cpu.m_r[18] || cpu.m_r[19]!=third || cpu.m_r[20])
                        throw std::runtime_error("reflection header masks changed");
                }
                cpu.m_IP+=4; // Intercept texture emission only, not any state/branch calculation.
            } else cpu.execute_one();
        }
        nm_reflection_update_t actual{};
        if(!nm_reflection_step(&in,&phase,&actual) || phase!=board.read_word(entity+6) ||
            actual.action!=expected.action || actual.others_only!=expected.others_only || actual.v_offset!=expected.v_offset || actual.u_offset!=expected.u_offset)
            throw std::runtime_error("native reflection differs from original6f24 state/upload intent");
        ++actions[actual.action];neighbor_restores+=actual.others_only;
        for(uint32_t value:{uint32_t(phase),uint32_t(actual.action),uint32_t(actual.others_only),uint32_t(actual.v_offset),uint32_t(actual.u_offset)})
            digest=(digest^value)*16777619u;
    }
    for(auto count:actions) if(!count) throw std::runtime_error("reflection branch has no coverage");
    if(!neighbor_restores) throw std::runtime_error("reflection neighbor branch has no coverage");
    uint16_t phase=123;nm_reflection_update_t output{1,2,3,1};nm_reflection_input_t in{};
    if(nm_reflection_step(nullptr,&phase,&output) || nm_reflection_step(&in,nullptr,&output) ||
        nm_reflection_step(&in,&phase,nullptr) || phase!=123 || output.v_offset!=1 || output.u_offset!=2 || output.action!=3 || output.others_only!=1)
        throw std::runtime_error("invalid reflection input published state");
    std::printf("native_reflection PASS cases=65536 original6f24..7020 none/UV/tunnel/restore=%u/%u/%u/%u neighbor_restore=%u hash=%08x; upload intent only, not window assets/runtime\n",
        actions[0],actions[1],actions[2],actions[3],neighbor_restores,digest);
}
inline void check_wheel_pose(rt::M2Board &board,const char *path) {
    std::unique_ptr<nm_drive_params_t,decltype(&std::free)> p(nm_drive_load(path),&std::free);
    if (!p || !board.tgp().booted() || !board.tgp().fifo_out_empty())
        throw std::runtime_error("wheel oracle requires validated math and idle booted TGP");
    constexpr uint32_t entity=0x5fc000, context=0x5fd000, models=0x5fe000;
    const uint32_t calls[4]={0x74d8,0x7564,0x75dc,0x7654};
    const unsigned hide_bits[4]={1,0,3,2};
    float xyz[4][3];
    for(unsigned w=0;w<4;++w) for(unsigned a=0;a<3;++a)
        xyz[w][a]=std::bit_cast<float>(board.read_dword(0x234af4+12*w+4*a));
    std::mt19937 random(0x57484545);
    unsigned matrices=0,hidden=0;float worst=0;
    for(unsigned i=0;i<4096;++i) {
        nm_player_t car{};
        car.filtered_pitch=std::bit_cast<int16_t>(uint16_t(random()));
        car.bank=std::bit_cast<int16_t>(uint16_t(random()));
        car.force.steer_angle=std::bit_cast<int16_t>(uint16_t(random()));
        for(auto &phase:car.wheel_phase) phase=std::bit_cast<int16_t>(uint16_t(random()));
        if(i<8) {
            const int16_t edges[8]={-32768,-32767,-16384,-1,0,1,16384,32767};
            car.filtered_pitch=edges[i];car.bank=edges[7-i];car.force.steer_angle=edges[(i+3)%8];
            car.wheel_phase[0]=edges[i];car.wheel_phase[1]=edges[7-i];
        }
        car.boundary.damage.damage=i&255;
        board.write_word(entity+0x52,uint16_t(car.filtered_pitch));
        board.write_word(entity+0x56,uint16_t(car.bank));
        board.write_word(entity+0x124,uint16_t(car.force.steer_angle));
        board.write_word(entity+0x5a,uint16_t(car.wheel_phase[0]));
        board.write_word(entity+0x250,uint16_t(car.wheel_phase[1]));
        board.write_dword(entity+0x208,car.boundary.damage.damage);
        board.write_dword(context+0x14,models);
        for(unsigned w=0;w<4;++w) board.write_dword(models+4*w,0x2800000+16*w);
        board.write_dword(0x5014a4,0); // Normal external view, not VR2 hiding.
        const unsigned mask=(i>>8)&15;
        board.write_dword(0x501a98,mask);
        board.write_dword(0x880100,0x1010); // Original identity command.
        rt::I960Ref cpu(&board);
        cpu.m_IP=0x7384;cpu.m_r[25]=entity;cpu.m_r[27]=0x880000;
        cpu.m_r[28]=0x4000;cpu.m_r[29]=context;
        unsigned count=0,seen=0;
        while(cpu.m_IP!=0x7664) {
            if(++count>512) throw std::runtime_error("wheel original instruction guard");
            unsigned w=0;while(w<4 && cpu.m_IP!=calls[w]) ++w;
            if(w==4) {cpu.execute_one();continue;}
            if((seen&(1u<<w)) || (mask&(1u<<hide_bits[w])) || cpu.m_r[16]!=0x2800000+16*w)
                throw std::runtime_error("wheel draw order/model/visibility mismatch");
            seen|=1u<<w;
            float actual[12];
            if(!nm_wheel_pose(p.get(),&car,w,xyz[w],actual))
                throw std::runtime_error("native wheel pose rejected valid source state");
            board.write_dword(0x880110,0x1111);
            for(unsigned a=0;a<12;++a) {
                const float expected=std::bit_cast<float>(board.read_dword(0x884000));
                const float error=fabsf(actual[a]-expected);
                if(!std::isfinite(expected) || error>12*FLT_EPSILON) {
                    std::fprintf(stderr,"wheel case=%u wheel=%u component=%u actual=%g expected=%g error=%g\n",
                        i,w,a,double(actual[a]),double(expected),double(error));
                    throw std::runtime_error("native wheel differs from original7384 i960/TGP");
                }
                if(error>worst) worst=error;
            }
            ++matrices;
            // Skip only geometry emission1786c: inspect its incoming matrix,
            // then execute the original pop/next-wheel branches unchanged.
            cpu.m_IP+=4;
        }
        for(unsigned w=0;w<4;++w) {
            if(bool(seen&(1u<<w))==bool(mask&(1u<<hide_bits[w])))
                throw std::runtime_error("missing or unexpected wheel draw");
            hidden+=!(seen&(1u<<w));
        }
        if(!board.tgp().fifo_out_empty()) throw std::runtime_error("wheel unread TGP output");
    }
    std::printf("native_wheel_pose PASS cases=4096 matrices=%u hidden=%u max_error=%g (12*FLT_EPSILON); original7384..7664, normal external view; not asset/runtime integration\n",
        matrices,hidden,double(worst));
    worst=0;
    unsigned special_matrices=0;
    for(unsigned i=0;i<12288;++i) {
        const bool special=i>=4096 && i<8192,shadow=i>=8192;
        nm_player_t car{};
        for(unsigned a=0;a<3;++a) {
            car.boundary.ground.position[a]=float(int(random()%20000)-10000)*0.125f;
            car.render_angles[a]=std::bit_cast<int16_t>(uint16_t(random()));
            car.boundary.damage.angle[a]=std::bit_cast<int16_t>(uint16_t(random()));
            board.write_dword(entity+0x1c+4*a,std::bit_cast<uint32_t>(car.boundary.ground.position[a]));
            board.write_word(entity+0x4c+2*a,(uint16_t)car.render_angles[a]);
            board.write_word(entity+0xca+2*a,(uint16_t)car.boundary.damage.angle[a]);
        }
        car.road_yaw=std::bit_cast<int16_t>(uint16_t(random()));
        car.inhibit_angle=std::bit_cast<int16_t>(uint16_t(random()));
        car.boundary.damage.overlay_model=(i/4)%4==0 ? 0 : (i/4)%4==1 ? 0x80000000u :
            std::bit_cast<uint32_t>(float(int(random()%512)-256)*0.125f);
        board.write_word(entity+0x26e,(uint16_t)car.road_yaw);
        board.write_word(entity+0xd6,(uint16_t)car.inhibit_angle);
        board.write_dword(entity+0x5c,car.boundary.damage.overlay_model);
        board.write_dword(0x880100,0x1010);
        const uint32_t starts[4]={0x7974,0x7c4c,0x7f20,0x81dc};
        const uint32_t stops[4]={0x7aa0,0x7d78,
            car.boundary.damage.overlay_model ? 0x804cu : 0x8058u,
            car.boundary.damage.overlay_model ? 0x8308u : 0x8314u};
        rt::I960Ref cpu(&board);cpu.m_IP=special ? 0xd144 : starts[i%4];cpu.m_r[24]=entity;
        const uint32_t shadow_starts[3]={0x7b04,0x7ddc,0x8098};
        const uint32_t shadow_stops[3]={0x7c18,0x7ef0,0x81ac};
        if(shadow) cpu.m_IP=car.boundary.damage.overlay_model ? shadow_starts[i%3] : starts[i%3];
        const uint32_t stop=shadow ? (car.boundary.damage.overlay_model ? shadow_stops[i%3] : stops[i%3]) :
            special ? 0xd21c : stops[i%4];
        cpu.m_r[29]=entity;
        cpu.m_r[27]=0x880000;cpu.m_r[28]=0x4000;
        unsigned steps=0;
        while(cpu.m_IP!=stop) {
            if(++steps>100) throw std::runtime_error("intro body instruction guard");
            cpu.execute_one();
        }
        float actual[12];
        if(!(shadow ? nm_intro_shadow_pose(p.get(),&car,actual) :
             special ? nm_intro_special_pose(p.get(),&car,actual) : nm_intro_body_pose(p.get(),&car,actual)))
            throw std::runtime_error("intro part rejected finite input");
        board.write_dword(0x880110,0x1111);
        for(unsigned a=0;a<12;++a) {
            const float expected=std::bit_cast<float>(board.read_dword(0x884000));
            const float error=fabsf(actual[a]-expected)/(1+fabsf(expected));
            if(!std::isfinite(expected) || error>16*FLT_EPSILON)
                throw std::runtime_error("intro body differs from original per-detail transform");
            worst=std::max(worst,error);
        }
        if(special) {
            ++special_matrices;
            board.write_dword(0x880060,0x606); // Balance d144's matrix push, no geometry emission skipped as proof.
        }
    }
    std::printf("native_intro_body_pose PASS cases=4096 scaled_error=%g; all4detail transforms to first draw, raw +/-zero gate; excludes attachments/shadows\n",double(worst));
    std::printf("native_intro_special_pose PASS cases=%u scaled_error=%g; original d144 to first draw, not model integration\n",special_matrices,double(worst));
    std::printf("native_intro_shadow_pose PASS cases=4096 scaled_error=%g; three crash detail blocks, normal inherited body, no draw/quota integration\n",double(worst));
    for(uint32_t bits:{0x7f800000u,0xff800000u,0x7fc00000u}) {
        nm_player_t car{};car.boundary.damage.overlay_model=bits;
        float output[12];std::fill_n(output,12,123.0f);
        if(nm_intro_body_pose(p.get(),&car,output)) throw std::runtime_error("nonfinite crash height accepted");
        for(float value:output) if(value!=123.0f) throw std::runtime_error("invalid crash pose published output");
        if(nm_intro_special_pose(p.get(),&car,output)) throw std::runtime_error("nonfinite special height accepted");
        for(float value:output) if(value!=123.0f) throw std::runtime_error("invalid special pose published output");
        // Shadow only tests raw5c, never reads its float height.
        if(!nm_intro_shadow_pose(p.get(),&car,output)) throw std::runtime_error("shadow consumed unused crash height");
        car.boundary.ground.position[1]=std::bit_cast<float>(bits);
        std::fill_n(output,12,123.0f);
        if(nm_intro_shadow_pose(p.get(),&car,output)) throw std::runtime_error("nonfinite shadow ground accepted");
        for(float value:output) if(value!=123.0f) throw std::runtime_error("invalid shadow pose published output");
    }
}
inline void check_intro_camera(rt::M2Board &board,const nm_drive_params_t *p,uint32_t base) {
    static_assert(sizeof(nm_intro_key_t)==64 && sizeof(nm_intro_anchor_t)==32 && sizeof(nm_intro_camera_t)==32);
    const unsigned count=base==0x2864030 ? 6 : 11;
    nm_intro_key_t keys[11]{};uint32_t times[11];
    for(unsigned i=0;i<count;++i) {
        const uint32_t at=base+48*i;
        times[i]=board.read_dword(at);
        keys[i].position_anchor=std::bit_cast<int32_t>(board.read_dword(at+4));
        keys[i].target_anchor=std::bit_cast<int32_t>(board.read_dword(at+20));
        for(unsigned a=0;a<3;++a) {
            keys[i].position[a]=std::bit_cast<float>(board.read_dword(at+8+4*a));
            keys[i].target[a]=std::bit_cast<float>(board.read_dword(at+24+4*a));
        }
        keys[i].roll=std::bit_cast<float>(board.read_dword(at+36));
        keys[i].focus=std::bit_cast<float>(board.read_dword(at+40));
        if(board.read_dword(at+44)) throw std::runtime_error("intro math oracle requires event-free keys");
    }
    std::vector<uint32_t> saved(0x100000/4);
    for(unsigned i=0;i<saved.size();++i) saved[i]=board.read_dword(0x500000+4*i);
    const auto restore=[&] {for(unsigned i=0;i<saved.size();++i) board.write_dword(0x500000+4*i,saved[i]);};
    float worst=0, view_worst=0;
    try {
        board.write_dword(0x501280,0x5fc000);
        board.write_dword(0x50149c,base-48);board.write_dword(0x5014a0,0);
        for(unsigned offset=0;offset<0x300;offset+=4) board.write_dword(0x5fe000+offset,0);
        board.write_dword(0x5012f0,0x5fa000);board.write_dword(0x5014a4,1);
        board.write_byte(0x501713,1);board.write_byte(0x501710,2);
        board.write_dword(0x5fa138,0);board.write_word(0x5fa128,0);
        nm_intro_cursor_t cursor{};
        for(unsigned tick=0;tick<2560;++tick) {
            nm_intro_anchor_t anchors[21]{};
            for(unsigned i=0;i<21;++i) {
                const uint32_t entity=0x5fa000+i*0x100;
                board.write_dword(0x5fc01c+4*i,entity);
                for(unsigned a=0;a<3;++a) {
                    anchors[i].position[a]=float(int((tick*13+i*97+a*53)%8192)-4096)/8;
                    anchors[i].angles[a]=std::bit_cast<int16_t>(uint16_t(tick*53+i*179+a*113));
                    board.write_dword(entity+0x1c+4*a,std::bit_cast<uint32_t>(anchors[i].position[a]));
                    board.write_word(entity+0x28+2*a,uint16_t(anchors[i].angles[a]));
                }
            }
            nm_intro_interval_t interval{};nm_intro_camera_t camera{};
            if(!nm_intro_camera_step(times,count,&cursor,&interval) ||
               !nm_intro_camera_sample(p,&keys[interval.key],&keys[interval.key+1],anchors,21,
                                       interval.elapsed,interval.duration,&camera))
                throw std::runtime_error("native intro camera rejected authored keys");
            if(!tick) {
                const auto unchanged=camera;
                auto invalid=keys[1];invalid.target_anchor=21;
                if(nm_intro_camera_sample(p,&keys[0],&invalid,anchors,21,0,100,&camera) ||
                   memcmp(&camera,&unchanged,sizeof(camera)) ||
                   nm_intro_camera_sample(p,&keys[0],&keys[1],anchors,21,0,0,&camera) ||
                   memcmp(&camera,&unchanged,sizeof(camera)))
                    throw std::runtime_error("invalid intro camera input mutated output");
            }
            rt::I960Ref cpu(&board);cpu.m_IP=0x5cac;cpu.m_r[27]=0x880000;cpu.m_r[28]=0x4000;
            unsigned steps=0;
            while(cpu.m_IP!=0x60e8) {
                if(++steps>4096) throw std::runtime_error("intro camera instruction guard");
                cpu.execute_one();
            }
            for(unsigned a=0;a<3;++a) for(unsigned target=0;target<2;++target) {
                const float expected=std::bit_cast<float>(target ? cpu.m_r[23+a] : board.read_dword(0x501720+4*a));
                const float actual=target ? camera.target[a] : camera.position[a];
                const float error=std::abs(actual-expected);worst=std::max(worst,error);
                // Synthetic anchors <=512 and local authored components <=300;
                // absolute 16*epsilon*1024 bound includes cancellation near0.
                if(!std::isfinite(expected) || error>16*FLT_EPSILON*1024)
                    throw std::runtime_error("intro anchored interpolation differs from original");
            }
            if(std::bit_cast<uint32_t>(camera.focus)!=board.read_dword(0x501734) ||
               uint16_t(camera.roll)!=board.read_word(0x50173c))
                throw std::runtime_error("intro roll/focus differs from original");
            // Same-input direction/view checks: interpolation error must not
            // be conflated with angle-table quantization at a boundary.
            nm_intro_camera_t source=camera;
            for(unsigned a=0;a<3;++a) {
                source.position[a]=std::bit_cast<float>(board.read_dword(0x501720+4*a));
                source.target[a]=std::bit_cast<float>(cpu.m_r[23+a]);
            }
            while(cpu.m_IP!=0x61fc) {
                if(++steps>4096) throw std::runtime_error("intro direction instruction guard");
                cpu.execute_one();
            }
            int16_t angles[3];
            if(!nm_intro_camera_angles(p,&source,angles)) throw std::runtime_error("intro direction rejected source");
            for(unsigned a=0;a<3;++a) if(uint16_t(angles[a])!=board.read_word(0x501738+2*a))
                throw std::runtime_error("intro direction differs from original60e8");
            if(board.read_dword(0x5014a0)!=cursor.tick) throw std::runtime_error("intro full tick differs");
            cpu.m_IP=0x6510;cpu.m_r[29]=0x5fe000;steps=0;
            while(cpu.m_IP!=0x6b58) {
                if(++steps>4096) throw std::runtime_error("intro view instruction guard");
                cpu.execute_one();
            }
            float view[12];int16_t heading;uint16_t sky;
            if(!nm_intro_camera_view(p,&source,view,&heading,&sky) || uint16_t(heading)!=board.read_word(0x5014a8))
                throw std::runtime_error("intro view heading differs");
            if((sky|0x2000)!=board.read_word(0x50130a))
                throw std::runtime_error("intro sky differs from original cinematic camera");
            for(unsigned a=0;a<12;++a) {
                const float expected=std::bit_cast<float>(board.read_dword(0x501560+4*a));
                const float scale=a<9 ? 1 : 1+fabsf(source.position[0])+fabsf(source.position[1])+fabsf(source.position[2]);
                const float error=fabsf(view[a]-expected)/scale;view_worst=std::max(view_worst,error);
                if(!std::isfinite(expected) || error>8*FLT_EPSILON) throw std::runtime_error("intro world view differs from original6510");
            }
            if(!board.tgp().fifo_out_empty()) throw std::runtime_error("intro unread TGP output");
        }
    } catch(...) {restore();throw;}
    restore();
    std::printf("native_intro_camera PASS ticks=2560 keys=%u live_anchors=21 max_abs=%g (16epsilon*1024 bound; original5cac..60e0, not look-at angles/view)\n",count,double(worst));
    std::printf("native_intro_view PASS ticks=2560 exact_angles/sky_scroll, max_scaled=%g (same-input original60e8/6510, world-space mode1)\n",double(view_worst));
}
inline void check_intro_speed(rt::M2Board &board,const nm_drive_params_t *p) {
    nm_intro_speed_params_t params{};
    params.desired_divisor=std::bit_cast<float>(board.read_dword(0x28b57a4));
    params.drag_offset=std::bit_cast<float>(board.read_dword(0x28b5814));
    params.minimum_delta=std::bit_cast<float>(board.read_dword(0x28b57b8));
    for(unsigned i=0;i<512;++i) params.response[i]=std::bit_cast<float>(board.read_dword(0x28b85a0+4*i));
    VehicleOracleBus bus(board);
    for(unsigned index=0;index<512;++index) for(unsigned variant=0;variant<32;++variant) {
        nm_player_t state{};
        state.speed_index=index;state.desired_speed=float(int(variant)-8)*0.1f+float(index)*0.01f;
        state.boundary.speed=variant==0 ? -1.f : float(index)/p->speed_index_scale;
        state.resistance=float(int(variant%7)-3)*0.03125f;
        state.force.longitudinal_delta=17;
        const uint32_t flags=1u<<(variant%4==0 ? 0 : variant%4==1 ? 3 : variant%4==2 ? 22 : 28);
        bus.entity.fill(0xa5);bus.globals.fill(0x5a);
        bus.write_dword(0x501090,flags);
        bus.write_dword(0x500018,index);bus.write_dword(0x500034,std::bit_cast<uint32_t>(state.boundary.speed));
        bus.write_dword(0x50003c,std::bit_cast<uint32_t>(state.force.longitudinal_delta));
        bus.write_dword(0x500044,std::bit_cast<uint32_t>(state.desired_speed));
        bus.write_dword(0x500164,std::bit_cast<uint32_t>(state.resistance));
        if(!nm_intro_speed_step(p,&params,flags,&state)) throw std::runtime_error("intro speed rejected finite input");
        auto expected=bus.entity;const auto globals=bus.globals;
        memcpy(expected.data()+0x18,&state.speed_index,4);
        memcpy(expected.data()+0x34,&state.boundary.speed,4);
        memcpy(expected.data()+0x3c,&state.force.longitudinal_delta,4);
        bus.run(0x11438,0x115e0);
        if(bus.entity!=expected || bus.globals!=globals) {
            std::fprintf(stderr,"intro speed mismatch index=%u variant=%u native=%08x/%08x/%08x source=%08x/%08x/%08x\n",index,variant,
                (uint32_t)state.speed_index,std::bit_cast<uint32_t>(state.boundary.speed),std::bit_cast<uint32_t>(state.force.longitudinal_delta),
                bus.read_dword(0x500018),bus.read_dword(0x500034),bus.read_dword(0x50003c));
            throw std::runtime_error("intro speed differs from original11438");
        }
    }
    std::puts("native_intro_speed PASS cases=16384 (all512curve indices, phase gating, source11438, full entity/global canaries)");
}
inline void check_intro_correction(rt::M2Board &board) {
    VehicleOracleBus bus(board);
    for(unsigned i=0;i<8192;++i) {
        nm_player_t player{};
        player.correction_ticks=std::bit_cast<int8_t>((uint8_t)i);
        player.correction[0]=float(int(i%101)-50)*0.125f;player.correction[1]=-player.correction[0];
        player.boundary.ground.position[0]=float(i)*0.25f;player.boundary.ground.position[2]=-float(i);
        player.boundary.heading=nm_yaw_angle(i*997);player.body_yaw=nm_yaw_angle(i*331);
        player.boundary.ground.pitch=nm_yaw_angle(i*7);player.boundary.ground.roll=nm_yaw_angle(i*101);
        player.render_roll_offset=nm_yaw_angle(i*171);player.boundary.damage.flags=i*2654435761u;
        const int16_t other=nm_yaw_angle(i*4711);int16_t own=nm_yaw_angle(i*103);
        const auto before=player;const auto old_own=own;
        PlayerMemory memory;memory.entity.fill(0xa5);memory.fields(player,true);
        memcpy(memory.entity.data()+0x212,&other,2);memcpy(memory.entity.data()+0x216,&own,2);
        bus.entity=memory.entity;bus.globals.fill(0x5a);const auto globals=bus.globals;
        if(!nm_intro_correction_step(&player,other,&own)) throw std::runtime_error("intro correction rejected finite input");
        nm_intro_render_angles(&player);memory.fields(player,true);memcpy(memory.entity.data()+0x216,&own,2);
        bus.run(0x153c4,0x154cc);bus.run(0xe760,0xe788);
        if(bus.entity!=memory.entity || bus.globals!=globals)
            throw std::runtime_error("intro correction/render angles differ from original153c4/e760");
        auto invalid=before;invalid.correction_ticks=3;invalid.correction[1]=INFINITY;
        const auto saved=invalid;own=old_own;
        if(nm_intro_correction_step(&invalid,other,&own) || memcmp(&invalid,&saved,sizeof(saved)) || own!=old_own)
            throw std::runtime_error("intro correction failure published partial outputs");
    }
    std::puts("native_intro_correction PASS cases=8192 (all signed timer bytes, wrapped heading, original153c4/e760 full entity/globals, atomic invalid arithmetic)");
}
inline void check_intro_events(rt::M2Board &board) {
    std::vector<uint32_t> saved(0x100000/4);
    for(unsigned i=0;i<saved.size();++i) saved[i]=board.read_dword(0x500000+4*i);
    const auto restore=[&] { for(unsigned i=0;i<saved.size();++i) board.write_dword(0x500000+4*i,saved[i]); };
    unsigned ticks=0,changed=0;
    try {
        for(uint32_t stage : {0x2865a80u,0x2865d58u}) {
            const uint32_t source=board.read_dword(stage),count=board.read_dword(stage+4);
            if(count<1 || count>44) throw std::runtime_error("intro stage car count invalid");
            std::vector<nm_intro_event_t> events;
            for(unsigned i=0;i<256;++i) {
                const uint32_t at=source+16*i,packed=board.read_dword(at+8),callback=board.read_dword(at+12);
                const int16_t actor=nm_yaw_angle(packed),lateral=nm_yaw_angle(packed>>16);
                float speed=0;
                if(actor>=0) {
                    const uint32_t index=board.read_dword(at+4);
                    if(index>=512 || (callback && callback!=0x2265c0)) throw std::runtime_error("unsupported intro event source");
                    speed=std::bit_cast<float>(board.read_dword(0x28b7520+4*index))*
                          std::bit_cast<float>(board.read_dword(0x28b573c));
                }
                events.push_back({board.read_dword(at),speed,actor,lateral,callback==0x2865d58 ? 1u : UINT32_MAX});
                if(actor<0) break;
            }
            if(events.back().actor>=0) throw std::runtime_error("intro event transition missing");
            constexpr uint32_t cars_base=0x5e0000,script=0x5fd000,stride=0x300;
            std::vector<nm_player_t> cars(count);
            std::vector<std::array<uint8_t,stride>> expected(count);
            for(unsigned car=0;car<count;++car) {
                expected[car].fill(0xa5);
                memcpy(expected[car].data()+8,&stride,4);
                memcpy(expected[car].data()+0x44,&cars[car].desired_speed,4);
                memcpy(expected[car].data()+0x140,&cars[car].lateral.target,2);
                for(unsigned b=0;b<stride;++b) board.write_byte(cars_base+stride*car+b,expected[car][b]);
            }
            board.write_dword(0x501220,cars_base);board.write_dword(0x50185c,source);
            board.write_dword(0x501858,stage);board.write_dword(script+12,0x2264f4);
            board.write_dword(script+0x34,0);board.write_byte(script+0x38,0);
            nm_intro_cursor_t cursor{};
            for(unsigned tick=0;tick<1100;++tick) {
                uint32_t next=UINT32_MAX;
                const int result=nm_intro_events_step(events.data(),events.size(),cars.data(),count,&cursor,&next);
                if(!result) throw std::runtime_error("intro native events rejected authored source");
                rt::I960Ref cpu(&board);cpu.m_IP=0x2264f4;
                cpu.m_r[27]=0x880000;cpu.m_r[28]=0x4000;cpu.m_r[29]=script;
                unsigned instructions=0;
                while(board.fetch(cpu.m_IP)!=0x0a000000 || cpu.m_rcache_pos) {
                    if(++instructions>4096) throw std::runtime_error("intro event original instruction guard");
                    cpu.execute_one();
                }
                for(unsigned car=0;car<count;++car) {
                    changed+=memcmp(expected[car].data()+0x44,&cars[car].desired_speed,4)!=0;
                    memcpy(expected[car].data()+0x44,&cars[car].desired_speed,4);
                    memcpy(expected[car].data()+0x140,&cars[car].lateral.target,2);
                    for(unsigned b=0;b<stride;++b) if(board.read_byte(cars_base+car*stride+b)!=expected[car][b])
                        throw std::runtime_error("intro event car differs from original2264f4");
                }
                if(board.read_dword(script+0x34)!=cursor.tick || board.read_byte(script+0x38)!=cursor.key ||
                   board.read_dword(script+12)!=(result==2 ? 0x226378u : 0x2264f4u) ||
                   board.read_dword(0x501858)!=(result==2 ? 0x2865d58u : stage) || (result==2 && next!=1))
                    throw std::runtime_error("intro event timing/transition differs from original2264f4");
                ++ticks;if(result==2) break;
                if(tick==1099) throw std::runtime_error("intro events did not reach transition");
            }
            nm_intro_event_t invalid[]={{0,2,0,123,0},{0,2,(int16_t)count,0,0},{UINT32_MAX,0,0,0,0}};
            nm_intro_cursor_t bad_cursor={1,0};uint32_t next=123;
            const auto before=cars;
            if(nm_intro_events_step(invalid,3,cars.data(),count,&bad_cursor,&next) ||
               memcmp(cars.data(),before.data(),count*sizeof(cars[0])) || bad_cursor.tick!=1 || bad_cursor.key || next!=123)
                throw std::runtime_error("intro invalid event batch published partial state");
        }
    } catch(...) { restore();throw; }
    restore();
    if(!changed) throw std::runtime_error("intro events never changed speed");
    std::printf("native_intro_events PASS ticks=%u speed_changes=%u (two authored stages, whole2264f4, exact timing/transitions, atomic invalid batch)\n",ticks,changed);
}
inline void check_follow_camera(rt::M2Board &board,const char *path) {
    std::unique_ptr<nm_drive_params_t,decltype(&std::free)> p(nm_drive_load(path),&std::free);
    if (!p || !board.tgp().booted() || !board.tgp().fifo_out_empty())
        throw std::runtime_error("follow-camera oracle requires validated math and idle booted TGP");
    check_intro_camera(board,p.get(),0x2864030);
    check_intro_camera(board,p.get(),0x2864158);
    check_intro_speed(board,p.get());
    check_intro_correction(board);
    check_intro_events(board);
    if(board.read_dword(0x28b5734)!=0x41200000)
        throw std::runtime_error("follow-scenery fixed margin changed");
    constexpr uint32_t entity=0x5fc000, camera=0x5fd000;
    std::mt19937 random(0x43414d);
    float worst=0;
    for(unsigned i=0;i<1024;++i) {
        for(unsigned j=0;j<0x300;j+=4) {board.write_dword(entity+j,0);board.write_dword(camera+j,0);}
        nm_player_t car{};
        auto &g=car.boundary.ground;
        for(unsigned a=0;a<3;++a) g.position[a]=float(int(random()%8193)-4096)/8;
        g.pitch=int16_t(int(random()%8192)-4096);g.yaw=std::bit_cast<int16_t>(uint16_t(i*65535/1023));
        car.body_yaw=g.yaw; /* The board write below is entity+2a, which is body yaw (6544). */
        g.roll=int16_t(int(random()%10001)-5000);
        car.boundary.rotation=int16_t(int(random()%2001)-1000);
        car.shake_angles[0]=int16_t(int(random()%501)-250);
        for(unsigned a=0;a<3;++a) board.write_dword(entity+0x1c+4*a,std::bit_cast<uint32_t>(g.position[a]));
        board.write_word(entity+0x28,uint16_t(g.pitch));board.write_word(entity+0x2a,uint16_t(g.yaw));board.write_word(entity+0x2c,uint16_t(g.roll));
        board.write_word(entity+0xee,uint16_t(car.boundary.rotation));
        board.write_word(0x501834,uint16_t(car.shake_angles[0]));
        board.write_dword(0x5012f0,entity);board.write_dword(0x5014a4,0);
        board.write_byte(0x501710,2);board.write_byte(0x501460,0);
        for(unsigned a=0;a<6;++a) board.write_dword(0x501720+4*a,p->camera_reset_words[a]);
        for(unsigned a=0;a<3;++a) board.write_word(0x501738+2*a,uint16_t(p->camera_reset_angles[a]));
        rt::I960Ref cpu(&board);
        cpu.m_IP=0x6510;cpu.m_r[27]=0x880000;cpu.m_r[28]=0x4000;cpu.m_r[29]=camera;
        unsigned count=0;
        while(cpu.m_IP!=0x6b58) {
            if(++count>2048) throw std::runtime_error("follow-camera original instruction guard");
            cpu.execute_one();
        }
        float matrix[12];int16_t heading;
        if(!nm_follow_view(p.get(),&car,matrix,&heading) || uint16_t(heading)!=board.read_word(0x5014a8))
            throw std::runtime_error("native follow-camera rejected input or wrong visibility heading");
        uint16_t sky;
        if(!nm_follow_sky_scroll(p.get(),&car,&sky) || (sky|0x2000)!=board.read_word(0x50130a))
            throw std::runtime_error("native sky scroll differs from original66e4..67f0");
        for(unsigned a=0;a<12;++a) {
            const float expected=std::bit_cast<float>(board.read_dword(0x501560+4*a));
            const float scale=a<9 ? 1 : 1+fabsf(g.position[0])+fabsf(g.position[1])+fabsf(g.position[2]);
            const float error=fabsf(matrix[a]-expected)/scale;
            if(!std::isfinite(expected) || error>8*FLT_EPSILON) {
                std::fprintf(stderr,"camera case=%u component=%u actual=%g expected=%g scaled_error=%g\n",i,a,double(matrix[a]),double(expected),double(error));
                throw std::runtime_error("native follow-camera differs from original6510 i960/TGP");
            }
            if(error>worst) worst=error;
        }
        if(!board.tgp().fifo_out_empty()) throw std::runtime_error("follow-camera unread TGP output");
        board.write_dword(0x501240,camera);
        rt::I960Ref selection(&board);
        selection.m_IP=0x5890;selection.m_r[27]=0x880000;selection.m_r[28]=0x4000;selection.m_r[29]=0x5fe000;
        count=0;
        while(selection.m_IP!=0x16e58) {
            if(++count>256) throw std::runtime_error("follow-scenery original instruction guard");
            selection.execute_one();
        }
        selection.m_IP=0x16e10;
        while(selection.m_IP!=0x16e54) {
            if(++count>512) throw std::runtime_error("follow-scenery cell instruction guard");
            selection.execute_one();
        }
        float origin[2];uint16_t cell;
        if(!nm_follow_scenery_origin(p.get(),&car,heading,origin,&cell) ||
            std::bit_cast<uint32_t>(origin[0])!=board.read_dword(0x5fe01c) ||
            std::bit_cast<uint32_t>(origin[1])!=board.read_dword(0x5fe024) || cell!=board.read_word(0x5fe02e))
            throw std::runtime_error("follow-scenery XZ/cell differs from original5890/16e10");
        if(!board.tgp().fifo_out_empty()) throw std::runtime_error("follow-scenery unread TGP output");
    }
    std::printf("native_follow_camera PASS cases=1024 original6510..6b58 mode0/reset/VR3; max_scaled_error=%g (8*FLT_EPSILON); no runtime board\n",double(worst));
    std::puts("native_follow_scenery PASS cases=1024 original5890..596c/16e10: bit-exact XZ/cell, reset offset plus authored10 margin");
    std::puts("native_follow_sky PASS cases=1024 original66e4..67f0: exact vertical scroll, normal reset follow camera");
    unsigned sky_valid=0,sky_rejected=0;
    uint32_t sky_hash=2166136261u;
    for(unsigned i=0;i<65536;++i) {
        nm_player_t car{};
        car.boundary.ground.pitch=std::bit_cast<int16_t>(uint16_t(i));
        car.boundary.ground.position[1]=float(int(random()%8193)-4096)/8;
        if(!(i&8191)) nm_sky_test_case(i>>13,&car);
        board.write_dword(camera+0xb8,p->camera_reset_words[1]);
        board.write_dword(camera+0x20,std::bit_cast<uint32_t>(car.boundary.ground.position[1]));
        const int pitch=car.boundary.ground.pitch;
        board.write_word(camera+0x28,uint16_t(pitch<0 ? -((-pitch+1)/2) : pitch/2));
        board.write_word(camera+0x52,uint16_t(p->camera_reset_angles[0]));
        rt::I960Ref cpu(&board);cpu.m_IP=0x66e4;
        cpu.m_r[27]=0x880000;cpu.m_r[28]=0x4000;cpu.m_r[29]=camera;
        unsigned count=0;
        while(cpu.m_IP!=0x67dc) {
            if(++count>128) throw std::runtime_error("sky pitch oracle instruction guard");
            cpu.execute_one();
        }
        const float preconvert=std::bit_cast<float>(cpu.m_r[16]);
        const double rounded=std::round(double(preconvert));
        const bool valid=std::isfinite(rounded) && rounded>=-2147483648.0 && rounded<2147483648.0;
        uint16_t actual=0xabcd;
        const bool accepted=nm_follow_sky_scroll(p.get(),&car,&actual);
        if(accepted!=valid || (!valid && actual!=0xabcd))
            throw std::runtime_error("sky pitch conversion domain/output preservation differs");
        if(valid) {
            // Do not execute the interpreter's undefined out-of-range C++ cast.
            while(cpu.m_IP!=0x67f8) cpu.execute_one();
            if((actual|0x2000)!=board.read_word(0x50130a))
                throw std::runtime_error("sky pitch sweep differs from original vertical scroll");
            ++sky_valid;sky_hash=(sky_hash^actual)*16777619u;
        } else {
            ++sky_rejected;
            if(sky_rejected<=4) std::printf("native_follow_sky_rejected pitch=%d height=%g preconvert=%g bits=%08x\n",
                pitch,double(car.boundary.ground.position[1]),double(preconvert),std::bit_cast<uint32_t>(preconvert));
        }
        if(!board.tgp().fifo_out_empty()) throw std::runtime_error("sky pitch unread TGP output");
    }
    std::printf("native_follow_sky_pitch PASS cases=65536 valid=%u nonfinite/out-of-range=%u hash=%08x; all int16 pitch bits, reset camera, height[-512,512]\n",
        sky_valid,sky_rejected,sky_hash);
    std::printf("native_sky_startup_fixture original-verified cases=8 hash=%08x\n",nm_sky_test_hash(p.get()));
    if(nm_sky_test_hash(p.get())!=NM_SKY_TEST_HASH) throw std::runtime_error("sky startup fixture hash changed");
}
}
