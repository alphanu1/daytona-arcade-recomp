#pragma once
#include "check_road.hpp"
#include "../include/race.h"
#include "../include/intro.h"
#include "../include/finish_actor.h"
#include "../include/race_records.h"
#include "../include/race_scene.h"
#include "../include/result_actor.h"
#include "../include/name_entry.h"
#include "system24.hpp"

namespace daytona::native {
/* Destructive ONLY to this host diagnostic's final reference state, after
 * scenario metrics/checks. RAM restored between cases; never a target path. */
inline void check_finish_prefix(rt::M2Board &board, const nm_drive_params_t *math,
        const nm_contact_pack_t *contact, const char *road_file, const char *paths) {
    const uint32_t entity=board.read_dword(0x501220);
    if(entity<0x500000 || entity>0x5ffd00 || entity%16 || !board.tgp().booted())
        throw std::runtime_error("finish prefix oracle requires initialized live player/TGP");
    std::vector<uint32_t> saved(0x100000/4);
    for(unsigned i=0;i<saved.size();++i) saved[i]=board.read_dword(0x500000+4*i);
    const auto restore=[&] { for(unsigned i=0;i<saved.size();++i) board.write_dword(0x500000+4*i,saved[i]); };
    try {
        for(unsigned branch=0;branch<4;++branch) {
            restore();
            LivePlayerCheck check;check.load_road(road_file,paths);
            board.write_dword(0x501090,1u<<24);
            board.write_byte(0x5010a0,24);board.write_byte(0x5010a4,24);
            rt::I960Ref cpu(&board);
            cpu.m_r[27]=0x880000;cpu.m_r[28]=0x4000;cpu.m_r[29]=entity;cpu.m_r[25]=entity;
            cpu.m_IP=0xda58;
            unsigned entry_steps=0;
            while(cpu.m_IP!=0xda84) {
                if(++entry_steps>16) throw std::runtime_error("controlled finish entry instruction guard");
                cpu.execute_one();
            }
            const uint16_t heading=(uint16_t)(board.read_word(entity+0x1c2)+(branch==NM_FINISH_REVERSE ? 20000 : 0));
            board.write_word(entity+0x48,heading);board.write_word(entity+0x58,heading);
            board.write_word(entity+0x138,(uint16_t)((board.read_word(entity+0x138)&~1u)|(branch==NM_FINISH_GUIDED)));
            board.write_byte(entity+0xec,branch==3 ? 8 : 0);
            board.write_byte(entity+0x20d,5);
            unsigned total_steps=0;
            for(unsigned tick=0;tick<32;++tick) {
                check.begin(board,entity,math,contact,false,true);
                cpu.m_IP=0xdaac;cpu.m_r[25]=entity;
                unsigned steps=0;
                while(cpu.m_IP!=0xe078) {
                    if(++steps>100000) throw std::runtime_error("finish prefix instruction guard");
                    cpu.execute_one();
                }
                check.finish(board,entity);
                total_steps+=steps;
            }
            std::printf("native_finish_prefix controlled_branch=%u ticks=32 instructions=%u PASS (persistent player, whole entity/shared outputs; outer scheduling excluded)\n",branch,total_steps);
        }
    } catch(...) { restore();throw; }
    restore();
}

inline void check_result_label_sources(rt::M2Board &board,const std::string &colors_output,
        const std::string &name_colors_output) {
    /* Host-only shared5454 context. Save every region touched by loaders and
     * label draws; unresolved penalty art is NOT assumed to be blank. */
    const uint32_t bases[]={0x1080000,0x1800000,0x1000000,0x500000};
    const unsigned sizes[]={0x80000,0x4000,0x10000,0x100000};
    std::vector<uint32_t> saved[4];
    for(unsigned r=0;r<4;++r) for(unsigned i=0;i<sizes[r];i+=4)
        saved[r].push_back(board.read_dword(bases[r]+i));
    const auto restore=[&] {
        for(unsigned r=0;r<4;++r) for(unsigned i=0;i<saved[r].size();++i)
            board.write_dword(bases[r]+4*i,saved[r][i]);
    };
    try {
        for(unsigned r=0;r<3;++r) for(unsigned i=0;i<sizes[r];i+=4)
            board.write_dword(bases[r]+i,0);
        board.write_dword(0x50133c,0);
        const auto run=[&](uint32_t start,uint32_t stop,uint32_t g0,uint32_t g1) {
            rt::I960Ref cpu(&board);cpu.m_IP=start;cpu.m_r[16]=g0;cpu.m_r[17]=g1;
            unsigned steps=0;
            while(cpu.m_IP!=stop) {
                if(++steps>2000000) throw std::runtime_error("result label source instruction guard");
                cpu.execute_one();
            }
        };
        run(0x5458,0x54c4,0,0);
        const auto hash_bytes=[&](uint32_t base,unsigned count) {
            uint32_t h=UINT32_C(2166136261);
            for(unsigned i=0;i<count;++i) h=(h^board.read_byte(base+i))*UINT32_C(16777619);
            return h;
        };
        if(hash_bytes(0x1080000,0x80000)!=UINT32_C(0xf195da14) ||
           hash_bytes(0x1800000,0x4000)!=UINT32_C(0x4f6a7f5e) ||
           board.read_dword(0x501338)!=5 || board.read_dword(0x50133c)!=0x8000)
            throw std::runtime_error("offline result shared loader bytes/context differ");
        std::vector<uint32_t> blocks={0x2878912,0x287887e,0x2878596};
        for(unsigned rank=0;rank<42;++rank) for(unsigned lane=0;lane<3;++lane) {
            const uint32_t p=board.read_dword(0x23424c+16*rank+4*lane);
            if(p && std::find(blocks.begin(),blocks.end(),p)==blocks.end()) blocks.push_back(p);
        }
        uint32_t h=UINT32_C(2166136261);unsigned pixels=0;
        const auto sample=[&](unsigned w,unsigned height) {
            if(!w || !height || w>512 || height>512)
                throw std::runtime_error("result label dimensions exceed one source page");
            for(unsigned y=0;y<height;++y) for(unsigned x=0;x<w;++x) {
                uint16_t pen=board.video().system24_pixel(0,x,y);
                if(!(pen&15)) pen=0;
                h=(h^(pen&255))*UINT32_C(16777619);
                h=(h^(pen>>8))*UINT32_C(16777619);++pixels;
            }
        };
        for(uint32_t p:blocks) {
            run(0x18e2c,0x18ecc,p,0x1000000);
            /* Authored blocks are only16-bit aligned; the bus dword API
             * aligns DOWN, unlike the i960's unaligned load wrapper. */
            const uint32_t width=board.read_word(p+8)|(uint32_t(board.read_word(p+10))<<16);
            const uint32_t height=board.read_word(p+4)|(uint32_t(board.read_word(p+6))<<16);
            sample(width*8,height*8);
        }
        for(unsigned glyph:{0x20u,0x22u,0x27u,0x30u,0x31u,0x32u,0x33u,0x34u,0x35u,0x36u,0x37u,0x38u,0x39u}) {
            run(0x18414,0x18474,0x8000+glyph,0x1000000);sample(16,24);
        }
        if(pixels!=131648 || h!=UINT32_C(0x59e1d3f5))
            throw std::runtime_error("offline result label pens differ from original writers/Video");
        std::printf("native_result_label_sources PASS shared5454 bytes,30 authored pen planes131648pixels hash=%08x; penalty art and scene RGB context pending\n",h);
        /* Original normal-result layout incl real rank/time callees, then the
         * actual register publisher. Page1 is explicitly blank in this proof;
         * inherited scene ownership is a separate integration obligation. */
        run(0x1a240,0x1a2b0,0,0);
        for(unsigned i=0;i<0x2000;i+=4) board.write_dword(0x1002000+i,0x00200020);
        board.write_dword(0x501220,0x5e0000);board.write_byte(0x5e006a,30);
        board.write_word(0x5e0064,4660);board.write_dword(0x5010cc,1237);
        board.write_dword(0x501a80,0);board.write_word(0x501320,0);
        run(0xb510,0xb5d0,0,0);
        std::vector<int> label_page(512*384);
        for(unsigned y=0;y<384;++y) for(unsigned x=0;x<512;++x) {
            const unsigned pen=board.video().system24_pixel(0,x,y);
            label_page[y*512+x]=(pen&15) ? (int)pen : -1;
        }
        unsigned scroll_pixels=0;uint32_t scroll_hash=UINT32_C(2166136261);
        int16_t scroll=512;unsigned mode=1;
        for(unsigned tick=0;tick<33;++tick) {
            if(tick) nm_result_wait_step(&mode,800-(int32_t)tick,&scroll);
            board.write_word(0x501300,(uint16_t)scroll);board.write_word(0x501302,0x4000);
            run(0x1a0c4,0x1a19c,0,0);
            for(unsigned y=0;y<384;++y) for(unsigned x=0;x<496;++x) {
                const int expected=x<(unsigned)scroll ? -1 : label_page[y*512+x-scroll];
                const int actual=daytona::system24_layer_pen(board.video(),1,false,(int)x,(int)y);
                if(actual!=expected) throw std::runtime_error("result label slide differs from System24 split mode");
                scroll_hash=(scroll_hash^(uint32_t)actual)*UINT32_C(16777619);++scroll_pixels;
            }
        }
        if(scroll || mode!=1) throw std::runtime_error("result scroll does not settle in32 ticks");
        std::printf("native_result_label_scroll PASS rank30 originalb510/1a0c4,33positions pixels=%u hash=%08x; translate+x, no alpha fade, blank page1 assumed\n",scroll_pixels,scroll_hash);
        const auto export_colors=[&](const std::string &path,const char *context) {
            if(path.empty()) return;
            if(std::filesystem::exists(path)) throw std::runtime_error("colour output already exists");
            std::ofstream out;out.exceptions(std::ios::failbit|std::ios::badbit);out.open(path);
            out << "# " << context << " palette, caller colour-translation/gamma context; NOT full scene proof\n";
            for(unsigned i=0;i<4096;++i)
                out << i << ' ' << board.read_word(0x1800000+2*i) << ' ' << board.video().system24_pen(i) << '\n';
            out.close();
        };
        export_colors(colors_output,"shared5454");
        // Independently execute the shared loader and phase29's scene overlay,
        // then original18e2c for the static blocks referenced by227a4c. No RGB
        // or complete scene proof: dynamic text and the3D name actor are separate.
        for(unsigned r=0;r<3;++r) for(unsigned i=0;i<sizes[r];i+=4)
            board.write_dword(bases[r]+i,0);
        board.write_dword(0x50133c,0);
        run(0x5458,0x54c4,0,0);
        run(0x5460,0x54c4,board.read_dword(0x2800014),0);
        if(hash_bytes(0x1080000,0x80000)!=UINT32_C(0x40bb0775) ||
           hash_bytes(0x1800000,0x4000)!=UINT32_C(0x178be191))
            throw std::runtime_error("offline name scene loader bytes differ");
        h=UINT32_C(2166136261);pixels=0;
        for(uint32_t p:{0x2883100u,0x288484cu,0x2884a0au,0x2884a4au}) {
            run(0x18e2c,0x18ecc,p,0x1000000);
            const uint32_t width=board.read_word(p+8)|(uint32_t(board.read_word(p+10))<<16);
            const uint32_t height=board.read_word(p+4)|(uint32_t(board.read_word(p+6))<<16);
            sample(width*8,height*8);
        }
        if(pixels!=207808 || h!=UINT32_C(0xd845cabe))
            throw std::runtime_error("offline name pens differ from original writers/Video");
        std::printf("native_name_label_sources PASS original5454/5460/18e2c: four static planes207808pixels hash=%08x; RGB/dynamic labels/3D excluded\n",h);
        export_colors(name_colors_output,"name5454+5460");
    } catch(...) { restore();throw; }
    restore();
}

inline void check_race(rt::M2Board &board,const std::string &label_colors={},const std::string &name_colors={}) {
    check_result_label_sources(board,label_colors,name_colors);
    {
        // Only these registers are accessed by227764/227820; intercepted
        // sound and finish callees are explicit intents, not silent omissions.
        constexpr uint32_t base=0x501380;
        std::array<uint32_t,16> saved{};
        for(unsigned i=0;i<saved.size();++i) saved[i]=board.read_dword(base+4*i);
        const uint32_t steering_saved=board.read_dword(0x501050),edge_saved=board.read_dword(0x501068);
        const auto restore=[&] {
            for(unsigned i=0;i<saved.size();++i) board.write_dword(base+4*i,saved[i]);
            board.write_dword(0x501050,steering_saved);board.write_dword(0x501068,edge_saved);
        };
        unsigned cases=0,finishes=0;uint32_t hash=UINT32_C(2166136261);
        try {
            for(unsigned i=0;i<29;++i) {
                const uint8_t glyph=i<26 ? 'A'+i : i==26 ? '.' : i==27 ? '}' : '{';
                if(board.read_byte(0x23b564+i)!=glyph) throw std::runtime_error("name alphabet differs");
                if(i<26 && board.read_dword(0x23bc9c+4*i)!=0xae2139+i)
                    throw std::runtime_error("name letter announcement differs");
            }
            for(unsigned raw=0;raw<256;++raw) for(unsigned count=0;count<4;++count)
            for(uint32_t edge:{0u,1u,UINT32_MAX}) {
                for(unsigned i=0;i<saved.size();++i) board.write_dword(base+4*i,0xa5a5a5a5);
                nm_name_entry_t s={.index=(raw+count)%29,.previous=123,.count=count,.name={'A','B','C',0x5a},.glyph='A'};
                board.write_dword(0x50139c,s.index);board.write_dword(0x501394,s.previous);
                board.write_dword(0x5013ac,count);board.write_byte(0x5013b8,s.glyph);
                for(unsigned i=0;i<4;++i) board.write_byte(0x5013a0+i,s.name[i]);
                board.write_dword(0x501050,raw);board.write_dword(0x501068,edge);
                std::array<uint32_t,16> expected{};
                for(unsigned i=0;i<expected.size();++i) expected[i]=board.read_dword(base+4*i);
                uint32_t selected_command=0,accept_command=0;
                if(!nm_name_select(&s,std::bit_cast<int8_t>((uint8_t)raw),&selected_command))
                    throw std::runtime_error("name steering rejected valid input");
                const int result=nm_name_accept(&s,edge,&accept_command);
                if(!result) throw std::runtime_error("name acceptance rejected valid input");
                expected[(0x501394-base)/4]=s.previous;expected[(0x50139c-base)/4]=s.index;
                expected[(0x5013ac-base)/4]=s.count;
                memcpy(&expected[(0x5013a0-base)/4],s.name,4);
                expected[(0x5013b8-base)/4]=(expected[(0x5013b8-base)/4]&0xffffff00u)|s.glyph;
                rt::I960Ref cpu(&board);cpu.m_IP=0x227764;
                unsigned steps=0,finish_calls=0;std::vector<uint32_t> commands;
                for(uint32_t stop:{0x22781cu,0x227904u}) {
                    while(cpu.m_IP!=stop) {
                        if(++steps>100) throw std::runtime_error("name input source guard");
                        if(cpu.m_IP==0x227804 || cpu.m_IP==0x227878 || cpu.m_IP==0x227888 || cpu.m_IP==0x2278d8) {
                            commands.push_back(cpu.m_r[16]);cpu.m_IP+=4;continue;
                        }
                        if(cpu.m_IP==0x227900) { ++finish_calls;cpu.m_IP+=4;continue; }
                        cpu.execute_one();
                    }
                    cpu.m_IP=0x227820;
                }
                std::vector<uint32_t> expected_commands;
                if(selected_command) expected_commands.push_back(selected_command);
                if(accept_command) expected_commands.push_back(accept_command);
                if(commands!=expected_commands || finish_calls!=(result==2)) throw std::runtime_error("name input intents differ");
                for(unsigned i=0;i<expected.size();++i) {
                    const uint32_t actual=board.read_dword(base+4*i);
                    if(actual!=expected[i]) throw std::runtime_error("name input state differs raw="+std::to_string(raw));
                    hash=(hash^actual)*UINT32_C(16777619);
                }
                finishes+=finish_calls;++cases;
            }
        } catch(...) { restore();throw; }
        restore();
        std::printf("native_name_input cases=%u finish_requests=%u hash=%08x PASS original227764/227820: all signed steering bytes/lengths/edges, alphabet/audio table, exact64-byte globals; finish callee/presentation excluded\n",cases,finishes,hash);
    }
    {
        nm_name_rules_t rules{};
        for(unsigned i=0;i<40;++i) {
            uint32_t name=0;
            for(unsigned b=0;b<3;++b) name|=(uint32_t)board.read_byte(0x23bda8+3*i+b)<<(8*b);
            rules.rules[i]={name,board.read_dword(0x23bd04+4*i),board.read_dword(0x23be24+4*i)};
        }
        for(unsigned i=0;i<2;++i) for(unsigned j=0;j<4;++j) for(unsigned b=0;b<3;++b)
            rules.replacements[i][j]|=(uint32_t)board.read_byte(board.read_dword(0x23bec8+4*i)+3*j+b)<<(8*b);
        rules.default_command=board.read_dword(0x23bec4);
        std::vector<uint32_t> saved(0x100000/4);
        for(unsigned i=0;i<saved.size();++i) saved[i]=board.read_dword(0x500000+4*i);
        const auto restore=[&] { for(unsigned i=0;i<saved.size();++i) board.write_dword(0x500000+4*i,saved[i]); };
        unsigned cases=0;uint32_t hash=UINT32_C(2166136261);
        try {
            for(unsigned index=0;index<=40;++index) for(unsigned frame=0;frame<4;++frame)
            for(uint32_t notification:{0u,1u,2u,UINT32_MAX}) {
                restore();auto expected=saved;
                const auto set=[&](uint32_t address,uint32_t value) {
                    board.write_dword(address,value);expected[(address-0x500000)/4]=value;
                };
                const uint32_t raw=(index<40 ? rules.rules[index].name : 0)|UINT32_C(0x5a000000);
                constexpr uint32_t actor=0x5f0000,selected=0x5f0100,record=0x5f0200;
                set(0x5013a0,raw);set(0x5013a4,0xa5a5a5a5);set(0x5013a8,0xa5a5a5a5);
                set(0x5010b0,frame);set(0x5010a8,UINT32_MAX);set(0x501398,notification);
                set(0x5013b4,selected);set(selected,record);set(record+20,0x12345678);
                set(actor+0x34,0xa5a5a5a5);
                nm_name_entry_t name{};memcpy(name.name,&raw,4);
                nm_name_commit_t result{};std::fill(std::begin(result.display),std::end(result.display),0xa5);
                uint32_t record_name=0x12345678;
                if(!nm_name_commit(&name,&rules,frame,notification,&record_name,&result))
                    throw std::runtime_error("name commit rejected valid source input");
                memcpy(&expected[0x13a0/4],name.name,4);
                memcpy((uint8_t *)&expected[0x13a4/4],result.display,7);
                expected[0x10a8/4]=result.timer;
                expected[(record+20-0x500000)/4]=record_name;
                expected[(actor+0x34-0x500000)/4]=(expected[(actor+0x34-0x500000)/4]&0xff00ffffu)|((uint32_t)result.phase<<16);
                rt::I960Ref cpu(&board);cpu.m_IP=0x227b1c;cpu.m_r[29]=actor;
                unsigned steps=0,calls=0;uint32_t command=0;
                while(cpu.m_IP!=0x2279b0 && cpu.m_IP!=0x2279c8) {
                    if(++steps>1000) throw std::runtime_error("name commit source guard");
                    if(cpu.m_IP==0x227c04) { cpu.m_IP=0x227938;continue; }
                    if(cpu.m_IP==0x2279ac || cpu.m_IP==0x2279c4) {
                        command=cpu.m_r[16];++calls;cpu.m_IP+=4;continue;
                    }
                    cpu.execute_one();
                }
                if(calls!=1 || command!=result.command) throw std::runtime_error("name commit silent command differs");
                for(unsigned i=0;i<expected.size();++i)
                    if(board.read_dword(0x500000+4*i)!=expected[i]) throw std::runtime_error("name commit RAM differs index="+std::to_string(index));
                for(uint32_t word:{expected[0x13a0/4],expected[0x13a4/4],expected[0x13a8/4],result.timer,record_name,(uint32_t)result.phase,command})
                    hash=(hash^word)*UINT32_C(16777619);
                ++cases;
            }
        } catch(...) { restore();throw; }
        restore();
        std::printf("native_name_commit cases=%u hash=%08x PASS original227934+227b1c:40 specials/default, four substitutions, notification bit0, byte3/display preservation/full RAM; audio is silent intent\n",cases,hash);
    }
    /* Formatting only: intercept the eight glyph draws, not the arithmetic.
     * Includes every raw signed16 lap input and wider total-time samples. */
    uint32_t time_hash=UINT32_C(2166136261),random_time=0;
    for(unsigned sample=0;sample<81920;++sample) {
        uint32_t ticks;
        if(sample<65536) ticks=sample<32768 ? sample : UINT32_C(0xffff0000)|sample;
        else { random_time=random_time*UINT32_C(1664525)+UINT32_C(1013904223);ticks=random_time; }
        uint16_t glyphs[8];nm_result_time_glyphs(ticks,glyphs);
        rt::I960Ref cpu(&board);cpu.m_IP=0x18544;cpu.m_r[16]=ticks;cpu.m_r[17]=0x1001416;
        unsigned steps=0,draws=0;
        while(cpu.m_IP!=0x1865c) {
            if(++steps>100) throw std::runtime_error("result time formatter instruction guard");
            switch(cpu.m_IP) {
            case 0x18588: case 0x185a4: case 0x185b4: case 0x185d0:
            case 0x185ec: case 0x185fc: case 0x18618: case 0x1863c: case 0x18650:
                if(draws>=8 || cpu.m_r[16]!=glyphs[7-draws] || cpu.m_r[17]!=0x1001416+28-4*draws)
                    throw std::runtime_error("result time glyph or placement differs");
                time_hash=(time_hash^cpu.m_r[16])*UINT32_C(16777619);
                ++draws;cpu.m_IP+=4;break;
            default: cpu.execute_one();break;
            }
        }
        if(draws!=8 || cpu.m_r[16]!=ticks || cpu.m_r[17]!=0x1001416)
            throw std::runtime_error("result time glyph count/register preservation differs");
    }
    std::printf("native_result_time cases=81920 PASS original18544: glyphs/positions/input restoration, signed16 laps and uint32 totals; digest=%08x\n",time_hash);
    // Characterize the real finish actor before importing its presentation.
    // Execute original arithmetic/TGP; skip ONLY the four geometry callees.
    // This is an oracle, never a target animation or recorded-frame asset.
    {
        if(!board.tgp().booted()) throw std::runtime_error("finish actor requires booted TGP");
        constexpr uint32_t actor=0x5f0000;
        std::vector<uint32_t> saved(0x100000/4);
        for(unsigned i=0;i<saved.size();++i) saved[i]=board.read_dword(0x500000+4*i);
        const auto restore=[&] { for(unsigned i=0;i<saved.size();++i) board.write_dword(0x500000+4*i,saved[i]); };
        try {
            unsigned clock_cases=0;
            for(uint32_t clock : {0u,999u,1000u,0x7fffffffu,0x80000000u,0xffffffffu})
            for(int32_t timer : {INT32_MIN,-1,0,191,192,193,576,INT32_MAX})
            for(uint32_t input : {0u,1u,16u,0xffffffffu}) {
                restore();
                board.write_dword(actor+0x7c,clock);
                board.write_dword(0x5010a8,(uint32_t)timer);
                board.write_dword(0x53e120,input);
                uint32_t next_clock=clock;int32_t next_timer=timer;
                nm_result_clock_step(&next_clock,&next_timer,(input&16)!=0);
                rt::I960Ref tail(&board);tail.m_IP=0xb660;tail.m_r[29]=actor;
                unsigned steps=0;
                while(tail.m_IP!=0xb6a0) {
                    if(++steps>20) throw std::runtime_error("result clock tail instruction guard");
                    tail.execute_one();
                }
                for(unsigned i=0;i<saved.size();++i) {
                    const uint32_t at=0x500000+4*i;
                    const uint32_t expected=at==actor+0x7c ? next_clock :
                        at==0x5010a8 ? (uint32_t)next_timer : at==0x53e120 ? input : saved[i];
                    if(board.read_dword(at)!=expected)
                        throw std::runtime_error("result clock/skip tail changed unexpected RAM");
                }
                ++clock_cases;
            }
            restore();
            std::printf("native_result_clock cases=%u PASS originalb660..b6a0: signed192 threshold, held bit4, clock wrap, full RAM; scene decrement excluded\n",clock_cases);
            unsigned announcement_cases=0;uint32_t announcement_hash=UINT32_C(2166136261);
            for(unsigned rank=0;rank<44;++rank) {
                restore();
                constexpr uint32_t player=0x5e0000;
                board.write_dword(0x501220,player);board.write_byte(player+0x6a,(uint8_t)rank);
                const uint32_t address=board.read_dword(0x2344fc+4*rank);
                nm_result_announcement_t rows[4];unsigned count=0;
                do {
                    rows[count]={(int32_t)board.read_dword(address+8*count),board.read_dword(address+8*count+4)};
                    ++count;
                } while(count<4 && rows[count-1].countdown!=-64);
                if(rows[count-1].countdown!=-64) throw std::runtime_error("result rank terminal row missing");
                uint32_t cursor=0;
                const auto check=[&](int32_t timer,uint32_t penalty) {
                    board.write_dword(actor+0x34,cursor);board.write_dword(0x5010a8,(uint32_t)timer);
                    board.write_dword(0x501a80,penalty);
                    uint32_t command=0x12345678,original=0;
                    if(!nm_result_announcement_step(rows,count,penalty,timer,&cursor,&command))
                        throw std::runtime_error("native result announcement rejected");
                    rt::I960Ref cpu(&board);cpu.m_IP=0xb484;cpu.m_r[29]=actor;
                    unsigned steps=0,calls=0;
                    while(cpu.m_IP!=0xb4d8) {
                        if(++steps>24) throw std::runtime_error("result announcement instruction guard");
                        if(cpu.m_IP==0xb4cc) { ++calls;original=cpu.m_r[16];cpu.m_IP+=4;continue; }
                        cpu.execute_one();
                    }
                    if(calls>1 || command!=original || cursor!=board.read_dword(actor+0x34))
                        throw std::runtime_error("result rank announcement differs");
                    ++announcement_cases;return command;
                };
                for(int32_t timer=800;timer>=576;--timer) {
                    const uint32_t command=check(timer,0);
                    announcement_hash=(announcement_hash^command)*UINT32_C(16777619);
                    announcement_hash=(announcement_hash^cursor)*UINT32_C(16777619);
                }
                for(unsigned row=0;row<count;++row)
                    for(int32_t timer : {rows[row].countdown-1,rows[row].countdown,rows[row].countdown+1,INT32_MIN,INT32_MAX})
                        for(uint32_t penalty : {0u,1u,UINT32_MAX}) { cursor=row;check(timer,penalty); }
            }
            restore();
            std::printf("native_result_announcements cases=%u ranks=44 PASS originalb484: exact equality/one command/cursor/penalty; 9900tick digest=%08x\n",announcement_cases,announcement_hash);
            unsigned entry_cases=0;
            for(int rank : {-128,-1,0,2,3,127})
            for(uint32_t penalty : {0u,1u,UINT32_MAX})
            for(int special : {0,1,-32768,32767}) {
                restore();
                constexpr uint32_t player=0x5e0000;
                board.write_dword(0x501220,player);board.write_byte(player+0x6a,(uint8_t)rank);
                board.write_dword(0x501a80,penalty);board.write_word(0x50132c,(uint16_t)special);
                nm_result_playback_t native{};nm_result_vec2_t initial[8];
                native.clock=91;native.cursor=9;native.target=7;
                for(unsigned i=0;i<8;++i) {
                    native.actor.other_angle[i]=(int16_t)(i*8191u);
                    initial[i]={nm_force_float(board.read_dword(0x2327ec+8*i)),
                        nm_force_float(board.read_dword(0x2327f0+8*i))};
                    board.write_word(actor+0x94+2*i,(uint16_t)native.actor.other_angle[i]);
                }
                const int mode=nm_result_playback_init(&native,initial,(int8_t)rank,penalty,(int16_t)special);
                rt::I960Ref entry(&board);entry.m_IP=0xb268;entry.m_r[29]=actor;
                unsigned steps=0,audio_calls=0,ui_resets=0;
                while(entry.m_IP!=0xb410) {
                    if(++steps>250) throw std::runtime_error("result entry instruction guard");
                    if(entry.m_IP==0xb324) { ++ui_resets;entry.m_IP+=4;continue; }
                    if((entry.m_IP>=0xb364 && entry.m_IP<=0xb3c4 && (entry.m_IP-0xb364)%12==0) ||
                        entry.m_IP==0xb3f4 || entry.m_IP==0xb400 || entry.m_IP==0xb40c) {
                        if(entry.m_r[16]!=nm_result_entry_command((unsigned)mode,audio_calls))
                            throw std::runtime_error("result entry command order differs");
                        ++audio_calls;entry.m_IP+=4;continue;
                    }
                    entry.execute_one();
                }
                if(board.read_dword(actor+12)!=(mode==1 ? 0xb414u : 0xb4dcu) ||
                   board.read_dword(actor+0x78)!=0x23282c || board.read_dword(actor+0x7c)!=native.clock ||
                   board.read_dword(actor+0x80)!=native.cursor || native.target ||
                   board.read_dword(actor+0x34) || board.read_dword(player+0x18) ||
                   ui_resets!=(mode==1) || audio_calls!=(mode==1 ? 12u : 3u) ||
                   nm_result_entry_command((unsigned)mode,audio_calls))
                    throw std::runtime_error("result entry mode/counters/effects differ");
                for(unsigned i=0;i<8;++i)
                    if(board.read_dword(actor+0x38+8*i)!=nm_force_bits(native.actor.position[i].x) ||
                       board.read_dword(actor+0x3c+8*i)!=nm_force_bits(native.actor.position[i].y) ||
                       board.read_word(actor+0x84+2*i)!=(uint16_t)native.actor.angle[i] ||
                       board.read_word(actor+0x94+2*i)!=(uint16_t)native.actor.other_angle[i])
                        throw std::runtime_error("result entry initial/inherited lanes differ");
                ++entry_cases;
            }
            restore();
            unsigned wait_cases=0;
            for(unsigned initial_mode : {1u,2u})
            for(int32_t timer : {INT32_MIN,-1,0,575,576,577,800,INT32_MAX})
            for(int value : {-32768,-1,0,15,16,17,32767}) {
                restore();
                unsigned mode=initial_mode;int16_t fade=(int16_t)value;
                const unsigned effects=nm_result_wait_step(&mode,timer,&fade);
                const uint32_t start=initial_mode==1 ? 0xb414 : 0xb4dc;
                board.write_dword(actor+12,start);board.write_dword(0x5010a8,(uint32_t)timer);
                board.write_word(0x501300,(uint16_t)value);
                rt::I960Ref wait(&board);wait.m_IP=start;wait.m_r[29]=actor;
                unsigned steps=0,actual_effects=0;
                while(wait.m_IP!=0xb480 && wait.m_IP!=0xb50c) {
                    if(++steps>40) throw std::runtime_error("result wait instruction guard");
                    if(wait.m_IP==0xb43c || wait.m_IP==0xb504) {
                        if(wait.m_r[16]!=0xae1002) throw std::runtime_error("result start sound differs");
                        actual_effects|=4;wait.m_IP+=4;continue;
                    }
                    if(wait.m_IP==0xb478) { actual_effects|=1;wait.m_IP+=4;continue; }
                    if(wait.m_IP==0xb47c || wait.m_IP==0xb508) { actual_effects|=2;wait.m_IP+=4;continue; }
                    wait.execute_one();
                }
                if(effects!=actual_effects || board.read_word(0x501300)!=(uint16_t)fade ||
                   board.read_dword(actor+12)!=(mode==3 ? 0xb654u : start) ||
                   board.read_dword(0x5010a8)!=(uint32_t)timer)
                    throw std::runtime_error("result waiting effects/fade/next-mode differ");
                ++wait_cases;
            }
            restore();
            std::printf("native_result_entry cases=%u wait_cases=%u PASS originalb268..b410/b414/b4dc: animation state/inherited lanes/branch/fade/callback intents; UI/audio callees excluded\n",entry_cases,wait_cases);
            // Authored result animation targets, both ROM timelines, persistent
            // state across450 ticks. Execute ALL position/angle math on TGP.
            unsigned result_ticks=0;uint32_t result_hash=UINT32_C(2166136261);
            unsigned result_matrices=0;float result_matrix_error=0;
            for(unsigned variant=0;variant<2;++variant) {
                restore();
                nm_result_actor_t state{};
                for(unsigned i=0;i<8;++i) {
                    state.position[i].x=nm_force_float(board.read_dword(0x2327ec+8*i));
                    state.position[i].y=nm_force_float(board.read_dword(0x2327f0+8*i));
                    state.other_angle[i]=(int16_t)(i*8191u); // Entry retains second angle array.
                }
                nm_result_key_t keys[21];
                for(unsigned i=0;i<21;++i) {
                    const uint32_t row=(variant ? 0x232744 : 0x23269c)+8*i;
                    keys[i]={(int32_t)board.read_dword(row),board.read_dword(row+4)};
                }
                uint32_t cursor=0,target_address=0x23282c;
                board.write_word(0x50132c,(uint16_t)variant);
                const auto select=[&](int32_t clock) {
                    board.write_dword(actor+0x78,target_address);
                    board.write_dword(actor+0x7c,(uint32_t)clock);
                    board.write_dword(actor+0x80,cursor);
                    if(!nm_result_target_select(keys,21,clock,&cursor,&target_address))
                        throw std::runtime_error("native result selector rejected valid key");
                    rt::I960Ref scheduler(&board);scheduler.m_IP=0xb6a4;scheduler.m_r[29]=actor;
                    unsigned steps=0;
                    while(scheduler.m_IP!=0xb6dc && scheduler.m_IP!=0xb6f8 && scheduler.m_IP!=0xb708) {
                        if(++steps>32) throw std::runtime_error("result selector instruction guard");
                        scheduler.execute_one();
                    }
                    if(board.read_dword(actor+0x78)!=target_address || board.read_dword(actor+0x80)!=cursor)
                        throw std::runtime_error("native result selector differs");
                };
                for(unsigned key=0;key<21;++key)
                    for(int32_t clock : {keys[key].tick-1,keys[key].tick,keys[key].tick+1,INT32_MIN,INT32_MAX}) {
                        cursor=key;target_address=0x23282c;select(clock);
                    }
                cursor=0;target_address=0x23282c;
                for(unsigned tick=0;tick<450;++tick) {
                    select((int32_t)tick);
                    nm_result_target_t targets[8];
                    for(unsigned i=0;i<8;++i) {
                        const uint32_t row=target_address+16*i;
                        targets[i].position.x=nm_force_float(board.read_dword(row));
                        targets[i].position.y=nm_force_float(board.read_dword(row+4));
                        targets[i].rate=nm_force_float(board.read_dword(row+8));
                        targets[i].angle=(int16_t)board.read_word(row+12);
                        targets[i].other_angle=(int16_t)board.read_word(row+14);
                        board.write_dword(actor+0x38+8*i,nm_force_bits(state.position[i].x));
                        board.write_dword(actor+0x3c+8*i,nm_force_bits(state.position[i].y));
                        board.write_word(actor+0x84+2*i,(uint16_t)state.angle[i]);
                        board.write_word(actor+0x94+2*i,(uint16_t)state.other_angle[i]);
                    }
                    board.write_dword(actor+0x78,target_address);
                    if(!nm_result_actor_step(&state,targets)) throw std::runtime_error("result animation rejected authored data");
                    rt::I960Ref result(&board);result.m_IP=0xb70c;
                    result.m_r[27]=0x880000;result.m_r[28]=0x4000;result.m_r[29]=actor;
                    unsigned steps=0;
                    while(result.m_IP!=0xb848) {
                        if(++steps>1500) throw std::runtime_error("result animation instruction guard");
                        result.execute_one();
                    }
                    for(unsigned i=0;i<8;++i)
                        if(board.read_dword(actor+0x38+8*i)!=nm_force_bits(state.position[i].x) ||
                           board.read_dword(actor+0x3c+8*i)!=nm_force_bits(state.position[i].y) ||
                           board.read_word(actor+0x84+2*i)!=(uint16_t)state.angle[i] ||
                           board.read_word(actor+0x94+2*i)!=(uint16_t)state.other_angle[i])
                            throw std::runtime_error("native result animation differs variant="+std::to_string(variant)+
                                " tick="+std::to_string(tick)+" part="+std::to_string(i));
                    // Execute the real draw transform chain, skipping ONLY the
                    // geometry callee. Independent analytic oracle establishes
                    // axes/order independently of the target SH4ZAM chain.
                    result.m_IP=0xb84c;steps=0;unsigned draws=0;
                    while(result.m_IP!=0xb944) {
                        if(++steps>600) throw std::runtime_error("result draw instruction guard");
                        if(result.m_IP!=0xb910) { result.execute_one();continue; }
                        if(draws>=8 || result.m_r[16]!=board.read_dword(0x23267c+4*draws))
                            throw std::runtime_error("result model draw order differs");
                        const float unit=6.2831853071795864769f/65536.0f;
                        const float y=state.other_angle[draws]*unit,z=state.angle[draws]*unit;
                        const float cy=cosf(y),sy=sinf(y),cz=cosf(z),sz=sinf(z);
                        const float expected[12]={1.2f*cy*cz,-1.2f*sz,1.2f*sy*cz,
                            1.2f*cy*sz,1.2f*cz,1.2f*sy*sz,-1.2f*sy,0,1.2f*cy,
                            state.position[draws].x,-0.01f,state.position[draws].y};
                        board.tgp().function_port_w(0x11*4,0x1111);
                        for(unsigned j=0;j<12;++j) {
                            const float original=nm_force_float(board.tgp().fifo_r());
                            const float error=fabsf(original-expected[j]);
                            result_matrix_error=std::max(result_matrix_error,error);
                            if(!std::isfinite(original) || (j<9 ? error>1e-5f :
                                    nm_force_bits(original)!=nm_force_bits(expected[j])))
                                throw std::runtime_error("result matrix differs tick="+std::to_string(tick)+
                                    " part="+std::to_string(draws)+" element="+std::to_string(j));
                        }
                        ++draws;++result_matrices;result.m_IP+=4;
                    }
                    if(draws!=8) throw std::runtime_error("result draw count differs");
                    ++result_ticks;
                    for(unsigned i=0;i<8;++i)
                        for(uint32_t value : {board.read_dword(actor+0x38+8*i),board.read_dword(actor+0x3c+8*i),
                                (uint32_t)board.read_word(actor+0x84+2*i),(uint32_t)board.read_word(actor+0x94+2*i)})
                            result_hash=(result_hash^value)*UINT32_C(16777619);
                }
                for(unsigned i=0;i<saved.size();++i) {
                    const uint32_t at=0x500000+4*i;
                    if((at>=actor+0x38 && at<actor+0x78) ||
                       (at>=actor+0x84 && at<actor+0xa4)) continue;
                    const uint32_t expected=at==actor+0x78 ? target_address :
                        at==actor+0x7c ? 449 : at==actor+0x80 ? cursor :
                        at==0x50132c ? (saved[i]&0xffff0000u)|variant : saved[i];
                    if(board.read_dword(at)!=expected)
                        throw std::runtime_error("result animation changed unrelated main RAM");
                }
            }
            restore();
            std::printf("native_result_animation ticks=%u parts=8 selector_edges=210 PASS originalb6a4+b70c..b848: signed strict threshold/one-key step, both authored timelines, persistent XY and angles; rendering/outer actor excluded\n",result_ticks);
            std::printf("native_result_animation original digest=%08x\n",result_hash);
            std::printf("native_result_matrices count=%u max_error=%g PASS originalb84c..b944: T*S(1.2)*Ry(other)*Rz(angle), exact translation/model order; geometry callee excluded\n",
                result_matrices,result_matrix_error);
            // Full record reset body, excluding226654's separate lap records.
            // Snapshot backup RAM as well: the tables live outside main RAM.
            const auto backup=board.backup_ram();
            auto expected_backup=backup;
            rt::I960Ref seed_reader(&board);
            for(unsigned course=0;course<3;++course) {
                const uint32_t source=board.read_dword(0x2266e0+8*course);
                const uint32_t target=board.read_dword(0x2266e4+8*course);
                nm_race_record_seed_t seed[24];nm_race_record_t table[24];
                for(unsigned i=0;i<24;++i) {
                    const uint32_t rank=board.read_byte(source+12*i);
                    seed[i]={rank,board.read_dword(0x2266d0+4*course)+(rank-1)*320u,
                        seed_reader.i960_read_dword_unaligned(source+12*i+8),
                        seed_reader.i960_read_dword_unaligned(source+12*i+4)};
                    table[i]={0xa5a5a5a5u,0xa5a5a5a5u,0x12340000u+i,
                        0xa5a5a5a5u,0xa5a5a5a5u,0xa5a5a5a5u};
                    memcpy(board.backup_ram().data()+target-0x1d00000+24*i,table+i,24);
                }
                nm_race_records_reset(table,seed);
                memcpy(expected_backup.data()+target-0x1d00000,table,sizeof(table));
            }
            try {
                rt::I960Ref reset(&board);reset.m_IP=0x2265d0;
                std::array<uint32_t,16> caller{};
                unsigned steps=0;
                while(reset.m_IP!=0x226634) {
                    if(++steps>3000) throw std::runtime_error("record reset instruction guard");
                    if(reset.m_IP==0x226600) {
                        std::copy_n(reset.m_r,16,caller.begin());reset.m_IP=0x226638;continue;
                    }
                    if(reset.m_IP==0x226650) {
                        std::copy(caller.begin(),caller.end(),reset.m_r);reset.m_IP=0x226604;continue;
                    }
                    reset.execute_one();
                }
                if(board.backup_ram()!=expected_backup) {
                    for(unsigned i=0;i<expected_backup.size();++i)
                        if(board.backup_ram()[i]!=expected_backup[i])
                            throw std::runtime_error("native record reset differs offset="+std::to_string(i)+
                                " expected="+std::to_string(expected_backup[i])+" actual="+
                                std::to_string(board.backup_ram()[i]));
                }
                for(unsigned i=0;i<saved.size();++i)
                    if(board.read_dword(0x500000+4*i)!=saved[i])
                        throw std::runtime_error("record reset changed main RAM");
            } catch(...) { board.backup_ram()=backup;throw; }
            board.backup_ram()=backup;
            std::puts("native_record_reset tables=3 slots=72 PASS original2265d0/226638: ranks/time/name/flags, preserves view, whole backup+main RAM; separate lap defaults excluded");
            // 226a5c selects backup-RAM owners, not ROM record payloads.
            // Courses2/3 alias BOTH owners. Exercise every sign-extended course
            // halfword before replacing these guest pointers with native owners.
            unsigned course_cases=0;
            const uint32_t tables[]={0x1d00360,0x1d005a0,0x1d007e0};
            const uint32_t selections[]={0x1d00a20,0x1d00a24,0x1d00a2c};
            for(unsigned raw=0;raw<65536;++raw) {
                const uint32_t course=raw<32768 ? raw : raw|0xffff0000u;
                const unsigned owner=(raw&3u)<2 ? raw&3u : 2;
                rt::I960Ref selector(&board);selector.m_IP=0x226a5c;
                selector.m_r[16]=course;
                unsigned steps=0;
                while(selector.m_IP!=0x226a80) {
                    if(++steps>8) throw std::runtime_error("record course selector instruction guard");
                    selector.execute_one();
                }
                if(board.read_dword(0x50138c)!=tables[owner] ||
                   board.read_dword(0x5013b4)!=selections[owner] ||
                   selector.m_r[16]!=(raw&3u))
                    throw std::runtime_error("record course owner/alias differs");
                ++course_cases;
            }
            for(unsigned i=0;i<saved.size();++i) {
                const uint32_t expected=i==0x138c/4 ? tables[2] :
                    i==0x13b4/4 ? selections[2] : saved[i];
                if(board.read_dword(0x500000+4*i)!=expected)
                    throw std::runtime_error("record course selector changed unrelated main RAM");
            }
            restore();
            std::printf("native_record_course_owners cases=%u PASS original226a5c: signed16 mask, three backup tables, courses2/3 alias table+selection; default payloads not yet imported\n",course_cases);
            unsigned view_cases=0;
            for(unsigned view=0;view<256;++view) for(uint32_t flags : {0u,1u,2u,UINT32_MAX}) {
                uint32_t counts[]={0x80000000,7,UINT32_MAX,0};
                for(unsigned i=0;i<4;++i) board.write_dword(0x501700+4*i,counts[i]);
                board.write_byte(0x501710,(uint8_t)view);board.write_dword(0x501854,flags);
                nm_race_view_tick(counts,flags,(uint8_t)view);
                rt::I960Ref counter(&board);counter.m_IP=0x3520;
                unsigned steps=0;
                while(counter.m_IP!=0x3534 && counter.m_IP!=0x3558) {
                    if(++steps>16) throw std::runtime_error("camera counter instruction guard");
                    counter.execute_one();
                }
                if(counter.m_IP!=((flags&1u) ? 0x3534u : 0x3558u))
                    throw std::runtime_error("camera counter start gate differs");
                for(unsigned i=0;i<4;++i) if(board.read_dword(0x501700+4*i)!=counts[i])
                    throw std::runtime_error("camera counter/wrap differs");
                if(board.read_byte(0x501710)!=view || board.read_dword(0x501854)!=flags)
                    throw std::runtime_error("camera counter altered input owner");
                ++view_cases;
            }
            restore();
            std::printf("native_view_counters cases=%u PASS original3520..3554: all view bytes, bit0 gate, unsigned wrap; phase22 pre-movement\n",view_cases);
            // Selected-course record operation; execute original integer code
            // including insertion sort. The course-pointer lookup alone is
            // replaced with our explicit table owner; calls use a local PC
            // return below so interpreter stack traffic cannot hide changes.
            unsigned record_cases=0;
            for(unsigned sample=0;sample<256;++sample) {
                restore();
                constexpr uint32_t base=0x5d0000, selected_address=0x5d0300;
                nm_race_record_t records[24];
                for(unsigned i=0;i<24;++i) {
                    records[i]={i,100u+10*i,0xa5a50000u+i,0x80000000u+i,1234u+i,0x414141u+i};
                    std::array<uint32_t,6> words;memcpy(words.data(),records+i,sizeof(records[i]));
                    for(unsigned w=0;w<6;++w) board.write_dword(base+i*24+w*4,words[w]);
                }
                const uint32_t times[]={0u,99u,100u,101u,150u,329u,330u,331u,0x80000000u,UINT32_MAX};
                const uint32_t time=times[sample%10],eligible=sample&3u;
                const uint32_t view_ticks[]={sample&4u ? 100u : UINT32_MAX,
                    sample&8u ? 50u : 100u,sample&16u ? 0u : 100u,sample&32u ? 0u : 200u};
                const uint32_t mode=sample&64u ? UINT32_MAX : 0;
                const uint32_t alternate=sample&128u ? 0x80000001u : 0;
                board.write_dword(0x50138c,base);board.write_dword(0x5013b4,selected_address);
                board.write_dword(selected_address,base+7*24);
                board.write_dword(0x501398,0x12345678);board.write_dword(0x5010d8,eligible);
                board.write_dword(0x5010cc,time);board.write_dword(0x501a80,mode);
                board.write_dword(0x501a8c,alternate);
                for(unsigned i=0;i<4;++i) board.write_dword(0x501700+4*i,view_ticks[i]);
                std::vector<uint32_t> expected(saved.size());
                for(unsigned i=0;i<expected.size();++i) expected[i]=board.read_dword(0x500000+4*i);
                unsigned selected=7;uint32_t notification=0x12345678;
                static nm_race_scene_t record_scene;
                record_scene={};record_scene.phase=25;record_scene.count=40;
                record_scene.shared.clock=time;
                record_scene.cars[0].car.checkpoints.finish_flags=eligible;
                record_scene.cars[0].car.checkpoints.reverse=alternate;
                std::copy_n(view_ticks,4,record_scene.view_ticks);
                const int inserted=nm_race_scene_record(&record_scene,records,mode,&selected,&notification);
                if(!inserted) throw std::runtime_error("native course record rejected valid state");
                for(unsigned i=0;i<24;++i) {
                    std::array<uint32_t,6> words;memcpy(words.data(),records+i,sizeof(records[i]));
                    for(unsigned w=0;w<6;++w) expected[(base-0x500000)/4+i*6+w]=words[w];
                }
                expected[(selected_address-0x500000)/4]=base+selected*24;
                expected[0x1398/4]=notification;
                rt::I960Ref record(&board);record.m_IP=0x2268dc;
                unsigned steps=0;
                while(record.m_IP!=0x2269cc) {
                    if(++steps>1500) throw std::runtime_error("record insertion instruction guard");
                    if(record.m_IP==0x2268fc) { record.m_IP=0x226900;continue; }
                    if(record.m_IP==0x2269a0) { record.m_IP=0x2269d0;continue; }
                    if(record.m_IP==0x226a58) { record.m_IP=0x2269a4;continue; }
                    record.execute_one();
                }
                for(unsigned i=0;i<expected.size();++i)
                    if(board.read_dword(0x500000+4*i)!=expected[i])
                        throw std::runtime_error("native record update differs sample="+std::to_string(sample)+
                            " address="+std::to_string(0x500000+4*i)+" expected="+std::to_string(expected[i])+
                            " actual="+std::to_string(board.read_dword(0x500000+4*i)));
                ++record_cases;
            }
            restore();
            std::printf("native_course_records cases=%u PASS original2268dc/2269d0 via native phase25 scene binding: unsigned/ties/rank labels/view/mode/name/notification/full RAM; selected course, persistence excluded\n",record_cases);
            // Phase25's object ownership transition, NOT just its known timer
            // tail. Score-record callee2268dc and cabinet command prefix are
            // excluded explicitly; preserve every other main-RAM word.
            unsigned shutdown_cases=0;
            for(unsigned opponents : {1u,39u,43u})
            for(uint32_t mode : {0u,1u,UINT32_MAX}) {
                restore();
                auto expected=saved;
                const auto set=[&](uint32_t address,uint32_t value) {
                    board.write_dword(address,value);expected[(address-0x500000)/4]=value;
                };
                set(0x501084,opponents);set(0x501a80,mode);
                set(0x501480,0xa5a50010);set(0x5010cc,0x12345678);
                set(0x5010a4,0xa5a50019);set(0x5010f8,0xa5a5a5a5);
                uint32_t address=0x5e0000;
                const uint32_t owners[]={0x501224,0x50121c,0x501280,0x501230,
                    0x501294,0x50124c,0x50125c,0x50122c};
                for(unsigned group=0;group<8;++group) {
                    set(owners[group],address);
                    const unsigned count=group==0 ? opponents : group==5 ? 6 : 1;
                    for(unsigned i=0;i<count;++i) {
                        const uint32_t flags=(0xa5a50000u^(((i+group)&1u)<<31))|(group<<8)|i;
                        const unsigned stride=32+16*(i%3);
                        set(address,flags);set(address+8,stride);set(address+12,0x12345678);
                        expected[(address-0x500000)/4]=group==7 ? flags|0x80000000u : flags&0x7fffffffu;
                        if(group==7) expected[(address+12-0x500000)/4]=0xb260;
                        address+=stride;
                    }
                }
                expected[0x1480/4]|=1;
                expected[0x13c4/4]=0x12345678;
                expected[0x10a8/4]=nm_race_timeout_ticks(mode);
                expected[0x10a4/4]=0xa5a5001a;expected[0x10f8/4]=0xa5a5a500;
                rt::I960Ref shutdown(&board);shutdown.m_IP=0x418c;
                unsigned steps=0,record_calls=0;
                while(shutdown.m_IP!=0x42e4) {
                    if(++steps>1000) throw std::runtime_error("phase25 object shutdown guard");
                    if(shutdown.m_IP==0x41b0) {
                        ++record_calls;shutdown.m_IP=0x41b4;continue;
                    }
                    shutdown.execute_one();
                }
                if(record_calls!=1) throw std::runtime_error("phase25 record callback count differs");
                for(unsigned i=0;i<expected.size();++i)
                    if(board.read_dword(0x500000+4*i)!=expected[i])
                        throw std::runtime_error("phase25 object ownership/retained RAM differs");
                ++shutdown_cases;
            }
            restore();
            std::printf("native_timeout_ownership cases=%u PASS original418c..42e0: opponents and7 object groups, heterogeneous strides, preserved flags/full RAM; cabinet prefix and record callee excluded\n",shutdown_cases);
            // Actual phase dispatch, not guessed adjacency to the finish code.
            if(board.read_dword(0x18cc+29*4)!=0x4b40 || board.read_dword(0x18cc+30*4)!=0x4d60)
                throw std::runtime_error("post-result phase dispatch differs");
            unsigned postresult_cases=0;
            for(unsigned phase : {29u,30u})
            for(uint32_t notification : {0u,1u,UINT32_MAX})
            for(uint32_t timer : {0u,1u,2u,1920u,0x7fffffffu,0x80000000u,UINT32_MAX})
            for(uint32_t flags : {0x00123456u,0x80123456u})
            for(unsigned cabinet : {0u,127u,128u,255u}) {
                restore();
                board.write_dword(0x501234,actor);
                board.write_dword(actor,flags);board.write_dword(actor+12,0x12345678);
                board.write_byte(0x5010a4,(uint8_t)phase);board.write_dword(0x5010a8,timer);
                board.write_dword(0x501398,notification);board.write_byte(0x53e5bc,(uint8_t)cabinet);
                auto expected=saved;
                for(unsigned i=0;i<expected.size();++i) expected[i]=board.read_dword(0x500000+4*i);
                nm_race_postresult_t state={.timer=timer,.name_flags=flags,.audio_command=0,
                    .phase=(uint8_t)phase,.cabinet_flags=(uint8_t)cabinet,.start_name=0,.return_to_attract=0};
                if(!nm_race_postresult_step(&state,notification)) throw std::runtime_error("post-result state rejected");
                expected[0x10a4/4]=(expected[0x10a4/4]&0xffffff00u)|state.phase;
                expected[0x10a8/4]=state.timer;
                expected[(actor-0x500000)/4]=state.name_flags;
                if(state.start_name) expected[(actor+12-0x500000)/4]=0x226a84;
                expected[0x3e5bc/4]=(expected[0x3e5bc/4]&0xffffff00u)|state.cabinet_flags;
                rt::I960Ref control(&board);control.m_IP=phase==29 ? 0x4d00 : 0x4d60;
                unsigned steps=0,return_calls=0,sound_calls=0;uint32_t command=0;
                while(control.m_IP!=0x4d5c && control.m_IP!=0x4d80 &&
                      control.m_IP!=0x4d9c && control.m_IP!=0x4dd8) {
                    if(++steps>64) throw std::runtime_error("post-result control guard");
                    if(control.m_IP==0x4d34 || control.m_IP==0x4dc0) {
                        ++sound_calls;command=control.m_r[16];control.m_IP+=4;continue;
                    }
                    if(control.m_IP==0x4dc4) { ++return_calls;control.m_IP+=8;continue; }
                    control.execute_one();
                }
                if(return_calls!=state.return_to_attract || sound_calls!=(state.audio_command!=0) || command!=state.audio_command)
                    throw std::runtime_error("post-result callback intents differ phase="+std::to_string(phase)+
                        " timer="+std::to_string(timer)+" notification="+std::to_string(notification)+
                        " returns="+std::to_string(return_calls)+" command="+std::to_string(command));
                for(unsigned i=0;i<expected.size();++i)
                    if(board.read_dword(0x500000+4*i)!=expected[i])
                        throw std::runtime_error("post-result phase/timer/owner/full RAM differs");
                ++postresult_cases;
            }
            restore();
            std::printf("native_postresult_control cases=%u PASS original4d00/4d60: actual dispatch, signed timer boundaries, name owner, silent intents/full RAM; scene loader/name actor/return callee excluded\n",postresult_cases);
            // Window initialization and submission are separate: later camera
            // writes may replace a center. Check both against original code;
            // do not mistake a borrowed export center for a finish constant.
            rt::I960Ref window(&board);window.m_IP=0x17b40;window.m_r[29]=actor;
            unsigned window_steps=0;
            while(window.m_IP!=0x17b6c) {
                if(++window_steps>12) throw std::runtime_error("window initialization guard");
                window.execute_one();
            }
            constexpr uint32_t window_record[]={0x43000000,0xffff0080,0x01f00200,
                0x00f80140,0x00f8013c,0x01600098,0x00f8013c};
            for(unsigned i=0;i<7;++i)
                if(board.read_dword(0x502800+4*i)!=window_record[i] ||
                   board.read_dword(0x17bc8+4*i)!=window_record[i])
                    throw std::runtime_error("authored window record differs");
            if(board.read_dword(actor+12)!=0x17b94)
                throw std::runtime_error("window actor updater differs");
            for(unsigned changed=0;changed<2;++changed) {
                // A camera can change center0 independently of the ROM record.
                const uint32_t center=changed ? 0x00f8010eu : window_record[3];
                board.write_dword(0x50280c,center);
                window.m_IP=0x17b94;window.m_r[26]=0x5e0000;window.m_r[28]=0;
                window_steps=0;
                while(window.m_IP!=0x17bac) {
                    if(++window_steps>8) throw std::runtime_error("window submission setup guard");
                    window.execute_one();
                }
                if(board.read_dword(0x5e0030)!=0)
                    throw std::runtime_error("window command differs");
                // Redirect FIFO stores to scratch RAM, observing each word
                // separately. No renderer/TGP state is modified by this test.
                for(unsigned i=1;i<7;++i) {
                    window.execute_one();
                    if(board.read_dword(0x5e0000)!=(i==3 ? center : window_record[i]))
                        throw std::runtime_error("window emitted coordinate differs");
                }
                if(window.m_IP!=0x17bc4) throw std::runtime_error("window submission length differs");
            }
            restore();
            std::puts("native_finish_window PASS original17b40/17b94: viewport(-1,128,496,512), centers(248,320)/(248,316)/(352,152)/(248,316); dynamic center forwarded, finish selection still unverified");
            // Prove the finish record selected by44bc comes from authored ROM,
            // not the first visible car's focus/light used by early exports.
            rt::I960Ref setup(&board);setup.m_IP=0x17d2c;
            unsigned setup_steps=0;
            while(setup.m_IP!=0x17d6c) {
                if(++setup_steps>20) throw std::runtime_error("finish render setup guard");
                setup.execute_one();
            }
            constexpr uint32_t render_setup[]={0,0x438c0000,0x438c0000,0,0,0x3f800000};
            // Execute the actual object-command port write while the authored
            // record is installed. Preserve the live geometry buffer/pointer.
            const uint32_t geo_start=board.tgp().geo_r(0x2008/4);
            const uint32_t geo_word=board.tgp().buffer_r(0);
            board.tgp().geo_w(0x1008/4,0);
            setup.m_IP=0x1798c;setup.m_r[10]=0x502030;
            setup.m_r[26]=0x800000;setup.m_r[3]=0;
            setup.execute_one();setup.execute_one();
            const uint32_t finish_opcode=board.tgp().buffer_r(0);
            const uint32_t emitted_bytes=board.tgp().geo_r(0x2008/4);
            board.tgp().buffer_w(0,geo_word,UINT32_MAX);
            board.tgp().geo_w(0x1008/4,geo_start);
            if(setup.m_IP!=0x17998 || emitted_bytes!=4 || finish_opcode!=0x00800000)
                throw std::runtime_error("finish object opcode/center selection differs");
            std::puts("native_finish_opcode PASS original1798c/17990 opcode00800000 center0 reverse0");
            for(unsigned i=0;i<6;++i) {
                if(board.read_dword(0x502030+4*i)!=render_setup[i] ||
                   board.read_dword(0x235dd0+4*i)!=render_setup[i])
                    throw std::runtime_error("finish render setup differs from authored record");
                board.write_dword(0x502030+4*i,saved[(0x2030+4*i)/4]);
            }
            std::puts("native_finish_render_setup PASS original17d2c focus280/280 light0/0/1; record0 is not geo mode");
            // Check the signed comparison independently across every bit pattern.
            for(unsigned timer=0;timer<65536;++timer) {
                board.write_word(actor+6,(uint16_t)timer);
                rt::I960Ref branch(&board);branch.m_r[29]=actor;branch.m_IP=0x4580;
                for(unsigned i=0;i<3;++i) branch.execute_one();
                const int signed_timer=timer<32768 ? (int)timer : (int)timer-65536;
                if(branch.m_IP!=(signed_timer<57 ? 0x48d0u : 0x458cu))
                    throw std::runtime_error("finish actor signed branch differs");
            }
            // Dirty actor bytes make untouched fields/partial stores observable.
            for(unsigned i=0;i<0x100;i+=4) board.write_dword(actor+i,0xa5a50000u+i);
            rt::I960Ref cpu(&board);cpu.m_r[27]=0x880000;cpu.m_r[28]=0x4000;
            cpu.m_r[29]=actor;cpu.m_IP=0x44dc;
            unsigned steps=0;
            while(cpu.m_IP!=0x4580) {
                if(++steps>40) throw std::runtime_error("finish actor initialization guard");
                cpu.execute_one();
            }
            const uint32_t initial[][2]={{0x1c,0x40966666},{0x20,0x40a00000},
                {0x24,0x40b00000},{0x34,0xc0a33333},{0x38,0x40a33333},
                {0x3c,0xc0966666},{0x40,0xbe99999a},{0x44,0xbdcccccd},
                {0x48,0x3dcccccd},{0x4c,0x3e99999a},{0x50,0},{0x54,0x3f000000}};
            for(const auto &entry:initial)
                if(board.read_dword(actor+entry[0])!=entry[1])
                    throw std::runtime_error("finish actor initial float bits differ");
            if(board.read_word(actor+6)!=256 || board.read_dword(actor+0x28)!=0xa5a50028 ||
               board.read_dword(actor+0x2c)!=0xa5a50000)
                throw std::runtime_error("finish actor initial partial stores differ");
            unsigned moving=0,settled=0;
            uint32_t pose_hash=2166136261u;
            nm_finish_actor_t native;
            if(!nm_finish_actor_init(&native,0x28)) throw std::runtime_error("native finish actor init rejected");
            constexpr uint32_t lists[]={0x288b460,0x288b488,0x288b44c,0x288b474};
            for(unsigned tick=0;tick<384;++tick) {
                const int native_branch=nm_finish_actor_step(&native);
                if(!native_branch) throw std::runtime_error("native finish actor step rejected");
                cpu.m_IP=0x4580;steps=0;unsigned draws=0;
                while(cpu.m_IP!=0x4b3c) {
                    if(++steps>300) throw std::runtime_error("finish actor tick guard");
                    if(cpu.m_IP==0x458c) ++moving;
                    if(cpu.m_IP==0x48d0) ++settled;
                    switch(cpu.m_IP) {
                    case 0x46f4:case 0x478c:case 0x4824:case 0x48bc:
                    case 0x4964:case 0x49fc:case 0x4a94:case 0x4b2c:
                        if(draws>=4 || cpu.m_r[16]!=lists[draws++])
                            throw std::runtime_error("finish actor model order differs");
                        {
                            float pose[12];
                            if(!nm_finish_actor_pose(&native,draws-1,native_branch,pose))
                                throw std::runtime_error("native finish actor pose rejected");
                            // Original178d0..17938 reads this same matrix command.
                            board.tgp().function_port_w(0x11*4,0x1111);
                            for(unsigned i=0;i<12;++i) {
                                const uint32_t original=board.tgp().fifo_r();
                                pose_hash=(pose_hash^original)*16777619u;
                                if(nm_force_bits(pose[i])!=original) {
                                    std::fprintf(stderr,"finish matrix tick=%u part=%u element=%u native=%08x original=%08x\n",
                                        tick,draws-1,i,nm_force_bits(pose[i]),original);
                                    throw std::runtime_error("native finish actor matrix differs");
                                }
                            }
                        }
                        cpu.m_IP+=4;break;
                    default:cpu.execute_one();break;
                    }
                }
                board.tgp().sync();
                if(draws!=4 || board.read_word(actor+6)!=(uint16_t)(255-tick))
                    throw std::runtime_error("finish actor draw count/timer differs");
                const unsigned advances=tick<200 ? tick+1 : 200;
                const uint32_t native_words[]={nm_force_bits(native.approach.x),nm_force_bits(native.approach.y),
                    nm_force_bits(native.approach.z),nm_force_bits(native.other_x.x),nm_force_bits(native.other_x.y),
                    nm_force_bits(native.other_x.z),native.tiny_y_bits};
                const unsigned offsets[]={0x1c,0x20,0x24,0x34,0x38,0x3c,0x50};
                for(unsigned i=0;i<7;++i) if(board.read_dword(actor+offsets[i])!=native_words[i])
                    throw std::runtime_error("native finish actor state differs");
                if(native.timer!=board.read_word(actor+6) || native.inherited_angle!=board.read_word(actor+0x28))
                    throw std::runtime_error("native finish actor integer state differs");
                // 4664 overwrites r3 with INTEGER655 before command0 at4678.
                // The original consequently accumulates subnormal float bits,
                // not another0.025 step. Preserve this evidence for SH4 DN mode.
                if(board.read_dword(actor+0x50)!=655*advances ||
                   board.read_word(actor+0x28)!=(uint16_t)(0x28+655*advances) ||
                   board.read_word(actor+0x2c)!=0)
                    throw std::runtime_error("finish actor raw subnormal/angle state differs");
                if(tick>=199 && (board.read_dword(actor+0x1c)!=0xbe999aca ||
                    board.read_dword(actor+0x20)!=0xb727d000 ||
                    board.read_dword(actor+0x24)!=0x3efffe73))
                    throw std::runtime_error("finish actor terminal translation differs");
                if(tick==0 || tick==199 || tick==200 || tick==383)
                    std::printf("native_finish_actor tick=%u timer=%04x x=%08x y=%08x z=%08x offset28=%04x rotation2c=%04x field50=%08x\n",
                        tick,board.read_word(actor+6),board.read_dword(actor+0x1c),
                        board.read_dword(actor+0x20),board.read_dword(actor+0x24),
                        board.read_word(actor+0x28),board.read_word(actor+0x2c),board.read_dword(actor+0x50));
            }
            if(moving!=200 || settled!=184) throw std::runtime_error("finish actor branch duration differs");
            for(unsigned i=0;i<saved.size();++i) {
                const uint32_t address=0x500000+4*i;
                if((address<actor || address>=actor+0x100) && board.read_dword(address)!=saved[i])
                    throw std::runtime_error("finish actor changed unrelated RAM");
            }
            std::puts("native_finish_actor PASS signed_cases=65536 ticks=384 moving=200 settled=184 draws=1536 (original TGP; geometry callees excluded)");
            std::printf("native_finish_actor matrices=1536 exact_bits PASS hash=%08x\n",pose_hash);
        } catch(...) { restore();throw; }
        restore();
    }
    // Full phase23 control flow, including19bf4's SOUND command queue writes.
    // Commands are data, not ROM graphics addresses. No sound device on target.
    {
        std::vector<uint32_t> saved(0x100000/4);
        for(unsigned i=0;i<saved.size();++i) saved[i]=board.read_dword(0x500000+4*i);
        const auto restore=[&] { for(unsigned i=0;i<saved.size();++i) board.write_dword(0x500000+4*i,saved[i]); };
        unsigned cases=0;
        try {
            for(int rank:{-128,-1,0,1,2,127}) for(uint32_t inhibit:{0u,1u,0x80000000u,UINT32_MAX})
            for(unsigned queue:{0u,255u}) {
                restore();
                const uint32_t flags=0xa5a50000u+cases;
                board.write_dword(0x50121c,0x5f0000);board.write_dword(0x501230,0x5f0300);
                board.write_dword(0x501220,0x5f0600);board.write_byte(0x5f066a,(uint8_t)rank);
                board.write_dword(0x5f0000,flags);board.write_dword(0x5f0300,~flags);
                board.write_dword(0x501a80,inhibit);board.write_byte(0x5010a4,23);
                board.write_byte(0x5028fe,(uint8_t)queue);board.write_byte(0x5028fd,9);
                board.write_byte(0x503001,0);board.write_byte(0x503003,15);
                nm_race_finish_entry_t native{};
                native.phase=23;native.previous_object_flags=flags;native.result_object_flags=~flags;
                if(!nm_race_finish_enter(inhibit,(int8_t)rank,&native))
                    throw std::runtime_error("native finish phase entry rejected");
                std::vector<uint32_t> expected(saved.size());
                // Build projected RAM with width-correct writes, then restore
                // the actual entry image before executing original code.
                std::vector<uint32_t> before(saved.size());
                for(unsigned i=0;i<before.size();++i) before[i]=board.read_dword(0x500000+4*i);
                board.write_dword(0x5014a4,native.camera_mode);board.write_dword(0x5010a8,native.timer);
                board.write_word(0x50132c,(uint16_t)native.countdown);
                board.write_dword(0x5f0000,native.previous_object_flags);
                board.write_dword(0x5f0300,native.result_object_flags);
                board.write_dword(0x5f030c,native.result_pending ? 0x44a8 : 0);
                board.write_dword(0x5f060c,native.player_finishing ? 0xda18 : 0);
                board.write_byte(0x5010a4,native.phase);
                board.write_byte(0x502900+queue,native.command);
                board.write_byte(0x5028fe,(uint8_t)(queue+1));
                for(unsigned i=0;i<native.audio_count;++i)
                    board.write_dword(0x503020+4*((15+i)&15),native.audio_commands[i]);
                board.write_byte(0x503001,native.audio_count);
                board.write_byte(0x503003,(uint8_t)((15+native.audio_count)&15));
                for(unsigned i=0;i<expected.size();++i) {
                    expected[i]=board.read_dword(0x500000+4*i);
                    board.write_dword(0x500000+4*i,before[i]);
                }
                rt::I960Ref cpu(&board);cpu.m_IP=0x4328;
                unsigned steps=0,commands=0;uint32_t requests[2]{};
                while(cpu.m_IP!=0x444c) {
                    if(++steps>80) throw std::runtime_error("finish phase entry instruction guard");
                    if(cpu.m_IP==0x43b4 || cpu.m_IP==0x4448) {
                        if(commands>=2) throw std::runtime_error("extra finish sound request");
                        requests[commands++]=cpu.m_r[16];
                        // Independent callee registers model CALL's local
                        // register window without using an artificial stack.
                        rt::I960Ref sound(&board);sound.m_IP=0x19bf4;sound.m_r[16]=cpu.m_r[16];
                        unsigned sound_steps=0;
                        while(sound.m_IP!=0x19c54) {
                            if(++sound_steps>32) throw std::runtime_error("finish sound enqueue instruction guard");
                            sound.execute_one();
                        }
                        cpu.m_IP+=4;
                    } else cpu.execute_one();
                }
                if(commands!=native.audio_count || memcmp(requests,native.audio_commands,sizeof(requests)))
                    throw std::runtime_error("finish phase sound intents differ");
                for(unsigned i=0;i<expected.size();++i)
                    if(board.read_dword(0x500000+4*i)!=expected[i])
                        throw std::runtime_error("finish phase entry RAM differs");
                const auto entered=native;
                if(nm_race_finish_enter(inhibit,(int8_t)rank,&native) || memcmp(&native,&entered,sizeof(native)))
                    throw std::runtime_error("finish phase entry repeated or mutated rejected state");
                ++cases;
            }
        } catch(...) { restore();throw; }
        restore();
        if(nm_race_finish_enter(0,0,nullptr)) throw std::runtime_error("finish phase entry accepted null");
        std::printf("native_finish_phase_entry cases=%u PASS (full RAM, byte/word queue wrap, ordered silent sound intents; original19bf4 included)\n",cases);
    }
    const uint16_t saved_wheel_phase=board.read_word(0x51005a);
    try {
        for(unsigned phase=0;phase<65536;++phase) for(unsigned level:{3u,4u}) {
            board.write_word(0x51005a,(uint16_t)phase);
            rt::I960Ref cpu(&board);cpu.m_r[24]=0x510000;
            cpu.m_IP=level==4 ? 0x7ab4 : 0x7d8c;
            const uint32_t end=level==4 ? 0x7ac8 : 0x7da0;
            unsigned steps=0;
            while(cpu.m_IP!=end) {
                if(++steps>8) throw std::runtime_error("wheel phase instruction guard");
                cpu.execute_one();
            }
            const int index=nm_intro_wheel_phase(level,(uint16_t)phase);
            if(index<0 || cpu.m_r[16]!=board.read_dword(0x832c+4*index))
                throw std::runtime_error("wheel phase differs from original7ab4/7d8c");
        }
    } catch(...) { board.write_word(0x51005a,saved_wheel_phase);throw; }
    board.write_word(0x51005a,saved_wheel_phase);
    for(unsigned level:{0u,1u,2u,5u,UINT32_MAX})
        if(nm_intro_wheel_phase(level,65535)!=-1) throw std::runtime_error("unexpected wheel detail level");
    std::puts("native_intro_wheel_phase cases=131072 PASS (original7ab4/7d8c, all signed16 phases)");
    const int32_t detail_edges[]={0,1,4,5,6,19,20,21,34,35,36,99,100,101,255,256,257,32767,INT32_MAX};
    unsigned level_cases=0;
    for(int32_t x:detail_edges) for(int32_t y:detail_edges)
    for(int32_t z: {-32768,-1,0,1,19,20,21,34,35,36,99,100,101,255,256,257,32767}) {
        rt::I960Ref cpu(&board);cpu.m_IP=0x8590;cpu.m_r[16]=(uint32_t)x;
        cpu.m_r[10]=(uint32_t)z;cpu.m_r[9]=0;
        cpu.m_r[12]=cpu.m_r[13]=cpu.m_r[14]=cpu.m_r[15]=0;
        unsigned steps=0;
        while(cpu.m_IP!=0x85f8) {
            if(++steps>50) throw std::runtime_error("detail level instruction guard");
            if(cpu.m_IP==0x85a0) { cpu.m_r[17]=(uint32_t)y;cpu.m_IP=0x85a4; }
            else cpu.execute_one();
        }
        if(cpu.m_r[9]!=nm_intro_detail_level(x,y,z))
            throw std::runtime_error("detail level differs from original8590");
        ++level_cases;
    }
    std::printf("native_intro_detail_level cases=%u PASS (original8590, converted integer boundaries; TGP/CVTRI excluded)\n",level_cases);
    // Exhaust every legal four-level distribution for the bounded44-car pool.
    // Original instruction execution, no independently invented LOD heuristic.
    const uint32_t saved_detail=board.read_dword(0x503201);
    unsigned detail_cases=0,max_wheel_parts=0,max_window_parts=0,max_shadow_parts=0,max_crash_parts=0;
    try {
        for(unsigned a=0;a<=44;++a) for(unsigned b=0;b<=44-a;++b)
        for(unsigned c=0;c<=44-a-b;++c) for(unsigned d=0;d<=44-a-b-c;++d) {
            uint8_t q[4]={(uint8_t)a,(uint8_t)b,(uint8_t)c,(uint8_t)d};
            for(unsigned i=0;i<4;++i) board.write_byte(0x503201+i,q[i]);
            if(!nm_intro_detail_budget(q)) throw std::runtime_error("valid detail quota rejected");
            rt::I960Ref cpu(&board);cpu.m_IP=0x836c;
            unsigned steps=0;
            while(cpu.m_IP!=0x847c) {
                if(++steps>10000) throw std::runtime_error("detail quota instruction guard");
                cpu.execute_one();
            }
            unsigned total=0,cost=0;const unsigned weights[4]={16,161,253,341};
            for(unsigned i=0;i<4;++i) {
                if(board.read_byte(0x503201+i)!=q[i])
                    throw std::runtime_error("detail quota differs from original836c");
                total+=q[i];cost+=q[i]*weights[i];
            }
            if(total!=a+b+c+d || cost>=2000) throw std::runtime_error("invalid detail budget output");
            const unsigned detailed=q[2]+q[3];
            max_wheel_parts=std::max(max_wheel_parts,total+detailed);
            max_window_parts=std::max(max_window_parts,total+2*detailed);
            max_shadow_parts=std::max(max_shadow_parts,total+2*detailed+q[1]+detailed);
            // Low-level crashes add attachments; normal high levels retain
            // all shadows. Up to3 low-level2 crashes also keep their shadows.
            max_crash_parts=std::max(max_crash_parts,2u*(q[0]+q[1])+4*detailed+std::min(3u,unsigned(q[1])));
            ++detail_cases;
        }
    } catch(...) { board.write_dword(0x503201,saved_detail);throw; }
    board.write_dword(0x503201,saved_detail);
    uint8_t invalid_detail[4]={44,1,0,0};
    if(nm_intro_detail_budget(invalid_detail) || invalid_detail[0]!=44 || invalid_detail[1]!=1 ||
       nm_intro_detail_budget(nullptr)) throw std::runtime_error("invalid detail budget accepted/mutated");
    std::printf("native_intro_detail_budget cases=%u PASS (all distributions up to44cars, original836c, strict2000cost)\n",detail_cases);
    if(max_wheel_parts!=49 || max_window_parts!=54 || max_shadow_parts!=59 || max_crash_parts+1!=NM_INTRO_MAX_PARTS)
        throw std::runtime_error("intro body/wheel/window instance bound changed");
    std::printf("native_intro_part_bound PASS body+wheel=%u body+wheel+window=%u with shadows=%u crash-maximum=%u; plus one special extra part\n",
        max_wheel_parts,max_window_parts,max_shadow_parts,max_crash_parts);
    const uint32_t saved_height=board.read_dword(0x51005c);
    const uint16_t saved_shadow_count=board.read_word(0x501120);
    for(unsigned level=2;level<=4;++level) for(unsigned count:{0u,1u,2u,3u,4u,65535u})
    for(uint32_t height:{0u,0x80000000u,0x3f800000u,0x7fc00000u}) {
        board.write_dword(0x51005c,height);board.write_word(0x501120,(uint16_t)count);
        const uint32_t starts[3]={0x8060,0x7da4,0x7acc};
        const uint32_t normal[3]={0x81b4,0x7ef8,0x7c20};
        const uint32_t crash[3]={0x8080,0x7dc4,0x7aec};
        const uint32_t skip[3]={0x81c0,0x7f04,0x7c2c};
        const unsigned k=level-2;
        rt::I960Ref cpu(&board);cpu.m_r[24]=0x510000;cpu.m_IP=starts[k];
        unsigned steps=0;
        while(cpu.m_IP!=normal[k] && cpu.m_IP!=crash[k] && cpu.m_IP!=skip[k]) {
            if(++steps>8) throw std::runtime_error("shadow quota instruction guard");
            cpu.execute_one();
        }
        unsigned actual_count=count;
        if(bool(nm_intro_shadow_select(level,height,&actual_count))!=(cpu.m_IP!=skip[k]) ||
           actual_count!=board.read_word(0x501120)) throw std::runtime_error("shadow quota differs from original");
    }
    board.write_dword(0x51005c,saved_height);board.write_word(0x501120,saved_shadow_count);
    for(unsigned level:{0u,1u,5u,UINT32_MAX}) {
        unsigned count=1;
        if(nm_intro_shadow_select(level,1,&count) || count!=1)
            throw std::runtime_error("invalid/invisible shadow consumed quota");
    }
    if(nm_intro_shadow_select(2,0,nullptr)) throw std::runtime_error("shadow accepted missing quota");
    std::puts("native_intro_shadow_select PASS 72 original cases, three detail blocks, raw +/-zero and quota boundaries");
    std::vector<uint32_t> detail_ram(0x100000/4);
    for(unsigned i=0;i<detail_ram.size();++i) detail_ram[i]=board.read_dword(0x500000+4*i);
    const auto restore_detail=[&] {
        for(unsigned i=0;i<detail_ram.size();++i) board.write_dword(0x500000+4*i,detail_ram[i]);
    };
    uint32_t seed=0x836c7668;
    try {
        for(unsigned trial=0;trial<4096;++trial) {
            const unsigned count=trial%45;uint8_t order[44]={};int16_t depths[44]={};
            board.write_byte(0x503200,count);
            for(unsigned i=0;i<count;++i) {
                order[i]=(uint8_t)i;seed=seed*1664525+1013904223;
                depths[i]=trial<90 ? 7 : std::bit_cast<int16_t>((uint16_t)(seed>>16));
            }
            for(unsigned i=count;i>1;--i) {
                seed=seed*1664525+1013904223;std::swap(order[i-1],order[seed%i]);
            }
            for(unsigned i=0;i<count;++i) {
                board.write_dword(0x503208+8*i,0x510000+order[i]*0x300);
                board.write_word(0x50320c+8*i,(uint16_t)depths[order[i]]);
            }
            if(!nm_intro_detail_order(order,depths,count)) throw std::runtime_error("valid detail order rejected");
            rt::I960Ref cpu(&board);cpu.m_IP=0x84e0;unsigned steps=0;
            while(cpu.m_IP!=0x8630) {
                if(++steps>10000) throw std::runtime_error("detail order instruction guard");
                // Prepared signed16 depths isolate the original ordering slice;
                // visibility/transform/classification require their own oracle.
                if(cpu.m_IP==0x852c) cpu.m_IP=0x85fc;
                else cpu.execute_one();
            }
            for(unsigned i=0;i<count;++i)
                if(board.read_dword(0x503208+8*i)!=0x510000+order[i]*0x300 ||
                   board.read_word(0x50320c+8*i)!=(uint16_t)depths[order[i]])
                    throw std::runtime_error("detail order differs from original85fc");
        }
        for(unsigned trial=0;trial<4096;++trial) {
            const unsigned count=trial%45;uint8_t requested[44]={},selected[44]={},quota[4]={};
            board.write_dword(0x501090,8);board.write_dword(0x501a84,1);
            board.write_byte(0x503200,count);
            for(unsigned i=0;i<count;++i) {
                seed=seed*1664525+1013904223;
                requested[i]=trial<225 ? trial/45 : (seed>>16)%5;
                if(requested[i]) ++quota[requested[i]-1];
                const uint32_t entity=0x510000+i*0x300;
                board.write_dword(0x503208+8*i,entity);board.write_byte(entity+0xc4,requested[i]);
                board.write_dword(entity+0x14,0);
                for(unsigned level=1;level<=4;++level) board.write_dword(entity+0x6c+4*level,0x100000+level);
            }
            if(!nm_intro_detail_assign(requested,count,selected) || !nm_intro_detail_budget(quota))
                throw std::runtime_error("valid detail assignment rejected");
            for(unsigned i=0;i<4;++i) board.write_byte(0x503201+i,quota[i]);
            rt::I960Ref cpu(&board);cpu.m_IP=0x7668;
            unsigned steps=0;
            while(cpu.m_IP!=0x77b0) {
                if(++steps>10000) throw std::runtime_error("detail assignment instruction guard");
                // Selection-only boundary: rendering calls consume the selected
                // model, but are not part of the imported quota operation.
                if(cpu.m_IP==0x7710 || cpu.m_IP==0x7740 || cpu.m_IP==0x7770 || cpu.m_IP==0x779c)
                    cpu.m_IP+=4;
                else cpu.execute_one();
            }
            for(unsigned i=0;i<count;++i) {
                const uint32_t entity=0x510000+i*0x300;
                if(board.read_byte(entity+0xc4)!=selected[i] ||
                   board.read_dword(entity+0x14)!=(selected[i] ? 0x100000u+selected[i] : 0)) {
                    std::fprintf(stderr,"detail trial=%u car=%u requested=%u selected=%u actual=%u model=%x\n",
                        trial,i,requested[i],selected[i],board.read_byte(entity+0xc4),board.read_dword(entity+0x14));
                    throw std::runtime_error("detail assignment differs from original7668");
                }
            }
        }
    } catch(...) { restore_detail();throw; }
    restore_detail();
    uint8_t duplicate_order[2]={0,0};const int16_t order_depths[2]={1,0};
    if(nm_intro_detail_order(duplicate_order,order_depths,2) || duplicate_order[0] || duplicate_order[1] ||
       nm_intro_detail_order(duplicate_order,order_depths,45) || nm_intro_detail_order(nullptr,order_depths,0))
        throw std::runtime_error("invalid detail order accepted/mutated");
    std::puts("native_intro_detail_order cases=4096 PASS (original85fc, signed16 depths, one backward pass)");
    uint8_t invalid_request[2]={1,5},detail_untouched[2]={0xa5,0xa5};
    if(nm_intro_detail_assign(invalid_request,2,detail_untouched) || detail_untouched[0]!=0xa5 || detail_untouched[1]!=0xa5 ||
       nm_intro_detail_assign(invalid_request,45,detail_untouched) ||
       nm_intro_detail_assign(nullptr,0,detail_untouched) || nm_intro_detail_assign(invalid_request,0,nullptr))
        throw std::runtime_error("invalid detail assignment accepted/mutated");
    std::puts("native_intro_detail_assign cases=4096 PASS (original7668 selection, render calls excluded)");
    // First authored cinematic script: independent original cursor execution.
    // No TGP instructions or emulated scheduler needed by this integer slice.
    for(uint32_t base : {0x2864030u,0x2864158u}) {
        std::vector<uint32_t> times;
        while(board.read_dword(base+48*times.size())!=UINT32_MAX) {
            const unsigned i=times.size();
            if(i>=2048) throw std::runtime_error("intro camera key bound exceeded");
            times.push_back(board.read_dword(base+48*i));
            if(board.read_dword(base+48*i+44)) throw std::runtime_error("intro cursor check requires event-free script");
        }
        const unsigned count=times.size();
        if(board.read_dword(base+48*count)!=UINT32_MAX || board.read_dword(base+48*count+4)!=base)
            throw std::runtime_error("intro camera self-loop source changed");
        const uint32_t saved_pointer=board.read_dword(0x50149c), saved_tick=board.read_dword(0x5014a0);
        board.write_dword(0x50149c,base-48);board.write_dword(0x5014a0,0);
        nm_intro_cursor_t cursor{};
        try {
            for(unsigned tick=0;tick<10000;++tick) {
                nm_intro_interval_t interval{};
                if(!nm_intro_camera_step(times.data(),count,&cursor,&interval)) throw std::runtime_error("intro cursor rejected source");
                rt::I960Ref cpu(&board);cpu.m_IP=0x5cb8;
                unsigned steps=0;
                while(cpu.m_IP!=0x5d30) {
                    if(++steps>128) throw std::runtime_error("intro cursor original instruction guard");
                    cpu.execute_one();
                }
                if(cpu.m_r[16]!=base+48*interval.key || cpu.m_r[17]!=base+48*(interval.key+1) ||
                   board.read_dword(0x5014a0)!=cursor.tick-1 ||
                   interval.elapsed!=cursor.tick-1-times[interval.key] ||
                   interval.duration!=times[interval.key+1]-times[interval.key])
                    throw std::runtime_error("intro cursor differs from original5cb8");
                board.write_dword(0x5014a0,cursor.tick);
            }
        } catch(...) {
            board.write_dword(0x50149c,saved_pointer);board.write_dword(0x5014a0,saved_tick);throw;
        }
        board.write_dword(0x50149c,saved_pointer);board.write_dword(0x5014a0,saved_tick);
        const uint32_t invalid_times[]={0,0,0};nm_intro_cursor_t bad{};nm_intro_interval_t out{99,98,97};
        if(nm_intro_camera_step(invalid_times,3,&bad,&out) || bad.tick || bad.key || out.key!=99 || out.elapsed!=98 || out.duration!=97)
            throw std::runtime_error("intro malformed zero-time sequence changed outputs");
        std::printf("native_intro_cursor ticks=10000 keys=%u PASS (original%08x, exact shot cuts/loop; not camera interpolation)\n",count,base);
    }
    VehicleOracleBus bus(board);
    for(unsigned raw=0;raw<65536;++raw) for(unsigned branch=0;branch<3;++branch) {
        bus.entity.fill((uint8_t)raw);bus.globals.fill(0xa5);
        nm_player_t s{};
        s.boundary.heading=nm_yaw_angle(raw*37u);
        s.boundary.ground.road_heading=nm_yaw_angle(raw+(uint16_t)s.boundary.heading);
        s.phase[0]=nm_yaw_angle(raw);s.path_offset=0xa55a;
        bus.write_word(0x500048,(uint16_t)s.boundary.heading);
        bus.write_word(0x5001c2,(uint16_t)s.boundary.ground.road_heading);
        bus.write_word(0x500128,(uint16_t)s.phase[0]);bus.write_word(0x500134,s.path_offset);
        if(!nm_player_finish_heading(&s,branch)) throw std::runtime_error("finish heading rejected valid branch");
        auto expected=bus.entity;const auto globals=bus.globals;
        memcpy(expected.data()+0x48,&s.boundary.heading,2);
        memcpy(expected.data()+0x58,&s.boundary.ground.yaw,2);
        memcpy(expected.data()+0x134,&s.path_offset,2);
        const uint32_t entries[]={0xdb3c,0xdb68,0xdba4};
        bus.first_instruction=entries[branch];bus.last_instruction=0xdbcc;
        rt::I960Ref cpu(&bus);cpu.m_IP=entries[branch];cpu.m_r[29]=0x500000;
        unsigned steps=0;
        while(cpu.m_IP!=0xdbd8) {
            if(++steps>24) throw std::runtime_error("finish heading instruction guard");
            cpu.execute_one();
        }
        if(bus.entity!=expected || bus.globals!=globals)
            throw std::runtime_error("finish heading differs from original");
    }
    for(unsigned raw=0;raw<65536;++raw) for(unsigned flag=0;flag<2;++flag) {
        nm_player_t s{};s.boundary.ground.road_heading=nm_yaw_angle(raw);
        s.path_flags=(uint16_t)(0xfffeu|flag);
        bus.write_word(0x500048,0);bus.write_word(0x5001c2,(uint16_t)s.boundary.ground.road_heading);
        bus.write_dword(0x500138,s.path_flags);
        const auto before=bus.entity;
        bus.first_instruction=0xdaf4;bus.last_instruction=0xdb20;
        rt::I960Ref cpu(&bus);cpu.m_IP=0xdaf4;cpu.m_r[29]=0x500000;
        unsigned steps=0;
        while(cpu.m_IP!=0xdb24 && cpu.m_IP!=0xdb60 && cpu.m_IP!=0xdb9c) {
            if(++steps>16) throw std::runtime_error("finish branch instruction guard");
            cpu.execute_one();
        }
        const unsigned branch=cpu.m_IP==0xdb24 ? NM_FINISH_GUIDED :
            cpu.m_IP==0xdb60 ? NM_FINISH_FORWARD : NM_FINISH_REVERSE;
        if(nm_player_finish_branch(&s)!=branch || bus.entity!=before)
            throw std::runtime_error("finish branch differs from original");
    }
    nm_player_t invalid{};const auto invalid_before=invalid;
    if(nm_player_finish_heading(&invalid,3) || nm_player_finish_heading(nullptr,0) ||
       memcmp(&invalid,&invalid_before,sizeof(invalid_before))) throw std::runtime_error("invalid finish heading mutated state");
    std::puts("native_finish_heading cases=196608 branch_cases=131072 PASS (full entity canary, angle wrap/negative shifts; motion excluded)");
    for(unsigned profile=0;profile<65536;++profile) {
        PlayerMemory before;
        for(unsigned i=0;i<before.entity.size();++i) before.entity[i]=(uint8_t)(profile+37*i);
        const uint16_t raw=(uint16_t)profile;
        memcpy(before.entity.data()+0x128,&raw,2);
        nm_player_t native{};before.fields(native,false);
        if(!nm_player_finish_entry(&native)) throw std::runtime_error("finish entry rejected state");
        bus.entity=before.entity;bus.globals=before.globals;
        bus.first_instruction=0xda58;bus.last_instruction=0xda80;
        rt::I960Ref cpu(&bus);cpu.m_IP=0xda58;cpu.m_r[29]=0x500000;
        unsigned steps=0;
        while(cpu.m_IP!=0xda84) {
            if(++steps>16) throw std::runtime_error("finish entry instruction guard");
            cpu.execute_one();
        }
        auto actual=before;actual.entity=bus.entity;actual.globals=bus.globals;
        auto projected=actual;projected.fields(native,true);
        if(projected!=actual) throw std::runtime_error("native finish entry differs from originalda58");
    }
    if(nm_player_finish_entry(nullptr)) throw std::runtime_error("finish entry accepted null state");
    std::puts("native_finish_entry cases=65536 PASS (all signed profiles, mapped state, dirty brake high bits; not coast loop)");
    for(unsigned entered=0;entered<2;++entered) for(unsigned bit=0;bit<32;++bit)
        for(unsigned inverse=0;inverse<2;++inverse) {
            const uint32_t flags=inverse ? ~(1u<<bit) : 1u<<bit;
            bus.entity.fill(0xa5);bus.globals.fill(0x5a);bus.checkpoint_memory.fill(0x69);
            bus.write_dword(0x501090,flags);
            bus.first_instruction=0xda18;bus.last_instruction=0xdaa8;
            rt::I960Ref cpu(&bus);cpu.m_IP=entered ? 0xda84 : 0xda18;cpu.m_r[29]=0x500000;
            unsigned steps=0;
            while(cpu.m_IP!=0xdaac && cpu.m_IP!=0xe078) {
                if(++steps>40) throw std::runtime_error("finish dispatch instruction guard");
                cpu.execute_one();
            }
            nm_player_t player{};player.boundary.ground.position[0]=-0.f;
            const auto before=player;
            nm_player_active_input_t input{};input.dynamics.speed_flags=flags;
            uint8_t native_entered=(uint8_t)entered;uint32_t hold=0x12345678;
            // Missing assets deliberately distinguish physics entry from the
            // disabled circle-only path. Enabled physics has its own oracle.
            const int accepted=nm_player_finish_tick(nullptr,nullptr,nullptr,nullptr,
                &input,&player,&native_entered,&hold);
            const bool disabled=cpu.m_IP==0xe078;
            uint32_t reference_hold;
            memcpy(&reference_hold,bus.checkpoint_memory.data()+0x684,4);
            if(reference_hold!=(!entered && !disabled ? 1u : 0x69696969u))
                throw std::runtime_error("finish dispatch timer hold differs from entry contract");
            auto expected=before;if(disabled) nm_player_circle_step(&expected);
            if(bool(accepted)!=disabled || memcmp(&player,&expected,sizeof(player)) ||
               native_entered!=entered || hold!=0x12345678)
                throw std::runtime_error("finish dispatch differs from original da18/da84 phase gates");
        }
    std::puts("native_finish_dispatch cases=128 PASS (all phase bits/complements, entry/stable, disabled circle and atomic missing-asset rejection)");
    for(unsigned pattern=0;pattern<256;++pattern) {
        PlayerMemory before;
        for(unsigned i=0;i<before.entity.size();++i) before.entity[i]=(uint8_t)(pattern+37*i);
        before.checkpoint_extra.fill((uint8_t)pattern);
        const uint32_t size=0x300;memcpy(before.entity.data()+8,&size,4);
        nm_player_t native{};before.fields(native,false);
        const float desired=std::bit_cast<float>(board.read_dword(0x28b57a4));
        if(!nm_player_base_reset(&native,desired)) throw std::runtime_error("base reset rejected ROM constant");
        bus.entity=before.entity;bus.checkpoint_memory=before.checkpoint_extra;
        bus.run(0xe3f4,0xe590);
        auto actual=before;actual.entity=bus.entity;actual.checkpoint_extra=bus.checkpoint_memory;
        auto projected=actual;projected.fields(native,true);
        if(projected!=actual) throw std::runtime_error("mapped native base reset differs from original");
    }
    std::puts("native_base_reset cases=256 PASS (all mapped fields/shared state, dirty input; entity metadata excluded)");
    // The apparent clearing loop never advances r15: do NOT translate it as
    // memset of the whole car. Execute original instructions with dirty tails.
    for(unsigned size=52;size<=768;++size) {
        bus.entity.fill(0xa5);bus.write_dword(0x500008,size);
        auto expected=bus.entity;
        std::fill(expected.begin()+0x34,expected.begin()+0x44,0);
        bus.first_instruction=0xe3f4;bus.last_instruction=0xe41c;
        rt::I960Ref cpu(&bus);cpu.m_IP=0xe3f4;cpu.m_r[29]=0x500000;
        unsigned steps=0;
        while(cpu.m_IP!=0xe420) {
            if(++steps>256) throw std::runtime_error("grid clear loop instruction guard");
            cpu.execute_one();
        }
        if(bus.entity!=expected) throw std::runtime_error("grid clear loop changed bytes outside34..43");
    }
    std::puts("native_grid_clear_extent cases=717 PASS (originale3f4..e41c clears ONLY16bytes at34, dirty tail preserved)");
    for(unsigned value=0;value<256;++value) {
        bus.entity.fill(0xa5);
        nm_player_t s{};
        for(unsigned i=0;i<4;++i) s.surface_ticks[i]=std::bit_cast<int8_t>((uint8_t)(value+37*i));
        for(unsigned i=0;i<2;++i) s.force.trigger_count[i]=std::bit_cast<int8_t>((uint8_t)(value+53*i));
        s.response_ticks=std::bit_cast<int8_t>((uint8_t)(value+91));
        const auto fields=[&](auto &bytes) {
            memcpy(bytes.data()+0x91,s.surface_ticks,4);
            memcpy(bytes.data()+0x95,s.force.trigger_count,2);
            bytes[0x98]=(uint8_t)s.response_ticks;
        };
        fields(bus.entity);auto expected=bus.entity;
        nm_player_effect_timers(&s);fields(expected);
        bus.run(0x9014,0x9084);
        if(bus.entity!=expected) throw std::runtime_error("effect timers differ from original9014");
    }
    std::puts("native_effect_timers cases=256 PASS (all byte patterns in all7 timers, full entity canary)");
    for(unsigned i=0;i<34;++i) {
        const uint32_t mask=i<32 ? 1u<<i : i==32 ? 0u : UINT32_MAX;
        bus.first_instruction=0xd328;bus.last_instruction=0xd34c;
        bus.write_dword(0x501090,mask);
        rt::I960Ref cpu(&bus);cpu.m_IP=0xd328;
        unsigned steps=0;
        while(cpu.m_IP!=0xd350 && cpu.m_IP!=0xe078) {
            if(++steps>16) throw std::runtime_error("rolling phase gate instruction guard");
            cpu.execute_one();
        }
        nm_player_t s{};const auto before=s;
        nm_player_active_input_t in{};in.dynamics.speed_flags=mask;
        uint32_t flags=1;uint8_t released=77;
        // Missing packs deliberately reject the active branch only.
        const bool accepted=nm_player_rolling_tick(nullptr,nullptr,nullptr,nullptr,&in,&s,&flags,&released);
        if(accepted!=(cpu.m_IP==0xe078) || flags!=1 ||
           (accepted ? released!=0 || s.proximity_circle[2]!=8 : released!=77 || memcmp(&s,&before,sizeof(s))))
            throw std::runtime_error("rolling phase gate differs from original d328 branch");
    }
    std::puts("native_rolling_phase_gate cases=34 PASS (all flag bits, original d328 branch)");
    for(unsigned offset=0;offset<65536;++offset) {
        nm_player_t s{};s.spin_body_offset=nm_yaw_angle(offset);
        s.boundary.ground.yaw=nm_yaw_angle(offset*997);
        bus.entity.fill(0xa5);
        bus.write_word(0x500058,(uint16_t)s.boundary.ground.yaw);
        bus.write_word(0x5000c0,(uint16_t)s.spin_body_offset);
        auto expected=bus.entity;
        nm_player_spin_pose(&s);
        memcpy(expected.data()+0x2a,&s.body_yaw,2);memcpy(expected.data()+0xc0,&s.spin_body_offset,2);
        bus.first_instruction=0xdbec;bus.last_instruction=0xdc10;
        rt::I960Ref cpu(&bus);cpu.m_IP=0xdbec;cpu.m_r[29]=0x500000;
        while(cpu.m_IP!=0xdc14) cpu.execute_one();
        if(bus.entity!=expected) throw std::runtime_error("spin body offset differs from original dbec..dc10");
    }
    std::puts("native_spin_body_offset cases=65536 PASS (old offset yaw plus signed correction)");
    unsigned braking=0;
    for(unsigned profile=0;profile<65536;++profile) {
        bus.entity.fill(0xa5);
        nm_player_t s{};
        s.phase[0]=nm_yaw_angle(profile); s.accel=(uint16_t)(profile*17);
        s.brake=0xa5a50000u|profile; s.steer=123456;
        s.boundary.damage.override_count=127;
        const auto fields=[&](auto &bytes) {
            memcpy(bytes.data()+0x128,&s.phase[0],2); memcpy(bytes.data()+0xac,&s.accel,2);
            memcpy(bytes.data()+0xa8,&s.brake,4); memcpy(bytes.data()+0xb0,&s.steer,4);
            bytes[0x15f]=s.boundary.damage.override_count;
        };
        fields(bus.entity); auto expected=bus.entity;
        nm_player_rolling_controls(&s); fields(expected);
        braking+=(s.brake&65535)==255;
        bus.run(0xd3f4,0xd434);
        if(bus.entity!=expected) throw std::runtime_error("rolling pedals differ from original i960");
    }
    if(braking!=3) throw std::runtime_error("rolling pedal branch coverage");
    std::printf("native_rolling_controls cases=65536 braking=%u PASS (wholed3f4, full scratch entity)\n",braking);
    unsigned releases=0;
    bus.first_instruction=0xd5a8; bus.last_instruction=0xd680;
    for(unsigned i=0;i<65792;++i) {
        bus.entity.fill(0xa5);bus.checkpoint_memory.fill(0x5a);
        nm_player_t s{};
        s.phase[0]=nm_yaw_angle(i<65536 ? i : 0);
        s.lap.completed=(uint8_t)i; s.lap.lap=(uint8_t)(i+17);
        s.lap.last_ticks=(uint16_t)(i*19);s.lap.best_ticks=(uint16_t)(i*37);
        s.checkpoints.last_lap=(uint8_t)(i+31);s.checkpoints.completed=(uint8_t)(i+61);
        uint32_t flags=i*15485863u;
        const auto fields=[&](auto &entity,auto &extra) {
            memcpy(entity.data()+0x128,&s.phase[0],2);
            entity[0x6d]=s.lap.completed;entity[0x6e]=s.lap.lap;
            memcpy(entity.data()+0x66,&s.lap.last_ticks,2);memcpy(entity.data()+0x64,&s.lap.best_ticks,2);
            extra[0xb8]=s.checkpoints.last_lap;extra[0xb9]=s.checkpoints.completed;
            memcpy(extra.data()+0x454,&flags,4);
        };
        fields(bus.entity,bus.checkpoint_memory);
        auto expected=bus.entity;auto extra=bus.checkpoint_memory;
        const bool released=nm_player_rolling_release(&s,&flags);
        releases+=released;fields(expected,extra);
        if(released) { const uint32_t updater=0xd7fc;memcpy(expected.data()+12,&updater,4); }
        rt::I960Ref cpu(&bus);cpu.m_IP=0xd5a8;cpu.m_r[29]=0x500000;
        unsigned steps=0;
        while(cpu.m_IP!=0xd680) {
            // Explicitly exclude d5bc..d644 text/scroll/register presentation.
            // Gate, updater transition and all gameplay resets execute original instructions.
            if(cpu.m_IP==0xd5bc) cpu.m_IP=0xd648;
            if(++steps>32) throw std::runtime_error("rolling release instruction guard");
            cpu.execute_one();
        }
        if(bus.entity!=expected || bus.checkpoint_memory!=extra)
            throw std::runtime_error("rolling release differs from original gameplay gate/reset");
    }
    if(releases!=257) throw std::runtime_error("rolling release gate coverage");
    std::printf("native_rolling_release cases=65792 events=%u PASS (original gate/updater/reset; UI excluded)\n",releases);
    unsigned grid_cases=0;
    bus.first_instruction=0xe23c; bus.last_instruction=0xe284;
    for(unsigned slot=0;slot<4;++slot)
    for(uint32_t count : {1u,2u,3u,195u,486u,990u,4096u})
    for(uint32_t authored : {0u,1u,99u,100u,194u,985u,65535u,65536u,0xffffffffu}) {
        bus.course_slot=(uint8_t)slot; bus.collision_wrap=count;
        bus.entity.fill(0xa5); bus.write_byte(0x50006e,0);
        bus.write_dword(0x500224,authored); // Scratch grid row at entity+0x200.
        auto expected=bus.entity;
        nm_race_grid_profile_t s{};
        const int16_t offset=std::bit_cast<int16_t>(board.read_word(0x234be8+2*slot));
        if(!nm_race_grid_profile(authored,offset,count,&s))
            throw std::runtime_error("native grid rejected valid reference input");
        memcpy(expected.data()+0x128,&s.profile,2);
        memcpy(expected.data()+0x12a,&s.previous,2);
        memcpy(expected.data()+0x12e,&s.furthest,2);
        memcpy(expected.data()+0x6e,&s.lap,1);
        rt::I960Ref cpu(&bus); cpu.m_IP=0xe23c;
        cpu.m_r[16]=0x500200; cpu.m_r[29]=0x500000;
        unsigned steps=0;
        while(cpu.m_IP!=0xe284) {
            if(++steps>32) throw std::runtime_error("grid instruction guard");
            cpu.execute_one();
        }
        if(bus.entity!=expected) throw std::runtime_error("native grid profile differs from i960");
        ++grid_cases;
    }
    nm_race_grid_profile_t untouched={1,2,3,4}, saved=untouched;
    if(nm_race_grid_profile(0,0,0,&untouched) || nm_race_grid_profile(0,0,4097,&untouched) ||
       nm_race_grid_profile(0,0,1,nullptr) || memcmp(&untouched,&saved,sizeof(saved)))
        throw std::runtime_error("invalid grid input mutated output");
    std::printf("native_race_grid cases=%u PASS (originale23c..e284; profile selection only)\n",grid_cases);
    bus.first_instruction=0x3560; bus.last_instruction=0x37c0;
    unsigned cases=0;
    for (uint8_t mode : {0,1,255}) for (uint16_t gate : {0,1,65535})
    for (uint32_t hold : {0u,1u,0xffffffffu})
    for (uint32_t remaining : {0u,1u,2u,0x7fffffffu,0x80000000u,0xffffffffu})
    for (uint32_t clock : {0u,1u,0xffffffffu})
    for (uint32_t flags : {0u,1u,2u,3u,0xffffffffu})
    for (uint32_t speed : {0u,1u,0x7fffffffu,0x80000000u,0xffffffffu}) {
        bus.entity.fill(0xa5); bus.globals.fill(0xa5);
        bus.write_byte(0x501074,mode); bus.write_word(0x500204,gate);
        memcpy(bus.checkpoint_memory.data()+0x684,&hold,4);
        bus.write_dword(0x500018,speed);
        bus.write_dword(0x5010d0,remaining); bus.write_dword(0x5010cc,clock);
        bus.write_dword(0x5010d8,flags);
        auto expected=bus.globals; const auto entity=bus.entity;
        const uint8_t phase=nm_race_active_timer(mode,gate,hold,&remaining,&clock,&flags);
        memcpy(expected.data()+0xd0,&remaining,4); memcpy(expected.data()+0xcc,&clock,4);
        memcpy(expected.data()+0xd8,&flags,4);
        rt::I960Ref cpu(&bus); cpu.m_IP=0x3560;
        unsigned steps=0;
        while(cpu.m_IP!=0x35f4 && cpu.m_IP!=0x371c && cpu.m_IP!=0x37c0) {
            if(++steps>64) throw std::runtime_error("race timer instruction guard");
            cpu.execute_one();
        }
        const unsigned actual=cpu.m_IP==0x35f4 ? 22 : cpu.m_IP==0x371c ? 23 : 25;
        if(actual!=phase || bus.globals!=expected || bus.entity!=entity)
            throw std::runtime_error("native race timer differs from original i960");
        ++cases;
    }
    std::printf("native_race_timer cases=%u PASS (original03560 decision, full scratch state; not whole phase22)\n",cases);
    unsigned finish_cases=0;
    for(unsigned rank=0;rank<256;++rank)
    for(uint32_t mode : {0u,1u,0xffffffffu})
    for(uint32_t timer : {0u,1u,2u,384u,0x7fffffffu,0x80000000u,0xffffffffu}) {
        bus.entity.fill(0xa5);bus.globals.fill(0x5a);
        bus.proximity_disabled=mode;
        bus.write_byte(0x50006a,(uint8_t)rank);
        bus.write_byte(0x5010a4,24);bus.write_dword(0x5010a8,timer);
        auto expected=bus.globals;const auto entity=bus.entity;
        const uint8_t phase=nm_race_finish_timer(&timer,std::bit_cast<int8_t>((uint8_t)rank),mode);
        memcpy(expected.data()+0xa8,&timer,4);expected[0xa4]=phase;
        bus.run(0x4454,0x44a4);
        if(bus.globals!=expected || bus.entity!=entity)
            throw std::runtime_error("finish timer differs from original04454");
        ++finish_cases;
    }
    std::printf("native_finish_timer cases=%u PASS (all signed ranks, zero boundary and wrap, whole scratch globals)\n",finish_cases);
    unsigned timeout_cases=0;
    for(uint32_t mode : {0u,1u,0xffffffffu}) {
        bus.globals.fill(0xa5);bus.write_byte(0x5010a4,25);bus.proximity_disabled=mode;
        auto expected=bus.globals;
        const uint32_t initial=nm_race_timeout_ticks(mode);
        memcpy(expected.data()+0xa8,&initial,4);expected[0xa4]=26;expected[0xf8]=0;
        bus.run(0x42a4,0x42e4);
        if(bus.globals!=expected || bus.proximity_disabled!=mode)
            throw std::runtime_error("timeout entry delay differs from original");
        for(uint32_t timer : {0u,1u,127u,128u,129u,608u,800u,0x7fffffffu,0x80000000u,0xffffffffu}) {
            bus.globals.fill(0xa5);bus.write_byte(0x5010a4,26);bus.write_dword(0x5010a8,timer);
            bus.proximity_disabled=mode;
            expected=bus.globals;uint32_t next_mode=mode;
            expected[0xa4]=nm_race_timeout_timer(&timer,&next_mode);
            memcpy(expected.data()+0xa8,&timer,4);
            const auto entity=bus.entity;
            bus.run(0x42e8,0x4324);
            if(bus.globals!=expected || bus.entity!=entity || bus.proximity_disabled!=next_mode)
                throw std::runtime_error("timeout timer differs from original");
            ++timeout_cases;
        }
    }
    std::printf("native_timeout_timer cases=%u entry_modes=3 PASS (original phase25 tail/26, full scratch globals)\n",timeout_cases);
}
}
