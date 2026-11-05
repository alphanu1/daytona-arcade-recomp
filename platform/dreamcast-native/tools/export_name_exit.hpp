// Host-only extraction of original name-exit matrices, never target runtime.
#pragma once
#include "runtime/m2_board.h"
#include "refcore/i960_ref.h"
#include <bit>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace daytona::native {
inline void export_name_exit(rt::M2Board &board,const std::string &path) {
    if(std::filesystem::exists(path)) throw std::runtime_error("name exit output already exists");
    constexpr uint32_t actor=0x510000;
    const uint16_t saved=board.read_word(actor+0x38);
    std::ofstream out(path);out<<std::setprecision(9);
    if(!out) throw std::runtime_error("cannot create name exit output");
    try {
        for(unsigned phase:{5u,7u}) for(unsigned glyph=0;glyph<29;++glyph)
            for(unsigned timer:{0u,1u,10u,20u,32u,64u,191u}) {
                if(phase==7 && timer>20) continue;
                board.write_word(actor+0x38,(uint16_t)timer);
                board.tgp().function_port_w(0x10*4,0x1010);
                rt::I960Ref cpu(&board);
                cpu.m_IP=phase==5 ? 0x226e1c : 0x2270c8;
                cpu.m_r[11]=0x23b8f2+32*glyph;
                cpu.m_r[27]=0x880000;cpu.m_r[28]=0x4000;cpu.m_r[29]=actor;
                const uint32_t end=phase==5 ? 0x227034 : 0x2271cc;
                unsigned steps=0;
                while(cpu.m_IP!=end) {
                    if(++steps>200) throw std::runtime_error("name exit instruction bound");
                    cpu.execute_one();
                }
                board.tgp().function_port_w(0x11*4,0x1111);
                out<<phase<<' '<<glyph<<' '<<timer;
                for(unsigned i=0;i<12;++i)
                    out<<' '<<std::bit_cast<float>(board.tgp().fifo_r());
                out<<'\n';
                if(!board.tgp().fifo_out_empty()) throw std::runtime_error("unread name exit output");
            }
    } catch(...) { board.write_word(actor+0x38,saved);throw; }
    board.write_word(actor+0x38,saved);
    out.close();if(!out) throw std::runtime_error("name exit output write failed");
}
}
