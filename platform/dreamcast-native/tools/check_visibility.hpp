// Host-only original i960/TGP oracle, after scenario completion.
#pragma once
#include "check_road.hpp"
#include "../include/visibility.h"
#include <random>

namespace daytona::native {
inline void check_visibility(rt::M2Board &board, const char *path) {
    std::unique_ptr<nm_drive_params_t,decltype(&std::free)> p(nm_drive_load(path),&std::free);
    if (!p || !board.tgp().booted() || !board.tgp().fifo_out_empty())
        throw std::runtime_error("visibility oracle requires validated math and idle booted TGP");
    nm_scenery_probe_t probes[4][12];
    for (unsigned q=0;q<4;++q) {
        const uint32_t base=board.read_dword(0x17658+4*q);
        if (base<0x28b5880 || base>0x28b5a60) throw std::runtime_error("invalid visibility table");
        for (unsigned i=0;i<12;++i) {
            const uint32_t at=base+(i/6)*80+(i%6)*12;
            auto &v=probes[q][i];
            v.x=std::bit_cast<float>(board.read_dword(at));
            v.z=std::bit_cast<float>(board.read_dword(at+4));
            v.mask=board.read_dword(at+8);
            if (!std::isfinite(v.x) || !std::isfinite(v.z) || std::abs(v.x)>384 ||
                std::abs(v.z)>384 || v.mask>>25) throw std::runtime_error("invalid visibility probe");
        }
    }
    VehicleOracleBus bus(board);
    std::mt19937 random(0x56495349);
    uint32_t wedge_hash=2166136261u, mask_hash=2166136261u;
    const auto coord=[&]() { return float(int(random()%32769)-16384)/8; };
    for (unsigned i=0;i<65536;++i) {
        const float x=coord(),z=coord();
        const float px=i%4 ? coord() : x, pz=i%8 ? coord() : z;
        bus.scenery_heading=std::bit_cast<int16_t>(uint16_t(i));
        const std::array<uint32_t,10> args{0,0,std::bit_cast<uint32_t>(x),std::bit_cast<uint32_t>(z),
            std::bit_cast<uint32_t>(px),std::bit_cast<uint32_t>(pz)};
        const auto cpu=bus.run(0x17668,0x17754,&args);
        const unsigned actual=nm_scenery_wedge(p.get(),x,z,px,pz,bus.scenery_heading);
        const unsigned expected=!(cpu.m_AC&4);
        if (actual!=expected) throw std::runtime_error("visibility wedge differs from i960/TGP sign bits");
        wedge_hash=(wedge_hash^actual)*16777619u;
        uint32_t mask;
        if (!nm_scenery_view_mask(p.get(),probes,x,z,bus.scenery_heading,&mask))
            throw std::runtime_error("native view mask rejected valid coordinate");
        const auto full=bus.run(0x1727c,0x17654,&args);
        if (mask!=full.m_r[17]) {
            std::fprintf(stderr,"visibility mask i=%u x=%g z=%g expected=%08x actual=%08x\n",
                i,double(x),double(z),full.m_r[17],mask);
            throw std::runtime_error("visibility mask differs from i960/TGP");
        }
        if (!board.tgp().fifo_out_empty()) throw std::runtime_error("visibility oracle unread TGP output");
        mask_hash=(mask_hash^mask)*16777619u;
    }
    uint32_t sentinel=0xdeadbeef;
    for (float bad : {NAN,INFINITY,-INFINITY,2147483648.f,-2147483904.f})
        if (nm_scenery_view_mask(p.get(),probes,bad,0,0,&sentinel) || sentinel!=0xdeadbeef)
            throw std::runtime_error("invalid visibility coordinate published result");
    uint8_t orders[2][25];
    for (unsigned o=0;o<2;++o) for (unsigned i=0;i<25;++i) {
        orders[o][i]=board.read_byte(0x17104+25*o+i);
        const int offset=int(i%5)-2+16*(int(i/5)-2);
        if (std::bit_cast<int8_t>(board.read_byte(0x17136+i))!=offset)
            throw std::runtime_error("source cell offsets differ from native5x5 grid");
    }
    uint32_t cell_hash=2166136261u,bitmap_hash=2166136261u;
    for (unsigned i=0;i<65536;++i) {
        bus.first_instruction=0x16ed0; bus.last_instruction=0x17074; // 17070 has an extension word.
        bus.course_slot=(i>>8)&3;
        bus.checkpoint_memory.fill(0xa5);
        // Source16e6c..16e88 clears draw/near words before this tested block.
        std::fill(bus.checkpoint_memory.begin()+0x120,bus.checkpoint_memory.begin()+0x160,0);
        rt::I960Ref cpu(&bus);
        const unsigned cell=i&255;
        cpu.m_r[16]=cell; cpu.m_IP=0x16ed0;
        const auto until=[&](uint32_t end) {
            unsigned instructions=0;
            while (cpu.m_IP!=end) {
                if (++instructions>2048) throw std::runtime_error("cell selection instruction guard");
                cpu.execute_one();
            }
        };
        until(0x16f5c); // Original boundary-mask instructions, no native shortcut.
        const uint32_t excluded=random()&0x1ffffff, visible=random()&0x1ffffff;
        cpu.m_r[13]|=excluded;
        cpu.m_r[16]=cell; cpu.m_r[17]=visible; cpu.m_IP=0x16f74;
        until(0x17078);
        nm_scenery_cells_t actual;
        const unsigned order=bus.course_slot==2 && (cell==72 || cell==88);
        if (!nm_scenery_cells(orders[order],cell,excluded,visible,&actual) ||
            actual.draw_count!=bus.checkpoint_memory[0x2c0] ||
            actual.near_count!=bus.checkpoint_memory[0x200] ||
            memcmp(actual.draw,bus.checkpoint_memory.data()+0x2c1,actual.draw_count) ||
            memcmp(actual.near,bus.checkpoint_memory.data()+0x201,actual.near_count))
            throw std::runtime_error("ordered native cell lists differ from original boundary/selection instructions");
        uint32_t masks[16];
        if(!nm_scenery_bitmaps(&actual,masks)) throw std::runtime_error("native scenery bitmap rejected valid lists");
        for(unsigned word=0;word<16;++word) {
            if(masks[word]!=bus.read_dword(0x501520+4*word))
                throw std::runtime_error("native draw/near bitmap differs from original selection stores");
            bitmap_hash=(bitmap_hash^masks[word])*16777619u;
        }
        for (uint8_t byte : std::bit_cast<std::array<uint8_t,sizeof(actual)>>(actual))
            cell_hash=(cell_hash^byte)*16777619u;
    }
    std::printf("native_visibility PASS cases=65536 all_headings wedge_hash=%08x mask_hash=%08x exact_i960_tgp; no runtime board\n",
        wedge_hash,mask_hash);
    if (wedge_hash!=0xe152566b || mask_hash!=0x5731d05a)
        throw std::runtime_error("visibility regression digest changed");
    std::printf("native_scenery_cells PASS cases=65536 all_cells/courses boundary_masks/alternate_order draw+near hash=%08x\n",cell_hash);
    if (cell_hash!=0x0c2394b7) throw std::runtime_error("scenery cell regression digest changed");
    std::printf("native_scenery_bitmaps PASS cases=65536 draw+near16words hash=%08x exact_original_stores\n",bitmap_hash);
    if(bitmap_hash!=0x2ba0d88c) throw std::runtime_error("scenery bitmap regression digest changed");
    uint32_t profile_hash=2166136261u; unsigned profile_cases=0;
    for (unsigned slot=0;slot<4;++slot) {
        bus.course_slot=slot;
        const uint32_t descriptor=board.read_dword(0x3a48+4*slot);
        const uint32_t profiles=board.read_dword(board.read_dword(descriptor+20));
        if (!profiles || profiles>4096) throw std::runtime_error("invalid course profile count");
        for (unsigned profile=0;profile<=profiles;++profile) {
            uint32_t at=board.read_dword(0x17150+4*slot);
            while (board.read_word(at+2)<profile) {
                at+=36;
                if (at>0x171f4) throw std::runtime_error("profile mask table overflow");
            }
            uint16_t rows[16];
            const bool active=profile>=board.read_word(at);
            if (active) for (unsigned r=0;r<16;++r) rows[r]=board.read_word(at+4+2*r);
            bus.write_word(0x500128,uint16_t(profile));
            for (unsigned cell=0;cell<256;++cell) {
                const uint32_t previous=random()&0x1ffffff;
                const std::array<uint32_t,10> args{cell,previous,cell&15,cell>>4};
                const auto cpu=bus.run(0x171f8,0x17278,&args);
                const uint32_t actual=previous|nm_scenery_profile_mask(active ? rows : nullptr,cell);
                if (actual!=cpu.m_r[17]) throw std::runtime_error("native profile exclusion differs from original instructions");
                profile_hash=(profile_hash^actual)*16777619u; ++profile_cases;
            }
        }
    }
    std::printf("native_scenery_profiles PASS cases=%u all_authored_profiles/cells/courses hash=%08x\n",profile_cases,profile_hash);
    if (profile_cases!=492544 || profile_hash!=0x10612ec0)
        throw std::runtime_error("scenery profile regression digest changed");
}
}
